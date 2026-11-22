#pragma once

#include <QtQml/qqmlregistration.h>
#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <functional>

#include "notices.h"
#include "reports.h"

// The reports that came with the scans, for the list under the series.
class ReportModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The report list comes from the backend")
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        Title = Qt::UserRole + 1,
        Date,
        Pdf,
        Study,
        Current,    // Of the study the pane is showing.
    };

    explicit ReportModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_reports.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return m_reports.size(); }

    void reset(const QList<Report> &reports);
    // Which study's reports are the shown one's, when a series is shown.
    void setStudy(const QString &studyUid, bool shown);
    void retranslate();

signals:
    void countChanged();

private:
    QList<Report> m_reports;
    QString m_study;
    bool m_shown = false;
};

// The report a pane has open to read beside its scans. A report's file is
// read on a worker, and what comes back is shown only if that report is
// still the one open.
class ReportReader : public QObject {
    Q_OBJECT

public:
    explicit ReportReader(QObject *parent = nullptr) : QObject(parent) {}

    ReportModel *model() { return &m_model; }
    // A new scan's reports; the one open is closed.
    void setReports(const QList<Report> &reports);
    void setStudy(const QString &studyUid, bool shown);

    int index() const { return m_index; }
    int count() const { return m_reports.size(); }
    QString title() const;
    QString study() const;
    QString date() const;
    bool isPdf() const;
    // A structured report's lines, once read: depth, heading, name, value.
    QVariantList lines() const;

    // Shows a report in the panel; launch also opens a PDF in the viewer,
    // from a copy only the user can read.
    void open(int index, bool launch);
    // The first report of a study, or failing that the first there is.
    void openFor(const QString &studyUid);
    void step(int delta);
    void close();
    // Where a PDF report is opened; tests catch it rather than launch a viewer.
    void setUrlOpener(std::function<bool(const QUrl &)> opener) { m_openUrl = std::move(opener); }
    void retranslate();

signals:
    void changed();
    void problem(const Words &words);

private:
    ReportModel m_model;
    QList<Report> m_reports;
    int m_index = -1;
    QList<ReportLine> m_lines;
    // Read, and found to say nothing.
    bool m_empty = false;
    int m_generation = 0;
    std::function<bool(const QUrl &)> m_openUrl;
};
