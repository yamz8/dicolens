#pragma once

#include <QList>
#include <QPointF>

#include "pixels.h"
#include "study.h"

// A ruler, area, angle, or Cobb angle drawn on a picture.
struct Measure {
    int kind = 1;            // 1 ruler, 2 area, 3 angle, 4 Cobb angle.
    QPointF a, b, c, d;      // Image pixels; c is an angle's far end,
                             // and c to d a Cobb angle's second line.
};

// How big a picture's pixels are. A CT or MR slice, and any plane cut from
// one, has one spacing throughout; ultrasound calibrates only the areas of
// the picture its scanner drew, each its own way.
struct MeasureScale {
    bool even = false;          // The spacing below holds everywhere.
    double across = 1, down = 1; // Millimetres a pixel.
    QList<Region> regions;
    double aspect = 1;          // How much wider a pixel is than tall.

    // The spacing at a point, if it's known there.
    bool at(double x, double y, double &across, double &down) const;
};

// What a measurement comes to, in numbers. A length or area is in
// millimetres when the picture's scale is known where it was drawn, and in
// pixels otherwise.
struct MeasureValues {
    int kind = 1;
    bool millimetres = false;
    double length = 0;          // A ruler.
    double area = 0;            // An area: mm² or px².
    qint64 count = 0;           // The pixels inside it, and their values.
    double mean = 0, sd = 0, low = 0, high = 0;
    double angle = 0;           // An angle or Cobb angle, in degrees.
    bool valid = false;
};

// One function for the window and the command line, so they agree. The
// frame is the picture's values, for an area's; scale turns stored values
// into shown ones (a PET's into SUVs).
MeasureValues measureValues(const Measure &m, const MeasureScale &scale, const Frame *frame = nullptr,
                            double valueScale = 1);
