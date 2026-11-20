#include "reformat.h"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>

namespace {

double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void cross(const double *a, const double *b, double *out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

int strongest(const double *v) {
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(v[i]) > std::abs(v[best]))
            best = i;
    return best;
}

// Which way a direction should run on screen: to the patient's left and
// back, and down towards the feet, as images are read.
bool runsBackwards(const double *v) {
    const int axis = strongest(v);
    return axis == 2 ? v[axis] > 0 : v[axis] < 0;
}

}

void ImagePlane::normal(double *n) const {
    cross(row, column, n);
}

void ImagePlane::point(double x, double y, double *p) const {
    for (int a = 0; a < 3; ++a)
        p[a] = origin[a] + x * rowSpacing * row[a] + y * columnSpacing * column[a];
}

ImagePlane slicePlane(const Series &series, int slice) {
    ImagePlane plane;
    if (slice < 0 || slice >= series.slices.size())
        return plane;
    const Slice &s = series.slices[slice];
    if (!series.hasOrientation || !s.hasPosition)
        return plane;
    plane.valid = true;
    for (int a = 0; a < 3; ++a) {
        plane.origin[a] = s.position[a];
        plane.row[a] = series.orientation[a];
        plane.column[a] = series.orientation[a + 3];
    }
    // Without a spacing, a pixel is taken as a millimetre, which still
    // places it, if not to scale.
    plane.rowSpacing = series.spacing[1];
    plane.columnSpacing = series.spacing[0];
    plane.width = series.columns;
    plane.height = series.rows;
    plane.frame = series.frame;
    return plane;
}

Volume makeVolume(const Series &series) {
    Volume v;
    const int n = series.slices.size();
    if (n < 3 || series.color || !series.hasOrientation || !series.hasSpacing)
        return v;
    for (const auto &s : series.slices)
        if (!s.hasPosition)
            return v;
    double normal[3];
    cross(series.orientation, series.orientation + 3, normal);

    // Slices a fixed step apart along the normal; a stack with gaps, or of
    // differently placed pieces, would come out distorted.
    QVector<double> along(n);
    for (int k = 0; k < n; ++k)
        along[k] = dot(series.slices[k].position, normal);
    const double step = (along[n - 1] - along[0]) / (n - 1);
    if (!std::isfinite(step) || std::abs(step) < 0.01)
        return v;
    for (int k = 1; k < n; ++k)
        if (std::abs((along[k] - along[k - 1]) - step) > std::abs(step) * 0.1)
            return v;
    // And one straight behind another: a CT taken with its gantry tilted
    // steps sideways from slice to slice, and cut as a block it would come
    // out sheared, with lengths through it too short.
    const double pixel = std::max(series.spacing[0], series.spacing[1]);
    for (int k = 1; k < n; ++k) {
        double aside = 0;
        for (int a = 0; a < 3; ++a) {
            const double d = series.slices[k].position[a] - series.slices[0].position[a] - (along[k] - along[0]) * normal[a];
            aside += d * d;
        }
        if (std::sqrt(aside) > pixel)
            return v;
    }

    v.valid = true;
    v.size[0] = series.columns;
    v.size[1] = series.rows;
    v.size[2] = n;
    for (int a = 0; a < 3; ++a) {
        v.origin[a] = series.slices[0].position[a];
        v.axis[0][a] = series.orientation[a];
        v.axis[1][a] = series.orientation[a + 3];
        v.axis[2][a] = normal[a] * (step > 0 ? 1 : -1);
    }
    v.spacing[0] = series.spacing[1];
    v.spacing[1] = series.spacing[0];
    v.spacing[2] = std::abs(step);
    v.frame = series.frame;
    return v;
}

