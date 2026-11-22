#include "volumerender.h"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <thread>

namespace {

double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void normalize(double *v) {
    const double length = std::sqrt(dot3(v, v));
    if (length > 1e-12)
        for (int i = 0; i < 3; ++i)
            v[i] /= length;
}

void cross3(const double *a, const double *b, double *out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// Runs rows of work on every core, each row once.
template <typename Work> void eachRow(int rows, Work work) {
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 32);
    std::atomic<int> next{0};
    auto run = [&] {
        for (int row = next++; row < rows; row = next++)
            work(row);
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t)
        pool.emplace_back(run);
    run();
    for (auto &t : pool)
        t.join();
}

void blockRanges(VoxelGrid &g) {
    const int B = VoxelGrid::Block;
    const int nx = g.volume.size[0], ny = g.volume.size[1], nz = g.volume.size[2];
    for (int a = 0; a < 3; ++a)
        g.blocks[a] = (g.volume.size[a] + B - 1) / B;
    const size_t count = size_t(g.blocks[0]) * g.blocks[1] * g.blocks[2];
    g.blockLow.assign(count, 65535);
    g.blockHigh.assign(count, 0);
    eachRow(g.blocks[2], [&](int bz) {
        for (int by = 0; by < g.blocks[1]; ++by)
            for (int bx = 0; bx < g.blocks[0]; ++bx) {
                quint16 lo = 65535, hi = 0;
                // One voxel past the block, which sampling between voxels reaches.
                for (int k = bz * B; k <= std::min(nz - 1, bz * B + B); ++k)
                    for (int j = by * B; j <= std::min(ny - 1, by * B + B); ++j) {
                        const quint16 *row = g.data.data() + (size_t(k) * ny + j) * nx;
                        for (int i = bx * B; i <= std::min(nx - 1, bx * B + B); ++i) {
                            lo = std::min(lo, row[i]);
                            hi = std::max(hi, row[i]);
                        }
                    }
                const size_t at = (size_t(bz) * g.blocks[1] + by) * g.blocks[0] + bx;
                g.blockLow[at] = lo;
                g.blockHigh[at] = hi;
            }
    });
}

struct Stop {
    double value;
    float r, g, b, alpha;    // Alpha is the opacity of one millimetre.
};

// Each look's colours and opacities by Hounsfield units, or for a surface
// by the window: 0 its bottom, 1 its top.
QVector<Stop> stops(Look look) {
    switch (look) {
    case Look::Bone:
        return {{160, 0.85f, 0.75f, 0.6f, 0}, {280, 0.93f, 0.86f, 0.74f, 0.5f}, {600, 0.98f, 0.95f, 0.88f, 0.9f},
                {3000, 1, 1, 0.97f, 0.95f}};
    case Look::Vessels:
        return {{150, 0.7f, 0.1f, 0.08f, 0}, {200, 0.85f, 0.2f, 0.15f, 0.25f}, {350, 1, 0.55f, 0.45f, 0.6f},
                {600, 1, 0.95f, 0.88f, 0.85f}, {3000, 1, 1, 0.97f, 0.95f}};
    case Look::Skin:
        return {{-600, 0.9f, 0.62f, 0.5f, 0}, {-300, 0.92f, 0.66f, 0.55f, 0.08f}, {-50, 0.95f, 0.74f, 0.64f, 0.6f},
                {3000, 0.95f, 0.78f, 0.7f, 0.7f}};
    case Look::Lungs:
        return {{-980, 0.55f, 0.7f, 0.95f, 0}, {-900, 0.6f, 0.75f, 0.98f, 0.015f}, {-650, 0.75f, 0.85f, 1, 0.05f},
                {-450, 0.85f, 0.9f, 1, 0}};
    case Look::Surface:
        return {{0, 0.92f, 0.86f, 0.8f, 0}, {0.5, 0.94f, 0.9f, 0.85f, 0.35f}, {1, 1, 0.97f, 0.94f, 0.7f}};
    case Look::Mip:
        break;
    }
    return {};
}

// What every packed value looks like, sampled a step apart.
struct Lut {
    static constexpr int Bins = 4096;      // 16 packed values a bin.
    std::vector<float> rgba;               // Premultiplied.
    std::vector<int> seen;                 // Bins up to each with any opacity.
    bool any(quint16 lo, quint16 hi) const { return seen[(hi >> 4) + 1] - seen[lo >> 4] > 0; }
};

Lut makeLut(const VoxelGrid &grid, const View3D &view, double stepMm) {
    Lut lut;
    lut.rgba.assign(Lut::Bins * 4, 0);
    lut.seen.assign(Lut::Bins + 1, 0);
    const QVector<Stop> s = stops(view.look);
    const bool windowed = view.look == Look::Surface;
    const double bottom = view.center - view.width / 2;
    for (int bin = 0; bin < Lut::Bins; ++bin) {
        double v = grid.value((bin + 0.5) * 16) - view.shift;
        if (windowed)
            v = (v - bottom) / std::max(1e-9, view.width);
        float c[4] = {0, 0, 0, 0};
        if (!s.isEmpty() && v > s.first().value) {
            int i = 1;
            while (i < s.size() && v > s[i].value)
                ++i;
            if (i >= s.size()) {
                c[0] = s.last().r, c[1] = s.last().g, c[2] = s.last().b, c[3] = s.last().alpha;
            } else {
                const Stop &a = s[i - 1], &b = s[i];
                const float f = float((v - a.value) / (b.value - a.value));
                c[0] = a.r + (b.r - a.r) * f;
                c[1] = a.g + (b.g - a.g) * f;
                c[2] = a.b + (b.b - a.b) * f;
                c[3] = a.alpha + (b.alpha - a.alpha) * f;
            }
        }
        // As opaque as that much of a millimetre's worth.
        const float alpha = c[3] <= 0 ? 0 : float(1 - std::pow(1 - std::min(0.999f, c[3]), stepMm));
        for (int k = 0; k < 3; ++k)
            lut.rgba[bin * 4 + k] = c[k] * alpha;
        lut.rgba[bin * 4 + 3] = alpha;
        lut.seen[bin + 1] = lut.seen[bin] + (alpha > 0.0005f ? 1 : 0);
    }
    return lut;
}

// The value between voxels, by how near each is; positions in voxels.
inline float sampleAt(const VoxelGrid &g, float x, float y, float z) {
    const int nx = g.volume.size[0], ny = g.volume.size[1], nz = g.volume.size[2];
    x = std::clamp(x, 0.f, float(nx - 1));
    y = std::clamp(y, 0.f, float(ny - 1));
    z = std::clamp(z, 0.f, float(nz - 1));
    const int x0 = std::min(int(x), nx - 1), y0 = std::min(int(y), ny - 1), z0 = std::min(int(z), nz - 1);
    const int dx = x0 + 1 < nx ? 1 : 0;
    const size_t dy = y0 + 1 < ny ? nx : 0, dz = z0 + 1 < nz ? size_t(nx) * ny : 0;
    const float fx = x - x0, fy = y - y0, fz = z - z0;
    const quint16 *p = g.data.data() + (size_t(z0) * ny + y0) * nx + x0;
    const float a = p[0] + (p[dx] - float(p[0])) * fx;
    const float b = p[dy] + (p[dy + dx] - float(p[dy])) * fx;
    const float c = p[dz] + (p[dz + dx] - float(p[dz])) * fx;
    const float d = p[dz + dy] + (p[dz + dy + dx] - float(p[dz + dy])) * fx;
    const float ab = a + (b - a) * fy, cd = c + (d - c) * fy;
    return ab + (cd - ab) * fz;
}

// A ray through the grid, in voxels: where it starts, how far one step
// goes, and the steps that are inside.
struct Ray {
    double start[3], delta[3];
    double first = 0, last = -1;
};

struct Frame3D {
    double pixel = 1;       // Millimetres a pixel.
    double depth = 0;       // Millimetres from the front to the back.
};

Frame3D frameFor(const VoxelGrid &grid, const View3D &view, QSize size) {
    Frame3D f;
    f.pixel = 2 * grid.fit / (std::max(1, std::min(size.width(), size.height())) * std::max(0.05, view.zoom));
    f.depth = 2 * grid.radius;
    return f;
}

// The patient point on the plane through the centre square to the view.
void screenPoint(const VoxelGrid &grid, const View3D &view, QSize size, double px, double py, double *p) {
    const Frame3D f = frameFor(grid, view, size);
    const double u = (px - size.width() / 2.0) * f.pixel - view.pan[0];
    const double v = (size.height() / 2.0 - py) * f.pixel - view.pan[1];
    for (int i = 0; i < 3; ++i)
        p[i] = grid.center[i] + view.right[i] * u + view.up[i] * v;
}

Ray rayThrough(const VoxelGrid &grid, const VoxelGrid &target, const View3D &view, QSize size, double px, double py,
               double stepMm) {
    Ray r;
    double p[3];
    screenPoint(grid, view, size, px, py, p);
    const double front = grid.radius - view.cut;
    double a[3], b[3];
    for (int i = 0; i < 3; ++i) {
        a[i] = p[i] + view.toward[i] * front;
        b[i] = a[i] - view.toward[i] * stepMm;
    }
    double va[3], vb[3];
    toVoxel(target.volume, a, va);
    toVoxel(target.volume, b, vb);
    for (int i = 0; i < 3; ++i) {
        r.start[i] = va[i];
        r.delta[i] = vb[i] - va[i];
    }
    // Only the steps inside the volume's box, and in front of the back.
    double lo = 0, hi = (grid.radius + front) / stepMm;
    for (int i = 0; i < 3; ++i) {
        const double min = -0.5, max = target.volume.size[i] - 0.5;
        if (std::abs(r.delta[i]) < 1e-12) {
            if (r.start[i] < min || r.start[i] > max)
                return r;
            continue;
        }
        double t0 = (min - r.start[i]) / r.delta[i], t1 = (max - r.start[i]) / r.delta[i];
        if (t0 > t1)
            std::swap(t0, t1);
        lo = std::max(lo, t0);
        hi = std::min(hi, t1);
    }
    r.first = std::ceil(lo);
    r.last = std::floor(hi);
    return r;
}

double minSpacing(const VoxelGrid &g) {
    return std::min({g.volume.spacing[0], g.volume.spacing[1], g.volume.spacing[2]});
}

// How far apart a ray's samples are, in millimetres: step of the finest
// voxel's size. But a ray crosses no more voxels than the grid has along its
// three sides together, and two samples to each of those is as fine as
// there is anything to see: a stack whose slices lie a long way apart next
// to its pixels' size (or whose file says they do) would otherwise be
// walked through at its pixels' pace, a ray taking hours.
double rayStep(const VoxelGrid &g, double step) {
    const double crossed = double(g.volume.size[0]) + g.volume.size[1] + g.volume.size[2];
    return step * std::max(minSpacing(g), g.radius / crossed);
}

} // namespace

