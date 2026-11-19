#include "volumeview.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QPainter>
#include <QQuickWindow>
#include <QThread>
#include <cmath>

namespace {

constexpr double Degree = M_PI / 180;

} // namespace

VolumeView::VolumeView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::NoButton);
    setFillColor(Qt::black);
    m_pool.setMaxThreadCount(1);
    // Once it stops moving, a fine picture.
    m_settle.setSingleShot(true);
    m_settle.setInterval(160);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        if (spinning())
            return;
        m_quickWanted = false;
        m_dirty = true;
        if (!m_rendering)
            startRender();
    });
    m_spin.setInterval(40);
    connect(&m_spin, &QTimer::timeout, this, [this] {
        m_view.turn(2 * Degree, 0);
        emit viewChanged();
        request(true);
    });
}

VolumeView::~VolumeView() {
    if (m_cancel)
        *m_cancel = true;
    m_pool.waitForDone();
}

void VolumeView::setBackend(Backend *backend) {
    if (backend == m_backend)
        return;
    if (m_backend)
        disconnect(m_backend, nullptr, this, nullptr);
    m_backend = backend;
    if (m_backend) {
        connect(m_backend, &Backend::decodedChanged, this, &VolumeView::prepare);
        connect(m_backend, &Backend::sliceChanged, this, [this] {
            prepare();
            // Where the planes meet moves its mark.
            update();
        });
        connect(m_backend, &Backend::windowChanged, this, [this] {
            if (m_view.look == Look::Mip || m_view.look == Look::Surface)
                request(false);
        });
        connect(m_backend, &Backend::fusionChanged, this, [this] { request(false); });
    }
    emit backendChanged();
    prepare();
}

void VolumeView::setAccent(const QColor &accent) {
    if (accent == m_accent)
        return;
    m_accent = accent;
    emit accentChanged();
    update();
}

void VolumeView::setActive(bool active) {
    if (active == m_active)
        return;
    m_active = active;
    if (!m_active)
        stopSpin();
    emit activeChanged();
    prepare();
    request(false);
}

QString VolumeView::status() const {
    if (!m_backend || !m_backend->hasImages() || m_backend->seriesIndex() < 0)
        return {};
    if (!m_backend->volume().valid || m_unpackable)
        return tr("This series can't be shown in 3D");
    if (!m_backend->allDecoded())
        return tr("Reading the series…");
    if (!m_full)
        return tr("Getting the 3D view ready…");
    return {};
}

QString VolumeView::lookName() const { return tr(::lookName(m_view.look)); }

QStringList VolumeView::edges() const {
    const double down[3] = {-m_view.up[0], -m_view.up[1], -m_view.up[2]};
    return edgeLabels(m_view.right, down);
}

void VolumeView::defaultLook() {
    const Series *s = nullptr;
    if (m_backend && m_backend->seriesIndex() >= 0 && m_backend->seriesIndex() < m_backend->scan().series.size())
        s = &m_backend->scan().series[m_backend->seriesIndex()];
    const QString modality = s ? s->modality : QString();
    m_view.look = modality == "CT" ? Look::Bone : modality == "PT" ? Look::Mip : Look::Surface;
    m_view.shift = 0;
}

void VolumeView::prepare() {
    if (!m_backend)
        return;
    const int index = m_backend->seriesIndex();
    const QList<Series> &all = m_backend->scan().series;
    const QString key = index >= 0 && index < all.size() && !all[index].slices.isEmpty()
                            ? QString::number(index) + '|' + all[index].slices.first().path
                            : QString();
    if (key != m_key) {
        m_key = key;
        m_full.reset();
        m_half.reset();
        m_image = QImage();
        m_fine = false;
        m_packing = false;
        m_unpackable = false;
        stopSpin();
        m_view = View3D();
        m_face = 0;
        defaultLook();
        emit viewChanged();
        emit stateChanged();
        update();
    }
    if (!m_active || key.isEmpty() || m_full || m_packing || m_unpackable)
        return;
    if (!m_backend->volume().valid || !m_backend->allDecoded()) {
        emit stateChanged();
        return;
    }
    m_packing = true;
    emit stateChanged();
    const QVector<Frame> frames = m_backend->frames();
    const Volume volume = m_backend->volume();
    QPointer<VolumeView> self(this);
    QThreadPool::globalInstance()->start([self, frames, volume, key] {
        auto full = std::make_shared<const VoxelGrid>(packVolume(volume, frames));
        auto half = full->valid ? std::make_shared<const VoxelGrid>(halve(*full)) : nullptr;
        QMetaObject::invokeMethod(qApp, [self, full, half, key] {
            if (!self || self->m_key != key)
                return;
            self->m_packing = false;
            if (!full->valid) {
                self->m_unpackable = true;
            } else {
                self->m_full = full;
                self->m_half = half;
            }
            emit self->stateChanged();
            self->request(false);
        }, Qt::QueuedConnection);
    });
}

