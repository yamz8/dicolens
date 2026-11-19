#pragma once

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QVector>
#include <atomic>
#include <vector>

#include "pixels.h"
#include "reformat.h"

// A series' voxels packed into one block for drawing in 3D, each value
// scaled from low to high into 16 bits.
struct VoxelGrid {
    bool valid = false;
    Volume volume;                  // Where the voxels lie, as packed.
    std::vector<quint16> data;      // Columns fastest, then rows, then slices.
    double low = 0, high = 1;
    // What the view centres on and fits, kept by halve() so a quick picture
    // lines up with the fine one.
    double center[3] = {0, 0, 0};
    double radius = 1;              // Half the box's diagonal: all of it, however turned.
    double fit = 1;                 // Half its longest side, a little over: what the view fits.
    // The least and most of each block of voxels (and the next voxel on,
    // which sampling reaches), so empty space is skipped.
    static constexpr int Block = 8;
    int blocks[3] = {0, 0, 0};
    std::vector<quint16> blockLow, blockHigh;

    double value(double packed) const { return low + packed * (high - low) / 65535; }
    double packed(double value) const { return (value - low) * 65535 / std::max(1e-9, high - low); }
};

// Packs a volume's decoded slices, each value times scale (a PET's SUV
// factor); invalid unless every slice is there in grey.
VoxelGrid packVolume(const Volume &volume, const QVector<Frame> &slices, double scale = 1);
// Half as many voxels each way, each the mean of those it covers: quick to
// draw while the volume turns.
VoxelGrid halve(const VoxelGrid &grid);

// How a volume is drawn: what each value is made to look like. CT has its
// tissues' looks; any other series has a surface at its window.
enum class Look { Bone, Vessels, Skin, Lungs, Surface, Mip };

// "Bone", "Vessels", and so on, in English.
const char *lookName(Look look);

struct View3D {
    // Screen right, screen up, and toward the viewer, in patient
    // coordinates: from the front, the patient's right on the left.
    double right[3] = {1, 0, 0};
    double up[3] = {0, 0, 1};
    double toward[3] = {0, -1, 0};
    double zoom = 1;
    double pan[2] = {0, 0};         // Millimetres right and up.
    double cut = 0;                 // Millimetres cut away from the front.
    Look look = Look::Bone;
    double shift = 0;               // Moves the look's values, in the series' units.
    // A surface and a MIP follow the series' window.
    double center = 0, width = 1;
    bool inverted = false;
    int colorMap = 0;

    // Turns the volume about the screen's up axis, then its right one.
    void turn(double across, double down);
    // Looking from the front, the back, the side, or the top.
    void face(int side);
};

// A PET drawn into the volume as its brightest along each ray, in colour.
struct Overlay3D {
    const VoxelGrid *grid = nullptr;
    double low = 0, high = 1;       // From clear to full colour.
    int colorMap = 2;
    double opacity = 0.6;
};

// The volume as seen, size pixels big, sampled step voxels apart (smaller
// is finer); null if cancelled.
QImage renderVolume(const VoxelGrid &grid, const View3D &view, QSize size, double step = 1,
                    const Overlay3D *overlay = nullptr, const std::atomic<bool> *cancel = nullptr);

// The patient point first seen at a pixel, if any is.
bool pickVolume(const VoxelGrid &grid, const View3D &view, QSize size, QPointF at, double *point);

// Where a patient point is on the screen.
QPointF projectPoint(const VoxelGrid &grid, const View3D &view, QSize size, const double *point);

// The value at a point in a grid's voxels, as the series has it; false
// outside the grid.
bool sampleGrid(const VoxelGrid &grid, const double *voxel, double &value);

// A picture with a PET over it, in colour where its values rise above low,
// for a picture lying in the patient as plane says.
QImage fuseSlice(const QImage &base, const ImagePlane &plane, const VoxelGrid &pet, const Overlay3D &look);
