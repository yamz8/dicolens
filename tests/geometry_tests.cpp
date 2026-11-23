// A CT taken with its gantry tilted. A shape of known size that lies in the
// patient is scanned straight and leaning, both ways, and whatever is cut,
// measured, or drawn from a leaning stack has to show the shape where it is
// and as big as it is, as the straight stack does. The tests also read the
// leaning stack the way it would be read without its slices' shifts, as a
// plain block with every slice straight behind the one before, and hold
// that to be wrong by the same measure: it is placing the slices that
// passes them.

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include <climits>

#include "pixels.h"
#include "reformat.h"
#include "study.h"
#include "volumerender.h"

#include "support.h"

namespace {

double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The shape, in the patient's millimetres: a box, beside it a rod that runs
// along the table, and a small ball off to one side that a PET's hot spot
// is put on. All are bone in air, and their edges fade over Soft mm either
// side, so that a value crosses halfway (0 HU) at the surface to a fraction
// of a voxel, however the slices cut it.
constexpr double Air = -1000, Bone = 1000, Halfway = 0, Soft = 3;
constexpr double BoxCenter[3] = {-14.5, 0, 0}, BoxHalf[3] = {10, 12, 18};
constexpr double RodAt[2] = {16.5, 0}, RodRadius = 4, RodLength = 72;
constexpr double Marker[3] = {10, -14, 22}, MarkerRadius = 3;

// The scanner: slices a millimetre a pixel, their centres a millimetre and
// a half apart along the table.
constexpr int Columns = 64, Rows = 64, Slices = 64;
constexpr double Pixel = 1, Step = 1.5;
constexpr int Tilts[] = {15, -15, 30, -30};

// How much of a point is inside a surface it lies that far outside of.
double solid(double outside) {
    const double t = std::clamp(0.5 - outside / (2 * Soft), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

double phantom(const double *p) {
    double box = -1e9;
    for (int a = 0; a < 3; ++a)
        box = std::max(box, std::abs(p[a] - BoxCenter[a]) - BoxHalf[a]);
    const double rod = std::max(std::hypot(p[0] - RodAt[0], p[1] - RodAt[1]) - RodRadius, std::abs(p[2]) - RodLength / 2);
    const double marker = std::sqrt(std::pow(p[0] - Marker[0], 2) + std::pow(p[1] - Marker[1], 2) + std::pow(p[2] - Marker[2], 2))
                          - MarkerRadius;
    return Air + (Bone - Air) * std::max({solid(box), solid(rod), solid(marker)});
}

// A series as the viewer has it: grouped and sorted, made a volume of, and
// every slice decoded.
struct Scanned {
    Series series;
    Volume volume;
    QVector<Frame> slices;
};

Scanned scanned(const QString &folder) {
    Scanned s;
    const Scan scan = scanFiles(listFiles({folder}, true));
    if (scan.series.size() != 1)
        return s;
    s.series = scan.series.first();
    s.volume = makeVolume(s.series);
    for (const Slice &slice : std::as_const(s.series.slices))
        s.slices << decodeFrame(DicomFile::read(slice.path), slice.frame);
    return s;
}

// The stack read without its slices' shifts: a plain block, pixel for pixel,
// every slice straight behind the one before. Put where its middle slice
// lies, which is as kind as a block that leans can be placed.
Volume plainBlock(const Scanned &s) {
    Volume v = s.volume;
    const QPointF shift = v.tilted() ? v.shift[v.size[2] / 2] : QPointF();
    const double corner[3] = {shift.x(), shift.y(), 0};
    toPatient(s.volume, corner, v.origin);
    v.shift.clear();
    v.slice[0] = v.slice[1] = 0;
    v.size[0] = s.series.columns;
    v.size[1] = s.series.rows;
    return v;
}

// Half the coarsest voxel: as near as a length read off a volume has to be.
double halfVoxel(const Volume &v) {
    return std::max({v.spacing[0], v.spacing[1], v.spacing[2]}) / 2;
}

// And how near the shape's soft edges let it be read, in millimetres: far
// inside half a voxel. A stack whose slices were put a fraction of a pixel
// from where they lie would be outside it.
constexpr double Fine = 0.1;

// Whether the slices either side of a patient point both reach it, with
// margin pixels to spare: where a picture is made of the scan itself and
// not of its edge.
bool covered(const Volume &v, const double *p, double margin = 1) {
    double voxel[3];
    toVoxel(v, p, voxel);
    if (voxel[2] < margin || voxel[2] > v.size[2] - 1 - margin)
        return false;
    for (int k : {int(std::floor(voxel[2])), int(std::ceil(voxel[2]))}) {
        const QPointF shift = v.tilted() ? v.shift[k] : QPointF();
        const double x = voxel[0] - shift.x(), y = voxel[1] - shift.y();
        if (x < margin || y < margin || x > v.columns() - 1 - margin || y > v.rows() - 1 - margin)
            return false;
    }
    return true;
}

// What a picture has at a patient point in its plane, between its pixels;
// false off the picture.
bool valueAt(const Frame &picture, const ImagePlane &plane, const double *p, double &value) {
    const double d[3] = {p[0] - plane.origin[0], p[1] - plane.origin[1], p[2] - plane.origin[2]};
    double stored;
    if (!sampleSlice(picture, dot(d, plane.row) / plane.rowSpacing, dot(d, plane.column) / plane.columnSpacing, stored))
        return false;
    value = stored * picture.slope + picture.intercept;
    return true;
}

// How far along a line through the patient the values first cross halfway,
// to a fraction of a step; -1 if they never do. sample(point, value) is
// false where there is nothing to read.
template <typename Sample> double crossing(Sample sample, const double *from, const double *along, double length) {
    const double step = 0.05;
    double before = 0;
    bool any = false;
    for (int i = 0; i * step <= length; ++i) {
        const double p[3] = {from[0] + i * step * along[0], from[1] + i * step * along[1], from[2] + i * step * along[2]};
        double value;
        if (!sample(p, value)) {
            any = false;
            continue;
        }
        if (any && (before < Halfway) != (value < Halfway))
            return (i - 1 + (Halfway - before) / (value - before)) * step;
        before = value;
        any = true;
    }
    return -1;
}

// How far a picture's values are from what should be there, at most and on
// average, over the pixels truth(point, value) answers for.
struct Difference {
    double most = 0, mean = 0;
    int count = 0;
};

template <typename Truth> Difference differenceFrom(const Frame &picture, const ImagePlane &plane, Truth truth) {
    Difference d;
    double sum = 0;
    for (int y = 0; y < picture.height; ++y)
        for (int x = 0; x < picture.width; ++x) {
            double p[3], expected;
            plane.point(x, y, p);
            if (!truth(p, expected))
                continue;
            const double off = std::abs(picture.value(x, y) - expected);
            d.most = std::max(d.most, off);
            sum += off;
            ++d.count;
        }
    d.mean = d.count > 0 ? sum / d.count : 0;
    return d;
}

// The length of the one long thing in a picture, as a ruler laid along it
// from end to end reads it by the picture's own spacing: through its
// middle, the way it runs, between where its values rise past halfway and
// where they fall back. direction is the way it runs, in the picture's
// millimetres across and down.
double lengthIn(const Frame &picture, double across, double down, double *direction) {
    double n = 0, mx = 0, my = 0;
    for (int y = 0; y < picture.height; ++y)
        for (int x = 0; x < picture.width; ++x)
            if (picture.value(x, y) >= Halfway) {
                mx += x * across;
                my += y * down;
                ++n;
            }
    if (n < 2)
        return 0;
    mx /= n;
    my /= n;
    double xx = 0, xy = 0, yy = 0;
    for (int y = 0; y < picture.height; ++y)
        for (int x = 0; x < picture.width; ++x)
            if (picture.value(x, y) >= Halfway) {
                xx += std::pow(x * across - mx, 2);
                xy += (x * across - mx) * (y * down - my);
                yy += std::pow(y * down - my, 2);
            }
    const double angle = 0.5 * std::atan2(2 * xy, xx - yy);
    direction[0] = std::cos(angle);
    direction[1] = std::sin(angle);
    double length = 0;
    for (int side : {1, -1}) {
        double before = Bone;
        for (double t = 0;; t += 0.05) {
            double stored;
            if (!sampleSlice(picture, (mx + side * t * direction[0]) / across, (my + side * t * direction[1]) / down, stored))
                return 0;
            const double value = stored * picture.slope + picture.intercept;
            if (value < Halfway) {
                length += t - 0.05 + 0.05 * (Halfway - before) / (value - before);
                break;
            }
            before = value;
        }
    }
    return length;
}

}

class GeometryTests : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    QMap<int, Scanned> scans;    // The shape, by the gantry's tilt in degrees.

    const Scanned &straight() const { return *scans.constFind(0); }

private slots:
    void initTestCase() {
        for (int tilt : {0, 15, -15, 30, -30}) {
            const QString folder = dir.filePath(QString::number(tilt));
            QVERIFY(QDir().mkpath(folder));
            QVERIFY(writeTiltedSeries(folder, "CT", "1.2.88." + QByteArray::number(100 + tilt), "1.2.88.1", Columns, Rows, Slices,
                                      Pixel, Step, tilt, [](const double *p) { return int(std::lround(phantom(p))) + 1024; }));
            const Scanned s = scanned(folder);
            QCOMPARE(s.slices.size(), Slices);
            for (const Frame &f : s.slices)
                QVERIFY(!f.isNull() && f.error.isEmpty());
            scans.insert(tilt, s);
        }
    }

    void takesATiltedStackAsASquareBlock() {
        for (auto it = scans.cbegin(); it != scans.cend(); ++it) {
            const int tilt = it.key();
            const Scanned &s = it.value();
            const Volume &v = s.volume;
            const double radians = tilt * M_PI / 180;
            QVERIFY2(v.valid, qPrintable(QString::number(tilt)));
            QCOMPARE(v.tilted(), tilt != 0);

            // Its axes are a millimetre long and square to each other, the
            // third along the slices' own normal, not along the table.
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    QVERIFY(std::abs(dot(v.axis[a], v.axis[b]) - (a == b ? 1 : 0)) < 1e-6);
            const double normal[3] = {0, -std::sin(radians), std::cos(radians)};
            QVERIFY(std::abs(std::abs(dot(v.axis[2], normal)) - 1) < 1e-6);
            // Along the normal the slices are nearer than the table's step:
            // by the tilt's cosine.
            QVERIFY(std::abs(v.spacing[2] - Step * std::cos(radians)) < 1e-6);
            QCOMPARE(v.spacing[0], Pixel);
            QCOMPARE(v.spacing[1], Pixel);

            // The block is as much taller as the last slice lies to the
            // side of the first, and holds the slices at their own size.
            const double each = Step * std::abs(std::sin(radians)) / Pixel;
            QCOMPARE(v.size[0], Columns);
            QCOMPARE(v.size[1], Rows + int(std::ceil((Slices - 1) * each - 1e-6)));
            QCOMPARE(v.size[2], Slices);
            QCOMPARE(v.columns(), Columns);
            QCOMPARE(v.rows(), Rows);
            if (tilt == 0) {
                QVERIFY(v.shift.isEmpty());
                continue;
            }
            QCOMPARE(v.shift.size(), Slices);
            double least = 1e9;
            for (int k = 0; k < Slices; ++k) {
                const QPointF shift = v.shift[k];
                // Each slice the same step down its columns from the one
                // before, none outside the block.
                QCOMPARE(shift.x(), 0.0);
                QVERIFY(shift.y() >= 0 && shift.y() + Rows <= v.size[1]);
                if (k > 0)
                    QVERIFY(std::abs(std::abs(shift.y() - v.shift[k - 1].y()) - each) < 1e-6);
                least = std::min(least, shift.y());
                // And where the block has a slice is where the slice is.
                const double corner[3] = {shift.x(), shift.y(), double(k)};
                double p[3];
                toPatient(v, corner, p);
                const ImagePlane own = slicePlane(s.series, k);
                for (int a = 0; a < 3; ++a)
                    QVERIFY(std::abs(p[a] - own.origin[a]) < 1e-6);
            }
            QCOMPARE(least, 0.0);
        }
    }

    void goesFromAPictureToItsVoxelsAndBack() {
        for (auto it = scans.cbegin(); it != scans.cend(); ++it) {
            const Scanned &s = it.value();
            const Volume &v = s.volume;
            for (int fixed = 0; fixed < 3; ++fixed) {
                const Cut cut = cutAcross(v, fixed);
                for (int index : {0, cut.count(v) / 2, cut.count(v) - 1}) {
                    const ImagePlane plane = cutPlane(v, cut, index);
                    const Frame picture = reformat(v, cut, index, s.slices);
                    QVERIFY(plane.valid);
                    QVERIFY(!picture.isNull());
                    // The picture is as big as its plane says, with rows and
                    // columns square to each other, spaced as the block is.
                    QCOMPARE(picture.width, plane.width);
                    QCOMPARE(picture.height, plane.height);
                    QCOMPARE(plane.width, cut.width(v));
                    QCOMPARE(plane.height, cut.height(v));
                    QVERIFY(std::abs(dot(plane.row, plane.column)) < 1e-6);
                    QVERIFY(std::abs(dot(plane.row, plane.row) - 1) < 1e-6);
                    QVERIFY(std::abs(dot(plane.column, plane.column) - 1) < 1e-6);
                    QCOMPARE(plane.rowSpacing, v.spacing[cut.horizontal]);
                    QCOMPARE(plane.columnSpacing, v.spacing[cut.vertical]);
                    for (const QPoint at : {QPoint(0, 0), QPoint(plane.width - 1, 0), QPoint(7, 11),
                                            QPoint(plane.width - 1, plane.height - 1)}) {
                        // From a pixel to the patient, to the voxel the
                        // pixel was read from, and back to the same point.
                        double p[3], voxel[3], back[3], expected[3];
                        plane.point(at.x(), at.y(), p);
                        toVoxel(v, p, voxel);
                        expected[cut.horizontal] = cut.flipHorizontal ? plane.width - 1 - at.x() : at.x();
                        expected[cut.vertical] = cut.flipVertical ? plane.height - 1 - at.y() : at.y();
                        expected[cut.fixed] = index;
                        if (fixed == 2 && v.tilted()) {
                            expected[0] += v.shift[index].x();
                            expected[1] += v.shift[index].y();
                        }
                        toPatient(v, voxel, back);
                        for (int a = 0; a < 3; ++a) {
                            QVERIFY(std::abs(voxel[a] - expected[a]) < 1e-6);
                            QVERIFY(std::abs(back[a] - p[a]) < 1e-6);
                        }
                        // And what the picture has there is the shape's
                        // value at that point.
                        QVERIFY(std::abs(picture.value(at.x(), at.y()) - phantom(p)) < 150);
                    }
                }
            }
            // Across the slices, the plane is each slice's own.
            const Cut scannedCut = cutAcross(v, 2);
            for (int k : {0, Slices / 2, Slices - 1}) {
                const ImagePlane plane = cutPlane(v, scannedCut, k), own = slicePlane(s.series, k);
                QCOMPARE(plane.width, own.width);
                QCOMPARE(plane.height, own.height);
                for (int a = 0; a < 3; ++a) {
                    QVERIFY(std::abs(plane.origin[a] - own.origin[a]) < 1e-6);
                    QVERIFY(std::abs(plane.row[a] - own.row[a]) < 1e-9);
                    QVERIFY(std::abs(plane.column[a] - own.column[a]) < 1e-9);
                }
            }
        }
    }

    void readsEveryVoxelWhereItLies() {
        // A stack that leans both ways at once, across no axis of the
        // patient's, with pixels longer than they are wide, numbered from
        // either end. Its values rise evenly through the patient, and
        // weighing values that rise evenly gives the value where the
        // weighing is done: so whatever is read out of the block for a
        // point has to be the value at that very point, to the rounding of
        // what was stored, and a tenth of a millimetre out would show.
        auto ramp = [](const double *p) { return 5000 + 20 * p[0] + 12 * p[1] + 16 * p[2]; };
        const int columns = 20, rows = 16, count = 12;
        const double across = 0.7, down = 1.1, apart = 2;
        double row[3] = {1, 0, 0}, column[3] = {0, 1, 0};
        const double about[3] = {1, 2, 3};
        turnAbout(about, 0.4, row);
        turnAbout(about, 0.4, column);
        const double normal[3] = {row[1] * column[2] - row[2] * column[1], row[2] * column[0] - row[0] * column[2],
                                  row[0] * column[1] - row[1] * column[0]};
        for (bool fromTheFarEnd : {false, true}) {
            QList<std::array<double, 3>> positions;
            for (int k = 0; k < count; ++k) {
                const int n = fromTheFarEnd ? count - 1 - k : k;
                std::array<double, 3> first = {-8, -7, -9};
                for (int a = 0; a < 3; ++a)
                    first[a] += n * (apart * normal[a] + 0.3 * row[a] - 0.9 * column[a]);
                positions << first;
            }
            QTemporaryDir folder;
            QVERIFY(writeSlicesAt(folder.path(), "CT", "1.2.89.1", "1.2.89", columns, rows, across, down, row, column, positions,
                                  [&](const double *p) { return int(std::lround(ramp(p))) + 1024; }));
            const Scanned s = scanned(folder.path());
            const Volume &v = s.volume;
            QVERIFY(v.valid);
            QVERIFY(v.tilted());
            QCOMPARE(s.slices.size(), count);
            QCOMPARE(v.size[0], columns + 5);    // 11 steps of 0.3 mm, at 0.7 mm a pixel.
            QCOMPARE(v.size[1], rows + 9);       // 11 steps of 0.9 mm, at 1.1 mm a pixel.
            QVERIFY(std::abs(v.spacing[2] - apart) < 1e-6);
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    QVERIFY(std::abs(dot(v.axis[a], v.axis[b]) - (a == b ? 1 : 0)) < 1e-6);

            // What slice k has at a patient point, worked out from where
            // the file says the slice is and nothing else: the value at the
            // point (or at the slice's edge, within half a pixel past it),
            // Off the slice beyond that, and Unsure on the line between.
            enum { Off, On, Unsure };
            auto read = [&](int k, const double *p, double &value) {
                const double *first = s.series.slices[k].position;
                const double d[3] = {p[0] - first[0], p[1] - first[1], p[2] - first[2]};
                double x = dot(d, row) / across, y = dot(d, column) / down;
                const double out = std::max({-0.5 - x, -0.5 - y, x - (columns - 0.5), y - (rows - 0.5)});
                if (std::abs(out) < 1e-6)
                    return int(Unsure);
                if (out > 0)
                    return int(Off);
                x = std::clamp(x, 0.0, columns - 1.0);
                y = std::clamp(y, 0.0, rows - 1.0);
                double q[3];
                for (int a = 0; a < 3; ++a)
                    q[a] = first[a] + x * across * row[a] + y * down * column[a];
                value = ramp(q);
                return int(On);
            };

            for (int fixed = 0; fixed < 3; ++fixed) {
                const Cut cut = cutAcross(v, fixed);
                // Every picture of every cut: the value where each pixel
                // lies, or nothing (air, on a CT) where no slice reaches.
                int read1 = 0, nothing1 = 0;
                for (int index = 0; index < cut.count(v); ++index) {
                    const Frame picture = reformat(v, cut, index, s.slices);
                    const ImagePlane plane = cutPlane(v, cut, index);
                    QCOMPARE(picture.width, plane.width);
                    QCOMPARE(picture.height, plane.height);
                    for (int y = 0; y < picture.height; ++y)
                        for (int x = 0; x < picture.width; ++x) {
                            double p[3], voxel[3], expected = 0;
                            plane.point(x, y, p);
                            toVoxel(v, p, voxel);
                            const int found = read(int(std::lround(voxel[2])), p, expected);
                            if (found == Unsure)
                                continue;
                            const double value = picture.value(x, y);
                            if (found == Off) {
                                QCOMPARE(value, -1000.0);
                                ++nothing1;
                            } else {
                                QVERIFY2(std::abs(value - expected) <= 1.01,
                                         qPrintable(QString("cut %1 picture %2 at %3, %4: %5, not %6").arg(fixed).arg(index).arg(x).arg(y).arg(value).arg(expected)));
                                ++read1;
                            }
                        }
                }
                QVERIFY(read1 >= columns * rows * count);
                QCOMPARE(nothing1 > 0, fixed != 2);

                // A slab goes through the block square to the cut, and
                // takes in only what its slices reach: nothing counts for
                // nothing, not for air.
                for (Projection projection : {Projection::Max, Projection::Min, Projection::Mean}) {
                    const int index = cut.count(v) / 2, half = 2;
                    const Frame slab = reformat(v, cut, index, s.slices, half, projection);
                    const ImagePlane plane = cutPlane(v, cut, index);
                    QCOMPARE(slab.width, plane.width);
                    QCOMPARE(slab.height, plane.height);
                    int read2 = 0;
                    for (int y = 0; y < slab.height; ++y)
                        for (int x = 0; x < slab.width; ++x) {
                            double p[3];
                            plane.point(x, y, p);
                            double most = -1e9, least = 1e9, sum = 0;
                            int reached = 0;
                            bool unsure = false;
                            for (int step = -half; step <= half; ++step) {
                                if (index + step < 0 || index + step >= cut.count(v))
                                    continue;
                                double q[3], voxel[3], value = 0;
                                for (int a = 0; a < 3; ++a)
                                    q[a] = p[a] + step * v.spacing[fixed] * v.axis[fixed][a];
                                toVoxel(v, q, voxel);
                                const int found = read(int(std::lround(voxel[2])), q, value);
                                unsure = unsure || found == Unsure;
                                if (found != On)
                                    continue;
                                most = std::max(most, value);
                                least = std::min(least, value);
                                sum += value;
                                ++reached;
                            }
                            if (unsure)
                                continue;
                            const double value = slab.value(x, y);
                            const double expected = reached == 0 ? -1000
                                                    : projection == Projection::Max ? most
                                                    : projection == Projection::Min ? least : sum / reached;
                            QVERIFY2(std::abs(value - expected) <= 1.2,
                                     qPrintable(QString("cut %1 slab %2 at %3, %4: %5, not %6").arg(fixed).arg(int(projection)).arg(x).arg(y).arg(value).arg(expected)));
                            read2 += reached > 0;
                        }
                    QVERIFY(read2 > slab.width * slab.height / 4);
                }
            }

            // A plane at an angle of its own, through the middle. Between
            // two slices that both reach a point, the value at the point;
            // where neither does, the least there is.
            double r[3] = {row[0], row[1], row[2]}, c[3] = {column[0], column[1], column[2]}, center[3];
            const double turn[3] = {0.3, 1, 0.2};
            turnAbout(turn, 0.6, r);
            turnAbout(turn, 0.6, c);
            slicePlane(s.series, count / 2).point(columns / 2, rows / 2, center);
            const Oblique oblique = obliqueThrough(v, center, r, c);
            QVERIFY(oblique.valid);
            const double low = -1024, high = low + 65535 * 0.125;
            enum { Outside, Inside, Partly };
            auto between = [&](const double *p) {
                double voxel[3];
                toVoxel(v, p, voxel);
                for (int a = 0; a < 3; ++a) {
                    if (voxel[a] < -0.5 - 1e-6 || voxel[a] > v.size[a] - 0.5 + 1e-6)
                        return int(Outside);
                    if (voxel[a] < -0.5 + 1e-6 || voxel[a] > v.size[a] - 0.5 - 1e-6)
                        return int(Partly);
                }
                if (voxel[2] < 0 || voxel[2] > count - 1)
                    return int(Partly);
                int inside = 0, outside = 0;
                for (int k : {int(std::floor(voxel[2])), int(std::ceil(voxel[2]))}) {
                    const double *first = s.series.slices[k].position;
                    const double d[3] = {p[0] - first[0], p[1] - first[1], p[2] - first[2]};
                    const double x = dot(d, row) / across, y = dot(d, column) / down;
                    inside += x >= 0 && y >= 0 && x <= columns - 1 && y <= rows - 1;
                    outside += x < -0.5 - 1e-6 || y < -0.5 - 1e-6 || x > columns - 0.5 + 1e-6 || y > rows - 0.5 + 1e-6;
                }
                return inside == 2 ? int(Inside) : outside == 2 ? int(Outside) : int(Partly);
            };
            for (int half : {0, 1}) {
                const int index = oblique.middle;
                const Frame picture = resample(v, oblique, index, s.slices, low, high, half, Projection::Mean);
                const ImagePlane plane = oblique.plane(index);
                QVERIFY(!picture.isNull());
                int read3 = 0, nothing3 = 0;
                for (int y = 0; y < picture.height; ++y)
                    for (int x = 0; x < picture.width; ++x) {
                        double p[3], sum = 0;
                        plane.point(x, y, p);
                        int inside = 0, outside = 0, planes = 0;
                        for (int step = std::max(-half, -index); step <= std::min(half, oblique.count - 1 - index); ++step) {
                            double q[3];
                            for (int a = 0; a < 3; ++a)
                                q[a] = p[a] + step * oblique.step * oblique.normal[a];
                            const int state = between(q);
                            inside += state == Inside;
                            outside += state == Outside;
                            sum += ramp(q);
                            ++planes;
                        }
                        const double value = picture.value(x, y);
                        if (inside == planes) {
                            QVERIFY2(std::abs(value - sum / planes) <= 0.6,
                                     qPrintable(QString("oblique at %1, %2: %3, not %4").arg(x).arg(y).arg(value).arg(sum / planes)));
                            ++read3;
                        } else if (outside == planes) {
                            QCOMPARE(value, low);
                            ++nothing3;
                        }
                    }
                QVERIFY(read3 > 100);
                QVERIFY(nothing3 > 100);
            }

            // Packed for drawing, the block is plain: every voxel holds
            // what lies at its own place, or the least there is.
            const VoxelGrid grid = packVolume(v, s.slices);
            QVERIFY(grid.valid);
            QVERIFY(!grid.volume.tilted());
            for (int a = 0; a < 3; ++a) {
                QCOMPARE(grid.volume.size[a], v.size[a]);
                QCOMPARE(grid.volume.spacing[a], v.spacing[a]);
                QCOMPARE(grid.volume.origin[a], v.origin[a]);
            }
            const double finest = (grid.high - grid.low) / 65535;
            int read4 = 0, nothing4 = 0;
            for (int k = 0; k < v.size[2]; ++k)
                for (int j = 0; j < v.size[1]; ++j)
                    for (int i = 0; i < v.size[0]; ++i) {
                        const double voxel[3] = {double(i), double(j), double(k)};
                        double p[3], expected = 0;
                        toPatient(grid.volume, voxel, p);
                        const int found = read(k, p, expected);
                        if (found == Unsure)
                            continue;
                        const quint16 packed = grid.data[(size_t(k) * v.size[1] + j) * v.size[0] + i];
                        if (found == Off) {
                            QCOMPARE(int(packed), 0);
                            ++nothing4;
                        } else {
                            QVERIFY(std::abs(grid.value(packed) - expected) <= 0.51 + finest);
                            ++read4;
                        }
                    }
            QVERIFY(read4 >= columns * rows * count);
            QVERIFY(nothing4 > 0);
        }
    }

    void cutsATiltedStackWhereAStraightOneDoes() {
        const Scanned &flat = straight();
        for (int tilt : Tilts) {
            const Scanned &s = *scans.constFind(tilt);
            const Volume &v = s.volume, plain = plainBlock(s);
            const double near = halfVoxel(v);

            // Sagittal, through the middle of the box: the same plane of
            // the patient as the straight stack's own sagittal cut.
            double voxel[3];
            toVoxel(v, BoxCenter, voxel);
            const Cut sagittal = cutAcross(v, 0);
            const int column = int(std::lround(voxel[0]));
            const Frame side = reformat(v, sagittal, column, s.slices);
            const ImagePlane sidePlane = cutPlane(v, sagittal, column);
            QCOMPARE(QString(planeName(sidePlane)), QString("Sagittal"));
            toVoxel(flat.volume, BoxCenter, voxel);
            const Cut flatSagittal = cutAcross(flat.volume, 0);
            const Frame flatSide = reformat(flat.volume, flatSagittal, int(std::lround(voxel[0])), flat.slices);
            const ImagePlane flatSidePlane = cutPlane(flat.volume, flatSagittal, int(std::lround(voxel[0])));
            QVERIFY(std::abs(sidePlane.origin[0] - flatSidePlane.origin[0]) < 1e-6);

            // Pixel for pixel through the patient, the two pictures agree
            // to what weighing between voxels can differ by, and both with
            // the shape itself.
            const Difference fromFlat = differenceFrom(side, sidePlane, [&](const double *p, double &expected) {
                return covered(v, p) && covered(flat.volume, p) && valueAt(flatSide, flatSidePlane, p, expected);
            });
            const Difference fromShape = differenceFrom(side, sidePlane, [&](const double *p, double &expected) {
                expected = phantom(p);
                return covered(v, p);
            });
            QVERIFY(fromFlat.count > 3000);
            QVERIFY2(fromFlat.most < 160 && fromFlat.mean < 5, qPrintable(QString("%1: %2 %3").arg(tilt).arg(fromFlat.most).arg(fromFlat.mean)));
            QVERIFY2(fromShape.most < 160 && fromShape.mean < 5, qPrintable(QString("%1: %2 %3").arg(tilt).arg(fromShape.most).arg(fromShape.mean)));
            // The plain block's picture of the same cut is sheared: far
            // from the shape wherever the shape has an edge.
            const Frame sheared = reformat(plain, cutAcross(plain, 0), column, s.slices);
            const ImagePlane shearedPlane = cutPlane(plain, cutAcross(plain, 0), column);
            const Difference plainFromShape = differenceFrom(sheared, shearedPlane, [&](const double *p, double &expected) {
                expected = phantom(p);
                return true;
            });
            QVERIFY2(plainFromShape.most > 1500 && plainFromShape.mean > 10 * fromShape.mean,
                     qPrintable(QString("%1: %2 %3").arg(tilt).arg(plainFromShape.most).arg(plainFromShape.mean)));

            // The box's faces: front and back at the same depth however
            // high up they're crossed, top and bottom at the same height
            // however far back. Straight, and square to each other.
            auto faces = [&](const Frame &picture, const ImagePlane &plane, QList<double> &frontBack, QList<double> &topBottom) {
                auto sample = [&](const double *p, double &value) { return valueAt(picture, plane, p, value); };
                for (double z : {-12.0, -6.0, 0.0, 6.0, 12.0})
                    for (double way : {1.0, -1.0}) {
                        const double from[3] = {plane.origin[0], -way * (BoxHalf[1] + 10), z}, along[3] = {0, way, 0};
                        frontBack << crossing(sample, from, along, 20) - 10;
                    }
                for (double y : {-8.0, -4.0, 0.0, 4.0, 8.0})
                    for (double way : {1.0, -1.0}) {
                        const double from[3] = {plane.origin[0], y, -way * (BoxHalf[2] + 10)}, along[3] = {0, 0, way};
                        topBottom << crossing(sample, from, along, 20) - 10;
                    }
            };
            QList<double> frontBack, topBottom, flatFrontBack, flatTopBottom, plainFrontBack, plainTopBottom;
            faces(side, sidePlane, frontBack, topBottom);
            faces(flatSide, flatSidePlane, flatFrontBack, flatTopBottom);
            faces(sheared, shearedPlane, plainFrontBack, plainTopBottom);
            for (int i = 0; i < frontBack.size(); ++i) {
                QVERIFY2(std::abs(frontBack[i]) <= Fine, qPrintable(QString("%1: %2").arg(tilt).arg(frontBack[i])));
                QVERIFY2(std::abs(topBottom[i]) <= Fine, qPrintable(QString("%1: %2").arg(tilt).arg(topBottom[i])));
                QVERIFY(std::abs(flatFrontBack[i]) <= Fine);
                QVERIFY(std::abs(flatTopBottom[i]) <= Fine);
                QVERIFY(std::abs(frontBack[i] - flatFrontBack[i]) <= Fine);
                QVERIFY(std::abs(topBottom[i] - flatTopBottom[i]) <= Fine);
            }
            // In the plain block the front and back lean with the stack:
            // from the lowest crossing to the highest, 24 mm up, they move
            // by 24 mm times the tilt's tangent.
            const auto [nearest, furthest] = std::minmax_element(plainFrontBack.cbegin(), plainFrontBack.cend());
            QVERIFY2(*furthest - *nearest > 24 * std::tan(std::abs(tilt) * M_PI / 180) - 2 * near,
                     qPrintable(QString("%1: %2 to %3").arg(tilt).arg(*nearest).arg(*furthest)));
            QVERIFY(*furthest - *nearest > 4 * near);

            // Coronal: the block's own coronal cut leans with its slices,
            // so the straight stack is cut along the same plane at an
            // angle, which is nothing new for it.
            toVoxel(v, BoxCenter, voxel);
            const Cut coronal = cutAcross(v, 1);
            const int row = int(std::lround(voxel[1]));
            const Frame front = reformat(v, coronal, row, s.slices);
            const ImagePlane frontPlane = cutPlane(v, coronal, row);
            double middle[3];
            frontPlane.point(front.width / 2, front.height / 2, middle);
            const Oblique same = obliqueThrough(flat.volume, middle, frontPlane.row, frontPlane.column);
            QVERIFY(same.valid);
            const Frame flatFront = resample(flat.volume, same, same.middle, flat.slices, -1024, 3071);
            const ImagePlane flatFrontPlane = same.plane(same.middle);
            const Difference frontFromFlat = differenceFrom(front, frontPlane, [&](const double *p, double &expected) {
                return covered(v, p) && covered(flat.volume, p) && valueAt(flatFront, flatFrontPlane, p, expected);
            });
            const Difference frontFromShape = differenceFrom(front, frontPlane, [&](const double *p, double &expected) {
                expected = phantom(p);
                return covered(v, p);
            });
            QVERIFY(frontFromFlat.count > 2000);
            QVERIFY2(frontFromFlat.most < 160 && frontFromFlat.mean < 5, qPrintable(QString("%1: %2 %3").arg(tilt).arg(frontFromFlat.most).arg(frontFromFlat.mean)));
            QVERIFY2(frontFromShape.most < 160 && frontFromShape.mean < 5, qPrintable(QString("%1: %2 %3").arg(tilt).arg(frontFromShape.most).arg(frontFromShape.mean)));
            // The box's sides, left and right, stand straight up the
            // picture, at the same place all the way along the normal.
            double facing[3], inPlane[3];
            frontPlane.normal(facing);
            const double off = (BoxCenter[0] - frontPlane.origin[0]) * facing[0] + (BoxCenter[1] - frontPlane.origin[1]) * facing[1]
                               + (BoxCenter[2] - frontPlane.origin[2]) * facing[2];
            for (int a = 0; a < 3; ++a)
                inPlane[a] = BoxCenter[a] - off * facing[a];
            auto sample = [&](const double *p, double &value) { return valueAt(front, frontPlane, p, value); };
            for (double up : {-12.0, -6.0, 0.0, 6.0, 12.0})
                for (double way : {1.0, -1.0}) {
                    // From 8 mm outside the box, in towards it.
                    const double along[3] = {way, 0, 0};
                    double from[3];
                    for (int a = 0; a < 3; ++a)
                        from[a] = inPlane[a] + up * frontPlane.column[a] - (a == 0 ? way * (BoxHalf[0] + 8) : 0);
                    const double edge = crossing(sample, from, along, 16) - 8;
                    QVERIFY2(std::abs(edge) <= Fine, qPrintable(QString("%1: %2").arg(tilt).arg(edge)));
                }
        }
    }

    void measuresTrueLengthsThroughATiltedStack() {
        for (int tilt : {0, 15, -15, 30, -30}) {
            const Scanned &s = *scans.constFind(tilt);
            const Volume &v = s.volume, plain = plainBlock(s);
            const double near = halfVoxel(v), cosine = std::cos(tilt * M_PI / 180);

            // The rod, 72 mm long, in the sagittal cut along its middle,
            // measured as a ruler would: end to end in the picture, by the
            // picture's spacing.
            const double onRod[3] = {RodAt[0], RodAt[1], 0};
            double voxel[3], direction[2];
            toVoxel(v, onRod, voxel);
            const Cut cut = cutAcross(v, 0);
            const int column = int(std::lround(voxel[0]));
            const Frame picture = reformat(v, cut, column, s.slices);
            const ImagePlane plane = cutPlane(v, cut, column);
            const double length = lengthIn(picture, plane.rowSpacing, plane.columnSpacing, direction);
            QVERIFY2(std::abs(length - RodLength) <= Fine, qPrintable(QString("%1: %2 mm").arg(tilt).arg(length)));
            // It runs along the table, as it does in the patient.
            const double runs = direction[0] * plane.row[2] + direction[1] * plane.column[2];
            QVERIFY2(std::abs(runs) > std::cos(M_PI / 180), qPrintable(QString("%1: %2").arg(tilt).arg(runs)));

            // Read as a plain block, the same rod stands straight up the
            // picture and comes out short by the tilt's cosine: 2.5 mm at
            // 15 degrees, and nearly 10 mm at 30.
            const Frame sheared = reformat(plain, cutAcross(plain, 0), column, s.slices);
            const ImagePlane shearedPlane = cutPlane(plain, cutAcross(plain, 0), column);
            const double shortened = lengthIn(sheared, shearedPlane.rowSpacing, shearedPlane.columnSpacing, direction);
            QVERIFY2(std::abs(shortened - RodLength * cosine) <= near, qPrintable(QString("%1: %2 mm").arg(tilt).arg(shortened)));
            if (tilt != 0)
                QVERIFY(RodLength - shortened > 2 * near);

            // The box, 36 mm from bottom to top. Up the plain block's
            // coronal cut is up the table, so that height is what it
            // shows, and reads short the same way.
            toVoxel(plain, BoxCenter, voxel);
            const Cut plainCoronal = cutAcross(plain, 1);
            const Frame shearedFront = reformat(plain, plainCoronal, int(std::lround(voxel[1])), s.slices);
            const int across = int(std::lround(plainCoronal.flipHorizontal ? plain.size[0] - 1 - voxel[0] : voxel[0]));
            auto height = [&](const Frame &f, int x, double spacing) {
                // From where the column through the box's middle rises
                // past halfway to where it falls back.
                double first = -1, last = -1;
                for (int y = 1; y < f.height; ++y) {
                    const double a = f.value(x, y - 1), b = f.value(x, y);
                    if (a < Halfway && b >= Halfway)
                        first = y - 1 + (Halfway - a) / (b - a);
                    if (a >= Halfway && b < Halfway)
                        last = y - 1 + (Halfway - a) / (b - a);
                }
                return (last - first) * spacing;
            };
            const double plainHeight = height(shearedFront, across, plain.spacing[2]);
            QVERIFY2(std::abs(plainHeight - 2 * BoxHalf[2] * cosine) <= near, qPrintable(QString("%1: %2 mm").arg(tilt).arg(plainHeight)));

            // The block's own coronal cut goes up the slices' normal, and
            // along that the box is longer than it is tall: by one over the
            // cosine, as the picture says.
            toVoxel(v, BoxCenter, voxel);
            const Cut coronal = cutAcross(v, 1);
            const Frame front = reformat(v, coronal, int(std::lround(voxel[1])), s.slices);
            const ImagePlane frontPlane = cutPlane(v, coronal, int(std::lround(voxel[1])));
            const int through = int(std::lround(coronal.flipHorizontal ? v.size[0] - 1 - voxel[0] : voxel[0]));
            const double along = height(front, through, frontPlane.columnSpacing);
            QVERIFY2(std::abs(along - 2 * BoxHalf[2] / cosine) <= Fine, qPrintable(QString("%1: %2 mm").arg(tilt).arg(along)));

            // Its true height is there in the sagittal cut, measured up
            // the table whichever way the picture leans.
            toVoxel(v, BoxCenter, voxel);
            const Frame side = reformat(v, cut, int(std::lround(voxel[0])), s.slices);
            const ImagePlane sidePlane = cutPlane(v, cut, int(std::lround(voxel[0])));
            auto sample = [&](const double *p, double &value) { return valueAt(side, sidePlane, p, value); };
            const double below[3] = {sidePlane.origin[0], 0, -BoxHalf[2] - 8}, above[3] = {sidePlane.origin[0], 0, BoxHalf[2] + 8};
            const double up[3] = {0, 0, 1}, downwards[3] = {0, 0, -1};
            const double tall = (2 * BoxHalf[2] + 16) - crossing(sample, below, up, 16) - crossing(sample, above, downwards, 16);
            QVERIFY2(std::abs(tall - 2 * BoxHalf[2]) <= Fine, qPrintable(QString("%1: %2 mm").arg(tilt).arg(tall)));
        }
    }

    void laysASlabAcrossTheSlicesSquareToThem() {
        for (int tilt : Tilts) {
            const Scanned &s = *scans.constFind(tilt);
            const Volume &v = s.volume;
            const Cut cut = cutAcross(v, 2);
            const int index = Slices / 2, half = 6;
            const ImagePlane plane = slicePlane(s.series, index);

            // With no slab it is the slice, pixel for pixel.
            const Frame one = reformat(v, cut, index, s.slices);
            QCOMPARE(one.width, Columns);
            QCOMPARE(one.height, Rows);
            QCOMPARE(one.mono, s.slices[index].mono);

            // A slab's picture is the slice's size and lies where the slice
            // does, and each pixel has the most there is along the normal
            // through it: not down the leaning stack, pixel under pixel.
            const Frame slab = reformat(v, cut, index, s.slices, half, Projection::Max);
            QCOMPARE(slab.width, Columns);
            QCOMPARE(slab.height, Rows);
            auto alongTheNormal = [&](const double *p, double &expected) {
                expected = -1e9;
                for (int step = -half; step <= half; ++step) {
                    double q[3];
                    for (int a = 0; a < 3; ++a)
                        q[a] = p[a] + step * v.spacing[2] * v.axis[2][a];
                    if (!covered(v, q, 1))
                        return false;
                    expected = std::max(expected, phantom(q));
                }
                return true;
            };
            const Difference fromShape = differenceFrom(slab, plane, alongTheNormal);
            QVERIFY(fromShape.count > 1000);
            QVERIFY2(fromShape.most < 120 && fromShape.mean < 4, qPrintable(QString("%1: %2 %3").arg(tilt).arg(fromShape.most).arg(fromShape.mean)));
            Frame under = slab;
            for (int y = 0; y < Rows; ++y)
                for (int x = 0; x < Columns; ++x) {
                    int most = 0;
                    for (int k = index - half; k <= index + half; ++k)
                        most = std::max(most, s.slices[k].stored(x, y));
                    under.mono[y * Columns + x] = quint16(most);
                }
            const Difference plainFromShape = differenceFrom(under, plane, alongTheNormal);
            QVERIFY2(plainFromShape.most > 600 && plainFromShape.mean > 10 * fromShape.mean,
                     qPrintable(QString("%1: %2 %3").arg(tilt).arg(plainFromShape.most).arg(plainFromShape.mean)));

            // So the rod, which runs along the table and not along the
            // normal, is drawn out down the picture: by as far as it leans
            // across the slab's depth, on top of its own width as the
            // slices cut it. Pixel under pixel it would be a plain disc.
            double onRod[3] = {RodAt[0], RodAt[1], 0}, facing[3];
            plane.normal(facing);
            const double off = dot(onRod, facing) - dot(plane.origin, facing);
            onRod[2] -= off / facing[2];
            const double radians = std::abs(tilt) * M_PI / 180;
            auto drawnOut = [&](const Frame &f) {
                auto sample = [&](const double *p, double &value) { return valueAt(f, plane, p, value); };
                double from[3], back[3];
                for (int a = 0; a < 3; ++a) {
                    from[a] = onRod[a] - 20 * plane.column[a];
                    back[a] = -plane.column[a];
                }
                const double first = crossing(sample, from, plane.column, 20);
                for (int a = 0; a < 3; ++a)
                    from[a] = onRod[a] + 20 * plane.column[a];
                return 40 - first - crossing(sample, from, back, 20);
            };
            const double disc = 2 * RodRadius / std::cos(radians);
            QVERIFY2(std::abs(drawnOut(slab) - (disc + 2 * half * Step * std::sin(radians))) <= Fine, qPrintable(QString("%1: %2").arg(tilt).arg(drawnOut(slab))));
            QVERIFY2(std::abs(drawnOut(under) - disc) <= Fine, qPrintable(QString("%1: %2").arg(tilt).arg(drawnOut(under))));
        }
    }

    void cutsATiltedStackAtAnAngleAsAStraightOne() {
        // A plane off every axis, through the middle of the box.
        const double row[3] = {1, 0.3, 0.2}, column[3] = {-0.2, 0.5, 0.9};
        const Scanned &flat = straight();
        const Oblique reference = obliqueThrough(flat.volume, BoxCenter, row, column);
        QVERIFY(reference.valid);
        const Frame expectedPicture = resample(flat.volume, reference, reference.middle, flat.slices, -1024, 3071);
        const ImagePlane expectedPlane = reference.plane(reference.middle);
        for (int tilt : Tilts) {
            const Scanned &s = *scans.constFind(tilt);
            const Volume &v = s.volume, plain = plainBlock(s);
            const Oblique oblique = obliqueThrough(v, BoxCenter, row, column);
            QVERIFY(oblique.valid);
            const Frame picture = resample(v, oblique, oblique.middle, s.slices, -1024, 3071);
            const ImagePlane plane = oblique.plane(oblique.middle);
            QVERIFY(!picture.isNull());
            const Difference fromFlat = differenceFrom(picture, plane, [&](const double *p, double &expected) {
                return covered(v, p) && covered(flat.volume, p) && valueAt(expectedPicture, expectedPlane, p, expected);
            });
            const Difference fromShape = differenceFrom(picture, plane, [&](const double *p, double &expected) {
                expected = phantom(p);
                return covered(v, p);
            });
            QVERIFY(fromFlat.count > 2000);
            QVERIFY2(fromFlat.most < 160 && fromFlat.mean < 5, qPrintable(QString("%1: %2 %3").arg(tilt).arg(fromFlat.most).arg(fromFlat.mean)));
            QVERIFY2(fromShape.most < 260 && fromShape.mean < 6, qPrintable(QString("%1: %2 %3").arg(tilt).arg(fromShape.most).arg(fromShape.mean)));
            // Nothing is smeared out past the slices: where neither slice
            // either side reaches, the picture has the least there is.
            int nothing = 0;
            for (int y = 0; y < picture.height; ++y)
                for (int x = 0; x < picture.width; ++x) {
                    double p[3], voxel[3];
                    plane.point(x, y, p);
                    toVoxel(v, p, voxel);
                    if (voxel[2] < 0 || voxel[2] > Slices - 1)
                        continue;
                    bool reached = false;
                    for (int k : {int(std::floor(voxel[2])), int(std::ceil(voxel[2]))}) {
                        const double i = voxel[0] - v.shift[k].x(), j = voxel[1] - v.shift[k].y();
                        reached = reached || (i > -0.6 && j > -0.6 && i < Columns - 0.4 && j < Rows - 0.4);
                    }
                    if (reached)
                        continue;
                    QCOMPARE(picture.value(x, y), -1024.0);
                    ++nothing;
                }
            QVERIFY(nothing > 100);

            const Oblique plainOblique = obliqueThrough(plain, BoxCenter, row, column);
            const Frame sheared = resample(plain, plainOblique, plainOblique.middle, s.slices, -1024, 3071);
            const Difference plainFromShape = differenceFrom(sheared, plainOblique.plane(plainOblique.middle), [&](const double *p, double &expected) {
                expected = phantom(p);
                return covered(plain, p);
            });
            QVERIFY2(plainFromShape.most > 1000 && plainFromShape.mean > 10 * fromShape.mean,
                     qPrintable(QString("%1: %2 %3").arg(tilt).arg(plainFromShape.most).arg(plainFromShape.mean)));
        }
    }

    void packsATiltedStackAsAPlainBlock() {
        for (int tilt : Tilts) {
            const Scanned &s = *scans.constFind(tilt);
            const Volume &v = s.volume;
            const double near = halfVoxel(v);
            const VoxelGrid grid = packVolume(v, s.slices);
            QVERIFY(grid.valid);
            // The grid is the block, with no shift left for anything that
            // draws it to know about.
            QVERIFY(!grid.volume.tilted());
            QCOMPARE(grid.volume.columns(), v.size[0]);
            QCOMPARE(grid.volume.rows(), v.size[1]);
            QCOMPARE(grid.data.size(), size_t(v.size[0]) * v.size[1] * v.size[2]);
            for (int a = 0; a < 3; ++a) {
                QCOMPARE(grid.volume.size[a], v.size[a]);
                QCOMPARE(grid.volume.spacing[a], v.spacing[a]);
                QCOMPARE(grid.volume.origin[a], v.origin[a]);
            }
            auto sample = [&](const VoxelGrid &g) {
                return [&g](const double *p, double &value) {
                    double voxel[3];
                    toVoxel(g.volume, p, voxel);
                    return sampleGrid(g, voxel, value);
                };
            };

            // In it the box is a box. At each of its eight corners the
            // three faces that meet there are where the box has them: each
            // crossed 3 mm in from the other two, coming from 12 mm
            // outside. And just inside the corner is bone, just outside it
            // air. In the plain block the front and back faces, crossed
            // 15 mm above or below the middle, are each out by voxels, if
            // they are found at all, and the worst by more than the stack
            // leans in 15 mm.
            const VoxelGrid plainGrid = packVolume(plainBlock(s), s.slices);
            QVERIFY(plainGrid.valid);
            double plainWorst = 0;
            for (int corner = 0; corner < 8; ++corner) {
                double sign[3], at[3], inside[3], outside[3], value;
                for (int a = 0; a < 3; ++a) {
                    sign[a] = (corner >> a & 1) ? 1 : -1;
                    at[a] = BoxCenter[a] + sign[a] * BoxHalf[a];
                    inside[a] = at[a] - sign[a] * 4.6;
                    outside[a] = at[a] + sign[a] * 4.6;
                }
                for (int a = 0; a < 3; ++a) {
                    double from[3], along[3] = {0, 0, 0};
                    for (int b = 0; b < 3; ++b)
                        from[b] = b == a ? at[b] + sign[b] * 12 : at[b] - sign[b] * 3;
                    along[a] = -sign[a];
                    const double edge = crossing(sample(grid), from, along, 24) - 12;
                    QVERIFY2(std::abs(edge) <= Fine, qPrintable(QString("%1: corner %2 face %3: %4").arg(tilt).arg(corner).arg(a).arg(edge)));
                    if (a != 1)
                        continue;
                    const double plainEdge = crossing(sample(plainGrid), from, along, 24);
                    const double out = plainEdge < 0 ? 24 : std::abs(plainEdge - 12);
                    QVERIFY2(out > 3 * near, qPrintable(QString("%1: corner %2: %3").arg(tilt).arg(corner).arg(out)));
                    plainWorst = std::max(plainWorst, out);
                }
                QVERIFY(sample(grid)(inside, value));
                QVERIFY2(value > 950, qPrintable(QString("%1: corner %2 inside: %3").arg(tilt).arg(corner).arg(value)));
                QVERIFY(sample(grid)(outside, value));
                QVERIFY2(value < -950, qPrintable(QString("%1: corner %2 outside: %3").arg(tilt).arg(corner).arg(value)));
            }
            QVERIFY(plainWorst > 15 * std::tan(std::abs(tilt) * M_PI / 180));

            // Drawn from the front, there is something to see, and a click
            // on the middle of the box's front finds that face.
            View3D view;
            view.face(0);
            const QSize size(96, 96);
            const QImage image = renderVolume(grid, view, size);
            QCOMPARE(image.size(), size);
            int lit = 0;
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x)
                    lit += qGray(image.pixel(x, y)) > 40;
            QVERIFY2(lit > 300 && lit < size.width() * size.height() / 2, qPrintable(QString::number(lit)));
            const double front[3] = {BoxCenter[0], BoxCenter[1] - BoxHalf[1], BoxCenter[2]};
            const QPointF pixel = projectPoint(grid, view, size, front);
            QVERIFY(qGray(image.pixel(pixel.toPoint())) > 40);
            double found[3];
            QVERIFY(pickVolume(grid, view, size, pixel, found));
            QVERIFY2(std::abs(found[0] - front[0]) < 0.1 && std::abs(found[1] - front[1]) < 1.5 && std::abs(found[2] - front[2]) < 0.1,
                     qPrintable(QString("%1: %2 %3 %4").arg(tilt).arg(found[0]).arg(found[1]).arg(found[2])));
            QVERIFY(!pickVolume(grid, view, size, QPointF(2, 2), found));

            // Halved for a quick picture, it is still the same block.
            const VoxelGrid quick = halve(grid);
            const double above[3] = {BoxCenter[0], BoxCenter[1], BoxCenter[2] + BoxHalf[2] + 8}, down[3] = {0, 0, -1};
            QVERIFY(std::abs(crossing(sample(quick), above, down, 16) - 8) <= Fine);
        }
    }

