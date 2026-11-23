#include "backend.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace {

// Megabytes, rounded up, for saying how much memory something takes.
int megabytes(qint64 bytes) { return int((bytes + (1 << 20) - 1) >> 20); }

}

Backend::Backend(FilePicker *picker, std::shared_ptr<Library> library, QObject *parent)
    : QObject(parent), m_picker(picker), m_exporter(picker), m_anonymous(picker) {
    connect(&m_notices, &Notices::problemChanged, this, &Backend::problemChanged);
    connect(&m_notices, &Notices::noticeChanged, this, &Backend::noticeChanged);
    connect(&m_claim, &Claim::changed, this, &Backend::decodedChanged);
    connect(&m_window, &Windowing::changed, this, [this] {
        emit windowChanged();
        repaint();
    });
    connect(&m_window, &Windowing::historyChanged, this, &Backend::historyChanged);
    connect(&m_fusion, &Fusion::changed, this, [this] {
        repaint();
        emit fusionChanged();
    });
    connect(&m_fusion, &Fusion::failed, this, [this](PetGrids::State why) {
        if (why == PetGrids::State::Unreadable) {
            m_notices.flash([] { return tr("The PET can't be read whole, so it can't be put over this"); });
            return;
        }
        // Its slices and the voxels packed from them.
        const int pet = ::fusionPartner(scan().series, m_seriesIndex);
        const qint64 bytes = m_library->cache()->wholeBytes(pet) + m_library->cache()->volumeBytes(pet);
        m_notices.setProblem([needed = megabytes(bytes), budget = megabytes(m_library->cache()->budget())] {
            return tr("The PET is too big to put over this: it takes %1 MB of memory, and Dicolens keeps to %2 MB")
                .arg(needed).arg(budget);
        });
    });
    connect(&m_reports, &ReportReader::changed, this, &Backend::reportChanged);
    connect(&m_reports, &ReportReader::problem, &m_notices, &Notices::setProblem);
    connect(&m_exporter, &Exporter::said, &m_notices, &Notices::flash);
    connect(&m_exporter, &Exporter::failed, &m_notices, &Notices::setProblem);
    connect(&m_anonymous, &AnonymousCopy::said, &m_notices, &Notices::flash);
    connect(&m_anonymous, &AnonymousCopy::failed, &m_notices, &Notices::setProblem);
    // Whether a region can be marked goes by the series and how it's cut.
    connect(this, &Backend::sliceChanged, this, &Backend::blackoutChanged);
    connect(this, &Backend::orientationChanged, this, &Backend::blackoutChanged);
    connect(&m_play, &QTimer::timeout, this, [this] {
        // Waits for the next slice rather than showing a gap.
        const int next = (sliceIndex() + 1) % std::max(1, sliceCount());
        if (built() || m_library->cache()->has(m_seriesIndex, next))
            setSliceIndex(next);
    });
    if (m_picker) {
        // The panes share the picker; each takes only the answers to what
        // it asked.
        connect(m_picker, &FilePicker::selected, this, [this](QObject *asker, FilePicker::Purpose, const QList<QUrl> &urls) {
            QVariantList list;
            for (const auto &url : urls)
                list << url;
            if (asker == this && !list.isEmpty())
                load(list);
        });
        connect(m_picker, &FilePicker::failed, this, [this](QObject *asker, const QString &message) {
            if (asker == this)
                m_notices.setProblem([message] { return message; });
        });
    }
    join(library ? library : std::make_shared<Library>());
    sync();
}

Backend::~Backend() = default;