QSize VolumeView::renderSize() const {
    const double ratio = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    QSize size(int(std::ceil(width() * ratio)), int(std::ceil(height() * ratio)));
    // Past this, finer pictures cost far more than they show.
    const int longest = std::max(size.width(), size.height());
    if (longest > 1600)
        size = (QSizeF(size) * (1600.0 / longest)).toSize();
    return size;
}

void VolumeView::request(bool quick) {
    if (!m_active || !m_full || width() < 1 || height() < 1)
        return;
    m_dirty = true;
    if (quick) {
        m_quickWanted = true;
        // A fine picture still being drawn is out of date.
        if (m_cancel)
            *m_cancel = true;
        m_settle.start();
    } else if (!m_settle.isActive()) {
        m_quickWanted = false;
    }
    if (!m_rendering)
        startRender();
}

void VolumeView::startRender() {
    if (!m_full)
        return;
    m_dirty = false;
    m_rendering = true;
    const bool quick = m_quickWanted;
    const auto grid = quick && m_half ? m_half : m_full;
    View3D view = m_view;
    if (m_backend) {
        view.center = m_backend->windowCenter() + (view.look == Look::Mip || view.look == Look::Surface ? view.shift : 0);
        view.width = std::max(1.0, m_backend->windowWidth());
        view.inverted = m_backend->inverted();
        view.colorMap = m_backend->colorMap();
    }
    QSize size = renderSize();
    if (quick)
        size = (QSizeF(size) / (spinning() ? 2.0 : 3.0)).toSize().expandedTo(QSize(1, 1));
    const double step = quick ? 1.0 : 0.75;
    const auto pet = m_backend ? m_backend->fusionGrid() : nullptr;
    const Overlay3D look = m_backend ? m_backend->fusionLook() : Overlay3D();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_cancel = quick ? nullptr : cancel;
    const int generation = ++m_generation;
    QPointer<VolumeView> self(this);
    m_pool.start([self, grid, view, size, step, pet, look, cancel, quick, generation] {
        Overlay3D overlay = look;
        overlay.grid = pet.get();
        const QImage image = renderVolume(*grid, view, size, step, pet ? &overlay : nullptr, cancel.get());
        QMetaObject::invokeMethod(qApp, [self, image, quick, generation] {
            if (!self)
                return;
            self->m_rendering = false;
            if (generation == self->m_generation && !image.isNull()) {
                self->m_image = image;
                self->m_fine = !quick;
                self->update();
            }
            if (self->m_dirty)
                self->startRender();
        }, Qt::QueuedConnection);
    });
}

void VolumeView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        request(true);
}

void VolumeView::turn(double dx, double dy) {
    m_view.turn(dx * 0.5 * Degree, dy * 0.5 * Degree);
    emit viewChanged();
    request(true);
}

void VolumeView::turnBy(double acrossDegrees, double downDegrees) {
    m_view.turn(acrossDegrees * Degree, downDegrees * Degree);
    emit viewChanged();
    request(false);
}

void VolumeView::zoomBy(double factor) {
    m_view.zoom = std::clamp(m_view.zoom * factor, 0.2, 20.0);
    emit viewChanged();
    request(true);
}

void VolumeView::pan(double dx, double dy) {
    if (!m_full || width() < 1 || height() < 1)
        return;
    const double pixel = 2 * m_full->fit / (std::min(width(), height()) * m_view.zoom);
    m_view.pan[0] += dx * pixel;
    m_view.pan[1] -= dy * pixel;
    emit viewChanged();
    request(true);
}

void VolumeView::cutDeeper(double mm) { setCut(m_view.cut + mm); }

void VolumeView::setCut(double mm) {
    const double most = m_full ? 2 * m_full->radius - 1 : 0;
    const double cut = std::clamp(mm, 0.0, std::max(0.0, most));
    if (cut == m_view.cut)
        return;
    m_view.cut = cut;
    emit viewChanged();
    request(true);
}

void VolumeView::beginShift() { m_shiftStart = m_view.shift; }

