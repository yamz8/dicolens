#include "backend.h"

#include "anonymize.h"
#include "archive.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDate>
#include <QDeadlineTimer>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoopLocker>
#include <QPointer>
#include <QProcess>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTime>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

// What one series' decoding shares between the pool's threads: which
// slices are taken, which one the viewer wants next, and the multi-frame
// files already parsed, so a cine loop is read from disk once.
struct DecodeJob {
    int generation = 0;
    // Read by every worker at once, so only ever through const access:
    // a non-const one would detach the shared list from several threads.
    QList<Slice> slices;
    bool video = false;
    QMutex mutex;
    QVector<char> claimed;
    std::atomic<int> wanted{0};
    std::atomic<bool> cancelled{false};
    QHash<QString, std::shared_ptr<DicomFile>> files;
    QHash<QString, std::shared_ptr<QList<QByteArray>>> frames;

    // The untaken slice nearest the one being looked at.
    int claim() {
        QMutexLocker lock(&mutex);
        const int n = claimed.size(), at = std::clamp(wanted.load(), 0, std::max(0, n - 1));
        for (int d = 0; d < n; ++d)
            for (int i : {at + d, at - d})
                if (i >= 0 && i < n && !claimed[i]) {
                    claimed[i] = 1;
                    return i;
                }
        return -1;
    }

    std::shared_ptr<DicomFile> open(const QString &path, std::shared_ptr<QList<QByteArray>> &split, QString *error) {
        {
            QMutexLocker lock(&mutex);
            if (files.contains(path)) {
                split = frames.value(path);
                return files.value(path);
            }
        }
        auto file = std::make_shared<DicomFile>(DicomFile::read(path, error));
        if (file->isValid() && file->integer(Tag::NumberOfFrames, 1) > 1) {
            if (file->encapsulated())
                split = std::make_shared<QList<QByteArray>>(readFrames(*file));
            QMutexLocker lock(&mutex);
            files.insert(path, file);
            frames.insert(path, split);
        }
        return file;
    }
};

