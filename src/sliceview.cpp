#include "sliceview.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

#include "backend.h"

SliceView::SliceView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    connect(this, &QQuickItem::widthChanged, this, [this] { update(); });
    connect(this, &QQuickItem::heightChanged, this, [this] { update(); });
}

void SliceView::setBackend(Backend *backend) {
    if (m_backend == backend)
        return;
    if (m_backend)
        disconnect(m_backend, nullptr, this, nullptr);
    m_backend = backend;
    if (m_backend) {
        connect(m_backend, &Backend::imageChanged, this, [this] { update(); });
        // A new scan starts fitted, and a ruler belongs to its slice.
        connect(m_backend, &Backend::sliceChanged, this, [this] {
            clearRuler();
            update();
        });
        connect(m_backend, &Backend::opened, this, &SliceView::fit);
    }
    emit backendChanged();
    update();
}

void SliceView::setAccent(const QColor &accent) {
    if (accent == m_accent)
        return;
    m_accent = accent;
    emit accentChanged();
    update();
}

QRectF SliceView::imageRect() const {
    if (!m_backend || m_backend->image().isNull())
        return {};
    const QImage image = m_backend->image();
    const double aspect = m_backend->pixelAspect();
    const double w = image.width() * aspect, h = image.height();
    const double scale = std::min(width() / w, height() / h) * m_zoom;
    const QSizeF size(w * scale, h * scale);
    const QPointF topLeft((width() - size.width()) / 2 + m_pan.x(), (height() - size.height()) / 2 + m_pan.y());
    return {topLeft, size};
}

QPointF SliceView::toImage(double x, double y) const {
    const QRectF r = imageRect();
    if (r.isEmpty())
        return {-1, -1};
    const QImage image = m_backend->image();
    return {(x - r.x()) / r.width() * image.width(), (y - r.y()) / r.height() * image.height()};
}

QPointF SliceView::toItem(double x, double y) const {
    const QRectF r = imageRect();
    if (r.isEmpty())
        return {};
    const QImage image = m_backend->image();
    return {r.x() + x / image.width() * r.width(), r.y() + y / image.height() * r.height()};
}

void SliceView::zoomAt(double x, double y, double factor) {
    const double zoom = std::clamp(m_zoom * factor, 0.25, 40.0);
    factor = zoom / m_zoom;
    // Keeps the point under the pointer where it is.
    const QPointF center(width() / 2 + m_pan.x(), height() / 2 + m_pan.y());
    const QPointF offset = QPointF(x, y) - center;
    m_pan += offset - offset * factor;
    m_zoom = zoom;
    emit viewChanged();
    update();
}

void SliceView::pan(double dx, double dy) {
    m_pan += QPointF(dx, dy);
    emit viewChanged();
    update();
}

void SliceView::fit() {
    m_zoom = 1;
    m_pan = {};
    emit viewChanged();
    update();
}

void SliceView::startRuler(double x, double y) {
    m_rulerA = m_rulerB = toImage(x, y);
    m_rulerVisible = true;
    emit rulerChanged();
    update();
}

void SliceView::moveRuler(double x, double y) {
    m_rulerB = toImage(x, y);
    emit rulerChanged();
    update();
}

void SliceView::clearRuler() {
    if (!m_rulerVisible)
        return;
    m_rulerVisible = false;
    emit rulerChanged();
    update();
}

QString SliceView::rulerText() const {
    if (!m_rulerVisible || !m_backend)
        return {};
    return m_backend->distance(m_rulerA.x(), m_rulerA.y(), m_rulerB.x(), m_rulerB.y());
}

void SliceView::paint(QPainter *painter) {
    const QRectF r = imageRect();
    if (r.isEmpty())
        return;
    const QImage image = m_backend->image();
    // Smooth until single pixels are big enough to be worth seeing as such.
    const bool smooth = r.width() / image.width() < 3;
    painter->setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    painter->drawImage(r, image);

    if (!m_rulerVisible)
        return;
    const QPointF a = toItem(m_rulerA.x(), m_rulerA.y()), b = toItem(m_rulerB.x(), m_rulerB.y());
    painter->setRenderHint(QPainter::Antialiasing);
    QPen shadow(QColor(0, 0, 0, 160), 4, Qt::SolidLine, Qt::RoundCap);
    QPen line(m_accent, 2, Qt::SolidLine, Qt::RoundCap);
    for (const QPen &pen : {shadow, line}) {
        painter->setPen(pen);
        painter->drawLine(a, b);
        // Short ticks across each end.
        const QPointF d = b - a;
        const double length = std::hypot(d.x(), d.y());
        if (length > 0.5) {
            const QPointF n(-d.y() / length * 6, d.x() / length * 6);
            painter->drawLine(a - n, a + n);
            painter->drawLine(b - n, b + n);
        }
    }

    const QString text = rulerText();
    QFont font = painter->font();
    font.setPixelSize(14);
    font.setBold(true);
    painter->setFont(font);
    const QFontMetricsF metrics(font);
    const QSizeF size(metrics.horizontalAdvance(text) + 14, metrics.height() + 6);
    QPointF at = b + QPointF(12, -size.height() - 4);
    at.setX(std::clamp(at.x(), 4.0, width() - size.width() - 4));
    at.setY(std::clamp(at.y(), 4.0, height() - size.height() - 4));
    const QRectF box(at, size);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(14, 14, 16, 220));
    painter->drawRect(box);
    painter->setPen(m_accent);
    painter->drawText(box, Qt::AlignCenter, text);
}
