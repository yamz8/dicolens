// Records what Dicolens does as short GIFs for the README: it makes up a
// hospital CD (a chest CT with its scout, a head MR, and an echo loop), opens
// the real window offscreen with a made-up theme and language, and drives it
// with keys and the mouse, drawing the pointer and each key pressed onto the
// frames. bin/demos builds and runs it; nothing here is part of the app.

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QtEndian>
#include <QtTest>

#include <cmath>
#include <functional>
#include <map>
#include <random>

#include "backend.h"
#include "filepicker.h"
#include "sliceview.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"

namespace {

// ---------------------------------------------------------------------------
// A small DICOM writer: explicit VR little endian, elements in tag order.

struct Dataset {
    std::map<quint32, std::pair<QByteArray, QByteArray>> elements;

    Dataset &set(quint32 tag, const char *vr, QByteArray value) {
        const QByteArray v(vr);
        if (value.size() % 2)
            value.append(v == "UI" || v == "OB" || v == "OW" ? '\0' : ' ');
        elements[tag] = {v, value};
        return *this;
    }
    Dataset &text(quint32 tag, const char *vr, const QString &value) { return set(tag, vr, value.toLatin1()); }
    Dataset &us(quint32 tag, quint16 value) {
        QByteArray b(2, 0);
        qToLittleEndian(value, b.data());
        return set(tag, "US", b);
    }
    Dataset &ul(quint32 tag, quint32 value) {
        QByteArray b(4, 0);
        qToLittleEndian(value, b.data());
        return set(tag, "UL", b);
    }
    Dataset &fd(quint32 tag, double value) {
        QByteArray b(8, 0);
        quint64 bits;
        std::memcpy(&bits, &value, 8);
        qToLittleEndian(bits, b.data());
        return set(tag, "FD", b);
    }
    Dataset &fl(quint32 tag, const std::vector<float> &values) {
        QByteArray b(int(values.size() * 4), 0);
        for (size_t i = 0; i < values.size(); ++i) {
            quint32 bits;
            std::memcpy(&bits, &values[i], 4);
            qToLittleEndian(bits, b.data() + i * 4);
        }
        return set(tag, "FL", b);
    }
    Dataset &seq(quint32 tag, const QList<Dataset> &items);
};

QByteArray encode(const Dataset &d) {
    QByteArray out;
    for (const auto &[tag, element] : d.elements) {
        const auto &[vr, value] = element;
        char b[4];
        qToLittleEndian(quint16(tag >> 16), b);
        qToLittleEndian(quint16(tag & 0xffff), b + 2);
        out.append(b, 4).append(vr);
        if (vr == "OB" || vr == "OW" || vr == "SQ" || vr == "UT" || vr == "UN") {
            out.append(2, '\0');
            qToLittleEndian(quint32(value.size()), b);
            out.append(b, 4);
        } else {
            qToLittleEndian(quint16(value.size()), b);
            out.append(b, 2);
        }
        out.append(value);
    }
    return out;
}

Dataset &Dataset::seq(quint32 tag, const QList<Dataset> &items) {
    QByteArray value;
    for (const auto &item : items) {
        const QByteArray body = encode(item);
        char b[8];
        qToLittleEndian(quint16(0xFFFE), b);
        qToLittleEndian(quint16(0xE000), b + 2);
        qToLittleEndian(quint32(body.size()), b + 4);
        value.append(b, 8).append(body);
    }
    elements[tag] = {"SQ", value};
    return *this;
}

// A coded concept, as structured reports name things.
Dataset code(const QString &value, const QString &meaning) {
    Dataset d;
    d.text(0x00080100, "SH", value).text(0x00080102, "SH", "DCM").text(0x00080104, "LO", meaning);
    return d;
}

QString uid() {
    static int next = 1000;
    return QStringLiteral("2.25.4187392057612%1").arg(next++);
}

QString writeDicom(const QString &path, Dataset d) {
    const QString sopClass = QString::fromLatin1(d.elements[0x00080016].second).trimmed().remove(QChar('\0'));
    const QString sop = uid();
    d.text(0x00080018, "UI", sop);
    Dataset meta;
    meta.set(0x00020001, "OB", QByteArray("\0\1", 2))
        .text(0x00020002, "UI", sopClass)
        .text(0x00020003, "UI", sop)
        .text(0x00020010, "UI", "1.2.840.10008.1.2.1")
        .text(0x00020012, "UI", "2.25.41873920576120001");
    const QByteArray metaBytes = encode(meta);
    QByteArray length(4, 0);
    qToLittleEndian(quint32(metaBytes.size()), length.data());
    Dataset group;
    group.set(0x00020000, "UL", length);

    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        qFatal("can't write %s", qPrintable(path));
    file.write(QByteArray(128, '\0') + "DICM" + encode(group) + metaBytes + encode(d));
    return sop;
}

QByteArray words(const std::vector<int> &values) {
    QByteArray out(int(values.size() * 2), 0);
    for (size_t i = 0; i < values.size(); ++i)
        qToLittleEndian(quint16(std::clamp(values[i], 0, 4095)), out.data() + i * 2);
    return out;
}

QString ds(double v) { return QString::number(v, 'g', 6); }

// What every image of one patient carries.
Dataset patient(const QString &studyUid, const QString &date, const QString &study, const QString &accession) {
    Dataset d;
    d.text(0x00080020, "DA", date)
        .text(0x00080030, "TM", "093000")
        .text(0x00080050, "SH", accession)
        .text(0x00080080, "LO", "Example Hospital")
        .text(0x00081030, "LO", study)
        .text(0x00100010, "PN", "Doe^Jane")
        .text(0x00100020, "LO", "DEMO-0001")
        .text(0x00100030, "DA", "19800101")
        .text(0x00100040, "CS", "F")
        .text(0x0020000D, "UI", studyUid);
    return d;
}

// ---------------------------------------------------------------------------
// A made-up chest, in Hounsfield units, at (x, y) millimetres on slice s
// (0 at the shoulders, 47 at the upper abdomen, 5 mm apart). x runs to the
// patient's left, y to the back, as an axial CT is shown.

double inside(double x, double y, double cx, double cy, double a, double b) {
    const double dx = (x - cx) / a, dy = (y - cy) / b;
    return dx * dx + dy * dy;
}

double smoothstep(double a, double b, double v) {
    const double t = std::clamp((v - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

struct Vessel {
    double x, y, phase, radius;
};

const std::vector<Vessel> &vessels() {
    static const std::vector<Vessel> list = [] {
        std::mt19937 random(7);
        std::uniform_real_distribution<double> u(0, 1);
        std::vector<Vessel> v;
        for (int side : {-1, 1})
            for (int i = 0; i < 46; ++i) {
                const double a = u(random) * 2 * M_PI, r = std::sqrt(u(random));
                const double x = side * 66 + std::cos(a) * r * 40, y = -2 + std::sin(a) * r * 62;
                // Larger towards the hilum, in the middle.
                const double hilum = 1 - std::min(1.0, std::abs(x - side * 30) / 60);
                v.push_back({x, y, u(random) * 6.28, 1.0 + 2.6 * hilum * u(random) + 0.4 * u(random)});
            }
        return v;
    }();
    return list;
}

// A scout is taken before the contrast goes in, so its vessels are plain blood.
double chest(double x, double y, double s, bool contrast = true) {
    const double blood = contrast ? 1 : 0;
    if (y > 108 && y < 116 && std::abs(x) < 175)
        return 140;  // The table.
    const double bodyA = 126 + 26 * smoothstep(-6, 14, s), bodyB = 92 + 8 * smoothstep(-6, 10, s);
    const double body = inside(x, y, 0, 0, bodyA, bodyB);
    if (body > 1)
        return -1000;
    double hu = body > 0.985 ? 30 : body > 0.87 ? -95 : 45;

    // Lungs, cut short below by the diaphragm, higher on the right.
    for (int side : {-1, 1}) {
        const double base = side < 0 ? 33 : 37;
        const double scale = std::sqrt(smoothstep(-1, 7, s)) * (1 - smoothstep(base - 6, base + 2, s) * 0.85);
        if (scale > 0.05 && inside(x, y, side * 66, -2, 46 * scale, 68 * scale) < 1) {
            hu = -860;
            for (const Vessel &v : vessels()) {
                if ((v.x < 0) != (side < 0))
                    continue;
                const double vx = side * 66 + (v.x - side * 66) * scale + 5 * std::sin(s * 0.35 + v.phase);
                const double vy = -2 + (v.y + 2) * scale + 5 * std::cos(s * 0.3 + v.phase);
                if (inside(x, y, vx, vy, v.radius, v.radius) < 1)
                    hu = 30;
            }
        }
    }

    // Upper abdomen: liver on the right, stomach and spleen on the left.
    if (s > 30) {
        const double l = smoothstep(30, 40, s);
        if (inside(x, y, -38, 6, 92 * l, 78 * l) < 1)
            hu = inside(x, y, -30, 20, 12, 10) < 1 || inside(x, y, -55, -10, 8, 8) < 1 ? 150 : 62;
        if (s > 37 && inside(x, y, 52, -26, 34, 28) < 1)
            hu = y < -36 ? -950 : 18;
        if (s > 38 && inside(x, y, 76, 34, 22, 36) < 1)
            hu = 52;
    }

    // The mediastinum.
    if (s > 14 && s < 38) {
        const double h = std::pow(std::sin(M_PI * (s - 14) / 24), 0.6);
        const double heart = inside(x, y, 18, -28, 52 * h, 42 * h);
        if (heart < 1.15 && heart >= 1)
            hu = -90;  // Pericardial fat.
        if (heart < 1) {
            // Muscle, with contrast in the chambers: the left ventricle behind
            // and to the left, the right in front, the atria higher up.
            hu = 58;
            const double low = smoothstep(18, 24, s), high = 1 - smoothstep(24, 30, s);
            if (inside(x, y, 36, -18, 22 * h, 17 * h) < 1 * low)
                hu = 45 + 190 * blood;
            if (inside(x, y, 4, -46, 25 * h, 12 * h) < 1 * low)
                hu = 45 + 160 * blood;
            if (inside(x, y, 26, -10, 20 * h, 14 * h) < 1 * high)
                hu = 45 + 180 * blood;
            if (inside(x, y, -14, -24, 17 * h, 16 * h) < 1 * high)
                hu = 45 + 155 * blood;
        }
    }
    if (s < 11 && inside(x, y, 0, -30, 11, 11) < 1)
        hu = inside(x, y, 0, -30, 8.5, 8.5) < 1 ? -1000 : 40;
    if (s >= 11 && s < 15)
        for (int side : {-1, 1})
            if (inside(x, y, side * (s - 10) * 5, -26, 7, 7) < 1)
                hu = inside(x, y, side * (s - 10) * 5, -26, 5, 5) < 1 ? -1000 : 40;
    if (s > 9 && s < 27 && inside(x, y, 10, -40, 15, 15) < 1)
        hu = 45 + 215 * blood;  // Ascending aorta.
    if (s >= 6 && s <= 10) {
        // The arch, from front to back.
        const double t = std::clamp((y + 40) / 82, 0.0, 1.0), cx = 10 + 12 * t, cy = -40 + 82 * t;
        if (inside(x, y, cx, cy, 14, 14) < 1)
            hu = 45 + 215 * blood;
    }
    if (s >= 6 && inside(x, y, 22, 42, 12, 12) < 1)
        hu = 45 + 215 * blood;  // Descending aorta.
    if (s > 11 && s < 17 && inside(x, y, -14, -42, 13, 13) < 1)
        hu = 45 + 195 * blood;  // Pulmonary trunk.
    if (s < 36 && inside(x, y, 6, 30, 7, 7) < 1)
        hu = inside(x, y, 6, 30, 2.2, 2.2) < 1 ? -900 : 38;  // Oesophagus.

    // Bone: sternum, ribs, and the spine.
    if (s > 3 && s < 32 && std::abs(x) < 14 && y > -91 && y < -80)
        hu = std::abs(x) > 11 || y < -89 || y > -82 ? 620 : 280;
    if (s < 42)
        for (int side : {-1, 1})
            for (int k = 0; k < 7; ++k) {
                if (std::fmod(s + k * 1.7 + (side > 0 ? 0.8 : 0), 3.0) > 1.7)
                    continue;
                const double phi = (35 + k * 20 + 6 * std::sin(s * 0.4 + k)) * M_PI / 180;
                const double rx = side * bodyA * 0.79 * std::sin(phi), ry = bodyB * 0.79 * std::cos(phi);
                const double r = inside(x, y, rx, ry, 6, 6);
                if (r < 1)
                    hu = r > 0.45 ? 700 : 260;
            }
    const double vertebra = inside(x, y, 0, 58, 18, 15);
    if (vertebra < 1)
        hu = vertebra > 0.72 ? 650 : 240;
    const double canal = inside(x, y, 0, 82, 8, 8);
    if (canal < 1)
        hu = 15;
    else if (canal < 2.6 && y > 76)
        hu = 560;
    if (std::abs(x) < 4 && y > 88 && y < 106)
        hu = 560;
    return hu;
}

// ---------------------------------------------------------------------------
// The CD: a chest CT with its scout, a head MR, and an echo loop.

// The chest CT, with a scout, or a year's earlier one without contrast to
// compare it with. Gives back each axial slice's SOP Instance UID.
QStringList makeChestCt(const QString &root, std::mt19937 &random, bool prior = false, QString *studyUid = nullptr,
                        QString *seriesUid = nullptr) {
    const QString study = uid(), axial = uid(), scout = uid(), frame = uid();
    if (studyUid)
        *studyUid = study;
    if (seriesUid)
        *seriesUid = axial;
    const QString folder = root + (prior ? "/ST000000" : "/ST000001");
    QStringList sops;
    const auto base = [&] {
        return patient(study, prior ? "20250310" : "20260314", prior ? "CT Chest" : "CT Chest with contrast",
                       prior ? "A0907" : "A1001")
            .text(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.2")
            .text(0x00080060, "CS", "CT")
            .text(0x00080070, "LO", "Made-up Scanner")
            .text(0x00180015, "CS", "CHEST")
            .text(0x00200052, "UI", frame)
            .us(0x00280002, 1)
            .text(0x00280004, "CS", "MONOCHROME2")
            .us(0x00280100, 16).us(0x00280101, 12).us(0x00280102, 11).us(0x00280103, 0)
            .text(0x00281052, "DS", "-1024")
            .text(0x00281053, "DS", "1");
    };
    std::normal_distribution<double> noise(0, 1);

    // The axial series: 48 slices, head to foot.
    const int n = 512, slices = 48;
    const double spacing = 0.7;
    for (int s = 0; s < slices; ++s) {
        std::vector<int> pixels(n * n);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) {
                const double x = (c - 255.5) * spacing, y = (r - 255.5) * spacing;
                // Four samples a pixel, so edges blend as a scanner's do.
                double hu = 0;
                for (double ox : {-0.25, 0.25})
                    for (double oy : {-0.25, 0.25})
                        hu += chest(x + ox * spacing, y + oy * spacing, s, !prior) / 4;
                const double sigma = hu <= -990 ? 1 : hu < -500 ? 7 : 3.5;
                pixels[r * n + c] = int(std::lround(hu + sigma * noise(random))) + 1024;
            }
        Dataset d = base();
        d.text(0x0008103E, "LO", "Axial 5 mm")
            .text(0x00180050, "DS", "5")
            .text(0x00180060, "DS", "120")
            .text(0x0020000E, "UI", axial)
            .text(0x00200011, "IS", "2")
            .text(0x00200013, "IS", QString::number(s + 1))
            .text(0x00200032, "DS", QStringLiteral("%1\\%1\\%2").arg(ds(-255.5 * spacing), ds(-s * 5.0)))
            .text(0x00200037, "DS", "1\\0\\0\\0\\1\\0")
            .text(0x00201041, "DS", ds(-s * 5.0))
            .us(0x00280010, n).us(0x00280011, n)
            .text(0x00280030, "DS", QStringLiteral("%1\\%1").arg(ds(spacing)))
            .text(0x00281050, "DS", "40")
            .text(0x00281051, "DS", "400")
            .set(0x7FE00010, "OW", words(pixels));
        sops << writeDicom(QStringLiteral("%1/SE000002/IM%2").arg(folder).arg(s + 1, 6, 10, QChar('0')), d);
    }
    if (prior)
        return sops;

    // The scout: the same chest seen from the front, as X-rays add up through it.
    const int cols = 512, rows = 400;
    const double step = 0.7;
    std::vector<double> sum(cols * rows);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const double x = (c - 255.5) * step, s = (r * step - 20) / 5;
            double total = 0;
            for (double y = -105; y <= 105; y += 2.5)
                total += std::max(0.0, chest(x, y, s, false) + 1000) / 1000 * 2.5;
            sum[r * cols + c] = total;
        }
    std::vector<int> pixels(cols * rows);
    for (int i = 0; i < cols * rows; ++i)
        pixels[i] = int(std::lround(std::pow(sum[i] / 260, 0.8) * 3600 + 14 * noise(random)));
    Dataset d = base();
    d.text(0x00080008, "CS", "ORIGINAL\\PRIMARY\\LOCALIZER")
        .text(0x0008103E, "LO", "Scout")
        .text(0x0020000E, "UI", scout)
        .text(0x00200011, "IS", "1")
        .text(0x00200013, "IS", "1")
        .text(0x00200032, "DS", QStringLiteral("%1\\0\\%2").arg(ds(-255.5 * step), ds(20)))
        .text(0x00200037, "DS", "1\\0\\0\\0\\0\\-1")
        .us(0x00280010, rows).us(0x00280011, cols)
        .text(0x00280030, "DS", QStringLiteral("%1\\%1").arg(ds(step)))
        .text(0x00281050, "DS", "2100")
        .text(0x00281051, "DS", "3400")
        .text(0x00281052, "DS", "0")
        .set(0x7FE00010, "OW", words(pixels));

    // An overlay plane over the scout, framing the part the axial series
    // covers, as scanners mark a scout.
    const int top = int((0 * 5 + 20) / step), bottom = int((47 * 5 + 20) / step), left = 34, right = 478;
    QByteArray bits((cols * rows + 15) / 16 * 2, 0);
    auto set = [&](int r, int c) { const int i = r * cols + c; bits[i / 8] = char(bits[i / 8] | (1 << (i % 8))); };
    for (int t = 0; t < 2; ++t) {
        for (int c = left; c <= right; ++c) {
            set(top + t, c);
            set(bottom - t, c);
        }
        for (int r = top; r <= bottom; ++r) {
            set(r, left + t);
            set(r, right - t);
        }
    }
    d.us(0x60000010, rows).us(0x60000011, cols)
        .text(0x60000022, "LO", "Scan range")
        .text(0x60000040, "CS", "G")
        .set(0x60000050, "SS", QByteArray("\x01\x00\x01\x00", 4))
        .us(0x60000100, 1).us(0x60000102, 0)
        .set(0x60003000, "OW", bits);
    writeDicom(folder + "/SE000001/IM000001", d);
    return sops;
}

// Where a point in millimetres from the middle of a 512 pixel, 0.7 mm CT
// slice is, in a presentation state's pixel units.
std::vector<float> ctPoint(double x, double y) {
    return {float(x / 0.7 + 256), float(y / 0.7 + 256)};
}

// A presentation state saved by someone reading the CT: the descending
// aorta circled and named, and a line across the heart, on one slice.
void makeMarks(const QString &root, const QString &study, const QString &series, const QString &sop) {
    Dataset image;
    image.text(0x00081150, "UI", "1.2.840.10008.5.1.4.1.1.2").text(0x00081155, "UI", sop);
    Dataset referenced;
    referenced.seq(0x00081140, {image}).text(0x0020000E, "UI", series);

    const std::vector<float> centre = ctPoint(22, 42), edge = ctPoint(36, 42);
    Dataset circle;
    circle.text(0x00700005, "CS", "PIXEL").us(0x00700020, 2).us(0x00700021, 2)
        .fl(0x00700022, {centre[0], centre[1], edge[0], edge[1]})
        .text(0x00700023, "CS", "CIRCLE").text(0x00700024, "CS", "N");
    std::vector<float> across = ctPoint(-30, -28), to = ctPoint(66, -28);
    Dataset line;
    line.text(0x00700005, "CS", "PIXEL").us(0x00700020, 2).us(0x00700021, 2)
        .fl(0x00700022, {across[0], across[1], to[0], to[1]})
        .text(0x00700023, "CS", "POLYLINE").text(0x00700024, "CS", "N");
    const std::vector<float> anchor = ctPoint(30, 52), boxTop = ctPoint(50, 66), boxBottom = ctPoint(110, 76);
    Dataset note;
    note.text(0x00700003, "CS", "PIXEL").text(0x00700004, "CS", "PIXEL")
        .text(0x00700006, "ST", "Descending aorta, 24 mm")
        .fl(0x00700010, {boxTop[0], boxTop[1]}).fl(0x00700011, {boxBottom[0], boxBottom[1]})
        .fl(0x00700014, {anchor[0], anchor[1]}).text(0x00700015, "CS", "Y");
    Dataset annotation;
    annotation.text(0x00700002, "CS", "READING").seq(0x00700008, {note}).seq(0x00700009, {circle, line});
    Dataset layer;
    layer.text(0x00700002, "CS", "READING").text(0x00700062, "IS", "1");

    Dataset d = patient(study, "20260314", "CT Chest with contrast", "A1001");
    d.text(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.11.1")
        .text(0x00080060, "CS", "PR")
        .text(0x0008103E, "LO", "Reading marks")
        .text(0x0020000E, "UI", uid())
        .text(0x00200011, "IS", "90")
        .text(0x00700080, "CS", "READING")
        .text(0x00700081, "LO", "Marks from the reading")
        .seq(0x00081115, {referenced})
        .seq(0x00700001, {annotation})
        .seq(0x00700060, {layer});
    writeDicom(root + "/ST000001/SE000090/PR000001", d);
}

// The CT's report, as a structured report: findings under headings, a
// measurement, and the impression.
void makeStructuredReport(const QString &root, const QString &study) {
    auto item = [](const QString &type, const QString &name) {
        Dataset d;
        d.text(0x0040A010, "CS", "CONTAINS").text(0x0040A040, "CS", type).seq(0x0040A043, {code("1", name)});
        return d;
    };
    auto text = [&](const QString &name, const QString &value) {
        return item("TEXT", name).text(0x0040A160, "UT", value);
    };
    Dataset diameter = item("NUM", "Descending aorta diameter");
    Dataset measured;
    measured.text(0x0040A30A, "DS", "24").seq(0x004008EA, {code("mm", "millimeter")});
    diameter.seq(0x0040A300, {measured});
    Dataset findings = item("CONTAINER", "Findings");
    findings.seq(0x0040A730, {
        text("Lungs", "Clear. No nodules, consolidation, or effusion."),
        text("Heart", "Normal in size. No pericardial effusion."),
        text("Mediastinum", "No enlarged lymph nodes."),
        diameter,
        text("Bones", "No suspicious lesions."),
    });
    Dataset impression = item("CONTAINER", "Impression");
    impression.seq(0x0040A730, {text("Conclusion", "No acute findings in the chest.")});
    Dataset history = item("CONTAINER", "Clinical history");
    history.seq(0x0040A730, {text("Reason for the scan", "Follow-up of a cough, compared with March 2025.")});

    Dataset d = patient(study, "20260314", "CT Chest with contrast", "A1001");
    d.text(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.88.33")
        .text(0x00080023, "DA", "20260315")
        .text(0x00080060, "CS", "SR")
        .text(0x0008103E, "LO", "Report")
        .text(0x0020000E, "UI", uid())
        .text(0x00200011, "IS", "99")
        .text(0x0040A040, "CS", "CONTAINER")
        .seq(0x0040A043, {code("18748-4", "CT Chest Report")})
        .text(0x0040A491, "CS", "COMPLETE")
        .text(0x0040A493, "CS", "VERIFIED")
        .seq(0x0040A730, {history, findings, impression});
    writeDicom(root + "/ST000001/SE000099/SR000001", d);
}

// The MR's report, as the PDF a hospital system wraps in DICOM.
void makePdfReport(const QString &root, const QString &study) {
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    {
        QPdfWriter pdf(&buffer);
        pdf.setTitle("MR Head report");
        QPainter p(&pdf);
        QFont font("DejaVu Sans", 12);
        p.setFont(font);
        int y = 400;
        for (const QString &line : {QString("Example Hospital — Radiology"), QString(), QString("MR Head, 2 May 2026"),
                                    QString(), QString("Findings: normal appearances of the brain."),
                                    QString("Impression: no abnormality.")}) {
            p.drawText(400, y, line);
            y += 300;
        }
    }
    Dataset d = patient(study, "20260502", "MR Head", "A1002");
    d.text(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.104.1")
        .text(0x00080023, "DA", "20260503")
        .text(0x00080060, "CS", "DOC")
        .text(0x0008103E, "LO", "Report")
        .text(0x0020000E, "UI", uid())
        .text(0x00200011, "IS", "99")
        .text(0x00420010, "ST", "MR Head report")
        .text(0x00420012, "LO", "application/pdf")
        .set(0x00420011, "OB", buffer.data());
    writeDicom(root + "/ST000002/SE000099/DOC00001", d);
}

QString makeHeadMr(const QString &root, std::mt19937 &random) {
    const QString study = uid(), series = uid(), frame = uid();
    std::normal_distribution<double> noise(0, 1);
    const int n = 256, slices = 20;
    const double spacing = 0.9;
    for (int s = 0; s < slices; ++s) {
        // s 0 is low, through the eyes; 19 is near the top of the head.
        const double shrink = s > 7 ? std::sqrt(std::max(0.05, 1 - std::pow((s - 7) / 14.0, 2))) : 1;
        std::vector<int> pixels(n * n);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) {
                const double x = (c - 127.5) * spacing, y = (r - 127.5) * spacing;
                const double rn = std::sqrt(inside(x, y, 0, 0, 74 * shrink, 92 * shrink));
                const double theta = std::atan2(y, x);
                double v = 0;
                if (rn <= 1) {
                    const double gyri = 0.86 - 0.05 * (0.5 + 0.5 * std::sin(theta * 15 + s * 0.9))
                                        - 0.02 * std::sin(theta * 7 - s * 0.5);
                    v = rn > 0.95 ? 1150 : rn > 0.89 ? 70 : rn > 0.875 ? 1700 : rn > gyri ? 1650 : rn > gyri - 0.09 ? 980 : 690;
                    if (std::abs(x) < 1.3 && rn < 0.87 && rn > 0.12)
                        v = 1600;  // Between the hemispheres.
                    if (s >= 6 && s <= 13) {
                        for (int side : {-1, 1}) {
                            if (inside(x, y, side * 10, -4, 6.5, 24) < 1)
                                v = 1800;  // Lateral ventricles.
                            if (s <= 10 && inside(x, y, side * 19, 10, 10, 13) < 1)
                                v = 930;  // Thalamus.
                        }
                    }
                    if (s < 5)
                        for (int side : {-1, 1})
                            if (inside(x, y, side * 31, -74, 13, 12) < 1)
                                v = inside(x, y, side * 31, -84, 6, 3) < 1 ? 300 : 1750;  // Eyes.
                }
                pixels[r * n + c] = int(std::lround(std::abs(v + (v > 0 ? 16 : 10) * noise(random))));
            }
        Dataset d = patient(study, "20260502", "MR Head", "A1002");
        d.text(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.4")
            .text(0x00080060, "CS", "MR")
            .text(0x0008103E, "LO", "T2 axial")
            .text(0x00180015, "CS", "HEAD")
            .text(0x00180050, "DS", "5")
            .text(0x00180087, "DS", "3")
            .text(0x0020000E, "UI", series)
            .text(0x00200011, "IS", "3")
            .text(0x00200013, "IS", QString::number(slices - s))
            .text(0x00200032, "DS", QStringLiteral("%1\\%1\\%2").arg(ds(-127.5 * spacing), ds(s * 5.0)))
            .text(0x00200037, "DS", "1\\0\\0\\0\\1\\0")
            .text(0x00200052, "UI", frame)
            .us(0x00280002, 1)
            .text(0x00280004, "CS", "MONOCHROME2")
            .us(0x00280010, n).us(0x00280011, n)
            .text(0x00280030, "DS", QStringLiteral("%1\\%1").arg(ds(spacing)))
            .us(0x00280100, 16).us(0x00280101, 12).us(0x00280102, 11).us(0x00280103, 0)
            .text(0x00281050, "DS", "900")
            .text(0x00281051, "DS", "1900")
            .set(0x7FE00010, "OW", words(pixels));
        writeDicom(QStringLiteral("%1/ST000002/SE000003/IM%2").arg(root).arg(slices - s, 6, 10, QChar('0')), d);
    }
    return study;
}

// A PET of the chest an hour after FDG, at PET's coarse resolution: the
// heart muscle and a nodule in the right lung take it up, the liver some.
void makePet(const QString &root, std::mt19937 &random) {
    const QString study = uid(), series = uid(), frame = uid();
    const int n = 128, slices = 48;
    const double spacing = 2.8;
    // 370 MBq at 09:15, the series corrected to its start at 10:30.
    const double dose = 370e6, halfLife = 6586.2, weight = 70;
    const double factor = weight * 1000 / (dose * std::pow(0.5, 4500 / halfLife));
    std::normal_distribution<double> noise(0, 1);
    Dataset drug;
    drug.text(0x00180031, "LO", "Fluorodeoxyglucose")
        .text(0x00181072, "TM", "091500")
        .text(0x00181074, "DS", ds(dose))
        .text(0x00181075, "DS", ds(halfLife));
    for (int s = 0; s < slices; ++s) {
        std::vector<int> pixels(n * n);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) {
                double uptake = 0;
                for (double ox : {-0.33, 0.0, 0.33})
                    for (double oy : {-0.33, 0.0, 0.33}) {
                        const double x = (c - 63.5 + ox) * spacing, y = (r - 63.5 + oy) * spacing;
                        const double hu = chest(x, y, s, false);
                        double u = hu < -900 || y > 108 ? 0 : hu < -500 ? 0.45 : hu < -50 ? 0.5 : hu > 200 ? 1.4 : 0.95;
                        if (s > 14 && s < 36 && std::hypot((x - 18) / 52, (y + 28) / 42) < 1 && hu > 40)
                            u = 4.8;   // The heart muscle.
                        if (s > 30 && x < -5 && hu > 50 && hu < 90)
                            u = 2.4;   // The liver.
                        const double nodule = std::hypot(x + 62, y + 8, (s - 21) * 5.0);
                        if (nodule < 11)
                            u = 9.0 * (1 - nodule / 22);
                        uptake += u / 9;
                    }
                const double bq = uptake / factor * (1 + 0.12 * noise(random));
                pixels[r * n + c] = int(std::lround(std::max(0.0, bq) / 10));
            }
        Dataset d = patient(study, "20260320", "PET-CT FDG", "A1107");
        d.text(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.128")
            .text(0x00080021, "DA", "20260320")
            .text(0x00080031, "TM", "103000")
            .text(0x00080060, "CS", "PT")
            .text(0x00080070, "LO", "Made-up Scanner")
            .text(0x0008103E, "LO", "PET FDG chest")
            .text(0x00101030, "DS", ds(weight))
            .text(0x00180050, "DS", "5")
            .text(0x0020000E, "UI", series)
            .text(0x00200011, "IS", "3")
            .text(0x00200013, "IS", QString::number(s + 1))
            .text(0x00200032, "DS", QStringLiteral("%1\\%1\\%2").arg(ds(-63.5 * spacing), ds(-s * 5.0)))
            .text(0x00200037, "DS", "1\\0\\0\\0\\1\\0")
            .text(0x00200052, "UI", frame)
            .us(0x00280002, 1)
            .text(0x00280004, "CS", "MONOCHROME2")
            .us(0x00280010, n).us(0x00280011, n)
            .text(0x00280030, "DS", QStringLiteral("%1\\%1").arg(ds(spacing)))
            .us(0x00280100, 16).us(0x00280101, 12).us(0x00280102, 11).us(0x00280103, 0)
            .text(0x00281050, "DS", ds(3.0 / factor))
            .text(0x00281051, "DS", ds(6.0 / factor))
            .text(0x00281052, "DS", "0")
            .text(0x00281053, "DS", "10")
            .seq(0x00540016, {drug})
            .text(0x00541001, "CS", "BQML")
            .text(0x00541102, "CS", "START")
            .set(0x7FE00010, "OW", words(pixels));
        writeDicom(QStringLiteral("%1/ST000003/SE000001/IM%2").arg(root).arg(s + 1, 6, 10, QChar('0')), d);
    }
}

void makeEcho(const QString &root, std::mt19937 &random) {
    const int cols = 320, rows = 256, frames = 30;
    std::uniform_real_distribution<double> u(0.0001, 1);
    // Speckle that stays put, so only the walls move from frame to frame.
    std::vector<double> speckle(cols * rows);
    for (double &v : speckle)
        v = std::sqrt(-2 * std::log(u(random)));
    QByteArray pixels;
    for (int f = 0; f < frames; ++f) {
        const double beat = 0.5 + 0.5 * std::cos(2 * M_PI * f / frames);
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                const double dx = c - 160, dy = r - 8, depth = std::hypot(dx, dy), angle = std::atan2(dx, dy);
                double v = 0;
                if (std::abs(angle) < 0.72 && depth > 14 && depth < 245) {
                    const double sp = speckle[r * cols + c];
                    double tissue = 0.42;
                    // Four chambers whose walls move with the beat.
                    const double lv = inside(c, r, 140, 120, 30 + 7 * beat, 62 + 6 * beat);
                    const double rv = inside(c, r, 192, 118, 22 + 6 * beat, 50 + 5 * beat);
                    const double atria = std::min(inside(c, r, 140, 205, 30, 26 - 4 * beat), inside(c, r, 194, 203, 24, 22 - 4 * beat));
                    if (lv < 1.35 || rv < 1.45 || atria < 1.35)
                        tissue = 0.85;
                    if (lv < 1 || rv < 1 || atria < 1)
                        tissue = 0.06;
                    if (std::abs(c - 168) < 4 && r > 190 && r < 215)
                        tissue = 0.85;  // The valve plane.
                    v = tissue * sp * 120 * (1.15 - depth / 420);
                }
                pixels.append(char(std::clamp(int(v), 0, 255)));
            }
    }
    Dataset d = patient(uid(), "20260620", "Echocardiogram", "A1003");
    d.text(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.3.1")
        .text(0x00080060, "CS", "US")
        .text(0x0008103E, "LO", "Apical 4 chamber")
        .text(0x00181063, "DS", "40")
        .text(0x0020000E, "UI", uid())
        .text(0x00200011, "IS", "1")
        .text(0x00200013, "IS", "1")
        .us(0x00280002, 1)
        .text(0x00280004, "CS", "MONOCHROME2")
        .text(0x00280008, "IS", QString::number(frames))
        .set(0x00280009, "AT", QByteArray("\x18\x00\x63\x10", 4))
        .us(0x00280010, rows).us(0x00280011, cols)
        .us(0x00280100, 8).us(0x00280101, 8).us(0x00280102, 7).us(0x00280103, 0)
        .set(0x7FE00010, "OB", pixels);
    // As ultrasound machines write it: no pixel spacing, but the sector's
    // area calibrated, 0.06 cm a pixel.
    Dataset sector;
    sector.us(0x00186012, 1).us(0x00186014, 1).ul(0x00186016, 2)
        .ul(0x00186018, 0).ul(0x0018601A, 8).ul(0x0018601C, cols - 1).ul(0x0018601E, rows - 1)
        .us(0x00186024, 3).us(0x00186026, 3)
        .fd(0x0018602C, 0.06).fd(0x0018602E, 0.06);
    d.seq(0x00186011, {sector});
    writeDicom(root + "/ST000003/SE000001/IM000001", d);
}

// ---------------------------------------------------------------------------
// The window's file chooser, answered at once.

class Picker : public FilePicker {
public:
    Picker(const QString &folder, const QString &saves) : m_folder(folder), m_saves(saves) {}
    void choose(Purpose purpose, const QString &suggestedName) override {
        const QString path = purpose == Purpose::SaveImage || purpose == Purpose::SaveVideo ? m_saves + "/" + suggestedName
                             : purpose == Purpose::SaveFolder                             ? m_saves
                                                                                          : m_folder;
        QTimer::singleShot(0, this, [this, purpose, path] { emit selected(purpose, {QUrl::fromLocalFile(path)}); });
    }

private:
    QString m_folder, m_saves;
};

// ---------------------------------------------------------------------------
// Frames, with the pointer and the keys pressed drawn on them.

class Recorder {
public:
    static constexpr int Fps = 12;

    Recorder(QQuickWindow *window, const QString &out) : m_window(window), m_out(out) {}

    void begin(const QString &name) {
        m_name = name;
        m_frames.reset(new QTemporaryDir);
        m_index = 0;
        m_badge.clear();
        m_pointerShown = false;
        m_clock.start();
        m_next = 0;
    }

    void finish() {
        const QString gif = m_out + "/" + m_name + ".gif";
        QProcess ffmpeg;
        ffmpeg.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        ffmpeg.start("ffmpeg", {"-v", "error", "-y", "-framerate", QString::number(Fps), "-i", m_frames->path() + "/%05d.png",
                                "-vf", "split[a][b];[a]palettegen=max_colors=128:stats_mode=full[p];"
                                       "[b][p]paletteuse=dither=none:diff_mode=rectangle",
                                gif});
        ffmpeg.waitForFinished(-1);
        qInfo().noquote() << QStringLiteral("%1: %2 frames, %3 KB").arg(gif).arg(m_index).arg(QFileInfo(gif).size() / 1024);
    }

    void hold(int ms) {
        const int count = std::max(1, ms * Fps / 1000);
        for (int i = 0; i < count; ++i)
            frame();
    }

    void badge(const QString &text) {
        m_badge = text;
        m_badgeFrames = Fps * 11 / 10;
    }

    void key(const QString &label, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, int holdMs = 900) {
        badge(label);
        QTest::keyClick(m_window, key, modifiers);
        hold(holdMs);
    }

    void type(const QString &text) {
        for (const QChar c : text) {
            QTest::keyClick(m_window, c.toLatin1());
            hold(110);
        }
    }

    void moveTo(QPointF to, int ms) {
        if (!m_pointerShown) {
            m_pointer = to + QPointF(160, 120);
            m_pointerShown = true;
        }
        const QPointF from = m_pointer;
        const int count = std::max(1, ms * Fps / 1000);
        for (int i = 1; i <= count; ++i) {
            const double t = double(i) / count, e = t * t * (3 - 2 * t);
            m_pointer = from + (to - from) * e;
            QTest::mouseMove(m_window, m_pointer.toPoint());
            frame();
        }
    }

    void drag(QPointF from, QPointF to, Qt::MouseButton button, int ms, const QString &label,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        moveTo(from, 350);
        badge(label);
        QTest::mousePress(m_window, button, modifiers, from.toPoint());
        m_pressed = true;
        moveTo(to, ms);
        QTest::mouseRelease(m_window, button, modifiers, to.toPoint());
        m_pressed = false;
    }

    void wheel(QPointF at, int notches, int ms, const QString &label, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        moveTo(at, 300);
        badge(label);
        const int every = std::max(1, ms * Fps / 1000 / std::abs(notches));
        for (int i = 0; i < std::abs(notches); ++i) {
            QWheelEvent event(at, m_window->mapToGlobal(at), {}, QPoint(0, notches > 0 ? -120 : 120), Qt::NoButton,
                              modifiers, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(m_window, &event);
            m_badgeFrames = std::max(m_badgeFrames, Fps / 2);
            hold(every * 1000 / Fps);
        }
    }

    void click(QPointF at, const QString &label) {
        moveTo(at, 300);
        badge(label);
        QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, at.toPoint());
        m_pressed = true;
        frame();
        QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, at.toPoint());
        m_pressed = false;
        hold(200);
    }

    void hidePointer() { m_pointerShown = false; }

private:
    void frame() {
        // Real time passes between frames, so playback and fades run as they would.
        const qint64 wait = m_next - m_clock.elapsed();
        if (wait > 0)
            QTest::qWait(int(wait));
        else
            QCoreApplication::processEvents();
        m_next = m_clock.elapsed() + 1000 / Fps;

        const double scale = 0.75;
        QImage image = m_window->grabWindow().scaled(QSize(960, 600), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        image = image.convertToFormat(QImage::Format_RGB32);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);

        if (m_badgeFrames > 0 && !m_badge.isEmpty()) {
            QFont font = p.font();
            font.setPixelSize(19);
            font.setBold(true);
            p.setFont(font);
            const QRectF text = p.fontMetrics().boundingRect(m_badge);
            const double alpha = std::min(1.0, m_badgeFrames / 4.0);
            QRectF box(0, 0, text.width() + 36, text.height() + 18);
            box.moveCenter(QPointF(image.width() / 2.0, image.height() - 60));
            p.setPen(QPen(QColor(255, 255, 255, int(60 * alpha)), 1));
            p.setBrush(QColor(18, 18, 22, int(225 * alpha)));
            p.drawRoundedRect(box, 9, 9);
            p.setPen(QColor(255, 255, 255, int(255 * alpha)));
            p.drawText(box, Qt::AlignCenter, m_badge);
            --m_badgeFrames;
        }

        if (m_pointerShown) {
            QPainterPath arrow;
            arrow.moveTo(0, 0);
            arrow.lineTo(0, 19);
            arrow.lineTo(4.6, 14.6);
            arrow.lineTo(8, 22);
            arrow.lineTo(11, 20.6);
            arrow.lineTo(7.6, 13.4);
            arrow.lineTo(13.6, 13.4);
            arrow.closeSubpath();
            p.translate(m_pointer * scale);
            if (m_pressed) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 214, 10, 110));
                p.drawEllipse(QPointF(0, 0), 13, 13);
            }
            p.setPen(QPen(Qt::black, 1.4));
            p.setBrush(Qt::white);
            p.drawPath(arrow);
        }
        p.end();
        image.save(QStringLiteral("%1/%2.png").arg(m_frames->path()).arg(m_index++, 5, 10, QChar('0')));
    }

    QQuickWindow *m_window;
    QString m_out, m_name;
    std::unique_ptr<QTemporaryDir> m_frames;
    int m_index = 0;
    QString m_badge;
    int m_badgeFrames = 0;
    QPointF m_pointer;
    bool m_pointerShown = false, m_pressed = false;
    QElapsedTimer m_clock;
    qint64 m_next = 0;
};

QQuickItem *findItem(QQuickItem *from, const QString &name) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == name)
            return child;
        if (auto *found = findItem(child, name))
            return found;
    }
    return nullptr;
}

