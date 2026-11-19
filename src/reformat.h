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
