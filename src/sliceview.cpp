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
        connect(m_backend, &Backend::orientationChanged, this, [this] { update(); });
        connect(m_backend, &Backend::marksChanged, this, [this] { update(); });
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

QTransform SliceView::transform() const {
    if (!m_backend || m_backend->image().isNull())
        return {};
    const QImage image = m_backend->image();
    const double aspect = m_backend->pixelAspect();
    // Fitted as turned: a quarter turn swaps the picture's sides.
    const bool sideways = m_backend->rotation() % 2;
    const double w = image.width() * aspect, h = image.height();
    const double scale = std::min(width() / (sideways ? h : w), height() / (sideways ? w : h)) * m_zoom;
    QTransform t;
    t.translate(width() / 2 + m_pan.x(), height() / 2 + m_pan.y());
    if (m_backend->flipped())
        t.scale(-1, 1);
    t.rotate(90 * m_backend->rotation());
    t.scale(scale * aspect, scale);
    t.translate(-image.width() / 2.0, -image.height() / 2.0);
    return t;
}

QRectF SliceView::imageRect() const {
    if (!m_backend || m_backend->image().isNull())
        return {};
    const QImage image = m_backend->image();
    return transform().mapRect(QRectF(0, 0, image.width(), image.height()));
}

QPointF SliceView::toImage(double x, double y) const {
    if (imageRect().isEmpty())
        return {-1, -1};
    return transform().inverted().map(QPointF(x, y));
}

QPointF SliceView::toItem(double x, double y) const {
    if (imageRect().isEmpty())
        return {};
    return transform().map(QPointF(x, y));
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
    const QTransform t = transform();
    // Smooth until single pixels are big enough to be worth seeing as such.
    const bool smooth = std::hypot(t.m11(), t.m12()) < 3;
    painter->save();
    painter->setTransform(t, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    painter->drawImage(QPointF(0, 0), image);
    // Overlay planes are bits over the image, drawn in the accent.
    if (const QImage mask = m_backend->overlay(); m_backend->marksShown() && !mask.isNull()) {
        QImage tinted(mask.size(), QImage::Format_ARGB32_Premultiplied);
        tinted.fill(m_accent);
        QPainter tint(&tinted);
        tint.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        tint.drawImage(0, 0, mask);
        tint.end();
        painter->drawImage(QPointF(0, 0), tinted);
    }
    painter->restore();
    if (m_backend->marksShown())
        paintMarks(painter, t, image.size());

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

    paintLabel(painter, b + QPointF(12, -4), rulerText());
}

void SliceView::paintLabel(QPainter *painter, QPointF at, const QString &text) {
    QFont font = painter->font();
    font.setPixelSize(14);
    font.setBold(true);
    painter->setFont(font);
    const QFontMetricsF metrics(font);
    const QSizeF size(metrics.horizontalAdvance(text) + 14, metrics.height() + 6);
    at.setY(at.y() - size.height());
    at.setX(std::clamp(at.x(), 4.0, std::max(4.0, width() - size.width() - 4)));
    at.setY(std::clamp(at.y(), 4.0, std::max(4.0, height() - size.height() - 4)));
    const QRectF box(at, size);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(14, 14, 16, 220));
    painter->drawRect(box);
    painter->setPen(m_accent);
    painter->drawText(box, Qt::AlignCenter, text);
}

void SliceView::paintMarks(QPainter *painter, const QTransform &t, const QSizeF &size) {
    const QList<Mark> marks = m_backend->marks();
    if (marks.isEmpty())
        return;
    // Display units are fractions of the picture; pixel units are pixels.
    auto place = [&](QPointF p, bool display) {
        return t.map(display ? QPointF(p.x() * size.width(), p.y() * size.height()) : p);
    };
    painter->setRenderHint(QPainter::Antialiasing);
    const QPen shadow(QColor(0, 0, 0, 160), 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    const QPen line(m_accent, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);

    for (const auto &m : marks) {
        if (m.kind == Mark::Kind::Text)
            continue;
        QPainterPath path;
        if (m.kind == Mark::Kind::Polyline) {
            path.moveTo(place(m.points[0], m.display));
            for (int i = 1; i < m.points.size(); ++i)
                path.lineTo(place(m.points[i], m.display));
        } else if (m.kind == Mark::Kind::Circle || m.kind == Mark::Kind::Ellipse) {
            // Built where it's drawn, so a turned or stretched image turns
            // and stretches the outline with it.
            QPointF c, major, minor;
            if (m.kind == Mark::Kind::Circle) {
                c = m.points[0];
                const QPointF edge = m.points[1] - c;
                major = edge;
                minor = QPointF(-edge.y(), edge.x());
            } else {
                c = (m.points[0] + m.points[1]) / 2;
                major = m.points[1] - c;
                minor = m.points[3] - (m.points[2] + m.points[3]) / 2;
            }
            for (int i = 0; i <= 72; ++i) {
                const double a = i * M_PI / 36;
                const QPointF p = place(c + major * std::cos(a) + minor * std::sin(a), m.display);
                i == 0 ? path.moveTo(p) : path.lineTo(p);
            }
        }
        for (const QPen &pen : {shadow, line}) {
            painter->setPen(pen);
            painter->setBrush(m.filled && &pen == &line ? QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 60) : QColor(Qt::transparent));
            if (m.kind == Mark::Kind::Points)
                for (const auto &p : m.points) {
                    const QPointF at = place(p, m.display);
                    painter->drawLine(at - QPointF(5, 0), at + QPointF(5, 0));
                    painter->drawLine(at - QPointF(0, 5), at + QPointF(0, 5));
                }
            else
                painter->drawPath(path);
        }
    }

    for (const auto &m : marks) {
        if (m.kind != Mark::Kind::Text)
            continue;
        // A note reads upright wherever a turn takes its box: from the
        // box's bottom left as it now lies, or just above its anchor.
        QPointF at = place(m.anchor, m.anchorDisplay) + QPointF(10, -10);
        if (m.hasBox)
            at = QRectF(place(m.box.topLeft(), m.boxDisplay), place(m.box.bottomRight(), m.boxDisplay)).normalized().bottomLeft();
        if (m.hasAnchor && m.anchorLine) {
            const QPointF anchor = place(m.anchor, m.anchorDisplay);
            for (const QPen &pen : {shadow, line}) {
                painter->setPen(pen);
                painter->drawLine(at, anchor);
            }
        }
        paintLabel(painter, at, m.text);
    }
}
