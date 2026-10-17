#include "backend.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDate>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTime>
#include <algorithm>
#include <cmath>

// What one series' decoding shares between the pool's threads: which
// slices are taken, which one the viewer wants next, and the multi-frame
// files already parsed, so a cine loop is read from disk once.
struct DecodeJob {
    int generation = 0;
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

QString tr(const char *text, int n = -1) { return QCoreApplication::translate("scans", text, nullptr, n); }

struct Preset {
    const char *name;
    double center;
    double width;
};

// The CT windows radiologists reach for, in Hounsfield units.
const Preset Presets[] = {
    {QT_TRANSLATE_NOOP("scans", "Brain"), 40, 80},
    {QT_TRANSLATE_NOOP("scans", "Soft tissue"), 40, 400},
    {QT_TRANSLATE_NOOP("scans", "Lung"), -600, 1500},
    {QT_TRANSLATE_NOOP("scans", "Bone"), 400, 1800},
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
    case 'Y': return tr("%n years old", n);
    case 'M': return tr("%n months old", n);
    case 'W': return tr("%n weeks old", n);
    case 'D': return tr("%n days old", n);
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
        if (next < m_decoded.size() && m_decoded[next])
            setSliceIndex(next);
    });
    if (m_picker) {
        connect(m_picker, &FilePicker::selected, this, [this](FilePicker::Purpose purpose, const QList<QUrl> &urls) {
            if (purpose == FilePicker::Purpose::SaveImage) {
                saveImage(urls.first().toLocalFile());
                return;
            }
            QVariantList list;
            for (const auto &url : urls)
                list << url;
            load(list);
        });
        connect(m_picker, &FilePicker::failed, this, &Backend::setProblem);
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
    if (m_picker)
        m_picker->choose(FilePicker::Purpose::OpenScan);
}