namespace {

// Qt's own tr() signature, so a counted string reads the same here as in a class.
QString tr(const char *text, const char *disambiguation = nullptr, int n = -1) {
    return QCoreApplication::translate("dicolens", text, disambiguation, n);
}

struct Preset {
    const char *name;
    double center;
    double width;
};

// The CT windows radiologists reach for, in Hounsfield units.
const Preset Presets[] = {
    {QT_TRANSLATE_NOOP("dicolens", "Brain"), 40, 80},
    {QT_TRANSLATE_NOOP("dicolens", "Soft tissue"), 40, 400},
    {QT_TRANSLATE_NOOP("dicolens", "Lung"), -600, 1500},
    {QT_TRANSLATE_NOOP("dicolens", "Bone"), 400, 1800},
};

QString number(double value, int decimals) { return QLocale().toString(value, 'f', decimals); }

QString localeDate(const QString &dicomDate) {
    const QDate date = QDate::fromString(DicomFile::formatDate(dicomDate), Qt::ISODate);
    return date.isValid() ? QLocale().toString(date, QLocale::ShortFormat) : dicomDate;
}

QString age(const QString &value) {
    // "054Y", "006M", "003W", "012D".
    if (value.size() < 2)
        return value;
    bool ok = false;
    const int n = value.left(value.size() - 1).toInt(&ok);
    if (!ok)
        return value;
    switch (value.back().toUpper().unicode()) {
    case 'Y': return tr("%Ln years old", nullptr, n);
    case 'M': return tr("%Ln months old", nullptr, n);
    case 'W': return tr("%Ln weeks old", nullptr, n);
    case 'D': return tr("%Ln days old", nullptr, n);
    }
    return value;
}

QString sex(const QString &value) {
    if (value == "M")
        return tr("Male");
    if (value == "F")
        return tr("Female");
    if (value == "O")
        return tr("Other");
    return {};
}

QImage colorize(const QImage &grey, int map) {
    static QHash<int, QVector<QRgb>> tables;
    if (!tables.contains(map)) {
        QVector<QRgb> table(256);
        for (int g = 0; g < 256; ++g)
            table[g] = colorScale(map, g);
        tables.insert(map, table);
    }
    QImage indexed = grey.copy();
    indexed.reinterpretAsFormat(QImage::Format_Indexed8);
    indexed.setColorTable(tables.value(map));
    return indexed.convertToFormat(QImage::Format_RGB32);
}

QTransform turn(const QSizeF &size, int rotation, bool flipped) {
    const bool sideways = rotation % 2;
    QTransform t;
    t.translate((sideways ? size.height() : size.width()) / 2, (sideways ? size.width() : size.height()) / 2);
    if (flipped)
        t.scale(-1, 1);
    t.rotate(90 * rotation);
    t.translate(-size.width() / 2, -size.height() / 2);
    return t;
}

QImage trueShape(const QImage &image, double aspect) {
    if (image.isNull() || std::abs(aspect - 1) < 0.01)
        return image;
    return image.scaled(qRound(image.width() * std::max(1.0, aspect)), qRound(image.height() * std::max(1.0, 1 / aspect)),
                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

}

Backend::Backend(FilePicker *picker, QObject *parent) : QObject(parent), m_picker(picker) {
    m_pool.setMaxThreadCount(std::max(2, QThread::idealThreadCount()));
    // Thumbnails have threads of their own, so they don't wait for the
    // shown series to finish decoding.
    m_thumbnailPool.setMaxThreadCount(2);
    m_noticeTimer.setSingleShot(true);
    m_noticeTimer.setInterval(2400);
    connect(&m_noticeTimer, &QTimer::timeout, this, [this] {
        m_notice.clear();
        emit noticeChanged();
    });
    connect(&m_play, &QTimer::timeout, this, [this] {
        // Waits for the next slice rather than showing a gap.
        const int next = (m_sliceIndex + 1) % std::max(1, sliceCount());
        if (built() || (next < m_decoded.size() && m_decoded[next]))
            setSliceIndex(next);
    });
    if (m_picker) {
        // The panes share the picker; each takes only the answers to what
        // it asked.
        connect(m_picker, &FilePicker::selected, this,
                [this](QObject *asker, FilePicker::Purpose purpose, const QList<QUrl> &urls) {
            if (asker != this || urls.isEmpty())
                return;
            if (purpose == FilePicker::Purpose::SaveImage) {
                saveImage(urls.first().toLocalFile());
                return;
            }
            if (purpose == FilePicker::Purpose::SaveVideo) {
                saveVideo(urls.first().toLocalFile());
                return;
            }
            if (purpose == FilePicker::Purpose::SaveFolder) {
                saveAnonymous(urls.first().toLocalFile());
                return;
            }
            QVariantList list;
            for (const auto &url : urls)
                list << url;
            load(list);
        });
        connect(m_picker, &FilePicker::failed, this, [this](QObject *asker, const QString &message) {
            if (asker == this)
                setProblem(message);
        });
    }
}

Backend::~Backend() {
    if (m_scanCancel)
        *m_scanCancel = true;
    if (m_thumbnailCancel)
        *m_thumbnailCancel = true;
    if (m_job)
        m_job->cancelled = true;
    m_pool.clear();
    m_thumbnailPool.clear();
    m_pool.waitForDone();
    m_thumbnailPool.waitForDone();
}

void Backend::open() {
    if (!m_picker)
        return;
    m_picker->choose(this, FilePicker::Purpose::OpenScan);
}

void Backend::openFolder() {
    if (!m_picker)
        return;
    m_picker->choose(this, FilePicker::Purpose::OpenFolder);
}

void Backend::load(const QVariantList &urlsOrPaths) {
    QStringList paths;
    for (const auto &value : urlsOrPaths) {
        const QUrl url = value.toUrl();
        paths << (url.isLocalFile() ? url.toLocalFile() : value.toString());
    }
    loadPaths(paths);
}

void Backend::loadPaths(const QStringList &paths) {
    QStringList existing;
    for (const auto &path : paths)
        if (QFileInfo::exists(path))
            existing << QFileInfo(path).absoluteFilePath();
    if (existing.isEmpty()) {
        if (!paths.isEmpty())
            setProblem(tr("%1 doesn't exist").arg(paths.first()));
        return;
    }

    // One file opens with the rest of its folder, as scans come a file a
    // slice; that file's slice is the one shown.
    QString focus;
    QStringList roots = existing;
    bool recursive = true;
    if (existing.size() == 1 && QFileInfo(existing.first()).isFile() && !Archive::isZip(existing.first())) {
        focus = existing.first();
        roots = {QFileInfo(focus).absolutePath()};
        recursive = false;
    }

    if (m_scanCancel)
        *m_scanCancel = true;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_scanCancel = cancel;
    const int generation = ++m_scanGeneration;
    m_scanning = true;
    m_scanDone = 0;
    m_scanTotal = 0;
    emit scanningChanged();
    emit scanProgress();
    setProblem({});

    m_pool.start([this, roots, recursive, focus, cancel, generation]() mutable {
        // A zip is unpacked and read as the folder it holds.
        for (QString &root : roots)
            if (QFileInfo(root).isFile() && Archive::isZip(root)) {
                QString error;
                const QString dir = Archive::unpack(root, &error);
                if (dir.isEmpty()) {
                    QMetaObject::invokeMethod(this, [this, error, generation] {
                        if (generation != m_scanGeneration)
                            return;
                        m_scanning = false;
                        emit scanningChanged();
                        setProblem(tr(error.toUtf8().constData()));
                    }, Qt::QueuedConnection);
                    return;
                }
                if (root == focus)
                    focus.clear();
                root = dir;
                recursive = true;
            }
        const QStringList files = listFiles(roots, recursive);
        QElapsedTimer throttle;
        throttle.start();
        Scan scan = scanFiles(files, [&](int done, int total) {
            if (throttle.elapsed() < 50 && done != total)
                return;
            throttle.restart();
            QMetaObject::invokeMethod(this, [this, done, total, generation] {
                if (generation != m_scanGeneration)
                    return;
                m_scanDone = done;
                m_scanTotal = total;
                emit scanProgress();
            }, Qt::QueuedConnection);
        }, cancel.get());
        if (*cancel)
            return;
        QMetaObject::invokeMethod(this, [this, scan, focus, generation] { scanFinished(generation, scan, focus); },
                                  Qt::QueuedConnection);
    });
}

void Backend::scanFinished(int generation, const Scan &scan, const QString &focusPath) {
    if (generation != m_scanGeneration)
        return;
    m_scanning = false;
    emit scanningChanged();

    if (scan.series.isEmpty()) {
        // What was open stays open.
        setProblem(scan.unreadable.isEmpty() ? tr("No DICOM images were found there")
                                             : tr("The DICOM files there couldn't be read: %1").arg(scan.unreadable.first()));
        return;
    }

    // The file that was opened is the slice shown, among its folder's.
    int series = 0, slice = 0;
    for (int s = 0; s < scan.series.size() && !focusPath.isEmpty(); ++s)
        for (int i = 0; i < scan.series[s].slices.size(); ++i)
            if (scan.series[s].slices[i].path == focusPath) {
                series = s;
                slice = i;
                s = scan.series.size();
                break;
            }
    showScan(scan, series, slice);
    if (!scan.unreadable.isEmpty())
        setProblem(tr("%Ln files couldn't be read", nullptr, int(scan.unreadable.size())) + " — " + scan.unreadable.first());

    // A thumbnail a series, from its middle slice.
    if (m_thumbnailCancel)
        *m_thumbnailCancel = true;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_thumbnailCancel = cancel;
    for (int s = 0; s < m_scan.series.size(); ++s) {
        const Series copy = m_scan.series[s];
        m_thumbnailPool.start([this, copy, s, generation, cancel] {
            if (*cancel)
                return;
            // A video's first frame is cheap; its middle would mean
            // decoding half of it.
            const Slice &middle = copy.slices[copy.slices.size() / 2];
            const Frame frame = isVideo(copy.transferSyntax)
                                    ? decodeVideo(DicomFile::read(copy.slices.first().path), 1).value(0)
                                    : decodeFrame(DicomFile::read(middle.path), middle.frame);
            if (*cancel || frame.isNull() || !frame.error.isEmpty())
                return;
            const QImage image = trueShape(render(frame, frame.windowCenter, frame.windowWidth, false), copy.pixelAspect())
                                     .scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QMetaObject::invokeMethod(this, [this, generation, s, image] { thumbnailDecoded(generation, s, image); },
                                      Qt::QueuedConnection);
        });
    }
}

void Backend::showScan(const Scan &scan, int series, int slice) {
    stop();
    dropFusion();
    if (m_job)
        m_job->cancelled = true;
    m_job.reset();
    m_scan = scan;
    m_seriesWindows.clear();
    m_seriesFileWindows.clear();
    {
        QMutexLocker lock(&m_thumbnailMutex);
        m_thumbnails.clear();
    }
    m_seriesIndex = -1;
    closeReport();
    emit seriesChanged();
    emit reportsChanged();
    selectSeries(series);
    setSliceIndex(slice);
    emit opened();
}

void Backend::share(Backend *other) {
    if (!other || other == this || other->m_scan.series.isEmpty())
        return;
    // Already showing that scan: what it shows stays.
    auto firstPath = [](const Scan &scan) { return scan.series.isEmpty() ? QString() : scan.series.first().slices.first().path; };
    if (m_scan.series.size() == other->m_scan.series.size() && firstPath(m_scan) == firstPath(other->m_scan))
        return;
    // Drops a scan of its own still being read.
    ++m_scanGeneration;
    m_scanning = false;
    emit scanningChanged();
    // Beside a series goes the same kind from another study, the closest
    // in time, to compare with; failing that, the next series.
    const QList<Series> &all = other->m_scan.series;
    const Series *shown = other->currentSeries();
    int pick = (other->m_seriesIndex + 1) % all.size();
    qint64 nearest = -1;
    const QDate when = QDate::fromString(shown ? shown->studyDate : QString(), "yyyyMMdd");
    for (int i = 0; shown && i < all.size(); ++i) {
        const Series &s = all[i];
        if (s.studyUid == shown->studyUid || s.modality != shown->modality || s.slices.size() < 2)
            continue;
        const QDate date = QDate::fromString(s.studyDate, "yyyyMMdd");
        const qint64 apart = when.isValid() && date.isValid() ? std::abs(date.daysTo(when)) : 0;
        if (nearest < 0 || apart < nearest) {
            nearest = apart;
            pick = i;
        }
    }
    showScan(other->m_scan, pick, 0);
}

void Backend::mirror(Backend *leader, int plane) {
    for (const auto &c : std::as_const(m_leaderConnections))
        disconnect(c);
    m_leaderConnections.clear();
    m_leader = leader == this ? nullptr : leader;
    if (!m_leader)
        return;
    // Drops a scan of its own still being read.
    ++m_scanGeneration;
    m_scanning = false;
    emit scanningChanged();
    auto firstPath = [](const Scan &scan) { return scan.series.isEmpty() ? QString() : scan.series.first().slices.first().path; };
    if (m_scan.series.size() != leader->m_scan.series.size() || firstPath(m_scan) != firstPath(leader->m_scan)) {
        stop();
        dropFusion();
        if (m_job)
            m_job->cancelled = true;
        m_job.reset();
        m_scan = leader->m_scan;
        m_seriesWindows.clear();
        m_seriesFileWindows.clear();
        {
            QMutexLocker lock(&m_thumbnailMutex);
            m_thumbnails.clear();
        }
        m_seriesIndex = -1;
        emit seriesChanged();
        emit reportsChanged();
    }
    m_leaderConnections << connect(leader, &Backend::frameArrived, this, &Backend::adoptFrame);
    m_leaderConnections << connect(leader, &Backend::fusionChanged, this, &Backend::adoptFusion);
    m_leaderConnections << connect(leader, &QObject::destroyed, this, [this] { mirror(nullptr, 0); });
    if (m_seriesIndex == leader->m_seriesIndex) {
        // Already showing it: what's read already is taken over.
        if (m_job)
            m_job->cancelled = true;
        m_job.reset();
        for (int i = 0; i < leader->m_frames.size() && i < m_frames.size(); ++i)
            if (leader->m_decoded.value(i) && !m_decoded.value(i))
                storeFrame(i, leader->m_frames[i]);
    } else {
        selectSeries(leader->m_seriesIndex);
    }
    matchWindow(leader);
    if (plane != m_plane) {
        if (m_plane == 3) {
            m_plane = 0;   // An oblique plane goes back to the volume's own first.
            m_oblique = Oblique();
        }
        setPlane(plane);
    }
    // The planes meet where the leader's do.
    if (m_volume.valid && leader->m_volume.valid) {
        for (int a = 0; a < 3; ++a)
            m_cursor[a] = std::clamp(leader->m_cursor[a], 0, m_volume.size[a] - 1);
        m_crossingShown = leader->m_crossingShown;
        if (m_plane != 0 && m_plane != 3)
            setSliceIndex(m_cursor[m_cut.fixed]);
    }
    adoptFusion();
}

void Backend::adoptFrame(int index) {
    if (!m_leader || m_leader->m_seriesIndex != m_seriesIndex || index < 0 || index >= m_frames.size() ||
        index >= m_leader->m_frames.size() || m_job)
        return;
    storeFrame(index, m_leader->m_frames[index]);
}

void Backend::adoptFusion() {
    if (!m_leader)
        return;
    const bool was = m_fused;
    if (m_leader->m_fused && m_leader->m_seriesIndex == m_seriesIndex) {
        m_fused = true;
        m_fusionSeries = m_leader->m_fusionSeries;
        m_fusionGrid = m_leader->m_fusionGrid;
        m_fusionLow = m_leader->m_fusionLow;
        m_fusionHigh = m_leader->m_fusionHigh;
        m_fusionOpacity = m_leader->m_fusionOpacity;
    } else {
        if (m_fusionJob)
            m_fusionJob->cancelled = true;
        m_fusionJob.reset();
        m_fused = false;
        m_fusionGrid.reset();
        m_fusionSeries = -1;
    }
    if (was || m_fused) {
        rerender();
        emit fusionChanged();
    }
}

void Backend::matchWindow(Backend *other) {
    if (!other || other == this || other->m_seriesIndex != m_seriesIndex || !other->m_windowSet)
        return;
    if (m_windowSet && m_center == other->m_center && m_width == other->m_width && m_inverted == other->m_inverted &&
        m_colorMap == other->m_colorMap)
        return;
    m_center = other->m_center;
    m_width = other->m_width;
    m_inverted = other->m_inverted;
    m_colorMap = other->m_colorMap;
    m_fileWindow = other->m_fileWindow;
    m_windowSet = true;
    emit windowChanged();
    rerender();
}

void Backend::follow(Backend *other) {
    const Series *mine = currentSeries();
    const Series *theirs = other ? other->currentSeries() : nullptr;
    if (!mine || !theirs || mine->slices.isEmpty() || theirs->slices.isEmpty())
        return;

    // The picture at the same place in the patient, when both cut the body
    // the same way; otherwise as far through.
    const ImagePlane there = other->imagePlane(), here = imagePlane();
    double a[3], b[3];
    here.normal(a);
    there.normal(b);
    const bool parallel = here.valid && there.valid && std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) > 0.95;
    // Planes that cross in one body stay put: the reference line shows
    // where the other is.
    if (!parallel && here.valid && there.valid && !here.frame.isEmpty() && here.frame == there.frame)
        return;
    int best = -1;
    if (parallel && m_plane == 3) {
        best = m_oblique.indexAt(there.origin);
    } else if (parallel && m_plane != 0) {
        const int f = m_cut.fixed;
        double along = 0;
        for (int i = 0; i < 3; ++i)
            along += (there.origin[i] - m_volume.origin[i]) * m_volume.axis[f][i];
        best = std::clamp(int(std::lround(along / m_volume.spacing[f])), 0, sliceCount() - 1);
    } else if (parallel) {
        const double target = there.origin[0] * a[0] + there.origin[1] * a[1] + there.origin[2] * a[2];
        double nearest = 0;
        for (int i = 0; i < mine->slices.size(); ++i) {
            const Slice &s = mine->slices[i];
            if (!s.hasPosition)
                continue;
            const double d = std::abs(s.position[0] * a[0] + s.position[1] * a[1] + s.position[2] * a[2] - target);
            if (best < 0 || d < nearest) {
                best = i;
                nearest = d;
            }
        }
    }
    if (best < 0) {
        const double fraction = other->sliceCount() > 1 ? double(other->m_sliceIndex) / (other->sliceCount() - 1) : 0;
        best = int(std::lround(fraction * (sliceCount() - 1)));
    }
    setSliceIndex(best);
}

void Backend::thumbnailDecoded(int generation, int series, const QImage &image) {
    if (generation != m_scanGeneration)
        return;
    {
        QMutexLocker lock(&m_thumbnailMutex);
        m_thumbnails.insert(series, image);
    }
    ++m_thumbnailsVersion;
    emit thumbnailsChanged();
}

QImage Backend::thumbnail(int series) const {
    QMutexLocker lock(&m_thumbnailMutex);
    return m_thumbnails.value(series);
}

QVariantList Backend::seriesList() const {
    QVariantList list;
    QString study;
    for (int i = 0; i < m_scan.series.size(); ++i) {
        const Series &s = m_scan.series[i];
        const QString key = s.studyUid + s.patientName;
        QString title = s.description;
        if (title.isEmpty())
            title = s.number.isEmpty() ? tr("Untitled series") : tr("Series %1").arg(s.number);
        list << QVariantMap{
            {"title", title},
            {"modality", s.modality},
            {"count", s.slices.size()},
            {"number", s.number},
            // The first series of each study carries its heading.
            {"study", i == 0 || key != study ? (s.studyDescription.isEmpty() ? tr("Study") : s.studyDescription) : QString()},
            {"studyDate", i == 0 || key != study ? localeDate(s.studyDate) : QString()},
            {"decodable", canDecode(s.transferSyntax)},
        };
        study = key;
    }
    return list;
}

const Series *Backend::currentSeries() const {
    return m_seriesIndex >= 0 && m_seriesIndex < m_scan.series.size() ? &m_scan.series[m_seriesIndex] : nullptr;
}

const Frame *Backend::currentFrame() const {
    if (built())
        return m_built.isNull() && m_buildError.isEmpty() ? nullptr : &m_built;
    if (m_sliceIndex < 0 || m_sliceIndex >= m_frames.size() || !m_decoded.value(m_sliceIndex))
        return nullptr;
    return &m_frames[m_sliceIndex];
}

int Backend::sliceCount() const {
    const Series *s = currentSeries();
    if (s && m_plane == 3)
        return m_oblique.count;
    if (s && m_plane != 0)
        return m_cut.count(m_volume);
    return s ? s->slices.size() : 0;
}

int Backend::acquiredSlice() const {
    return m_plane != 0 ? m_cursor[2] : m_sliceIndex;
}

void Backend::build() {
    m_built = Frame();
    m_buildError.clear();
    // Every slice goes into the picture, so it waits for them all.
    if (!built() || !m_volume.valid || m_decodedCount < m_frames.size())
        return;
    int half = 0;
    Projection projection = Projection::Max;
    if (m_plane == 3) {
        if (!m_rangeKnown) {
            bool first = true;
            for (const Frame &f : std::as_const(m_frames)) {
                if (f.isNull() || f.isColor())
                    continue;
                double low, high;
                f.range(low, high);
                m_low = first ? low : std::min(m_low, low);
                m_high = first ? high : std::max(m_high, high);
                first = false;
            }
            m_rangeKnown = !first;
        }
        slabFor(m_oblique.step, half, projection);
        m_built = resample(m_volume, m_oblique, m_sliceIndex, m_frames, m_low, m_high, half, projection);
    } else {
        const Cut cut = m_plane != 0 ? m_cut : cutAcross(m_volume, 2);
        slabFor(m_volume.spacing[cut.fixed], half, projection);
        m_built = reformat(m_volume, cut, m_sliceIndex, m_frames, half, projection);
    }
    if (m_built.isNull())
        m_buildError = QT_TRANSLATE_NOOP("dicolens", "Some slices can't be read, so this picture can't be made");
}

void Backend::slabFor(double step, int &half, Projection &projection) const {
    half = 0;
    projection = Projection::Max;
    if (m_slab == 0)
        return;
    half = std::max(1, int(std::lround(m_slabMm / step / 2)));
    projection = m_slab == 1 ? Projection::Max : m_slab == 2 ? Projection::Min : Projection::Mean;
}

void Backend::crossingPoint(double *point) const {
    const double voxel[3] = {double(m_cursor[0]), double(m_cursor[1]), double(m_cursor[2])};
    toPatient(m_volume, voxel, point);
}

bool Backend::crossing(QPointF &at) const {
    if (!m_crossingShown || !m_volume.valid)
        return false;
    const ImagePlane p = imagePlane();
    if (!p.valid)
        return false;
    double point[3], n[3], d[3];
    crossingPoint(point);
    p.normal(n);
    for (int i = 0; i < 3; ++i)
        d[i] = point[i] - p.origin[i];
    // Only in the plane, give or take the voxel it was rounded to.
    const double off = std::abs(d[0] * n[0] + d[1] * n[1] + d[2] * n[2]);
    if (off > std::max({m_volume.spacing[0], m_volume.spacing[1], m_volume.spacing[2]}))
        return false;
    at = QPointF((d[0] * p.row[0] + d[1] * p.row[1] + d[2] * p.row[2]) / p.rowSpacing,
                 (d[0] * p.column[0] + d[1] * p.column[1] + d[2] * p.column[2]) / p.columnSpacing);
    return true;
}

QVariantList Backend::pointAt(double x, double y) const {
    const ImagePlane p = imagePlane();
    if (!p.valid || p.frame.isEmpty() || x < -0.5 || y < -0.5 || x > p.width - 0.5 || y > p.height - 0.5)
        return {};
    double point[3];
    p.point(x, y, point);
    return {point[0], point[1], point[2], p.frame};
}

bool Backend::goThrough(const QVariantList &point) {
    const Series *s = currentSeries();
    if (!s || point.size() != 4 || s->frame.isEmpty() || point[3].toString() != s->frame)
        return false;
    const double p[3] = {point[0].toDouble(), point[1].toDouble(), point[2].toDouble()};
    if (!m_volume.valid) {
        // A plain stack: the slice nearest the point.
        int best = -1;
        double nearest = 0;
        for (int i = 0; i < s->slices.size(); ++i) {
            const ImagePlane plane = slicePlane(*s, i);
            if (!plane.valid)
                continue;
            double n[3];
            plane.normal(n);
            const double d = std::abs((p[0] - plane.origin[0]) * n[0] + (p[1] - plane.origin[1]) * n[1] + (p[2] - plane.origin[2]) * n[2]);
            if (best < 0 || d < nearest) {
                best = i;
                nearest = d;
            }
        }
        if (best >= 0)
            setSliceIndex(best);
        return best >= 0;
    }
    double voxel[3];
    toVoxel(m_volume, p, voxel);
    for (int a = 0; a < 3; ++a)
        m_cursor[a] = std::clamp(int(std::lround(voxel[a])), 0, m_volume.size[a] - 1);
    m_crossingShown = true;
    const int index = m_plane == 3 ? m_oblique.indexAt(p) : m_plane != 0 ? m_cursor[m_cut.fixed] : m_cursor[2];
    if (index != m_sliceIndex) {
        // Moving along keeps the point; setSliceIndex would round it.
        const int keep[3] = {m_cursor[0], m_cursor[1], m_cursor[2]};
        setSliceIndex(index);
        std::copy(keep, keep + 3, m_cursor);
    }
    emit sliceChanged();
    return true;
}

void Backend::becomeOblique(const double *center, const double *row, const double *column) {
    // Shown the usual way up, whichever way it was turned to face.
    const double n[3] = {row[1] * column[2] - row[2] * column[1], row[2] * column[0] - row[0] * column[2],
                         row[0] * column[1] - row[1] * column[0]};
    double r[3] = {row[0], row[1], row[2]}, c[3] = {column[0], column[1], column[2]};
    upright(n, r, c);
    const Oblique o = obliqueThrough(m_volume, center, r, c);
    if (!o.valid)
        return;
    stop();
    m_oblique = o;
    m_plane = 3;
    m_sliceIndex = o.middle;
    m_image = QImage();
    build();
    updateInfo();
    emit sliceChanged();
    emit orientationChanged();
    showSlice();
}

void Backend::tilt(int axis, double degrees) {
    if (!currentSeries())
        return;
    if (!m_volume.valid) {
        flash(tr("This series can't be cut another way"));
        return;
    }
    const ImagePlane p = imagePlane();
    if (!p.valid)
        return;
    double row[3] = {p.row[0], p.row[1], p.row[2]}, column[3] = {p.column[0], p.column[1], p.column[2]};
    // Up-down turns the row about the column; left-right the column
    // about the row.
    const double radians = degrees * M_PI / 180;
    if (axis == 0)
        turnAbout(p.column, radians, row);
    else
        turnAbout(p.row, radians, column);
    // Through the point the planes meet at, moved into this plane.
    double center[3], n[3];
    crossingPoint(center);
    p.normal(n);
    const double off = (center[0] - p.origin[0]) * n[0] + (center[1] - p.origin[1]) * n[1] + (center[2] - p.origin[2]) * n[2];
    for (int i = 0; i < 3; ++i)
        center[i] -= off * n[i];
    becomeOblique(center, row, column);
    flash(planeName());
}

void Backend::beginTurn() {
    const ImagePlane p = imagePlane();
    std::copy(p.row, p.row + 3, m_turnRow);
    std::copy(p.column, p.column + 3, m_turnColumn);
}

void Backend::turnPlane(const double *axis, double radians) {
    if (!m_volume.valid || !imagePlane().valid)
        return;
    double row[3], column[3], center[3];
    std::copy(m_turnRow, m_turnRow + 3, row);
    std::copy(m_turnColumn, m_turnColumn + 3, column);
    turnAbout(axis, radians, row);
    turnAbout(axis, radians, column);
    crossingPoint(center);
    becomeOblique(center, row, column);
}

ImagePlane Backend::imagePlane() const {
    const Series *s = currentSeries();
    if (!s)
        return {};
    if (m_plane == 3)
        return m_oblique.plane(m_sliceIndex);
    if (m_plane != 0)
        return cutPlane(m_volume, m_cut, m_sliceIndex);
    return slicePlane(*s, m_sliceIndex);
}

QString Backend::cutKey() const {
    if (m_plane != 3)
        return {};
    // Where the stack's first plane lies and how it's turned, to the
    // hundredth of a millimetre, which a tilt of any size moves.
    const ImagePlane p = m_oblique.plane(0);
    QStringList parts;
    for (const double *vector : {p.origin, p.row, p.column})
        for (int i = 0; i < 3; ++i)
            parts << QString::number(std::round(vector[i] * 100) / 100 + 0.0, 'f', 2);
    return parts.join(',');
}

QString Backend::planeName() const {
    const ImagePlane p = imagePlane();
    if (!p.valid)
        return {};
    // A cut the user tilted is oblique once it's off the body's axes,
    // though a scanner's slices are named by the axis they're nearest.
    if (m_plane == 3) {
        double n[3];
        p.normal(n);
        if (std::max({std::abs(n[0]), std::abs(n[1]), std::abs(n[2])}) < 0.9998)
            return tr("Oblique");
    }
    return tr(::planeName(p));
}

QString Backend::slabName() const {
    const QString mm = number(m_slabMm, 0);
    switch (m_slab) {
    case 1: return tr("MIP %1 mm").arg(mm);
    case 2: return tr("MinIP %1 mm").arg(mm);
    case 3: return tr("Average %1 mm").arg(mm);
    }
    return {};
}

void Backend::nextPlane() {
    if (!currentSeries())
        return;
    if (!m_volume.valid) {
        flash(tr("This series can't be cut another way"));
        return;
    }
    setPlane(m_plane == 3 ? 0 : (m_plane + 1) % 3);
    flash(planeName());
}

void Backend::setPlane(int plane) {
    if (plane < 0 || plane > 2 || plane == m_plane || (plane != 0 && !m_volume.valid))
        return;
    stop();
    // The planes meet at one point, which stays where it is.
    if (m_plane == 3)
        ;   // Kept as the oblique plane moved.
    else if (m_plane != 0)
        m_cursor[m_cut.fixed] = m_sliceIndex;
    else
        m_cursor[2] = m_sliceIndex;
    m_plane = plane;
    // An axial stack's rows make coronal planes, and its columns sagittal.
    m_cut = cutAcross(m_volume, plane == 0 ? 2 : plane == 1 ? 1 : 0);
    m_sliceIndex = std::clamp(m_cursor[m_cut.fixed], 0, std::max(0, sliceCount() - 1));
    m_rotation = 0;
    m_flipped = false;
    m_image = QImage();
    if (m_job)
        m_job->wanted = acquiredSlice();
    build();
    updateInfo();
    emit sliceChanged();
    emit orientationChanged();
    showSlice();
    updateTags();
}

void Backend::nextSlab() {
    if (!currentSeries())
        return;
    if (!m_volume.valid) {
        flash(tr("This series can't be made into slabs"));
        return;
    }
    m_slab = (m_slab + 1) % 4;
    build();
    updateInfo();
    emit sliceChanged();
    emit orientationChanged();
    showSlice();
    flash(m_slab == 0 ? tr("Thin slices") : slabName());
}

void Backend::nextSlabThickness() {
    if (m_slab == 0) {
        nextSlab();
        return;
    }
    static const double sizes[] = {5, 10, 20, 40};
    double next = sizes[0];
    for (double s : sizes)
        if (s > m_slabMm + 0.5) {
            next = s;
            break;
        }
    if (m_slabMm >= 40)
        next = sizes[0];
    m_slabMm = next;
    build();
    updateInfo();
    emit orientationChanged();
    showSlice();
    flash(slabName());
}

bool Backend::frameReady() const {
    if (built() && !m_buildError.isEmpty())
        return false;
    const Frame *f = currentFrame();
    return f && f->error.isEmpty() && !f->isNull();
}

QString Backend::frameError() const {
    if (built() && !m_buildError.isEmpty())
        return tr(m_buildError.toUtf8().constData());
    const Frame *f = currentFrame();
    if (!f || f->error.isEmpty())
        return {};
    const QString text = tr(f->error.toUtf8().constData());
    return f->detail.isEmpty() ? text : text + "\n" + f->detail;
}

bool Backend::color() const {
    const Frame *f = currentFrame();
    return f && f->isColor();
}

double Backend::pixelAspect() const {
    const Series *s = currentSeries();
    if (s && m_plane == 3)
        return 1.0;
    if (s && m_plane != 0)
        return m_volume.spacing[m_cut.horizontal] / m_volume.spacing[m_cut.vertical];
    return s ? s->pixelAspect() : 1.0;
}

QStringList Backend::edges() const {
    const Series *s = currentSeries();
    if (!s)
        return {};
    QStringList e = s->edgeLabels();   // Left, right, top, bottom.
    if (m_plane != 0) {
        const ImagePlane p = imagePlane();
        e = ::edgeLabels(p.row, p.column);
    }
    if (e.size() != 4)
        return e;
    // Each clockwise turn brings the left edge to the top.
    for (int i = 0; i < m_rotation; ++i)
        e = {e[3], e[2], e[0], e[1]};
    if (m_flipped)
        e.swapItemsAt(0, 1);
    return e;
}

QTransform Backend::orientation(const QSizeF &size) const {
    return turn(size, m_rotation, m_flipped);
}

void Backend::rotate(int quarterTurns) {
    if (!currentSeries())
        return;
    // Turning a flipped picture turns its unflipped self the other way.
    m_rotation = ((m_rotation + (m_flipped ? -quarterTurns : quarterTurns)) % 4 + 4) % 4;
    emit orientationChanged();
}

void Backend::flipHorizontal() {
    if (!currentSeries())
        return;
    m_flipped = !m_flipped;
    emit orientationChanged();
}

void Backend::flipVertical() {
    if (!currentSeries())
        return;
    // Top to bottom is left to right, then half a turn.
    m_flipped = !m_flipped;
    m_rotation = (m_rotation + 2) % 4;
    emit orientationChanged();
}

void Backend::resetOrientation() {
    if (m_rotation == 0 && !m_flipped)
        return;
    m_rotation = 0;
    m_flipped = false;
    emit orientationChanged();
}

void Backend::toggleMarks() {
    m_marksShown = !m_marksShown;
    emit marksChanged();
    if (hasMarks())
        flash(m_marksShown ? tr("Showing marks") : tr("Hiding marks"));
    else
        flash(tr("This image has no marks"));
}

QImage Backend::overlay() const {
    const Frame *f = currentFrame();
    return f ? f->overlay : QImage();
}

QList<Mark> Backend::marks() const {
    QList<Mark> out;
    const Series *s = currentSeries();
    // Marks belong to a slice, not to a plane cut across many.
    if (!s || m_scan.marks.isEmpty() || m_plane != 0)
        return out;
    const Slice &slice = s->slices.value(m_sliceIndex);
    if (slice.sop.isEmpty())
        return out;
    for (const auto &m : m_scan.marks)
        if (m.appliesTo(slice.sop, slice.frame))
            out << m;
    return out;
}

bool Backend::hasMarks() const {
    return !overlay().isNull() || !marks().isEmpty();
}

QVariantList Backend::reportList() const {
    QVariantList list;
    const Series *s = currentSeries();
    for (const auto &r : m_scan.reports)
        list << QVariantMap{
            {"title", r.title.isEmpty() ? tr("Report") : r.title},
            {"date", localeDate(r.date)},
            {"pdf", r.kind == Report::Kind::Pdf},
            {"study", r.studyDescription},
            {"current", s && s->studyUid == r.studyUid},
        };
    return list;
}

QString Backend::reportTitle() const {
    if (m_reportIndex < 0 || m_reportIndex >= m_scan.reports.size())
        return {};
    const QString title = m_scan.reports[m_reportIndex].title;
    return title.isEmpty() ? tr("Report") : title;
}

void Backend::openReport(int index) {
    selectReport(index, true);
}

bool Backend::reportIsPdf() const {
    return m_reportIndex >= 0 && m_reportIndex < m_scan.reports.size()
           && m_scan.reports[m_reportIndex].kind == Report::Kind::Pdf;
}

void Backend::selectReport(int index, bool launch) {
    if (index < 0 || index >= m_scan.reports.size())
        return;
    const Report &report = m_scan.reports[index];
    const DicomFile file = DicomFile::read(report.path);
    m_reportIndex = index;
    m_reportLines.clear();
    if (report.kind == Report::Kind::Structured) {
        for (const auto &line : Reports::lines(file))
            m_reportLines << QVariantMap{{"depth", line.depth}, {"heading", line.heading}, {"name", line.name}, {"value", line.value}};
        if (m_reportLines.isEmpty())
            m_reportLines << QVariantMap{{"depth", 0}, {"heading", false}, {"name", QString()}, {"value", tr("This report is empty")}};
    }
    emit reportChanged();
    // A PDF waits in the panel until it's asked for, so stepping past one
    // doesn't launch a viewer.
    if (report.kind != Report::Kind::Pdf || !launch)
        return;

    // It opens in the PDF viewer, from a copy in the user's runtime folder,
    // which only they can read and which is gone at logout.
    const QByteArray pdf = Reports::pdf(file);
    if (!pdf.startsWith("%PDF")) {
        setProblem(tr("This report can't be read"));
        return;
    }
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
    if (!out.open(QIODevice::WriteOnly) || out.write(pdf) != pdf.size() || !out.commit()) {
        setProblem(tr("The report couldn't be opened: %1").arg(out.errorString()));
        return;
    }
    const QUrl url = QUrl::fromLocalFile(path);
    if (!(m_openUrl ? m_openUrl(url) : QDesktopServices::openUrl(url)))
        setProblem(tr("No app is set to open PDF files"));
}

void Backend::openStudyReport() {
    if (m_scan.reports.isEmpty()) {
        flash(tr("No reports came with these scans"));
        return;
    }
    const Series *s = currentSeries();
    int index = 0;
    for (int i = 0; i < m_scan.reports.size(); ++i)
        if (s && m_scan.reports[i].studyUid == s->studyUid) {
            index = i;
            break;
        }
    selectReport(index, false);
}

void Backend::stepReport(int delta) {
    if (m_reportIndex < 0 || m_scan.reports.isEmpty())
        return;
    const int index = std::clamp(m_reportIndex + delta, 0, int(m_scan.reports.size()) - 1);
    if (index != m_reportIndex)
        selectReport(index, false);
}

void Backend::closeReport() {
    if (m_reportIndex < 0)
        return;
    m_reportIndex = -1;
    m_reportLines.clear();
    emit reportChanged();
}

void Backend::selectSeries(int index) {
    if (index < 0 || index >= m_scan.series.size() || index == m_seriesIndex)
        return;
    stop();
    dropFusion();
    if (m_seriesIndex >= 0 && m_windowSet)
        m_seriesWindows.insert(m_seriesIndex, {m_center, m_width, m_inverted});
    m_seriesIndex = index;
    m_sliceIndex = 0;
    m_undo.clear();
    m_redo.clear();
    emit historyChanged();
    m_windowSet = m_seriesWindows.contains(index);
    if (m_windowSet) {
        const WindowState w = m_seriesWindows.value(index);
        m_center = w.center;
        m_width = w.width;
        m_inverted = w.inverted;
        m_fileWindow = m_seriesFileWindows.value(index);
        emit windowChanged();
    }
    m_image = QImage();
    // A turn suits one series; the next is shown as it was taken.
    m_rotation = 0;
    m_flipped = false;
    // PET starts in its colours; the rest in grey.
    m_colorMap = m_scan.series[index].modality == "PT" ? 2 : 0;
    emit windowChanged();
    const Series &chosen = m_scan.series[index];
    m_volume = makeVolume(chosen);
    m_plane = 0;
    m_slab = 0;
    m_cut = Cut();
    m_cursor[0] = chosen.columns / 2;
    m_cursor[1] = chosen.rows / 2;
    m_cursor[2] = 0;
    m_crossingShown = false;
    m_oblique = Oblique();
    m_rangeKnown = false;
    m_built = Frame();
    m_buildError.clear();
    startDecoding();
    updateInfo();
    emit sliceChanged();
    emit orientationChanged();
    emit reportsChanged();
    showSlice();
    updateTags();
}

void Backend::stepSeries(int delta) {
    if (m_scan.series.isEmpty())
        return;
    selectSeries(std::clamp(m_seriesIndex + delta, 0, int(m_scan.series.size()) - 1));
}

void Backend::startDecoding() {
    if (m_job)
        m_job->cancelled = true;
    const Series *series = currentSeries();
    m_frames = QVector<Frame>(series ? series->slices.size() : 0);
    m_decoded = QVector<bool>(m_frames.size(), false);
    m_decodedCount = 0;
    emit decodedChanged();
    m_job.reset();
    if (!series)
        return;
    // A mirror takes what its leader has read, and the rest as it comes.
    if (m_leader && m_leader->m_seriesIndex == m_seriesIndex && m_leader->m_frames.size() == m_frames.size()) {
        m_frames = m_leader->m_frames;
        m_decoded = m_leader->m_decoded;
        m_decodedCount = m_leader->m_decodedCount;
        emit decodedChanged();
        return;
    }
    m_job = decode(*series, m_sliceIndex, [this](int generation, int index, const Frame &frame) {
        frameDecoded(generation, index, frame);
    });
}

std::shared_ptr<DecodeJob> Backend::decode(const Series &series, int wanted,
                                           std::function<void(int, int, const Frame &)> done) {
    auto job = std::make_shared<DecodeJob>();
    job->generation = ++m_jobGeneration;
    job->slices = series.slices;
    job->claimed = QVector<char>(series.slices.size(), 0);
    job->wanted = wanted;
    job->video = isVideo(series.transferSyntax);

    QPointer<Backend> self(this);
    auto post = [self, job, done](int index, const Frame &frame) {
        QMetaObject::invokeMethod(qApp, [self, job, index, frame, done] {
            if (self)
                done(job->generation, index, frame);
        }, Qt::QueuedConnection);
    };

    if (job->video) {
        // A video is one stream; ffmpeg decodes it whole.
        m_pool.start([job, post] {
            QString error;
            const DicomFile file = DicomFile::read(std::as_const(job->slices).first().path, &error);
            const QList<Frame> frames = decodeVideo(file);
            for (int i = 0; i < job->slices.size() && !job->cancelled; ++i)
                post(i, frames.value(std::min<int>(std::as_const(job->slices)[i].frame, frames.size() - 1)));
        });
        return job;
    }

    const int workers = std::min<int>(m_pool.maxThreadCount(), job->slices.size());
    for (int w = 0; w < workers; ++w)
        m_pool.start([job, post] {
            while (!job->cancelled) {
                const int index = job->claim();
                if (index < 0)
                    return;
                const Slice slice = std::as_const(job->slices)[index];
                QString error;
                std::shared_ptr<QList<QByteArray>> split;
                const auto file = job->open(slice.path, split, &error);
                Frame frame;
                if (!file->isValid()) {
                    frame.error = QT_TRANSLATE_NOOP("dicolens", "This file can't be read");
                    frame.detail = error;
                } else {
                    frame = decodeFrame(*file, slice.frame, split.get());
                    if (frame.error.isEmpty() && !frame.isNull())
                        frame.overlay = Marks::overlay(*file, slice.frame, frame.height, frame.width);
                }
                if (!job->cancelled)
                    post(index, frame);
            }
        });
    return job;
}

void Backend::frameDecoded(int generation, int index, const Frame &frame) {
    if (!m_job || generation != m_job->generation || index < 0 || index >= m_frames.size())
        return;
    storeFrame(index, frame);
    emit frameArrived(index);
}

void Backend::storeFrame(int index, const Frame &frame) {
    m_frames[index] = frame;
    if (!m_decoded[index]) {
        m_decoded[index] = true;
        ++m_decodedCount;
        emit decodedChanged();
    }
    if (built()) {
        if (m_decodedCount == m_frames.size()) {
            build();
            showSlice();
        }
        return;
    }
    if (index == m_sliceIndex)
        showSlice();
}

void Backend::setSliceIndex(int index) {
    const int count = sliceCount();
    if (count == 0)
        return;
    index = std::clamp(index, 0, count - 1);
    if (index == m_sliceIndex && m_seriesIndex >= 0 && !m_image.isNull())
        return;
    m_sliceIndex = index;
    if (m_plane == 3) {
        // The point moves along with the plane.
        double point[3], voxel[3];
        const ImagePlane p = m_oblique.plane(index);
        crossingPoint(point);
        double n[3];
        p.normal(n);
        const double off = (point[0] - p.origin[0]) * n[0] + (point[1] - p.origin[1]) * n[1] + (point[2] - p.origin[2]) * n[2];
        for (int i = 0; i < 3; ++i)
            point[i] -= off * n[i];
        toVoxel(m_volume, point, voxel);
        for (int a = 0; a < 3; ++a)
            m_cursor[a] = std::clamp(int(std::lround(voxel[a])), 0, m_volume.size[a] - 1);
    } else if (m_plane != 0) {
        m_cursor[m_cut.fixed] = index;
    } else {
        m_cursor[2] = index;
    }
    if (m_job)
        m_job->wanted = acquiredSlice();
    build();
    updateInfo();
    emit sliceChanged();
    showSlice();
    updateTags();
}

void Backend::step(int delta) {
    if (sliceCount() > 0)
        setSliceIndex(m_sliceIndex + delta);
}

void Backend::showSlice() {
    const Frame *frame = currentFrame();
    if (frame && frame->error.isEmpty() && !frame->isNull() && !m_windowSet) {
        // The first slice seen sets the window for the whole series, so
        // scrolling doesn't flicker between each file's own.
        m_fileWindow = {frame->windowCenter, frame->windowWidth, false};
        m_seriesFileWindows.insert(m_seriesIndex, m_fileWindow);
        m_center = frame->windowCenter;
        m_width = frame->windowWidth;
        m_windowSet = true;
        emit windowChanged();
    }
    if (frame && !frame->error.isEmpty())
        m_image = QImage();
    rerender();
}

void Backend::rerender() {
    // Until the slice arrives, the last picture stays up rather than a gap.
    if (const Frame *frame = currentFrame(); frame && frame->error.isEmpty()) {
        m_image = render(*frame, m_center, m_width, m_inverted);
        if (m_colorMap != 0 && m_image.format() == QImage::Format_Grayscale8)
            m_image = colorize(m_image, m_colorMap);
        if (m_fused && m_fusionGrid && !frame->isColor())
            m_image = fuseSlice(m_image, imagePlane(), *m_fusionGrid, fusionLook());
    }
    emit imageChanged();
}

QString Backend::colorMapName() const {
    switch (m_colorMap) {
    case 1: return tr("Hot iron");
    case 2: return tr("PET colours");
    case 3: return tr("Rainbow");
    }
    return {};
}

void Backend::nextColorMap() {
    if (!hasImages())
        return;
    if (color()) {
        flash(tr("Colour scales are for grey images"));
        return;
    }
    m_colorMap = (m_colorMap + 1) % 4;
    emit windowChanged();
    rerender();
    flash(m_colorMap == 0 ? tr("Grey") : colorMapName());
}

QString Backend::windowName() const {
    for (const auto &p : Presets)
        if (p.center == m_center && p.width == m_width)
            return tr(p.name);
    return {};
}

void Backend::pushHistory() {
    m_undo.append({m_center, m_width, m_inverted});
    if (m_undo.size() > 200)
        m_undo.removeFirst();
    m_redo.clear();
    emit historyChanged();
}

void Backend::applyWindow(const WindowState &state) {
    m_center = state.center;
    m_width = std::max(1.0, state.width);
    m_inverted = state.inverted;
    m_windowSet = true;
    emit windowChanged();
    rerender();
}

void Backend::beginWindowDrag() {
    pushHistory();
    m_dragStart = {m_center, m_width, m_inverted};
}

void Backend::dragWindow(double dx, double dy) {
    if (color())
        return;
    // Proportional, so the same drag suits a 0–255 ultrasound and a
    // 4000-unit CT alike: across sets contrast, down sets brightness.
    WindowState state = m_dragStart;
    state.width = std::max(1.0, m_dragStart.width * std::exp(dx * 0.005));
    state.center = m_dragStart.center + dy * m_dragStart.width * 0.0025;
    applyWindow(state);
}

void Backend::preset(int n) {
    if (n < 1 || n > int(std::size(Presets)) || color())
        return;
    pushHistory();
    applyWindow({Presets[n - 1].center, Presets[n - 1].width, m_inverted});
}

void Backend::fileWindow() {
    if (!m_windowSet)
        return;
    pushHistory();
    applyWindow({m_fileWindow.center, m_fileWindow.width, m_inverted});
}

void Backend::fullRange() {
    const Frame *frame = currentFrame();
    if (!frame || frame->isColor() || !frame->error.isEmpty())
        return;
    double low = 0, high = 0;
    frame->range(low, high);
    pushHistory();
    applyWindow({(low + high) / 2, std::max(1.0, high - low + 1), m_inverted});
}

void Backend::toggleInvert() {
    if (!hasImages())
        return;
    pushHistory();
    applyWindow({m_center, m_width, !m_inverted});
}

void Backend::undo() {
    const WindowState now{m_center, m_width, m_inverted};
    while (!m_undo.isEmpty() && m_undo.last() == now)
        m_undo.removeLast();
    if (m_undo.isEmpty()) {
        emit historyChanged();
        return;
    }
    m_redo.append(now);
    applyWindow(m_undo.takeLast());
    emit historyChanged();
}

void Backend::redo() {
    if (m_redo.isEmpty())
        return;
    m_undo.append({m_center, m_width, m_inverted});
    applyWindow(m_redo.takeLast());
    emit historyChanged();
}

void Backend::togglePlay() {
    if (m_play.isActive()) {
        stop();
        return;
    }
    const Series *s = currentSeries();
    if (!s || s->slices.size() < 2)
        return;
    const double fps = s->frameRate > 0 ? std::clamp(s->frameRate, 1.0, 120.0) : 12.0;
    m_play.start(qRound(1000 / fps));
    emit playingChanged();
}

void Backend::stop() {
    if (!m_play.isActive())
        return;
    m_play.stop();
    emit playingChanged();
}

QString Backend::valueAt(double x, double y) const {
    const Frame *frame = currentFrame();
    if (!frame || frame->isNull() || !frame->error.isEmpty())
        return {};
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    if (ix < 0 || iy < 0 || ix >= frame->width || iy >= frame->height)
        return {};
    const QString where = QStringLiteral("%1, %2").arg(QLocale().toString(ix), QLocale().toString(iy));
    if (frame->isColor()) {
        const QColor c = frame->color.pixelColor(ix, iy);
        return tr("%1   R %2  G %3  B %4").arg(where, QLocale().toString(c.red()), QLocale().toString(c.green()),
                                               QLocale().toString(c.blue()));
    }
    // The PET's value there too, when it's over the picture.
    QString pet;
    if (m_fused && m_fusionGrid) {
        double point[3], voxel[3], value;
        const ImagePlane plane = imagePlane();
        if (plane.valid) {
            plane.point(x - 0.5, y - 0.5, point);
            toVoxel(m_fusionGrid->volume, point, voxel);
            if (sampleGrid(*m_fusionGrid, voxel, value))
                pet = "   " + (m_scan.series.value(m_fusionSeries).suv > 0 ? tr("PET %1 SUV").arg(number(value, 2))
                                                                            : tr("PET %1").arg(number(value, 0)));
        }
    }
    return valueText(*frame, ix, iy, where) + pet;
}

QString Backend::valueText(const Frame &f, int ix, int iy, const QString &where) const {
    const Frame *frame = &f;
    const double v = frame->value(ix, iy);
    const bool whole = frame->slope == std::floor(frame->slope) && frame->intercept == std::floor(frame->intercept);
    const QString value = number(v, whole ? 0 : 2);
    const Series *s = currentSeries();
    if (s && s->suv > 0)
        return tr("%1   %2 SUV").arg(where, number(shownValue(v), 2));
    if (s && s->modality == "CT")
        return tr("%1   %2 HU").arg(where, value);
    return QStringLiteral("%1   %2").arg(where, value);
}

MeasureScale Backend::measureScale() const {
    MeasureScale scale;
    const Series *s = currentSeries();
    if (!s)
        return scale;
    scale.aspect = pixelAspect();
    if (m_plane == 3) {
        scale.even = true;
        scale.across = scale.down = m_oblique.spacing;
    } else if (m_plane != 0) {
        scale.even = true;
        scale.across = m_volume.spacing[m_cut.horizontal];
        scale.down = m_volume.spacing[m_cut.vertical];
    } else if (s->hasSpacing) {
        scale.even = true;
        scale.across = s->spacing[1];
        scale.down = s->spacing[0];
    } else {
        // Ultrasound calibrates the picture's areas, not the whole frame.
        scale.regions = s->regions;
    }
    return scale;
}

bool Backend::spacingAt(double x, double y, double &across, double &down) const {
    return currentSeries() && measureScale().at(x, y, across, down);
}

double Backend::shownValue(double value) const {
    const Series *s = currentSeries();
    return s && s->suv > 0 ? value * s->suv : value;
}

QString Backend::unit() const {
    const Series *s = currentSeries();
    if (!s)
        return {};
    if (s->suv > 0)
        return QStringLiteral("SUV");
    return s->modality == "CT" ? QStringLiteral("HU") : QString();
}

QString Backend::measureText(const Measure &m) const {
    const Series *s = currentSeries();
    if (!s)
        return {};
    const Frame *frame = m.kind == 2 ? currentFrame() : nullptr;
    if (m.kind == 2 && (!frame || frame->isNull() || !frame->error.isEmpty()))
        return {};
    const MeasureValues v = measureValues(m, measureScale(), frame, s->suv > 0 ? s->suv : 1);
    if (!v.valid)
        return {};
    if (m.kind == 3 || m.kind == 4)
        return m.kind == 4 ? tr("Cobb %1").arg(angleText(v.angle)) : angleText(v.angle);
    if (m.kind == 1) {
        if (!v.millimetres)
            return tr("%1 px").arg(number(v.length, 0));
        return v.length >= 100 ? tr("%1 cm").arg(number(v.length / 10, 1)) : tr("%1 mm").arg(number(v.length, 1));
    }
    const QString area = !v.millimetres ? tr("%1 px²").arg(number(v.area, 0))
                         : v.area >= 100 ? tr("%1 cm²").arg(number(v.area / 100, 2))
                                         : tr("%1 mm²").arg(number(v.area, 1));
    QStringList lines{tr("Area %1").arg(area)};
    if (v.count == 0)
        return lines.join('\n');
    const QString u = unit();
    const int decimals = u == "SUV" ? 2 : (u == "HU" || (frame->slope == 1 && frame->intercept == std::floor(frame->intercept))) ? 0 : 2;
    auto value = [&](double x, int extra = 0) { return u.isEmpty() ? number(x, decimals + extra) : number(x, decimals + extra) + " " + u; };
    lines << tr("Mean %1  ·  SD %2").arg(value(v.mean, decimals == 0 ? 1 : 0), number(v.sd, decimals == 0 ? 1 : decimals));
    lines << tr("Min %1  ·  Max %2").arg(value(v.low), value(v.high));
    return lines.join('\n');
}

QString Backend::distance(double x1, double y1, double x2, double y2) const {
    Measure m;
    m.kind = 1;
    m.a = QPointF(x1, y1);
    m.b = QPointF(x2, y2);
    return measureText(m);
}

QString Backend::areaStats(double x, double y, double width, double height) const {
    Measure m;
    m.kind = 2;
    m.a = QPointF(x, y);
    m.b = QPointF(x + width, y + height);
    return measureText(m);
}

QString Backend::angleText(double degrees) const {
    return tr("%1°").arg(number(degrees, 1));
}

void Backend::exportImage() {
    m_picture = QImage();
    const Series *s = currentSeries();
    if (!s || m_image.isNull() || !m_picker)
        return;
    QString name = s->description.isEmpty() ? s->modality : s->description;
    name.replace(QRegularExpression("[^\\w\\-]+"), "-");
    m_picker->choose(this, FilePicker::Purpose::SaveImage,
                     QStringLiteral("%1-%2.png").arg(name.isEmpty() ? QStringLiteral("scan") : name).arg(m_sliceIndex + 1));
}

void Backend::saveImage(const QString &path) {
    const QImage picture = std::exchange(m_picture, QImage());
    if (path.isEmpty() || (m_image.isNull() && picture.isNull()))
        return;
    // Written beside the target and renamed over it, so a failure never
    // leaves half a file.
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || ![&] {
            if (!picture.isNull())
                return picture.save(&file, "PNG");
            const QImage image = trueShape(m_image, pixelAspect());
            return image.transformed(orientation(image.size())).save(&file, "PNG");
        }() || !file.commit()) {
        setProblem(tr("The image couldn't be saved: %1").arg(file.errorString()));
        return;
    }
    flash(tr("Saved %1").arg(QFileInfo(path).fileName()));
}