Cut cutAcross(const Volume &volume, int fixedAxis) {
    Cut cut;
    cut.fixed = fixedAxis;
    // As acquired: as the scanner laid it out.
    if (fixedAxis == 2)
        return cut;
    int free[2], f = 0;
    for (int a = 0; a < 3; ++a)
        if (a != fixedAxis)
            free[f++] = a;
    // Across goes left to right on axial and coronal planes, and front to
    // back on a sagittal one.
    const int across = strongest(volume.axis[fixedAxis]) == 0 ? 1 : 0;
    const bool firstAcross = std::abs(volume.axis[free[0]][across]) >= std::abs(volume.axis[free[1]][across]);
    cut.horizontal = firstAcross ? free[0] : free[1];
    cut.vertical = firstAcross ? free[1] : free[0];
    cut.flipHorizontal = runsBackwards(volume.axis[cut.horizontal]);
    cut.flipVertical = runsBackwards(volume.axis[cut.vertical]);
    return cut;
}

Frame reformat(const Volume &volume, const Cut &cut, int index, const QVector<Frame> &slices, int half,
               Projection projection) {
    Frame out;
    if (!volume.valid || index < 0 || index >= cut.count(volume) || slices.size() != volume.size[2])
        return out;
    const int columns = volume.size[0], rows = volume.size[1];
    bool sameScale = true;
    for (const auto &f : slices) {
        if (f.isNull() || f.isColor() || !f.error.isEmpty() || f.width != columns || f.height != rows)
            return out;
        sameScale = sameScale && f.slope == slices[0].slope && f.intercept == slices[0].intercept
                    && f.isSigned == slices[0].isSigned;
    }

    const int width = cut.width(volume), height = cut.height(volume);
    out.width = width;
    out.height = height;
    out.mono.resize(qsizetype(width) * height);
    out.inverted = slices[0].inverted;
    out.windowCenter = slices[0].windowCenter;
    out.windowWidth = slices[0].windowWidth;
    out.isSigned = slices[0].isSigned;
    out.slope = slices[0].slope;
    out.intercept = slices[0].intercept;
    const int first = std::max(0, index - half), last = std::min(cut.count(volume) - 1, index + half);
    // Padding outside the scan would win every MIP; it stays only in one
    // slice, or a MinIP of one scale, where it's still what it was.
    out.hasPadding = slices[0].hasPadding && (first == last || (sameScale && projection == Projection::Min));
    out.padding = slices[0].padding;

    // Slices rescaled differently (PET, often), and averages, which fall
    // between stored values, are brought to one fine scale of their own.
    const bool rescale = !sameScale || (projection == Projection::Mean && first != last);
    double low = 0;
    double slope = 1;
    if (rescale) {
        double high = 0;
        slices[0].range(low, high);
        for (const auto &f : slices) {
            double a, b;
            f.range(a, b);
            low = std::min(low, a);
            high = std::max(high, b);
        }
        slope = std::max(1e-9, (high - low) / 65535);
        out.slope = slope;
        out.intercept = low;
        out.isSigned = false;
    }

    int at[3];
    for (int y = 0; y < height; ++y) {
        at[cut.vertical] = cut.flipVertical ? height - 1 - y : y;
        for (int x = 0; x < width; ++x) {
            at[cut.horizontal] = cut.flipHorizontal ? width - 1 - x : x;
            double pick = 0;
            int stored = 0;
            for (int k = first; k <= last; ++k) {
                at[cut.fixed] = k;
                const Frame &f = slices[at[2]];
                if (!rescale && first == last) {
                    stored = f.mono[qsizetype(at[1]) * columns + at[0]];
                    break;
                }
                // Signed values compare as numbers, not as bit patterns.
                const double v = rescale ? f.value(at[0], at[1]) : f.stored(at[0], at[1]);
                if (k == first)
                    pick = v;
                else if (projection == Projection::Max)
                    pick = std::max(pick, v);
                else if (projection == Projection::Min)
                    pick = std::min(pick, v);
                else
                    pick += v;
            }
            if (first != last || rescale) {
                if (projection == Projection::Mean)
                    pick /= last - first + 1;
                stored = rescale ? int(std::lround((pick - low) / slope)) : int(pick);
            }
            out.mono[qsizetype(y) * width + x] = quint16(stored);
        }
    }
    return out;
}

