// A whole scan: several files (the input, cut at NextFile) grouped into
// series and sorted, and each series that stacks into a volume cut across
// its axes, tilted, packed, and drawn in 3D, all at a few pixels' size.

#include "harness.h"
#include "reformat.h"
#include "study.h"
#include "volumerender.h"

#include <QHash>

namespace {

constexpr int MaxFiles = 8;
// Decoding and drawing take time by the slice; a series of more is only
// grouped and sorted.
constexpr int MaxSlices = 64;

void draw(const Series &series, const Volume &volume, const QVector<Frame> &slices) {
    for (int axis = 0; axis < 3; ++axis) {
        const Cut cut = cutAcross(volume, axis);
        const int middle = cut.count(volume) / 2;
        touch(reformat(volume, cut, middle, slices));
        touch(reformat(volume, cut, middle, slices, 1, Projection::Min));
        touch(reformat(volume, cut, middle, slices, 2, Projection::Mean));
        (void)planeName(cutPlane(volume, cut, middle));
    }

    const VoxelGrid grid = packVolume(volume, slices, series.suv > 0 ? series.suv : 1);
    if (!grid.valid)
        return;

    // A plane tilted off every axis, through the middle.
    double row[3] = {1, 0, 0}, column[3] = {0, 1, 0};
    const double axis[3] = {1, 2, 3};
    turnAbout(axis, 0.5, row);
    turnAbout(axis, 0.5, column);
    const Oblique oblique = obliqueThrough(volume, grid.center, row, column);
    // As big as the volume makes it, which a made-up spacing can stretch to
    // a million pixels; those are drawn by the tests, not here.
    if (oblique.valid && qint64(oblique.width) * oblique.height <= 96 * 96) {
        touch(resample(volume, oblique, oblique.middle, slices, grid.low, grid.high));
        touch(resample(volume, oblique, oblique.indexAt(volume.origin), slices, grid.low, grid.high, 1, Projection::Mean));
        (void)planeName(oblique.plane(0));
    }

    const VoxelGrid quick = halve(grid);
    const QSize size(12, 10);
    View3D view;
    view.center = slices.first().windowCenter;
    view.width = slices.first().windowWidth;
    view.turn(0.4, 0.3);
    for (Look look : {defaultLook(series.modality), Look::Surface, Look::Mip}) {
        view.look = look;
        (void)renderVolume(quick, view, size);
    }
    double low = 0, high = 1;
    petRange(grid, series.suv > 0, low, high);
    const Overlay3D overlay{&quick, low, high, 2, 0.6};
    view.face(1);
    (void)renderVolume(grid, view, size, 2, &overlay);
    double point[3] = {0, 0, 0}, value = 0;
    if (pickVolume(grid, view, size, QPointF(6, 5), point))
        (void)projectPoint(grid, view, size, point);
    double voxel[3];
    toVoxel(volume, point, voxel);
    (void)sampleGrid(grid, voxel, value);
    const QImage base = render(slices.first(), view.center, view.width, false);
    (void)fuseSlice(base, slicePlane(series, 0), grid, overlay);
}

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static MemoryFile memory[MaxFiles];
    const QByteArray input = QByteArray::fromRawData(reinterpret_cast<const char *>(data), qsizetype(size));
    const QByteArray next(NextFile);

    Scan scan;
    QHash<QString, DicomFile> files;
    qsizetype from = 0;
    for (int i = 0; i < MaxFiles && from <= input.size(); ++i) {
        qsizetype to = input.indexOf(next, from);
        if (to < 0 || i == MaxFiles - 1)
            to = input.size();
        memory[i].fill(data + from, size_t(to - from));
        from = to + next.size();
        const DicomFile file = DicomFile::read(memory[i].path());
        if (!file.isValid())
            continue;
        addFile(scan, file);
        (void)suvFactor(file);
        files.insert(file.path(), file);
    }
    sortScan(scan);

    for (int s = 0; s < scan.series.size(); ++s) {
        const Series &series = scan.series[s];
        (void)series.edgeLabels();
        (void)series.pixelAspect();
        (void)fusionPartner(scan.series, s);
        (void)slicePlane(series, 0);
        const Volume volume = makeVolume(series);
        if (!volume.valid || series.slices.size() > MaxSlices)
            continue;
        QVector<Frame> slices;
        for (const Slice &slice : series.slices)
            slices << decodeFrame(files.value(slice.path), slice.frame);
        draw(series, volume, slices);
    }
    return 0;
}
