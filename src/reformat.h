#pragma once

#include <QPointF>
#include <QString>
#include <QVector>
#include <algorithm>

#include "pixels.h"
#include "study.h"

// Where an image lies in the patient: the centre of its top left pixel,
// the directions along its rows and down its columns, and how far apart
// its pixels are, in millimetres. Patient coordinates are DICOM's: x to
// the patient's left, y to the back, z to the head.
struct ImagePlane {
    bool valid = false;
    double origin[3] = {0, 0, 0};
    double row[3] = {1, 0, 0};
    double column[3] = {0, 1, 0};
    double rowSpacing = 1;       // Between pixels along a row.
    double columnSpacing = 1;    // Between pixels down a column.
    int width = 0;
    int height = 0;
    QString frame;               // Frame of Reference UID: planes compare only within one.

    void normal(double *n) const;
    // The patient point at an image position, in pixels from the top left
    // pixel's centre.
    void point(double x, double y, double *p) const;
};

// The plane of one slice as the scanner took it.
ImagePlane slicePlane(const Series &series, int slice);

// A series as a block of voxels: columns (i), rows (j), and slices (k),
// each with a direction and spacing. Only stacks of parallel, evenly spaced
// grey slices make one.
//
// A CT taken with its gantry tilted has such slices, but the table still
// moves straight through it, so each slice lies a little to the side of the
// one before, within its own plane: the stack leans. It is a block all the
// same. Its third axis is the slices' true normal, and each slice is a layer
// of it, moved within the layer by that slice's shift; the block is as much
// wider and taller as it takes to hold every slice where it lies. Voxel
// (i, j, k) is then what slice k has at (i, j) less its shift, between its
// pixels when the shift isn't a whole number of them, and nothing where that
// is off the slice. So planes, cuts, and lengths are as true in a leaning
// stack as in a straight one, and only what reads the slices' pixels has to
// know which it is.
struct Volume {
    bool valid = false;
    int size[3] = {0, 0, 0};
    double origin[3] = {0, 0, 0};
    double axis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double spacing[3] = {1, 1, 1};
    QString frame;
    // Each slice's shift within its layer, in voxels across and down, none
    // below zero. Empty for slices that lie straight behind one another,
    // where a voxel is simply a pixel.
    QVector<QPointF> shift;
    // The slices' own columns and rows, which a tilted stack's layers are
    // bigger than.
    int slice[2] = {0, 0};
    // What there is where no slice reaches, in the series' values: air on a
    // CT, and no signal on anything else.
    double empty = 0;

    bool tilted() const { return !shift.isEmpty(); }
    int columns() const { return tilted() ? slice[0] : size[0]; }
    int rows() const { return tilted() ? slice[1] : size[1]; }
};

// A stack is tilted when its slices step evenly to the side, each within a
// pixel of the line through the first and the last, and no steeper than a
// gantry leans (35 degrees, a little over what scanners do). One that
// wanders, or leans further, is more likely damaged than tilted, and makes
// no volume.
Volume makeVolume(const Series &series);

// A cut through a volume across one of its axes, laid out on screen as
// radiologists expect: head up, the patient's right on the left, the front
// on the left of a sagittal.
struct Cut {
    int fixed = 2;
    int horizontal = 0;
    int vertical = 1;
    bool flipHorizontal = false;
    bool flipVertical = false;

    // Across the slices themselves (fixed 2) the pictures are the slices as
    // they were scanned, each in its own pixels, which a tilted stack's
    // layers are bigger than.
    int along(const Volume &v, int axis) const { return fixed == 2 && axis < 2 && v.tilted() ? v.slice[axis] : v.size[axis]; }
    int width(const Volume &v) const { return along(v, horizontal); }
    int height(const Volume &v) const { return along(v, vertical); }
    int count(const Volume &v) const { return v.size[fixed]; }
};

Cut cutAcross(const Volume &volume, int fixedAxis);

// How a slab of slices becomes one picture: its brightest value at each
// point (a MIP, which picks out vessels and nodules), its darkest (a MinIP,
// for airways), or their average (a thicker slice, with less noise).
enum class Projection { Max, Min, Mean };