void Backend::join(const std::shared_ptr<Library> &library) {
    if (library == m_library)
        return;
    if (m_library) {
        disconnect(m_library.get(), nullptr, this, nullptr);
        disconnect(m_library->cache(), nullptr, this, nullptr);
        disconnect(m_library->blackouts(), nullptr, this, nullptr);
        // Let go of once this call is over: joining another library can
        // be asked for from inside a signal of the one being left.
        QTimer::singleShot(0, qApp, [old = m_library] {});
    }
    m_library = library;
    m_serial = -1;
    m_claim.setCache(m_library->cache());
    m_fusion.setGrids(m_library->grids());
    m_anonymous.setLibrary(m_library.get());
    connect(m_library.get(), &Library::scanningChanged, this, &Backend::scanningChanged);
    connect(m_library.get(), &Library::scanProgress, this, &Backend::scanProgress);
    connect(m_library.get(), &Library::opened, this, &Backend::scanOpened);
    connect(m_library.get(), &Library::problem, this, [this](QObject *asker, const Words &words) {
        if (asker == this)
            m_notices.setProblem(words);
    });
    connect(m_library->cache(), &FrameCache::arrived, this, &Backend::frameArrived);
    connect(m_library->cache(), &FrameCache::evicted, this, [this](int series) {
        if (series == m_seriesIndex)
            emit decodedChanged();
    });
    connect(m_library->blackouts(), &Blackouts::changed, this, &Backend::blackoutChanged);
    emit scanningChanged();
    emit scanProgress();
}

void Backend::sync() {
    if (m_serial == m_library->serial())
        return;
    m_serial = m_library->serial();
    stop();
    m_fusion.stop();
    m_window.forget();
    m_seriesIndex = -1;
    m_planes.setSeries(Series());
    m_built = Frame();
    m_buildError.clear();
    m_shown = Frame();
    m_info.clear();
    m_reports.setReports(scan().reports);
    m_reports.setStudy({}, false);
    updateInterest();
    updateTags();
    emit seriesChanged();
    emit reportsChanged();
    emit sliceChanged();
    emit decodedChanged();
    emit orientationChanged();
    repaint();
}

void Backend::scanOpened(QObject *asker, int series, int slice) {
    sync();
    // The pane that opened it shows the slice that was opened; the others
    // show nothing of it until they're given a series.
    if (asker == this) {
        selectSeries(series);
        setSliceIndex(slice);
    }
    emit opened();
}

void Backend::open() {
    if (m_picker)
        m_picker->choose(this, FilePicker::Purpose::OpenScan);
}

void Backend::openFolder() {
    if (m_picker)
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
    m_library->open(paths, this);
}

void Backend::share(Backend *other) {
    if (!other || other == this || !other->hasImages())
        return;
    join(other->m_library);
    sync();
    // Already showing a series of that scan: what it shows stays, and is
    // kept in memory again if it was put away.
    if (m_seriesIndex >= 0) {
        updateInterest();
        build();
        showSlice();
        return;
    }
    selectSeries(m_library->companion(other->m_seriesIndex));
    setSliceIndex(0);
    emit opened();
}

void Backend::mirror(Backend *leader, int plane) {
    for (const auto &c : std::as_const(m_leaderConnections))
        disconnect(c);
    m_leaderConnections.clear();
    m_leader = leader == this ? nullptr : leader;
    if (!m_leader) {
        rest();
        return;
    }
    join(leader->m_library);
    sync();
    m_leaderConnections << connect(leader, &Backend::fusionChanged, this, &Backend::adoptFusion);
    m_leaderConnections << connect(leader, &QObject::destroyed, this, [this] { mirror(nullptr, 0); });
    // The slices are the leader's own, in the library: none is read again.
    selectSeries(leader->m_seriesIndex);
    matchWindow(leader);
    if (plane != m_planes.plane()) {
        // A tilted plane goes back to the volume's own first.
        const bool tilted = m_planes.plane() == 3;
        if (tilted)
            m_planes.straighten();
        if (tilted && plane == 0)
            cutChanged();
        else
            setPlane(plane);
    }
    // The planes meet where the leader's do.
    m_planes.meetWhere(leader->planes());
    sliceMoved();
    adoptFusion();
}

void Backend::rest() {
    stop();
    m_claim.show(-1, -1, false);
}

void Backend::adoptFusion() {
    if (!m_leader)
        return;
    if (m_leader->fused() && m_leader->m_seriesIndex == m_seriesIndex)
        m_fusion.follow(m_leader->fusion());
    else
        m_fusion.stop();
}

void Backend::matchWindow(Backend *other) {
    if (other && other != this && other->m_seriesIndex == m_seriesIndex)
        m_window.match(other->window());
}