ImagePlane cutPlane(const Volume &volume, const Cut &cut, int index) {
    ImagePlane plane;
    if (!volume.valid)
        return plane;
    plane.valid = true;
    plane.width = cut.width(volume);
    plane.height = cut.height(volume);
    plane.frame = volume.frame;
    int at[3];
    at[cut.fixed] = index;
    at[cut.horizontal] = cut.flipHorizontal ? plane.width - 1 : 0;
    at[cut.vertical] = cut.flipVertical ? plane.height - 1 : 0;
    for (int a = 0; a < 3; ++a) {
        plane.origin[a] = volume.origin[a];
        for (int axis = 0; axis < 3; ++axis)
            plane.origin[a] += at[axis] * volume.spacing[axis] * volume.axis[axis][a];
        plane.row[a] = volume.axis[cut.horizontal][a] * (cut.flipHorizontal ? -1 : 1);
        plane.column[a] = volume.axis[cut.vertical][a] * (cut.flipVertical ? -1 : 1);
    }
    plane.rowSpacing = volume.spacing[cut.horizontal];
    plane.columnSpacing = volume.spacing[cut.vertical];
    return plane;
}

const char *planeName(const ImagePlane &plane) {
    double n[3];
    plane.normal(n);
    const int axis = strongest(n);
    if (std::abs(n[axis]) < 0.8)
        return QT_TRANSLATE_NOOP("dicolens", "Oblique");
    if (axis == 0)
        return QT_TRANSLATE_NOOP("dicolens", "Sagittal");
    if (axis == 1)
        return QT_TRANSLATE_NOOP("dicolens", "Coronal");
    return QT_TRANSLATE_NOOP("dicolens", "Axial");
}

void toVoxel(const Volume &volume, const double *point, double *voxel) {
    for (int a = 0; a < 3; ++a)
        voxel[a] = dot(volume.axis[a], point) / volume.spacing[a] - dot(volume.axis[a], volume.origin) / volume.spacing[a];
}

void toPatient(const Volume &volume, const double *voxel, double *point) {
    for (int i = 0; i < 3; ++i) {
        point[i] = volume.origin[i];
        for (int a = 0; a < 3; ++a)
            point[i] += voxel[a] * volume.spacing[a] * volume.axis[a][i];
    }
}

void upright(const double *normal, double *row, double *column) {
    double n[3] = {normal[0], normal[1], normal[2]};
    const double length = std::sqrt(dot(n, n));
    if (length < 1e-12)
        return;
    for (double &x : n)
        x /= length;
    // Down the picture: toward the back, or toward the feet.
    const bool axial = std::abs(n[2]) >= std::sqrt(0.5);
    const double down[3] = {0, axial ? 1.0 : 0.0, axial ? 0.0 : -1.0};
    const double along = dot(down, n);
    double c[3];
    for (int i = 0; i < 3; ++i)
        c[i] = down[i] - along * n[i];
    const double cl = std::sqrt(dot(c, c));
    if (cl < 1e-9)
        return;
    for (double &x : c)
        x /= cl;
    // Across: toward the patient's left, or the back, whichever it's
    // nearer.
    double r[3];
    cross(c, n, r);
    const int lead = std::abs(r[0]) >= std::abs(r[1]) ? 0 : 1;
    if (r[lead] < 0)
        for (double &x : r)
            x = -x;
    for (int i = 0; i < 3; ++i) {
        row[i] = r[i];
        column[i] = c[i];
    }
}