const char *lookName(Look look) {
    switch (look) {
    case Look::Bone: return QT_TRANSLATE_NOOP("dicolens", "Bone");
    case Look::Vessels: return QT_TRANSLATE_NOOP("dicolens", "Vessels");
    case Look::Skin: return QT_TRANSLATE_NOOP("dicolens", "Skin");
    case Look::Lungs: return QT_TRANSLATE_NOOP("dicolens", "Lungs");
    case Look::Surface: return QT_TRANSLATE_NOOP("dicolens", "Surface");
    case Look::Mip: return QT_TRANSLATE_NOOP("dicolens", "MIP");
    }
    return "";
}

void View3D::turn(double across, double down) {
    // Dragging across spins about the screen's up axis, and down about its
    // right one, as if the volume were held by its front.
    const double upAxis[3] = {up[0], up[1], up[2]};
    turnAbout(upAxis, across, right);
    turnAbout(upAxis, across, toward);
    const double rightAxis[3] = {right[0], right[1], right[2]};
    turnAbout(rightAxis, down, up);
    turnAbout(rightAxis, down, toward);
    // Kept square, however many turns.
    normalize(toward);
    cross3(up, toward, right);
    normalize(right);
    cross3(toward, right, up);
    normalize(up);
}

void View3D::face(int side) {
    static const double sides[][9] = {
        {1, 0, 0, 0, 0, 1, 0, -1, 0},     // From the front.
        {-1, 0, 0, 0, 0, 1, 0, 1, 0},     // From the back.
        {0, 1, 0, 0, 0, 1, 1, 0, 0},      // From the patient's left.
        {0, -1, 0, 0, 0, 1, -1, 0, 0},    // From the patient's right.
        {1, 0, 0, 0, -1, 0, 0, 0, 1},     // From the head.
    };
    const double *s = sides[std::clamp(side, 0, 4)];
    for (int i = 0; i < 3; ++i) {
        right[i] = s[i];
        up[i] = s[3 + i];
        toward[i] = s[6 + i];
    }
}