void Backend::exportVideo() {
    const Series *s = currentSeries();
    if (!s || !m_picker)
        return;
    if (sliceCount() < 2) {
        flash(tr("A video needs more than one image"));
        return;
    }
    if (m_decodedCount < m_frames.size()) {
        flash(tr("The series is still being read"));
        return;
    }
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
        setProblem(tr("Saving a video needs ffmpeg"));
        return;
    }
    QString name = s->description.isEmpty() ? s->modality : s->description;
    if (m_plane != 0)
        name += "-" + QString::fromLatin1(::planeName(imagePlane()));
    name.replace(QRegularExpression("[^\\w\\-]+"), "-");
    m_picker->choose(this, FilePicker::Purpose::SaveVideo, (name.isEmpty() ? QStringLiteral("scan") : name) + ".mp4");
}

void Backend::saveVideo(const QString &path) {
    const Series *s = currentSeries();
    if (path.isEmpty() || !s)
        return;
    // Everything the pictures are made from is copied for the worker,
    // which renders them as shown and feeds them to ffmpeg.
    struct Video {
        QVector<Frame> frames;
        Volume volume;
        Cut cut;
        Oblique oblique;
        double low = 0, high = 0;
        bool built = false;
        int count = 0, half = 0, colorMap = 0, rotation = 0;
        Projection projection = Projection::Max;
        double center = 0, width = 0, aspect = 1, fps = 12;
        bool inverted = false, flipped = false;
        // A PET over it, with where each picture lies to put it there.
        Series series;
        int plane = 0;
        std::shared_ptr<const VoxelGrid> pet;
        Overlay3D petLook;
    } v;
    v.series = *s;
    v.plane = m_plane;
    v.pet = fusionGrid();
    v.petLook = fusionLook();
    v.frames = m_frames;
    v.volume = m_volume;
    v.cut = m_plane != 0 && m_plane != 3 ? m_cut : cutAcross(m_volume, 2);
    if (m_plane == 3) {
        v.oblique = m_oblique;
        v.low = m_low;
        v.high = m_high;
    }
    v.built = built();
    v.count = sliceCount();
    slabFor(m_plane == 3 ? m_oblique.step : m_volume.spacing[v.cut.fixed], v.half, v.projection);
    v.colorMap = m_colorMap;
    v.rotation = m_rotation;
    v.flipped = m_flipped;
    v.center = m_center;
    v.width = m_width;
    v.inverted = m_inverted;
    v.aspect = pixelAspect();
    v.fps = s->frameRate > 0 && m_plane == 0 ? std::clamp(s->frameRate, 1.0, 60.0) : 12.0;
    flash(tr("Saving the video…"));

    QPointer<Backend> self(this);
    auto locker = std::make_shared<QEventLoopLocker>();
    QThreadPool::globalInstance()->start([self, v, path, locker] {
        auto picture = [&v](int i) {
            const Frame f = !v.built ? v.frames.value(i)
                            : v.oblique.valid ? resample(v.volume, v.oblique, i, v.frames, v.low, v.high, v.half, v.projection)
                                              : reformat(v.volume, v.cut, i, v.frames, v.half, v.projection);
            if (f.isNull() || !f.error.isEmpty())
                return QImage();
            QImage image = render(f, v.center, v.width, v.inverted);
            if (v.colorMap != 0 && image.format() == QImage::Format_Grayscale8)
                image = colorize(image, v.colorMap);
            if (v.pet && !f.isColor()) {
                const ImagePlane where = v.plane == 3 ? v.oblique.plane(i)
                                         : v.plane != 0 ? cutPlane(v.volume, v.cut, i) : slicePlane(v.series, i);
                image = fuseSlice(image, where, *v.pet, v.petLook);
            }
            image = trueShape(image, v.aspect);
            return image.transformed(turn(image.size(), v.rotation, v.flipped)).convertToFormat(QImage::Format_RGB32);
        };
        QString error;
        const QImage first = picture(0);
        const QFileInfo target(path);
        const QString part = target.absolutePath() + "/." + target.fileName() + ".part";
        if (first.isNull()) {
            error = QStringLiteral("the first image can't be read");
        } else {
            QProcess ffmpeg;
            ffmpeg.setProcessChannelMode(QProcess::SeparateChannels);
            // Video wants even sides; the padding is black.
            ffmpeg.start("ffmpeg", {"-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgra", "-s",
                                    QStringLiteral("%1x%2").arg(first.width()).arg(first.height()), "-r",
                                    QString::number(v.fps), "-i", "-", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-c:v",
                                    "libx264", "-pix_fmt", "yuv420p", "-crf", "18", "-movflags", "+faststart", "-f",
                                    "mp4", part});
            if (!ffmpeg.waitForStarted()) {
                error = QStringLiteral("ffmpeg didn't start");
            } else {
                for (int i = 0; i < v.count; ++i) {
                    QImage image = i == 0 ? first : picture(i);
                    if (image.size() != first.size()) {
                        image = QImage(first.size(), QImage::Format_RGB32);
                        image.fill(Qt::black);
                    }
                    ffmpeg.write(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
                    ffmpeg.waitForBytesWritten(-1);
                }
                ffmpeg.closeWriteChannel();
                ffmpeg.waitForFinished(-1);
                if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0)
                    error = QString::fromUtf8(ffmpeg.readAllStandardError()).trimmed().section('\n', -1);
            }
        }
        // Written beside the target and renamed over it, so a failure never
        // leaves half a video.
        if (error.isEmpty() && std::rename(QFile::encodeName(part).constData(), QFile::encodeName(path).constData()) != 0)
            error = QString::fromLocal8Bit(std::strerror(errno));
        if (!error.isEmpty())
            QFile::remove(part);
        QMetaObject::invokeMethod(qApp, [self, error, path] {
            if (!self)
                return;
            if (error.isEmpty())
                self->flash(tr("Saved %1").arg(QFileInfo(path).fileName()));
            else
                self->setProblem(tr("The video couldn't be saved: %1").arg(error));
        }, Qt::QueuedConnection);
    });
}

