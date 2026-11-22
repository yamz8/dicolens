#pragma once

#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include "measure.h"
#include "pixels.h"
#include "reformat.h"
#include "study.h"

// How a pane cuts its series: as scanned, across it the other two ways, or
// tilted to any angle; thin or as a slab; which picture of that stack is
// shown; and the point every plane goes through, which a click sets and
// the other views share.
class Planes {
public:
    // How the pictures of one stack are made from the series' slices, with
    // nothing that changes from picture to picture, so a video being saved
    // can make every one of them on its own thread.
    struct Recipe {
        Series series;
        Volume volume;
        Cut cut;
        Oblique oblique;
        int plane = 0;
        bool built = false;             // Made from the slices, not one of them.
        int count = 0;
        int half = 0;                   // Slices either side, in a slab.
        Projection projection = Projection::Max;
        double low = 0, high = 0;       // The volume's values, for a tilted plane.

        // Null unless every slice is there.
        Frame make(int index, const QVector<Frame> &slices) const;
        ImagePlane where(int index) const;
    };

    // A new series: as scanned, thin, from its first slice.
    void setSeries(const Series &series);

    const Series &series() const { return m_series; }
    // A stack of even, parallel slices can be cut the other two ways.
    const Volume &volume() const { return m_volume; }
    // 0 is the plane as scanned, 1 and 2 the other two, and 3 a tilted one.
    int plane() const { return m_plane; }
    // 0 thin, then a MIP, a MinIP, and an average.
    int slab() const { return m_slab; }
    double slabMm() const { return m_slabMm; }
    int index() const { return m_index; }
    int count() const;
    // Shows a picture built from the slices, not one of them.
    bool built() const { return m_plane != 0 || m_slab != 0; }
    // The slice of the series behind what's shown: the one shown, or the
    // one through where the planes meet.
    int acquired() const { return m_plane != 0 ? m_cursor[2] : m_index; }
    double pixelAspect() const;
    ImagePlane imagePlane() const;
    // Patient directions at the picture's left, right, top, and bottom.
    QStringList edges() const;
    // Which cut a tilted plane is, told from any other; empty otherwise.
    QString cutKey() const;
    QSize size() const;
    MeasureScale scale() const;

    bool crossingShown() const { return m_crossingShown; }
    // The patient point where the planes meet.
    void crossingPoint(double *point) const;
    // The same in this picture's pixels, when a click set it and it lies
    // in this plane.
    bool crossing(QPointF &at) const;

    // Each says whether anything changed.
    bool setPlane(int plane);
    bool setIndex(int index);
    void nextSlab() { m_slab = (m_slab + 1) % 4; }
    void nextThickness();
    void thin() { m_slab = 0; }
    // Back to the slices as scanned, at the one the planes meet in.
    void straighten();
    // Makes the planes meet at a patient point and shows the picture
    // through it; false when the series has no place for the point.
    bool goThrough(const double *point);
    // Tilts the cut about the picture's up-down axis (0) or its left-right
    // one (1), through where the planes meet.
    bool tilt(int axis, double degrees);
    // Turns the plane about an axis through where the planes meet, from
    // where it was when the turn began.
    void beginTurn();
    bool turn(const double *axis, double radians);
    // The planes meet where another view's do.
    void meetWhere(const Planes &other);

    // The shown picture's recipe; a tilted plane needs the slices to know
    // the range of their values.
    Recipe recipe(const QVector<Frame> &slices);

private:
    bool becomeOblique(const double *center, const double *row, const double *column);

    Series m_series;
    Volume m_volume;
    Cut m_cut;
    int m_plane = 0;
    int m_slab = 0;
    double m_slabMm = 10;
    int m_index = 0;
    int m_cursor[3] = {0, 0, 0};   // A voxel the planes meet at.
    bool m_crossingShown = false;  // Set by a click, so drawn.
    Oblique m_oblique;
    double m_turnRow[3] = {1, 0, 0}, m_turnColumn[3] = {0, 1, 0};
    double m_low = 0, m_high = 0;  // The volume's values, once it's all read.
    bool m_rangeKnown = false;
};