    void laysAPetOverATiltedCt() {
        // A PET of the same patient, taken straight, in voxels 4 mm a side:
        // one hot ball, centred on the shape's small ball.
        QTemporaryDir folder;
        QVERIFY(writeTiltedSeries(folder.path(), "PT", "1.2.88.50", "1.2.88.1", 24, 24, 24, 4, 4, 0, [](const double *p) {
            const double away = std::sqrt(std::pow(p[0] - Marker[0], 2) + std::pow(p[1] - Marker[1], 2) + std::pow(p[2] - Marker[2], 2));
            return int(std::lround(10000 * solid(away - 8)));
        }));
        const Scanned pet = scanned(folder.path());
        QVERIFY(pet.volume.valid);
        const VoxelGrid grid = packVolume(pet.volume, pet.slices);
        QVERIFY(grid.valid);
        Overlay3D look;
        look.grid = &grid;
        look.low = 2000;
        look.high = 9000;
        look.opacity = 1;

        // Where the PET shows in a picture: the middle of the pixels it
        // changed.
        auto hotSpot = [&](const Frame &picture, const ImagePlane &plane, QPointF &at) {
            const QImage base = render(picture, 0, 2000, false).convertToFormat(QImage::Format_RGB32);
            const QImage fused = fuseSlice(base, plane, grid, look);
            double x = 0, y = 0;
            int changed = 0;
            for (int j = 0; j < fused.height(); ++j)
                for (int i = 0; i < fused.width(); ++i)
                    if (fused.pixel(i, j) != base.pixel(i, j)) {
                        x += i;
                        y += j;
                        ++changed;
                    }
            at = changed > 0 ? QPointF(x / changed, y / changed) : QPointF();
            return changed;
        };
        // Where a patient point is in a picture, in pixels.
        auto pixelOf = [](const ImagePlane &plane, const double *p) {
            const double d[3] = {p[0] - plane.origin[0], p[1] - plane.origin[1], p[2] - plane.origin[2]};
            return QPointF(dot(d, plane.row) / plane.rowSpacing, dot(d, plane.column) / plane.columnSpacing);
        };
        auto apart = [](QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); };