bool write(const QString &path, const QByteArray &contents) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray themeColors(const QString &name) {
    QFile file("/usr/share/omarchy/themes/" + name + "/colors.toml");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

int main(int argc, char *argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");
    // --scans <folder> only makes the CD, to open in the app by hand.
    const bool scansOnly = app.arguments().value(1) == "--scans";
    const QStringList only = app.arguments().mid(2);
    const QString out = QDir(app.arguments().value(1, "docs/demos")).absolutePath();
    if (!scansOnly)
        QDir().mkpath(out);

    QTemporaryDir work;
    const QString cd = scansOnly ? QDir(app.arguments().value(2)).absoluteFilePath("DICOM") : work.filePath("DEMO_CD/DICOM");
    std::mt19937 random(2026);
    qInfo() << "Making up the scans";
    QString ctStudy, ctSeries;
    makeChestCt(cd, random, true);
    const QStringList ctSlices = makeChestCt(cd, random, false, &ctStudy, &ctSeries);
    makeMarks(cd, ctStudy, ctSeries, ctSlices.value(22));
    makeStructuredReport(cd, ctStudy);
    makePdfReport(cd, makeHeadMr(cd, random));
    makeEcho(cd, random);
    makePet(cd, random);
    if (scansOnly)
        return 0;

    // The default look, in English, whatever this desktop has.
    write(work.filePath("omarchy/language"), "en\n");
    Translator translator(work.filePath("omarchy/language"));
    app.installTranslator(&translator);
    Theme theme(work.filePath("state"));
    Picker picker(work.filePath("DEMO_CD"), work.path());
    Backend backend(&picker);
    Backend partner(&picker);
    // The PDF report stays in the window rather than opening a viewer.
    backend.setUrlOpener([](const QUrl &) { return true; });

    qmlRegisterType<SliceView>("Dicolens", 1, 0, "SliceView");
    qmlRegisterUncreatableType<Backend>("Dicolens", 1, 0, "Backend", "The backend is given to QML as \"backend\"");
    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    QObject::connect(&translator, &Translator::changed, &backend, [&backend, &partner] {
        backend.retranslate();
        partner.retranslate();
    });
    engine.addImageProvider("thumbnails", new ThumbnailProvider(&backend));
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("partner", &partner);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    window->resize(1280, 800);
    if (!QTest::qWaitForWindowExposed(window))
        return 1;
    window->requestActivate();
    if (!QTest::qWaitForWindowActive(window))
        return 1;

    auto *view = qobject_cast<SliceView *>(findItem(window->contentItem(), "view"));
    // Where a point of the image, in millimetres from its middle, is on the window.
    auto at = [&](double xmm, double ymm, double spacing = 0.7) {
        const QPointF image(xmm / spacing + backend.imageWidth() / 2.0, ymm / spacing + backend.imageHeight() / 2.0);
        return view->mapToScene(view->toItem(image.x(), image.y()));
    };
    auto settle = [&] {
        backend.waitForSlice(10000);
        QTest::qWait(400);
    };
    auto showSeries = [&](const QString &title, int slice) {
        if (!backend.hasImages()) {
            backend.loadPaths({cd});
            QTRY_VERIFY_WITH_TIMEOUT(backend.hasImages() && !backend.scanning(), 20000);
        }
        for (int i = 0; i < backend.seriesList().size(); ++i)
            if (backend.seriesList()[i].toMap().value("title").toString().contains(title))
                backend.selectSeries(i);
        if (slice >= 0)
            backend.setSliceIndex(slice);
        settle();
        backend.fileWindow();
        QMetaObject::invokeMethod(view, "fit");
        settle();
    };

    Recorder rec(window, out);
    struct Scene {
        QString name;
        std::function<void()> prepare, play;
    };
    const QList<Scene> scenes{
        {"open", [] {},
         [&] {
             rec.hold(1300);
             rec.key("Shift+O", Qt::Key_O, Qt::ShiftModifier, 400);
             QTRY_VERIFY_WITH_TIMEOUT(backend.hasImages() && !backend.scanning(), 20000);
             rec.hold(3200);
             rec.key("→", Qt::Key_Right, Qt::NoModifier, 1300);
             rec.key("→", Qt::Key_Right, Qt::NoModifier, 1300);
             rec.key("→", Qt::Key_Right, Qt::NoModifier, 1600);
         }},
        {"slices", [&] { showSeries("Axial", 4); },
         [&] {
             const QPointF middle = at(0, 0);
             rec.moveTo(middle, 500);
             rec.wheel(middle, 18, 2600, "Wheel");
             rec.key("↓", Qt::Key_Down, Qt::NoModifier, 500);
             rec.key("↓", Qt::Key_Down, Qt::NoModifier, 500);
             rec.key("PgDn", Qt::Key_PageDown, Qt::NoModifier, 900);
             rec.key("Home", Qt::Key_Home, Qt::NoModifier, 900);
             rec.key("End", Qt::Key_End, Qt::NoModifier, 900);
             const QRectF bar = view->mapRectToScene(view->boundingRect());
             rec.drag(QPointF(bar.left() + bar.width() * 0.9, bar.bottom() - 4), QPointF(bar.left() + bar.width() * 0.45, bar.bottom() - 4),
                      Qt::LeftButton, 1800, "Drag along the bar");
             rec.hold(900);
         }},
        {"window", [&] { showSeries("Axial", 22); },
         [&] {
             const QPointF middle = at(0, 0);
             rec.drag(middle, middle + QPointF(-140, -90), Qt::LeftButton, 1300, "Drag");
             rec.drag(middle + QPointF(-140, -90), middle + QPointF(170, 60), Qt::LeftButton, 1500, "Drag");
             rec.hold(500);
             rec.hidePointer();
             rec.key("1  Brain", Qt::Key_1, Qt::NoModifier, 1100);
             rec.key("2  Soft tissue", Qt::Key_2, Qt::NoModifier, 1100);
             rec.key("3  Lung", Qt::Key_3, Qt::NoModifier, 1300);
             rec.key("4  Bone", Qt::Key_4, Qt::NoModifier, 1300);
             rec.key("A  Full range", Qt::Key_A, Qt::NoModifier, 1100);
             rec.key("I  Invert", Qt::Key_I, Qt::NoModifier, 1100);
             rec.key("Ctrl+Z", Qt::Key_Z, Qt::ControlModifier, 900);
             rec.key("Ctrl+Z", Qt::Key_Z, Qt::ControlModifier, 900);
             rec.key("0  The file's window", Qt::Key_0, Qt::NoModifier, 1200);
         }},
        {"zoom", [&] { showSeries("Axial", 22); },
         [&] {
             const QPointF heart = at(20, -24);
             rec.wheel(heart, -5, 1200, "Ctrl+Wheel", Qt::ControlModifier);
             rec.hold(500);
             rec.drag(heart, heart + QPointF(-200, 90), Qt::MiddleButton, 1300, "Middle-drag");
             rec.hold(400);
             rec.drag(heart + QPointF(-200, 90), heart + QPointF(-200, 30), Qt::RightButton, 900, "Right-drag");
             rec.hold(600);
             rec.key("F  Fit", Qt::Key_F, Qt::NoModifier, 1200);
         }},
        {"measure", [&] { showSeries("Axial", 22); },
         [&] {
             rec.moveTo(at(-70, -10), 700);
             rec.hold(900);
             rec.moveTo(at(22, 42), 600);
             rec.hold(900);
             rec.moveTo(at(0, 52), 600);
             rec.hold(900);
             rec.key("M  Measure", Qt::Key_M, Qt::NoModifier, 500);
             rec.drag(at(-38, -24), at(78, -24), Qt::LeftButton, 1500, "Drag");
             rec.hold(1300);
             rec.drag(at(-18, 58), at(18, 58), Qt::LeftButton, 900, "Drag");
             rec.hold(1300);
             rec.key("E  Area", Qt::Key_E, Qt::NoModifier, 500);
             rec.drag(at(13, 33), at(31, 51), Qt::LeftButton, 1300, "Drag inside the aorta");
             rec.hold(2200);
             rec.key("Shift+M  Angle", Qt::Key_M, Qt::ShiftModifier, 500);
             rec.drag(at(-70, 30), at(-20, 75), Qt::LeftButton, 1100, "Drag one arm");
             rec.moveTo(at(-50, 95), 700);
             rec.click(at(-60, 100), "Click the other");
             rec.hold(1800);
             rec.key("Esc", Qt::Key_Escape, Qt::NoModifier, 500);
             rec.key("Shift+Delete  Clear", Qt::Key_Delete, Qt::ShiftModifier, 900);
         }},
        {"planes", [&] { showSeries("Axial", 22); },
         [&] {
             rec.hidePointer();
             rec.hold(900);
             rec.key("V  Coronal", Qt::Key_V, Qt::NoModifier, 1600);
             rec.wheel(at(0, 0), -6, 1400, "Wheel");
             rec.hidePointer();
             rec.key("V  Sagittal", Qt::Key_V, Qt::NoModifier, 1800);
             rec.key("V  As scanned", Qt::Key_V, Qt::NoModifier, 1100);
             // Side by side, the same scan cut across: each side shows
             // where the other is.
             rec.key("C  Side by side", Qt::Key_C, Qt::NoModifier, 300);
             partner.waitForSlice(10000);
             partner.selectSeries(backend.seriesIndex());
             partner.waitForSlice(10000);
             rec.key("V  Coronal on this side", Qt::Key_V, Qt::NoModifier, 400);
             partner.waitForSlice(10000);
             QMetaObject::invokeMethod(findItem(window->contentItem(), "rightView"), "fit");
             rec.hold(1200);
             rec.wheel(at(0, 0), 10, 2400, "Wheel: the line follows");
             rec.hidePointer();
             rec.key("C  Back to one", Qt::Key_C, Qt::NoModifier, 900);
         }},
        {"slabs", [&] { showSeries("Axial", 22); },
         [&] {
             rec.hidePointer();
             rec.hold(800);
             rec.key("B  MIP", Qt::Key_B, Qt::NoModifier, 1700);
             rec.key("Shift+B  Thicker", Qt::Key_B, Qt::ShiftModifier, 1500);
             rec.key("3  Lung", Qt::Key_3, Qt::NoModifier, 500);
             rec.key("B  MinIP", Qt::Key_B, Qt::NoModifier, 1700);
             rec.key("B  Average", Qt::Key_B, Qt::NoModifier, 1300);
             rec.key("B  Thin again", Qt::Key_B, Qt::NoModifier, 1100);
         }},
        {"pet", [&] { showSeries("PET", 21); },
         [&] {
             const auto atPet = [&](double x, double y) {
                 return view->mapToScene(view->toItem(x / 2.8 + 64, y / 2.8 + 64));
             };
             rec.moveTo(atPet(-62, -8), 900);
             rec.hold(1500);
             rec.key("E  Area", Qt::Key_E, Qt::NoModifier, 400);
             rec.drag(atPet(-78, -24), atPet(-46, 8), Qt::LeftButton, 1100, "Drag around it");
             rec.hold(2200);
             rec.hidePointer();
             rec.key("G  Hot iron", Qt::Key_G, Qt::NoModifier, 1000);
             rec.key("G  Rainbow", Qt::Key_G, Qt::NoModifier, 1000);
             rec.key("G  Grey", Qt::Key_G, Qt::NoModifier, 1000);
             rec.key("I  Invert", Qt::Key_I, Qt::NoModifier, 1300);
             rec.key("I", Qt::Key_I, Qt::NoModifier, 300);
             rec.key("G", Qt::Key_G, Qt::NoModifier, 300);
             rec.key("G", Qt::Key_G, Qt::NoModifier, 300);
             rec.key("Esc", Qt::Key_Escape, Qt::NoModifier, 200);
             rec.key("Shift+Delete", Qt::Key_Delete, Qt::ShiftModifier, 300);
         }},
        {"turn", [&] { showSeries("T2 axial", 12); },
         [&] {
             rec.hidePointer();
             rec.hold(900);
             rec.key("]  Turn right", Qt::Key_BracketRight, Qt::NoModifier, 1100);
             rec.key("]  Turn right", Qt::Key_BracketRight, Qt::NoModifier, 1100);
             rec.key("[  Turn left", Qt::Key_BracketLeft, Qt::NoModifier, 1100);
             rec.key("X  Flip left to right", Qt::Key_X, Qt::NoModifier, 1300);
             rec.key("Y  Flip top to bottom", Qt::Key_Y, Qt::NoModifier, 1300);
             rec.key("R  Reset", Qt::Key_R, Qt::NoModifier, 1200);
         }},
        {"marks", [&] { showSeries("Axial", 22); },
         [&] {
             rec.hidePointer();
             rec.hold(2200);
             rec.key("N  Hide marks", Qt::Key_N, Qt::NoModifier, 1300);
             rec.key("N  Show them", Qt::Key_N, Qt::NoModifier, 1500);
             rec.key("←  Scout", Qt::Key_Left, Qt::NoModifier, 2400);
         }},
        {"reports", [&] { showSeries("Axial", 22); },
         [&] {
             rec.hidePointer();
             rec.hold(900);
             rec.key("P  Read the report", Qt::Key_P, Qt::NoModifier, 3200);
             rec.key("↓", Qt::Key_Down, Qt::NoModifier, 500);
             rec.key("↓", Qt::Key_Down, Qt::NoModifier, 900);
             rec.key("→  Next report", Qt::Key_Right, Qt::NoModifier, 2200);
             rec.key("Esc", Qt::Key_Escape, Qt::NoModifier, 1000);
         }},
        {"compare", [&] { showSeries("Axial", 22); },
         [&] {
             rec.hold(700);
             rec.key("C  Compare with last year", Qt::Key_C, Qt::NoModifier, 400);
             partner.waitForSlice(10000);
             QMetaObject::invokeMethod(findItem(window->contentItem(), "rightView"), "fit");
             rec.hold(1800);
             const QPointF left = at(0, 0);
             rec.wheel(left, 12, 2200, "Wheel: both sides follow");
             rec.wheel(left, -8, 1500, "Wheel");
             rec.hidePointer();
             rec.key("Tab  The other side", Qt::Key_Tab, Qt::NoModifier, 900);
             rec.key("3  Lung", Qt::Key_3, Qt::NoModifier, 1200);
             rec.key("Tab", Qt::Key_Tab, Qt::NoModifier, 500);
             rec.key("3  Lung", Qt::Key_3, Qt::NoModifier, 1500);
             rec.key("C  Back to one", Qt::Key_C, Qt::NoModifier, 1100);
         }},
        {"ultrasound", [&] { showSeries("Apical", -1); },
         [&] {
             const auto atPixel = [&](double x, double y) { return view->mapToScene(view->toItem(x, y)); };
             rec.hold(700);
             rec.key("M  Measure", Qt::Key_M, Qt::NoModifier, 500);
             rec.drag(atPixel(140, 62), atPixel(140, 180), Qt::LeftButton, 1500, "Drag");
             rec.hold(1300);
             rec.drag(atPixel(110, 120), atPixel(170, 120), Qt::LeftButton, 1000, "Drag");
             rec.hold(1500);
             rec.key("Esc", Qt::Key_Escape, Qt::NoModifier, 500);
             rec.key("Esc", Qt::Key_Escape, Qt::NoModifier, 700);
         }},
        {"series", [&] { showSeries("Scout", -1); },
         [&] {
             rec.hold(1300);
             rec.key("→  CT", Qt::Key_Right, Qt::NoModifier, 1000);
             rec.key("→  MR", Qt::Key_Right, Qt::NoModifier, 700);
             rec.wheel(at(0, 0, 0.9), 8, 1100, "Wheel");
             rec.hidePointer();
             rec.key("→  Ultrasound", Qt::Key_Right, Qt::NoModifier, 900);
             rec.key("Space  Play", Qt::Key_Space, Qt::NoModifier, 4200);
             rec.key("Space  Pause", Qt::Key_Space, Qt::NoModifier, 900);
         }},
        {"tags", [&] { showSeries("Axial", 22); },
         [&] {
             rec.hold(700);
             rec.key("T  Tags", Qt::Key_T, Qt::NoModifier, 1500);
             rec.key("Ctrl+F", Qt::Key_F, Qt::ControlModifier, 300);
             rec.type("spacing");
             rec.hold(1500);
             rec.key("Esc", Qt::Key_Escape, Qt::NoModifier, 400);
             rec.key("↓", Qt::Key_Down, Qt::NoModifier, 400);
             rec.key("↓", Qt::Key_Down, Qt::NoModifier, 700);
             rec.key("T", Qt::Key_T, Qt::NoModifier, 900);
         }},
        {"privacy", [&] { showSeries("Axial", 22); },
         [&] {
             rec.hold(1000);
             rec.key("H  Hide details", Qt::Key_H, Qt::NoModifier, 1700);
             rec.key("H  Show them", Qt::Key_H, Qt::NoModifier, 1200);
         }},
        {"export", [&] { showSeries("Axial", 22); },
         [&] {
             rec.hold(700);
             rec.key("Ctrl+E  Save as PNG", Qt::Key_E, Qt::ControlModifier, 2300);
             rec.key("Ctrl+Shift+E  Save as a video", Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier, 2800);
             rec.key("Ctrl+C  Copy", Qt::Key_C, Qt::ControlModifier, 2600);
         }},
        {"anonymous", [&] { showSeries("Axial", 22); },
         [&] {
             rec.hidePointer();
             rec.hold(900);
             rec.key("Ctrl+Shift+S  Anonymous copy", Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier, 300);
             QTRY_VERIFY_WITH_TIMEOUT(backend.notice().startsWith("Saved") || !backend.problem().isEmpty(), 60000);
             rec.hold(2600);
             // The copy, opened: the same scans, with nobody's name on them.
             rec.badge("The copy, opened");
             backend.loadPaths({work.filePath("Anonymous scans")});
             QTRY_VERIFY_WITH_TIMEOUT(backend.hasImages() && !backend.scanning(), 20000);
             for (int i = 0; i < backend.seriesList().size(); ++i)
                 if (backend.seriesList()[i].toMap().value("title").toString().contains("Axial"))
                     backend.selectSeries(i);
             backend.setSliceIndex(22);
             settle();
             rec.hold(2600);
             backend.dismissProblem();
             // Later scenes open the CD again.
             backend.loadPaths({cd});
             QTRY_VERIFY_WITH_TIMEOUT(backend.hasImages() && !backend.scanning(), 20000);
         }},
        {"help", [&] { showSeries("Axial", 22); },
         [&] {
             rec.hold(600);
             rec.badge("?");
             QTest::keyClick(window, '?');
             rec.hold(3800);
             rec.key("Esc", Qt::Key_Escape, Qt::NoModifier, 800);
         }},
        {"theme", [&] { showSeries("Axial", 22); },
         [&] {
             rec.hold(1200);
             const auto setTheme = [&](const QString &name, const QString &label) {
                 rec.badge(label);
                 write(work.filePath("state/theme/colors.toml"), themeColors(name));
                 rec.hold(1700);
             };
             const auto setLanguage = [&](const QByteArray &code, const QString &label) {
                 rec.badge(label);
                 write(work.filePath("omarchy/language"), code + "\n");
                 rec.hold(1700);
             };
             setTheme("tokyo-night", "Theme: Tokyo Night");
             setTheme("catppuccin-latte", "Theme: Catppuccin Latte");
             setTheme("everforest", "Theme: Everforest");
             setLanguage("da", "Language: Dansk");
             setLanguage("ar", "Language: العربية");
             setLanguage("ja", "Language: 日本語");
             setLanguage("en", "Language: English");
         }},
    };

    for (const Scene &scene : scenes) {
        if (!only.isEmpty() && !only.contains(scene.name))
            continue;
        // Each scene starts as the app does: nothing hidden, nothing measured.
        QTest::keyClick(window, Qt::Key_Escape);
        scene.prepare();
        rec.begin(scene.name);
        scene.play();
        rec.finish();
    }
    return 0;
}
