#include "anonymouscopy.h"

#include <QCoreApplication>
#include <QEventLoopLocker>
#include <QFileInfo>
#include <QLocale>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QUrl>
#include <algorithm>

#include "seriesmodel.h"

AnonymousCopy::AnonymousCopy(FilePicker *picker, QObject *parent) : QObject(parent), m_picker(picker) {
    // Q quits under a plan still being worked out: it stops rather than
    // keep the app waiting for something nobody will read.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (m_cancel)
            *m_cancel = true;
    });
    if (!m_picker)
        return;
    // The panes share the chooser; this takes only the answers to what it
    // asked.
    connect(m_picker, &FilePicker::selected, this,
            [this](QObject *asker, FilePicker::Purpose purpose, const QList<QUrl> &urls) {
        if (asker == this && purpose == FilePicker::Purpose::SaveFolder && !urls.isEmpty())
            save(urls.first().toLocalFile());
    });
    connect(m_picker, &FilePicker::failed, this, [this](QObject *asker, const QString &message) {
        if (asker == this)
            emit failed([message] { return message; });
    });
}

AnonymousCopy::~AnonymousCopy() {
    if (m_cancel)
        *m_cancel = true;
}

void AnonymousCopy::setLibrary(Library *library) {
    if (library == m_library)
        return;
    if (m_library)
        disconnect(m_library, nullptr, this, nullptr);
    m_library = library;
    if (!m_library)
        return;
    // A plan is of the scan that was open when it was asked for: another
    // one opened over it leaves nothing to choose a folder for.
    connect(m_library, &Library::opened, this, [this] {
        if (m_stage == Planning || m_stage == Planned)
            close();
    });
}

void AnonymousCopy::begin() {
    if (m_stage == Saving || m_stage == Saved) {
        // Still being written, or not yet dismissed: that is what's shown.
        if (!m_shown) {
            m_shown = true;
            emit changed();
        }
        return;
    }
    if (!m_library || m_library->scan().series.isEmpty())
        return;
    m_scan = m_library->scan();
    // The choice starts as the profile has it each time, as leaving it on
    // from a copy made before would keep text nobody looked at again.
    m_options = {};
    m_options.blackout = m_library->blackouts()->all();
    m_shown = true;
    plan();
}

void AnonymousCopy::setKeepDescriptions(bool keep) {
    if (keep == m_options.keepDescriptions || (m_stage != Planning && m_stage != Planned))
        return;
    m_options.keepDescriptions = keep;
    plan();
}

void AnonymousCopy::plan() {
    if (m_cancel)
        *m_cancel = true;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_cancel = cancel;
    const int generation = ++m_generation;
    m_stage = Planning;
    m_result = {};
    emit changed();
    QPointer<AnonymousCopy> self(this);
    // Every file is read, twice: for what it says of the patient, and for
    // what a copy of it would hold. That is seconds for a CD.
    QThreadPool::globalInstance()->start([self, scan = m_scan, options = m_options, cancel, generation] {
        const Anonymize::Result result = Anonymize::plan(scan, options, cancel.get());
        if (*cancel)
            return;
        QMetaObject::invokeMethod(qApp, [self, result, generation] {
            if (!self || generation != self->m_generation || self->m_stage != Planning)
                return;
            self->m_result = result;
            self->m_stage = Planned;
            emit self->changed();
        }, Qt::QueuedConnection);
    });
}

void AnonymousCopy::choose() {
    if (canSave())
        m_picker->choose(this, FilePicker::Purpose::SaveFolder);
}

void AnonymousCopy::save(const QString &folder) {
    // A folder chosen after the plan was put away, or before there was
    // one, is for nothing: only what was looked at is written.
    if (folder.isEmpty() || m_stage != Planned || m_result.written == 0)
        return;
    m_stage = Saving;
    emit changed();
    QPointer<AnonymousCopy> self(this);
    // Q may quit under a copy still being written: it finishes first.
    auto locker = std::make_shared<QEventLoopLocker>();
    QThreadPool::globalInstance()->start([self, scan = m_scan, options = m_options, folder, locker] {
        const Anonymize::Result result = Anonymize::copyScan(scan, folder, nullptr, options);
        QMetaObject::invokeMethod(qApp, [self, result] {
            if (self)
                self->finished(result);
        }, Qt::QueuedConnection);
    });
}

void AnonymousCopy::finished(const Anonymize::Result &result) {
    m_result = result;
    m_stage = Saved;
    if (result.written > 0)
        emit said([written = result.written, name = folderName()] {
            return tr("Saved %Ln files in %1", nullptr, written).arg(name);
        });
    // Put away while it was written, it comes back only with something to
    // say that the notice doesn't: what needs a look stays up until it's
    // dismissed.
    const bool attention = result.written == 0 || !result.uncopied.isEmpty() || result.skipped > 0
                           || result.annotations > 0 || unhidden() > 0;
    if (!m_shown && attention)
        m_shown = true;
    if (!m_shown) {
        m_stage = Closed;
        m_result = {};
        m_scan = {};
    }
    emit changed();
}

void AnonymousCopy::close() {
    if (m_stage == Closed && !m_shown)
        return;
    m_shown = false;
    if (m_stage != Saving) {
        if (m_cancel)
            *m_cancel = true;
        ++m_generation;
        m_stage = Closed;
        m_result = {};
        m_scan = {};
        m_options = {};
    }
    emit changed();
}