VoxelGrid packVolume(const Volume &volume, const QVector<Frame> &slices, double scale) {
    VoxelGrid g;
    const int nx = volume.size[0], ny = volume.size[1], nz = volume.size[2];
    if (!volume.valid || slices.size() != nz || nx <= 0 || ny <= 0 || nz <= 0)
        return g;
    const bool tilted = volume.tilted();
    if (tilted && volume.shift.size() != nz)
        return g;
    bool first = true;
    for (const Frame &f : slices) {
        if (f.isNull() || f.isColor() || !f.error.isEmpty() || f.width != volume.columns() || f.height != volume.rows())
            return g;
        double lo, hi;
        f.range(lo, hi);
        lo *= scale;
        hi *= scale;
        if (scale < 0)
            std::swap(lo, hi);
        g.low = first ? lo : std::min(g.low, lo);
        g.high = first ? hi : std::max(g.high, hi);
        first = false;
    }
    // Values a scale (a made-up SUV factor, say) has taken past what a
    // number holds have no range to pack them into.
    if (!std::isfinite(g.low) || !std::isfinite(g.high) || !std::isfinite(g.high - g.low))
        return g;
    if (g.high <= g.low)
        g.high = g.low + 1;
    // A tilted stack's slices are put where they lie in the block as they
    // are packed, so the grid itself is a plain block with every voxel in
    // its place, and nothing that draws it or looks a value up in it has a
    // shift to know about.
    g.volume = volume;
    g.volume.shift.clear();
    g.volume.slice[0] = g.volume.slice[1] = 0;
    g.data.resize(size_t(nx) * ny * nz);
    // (So big a value that one more is no more has a span of nothing.)
    const double factor = g.high > g.low ? 65535 / (g.high - g.low) : 0;
    eachRow(nz, [&](int k) {
        const Frame &f = slices[k];
        quint16 *out = g.data.data() + size_t(k) * nx * ny;
        if (tilted) {
            // What the slice has at each voxel of its layer; where it
            // doesn't reach is as empty as padding is.
            const QPointF shift = volume.shift[k];
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i) {
                    double stored, v = g.low;
                    if (sampleSlice(f, i - shift.x(), j - shift.y(), stored) && !(f.hasPadding && stored == f.padding))
                        v = (stored * f.slope + f.intercept) * scale;
                    out[size_t(j) * nx + i] = quint16(std::clamp((v - g.low) * factor + 0.5, 0.0, 65535.0));
                }
            return;
        }
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const int stored = f.stored(i, j);
                const double v = f.hasPadding && stored == f.padding ? g.low : (stored * f.slope + f.intercept) * scale;
                out[size_t(j) * nx + i] = quint16(std::clamp((v - g.low) * factor + 0.5, 0.0, 65535.0));
            }
    });
    // Fitted to the whole box, whichever way it's turned.
    double c[3] = {(nx - 1) / 2.0, (ny - 1) / 2.0, (nz - 1) / 2.0};
    toPatient(volume, c, g.center);
    g.radius = 0;
    for (int a = 0; a < 3; ++a)
        g.radius += std::pow(volume.size[a] * volume.spacing[a], 2);
    g.radius = std::max(1.0, std::sqrt(g.radius) / 2);
    g.fit = 0;
    for (int a = 0; a < 3; ++a)
        g.fit = std::max(g.fit, volume.size[a] * volume.spacing[a] / 2 * 1.1);
    g.fit = std::min(g.fit, g.radius);
    blockRanges(g);
    g.valid = true;
    return g;
}

