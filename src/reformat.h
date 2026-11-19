#pragma once

#include <QString>
#include <QVector>

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
struct Volume {
    bool valid = false;
    int size[3] = {0, 0, 0};
    double origin[3] = {0, 0, 0};
    double axis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double spacing[3] = {1, 1, 1};
    QString frame;
};

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

    int width(const Volume &v) const { return v.size[horizontal]; }
    int height(const Volume &v) const { return v.size[vertical]; }
    int count(const Volume &v) const { return v.size[fixed]; }
};

Cut cutAcross(const Volume &volume, int fixedAxis);

// How a slab of slices becomes one picture: its brightest value at each
// point (a MIP, which picks out vessels and nodules), its darkest (a MinIP,
// for airways), or their average (a thicker slice, with less noise).
enum class Projection { Max, Min, Mean };

// The picture of one cut, from the series' decoded slices, or of the slab
// reaching half slices either side of it; null unless every slice is there.
Frame reformat(const Volume &volume, const Cut &cut, int index, const QVector<Frame> &slices, int half = 0,
               Projection projection = Projection::Max);

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

// The row and column a plane facing along normal is shown with, as
// radiologists lay pictures out: nearer axial, the back down and the
// patient's right on the left; otherwise the head up, and the patient's
// right, or front, on the left.
void upright(const double *normal, double *row, double *column);

// Turns a vector about an axis, right-handed, by an angle in radians.
void turnAbout(const double *axis, double angle, double *v);
