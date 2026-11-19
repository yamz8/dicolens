#include "measure.h"

#include <QRectF>
#include <algorithm>
#include <cmath>

bool MeasureScale::at(double x, double y, double &a, double &d) const {
    if (even) {
        a = across;
        d = down;
        return true;
    }
    for (const Region &r : regions)
        if (r.contains(x, y)) {
            a = r.deltaX;
            d = r.deltaY;
            return true;
        }
    return false;
}

MeasureValues measureValues(const Measure &m, const MeasureScale &scale, const Frame *frame, double valueScale) {
    MeasureValues v;
    v.kind = m.kind;
    if (m.kind == 2) {
        const QRectF box = QRectF(m.a, m.b).normalized();
        if (box.width() <= 0 || box.height() <= 0)
            return v;
        const double cx = box.center().x(), cy = box.center().y(), rx = box.width() / 2, ry = box.height() / 2;
        double across, down;
        v.millimetres = scale.at(cx, cy, across, down);
        v.area = M_PI * rx * ry * (v.millimetres ? across * down : 1);
        v.valid = true;
        if (!frame || frame->isNull() || frame->isColor() || !frame->error.isEmpty())
            return v;
        // Pixels whose centres fall inside the ellipse.
        double sum = 0, squares = 0;
        const int x0 = std::max(0, int(std::floor(box.left()))), x1 = std::min(frame->width - 1, int(std::ceil(box.right())));
        const int y0 = std::max(0, int(std::floor(box.top()))), y1 = std::min(frame->height - 1, int(std::ceil(box.bottom())));
        for (int j = y0; j <= y1; ++j)
            for (int i = x0; i <= x1; ++i) {
                const double dx = (i + 0.5 - cx) / rx, dy = (j + 0.5 - cy) / ry;
                if (dx * dx + dy * dy > 1)
                    continue;
                const double value = frame->value(i, j) * valueScale;
                if (v.count == 0)
                    v.low = v.high = value;
                v.low = std::min(v.low, value);
                v.high = std::max(v.high, value);
                sum += value;
                squares += value * value;
                ++v.count;
            }
        if (v.count > 0) {
            v.mean = sum / v.count;
            v.sd = std::sqrt(std::max(0.0, squares / v.count - v.mean * v.mean));
        }
        return v;
    }
    if (m.kind == 3 || m.kind == 4) {
        // Pixels needn't be square: the angle is the one in the body.
        const QPointF u = m.kind == 3 ? m.a - m.b : m.b - m.a, w = m.kind == 3 ? m.c - m.b : m.d - m.c;
        const double ux = u.x() * scale.aspect, wx = w.x() * scale.aspect;
        const double lu = std::hypot(ux, u.y()), lw = std::hypot(wx, w.y());
        if (lu < 1e-9 || lw < 1e-9)
            return v;
        double cosine = std::clamp((ux * wx + u.y() * w.y()) / (lu * lw), -1.0, 1.0);
        // Between two lines, whichever way each was drawn: at most 90°.
        if (m.kind == 4)
            cosine = std::abs(cosine);
        v.angle = std::acos(cosine) * 180 / M_PI;
        v.valid = true;
        return v;
    }
    // A ruler. On ultrasound, a line inside one calibrated area has a
    // length; across areas, or outside them, only pixels.
    const double dx = m.b.x() - m.a.x(), dy = m.b.y() - m.a.y();
    v.valid = true;
    if (scale.even) {
        v.millimetres = true;
        v.length = std::hypot(dx * scale.across, dy * scale.down);
        return v;
    }
    for (const Region &r : scale.regions)
        if (r.contains(m.a.x(), m.a.y()) && r.contains(m.b.x(), m.b.y())) {
            v.millimetres = true;
            v.length = std::hypot(dx * r.deltaX, dy * r.deltaY);
            return v;
        }
    v.length = std::hypot(dx, dy);
    return v;
}