VoxelGrid halve(const VoxelGrid &grid) {
    VoxelGrid h;
    if (!grid.valid)
        return h;
    h.volume = grid.volume;
    int factor[3];
    for (int a = 0; a < 3; ++a) {
        factor[a] = grid.volume.size[a] >= 4 ? 2 : 1;
        h.volume.size[a] = grid.volume.size[a] / factor[a];
        h.volume.spacing[a] = grid.volume.spacing[a] * factor[a];
    }
    // Each new voxel sits in the middle of those it covers.
    double shift[3] = {(factor[0] - 1) / 2.0, (factor[1] - 1) / 2.0, (factor[2] - 1) / 2.0};
    toPatient(grid.volume, shift, h.volume.origin);
    h.low = grid.low;
    h.high = grid.high;
    for (int i = 0; i < 3; ++i)
        h.center[i] = grid.center[i];
    h.radius = grid.radius;
    h.fit = grid.fit;
    const int nx = h.volume.size[0], ny = h.volume.size[1], nz = h.volume.size[2];
    const int sx = grid.volume.size[0], sy = grid.volume.size[1];
    h.data.resize(size_t(nx) * ny * nz);
    eachRow(nz, [&](int k) {
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                quint32 sum = 0;
                for (int dz = 0; dz < factor[2]; ++dz)
                    for (int dy = 0; dy < factor[1]; ++dy)
                        for (int dx = 0; dx < factor[0]; ++dx)
                            sum += grid.data[(size_t(k * factor[2] + dz) * sy + j * factor[1] + dy) * sx + i * factor[0] + dx];
                h.data[(size_t(k) * ny + j) * nx + i] = quint16(sum / (factor[0] * factor[1] * factor[2]));
            }
    });
    blockRanges(h);
    h.valid = true;
    return h;
}

