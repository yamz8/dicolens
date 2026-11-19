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
        // Measurements stay on their slice, and come back with it.
        connect(m_backend, &Backend::sliceChanged, this, [this] {
            m_drawing = m_secondArm = false;
            emit measuresChanged();
            update();
        });
        // A new scan starts fitted, with nothing measured.
        connect(m_backend, &Backend::opened, this, [this] {
            m_measures.clear();
            m_drawing = m_secondArm = false;
            emit measuresChanged();
            fit();
        });
        connect(m_backend, &Backend::orientationChanged, this, [this] { update(); });
        connect(m_backend, &Backend::marksChanged, this, [this] { update(); });
    }
    emit backendChanged();
    update();
}

void SliceView::setReference(Backend *reference) {
    if (m_reference == reference)
        return;
    if (m_reference)
        disconnect(m_reference, nullptr, this, nullptr);
    m_reference = reference;
    if (m_reference) {
        connect(m_reference, &Backend::sliceChanged, this, [this] { update(); });
        connect(m_reference, &Backend::orientationChanged, this, [this] { update(); });
    }
    emit referenceChanged();
    update();
}

bool SliceView::referenceLine(QPointF &a, QPointF &b) const {
    if (!m_backend || !m_reference || m_reference == m_backend || m_backend->image().isNull())
        return false;
    const ImagePlane here = m_backend->imagePlane(), there = m_reference->imagePlane();
    if (!here.valid || !there.valid || here.frame.isEmpty() || here.frame != there.frame)
        return false;
    // Points of this picture, o + x·r + y·c, that lie in the other plane:
    // a line, ax + by + d = 0, unless the two are parallel.
    double n[3];
    there.normal(n);
    double ka = 0, kb = 0, d = 0;
    for (int i = 0; i < 3; ++i) {
        ka += here.rowSpacing * here.row[i] * n[i];
        kb += here.columnSpacing * here.column[i] * n[i];
        d += (here.origin[i] - there.origin[i]) * n[i];
    }
    if (std::hypot(ka, kb) < 1e-3 * std::max(here.rowSpacing, here.columnSpacing))
        return false;
    // Where the line crosses the image's edges, at pixel centres' bounds.
    const double x0 = -0.5, y0 = -0.5, x1 = m_backend->image().width() - 0.5, y1 = m_backend->image().height() - 0.5;
    QList<QPointF> hits;
    auto add = [&](double x, double y) {
        if (x >= x0 - 1e-6 && x <= x1 + 1e-6 && y >= y0 - 1e-6 && y <= y1 + 1e-6)
            hits << QPointF(x, y);
    };
    if (std::abs(kb) > 1e-9) {
        add(x0, -(ka * x0 + d) / kb);
        add(x1, -(ka * x1 + d) / kb);
    }
    if (std::abs(ka) > 1e-9) {
        add(-(kb * y0 + d) / ka, y0);
        add(-(kb * y1 + d) / ka, y1);
    }
    if (hits.size() < 2)
        return false;
    a = hits.first();
    b = hits.first();
    for (const QPointF &p : hits)
        if (QLineF(a, p).length() > QLineF(a, b).length())
            b = p;
    return QLineF(a, b).length() > 0.5;
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

QString SliceView::key() const {
    if (!m_backend)
        return {};
    return QStringLiteral("%1/%2/%3").arg(m_backend->seriesIndex()).arg(m_backend->plane()).arg(m_backend->sliceIndex());
}

void SliceView::setTool(int tool) {
    if (tool == m_tool)
        return;
    m_tool = tool;
    m_drawing = m_secondArm = false;
    emit toolChanged();
    emit measuresChanged();
    update();
}

void SliceView::press(double x, double y) {
    const QPointF p = toImage(x, y);
    if (m_secondArm) {
        // The angle's far end, which finishes it.
        m_current.c = p;
        m_measures[key()] << m_current;
        m_drawing = m_secondArm = false;
        emit measuresChanged();
        update();
        return;
    }
    if (m_tool == 0)
        return;
    m_current = {m_tool, p, p, p};
    m_drawing = true;
    emit measuresChanged();
    update();
}

void SliceView::drag(double x, double y) {
    if (!m_drawing || m_secondArm)
        return;
    m_current.b = toImage(x, y);
    m_current.c = m_current.b;
    update();
}

void SliceView::release(double x, double y) {
    if (!m_drawing || m_secondArm)
        return;
    m_current.b = toImage(x, y);
    m_current.c = m_current.b;
    // A click with no drag measures nothing.
    const QPointF d = toItem(m_current.b.x(), m_current.b.y()) - toItem(m_current.a.x(), m_current.a.y());
    if (std::hypot(d.x(), d.y()) < 3) {
        m_drawing = false;
        emit measuresChanged();
        update();
        return;
    }
    if (m_current.kind == 3) {
        m_secondArm = true;
        update();
        return;
    }
    m_measures[key()] << m_current;
    m_drawing = false;
    emit measuresChanged();
    update();
}

void SliceView::hover(double x, double y) {
    if (!m_secondArm)
        return;
    m_current.c = toImage(x, y);
    update();
}

bool SliceView::removeLast() {
    if (m_drawing) {
        m_drawing = m_secondArm = false;
    } else {
        auto it = m_measures.find(key());
        if (it == m_measures.end() || it->isEmpty())
            return false;
        it->removeLast();
    }
    emit measuresChanged();
    update();
    return true;
}

void SliceView::clearMeasures() {
    m_measures.remove(key());
    m_drawing = m_secondArm = false;
    emit measuresChanged();
    update();
}

QString SliceView::text(const Measure &m) const {
    if (!m_backend)
        return {};
    if (m.kind == 2) {
        const QRectF box = QRectF(m.a, m.b).normalized();
        return m_backend->areaStats(box.x(), box.y(), box.width(), box.height());
    }
    if (m.kind == 3) {
        // Pixels needn't be square: the angle is the one in the body.
        const double aspect = m_backend->pixelAspect();
        const QPointF u = m.a - m.b, v = m.c - m.b;
        const double ux = u.x() * aspect, vx = v.x() * aspect;
        const double lu = std::hypot(ux, u.y()), lv = std::hypot(vx, v.y());
        if (lu < 1e-9 || lv < 1e-9)
            return {};
        const double cosine = std::clamp((ux * vx + u.y() * v.y()) / (lu * lv), -1.0, 1.0);
        return m_backend->angleText(std::acos(cosine) * 180 / M_PI);
    }
    return m_backend->distance(m.a.x(), m.a.y(), m.b.x(), m.b.y());
}

QStringList SliceView::measureTexts() const {
    QStringList out;
    for (const auto &m : m_measures.value(key()))
        out << text(m);
    return out;
}

void SliceView::paintMeasure(QPainter *painter, const QTransform &t, const Measure &m, bool active) {
    painter->setRenderHint(QPainter::Antialiasing);
    const QPointF a = t.map(m.a), b = t.map(m.b), c = t.map(m.c);
    QPen shadow(QColor(0, 0, 0, 160), 4, Qt::SolidLine, Qt::RoundCap);
    QPen line(m_accent, 2, Qt::SolidLine, Qt::RoundCap);
    painter->setBrush(Qt::NoBrush);
    for (const QPen &pen : {shadow, line}) {
        painter->setPen(pen);
        if (m.kind == 2) {
            // The ellipse in the box, turned with the picture.
            QPainterPath path;
            path.addEllipse(QRectF(m.a, m.b).normalized());
            painter->drawPath(t.map(path));
        } else {
            painter->drawLine(a, b);
            if (m.kind == 3 && (m_secondArm || !active))
                painter->drawLine(b, c);
            // Short ticks across a ruler's ends.
            const QPointF d = b - a;
            const double length = std::hypot(d.x(), d.y());
            if (m.kind == 1 && length > 0.5) {
                const QPointF n(-d.y() / length * 6, d.x() / length * 6);
                painter->drawLine(a - n, a + n);
                painter->drawLine(b - n, b + n);
            }
        }
    }
    if (m.kind == 3 && active && !m_secondArm)
        return;
    QPointF at = m.kind == 2 ? QPointF(std::max({a.x(), b.x()}) + 8, std::min({a.y(), b.y()}) + 4)
                             : b + QPointF(12, -4);
    paintLabel(painter, at, text(m));
}

void SliceView::paint(QPainter *painter) {
    const QRectF r = imageRect();
    if (r.isEmpty())
        return;
    const QImage image = m_backend->image();
    const QTransform t = transform();
    // Smooth until single pixels are big enough to be worth seeing as such;
    // a plane cut across thick slices is smoothed between them, as it's
    // their spacing, not real detail, that makes its pixels tall.
    const bool smooth = std::min(std::hypot(t.m11(), t.m12()), std::hypot(t.m21(), t.m22())) < 3;
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

    if (QPointF ra, rb; referenceLine(ra, rb)) {
        painter->setRenderHint(QPainter::Antialiasing);
        QColor color = m_accent;
        color.setAlpha(200);
        QPen pen(color, 1.5, Qt::DashLine);
        painter->setPen(pen);
        painter->drawLine(t.map(ra), t.map(rb));
    }

    for (const auto &m : m_measures.value(key()))
        paintMeasure(painter, t, m, false);
    if (m_drawing)
        paintMeasure(painter, t, m_current, true);
}

void SliceView::paintLabel(QPainter *painter, QPointF at, const QString &text) {
    QFont font = painter->font();
    font.setPixelSize(14);
    font.setBold(true);
    painter->setFont(font);
    const QFontMetricsF metrics(font);
    const QStringList lines = text.split('\n');
    double widest = 0;
    for (const QString &line : lines)
        widest = std::max(widest, metrics.horizontalAdvance(line));
    const QSizeF size(widest + 14, metrics.lineSpacing() * lines.size() + 6);
    // One line sits above the point; a block of them hangs below it.
    if (lines.size() == 1)
        at.setY(at.y() - size.height());
    at.setX(std::clamp(at.x(), 4.0, std::max(4.0, width() - size.width() - 4)));
    at.setY(std::clamp(at.y(), 4.0, std::max(4.0, height() - size.height() - 4)));
    const QRectF box(at, size);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(14, 14, 16, 220));
    painter->drawRect(box);
    painter->setPen(m_accent);
    painter->drawText(box.adjusted(7, 3, -7, -3), (lines.size() == 1 ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter), text);
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
