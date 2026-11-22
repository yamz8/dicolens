#include "reportreader.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThreadPool>
#include <algorithm>

#include "readout.h"

QHash<int, QByteArray> ReportModel::roleNames() const {
    return {{Title, "title"}, {Date, "date"}, {Pdf, "pdf"}, {Study, "study"}, {Current, "current"}};
}

QVariant ReportModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_reports.size())
        return {};
    const Report &r = m_reports[index.row()];
    switch (role) {
    case Title: return r.title.isEmpty() ? tr("Report") : r.title;
    case Date: return Readout::date(r.date);
    case Pdf: return r.kind == Report::Kind::Pdf;
    case Study: return r.studyDescription;
    case Current: return m_shown && r.studyUid == m_study;
    }
    return {};
}

void ReportModel::reset(const QList<Report> &reports) {
    beginResetModel();
    m_reports = reports;
    endResetModel();
    emit countChanged();
}

void ReportModel::setStudy(const QString &studyUid, bool shown) {
    if (studyUid == m_study && shown == m_shown)
        return;
    m_study = studyUid;
    m_shown = shown;
    if (!m_reports.isEmpty())
        emit dataChanged(index(0), index(m_reports.size() - 1), {Current});
}

void ReportModel::retranslate() {
    if (!m_reports.isEmpty())
        emit dataChanged(index(0), index(m_reports.size() - 1), {Title, Date});
}

void ReportReader::setReports(const QList<Report> &reports) {
    close();
    ++m_generation;
    m_reports = reports;
    m_model.reset(reports);
}

void ReportReader::setStudy(const QString &studyUid, bool shown) {
    m_model.setStudy(studyUid, shown);
}

QString ReportReader::title() const {
    if (m_index < 0 || m_index >= m_reports.size())
        return {};
    const QString title = m_reports[m_index].title;
    return title.isEmpty() ? tr("Report") : title;
}

QString ReportReader::study() const {
    return m_reports.value(m_index).studyDescription;
}

QString ReportReader::date() const {
    return m_index >= 0 && m_index < m_reports.size() ? Readout::date(m_reports[m_index].date) : QString();
}

bool ReportReader::isPdf() const {
    return m_index >= 0 && m_index < m_reports.size() && m_reports[m_index].kind == Report::Kind::Pdf;
}

QVariantList ReportReader::lines() const {
    QVariantList out;
    for (const auto &line : m_lines)
        out << QVariantMap{{"depth", line.depth}, {"heading", line.heading}, {"name", line.name}, {"value", line.value}};
    if (m_empty)
        out << QVariantMap{{"depth", 0}, {"heading", false}, {"name", QString()}, {"value", tr("This report is empty")}};
    return out;
}

void ReportReader::open(int index, bool launch) {
    if (index < 0 || index >= m_reports.size())
        return;
    const Report report = m_reports[index];
    // Whatever is still being read is of the report open before.
    const int generation = ++m_generation;
    if (index != m_index) {
        m_index = index;
        m_lines.clear();
        m_empty = false;
    }
    emit changed();
    const bool pdf = report.kind == Report::Kind::Pdf;
    // A PDF waits in the panel until it's asked for, so stepping past one
    // doesn't launch a viewer; a structured report read already is there.
    if (pdf ? !launch : (m_empty || !m_lines.isEmpty()))
        return;

    QPointer<ReportReader> self(this);
    QThreadPool::globalInstance()->start([self, report, index, generation] {
        const DicomFile file = DicomFile::read(report.path);
        if (report.kind == Report::Kind::Structured) {
            const QList<ReportLine> lines = Reports::lines(file);
            QMetaObject::invokeMethod(qApp, [self, lines, generation] {
                if (!self || generation != self->m_generation)
                    return;
                self->m_lines = lines;
                self->m_empty = lines.isEmpty();
                emit self->changed();
            }, Qt::QueuedConnection);
            return;
        }

        // It opens in the PDF viewer, from a copy in the user's runtime
        // folder, which only they can read and which is gone at logout.
        Words failure;
        QUrl url;
        const QByteArray bytes = Reports::pdf(file);
        if (!bytes.startsWith("%PDF")) {
            failure = [] { return tr("This report can't be read"); };
        } else {
            QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
            if (base.isEmpty())
                base = QDir::tempPath();
            const QString dir = base + "/dicolens";
            QDir().mkpath(dir);
            QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            QString name = report.title.isEmpty() ? QStringLiteral("report") : report.title;
            name.replace(QRegularExpression("[^\\w\\-]+"), "-");
            const QString path = QStringLiteral("%1/%2-%3.pdf").arg(dir, name).arg(index + 1);
            QSaveFile out(path);
            out.setDirectWriteFallback(false);
            if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
                failure = [error = out.errorString()] { return tr("The report couldn't be opened: %1").arg(error); };
            else
                url = QUrl::fromLocalFile(path);
        }
        QMetaObject::invokeMethod(qApp, [self, failure, url, generation] {
            // Stepped on, or closed, while the copy was made: not opened.
            if (!self || generation != self->m_generation)
                return;
            if (failure)
                emit self->problem(failure);
            else if (!(self->m_openUrl ? self->m_openUrl(url) : QDesktopServices::openUrl(url)))
                emit self->problem([] { return tr("No app is set to open PDF files"); });
        }, Qt::QueuedConnection);
    });
}

void ReportReader::openFor(const QString &studyUid) {
    int index = 0;
    for (int i = 0; i < m_reports.size(); ++i)
        if (m_reports[i].studyUid == studyUid) {
            index = i;
            break;
        }
    open(index, false);
}

void ReportReader::step(int delta) {
    if (m_index < 0 || m_reports.isEmpty())
        return;
    const int index = std::clamp(m_index + delta, 0, int(m_reports.size()) - 1);
    if (index != m_index)
        open(index, false);
}

void ReportReader::close() {
    if (m_index < 0)
        return;
    ++m_generation;
    m_index = -1;
    m_lines.clear();
    m_empty = false;
    emit changed();
}

void ReportReader::retranslate() {
    m_model.retranslate();
    emit changed();
}