void Backend::exportAnonymous() {
    if (!hasImages() || !m_picker)
        return;
    m_picker->choose(this, FilePicker::Purpose::SaveFolder);
}

void Backend::saveAnonymous(const QString &folder) {
    if (folder.isEmpty() || !hasImages())
        return;
    flash(tr("Saving an anonymous copy…"));
    QPointer<Backend> self(this);
    auto locker = std::make_shared<QEventLoopLocker>();
    const Scan scan = m_scan;
    QThreadPool::globalInstance()->start([self, scan, folder, locker] {
        const Anonymize::Result result = Anonymize::copyScan(scan, folder);
        QMetaObject::invokeMethod(qApp, [self, result] {
            if (!self)
                return;
            if (result.written == 0) {
                self->setProblem(tr("The anonymous copy couldn't be saved: %1").arg(result.errors.value(0)));
                return;
            }
            self->flash(tr("Saved %Ln files in %1", nullptr, result.written).arg(QFileInfo(result.folder).fileName()));
            // What the copy can't promise is said where it stays up.
            QStringList warnings;
            if (result.showDetails > 0)
                warnings << tr("%Ln of the images may show the patient's details in the picture itself, which a copy can't take out. Look at them before sharing.", nullptr, result.showDetails);
            if (result.skipped > 0)
                warnings << tr("Reports were left out, as their text names the patient.");
            if (!result.errors.isEmpty())
                warnings << tr("%Ln files couldn't be copied.", nullptr, result.errors.size());
            if (!warnings.isEmpty())
                self->setProblem(warnings.join(' '));
        }, Qt::QueuedConnection);
    });
}