void turnAbout(const double *axis, double angle, double *v) {
    // Rodrigues' rotation formula.
    const double length = std::sqrt(dot(axis, axis));
    if (length < 1e-12)
        return;
    const double k[3] = {axis[0] / length, axis[1] / length, axis[2] / length};
    double kv[3];
    cross(k, v, kv);
    const double c = std::cos(angle), s = std::sin(angle), along = dot(k, v) * (1 - c);
    for (int i = 0; i < 3; ++i)
        v[i] = v[i] * c + kv[i] * s + k[i] * along;
}

ImagePlane Oblique::plane(int index) const {
    ImagePlane p;
    if (!valid)
        return p;
    p.valid = true;
    p.width = width;
    p.height = height;
    p.rowSpacing = p.columnSpacing = spacing;
    p.frame = frame;
    const double along = (index - middle) * step;
    for (int i = 0; i < 3; ++i) {
        p.origin[i] = center[i] + left * row[i] + top * column[i] + along * normal[i];
        p.row[i] = row[i];
        p.column[i] = column[i];
    }
    return p;
}

int Oblique::indexAt(const double *point) const {
    double d[3] = {point[0] - center[0], point[1] - center[1], point[2] - center[2]};
    return std::clamp(middle + int(std::lround(dot(d, normal) / step)), 0, std::max(0, count - 1));
}

Oblique obliqueThrough(const Volume &volume, const double *point, const double *row, const double *column) {
    Oblique o;
    if (!volume.valid)
        return o;
    double r[3] = {row[0], row[1], row[2]};
    const double rl = std::sqrt(dot(r, r));
    if (rl < 1e-9)
        return o;
    for (double &x : r)
        x /= rl;
    // The column, made square to the row.
    double c[3], along = dot(column, r);
    for (int i = 0; i < 3; ++i)
        c[i] = column[i] - along * r[i];
    const double cl = std::sqrt(dot(c, c));
    if (cl < 1e-9)
        return o;
    for (double &x : c)
        x /= cl;
    double n[3];
    cross(r, c, n);
    for (int i = 0; i < 3; ++i) {
        o.center[i] = point[i];
        o.row[i] = r[i];
        o.column[i] = c[i];
        o.normal[i] = n[i];
    }
    // As fine as the volume's finest, and a step its voxels' depth along
    // the normal.
    o.spacing = std::min({volume.spacing[0], volume.spacing[1], volume.spacing[2]});
    o.step = 0;
    for (int a = 0; a < 3; ++a)
        o.step += std::abs(dot(n, volume.axis[a])) * volume.spacing[a];
    o.step = std::max(o.step, o.spacing);

    // Whatever of the volume the planes can reach: its corners, seen along
    // each direction from the centre.
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (int corner = 0; corner < 8; ++corner) {
        double voxel[3], p[3];
        for (int a = 0; a < 3; ++a)
            voxel[a] = (corner >> a & 1) ? volume.size[a] - 0.5 : -0.5;
        toPatient(volume, voxel, p);
        double d[3] = {p[0] - point[0], p[1] - point[1], p[2] - point[2]};
        const double along[3] = {dot(d, r), dot(d, c), dot(d, n)};
        for (int i = 0; i < 3; ++i) {
            lo[i] = std::min(lo[i], along[i]);
            hi[i] = std::max(hi[i], along[i]);
        }
    }
    // A picture as big as 1024 pixels a side at most.
    o.spacing = std::max({o.spacing, (hi[0] - lo[0]) / 1024, (hi[1] - lo[1]) / 1024});
    // Pixel centres half a pixel in from the edges, so a plane along the
    // volume's own axes lands on its voxels.
    o.left = lo[0] + o.spacing / 2;
    o.top = lo[1] + o.spacing / 2;
    o.width = std::max(1, int(std::round((hi[0] - lo[0]) / o.spacing)));
    o.height = std::max(1, int(std::round((hi[1] - lo[1]) / o.spacing)));
    o.middle = std::max(0, int(std::ceil(-lo[2] / o.step)));
    o.count = o.middle + std::max(0, int(std::floor(hi[2] / o.step))) + 1;
    o.frame = volume.frame;
    o.valid = true;
    return o;
}

