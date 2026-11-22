#include "library.h"

#include <QDate>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <cstdlib>

#include "archive.h"
#include "picture.h"

Library::Library(QObject *parent) : QObject(parent), m_grids(&m_cache) {
    m_scanPool.setMaxThreadCount(1);
    m_thumbnailPool.setMaxThreadCount(2);
}

Library::~Library() {
    if (m_scanCancel)
        *m_scanCancel = true;
    if (m_thumbnailCancel)
        *m_thumbnailCancel = true;
    m_scanPool.clear();
    m_thumbnailPool.clear();
    m_scanPool.waitForDone();
    m_thumbnailPool.waitForDone();
}

QImage Library::thumbnail(int series) const {
    QMutexLocker lock(&m_thumbnailMutex);
    return m_thumbnails.value(series);
}

int Library::companion(int series) const {
    const QList<Series> &all = m_scan.series;
    if (all.isEmpty())
        return -1;
    int pick = (series + 1) % all.size();
    if (series < 0 || series >= all.size())
        return pick;
    const Series &shown = all[series];
    qint64 nearest = -1;
    const QDate when = QDate::fromString(shown.studyDate, "yyyyMMdd");
    for (int i = 0; i < all.size(); ++i) {
        const Series &s = all[i];
        if (s.studyUid == shown.studyUid || s.modality != shown.modality || s.slices.size() < 2)
            continue;
        const QDate date = QDate::fromString(s.studyDate, "yyyyMMdd");
        const qint64 apart = when.isValid() && date.isValid() ? std::abs(date.daysTo(when)) : 0;
        if (nearest < 0 || apart < nearest) {
            nearest = apart;
            pick = i;
        }
    }
    return pick;
}

QList<Mark> Library::marksOn(const Slice &slice) const {
    QList<Mark> out;
    if (slice.sop.isEmpty())
        return out;
    for (const auto &m : m_scan.marks)
        if (m.appliesTo(slice.sop, slice.frame))
            out << m;
    return out;
}

void Library::open(const QStringList &paths, QObject *asker) {
    QStringList existing;
    for (const auto &path : paths)
        if (QFileInfo::exists(path))
            existing << QFileInfo(path).absoluteFilePath();
    if (existing.isEmpty()) {
        if (!paths.isEmpty())
            emit problem(asker, [path = paths.first()] { return tr("%1 doesn't exist").arg(path); });
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
    const int generation = ++m_generation;
    m_scanning = true;
    m_scanDone = 0;
    m_scanTotal = 0;
    emit scanningChanged();
    emit scanProgress();
    emit problem(asker, {});

    // Whoever asked may be gone by the time the scan is read.
    const QPointer<QObject> who(asker);
    m_scanPool.start([this, roots, recursive, focus, cancel, generation, who]() mutable {
        // A zip is unpacked and read as the folder it holds.
        for (QString &root : roots)
            if (QFileInfo(root).isFile() && Archive::isZip(root)) {
                QString error;
                const QString dir = Archive::unpack(root, &error);
                if (dir.isEmpty()) {
                    QMetaObject::invokeMethod(this, [this, error, generation, who] {
                        if (generation != m_generation)
                            return;
                        m_scanning = false;
                        emit scanningChanged();
                        emit problem(who, [error] { return tr(error.toUtf8().constData()); });
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
                if (generation != m_generation)
                    return;
                m_scanDone = done;
                m_scanTotal = total;
                emit scanProgress();
            }, Qt::QueuedConnection);
        }, cancel.get());
        if (*cancel)
            return;
        QMetaObject::invokeMethod(this, [this, scan, focus, generation, who] { finished(generation, scan, focus, who); },
                                  Qt::QueuedConnection);
    });
}

void Library::finished(int generation, const Scan &scan, const QString &focusPath, QObject *asker) {
    if (generation != m_generation)
        return;
    m_scanning = false;
    emit scanningChanged();

    if (scan.series.isEmpty()) {
        // What was open stays open.
        if (scan.unreadable.isEmpty())
            emit problem(asker, [] { return tr("No DICOM images were found there"); });
        else
            emit problem(asker, [first = scan.unreadable.first()] {
                return tr("The DICOM files there couldn't be read: %1").arg(first);
            });
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

    // Everything kept was of the scan before.
    m_scan = scan;
    ++m_serial;
    m_grids.reset(scan.series);
    m_cache.reset(scan.series);
    {
        QMutexLocker lock(&m_thumbnailMutex);
        m_thumbnails.clear();
    }
    m_seriesModel.reset(scan.series);
    emit opened(asker, series, slice);
    if (!scan.unreadable.isEmpty())
        emit problem(asker, [count = int(scan.unreadable.size()), first = scan.unreadable.first()] {
            return tr("%Ln files couldn't be read", nullptr, count) + " — " + first;
        });
    makeThumbnails();
}

void Library::makeThumbnails() {
    // A thumbnail a series, from its middle slice.
    if (m_thumbnailCancel)
        *m_thumbnailCancel = true;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_thumbnailCancel = cancel;
    for (int s = 0; s < m_scan.series.size(); ++s) {
        const Series copy = m_scan.series[s];
        m_thumbnailPool.start([this, copy, s, serial = m_serial, cancel] {
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
            QMetaObject::invokeMethod(this, [this, serial, s, image] {
                if (serial != m_serial)
                    return;
                {
                    QMutexLocker lock(&m_thumbnailMutex);
                    m_thumbnails.insert(s, image);
                }
                m_seriesModel.thumbnailArrived(s);
            }, Qt::QueuedConnection);
        });
    }
}