void Backend::exportPicture(const QImage &picture) {
    const Series *s = currentSeries();
    if (!s || picture.isNull() || !m_picker)
        return;
    QString name = s->description.isEmpty() ? s->modality : s->description;
    name.replace(QRegularExpression("[^\\w\\-]+"), "-");
    m_picture = picture;
    m_picker->choose(this, FilePicker::Purpose::SaveImage, QStringLiteral("%1-3D.png").arg(name.isEmpty() ? QStringLiteral("scan") : name));
}

void Backend::copyPicture(const QImage &picture) {
    if (picture.isNull())
        return;
    QGuiApplication::clipboard()->setImage(picture);
    flash(tr("Copied the image"));
}

void Backend::copyImage() {
    if (m_image.isNull())
        return;
    const QImage image = trueShape(m_image, pixelAspect());
    QGuiApplication::clipboard()->setImage(image.transformed(orientation(image.size())));
    flash(tr("Copied the image"));
}

int Backend::fusionPartner(int index) const {
    return ::fusionPartner(m_scan.series, index);
}

QString Backend::fusionName() const {
    if (!m_fused)
        return {};
    const Series *s = currentSeries();
    const QString under = s ? s->modality : QString();
    if (!m_fusionGrid)
        return tr("PET over %1  ·  reading the PET…").arg(under);
    return tr("PET over %1  ·  %2%").arg(under, number(m_fusionOpacity * 100, 0));
}