Frame resample(const Volume &volume, const Oblique &oblique, int index, const QVector<Frame> &slices, double low,
               double high, int half, Projection projection) {
    Frame out;
    if (!volume.valid || !oblique.valid || index < 0 || index >= oblique.count || slices.size() != volume.size[2])
        return out;
    const int columns = volume.size[0], rows = volume.size[1], depth = volume.size[2];
    for (const auto &f : slices)
        if (f.isNull() || f.isColor() || !f.error.isEmpty() || f.width != columns || f.height != rows)
            return out;
    out.width = oblique.width;
    out.height = oblique.height;
    out.mono.resize(out.width * out.height);
    out.inverted = slices[0].inverted;
    out.windowCenter = slices[0].windowCenter;
    out.windowWidth = slices[0].windowWidth;
    const double slope = std::max(1e-9, (high - low) / 65535);
    out.slope = slope;
    out.intercept = low;

    // A voxel's value, in between them weighed by how near each is.
    auto sample = [&](const double *v) {
        if (v[0] < -0.5 || v[1] < -0.5 || v[2] < -0.5 || v[0] > columns - 0.5 || v[1] > rows - 0.5 || v[2] > depth - 0.5)
            return low;
        const double x = std::clamp(v[0], 0.0, columns - 1.0), y = std::clamp(v[1], 0.0, rows - 1.0),
                     z = std::clamp(v[2], 0.0, depth - 1.0);
        const int x0 = std::min(int(x), columns - 1), y0 = std::min(int(y), rows - 1), z0 = std::min(int(z), depth - 1);
        const int x1 = std::min(x0 + 1, columns - 1), y1 = std::min(y0 + 1, rows - 1), z1 = std::min(z0 + 1, depth - 1);
        const double fx = x - x0, fy = y - y0, fz = z - z0;
        auto at = [&](int k, int j, int i) { return slices[k].value(i, j); };
        const double a = at(z0, y0, x0) * (1 - fx) + at(z0, y0, x1) * fx;
        const double b = at(z0, y1, x0) * (1 - fx) + at(z0, y1, x1) * fx;
        const double c = at(z1, y0, x0) * (1 - fx) + at(z1, y0, x1) * fx;
        const double d = at(z1, y1, x0) * (1 - fx) + at(z1, y1, x1) * fx;
        return ((a * (1 - fy) + b * fy) * (1 - fz) + (c * (1 - fy) + d * fy) * fz);
    };

    // Stepping through voxel space directly: each pixel across, down, and
    // plane along is a fixed move in it.
    double origin[3], across[3], down[3], deeper[3];
    const ImagePlane p = oblique.plane(index);
    toVoxel(volume, p.origin, origin);
    for (int a = 0; a < 3; ++a) {
        across[a] = dot(volume.axis[a], oblique.row) * oblique.spacing / volume.spacing[a];
        down[a] = dot(volume.axis[a], oblique.column) * oblique.spacing / volume.spacing[a];
        deeper[a] = dot(volume.axis[a], oblique.normal) * oblique.step / volume.spacing[a];
    }
    const int first = std::max(-half, -index), last = std::min(half, oblique.count - 1 - index);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            double pick = 0;
            for (int k = first; k <= last; ++k) {
                double v[3];
                for (int a = 0; a < 3; ++a)
                    v[a] = origin[a] + x * across[a] + y * down[a] + k * deeper[a];
                const double value = sample(v);
                if (k == first)
                    pick = value;
                else if (projection == Projection::Max)
                    pick = std::max(pick, value);
                else if (projection == Projection::Min)
                    pick = std::min(pick, value);
                else
                    pick += value;
            }
            if (projection == Projection::Mean)
                pick /= last - first + 1;
            out.mono[y * out.width + x] = quint16(std::clamp(std::lround((pick - low) / slope), 0L, 65535L));
        }
    return out;
}
