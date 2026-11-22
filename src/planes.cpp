#include "planes.h"

#include <algorithm>
#include <cmath>

Frame Planes::Recipe::make(int index, const QVector<Frame> &slices) const {
    if (!built)
        return slices.value(index);
    if (plane == 3)
        return resample(volume, oblique, index, slices, low, high, half, projection);
    return reformat(volume, cut, index, slices, half, projection);
}

ImagePlane Planes::Recipe::where(int index) const {
    if (plane == 3)
        return oblique.plane(index);
    if (plane != 0)
        return cutPlane(volume, cut, index);
    return slicePlane(series, index);
}

void Planes::setSeries(const Series &series) {
    m_series = series;
    m_volume = makeVolume(series);
    m_plane = 0;
    m_slab = 0;
    m_cut = Cut();
    m_index = 0;
    // The planes meet in the middle of the first slice until a click says
    // where.
    m_cursor[0] = series.columns / 2;
    m_cursor[1] = series.rows / 2;
    m_cursor[2] = 0;
    m_crossingShown = false;
    m_oblique = Oblique();
    m_rangeKnown = false;
}

int Planes::count() const {
    if (m_plane == 3)
        return m_oblique.count;
    if (m_plane != 0)
        return m_cut.count(m_volume);
    return m_series.slices.size();
}

double Planes::pixelAspect() const {
    if (m_plane == 3)
        return 1.0;
    if (m_plane != 0)
        return m_volume.spacing[m_cut.horizontal] / m_volume.spacing[m_cut.vertical];
    return m_series.pixelAspect();
}

ImagePlane Planes::imagePlane() const {
    if (m_series.slices.isEmpty())
        return {};
    if (m_plane == 3)
        return m_oblique.plane(m_index);
    if (m_plane != 0)
        return cutPlane(m_volume, m_cut, m_index);
    return slicePlane(m_series, m_index);
}

QStringList Planes::edges() const {
    if (m_plane == 0)
        return m_series.edgeLabels();
    const ImagePlane p = imagePlane();
    return edgeLabels(p.row, p.column);
}