Overlay3D Backend::fusionLook() const {
    Overlay3D look;
    look.grid = m_fusionGrid.get();
    look.low = m_fusionLow;
    look.high = m_fusionHigh;
    look.colorMap = 1;
    look.opacity = m_fusionOpacity;
    return look;
}

void Backend::toggleFusion() {
    if (m_fused) {
        dropFusion();
        rerender();
        flash(tr("PET off"));
        return;
    }
    const Series *s = currentSeries();
    if (!s)
        return;
    const int partner = fusionPartner(m_seriesIndex);
    if (partner < 0) {
        flash(s->modality == "PT" ? tr("There's no CT or MR taken with this PET to put it over")
                                  : tr("There's no PET taken with this series to put over it"));
        return;
    }
    // A PET goes over its CT, so the CT is shown.
    if (s->modality == "PT") {
        const int pet = m_seriesIndex;
        selectSeries(partner);
        startFusion(pet);
    } else {
        startFusion(partner);
    }
    flash(fusionName());
}

void Backend::nextFusionOpacity() {
    if (!m_fused) {
        toggleFusion();
        return;
    }
    m_fusionOpacity = m_fusionOpacity < 0.4 ? 0.5 : m_fusionOpacity < 0.6 ? 0.7 : m_fusionOpacity < 0.8 ? 1.0 : 0.3;
    rerender();
    emit fusionChanged();
    flash(fusionName());
}

