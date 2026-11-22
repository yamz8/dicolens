#pragma once

#include <QString>
#include <QVariantMap>

#include "measure.h"
#include "pixels.h"
#include "planes.h"
#include "study.h"
#include "volumerender.h"

// What's written beside and over a picture, in the user's language and
// numbers: who and what it shows, the value under the pointer, and what a
// measurement comes to.
namespace Readout {

QString number(double value, int decimals);
// Which way a picture cuts the body. A cut the user tilted is oblique once
// it's off the body's axes, though a scanner's slices are named by the
// axis they're nearest.
QString planeName(const ImagePlane &plane, bool tilted);
// Such as "MIP 10 mm"; nothing for thin slices.
QString slabName(int slab, double millimetres);
// Nothing for grey.
QString colorMapName(int map);
// A DICOM date as the user's locale writes one.
QString date(const QString &dicomDate);

// The corners' text for a series as a pane cuts it. planeName and slabName
// are the pane's words for its cut, empty for the slices as scanned.
QVariantMap info(const Series &series, const Planes &planes, const QString &planeName, const QString &slabName);

// The place and value of one pixel: Hounsfield units on CT, an SUV on a
// PET with what that takes, the file's own numbers otherwise.
QString value(const Series &series, const Frame &frame, int x, int y);
// A PET's value at a patient point, to go after the picture's own, or
// nothing where the PET doesn't reach.
QString petValue(const VoxelGrid &pet, const double *point, bool suv);

// A ruler's length, an area with what's in it, or an angle; empty when it
// can't be measured.
QString measure(const Series &series, const Measure &m, const MeasureScale &scale, const Frame *frame);
QString angle(double degrees);

}