void Backend::openFolder() {
    if (m_picker)
        m_picker->choose(FilePicker::Purpose::OpenFolder);
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
    if (existing.size() == 1 && QFileInfo(existing.first()).isFile()) {
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

    m_pool.start([this, roots, recursive, focus, cancel, generation] {
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

    stop();
    if (m_job)
        m_job->cancelled = true;
    m_job.reset();
    m_scan = scan;
    m_seriesWindows.clear();
    {
        QMutexLocker lock(&m_thumbnailMutex);
        m_thumbnails.clear();
    }
    m_seriesIndex = -1;

    int series = 0, slice = 0;
    for (int s = 0; s < m_scan.series.size() && !focusPath.isEmpty(); ++s)
        for (int i = 0; i < m_scan.series[s].slices.size(); ++i)
            if (m_scan.series[s].slices[i].path == focusPath) {
                series = s;
                slice = i;
                s = m_scan.series.size();
                break;
            }
    emit seriesChanged();
    if (!scan.unreadable.isEmpty())
        setProblem(tr("%n files couldn't be read", nullptr, int(scan.unreadable.size())) + " — " + scan.unreadable.first());
    selectSeries(series);
    setSliceIndex(slice);
    emit opened();

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
    if (m_sliceIndex < 0 || m_sliceIndex >= m_frames.size() || !m_decoded.value(m_sliceIndex))
        return nullptr;
    return &m_frames[m_sliceIndex];
}

int Backend::sliceCount() const {
    const Series *s = currentSeries();
    return s ? s->slices.size() : 0;
}

bool Backend::frameReady() const {
    const Frame *f = currentFrame();
    return f && f->error.isEmpty() && !f->isNull();
}

QString Backend::frameError() const {
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
    return s ? s->pixelAspect() : 1.0;
}

QStringList Backend::edges() const {
    const Series *s = currentSeries();
    return s ? s->edgeLabels() : QStringList();
}

void Backend::selectSeries(int index) {
    if (index < 0 || index >= m_scan.series.size() || index == m_seriesIndex)
        return;
    stop();
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
        emit windowChanged();
    }
    m_image = QImage();
    startDecoding();
    updateInfo();
    emit sliceChanged();
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
    if (!series)
        return;

    auto job = std::make_shared<DecodeJob>();
    job->generation = ++m_jobGeneration;
    job->slices = series->slices;
    job->claimed = QVector<char>(series->slices.size(), 0);
    job->wanted = m_sliceIndex;
    job->video = isVideo(series->transferSyntax);
    m_job = job;

    auto post = [this, job](int index, const Frame &frame) {
        QMetaObject::invokeMethod(this, [this, job, index, frame] { frameDecoded(job->generation, index, frame); },
                                  Qt::QueuedConnection);
    };

    if (job->video) {
        // A video is one stream; ffmpeg decodes it whole.
        m_pool.start([job, post] {
            QString error;
            const DicomFile file = DicomFile::read(job->slices.first().path, &error);
            const QList<Frame> frames = decodeVideo(file);
            for (int i = 0; i < job->slices.size() && !job->cancelled; ++i)
                post(i, frames.value(std::min<int>(job->slices[i].frame, frames.size() - 1)));
        });
        return;
    }

    const int workers = std::min<int>(m_pool.maxThreadCount(), job->slices.size());
    for (int w = 0; w < workers; ++w)
        m_pool.start([job, post] {
            while (!job->cancelled) {
                const int index = job->claim();
                if (index < 0)
                    return;
                const Slice slice = job->slices[index];
                QString error;
                std::shared_ptr<QList<QByteArray>> split;
                const auto file = job->open(slice.path, split, &error);
                Frame frame;
                if (!file->isValid()) {
                    frame.error = "This file can't be read";
                    frame.detail = error;
                } else {
                    frame = decodeFrame(*file, slice.frame, split.get());
                }
                if (!job->cancelled)
                    post(index, frame);
            }
        });
}

void Backend::frameDecoded(int generation, int index, const Frame &frame) {
    if (!m_job || generation != m_job->generation || index < 0 || index >= m_frames.size())
        return;
    m_frames[index] = frame;
    if (!m_decoded[index]) {
        m_decoded[index] = true;
        ++m_decodedCount;
        emit decodedChanged();
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
    if (m_job)
        m_job->wanted = index;
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
    if (const Frame *frame = currentFrame(); frame && frame->error.isEmpty())
        m_image = render(*frame, m_center, m_width, m_inverted);
    emit imageChanged();
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
    const double v = frame->value(ix, iy);
    const bool whole = frame->slope == std::floor(frame->slope) && frame->intercept == std::floor(frame->intercept);
    const QString value = number(v, whole ? 0 : 2);
    const Series *s = currentSeries();
    if (s && s->modality == "CT")
        return tr("%1   %2 HU").arg(where, value);
    return QStringLiteral("%1   %2").arg(where, value);
}

QString Backend::distance(double x1, double y1, double x2, double y2) const {
    const Series *s = currentSeries();
    if (!s)
        return {};
    if (s->hasSpacing) {
        const double dx = (x2 - x1) * s->spacing[1], dy = (y2 - y1) * s->spacing[0];
        const double mm = std::hypot(dx, dy);
        return mm >= 100 ? tr("%1 cm").arg(number(mm / 10, 1)) : tr("%1 mm").arg(number(mm, 1));
    }
    return tr("%1 px").arg(number(std::hypot(x2 - x1, y2 - y1), 0));
}

void Backend::exportImage() {
    const Series *s = currentSeries();
    if (!s || m_image.isNull() || !m_picker)
        return;
    QString name = s->description.isEmpty() ? s->modality : s->description;
    name.replace(QRegularExpression("[^\\w\\-]+"), "-");
    m_picker->choose(FilePicker::Purpose::SaveImage,
                     QStringLiteral("%1-%2.png").arg(name.isEmpty() ? QStringLiteral("scan") : name).arg(m_sliceIndex + 1));
}

void Backend::saveImage(const QString &path) {
    if (path.isEmpty() || m_image.isNull())
        return;
    // Written beside the target and renamed over it, so a failure never
    // leaves half a file.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !trueShape(m_image, pixelAspect()).save(&file, "PNG") || !file.commit()) {
        setProblem(tr("The image couldn't be saved: %1").arg(file.errorString()));
        return;
    }
    flash(tr("Saved %1").arg(QFileInfo(path).fileName()));
}

void Backend::copyImage() {
    if (m_image.isNull())
        return;
    QGuiApplication::clipboard()->setImage(trueShape(m_image, pixelAspect()));
    flash(tr("Copied the image"));
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
    if (slice.hasLocation)
        geometry << tr("Location %1 mm").arg(number(slice.location, 1));
    if (s->thickness > 0)
        geometry << tr("Thickness %1 mm").arg(number(s->thickness, 1));

    m_info = {
        {"patient", s->patientName},
        {"patientDetails", details.join("  ·  ")},
        {"study", s->studyDescription},
        {"date", when.trimmed()},
        {"institution", s->institution},
        {"series", series},
        {"modality", s->modality},
        {"geometry", geometry.join("  ·  ")},
        {"size", QStringLiteral("%1 × %2").arg(QLocale().toString(s->columns), QLocale().toString(s->rows))},
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
    const Slice &slice = s->slices.value(m_sliceIndex);
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
            const bool perFrame = e.tag == 0x52009230;
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
    emit seriesChanged();
    emit sliceChanged();
    emit windowChanged();
    emit imageChanged();
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