QImage renderVolume(const VoxelGrid &grid, const View3D &view, QSize size, double step, const Overlay3D *overlay,
                    const std::atomic<bool> *cancel) {
    QImage image(size, QImage::Format_RGB32);
    if (size.isEmpty())
        return image;
    image.fill(Qt::black);
    if (!grid.valid)
        return image;
    const double stepMm = rayStep(grid, std::max(0.1, step));
    const bool mip = view.look == Look::Mip;
    const Lut lut = mip ? Lut() : makeLut(grid, view, stepMm);
    const int nx = grid.volume.size[0], ny = grid.volume.size[1];
    const int B = VoxelGrid::Block;

    // Which blocks have anything to see in this look.
    std::vector<char> empty;
    if (!mip) {
        empty.resize(grid.blockLow.size());
        for (size_t b = 0; b < empty.size(); ++b)
            empty[b] = !lut.any(grid.blockLow[b], grid.blockHigh[b]);
    }

    // A light from over the viewer's shoulder, and the way to the viewer.
    double light[3];
    for (int i = 0; i < 3; ++i)
        light[i] = view.toward[i] + 0.35 * view.up[i] - 0.25 * view.right[i];
    normalize(light);
    double halfway[3];
    for (int i = 0; i < 3; ++i)
        halfway[i] = light[i] + view.toward[i];
    normalize(halfway);
    // From a gradient in voxels to one in the patient.
    double toWorld[3][3];
    for (int a = 0; a < 3; ++a)
        for (int i = 0; i < 3; ++i)
            toWorld[a][i] = grid.volume.axis[a][i] / grid.volume.spacing[a];

    // A MIP and the PET's colours both go through the series' colour scales.
    QRgb scaleTable[3][256];
    for (int m = 0; m < 3; ++m)
        for (int g = 0; g < 256; ++g)
            scaleTable[m][g] = colorScale(m + 1, g);
    auto scaled = [&](int map, int g) -> QRgb {
        if (map <= 0)
            return qRgb(g, g, g);
        return scaleTable[std::clamp(map, 1, 3) - 1][std::clamp(g, 0, 255)];
    };

    const double bottom = grid.packed(view.center - view.width / 2);
    const double span = std::max(1e-9, grid.packed(view.center + view.width / 2) - bottom);

    bool stopped = false;
    eachRow(size.height(), [&](int y) {
        if (stopped || (cancel && cancel->load())) {
            stopped = true;
            return;
        }
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const Ray r = rayThrough(grid, grid, view, size, x + 0.5, y + 0.5, stepMm);
            float red = 0, green = 0, blue = 0, alpha = 0;
            if (mip) {
                float most = -1;
                for (double t = r.first; t <= r.last; ++t) {
                    const float v = sampleAt(grid, float(r.start[0] + t * r.delta[0]), float(r.start[1] + t * r.delta[1]),
                                             float(r.start[2] + t * r.delta[2]));
                    most = std::max(most, v);
                }
                if (most >= 0) {
                    int g = int(std::clamp((most - bottom) / span, 0.0, 1.0) * 255 + 0.5);
                    if (view.inverted)
                        g = 255 - g;
                    const QRgb c = scaled(view.colorMap, g);
                    red = qRed(c) / 255.f, green = qGreen(c) / 255.f, blue = qBlue(c) / 255.f;
                    alpha = 1;
                }
            } else {
                for (double t = r.first; t <= r.last && alpha < 0.98f; ++t) {
                    const float px = float(r.start[0] + t * r.delta[0]), py = float(r.start[1] + t * r.delta[1]),
                                pz = float(r.start[2] + t * r.delta[2]);
                    const int bx = std::clamp(int(px + 0.5f), 0, nx - 1) / B, by = std::clamp(int(py + 0.5f), 0, ny - 1) / B,
                              bz = std::clamp(int(pz + 0.5f), 0, grid.volume.size[2] - 1) / B;
                    if (empty[(size_t(bz) * grid.blocks[1] + by) * grid.blocks[0] + bx])
                        continue;
                    const float v = sampleAt(grid, px, py, pz);
                    const float *c = &lut.rgba[(std::min(65535, int(v)) >> 4) * 4];
                    if (c[3] <= 0.0005f)
                        continue;
                    // Lit by the surface the values make there.
                    const float g[3] = {sampleAt(grid, px + 1, py, pz) - sampleAt(grid, px - 1, py, pz),
                                        sampleAt(grid, px, py + 1, pz) - sampleAt(grid, px, py - 1, pz),
                                        sampleAt(grid, px, py, pz + 1) - sampleAt(grid, px, py, pz - 1)};
                    double n[3] = {0, 0, 0};
                    for (int a = 0; a < 3; ++a)
                        for (int i = 0; i < 3; ++i)
                            n[i] += g[a] * toWorld[a][i];
                    const double length = std::sqrt(dot3(n, n));
                    float shade = 1, shine = 0;
                    if (length > 1e-3) {
                        for (double &k : n)
                            k /= length;
                        const double diffuse = std::abs(dot3(n, light));
                        shade = float(0.28 + 0.72 * diffuse);
                        shine = float(0.3 * std::pow(std::abs(dot3(n, halfway)), 24));
                    }
                    const float left = 1 - alpha;
                    red += left * (c[0] * shade + c[3] * shine);
                    green += left * (c[1] * shade + c[3] * shine);
                    blue += left * (c[2] * shade + c[3] * shine);
                    alpha += left * c[3];
                }
            }
            // The PET's brightest along the same ray, over what's drawn.
            if (overlay && overlay->grid && overlay->grid->valid) {
                const VoxelGrid &pet = *overlay->grid;
                const double petStep = rayStep(pet, 0.5);
                const Ray q = rayThrough(grid, pet, view, size, x + 0.5, y + 0.5, petStep);
                float most = -1;
                for (double t = q.first; t <= q.last; ++t)
                    most = std::max(most, sampleAt(pet, float(q.start[0] + t * q.delta[0]), float(q.start[1] + t * q.delta[1]),
                                                   float(q.start[2] + t * q.delta[2])));
                if (most >= 0) {
                    const double v = pet.value(most);
                    const double f = std::clamp((v - overlay->low) / std::max(1e-9, overlay->high - overlay->low), 0.0, 1.0);
                    const float a = float(overlay->opacity * std::min(1.0, f * 2.5));
                    if (a > 0) {
                        const QRgb c = scaled(overlay->colorMap, int(f * 255 + 0.5));
                        red = red * (1 - a) + qRed(c) / 255.f * a;
                        green = green * (1 - a) + qGreen(c) / 255.f * a;
                        blue = blue * (1 - a) + qBlue(c) / 255.f * a;
                    }
                }
            }
            line[x] = qRgb(int(std::clamp(red, 0.f, 1.f) * 255 + 0.5f), int(std::clamp(green, 0.f, 1.f) * 255 + 0.5f),
                           int(std::clamp(blue, 0.f, 1.f) * 255 + 0.5f));
        }
    });
    if (stopped)
        return {};
    return image;
}