void Backend::startFusion(int series) {
    dropFusion();
    if (series < 0 || series >= m_scan.series.size())
        return;
    m_fused = true;
    m_fusionSeries = series;
    const Series &pet = m_scan.series[series];
    m_fusionFrames = QVector<Frame>(pet.slices.size());
    m_fusionDecoded = 0;
    m_fusionJob = decode(pet, pet.slices.size() / 2, [this](int generation, int index, const Frame &frame) {
        fusionDecoded(generation, index, frame);
    });
    emit fusionChanged();
}

void Backend::fusionDecoded(int generation, int index, const Frame &frame) {
    if (!m_fusionJob || generation != m_fusionJob->generation || index < 0 || index >= m_fusionFrames.size())
        return;
    m_fusionFrames[index] = frame;
    if (++m_fusionDecoded < m_fusionFrames.size())
        return;
    m_fusionJob.reset();
    const Series &pet = m_scan.series[m_fusionSeries];
    auto grid = std::make_shared<VoxelGrid>(packVolume(makeVolume(pet), m_fusionFrames, pet.suv > 0 ? pet.suv : 1));
    m_fusionFrames.clear();
    if (!grid->valid) {
        dropFusion();
        rerender();
        flash(tr("The PET can't be read whole, so it can't be put over this"));
        return;
    }
    petRange(*grid, pet.suv > 0, m_fusionLow, m_fusionHigh);
    m_fusionGrid = grid;
    rerender();
    emit fusionChanged();
}