QString Planes::cutKey() const {
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

QSize Planes::size() const {
    if (m_plane == 3)
        return {m_oblique.width, m_oblique.height};
    if (m_plane != 0)
        return {m_cut.width(m_volume), m_cut.height(m_volume)};
    return {m_series.columns, m_series.rows};
}

MeasureScale Planes::scale() const {
    MeasureScale scale;
    scale.aspect = pixelAspect();
    if (m_plane == 3) {
        scale.even = true;
        scale.across = scale.down = m_oblique.spacing;
    } else if (m_plane != 0) {
        scale.even = true;
        scale.across = m_volume.spacing[m_cut.horizontal];
        scale.down = m_volume.spacing[m_cut.vertical];
    } else if (m_series.hasSpacing) {
        scale.even = true;
        scale.across = m_series.spacing[1];
        scale.down = m_series.spacing[0];
    } else {
        // Ultrasound calibrates the picture's areas, not the whole frame.
        scale.regions = m_series.regions;
    }
    return scale;
}

void Planes::crossingPoint(double *point) const {
    const double voxel[3] = {double(m_cursor[0]), double(m_cursor[1]), double(m_cursor[2])};
    toPatient(m_volume, voxel, point);
}

bool Planes::crossing(QPointF &at) const {
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

bool Planes::setPlane(int plane) {
    if (plane < 0 || plane > 2 || plane == m_plane || (plane != 0 && !m_volume.valid))
        return false;
    // The planes meet at one point, which stays where it is; a tilted
    // plane moved it as it went.
    if (m_plane == 0)
        m_cursor[2] = m_index;
    else if (m_plane != 3)
        m_cursor[m_cut.fixed] = m_index;
    m_plane = plane;
    // An axial stack's rows make coronal planes, and its columns sagittal.
    m_cut = cutAcross(m_volume, plane == 0 ? 2 : plane == 1 ? 1 : 0);
    m_index = std::clamp(m_cursor[m_cut.fixed], 0, std::max(0, count() - 1));
    return true;
}

bool Planes::setIndex(int index) {
    const int n = count();
    if (n == 0)
        return false;
    index = std::clamp(index, 0, n - 1);
    const bool moved = index != m_index;
    m_index = index;
    if (m_plane == 3) {
        // The point moves along with the plane.
        double point[3], voxel[3], normal[3];
        const ImagePlane p = m_oblique.plane(index);
        crossingPoint(point);
        p.normal(normal);
        const double off = (point[0] - p.origin[0]) * normal[0] + (point[1] - p.origin[1]) * normal[1]
                           + (point[2] - p.origin[2]) * normal[2];
        for (int i = 0; i < 3; ++i)
            point[i] -= off * normal[i];
        toVoxel(m_volume, point, voxel);
        for (int a = 0; a < 3; ++a)
            m_cursor[a] = std::clamp(int(std::lround(voxel[a])), 0, m_volume.size[a] - 1);
    } else if (m_plane != 0) {
        m_cursor[m_cut.fixed] = index;
    } else {
        m_cursor[2] = index;
    }
    return moved;
}

void Planes::nextThickness() {
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
}

void Planes::straighten() {
    m_plane = 0;
    m_cut = Cut();
    m_oblique = Oblique();
    m_index = std::clamp(m_cursor[2], 0, std::max(0, int(m_series.slices.size()) - 1));
}

bool Planes::goThrough(const double *p) {
    if (!m_volume.valid) {
        // A plain stack: the slice nearest the point.
        int best = -1;
        double nearest = 0;
        for (int i = 0; i < m_series.slices.size(); ++i) {
            const ImagePlane plane = slicePlane(m_series, i);
            if (!plane.valid)
                continue;
            double n[3];
            plane.normal(n);
            const double d = std::abs((p[0] - plane.origin[0]) * n[0] + (p[1] - plane.origin[1]) * n[1]
                                      + (p[2] - plane.origin[2]) * n[2]);
            if (best < 0 || d < nearest) {
                best = i;
                nearest = d;
            }
        }
        if (best >= 0)
            setIndex(best);
        return best >= 0;
    }
    double voxel[3];
    toVoxel(m_volume, p, voxel);
    for (int a = 0; a < 3; ++a)
        m_cursor[a] = std::clamp(int(std::lround(voxel[a])), 0, m_volume.size[a] - 1);
    m_crossingShown = true;
    // The point is kept as clicked: moving the picture along by its index
    // would round it to the plane.
    const int index = m_plane == 3 ? m_oblique.indexAt(p) : m_plane != 0 ? m_cursor[m_cut.fixed] : m_cursor[2];
    m_index = std::clamp(index, 0, std::max(0, count() - 1));
    return true;
}

bool Planes::becomeOblique(const double *center, const double *row, const double *column) {
    // Shown the usual way up, whichever way it was turned to face.
    const double n[3] = {row[1] * column[2] - row[2] * column[1], row[2] * column[0] - row[0] * column[2],
                         row[0] * column[1] - row[1] * column[0]};
    double r[3] = {row[0], row[1], row[2]}, c[3] = {column[0], column[1], column[2]};
    upright(n, r, c);
    const Oblique o = obliqueThrough(m_volume, center, r, c);
    if (!o.valid)
        return false;
    m_oblique = o;
    m_plane = 3;
    m_index = o.middle;
    return true;
}

bool Planes::tilt(int axis, double degrees) {
    const ImagePlane p = imagePlane();
    if (!m_volume.valid || !p.valid)
        return false;
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
    return becomeOblique(center, row, column);
}

void Planes::beginTurn() {
    const ImagePlane p = imagePlane();
    std::copy(p.row, p.row + 3, m_turnRow);
    std::copy(p.column, p.column + 3, m_turnColumn);
}

bool Planes::turn(const double *axis, double radians) {
    if (!m_volume.valid || !imagePlane().valid)
        return false;
    double row[3], column[3], center[3];
    std::copy(m_turnRow, m_turnRow + 3, row);
    std::copy(m_turnColumn, m_turnColumn + 3, column);
    turnAbout(axis, radians, row);
    turnAbout(axis, radians, column);
    crossingPoint(center);
    return becomeOblique(center, row, column);
}

void Planes::meetWhere(const Planes &other) {
    if (!m_volume.valid || !other.m_volume.valid)
        return;
    for (int a = 0; a < 3; ++a)
        m_cursor[a] = std::clamp(other.m_cursor[a], 0, m_volume.size[a] - 1);
    m_crossingShown = other.m_crossingShown;
    if (m_plane != 0 && m_plane != 3)
        m_index = std::clamp(m_cursor[m_cut.fixed], 0, std::max(0, count() - 1));
}

Planes::Recipe Planes::recipe(const QVector<Frame> &slices) {
    Recipe r;
    r.series = m_series;
    r.volume = m_volume;
    r.plane = m_plane;
    r.built = built();
    r.count = count();
    r.cut = m_plane != 0 && m_plane != 3 ? m_cut : cutAcross(m_volume, 2);
    double step = m_volume.spacing[r.cut.fixed];
    if (m_plane == 3) {
        if (!m_rangeKnown) {
            bool first = true;
            for (const Frame &f : slices) {
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
        r.oblique = m_oblique;
        r.low = m_low;
        r.high = m_high;
        step = m_oblique.step;
    }
    if (m_slab != 0) {
        r.half = std::max(1, int(std::lround(m_slabMm / step / 2)));
        r.projection = m_slab == 1 ? Projection::Max : m_slab == 2 ? Projection::Min : Projection::Mean;
    }
    return r;
}