void Backend::follow(Backend *other) {
    if (!other || sliceCount() == 0 || other->sliceCount() == 0)
        return;
    // The picture at the same place in the patient, when both cut the body
    // the same way; otherwise as far through.
    const ImagePlane there = other->imagePlane(), here = imagePlane();
    const bool parallel = m_planes.parallelTo(there);
    // Planes that cross in one body stay put: the reference line shows
    // where the other is.
    if (!parallel && here.valid && there.valid && !here.frame.isEmpty() && here.frame == there.frame)
        return;
    int best = parallel ? m_planes.indexAt(there) : -1;
    if (best < 0) {
        const double fraction = other->sliceCount() > 1 ? double(other->sliceIndex()) / (other->sliceCount() - 1) : 0;
        best = int(std::lround(fraction * (sliceCount() - 1)));
    }
    setSliceIndex(best);
}

const Series *Backend::currentSeries() const {
    return m_seriesIndex >= 0 && m_seriesIndex < scan().series.size() ? &scan().series[m_seriesIndex] : nullptr;
}

const Frame *Backend::currentFrame() const {
    if (built())
        return m_built.isNull() && m_buildError.isEmpty() ? nullptr : &m_built;
    return m_library->cache()->frame(m_seriesIndex, sliceIndex());
}

const QVector<Frame> &Backend::frames() const { return m_library->cache()->frames(m_seriesIndex); }

bool Backend::allDecoded() const { return m_library->cache()->complete(m_seriesIndex); }

int Backend::decodedCount() const { return m_library->cache()->count(m_seriesIndex); }

int Backend::sliceCount() const { return currentSeries() ? m_planes.count() : 0; }

double Backend::pixelAspect() const { return currentSeries() ? m_planes.pixelAspect() : 1.0; }

ImagePlane Backend::imagePlane() const { return currentSeries() ? m_planes.imagePlane() : ImagePlane(); }

QStringList Backend::edges() const { return currentSeries() ? m_turn.edges(m_planes.edges()) : QStringList(); }

void Backend::updateInterest() {
    if (m_claim.show(m_seriesIndex, m_planes.acquired(), built()))
        return;
    // The room this pane's plane or slab had is gone: it goes back to the
    // slices as scanned, which need only the one shown, and says why.
    m_notices.setProblem(tooBig(m_claim.bytes(false)));
    m_planes.straighten();
    m_planes.thin();
    m_claim.show(m_seriesIndex, m_planes.acquired(), false);
    m_built = Frame();
    m_buildError.clear();
    m_shown = Frame();
    updateInfo();
    emit sliceChanged();
    emit orientationChanged();
    showSlice();
}

Words Backend::tooBig(qint64 bytes) const {
    return [needed = megabytes(bytes), budget = megabytes(m_library->cache()->budget())] {
        return tr("This series is too big to cut another way or show in 3D: that takes %1 MB of memory, and Dicolens keeps to %2 MB")
            .arg(needed).arg(budget);
    };
}

bool Backend::volumeFits() {
    if (m_seriesIndex < 0)
        return false;
    if (m_claim.fits(m_seriesIndex, m_planes.acquired()))
        return true;
    m_notices.setProblem(tooBig(m_claim.bytes(false)));
    return false;
}

QString Backend::volumeRefusal() const {
    return m_claim.volumeRefused() ? tooBig(m_claim.bytes(true))() : QString();
}

void Backend::build() {
    m_built = Frame();
    m_buildError.clear();
    // Every slice goes into the picture, so it waits for them all.
    if (!built() || !canReformat() || !allDecoded())
        return;
    m_built = m_planes.recipe(frames()).make(sliceIndex(), frames());
    if (m_built.isNull())
        m_buildError = QT_TRANSLATE_NOOP("dicolens", "Some slices can't be read, so this picture can't be made");
}

void Backend::frameArrived(int series, int index) {
    if (series != m_seriesIndex)
        return;
    emit decodedChanged();
    if (built()) {
        if (allDecoded() && m_built.isNull() && m_buildError.isEmpty()) {
            build();
            showSlice();
        }
    } else if (index == sliceIndex()) {
        showSlice();
    }
}

