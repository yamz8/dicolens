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
    if (std::abs(step) < 0.01)
        return v;
    for (int k = 1; k < n; ++k)
        if (std::abs((along[k] - along[k - 1]) - step) > std::abs(step) * 0.1)
            return v;

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
    out.mono.resize(width * height);
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
                    stored = f.mono[at[1] * columns + at[0]];
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
            out.mono[y * width + x] = quint16(stored);
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
