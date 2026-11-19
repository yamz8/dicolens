#pragma once

#include <QImage>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

#include "dicom.h"

// A mark someone left on an image: an outline, an arrow's line, a point, or
// a note, from a presentation state (GSPS) saved beside the images.
struct Mark {
    enum class Kind { Polyline, Points, Circle, Ellipse, Text };

    Kind kind = Kind::Polyline;
    // Image pixels, with 0,0 the top left corner of the top left pixel, or
    // fractions of the image when display is set.
    QList<QPointF> points;
    bool display = false;
    bool filled = false;

    QString text;
    bool hasBox = false;
    QRectF box;
    bool boxDisplay = false;
    bool hasAnchor = false;
    QPointF anchor;
    bool anchorDisplay = false;
    bool anchorLine = false;

    // The images it belongs to, by SOP Instance UID; frame 0 means every
    // frame of that image.
    QList<QPair<QString, int>> images;

    bool appliesTo(const QString &sop, int frame) const;
};

namespace Marks {

bool isPresentationState(const DicomFile &file);

// The marks a presentation state draws.
QList<Mark> read(const DicomFile &file);

// The overlay planes a file carries for one frame, as an alpha mask the size
// of the image: set where any overlay has a bit. Null when there are none.
QImage overlay(const DicomFile &file, int frame, int rows, int columns);

}