void VolumeView::shiftLook(double dx) {
    // Hounsfield units for CT's looks; a share of the window for the rest.
    const bool windowed = m_view.look == Look::Mip || m_view.look == Look::Surface;
    const double perPixel = windowed && m_backend ? std::max(1.0, m_backend->windowWidth()) / 300 : 2;
    m_view.shift = m_shiftStart + dx * perPixel;
    emit viewChanged();
    request(true);
}

void VolumeView::nextLook() {
    const Series *s = nullptr;
    if (m_backend && m_backend->seriesIndex() >= 0 && m_backend->seriesIndex() < m_backend->scan().series.size())
        s = &m_backend->scan().series[m_backend->seriesIndex()];
    static const Look ct[] = {Look::Bone, Look::Vessels, Look::Skin, Look::Lungs, Look::Mip};
    static const Look other[] = {Look::Surface, Look::Mip};
    const bool isCt = s && s->modality == "CT";
    const Look *order = isCt ? ct : other;
    const int count = isCt ? 5 : 2;
    int at = 0;
    for (int i = 0; i < count; ++i)
        if (order[i] == m_view.look)
            at = i;
    m_view.look = order[(at + 1) % count];
    m_view.shift = 0;
    emit viewChanged();
    if (m_backend)
        m_backend->announce(lookName());
    request(false);
}

void VolumeView::face(int side) {
    m_view.face(side);
    emit viewChanged();
    request(false);
}

void VolumeView::nextFace() {
    static const int order[] = {0, 2, 1, 3, 4};
    static const char *names[] = {QT_TRANSLATE_NOOP("dicolens", "From the front"),
                                  QT_TRANSLATE_NOOP("dicolens", "From the patient's left"),
                                  QT_TRANSLATE_NOOP("dicolens", "From the back"),
                                  QT_TRANSLATE_NOOP("dicolens", "From the patient's right"),
                                  QT_TRANSLATE_NOOP("dicolens", "From above")};
    m_face = (m_face + 1) % 5;
    face(order[m_face]);
    if (m_backend)
        m_backend->announce(tr(names[m_face]));
}

void VolumeView::fit() {
    m_view.zoom = 1;
    m_view.pan[0] = m_view.pan[1] = 0;
    emit viewChanged();
    request(false);
}

void VolumeView::reset() {
    stopSpin();
    const View3D fresh;
    m_view = fresh;
    m_face = 0;
    defaultLook();
    emit viewChanged();
    request(false);
}

void VolumeView::toggleSpin() {
    if (spinning()) {
        stopSpin();
        return;
    }
    if (!m_full)
        return;
    m_spin.start();
    emit spinningChanged();
}

void VolumeView::stopSpin() {
    if (!m_spin.isActive())
        return;
    m_spin.stop();
    emit spinningChanged();
    m_settle.start();
}

QVariantList VolumeView::pointAt(double x, double y) const {
    if (!m_full || !m_backend)
        return {};
    View3D view = m_view;
    if (m_backend) {
        view.center = m_backend->windowCenter() + view.shift;
        view.width = std::max(1.0, m_backend->windowWidth());
    }
    double point[3];
    if (!pickVolume(*m_full, view, QSize(int(width()), int(height())), QPointF(x, y), point))
        return {};
    return {point[0], point[1], point[2], m_full->volume.frame};
}

void VolumeView::exportImage() {
    if (m_backend && !m_image.isNull())
        m_backend->exportPicture(m_image);
}

void VolumeView::copyImage() {
    if (m_backend && !m_image.isNull())
        m_backend->copyPicture(m_image);
}

void VolumeView::paint(QPainter *painter) {
    if (m_image.isNull())
        return;
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(boundingRect(), m_image);
    // Where the planes meet, from a click here or in a slice.
    if (m_full && m_backend && m_backend->crossingShown()) {
        double point[3];
        m_backend->crossingPoint(point);
        const QPointF at = projectPoint(*m_full, m_view, QSize(int(width()), int(height())), point);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(QColor(0, 0, 0, 160), 4));
        painter->drawLine(at - QPointF(9, 0), at + QPointF(9, 0));
        painter->drawLine(at - QPointF(0, 9), at + QPointF(0, 9));
        painter->setPen(QPen(m_accent, 2));
        painter->drawLine(at - QPointF(8, 0), at + QPointF(8, 0));
        painter->drawLine(at - QPointF(0, 8), at + QPointF(0, 8));
    }
}

bool VolumeView::waitForPicture(int milliseconds) {
    QDeadlineTimer deadline(milliseconds);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (m_full && m_fine && !m_rendering && !m_dirty && !m_packing && !m_settle.isActive())
            return true;
        QThread::msleep(5);
    }
    return false;
}