bool pickVolume(const VoxelGrid &grid, const View3D &view, QSize size, QPointF at, double *point) {
    if (!grid.valid || size.isEmpty())
        return false;
    const double stepMm = rayStep(grid, 0.5);
    const Ray r = rayThrough(grid, grid, view, size, at.x(), at.y(), stepMm);
    const bool mip = view.look == Look::Mip;
    const Lut lut = mip ? Lut() : makeLut(grid, view, stepMm);
    double best = -1, found = -1;
    float alpha = 0;
    for (double t = r.first; t <= r.last; ++t) {
        const float v = sampleAt(grid, float(r.start[0] + t * r.delta[0]), float(r.start[1] + t * r.delta[1]),
                                 float(r.start[2] + t * r.delta[2]));
        if (mip) {
            if (v > best) {
                best = v;
                found = t;
            }
            continue;
        }
        alpha += (1 - alpha) * lut.rgba[(std::min(65535, int(v)) >> 4) * 4 + 3];
        // Where it's mostly hidden what's behind.
        if (alpha >= 0.5f) {
            found = t;
            break;
        }
    }
    if (found < 0)
        return false;
    const double voxel[3] = {r.start[0] + found * r.delta[0], r.start[1] + found * r.delta[1], r.start[2] + found * r.delta[2]};
    toPatient(grid.volume, voxel, point);
    return true;
}