        for (auto it = scans.cbegin(); it != scans.cend(); ++it) {
            const int tilt = it.key();
            const Scanned &s = it.value();
            const Volume &v = s.volume, plain = plainBlock(s);
            double voxel[3];
            toVoxel(v, Marker, voxel);
            struct Shown {
                Frame picture;
                ImagePlane plane;
            };
            // The slice through the ball as it was scanned, a slab about
            // it, and the two cuts through it.
            const int slice = int(std::lround(voxel[2]));
            QList<Shown> shown{{s.slices[slice], slicePlane(s.series, slice)},
                               {reformat(v, cutAcross(v, 2), slice, s.slices, 3, Projection::Max), cutPlane(v, cutAcross(v, 2), slice)}};
            for (int fixed : {0, 1}) {
                const Cut cut = cutAcross(v, fixed);
                const int index = int(std::lround(voxel[fixed]));
                shown << Shown{reformat(v, cut, index, s.slices), cutPlane(v, cut, index)};
            }
            for (const Shown &one : std::as_const(shown)) {
                QPointF at;
                QVERIFY(hotSpot(one.picture, one.plane, at) > 50);
                // The hot spot is on the pixel the ball's middle is at, and
                // the CT under it there is the ball.
                QVERIFY2(apart(at, pixelOf(one.plane, Marker)) <= 0.5,
                         qPrintable(QString("%1: %2, %3").arg(tilt).arg(at.x()).arg(at.y())));
                QVERIFY(one.picture.value(int(std::lround(at.x())), int(std::lround(at.y()))) > 500);
            }
            if (tilt == 0)
                continue;
            // Over the plain block's cuts the PET lands where the block
            // says the ball is, and the ball isn't there: the CT under the
            // hot spot is air.
            toVoxel(plain, Marker, voxel);
            for (int fixed : {0, 1}) {
                const Cut cut = cutAcross(plain, fixed);
                const int index = int(std::lround(voxel[fixed]));
                const Frame picture = reformat(plain, cut, index, s.slices);
                QPointF at;
                QVERIFY(hotSpot(picture, cutPlane(plain, cut, index), at) > 50);
                QVERIFY(picture.value(int(std::lround(at.x())), int(std::lround(at.y()))) < -500);
            }
        }
    }

    void refusesAStackThatWandersOrLeansTooFar() {
        // Slices 2 mm apart, each as far to the side of the first as aside
        // says, in millimetres along the rows and down the columns.
        auto stack = [](int slices, int size, const std::function<QPointF(int)> &aside) {
            Series s;
            s.modality = "CT";
            s.rows = s.columns = size;
            s.hasSpacing = s.hasOrientation = true;
            for (int k = 0; k < slices; ++k) {
                Slice slice;
                slice.hasPosition = true;
                slice.position[0] = aside(k).x();
                slice.position[1] = aside(k).y();
                slice.position[2] = 2.0 * k;
                s.slices << slice;
            }
            return makeVolume(s);
        };
        auto leaning = [&](double degrees, int slices = 20, int size = 32) {
            const double each = 2 * std::tan(degrees * M_PI / 180);
            return stack(slices, size, [=](int k) { return QPointF(0, k * each); });
        };
        // As far as gantries lean, and a little over, either way.
        QVERIFY(leaning(0).valid && !leaning(0).tilted());
        for (double degrees : {5.0, 30.0, -30.0, 34.0, -34.0})
            QVERIFY2(leaning(degrees).valid && leaning(degrees).tilted(), qPrintable(QString::number(degrees)));
        // Further than any does: damaged, not tilted.
        for (double degrees : {36.0, -36.0, 45.0, -60.0, 89.0})
            QVERIFY2(!leaning(degrees).valid, qPrintable(QString::number(degrees)));

        // Within a pixel of straight is straight, as it always was.
        const Volume wobbly = stack(20, 32, [](int k) { return QPointF(0, k % 2 ? 0.4 : 0); });
        QVERIFY(wobbly.valid && !wobbly.tilted());
        // Within a pixel of the line it leans along is leaning, and each
        // slice is placed where it says it is.
        const Volume wobblyLean = stack(20, 32, [](int k) { return QPointF(0, k * 0.8 + (k % 2 ? 0.4 : 0)); });
        QVERIFY(wobblyLean.valid && wobblyLean.tilted());
        QVERIFY(std::abs(wobblyLean.shift[3].y() - (3 * 0.8 + 0.4)) < 1e-9);
        // Leaning both ways at once is still leaning.
        const Volume both = stack(20, 32, [](int k) { return QPointF(k * 0.5, k * -0.6); });
        QVERIFY(both.valid && both.tilted());
        QCOMPARE(both.size[0], 32 + 10);
        QCOMPARE(both.size[1], 32 + 12);
        QCOMPARE(both.shift[0], QPointF(0, 19 * 0.6));
        // One slice out of line, slices that go from side to side, and a
        // stack that bends: none is a stack a gantry took.
        QVERIFY(!stack(20, 32, [](int k) { return QPointF(0, k * 0.8 + (k == 7 ? 2.5 : 0)); }).valid);
        QVERIFY(!stack(20, 32, [](int k) { return QPointF(0, k % 2 ? 3 : 0); }).valid);
        QVERIFY(!stack(20, 32, [](int k) { return QPointF(k % 3 == 0 ? 2 : 0, k * 0.8); }).valid);
        QVERIFY(!stack(20, 32, [](int k) { return QPointF(0, 0.02 * k * k); }).valid);
        // And a lean within a gantry's, of slices so small that the block
        // to hold them would be mostly nothing.
        QVERIFY(leaning(30, 20, 32).valid);
        QVERIFY(!leaning(30, 60, 4).valid);
        // Positions the slices' own directions can't account for: a normal
        // that isn't a millimetre long stretches the stack off its line.
        Series stretched;
        stretched.rows = stretched.columns = 32;
        stretched.hasSpacing = stretched.hasOrientation = true;
        stretched.orientation[0] = 1.02;
        for (int k = 0; k < 40; ++k) {
            Slice slice;
            slice.hasPosition = true;
            slice.position[2] = 2.0 * k;
            stretched.slices << slice;
        }
        QVERIFY(!makeVolume(stretched).valid);
    }

    void keepsPaddingAndNothingApart() {
        // Three slices of 4 by 4, each half a pixel further down than the
        // one before, their left column padding (the scanner's mark for
        // where it didn't scan) and the rest 100, 200, and 300.
        Volume v;
        v.valid = true;
        v.size[0] = 4;
        v.size[1] = 5;
        v.size[2] = 3;
        v.slice[0] = v.slice[1] = 4;
        v.shift = {QPointF(0, 0), QPointF(0, 0.5), QPointF(0, 1)};
        v.empty = -1000;
        QVector<Frame> slices;
        for (int k = 0; k < 3; ++k) {
            Frame f;
            f.width = f.height = 4;
            f.isSigned = true;
            f.hasPadding = true;
            f.padding = -2000;
            f.mono.resize(16);
            for (int i = 0; i < 16; ++i)
                f.mono[i] = quint16(qint16(i % 4 == 0 ? -2000 : 100 * (k + 1)));
            slices << f;
        }
        // Down the columns: each picture has a row for each slice, the
        // slices' own values in it, their padding still padding, and
        // padding too where a slice doesn't reach, never a mix of either
        // with a value.
        const Cut cut = cutAcross(v, 1);
        for (int index = 0; index < 5; ++index) {
            const Frame picture = reformat(v, cut, index, slices);
            QVERIFY(picture.hasPadding);
            QCOMPARE(picture.padding, -2000);
            for (int y = 0; y < picture.height; ++y)
                for (int x = 0; x < picture.width; ++x) {
                    const int k = cut.flipVertical ? 2 - y : y, i = cut.flipHorizontal ? 3 - x : x;
                    const double own = index - v.shift[k].y();
                    const bool reached = own >= -0.5 && own <= 3.5;
                    QCOMPARE(picture.stored(x, y), i == 0 || !reached ? -2000 : 100 * (k + 1));
                }
        }
        // Without padding, nothing is air: what the volume's empty value
        // is stored as.
        for (Frame &f : slices) {
            f.hasPadding = false;
            f.intercept = -24;
        }
        const Frame bare = reformat(v, cut, 4, slices);
        QVERIFY(!bare.hasPadding);
        QCOMPARE(bare.stored(1, cut.flipVertical ? 2 : 0), -976);
        QCOMPARE(bare.stored(1, cut.flipVertical ? 0 : 2), 300);

        // Packed, padding and nothing are both the least there is.
        for (Frame &f : slices)
            f.hasPadding = true;
        const VoxelGrid grid = packVolume(v, slices);
        QVERIFY(grid.valid);
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 5; ++j)
                for (int i = 0; i < 4; ++i) {
                    const double own = j - v.shift[k].y();
                    const bool empty = i == 0 || own < -0.5 || own > 3.5;
                    const double value = grid.value(grid.data[(size_t(k) * 5 + j) * 4 + i]);
                    QVERIFY(std::abs(value - (empty ? grid.low : 100 * (k + 1) - 24)) < 0.01);
                }
        QCOMPARE(grid.low, 76.0);
    }

    void readsAStraightStackAsThePlainBlockItIs() {
        // A stack with no tilt is what it always was: voxel (i, j, k) is
        // pixel (i, j) of slice k and nothing else, in every picture made
        // of it.
        const Scanned &s = straight();
        const Volume &v = s.volume;
        QVERIFY(v.valid);
        QVERIFY(!v.tilted());
        QVERIFY(v.shift.isEmpty());
        for (int a = 0; a < 3; ++a)
            QCOMPARE(v.origin[a], s.series.slices[0].position[a]);
        QCOMPARE(v.size[0], Columns);
        QCOMPARE(v.size[1], Rows);

        double low = 1e9, high = -1e9;
        for (const Frame &f : s.slices) {
            double a, b;
            f.range(a, b);
            low = std::min(low, a);
            high = std::max(high, b);
        }
        for (int fixed = 0; fixed < 3; ++fixed) {
            const Cut cut = cutAcross(v, fixed);
            const int width = cut.width(v), height = cut.height(v);
            QCOMPARE(width, v.size[cut.horizontal]);
            QCOMPARE(height, v.size[cut.vertical]);
            for (int index : {0, 20, cut.count(v) - 1})
                for (int half : {0, 3})
                    for (Projection projection : {Projection::Max, Projection::Min, Projection::Mean}) {
                        const Frame picture = reformat(v, cut, index, s.slices, half, projection);
                        QCOMPARE(picture.width, width);
                        QCOMPARE(picture.height, height);
                        const int first = std::max(0, index - half), last = std::min(cut.count(v) - 1, index + half);
                        const bool averaged = projection == Projection::Mean && first != last;
                        const double slope = (high - low) / 65535;
                        int at[3];
                        for (int y = 0; y < height; ++y)
                            for (int x = 0; x < width; ++x) {
                                at[cut.horizontal] = cut.flipHorizontal ? width - 1 - x : x;
                                at[cut.vertical] = cut.flipVertical ? height - 1 - y : y;
                                int most = INT_MIN, least = INT_MAX;
                                double sum = 0;
                                for (int k = first; k <= last; ++k) {
                                    at[fixed] = k;
                                    const Frame &f = s.slices[at[2]];
                                    most = std::max(most, f.stored(at[0], at[1]));
                                    least = std::min(least, f.stored(at[0], at[1]));
                                    sum += f.value(at[0], at[1]);
                                }
                                const int expected = averaged ? int(std::lround((sum / (last - first + 1) - low) / slope))
                                                     : projection == Projection::Min ? least : most;
                                if (first == last)
                                    QCOMPARE(most, least);
                                QCOMPARE(int(picture.mono[y * width + x]), expected);
                            }
                    }
        }

        // At an angle: each pixel weighed from the eight voxels around its
        // point, pixel (i, j) of slice k each.
        const double row[3] = {1, 0.3, 0.2}, column[3] = {-0.2, 0.5, 0.9};
        const Oblique oblique = obliqueThrough(v, BoxCenter, row, column);
        QVERIFY(oblique.valid);
        const Frame picture = resample(v, oblique, oblique.middle, s.slices, -1024, 3071);
        const ImagePlane plane = oblique.plane(oblique.middle);
        int weighed = 0;
        for (int y = 0; y < picture.height; ++y)
            for (int x = 0; x < picture.width; ++x) {
                double p[3], voxel[3];
                plane.point(x, y, p);
                toVoxel(v, p, voxel);
                double expected = -1024;
                if (voxel[0] >= -0.5 + 1e-9 && voxel[1] >= -0.5 + 1e-9 && voxel[2] >= -0.5 + 1e-9 && voxel[0] <= Columns - 0.5 - 1e-9
                    && voxel[1] <= Rows - 0.5 - 1e-9 && voxel[2] <= Slices - 0.5 - 1e-9) {
                    const double i = std::clamp(voxel[0], 0.0, Columns - 1.0), j = std::clamp(voxel[1], 0.0, Rows - 1.0),
                                 k = std::clamp(voxel[2], 0.0, Slices - 1.0);
                    const int i0 = std::min(int(i), Columns - 1), j0 = std::min(int(j), Rows - 1), k0 = std::min(int(k), Slices - 1);
                    const int i1 = std::min(i0 + 1, Columns - 1), j1 = std::min(j0 + 1, Rows - 1), k1 = std::min(k0 + 1, Slices - 1);
                    const double fi = i - i0, fj = j - j0, fk = k - k0;
                    auto at = [&](int kk, int jj, int ii) { return s.slices[kk].value(ii, jj); };
                    const double a = at(k0, j0, i0) * (1 - fi) + at(k0, j0, i1) * fi, b = at(k0, j1, i0) * (1 - fi) + at(k0, j1, i1) * fi;
                    const double c = at(k1, j0, i0) * (1 - fi) + at(k1, j0, i1) * fi, d = at(k1, j1, i0) * (1 - fi) + at(k1, j1, i1) * fi;
                    expected = (a * (1 - fj) + b * fj) * (1 - fk) + (c * (1 - fj) + d * fj) * fk;
                    ++weighed;
                } else if (std::abs(voxel[0] + 0.5) < 1e-6 || std::abs(voxel[1] + 0.5) < 1e-6 || std::abs(voxel[2] + 0.5) < 1e-6
                           || std::abs(voxel[0] - Columns + 0.5) < 1e-6 || std::abs(voxel[1] - Rows + 0.5) < 1e-6
                           || std::abs(voxel[2] - Slices + 0.5) < 1e-6) {
                    continue;
                }
                // The same but for which way a value exactly between two
                // of the picture's steps is rounded.
                const double steps = (expected + 1024) / picture.slope;
                QVERIFY2(std::abs(picture.mono[y * picture.width + x] - steps) <= 0.5 + 1e-6,
                         qPrintable(QString("%1, %2: %3, not %4").arg(x).arg(y).arg(picture.mono[y * picture.width + x]).arg(steps)));
            }
        QVERIFY(weighed > 2000);

        // Packed: each voxel its own pixel, on the grid's scale.
        const VoxelGrid grid = packVolume(v, s.slices);
        QVERIFY(grid.valid);
        QCOMPARE(grid.low, low);
        QCOMPARE(grid.high, high);
        QCOMPARE(grid.data.size(), size_t(Columns) * Rows * Slices);
        const double factor = 65535 / (high - low);
        for (int k = 0; k < Slices; ++k)
            for (int j = 0; j < Rows; ++j)
                for (int i = 0; i < Columns; ++i) {
                    const double packed = (s.slices[k].value(i, j) - low) * factor + 0.5;
                    const int got = grid.data[(size_t(k) * Rows + j) * Columns + i];
                    QVERIFY(packed - got > -1e-6 && packed - got < 1 + 1e-6);
                }
    }
};

QTEST_GUILESS_MAIN(GeometryTests)
#include "geometry_tests.moc"