void Backend::selectSeries(int index) {
    sync();
    if (index < 0 || index >= scan().series.size() || index == m_seriesIndex)
        return;
    stop();
    m_fusion.stop();
    const int from = m_seriesIndex;
    const Series &chosen = scan().series[index];
    m_seriesIndex = index;
    m_shown = Frame();
    // A turn suits one series; the next is shown as it was taken.
    m_turn = Turn();
    m_planes.setSeries(chosen);
    m_built = Frame();
    m_buildError.clear();
    // PET starts in its colours; the rest in grey.
    m_window.enter(from, index, chosen.modality == "PT");
    updateInterest();
    updateInfo();
    m_reports.setStudy(chosen.studyUid, true);
    emit sliceChanged();
    emit orientationChanged();
    emit decodedChanged();
    // A series another pane has read already is there to show at once.
    showSlice();
    updateTags();
}

void Backend::stepSeries(int delta) {
    if (hasImages())
        selectSeries(std::clamp(m_seriesIndex + delta, 0, int(scan().series.size()) - 1));
}

void Backend::setSliceIndex(int index) {
    const int count = sliceCount();
    if (count == 0)
        return;
    index = std::clamp(index, 0, count - 1);
    if (index == sliceIndex() && !m_shown.isNull())
        return;
    m_planes.setIndex(index);
    sliceMoved();
}

void Backend::step(int delta) {
    if (sliceCount() > 0)
        setSliceIndex(sliceIndex() + delta);
}

void Backend::sliceMoved() {
    updateInterest();
    build();
    updateInfo();
    emit sliceChanged();
    showSlice();
    updateTags();
}

void Backend::cutChanged() {
    // The picture before was another shape; it doesn't stay up.
    m_shown = Frame();
    updateInterest();
    build();
    updateInfo();
    emit sliceChanged();
    emit orientationChanged();
    showSlice();
}

void Backend::showSlice() {
    const Frame *frame = currentFrame();
    if (frame && frame->error.isEmpty() && !frame->isNull()) {
        m_window.seen(m_seriesIndex, frame->windowCenter, frame->windowWidth);
        m_shown = *frame;
        m_shownPlane = imagePlane();
        if (!built())
            m_library->cache()->touch(m_seriesIndex, sliceIndex());
    } else if (frame) {
        m_shown = Frame();
    }
    // Until the slice arrives, the last picture stays up rather than a gap.
    repaint();
}

void Backend::repaint() {
    m_stale = true;
    emit imageChanged();
}

Picture Backend::picture() const {
    Picture p;
    p.frame = m_shown;
    p.center = m_window.center();
    p.width = m_window.width();
    p.inverted = m_window.inverted();
    p.colorMap = m_window.colorMap();
    p.pet = m_fusion.grid();
    p.petLook = m_fusion.look();
    p.plane = m_shownPlane;
    return p;
}

QImage Backend::image() const {
    if (m_stale) {
        m_image = picture().render();
        m_stale = false;
        ++m_renders;
    }
    return m_image;
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

void Backend::updateInfo() {
    const Series *s = currentSeries();
    m_info = s ? Readout::info(*s, m_planes, planeName(), slabName()) : QVariantMap();
}

void Backend::setTagsWanted(bool wanted) {
    if (wanted == m_tags.wanted())
        return;
    m_tags.setWanted(wanted);
    emit tagsChanged();
}

void Backend::setTagFilter(const QString &filter) {
    if (filter == m_tags.filter())
        return;
    m_tags.setFilter(filter);
    emit tagsChanged();
}

void Backend::updateTags() {
    // The file behind what's shown: the slice, or the one through where
    // the planes meet.
    const Series *s = currentSeries();
    const Slice slice = s ? s->slices.value(m_planes.acquired()) : Slice();
    m_tags.show(slice.path, slice.frame);
}

void Backend::retranslate() {
    updateInfo();
    m_notices.retranslate();
    m_reports.retranslate();
    m_tags.retranslate();
    m_anonymous.retranslate();
    m_library->retranslate();
    emit orientationChanged();
    emit sliceChanged();
    emit windowChanged();
    emit imageChanged();
    emit fusionChanged();
}

bool Backend::waitForSlice(int milliseconds) {
    QDeadlineTimer deadline(milliseconds);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!scanning() && (frameReady() || !frameError().isEmpty()))
            return true;
        QThread::msleep(2);
    }
    return false;
}