QPointF projectPoint(const VoxelGrid &grid, const View3D &view, QSize size, const double *point) {
    const Frame3D f = frameFor(grid, view, size);
    const double d[3] = {point[0] - grid.center[0], point[1] - grid.center[1], point[2] - grid.center[2]};
    const double u = dot3(d, view.right) + view.pan[0], v = dot3(d, view.up) + view.pan[1];
    return {size.width() / 2.0 + u / f.pixel, size.height() / 2.0 - v / f.pixel};
}

bool sampleGrid(const VoxelGrid &grid, const double *voxel, double &value) {
    if (!grid.valid)
        return false;
    for (int a = 0; a < 3; ++a)
        if (voxel[a] < -0.5 || voxel[a] > grid.volume.size[a] - 0.5)
            return false;
    value = grid.value(sampleAt(grid, float(voxel[0]), float(voxel[1]), float(voxel[2])));
    return true;
}

QImage fuseSlice(const QImage &base, const ImagePlane &plane, const VoxelGrid &pet, const Overlay3D &look) {
    if (base.isNull() || !plane.valid || !pet.valid)
        return base;
    QImage out = base.convertToFormat(QImage::Format_RGB32);
    // Each pixel across and down is a fixed move through the PET's voxels.
    double origin[3], across[3], down[3], p[3];
    toVoxel(pet.volume, plane.origin, origin);
    for (int i = 0; i < 3; ++i)
        p[i] = plane.origin[i] + plane.row[i] * plane.rowSpacing;
    toVoxel(pet.volume, p, across);
    for (int i = 0; i < 3; ++i)
        p[i] = plane.origin[i] + plane.column[i] * plane.columnSpacing;
    toVoxel(pet.volume, p, down);
    for (int a = 0; a < 3; ++a) {
        across[a] -= origin[a];
        down[a] -= origin[a];
    }
    // The picture may be another size than the plane's pixels, as a
    // slab's is not; scaled to it.
    const double sx = plane.width > 0 ? double(plane.width) / out.width() : 1;
    const double sy = plane.height > 0 ? double(plane.height) / out.height() : 1;
    QRgb table[256];
    for (int g = 0; g < 256; ++g)
        table[g] = colorScale(std::clamp(look.colorMap, 1, 3), g);
    const double span = std::max(1e-9, look.high - look.low);
    eachRow(out.height(), [&](int y) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const double fx = x * sx, fy = y * sy;
            const double v[3] = {origin[0] + fx * across[0] + fy * down[0], origin[1] + fx * across[1] + fy * down[1],
                                 origin[2] + fx * across[2] + fy * down[2]};
            double value;
            if (!sampleGrid(pet, v, value))
                continue;
            const double f = std::clamp((value - look.low) / span, 0.0, 1.0);
            const double a = look.opacity * std::min(1.0, f * 2.5);
            if (a <= 0)
                continue;
            const QRgb c = table[int(f * 255 + 0.5)], b = line[x];
            line[x] = qRgb(int(qRed(b) * (1 - a) + qRed(c) * a + 0.5), int(qGreen(b) * (1 - a) + qGreen(c) * a + 0.5),
                           int(qBlue(b) * (1 - a) + qBlue(c) * a + 0.5));
        }
    });
    return out;
}