// The picture of one cut, from the series' decoded slices, or of the slab
// reaching half slices either side of it; null unless every slice is there.
// A tilted stack's slab is taken square to the cut all the same, and where
// none of it reaches a slice the picture has the slices' padding, or
// failing that the volume's empty value.
Frame reformat(const Volume &volume, const Cut &cut, int index, const QVector<Frame> &slices, int half = 0,
               Projection projection = Projection::Max);

// Where reformat's picture of that cut lies. Across a tilted stack's slices
// that is the slice's own place, as slicePlane has it, not the whole layer's.
ImagePlane cutPlane(const Volume &volume, const Cut &cut, int index);

// "Axial", "Coronal", "Sagittal", or "Oblique", in English, by which way
// the plane faces.
const char *planeName(const ImagePlane &plane);

// A plane at any angle through a volume, and the stack of planes parallel
// to it that cross the whole volume, a step apart.
struct Oblique {
    bool valid = false;
    double center[3] = {0, 0, 0};   // A point the middle plane goes through.
    double row[3] = {1, 0, 0};
    double column[3] = {0, 1, 0};
    double normal[3] = {0, 0, 1};
    double spacing = 1;             // Between pixels, both ways.
    double step = 1;                // Between planes.
    double left = 0, top = 0;       // The picture's corner, from the centre, in mm.
    int width = 0;
    int height = 0;
    int count = 0;
    int middle = 0;                 // The plane through the centre.
    QString frame;

    ImagePlane plane(int index) const;
    // The plane nearest a point.
    int indexAt(const double *point) const;
};

// The planes along row and column (made square to each other) through a
// point, sized to take in the whole volume.
Oblique obliqueThrough(const Volume &volume, const double *point, const double *row, const double *column);

// One oblique picture, or the slab half planes either side of it,
// interpolated between voxels. low and high are the volume's value range;
// outside it is low.
Frame resample(const Volume &volume, const Oblique &oblique, int index, const QVector<Frame> &slices, double low,
               double high, int half = 0, Projection projection = Projection::Max);

// From patient millimetres to the volume's voxel indices, and back.
void toVoxel(const Volume &volume, const double *point, double *voxel);
void toPatient(const Volume &volume, const double *voxel, double *point);

// A grey picture's stored value at a place between its pixels (x across and
// y down from the top left pixel's centre), the four around it weighed by
// how near each is. False off the picture, more than half a pixel past its
// outermost centres: there is nothing there, and the nearest pixel's value
// isn't smeared out to it. Padding marks where the scan isn't, and is no
// value to mix with one, so next to it the nearest pixel is taken whole.
// This is how a tilted stack's voxel (i, j, k) is read: from slice k, at
// (i, j) less volume.shift[k].
inline bool sampleSlice(const Frame &frame, double x, double y, double &stored) {
    const int w = frame.width, h = frame.height;
    if (!(x >= -0.5 && y >= -0.5 && x <= w - 0.5 && y <= h - 0.5) || frame.mono.size() < qsizetype(w) * h)
        return false;
    const double cx = std::clamp(x, 0.0, w - 1.0), cy = std::clamp(y, 0.0, h - 1.0);
    const int x0 = int(cx), y0 = int(cy);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const double fx = cx - x0, fy = cy - y0;
    const quint16 *top = frame.mono.constData() + qsizetype(y0) * w, *bottom = frame.mono.constData() + qsizetype(y1) * w;
    // Signed values mix as numbers, not as bit patterns.
    const bool isSigned = frame.isSigned;
    const int a = isSigned ? qint16(top[x0]) : top[x0], b = isSigned ? qint16(top[x1]) : top[x1];
    const int c = isSigned ? qint16(bottom[x0]) : bottom[x0], d = isSigned ? qint16(bottom[x1]) : bottom[x1];
    if (frame.hasPadding && (a == frame.padding || b == frame.padding || c == frame.padding || d == frame.padding)) {
        stored = fy < 0.5 ? (fx < 0.5 ? a : b) : (fx < 0.5 ? c : d);
        return true;
    }
    stored = (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
    return true;
}

// The row and column a plane facing along normal is shown with, as
// radiologists lay pictures out: nearer axial, the back down and the
// patient's right on the left; otherwise the head up, and the patient's
// right, or front, on the left.
void upright(const double *normal, double *row, double *column);

// Turns a vector about an axis, right-handed, by an angle in radians.
void turnAbout(const double *axis, double angle, double *v);
