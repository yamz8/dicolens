#include "sliceview.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

#include "backend.h"

namespace {

// A plane counts its pixels from the middle of the first, as DICOM places
// an image; the picture drawn counts from that pixel's corner.
const QPointF Half(0.5, 0.5);

}

SliceView::SliceView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    connect(this, &QQuickItem::widthChanged, this, [this] { update(); });
    connect(this, &QQuickItem::heightChanged, this, [this] { update(); });
    // Another view of the same series may have measured it.
    connect(&MeasureStore::instance(), &MeasureStore::changed, this, [this] {
        emit measuresChanged();
        update();
    });
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
        // One half drawn is dropped whole, or a Cobb angle's first line
        // would wait for its second on another slice.
        connect(m_backend, &Backend::sliceChanged, this, [this] {
            m_drawing = m_secondArm = m_secondLine = m_firstDone = false;
            emit measuresChanged();
            update();
        });
        // A new scan starts fitted, with what was measured on it before.
        connect(m_backend, &Backend::opened, this, [this] {
            m_drawing = m_secondArm = m_secondLine = m_firstDone = false;
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

void SliceView::setSecondReference(Backend *reference) {
    if (m_secondReference == reference)
        return;
    if (m_secondReference && m_secondReference != m_reference)
        disconnect(m_secondReference, nullptr, this, nullptr);
    m_secondReference = reference;
    if (m_secondReference) {
        connect(m_secondReference, &Backend::sliceChanged, this, [this] { update(); });
        connect(m_secondReference, &Backend::orientationChanged, this, [this] { update(); });
    }
    emit referenceChanged();
    update();
}

bool SliceView::lineTo(Backend *other, QPointF &a, QPointF &b) const {
    if (!m_backend || !other || other == m_backend || m_backend->imageWidth() == 0)
        return false;
    const ImagePlane here = m_backend->imagePlane(), there = other->imagePlane();
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
    const double x0 = -0.5, y0 = -0.5, x1 = m_backend->imageWidth() - 0.5, y1 = m_backend->imageHeight() - 0.5;
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

void SliceView::setTextScale(double scale) {
    if (scale <= 0 || scale == m_textScale)
        return;
    m_textScale = scale;
    emit accentChanged();
    update();
}

QTransform SliceView::transform() const {
    // The picture's size is all this needs, which the backend knows
    // without making the picture.
    const QSizeF image = m_backend ? QSizeF(m_backend->imageWidth(), m_backend->imageHeight()) : QSizeF();
    if (image.isEmpty())
        return {};
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
    if (!m_backend || m_backend->imageWidth() == 0)
        return {};
    return transform().mapRect(QRectF(0, 0, m_backend->imageWidth(), m_backend->imageHeight()));
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

QString SliceView::seriesUid() const {
    if (!m_backend)
        return {};
    const QList<Series> &all = m_backend->scan().series;
    const int i = m_backend->seriesIndex();
    if (i < 0 || i >= all.size())
        return {};
    // A series without a UID still keeps its measurements while it's open.
    return all[i].uid.isEmpty() ? QStringLiteral("unnamed:%1:%2").arg(i).arg(all[i].slices.value(0).path) : all[i].uid;
}

QList<Measure> SliceView::measures() const {
    const QString uid = seriesUid();
    return uid.isEmpty() ? QList<Measure>()
                         : MeasureStore::instance().at(uid, m_backend->plane(), m_backend->sliceIndex(), m_backend->cutKey());
}

void SliceView::keep(const Measure &m) {
    const QString uid = seriesUid();
    if (!uid.isEmpty())
        MeasureStore::instance().add(uid, m_backend->plane(), m_backend->sliceIndex(), m, m_backend->cutKey());
}

void SliceView::setTool(int tool) {
    if (tool == m_tool)
        return;
    m_tool = tool;
    m_drawing = m_secondArm = m_secondLine = m_firstDone = false;
    emit toolChanged();
    emit measuresChanged();
    update();
}

void SliceView::press(double x, double y) {
    const QPointF p = toImage(x, y);
    if (m_secondArm) {
        // The angle's far end, which finishes it.
        m_current.c = p;
        keep(m_current);
        m_drawing = m_secondArm = false;
        emit measuresChanged();
        update();
        return;
    }
    if (m_firstDone) {
        // A Cobb angle's second line starts.
        m_current.c = m_current.d = p;
        m_secondLine = true;
        m_firstDone = false;
        update();
        return;
    }
    if (m_tool == 0)
        return;
    m_current = {m_tool, p, p, p, p};
    m_drawing = true;
    emit measuresChanged();
    update();
}

void SliceView::drag(double x, double y) {
    if (!m_drawing || m_secondArm || m_firstDone)
        return;
    if (m_secondLine) {
        m_current.d = toImage(x, y);
        update();
        return;
    }
    m_current.b = toImage(x, y);
    m_current.c = m_current.b;
    update();
}

void SliceView::release(double x, double y) {
    if (!m_drawing || m_secondArm || m_firstDone)
        return;
    if (m_secondLine) {
        m_current.d = toImage(x, y);
        const QPointF d = toItem(m_current.d.x(), m_current.d.y()) - toItem(m_current.c.x(), m_current.c.y());
        // A click with no drag waits for a line instead.
        if (std::hypot(d.x(), d.y()) < 3) {
            m_secondLine = false;
            m_firstDone = true;
            update();
            return;
        }
        keep(m_current);
        m_drawing = m_secondLine = false;
        emit measuresChanged();
        update();
        return;
    }
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
    if (m_current.kind == 4) {
        m_firstDone = true;
        update();
        return;
    }
    keep(m_current);
    m_drawing = false;
    emit measuresChanged();
    update();
}

QPointF SliceView::toMm(QPointF p) const {
    double across = 1, down = 1;
    if (m_backend)
        m_backend->spacingAt(p.x(), p.y(), across, down);
    return {p.x() * across, p.y() * down};
}

Backend *SliceView::referenceNear(double x, double y) const {
    Backend *found = nullptr;
    double best = 8;
    for (Backend *other : {m_reference.data(), m_secondReference.data()}) {
        QPointF a, b;
        if (!other || !lineTo(other, a, b) || !other->canReformat())
            continue;
        const QLineF line(toItem(a.x() + Half.x(), a.y() + Half.y()), toItem(b.x() + Half.x(), b.y() + Half.y()));
        const QPointF p(x, y), d = line.p2() - line.p1();
        const double length2 = d.x() * d.x() + d.y() * d.y();
        if (length2 < 1e-9)
            continue;
        const double t = std::clamp(((p - line.p1()).x() * d.x() + (p - line.p1()).y() * d.y()) / length2, 0.0, 1.0);
        const QPointF nearest = line.p1() + t * d;
        const double distance = std::hypot(p.x() - nearest.x(), p.y() - nearest.y());
        if (distance < best) {
            best = distance;
            found = other;
        }
    }
    return found;
}

bool SliceView::nearReference(double x, double y) const { return referenceNear(x, y) != nullptr; }

void SliceView::beginTurn(double x, double y) {
    QPointF a, b;
    m_turning = false;
    m_turnTarget = referenceNear(x, y);
    if (!m_turnTarget)
        m_turnTarget = m_reference;
    if (!m_backend || !m_turnTarget || !lineTo(m_turnTarget, a, b) || !m_turnTarget->canReformat())
        return;
    const ImagePlane here = m_backend->imagePlane();
    // It turns about the point on the line nearest where the planes meet,
    // which becomes where they meet.
    double point[3];
    m_turnTarget->crossingPoint(point);
    double d[3] = {point[0] - here.origin[0], point[1] - here.origin[1], point[2] - here.origin[2]};
    const QPointF c((d[0] * here.row[0] + d[1] * here.row[1] + d[2] * here.row[2]) / here.rowSpacing,
                    (d[0] * here.column[0] + d[1] * here.column[1] + d[2] * here.column[2]) / here.columnSpacing);
    const QPointF ab = b - a;
    const double t = std::clamp(((c - a).x() * ab.x() + (c - a).y() * ab.y()) / (ab.x() * ab.x() + ab.y() * ab.y()), 0.0, 1.0);
    m_turnPivot = a + t * ab;
    here.point(m_turnPivot.x(), m_turnPivot.y(), point);
    m_turnTarget->goThrough({point[0], point[1], point[2], here.frame});
    here.normal(m_turnAxis);
    m_turnStart = toImage(x, y) - Half;
    m_turnTarget->beginTurn();
    m_turning = true;
}

void SliceView::turnTo(double x, double y) {
    if (!m_turning || !m_turnTarget)
        return;
    const QPointF u = toMm(m_turnStart) - toMm(m_turnPivot), v = toMm(toImage(x, y) - Half) - toMm(m_turnPivot);
    if (std::hypot(u.x(), u.y()) < 1e-6 || std::hypot(v.x(), v.y()) < 1e-6)
        return;
    // Turning by θ about the normal takes the row toward the column, as
    // the angle grows from x toward y in the picture.
    const double angle = std::atan2(v.y(), v.x()) - std::atan2(u.y(), u.x());
    m_turnTarget->turnPlane(m_turnAxis, angle);
}

void SliceView::hover(double x, double y) {
    if (!m_secondArm)
        return;
    m_current.c = toImage(x, y);
    update();
}

bool SliceView::removeLast() {
    if (m_drawing) {
        m_drawing = m_secondArm = m_secondLine = m_firstDone = false;
    } else {
        const QString uid = seriesUid();
        if (uid.isEmpty()
            || !MeasureStore::instance().removeLast(uid, m_backend->plane(), m_backend->sliceIndex(), m_backend->cutKey()))
            return false;
    }
    emit measuresChanged();
    update();
    return true;
}

void SliceView::clearMeasures() {
    if (const QString uid = seriesUid(); !uid.isEmpty())
        MeasureStore::instance().clear(uid, m_backend->plane(), m_backend->sliceIndex(), m_backend->cutKey());
    m_drawing = m_secondArm = m_secondLine = m_firstDone = false;
    emit measuresChanged();
    update();
}

QString SliceView::text(const Measure &m) const {
    return m_backend ? m_backend->measureText(m) : QString();
}

QStringList SliceView::measureTexts() const {
    QStringList out;
    for (const auto &m : measures())
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
            if (m.kind == 4 && (m_secondLine || !active))
                painter->drawLine(c, t.map(m.d));
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
    if ((m.kind == 3 && active && !m_secondArm) || (m.kind == 4 && active && !m_secondLine))
        return;
    if (m.kind == 4) {
        paintLabel(painter, t.map(m.d) + QPointF(12, -4), text(m));
        return;
    }
    QPointF at = m.kind == 2 ? QPointF(std::max({a.x(), b.x()}) + 8, std::min({a.y(), b.y()}) + 4)
                             : b + QPointF(12, -4);
    paintLabel(painter, at, text(m));
}

void SliceView::paint(QPainter *painter) {
    const QRectF r = imageRect();
    if (r.isEmpty())
        return;
    // Painted in software, from the picture the backend windows on the
    // CPU, which it makes here if nothing has asked for it since it changed.
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

    for (Backend *other : {m_reference.data(), m_secondReference.data()})
        if (QPointF ra, rb; lineTo(other, ra, rb)) {
            painter->setRenderHint(QPainter::Antialiasing);
            QColor color = m_accent;
            color.setAlpha(200);
            QPen pen(color, 1.5, Qt::DashLine);
            painter->setPen(pen);
            painter->drawLine(t.map(ra + Half), t.map(rb + Half));
        }
    // Where the planes meet, once a click has set it.
    if (QPointF at; m_backend->crossing(at)) {
        painter->setRenderHint(QPainter::Antialiasing);
        const QPointF c = t.map(at + Half);
        for (const QPen &pen : {QPen(QColor(0, 0, 0, 160), 4), QPen(m_accent, 2)}) {
            painter->setPen(pen);
            painter->drawLine(c - QPointF(9, 0), c - QPointF(3, 0));
            painter->drawLine(c + QPointF(3, 0), c + QPointF(9, 0));
            painter->drawLine(c - QPointF(0, 9), c - QPointF(0, 3));
            painter->drawLine(c + QPointF(0, 3), c + QPointF(0, 9));
        }
    }

    for (const auto &m : measures())
        paintMeasure(painter, t, m, false);
    if (m_drawing)
        paintMeasure(painter, t, m_current, true);
}

void SliceView::paintLabel(QPainter *painter, QPointF at, const QString &text) {
    QFont font = painter->font();
    font.setPixelSize(qRound(14 * m_textScale));
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