Look defaultLook(const QString &modality) {
    return modality == "CT" ? Look::Bone : modality == "PT" ? Look::Mip : Look::Surface;
}

int fusionPartner(const QList<Series> &all, int index) {
    if (index < 0 || index >= all.size())
        return -1;
    const Series &s = all[index];
    const bool pet = s.modality == "PT";
    if (!pet && s.modality != "CT" && s.modality != "MR")
        return -1;
    int best = -1;
    qint64 bestScore = -1;
    for (int i = 0; i < all.size(); ++i) {
        const Series &o = all[i];
        const bool wanted = pet ? (o.modality == "CT" || o.modality == "MR") : o.modality == "PT";
        if (i == index || !wanted || o.color || o.slices.size() < 2)
            continue;
        const bool sameFrame = !s.frame.isEmpty() && s.frame == o.frame;
        const bool sameStudy = !s.studyUid.isEmpty() && s.studyUid == o.studyUid && s.patientId == o.patientId;
        if (!sameFrame && !sameStudy)
            continue;
        // The PET has to be a volume; the series under it only has to have
        // places for its pictures.
        if (!makeVolume(pet ? s : o).valid)
            continue;
        const qint64 score = (sameFrame ? 1000000 : 0) + (o.modality == "CT" ? 100000 : 0) + o.slices.size();
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

void petRange(const VoxelGrid &grid, bool suv, double &low, double &high) {
    if (suv) {
        // From where uptake starts to stand out.
        low = 1;
        high = 6;
        return;
    }
    std::vector<int> counts(256, 0);
    size_t total = 0;
    for (quint16 v : grid.data)
        if (v > 0) {
            ++counts[v >> 8];
            ++total;
        }
    size_t seen = 0;
    int top = 255;
    for (int i = 0; i < 256; ++i) {
        seen += counts[i];
        if (seen >= total * 0.995) {
            top = i;
            break;
        }
    }
    const double brightest = grid.value((top + 1) * 256.0);
    low = grid.low + (brightest - grid.low) * 0.2;
    high = grid.low + (brightest - grid.low) * 0.9;
}
