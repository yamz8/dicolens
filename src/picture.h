#pragma once

#include <QImage>
#include <QSize>
#include <QStringList>
#include <QTransform>
#include <memory>

#include "pixels.h"
#include "reformat.h"
#include "volumerender.h"

// Everything the picture on screen is made from, before any of it is made
// into colours: the slice's values as the file stored them, the window they
// are seen through, and the PET that goes over them. A shader can draw from
// these alone; render() is the same picture made on the CPU, for the view
// that paints in software, for saving, and for copying.
struct Picture {
    // Grey: frame.mono holds width × height stored values, 16 bits each,
    // signed when frame.isSigned, and a value means stored × frame.slope +
    // frame.intercept; frame.inverted is a file whose low values are white.
    // Colour: frame.color is the picture, and the window doesn't apply.
    Frame frame;
    double center = 0;
    double width = 1;
    bool inverted = false;
    // 0 grey; otherwise a colour scale, as colorScale() numbers them.
    int colorMap = 0;
    // A PET over it: its voxels, how they're coloured, and where this
    // picture lies in the patient, which says which voxels fall on it.
    std::shared_ptr<const VoxelGrid> pet;
    Overlay3D petLook;
    ImagePlane plane;

    bool isNull() const { return frame.isNull() || !frame.error.isEmpty(); }
    QSize size() const { return isNull() ? QSize() : QSize(frame.width, frame.height); }
    QImage render() const;
};

// An 8-bit grey picture through a colour scale.
QImage colorize(const QImage &grey, int map);

// From a picture's pixels to the picture turned by quarter turns clockwise,
// after a flip left to right when flipped.
QTransform turned(const QSizeF &size, int rotation, bool flipped);

// How a pane turns its picture: quarter turns clockwise, after a flip
// left to right when flipped.
struct Turn {
    int rotation = 0;
    bool flipped = false;

    bool isNone() const { return rotation == 0 && !flipped; }
    void rotate(int quarterTurns);
    void flipHorizontal() { flipped = !flipped; }
    void flipVertical();
    // The patient directions at the left, right, top, and bottom edges,
    // given those of the picture before it was turned.
    QStringList edges(QStringList unturned) const;
    QTransform transform(const QSizeF &size) const { return turned(size, rotation, flipped); }
};

// A picture stretched so its pixels are as wide as they are tall on screen.
QImage trueShape(const QImage &image, double aspect);

// As it's saved and copied: at its true shape, turned as it's shown.
QImage asShown(const QImage &image, double aspect, const Turn &turn);