void Backend::dropFusion() {
    if (m_fusionJob)
        m_fusionJob->cancelled = true;
    m_fusionJob.reset();
    m_fusionFrames.clear();
    m_fusionGrid.reset();
    m_fusionSeries = -1;
    if (m_fused) {
        m_fused = false;
        emit fusionChanged();
    }
}

void Backend::setProblem(const QString &problem) {
    if (problem == m_problem)
        return;
    m_problem = problem;
    emit problemChanged();
}

void Backend::dismissProblem() { setProblem({}); }

void Backend::flash(const QString &notice) {
    m_notice = notice;
    emit noticeChanged();
    m_noticeTimer.start();
}

void Backend::updateInfo() {
    m_info.clear();
    const Series *s = currentSeries();
    if (!s)
        return;
    QStringList details;
    if (!s->patientId.isEmpty())
        details << s->patientId;
    if (!s->patientBirthDate.isEmpty())
        details << tr("Born %1").arg(localeDate(s->patientBirthDate));
    if (!s->patientAge.isEmpty())
        details << age(s->patientAge);
    if (!sex(s->patientSex).isEmpty())
        details << sex(s->patientSex);

    QString when = localeDate(s->studyDate);
    const QTime time = QTime::fromString(DicomFile::formatTime(s->studyTime), "hh:mm:ss");
    if (time.isValid())
        when += "  " + QLocale().toString(time, QLocale::ShortFormat);

    QString series = s->description;
    if (!s->number.isEmpty())
        series = series.isEmpty() ? tr("Series %1").arg(s->number) : tr("Series %1: %2").arg(s->number, series);

    QStringList geometry;
    const Slice &slice = s->slices.value(m_sliceIndex);
    QString size = QStringLiteral("%1 × %2").arg(QLocale().toString(s->columns), QLocale().toString(s->rows));
    if (m_plane != 0) {
        geometry << planeName();
        size = m_plane == 3 ? QStringLiteral("%1 × %2").arg(QLocale().toString(m_oblique.width), QLocale().toString(m_oblique.height))
                            : QStringLiteral("%1 × %2").arg(QLocale().toString(m_cut.width(m_volume)), QLocale().toString(m_cut.height(m_volume)));
    } else {
        if (slice.hasLocation)
            geometry << tr("Location %1 mm").arg(number(slice.location, 1));
        if (s->thickness > 0 && m_slab == 0)
            geometry << tr("Thickness %1 mm").arg(number(s->thickness, 1));
    }
    if (m_slab != 0)
        geometry << slabName();

    m_info = {
        {"patient", s->patientName},
        {"patientDetails", details.join("  ·  ")},
        {"study", s->studyDescription},
        {"date", when.trimmed()},
        {"institution", s->institution},
        {"series", series},
        {"modality", s->modality},
        {"geometry", geometry.join("  ·  ")},
        {"size", size},
    };
}

void Backend::setTagsWanted(bool wanted) {
    if (wanted == m_tagsWanted)
        return;
    m_tagsWanted = wanted;
    updateTags();
}

void Backend::setTagFilter(const QString &filter) {
    if (filter == m_tagFilter)
        return;
    m_tagFilter = filter;
    updateTags();
}

void Backend::updateTags() {
    m_tags.clear();
    const Series *s = currentSeries();
    if (!m_tagsWanted || !s || s->slices.isEmpty()) {
        emit tagsChanged();
        return;
    }
    const Slice &slice = s->slices.value(acquiredSlice());
    const DicomFile file = DicomFile::read(slice.path);
    const QString filter = m_tagFilter.trimmed();
    const int limit = 3000;

    std::function<void(const QList<DicomElement> &, int)> add = [&](const QList<DicomElement> &elements, int depth) {
        for (const auto &e : elements) {
            if (m_tags.size() >= limit)
                return;
            const QString tag = tagText(e.tag), name = Dictionary::name(e.tag), value = file.display(e);
            const bool matches = filter.isEmpty() || tag.contains(filter, Qt::CaseInsensitive) ||
                                 name.contains(filter, Qt::CaseInsensitive) || value.contains(filter, Qt::CaseInsensitive);
            if (matches)
                m_tags << QVariantMap{{"depth", depth}, {"tag", tag}, {"name", name}, {"vr", QString::fromLatin1(e.vr)},
                                      {"value", value}, {"item", false}};
            // Per-frame groups hold one item a frame; only the shown
            // frame's is worth reading.
            const bool perFrame = e.tag == Tag::PerFrameGroups;
            for (int i = 0; i < e.items.size(); ++i) {
                if (perFrame && i != slice.frame)
                    continue;
                if (filter.isEmpty())
                    m_tags << QVariantMap{{"depth", depth + 1}, {"tag", QString()},
                                          {"name", perFrame ? tr("Frame %1").arg(i + 1) : tr("Item %1").arg(i + 1)},
                                          {"vr", QString()}, {"value", QString()}, {"item", true}};
                add(e.items[i], depth + 2);
            }
        }
    };
    add(file.elements(), 0);
    emit tagsChanged();
}

void Backend::retranslate() {
    updateInfo();
    emit reportChanged();
    emit reportsChanged();
    emit orientationChanged();
    emit seriesChanged();
    emit sliceChanged();
    emit windowChanged();
    emit imageChanged();
    emit fusionChanged();
    updateTags();
}

bool Backend::waitForSlice(int milliseconds) {
    QDeadlineTimer deadline(milliseconds);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!m_scanning && (frameReady() || !frameError().isEmpty()))
            return true;
        QThread::msleep(2);
    }
    return false;
}