QVariantList AnonymousCopy::counts(const QList<Anonymize::Count> &counts) const {
    QVariantList rows;
    for (const Anonymize::Count &c : counts)
        // An attribute keeps the name the standard gives it; only
        // Dicolens' own words for a set of them are translated.
        rows << QVariantMap{{"name", c.standard ? c.name : tr(c.name.toUtf8().constData())}, {"tag", c.tag}, {"files", c.files}};
    return rows;
}

QString AnonymousCopy::seriesName(int series) const {
    return series >= 0 && series < m_scan.series.size() ? SeriesModel::titleOf(m_scan.series[series]) : QString();
}

QString AnonymousCopy::seriesNames(const QList<int> &series) const {
    QStringList names;
    for (const int s : series)
        names << seriesName(s);
    return QLocale().createSeparatedList(names);
}

QVariantList AnonymousCopy::pictures() const {
    QList<int> series = m_result.showDetailsIn.keys() + m_options.blackout.keys();
    std::sort(series.begin(), series.end());
    series.erase(std::unique(series.begin(), series.end()), series.end());
    QVariantList rows;
    for (const int s : std::as_const(series)) {
        if (s < 0 || s >= m_scan.series.size())
            continue;
        rows << QVariantMap{{"name", seriesName(s)},
                            {"modality", m_scan.series[s].modality},
                            {"images", int(m_scan.series[s].slices.size())},
                            {"flagged", m_result.showDetailsIn.value(s)},
                            {"regions", int(m_options.blackout.value(s).size())}};
    }
    return rows;
}

int AnonymousCopy::regions() const {
    int count = 0;
    for (const QList<QRect> &regions : m_options.blackout)
        count += regions.size();
    return count;
}

int AnonymousCopy::unhidden() const {
    int count = 0;
    for (auto it = m_result.showDetailsIn.constBegin(); it != m_result.showDetailsIn.constEnd(); ++it)
        if (m_options.blackout.value(it.key()).isEmpty())
            count += it.value();
    return count;
}

int AnonymousCopy::refusedFiles() const {
    return int(std::count_if(m_result.uncopied.constBegin(), m_result.uncopied.constEnd(),
                             [](const Anonymize::Uncopied &u) { return u.refused; }));
}

QVariantList AnonymousCopy::refused() const {
    QHash<QString, int> seriesOf;
    for (const Anonymize::Uncopied &u : m_result.uncopied)
        seriesOf.insert(u.path, u.series);
    // One row for each thing found in an attribute, however many files it
    // was found in: what was found is the patient's, and is said once.
    struct Found {
        const Anonymize::Failure *first = nullptr;
        QSet<QString> files;
        QList<int> series;
    };
    QList<Found> found;
    for (const Anonymize::Failure &f : m_result.failures) {
        auto same = std::find_if(found.begin(), found.end(), [&f](const Found &other) {
            return other.first->tag == f.tag && other.first->what == f.what && other.first->found == f.found;
        });
        if (same == found.end()) {
            found << Found{&f, {}, {}};
            same = found.end() - 1;
        }
        same->files.insert(f.path);
        if (const int s = seriesOf.value(f.path, -1); s >= 0 && !same->series.contains(s))
            same->series << s;
    }
    QVariantList rows;
    for (const Found &group : std::as_const(found)) {
        const Anonymize::Failure &f = *group.first;
        // The anonymizer says both in English: an attribute keeps the
        // standard's name, and the rest is put in the user's language here.
        const bool named = !Dictionary::name(f.tag).isEmpty();
        rows << QVariantMap{{"name", named ? f.name : tr(f.name.toUtf8().constData())},
                            {"tag", tagText(f.tag)},
                            {"what", tr(f.what.toUtf8().constData())},
                            {"found", f.found},
                            {"files", int(group.files.size())},
                            {"series", seriesNames(group.series)}};
    }
    return rows;
}

QVariantList AnonymousCopy::uncopied() const {
    struct Why {
        QString reason;
        int files = 0;
        QList<int> series;
    };
    QList<Why> reasons;
    for (const Anonymize::Uncopied &u : m_result.uncopied) {
        if (u.refused)
            continue;
        auto same = std::find_if(reasons.begin(), reasons.end(), [&u](const Why &other) { return other.reason == u.reason; });
        if (same == reasons.end()) {
            reasons << Why{u.reason, 0, {}};
            same = reasons.end() - 1;
        }
        ++same->files;
        if (!same->series.contains(u.series))
            same->series << u.series;
    }
    QVariantList rows;
    for (const Why &why : std::as_const(reasons))
        // Translated where the anonymizer marked its words for it; a
        // decoder's own detail stays as it came.
        rows << QVariantMap{{"reason", tr(why.reason.toUtf8().constData())},
                            {"files", why.files},
                            {"series", seriesNames(why.series)}};
    return rows;
}

QString AnonymousCopy::folderName() const {
    return QFileInfo(m_result.folder).fileName();
}

QString AnonymousCopy::failure() const {
    if (m_stage != Saved || !m_result.folder.isEmpty())
        return {};
    return tr("The anonymous copy couldn't be saved: %1").arg(m_result.errors.value(0));
}
