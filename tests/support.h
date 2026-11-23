#pragma once

// What the tests make their scans with: a small DICOM writer, stacks of
// slices with values that say where they came from, a file chooser that
// answers at once, and encoders for the compressed kinds.

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QUrl>
#include <QVector>
#include <QtEndian>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>

#include "dicom.h"
#include "filepicker.h"

// A small DICOM writer, enough to make the files each test needs.
struct Writer {
    bool explicitVr = true;
    bool bigEndian = false;
    QByteArray body;

    void u16(QByteArray &out, quint16 v) const {
        char b[2];
        bigEndian ? qToBigEndian(v, b) : qToLittleEndian(v, b);
        out.append(b, 2);
    }
    void u32(QByteArray &out, quint32 v) const {
        char b[4];
        bigEndian ? qToBigEndian(v, b) : qToLittleEndian(v, b);
        out.append(b, 4);
    }
    QByteArray header(quint32 tag, const QByteArray &vr, quint32 length) const {
        QByteArray out;
        u16(out, tag >> 16);
        u16(out, tag & 0xFFFF);
        if (!explicitVr) {
            u32(out, length);
            return out;
        }
        out += vr;
        static const QList<QByteArray> longVrs{"OB", "OW", "SQ", "UN", "UT"};
        if (longVrs.contains(vr)) {
            out += QByteArray(2, 0);
            u32(out, length);
        } else {
            u16(out, quint16(length));
        }
        return out;
    }
    Writer &add(quint32 tag, const QByteArray &vr, QByteArray value) {
        if (value.size() % 2)
            value += vr == "UI" || vr == "OB" ? '\0' : ' ';
        body += header(tag, vr, value.size()) + value;
        return *this;
    }
    Writer &us(quint32 tag, quint16 v) {
        QByteArray value;
        u16(value, v);
        return add(tag, "US", value);
    }
    Writer &image(int rows, int columns, int bits, int stored, bool isSigned, const QByteArray &photometric = "MONOCHROME2") {
        add(Tag::SeriesInstance, "UI", "1.2.3.4");
        us(Tag::SamplesPerPixel, photometric.startsWith("MONO") ? 1 : 3);
        add(Tag::Photometric, "CS", photometric);
        us(Tag::Rows, rows);
        us(Tag::Columns, columns);
        us(Tag::BitsAllocated, bits);
        us(Tag::BitsStored, stored);
        us(Tag::HighBit, stored - 1);
        return us(Tag::PixelRepresentation, isSigned ? 1 : 0);
    }
    Writer &pixels(const QByteArray &data, const QByteArray &vr = "OW") { return add(Tag::PixelData, vr, data); }
    // A sequence of defined length, each item another writer's body.
    Writer &sequence(quint32 tag, const QList<Writer> &items) {
        QByteArray value;
        for (const auto &item : items) {
            u16(value, 0xFFFE);
            u16(value, 0xE000);
            u32(value, item.body.size());
            value += item.body;
        }
        body += header(tag, "SQ", value.size()) + value;
        return *this;
    }
    Writer &floats(quint32 tag, const QList<float> &values) {
        QByteArray value;
        for (float f : values) {
            quint32 bits;
            std::memcpy(&bits, &f, 4);
            u32(value, bits);
        }
        return add(tag, "FL", value);
    }
    Writer &fd(quint32 tag, double v) {
        quint64 bits;
        std::memcpy(&bits, &v, 8);
        QByteArray value;
        u32(value, quint32(bits));
        u32(value, quint32(bits >> 32));
        return add(tag, "FD", value);
    }
    Writer &ul(quint32 tag, quint32 v) {
        QByteArray value;
        u32(value, v);
        return add(tag, "UL", value);
    }
    // A coded concept: what an item of a structured report is about.
    static Writer code(const QByteArray &value, const QByteArray &meaning) {
        Writer w;
        w.add(0x00080100, "SH", value).add(0x00080102, "SH", "DCM").add(Tag::CodeMeaning, "LO", meaning);
        return w;
    }
    Writer &encapsulated(const QList<QByteArray> &fragments, const QByteArray &offsetTable = {}) {
        QByteArray out;
        u16(out, 0x7FE0);
        u16(out, 0x0010);
        out += "OB";
        out += QByteArray(2, 0);
        u32(out, 0xFFFFFFFF);
        auto item = [&](const QByteArray &data) {
            u16(out, 0xFFFE);
            u16(out, 0xE000);
            u32(out, data.size());
            out += data;
        };
        item(offsetTable);
        for (QByteArray f : fragments) {
            if (f.size() % 2)
                f += '\0';
            item(f);
        }
        u16(out, 0xFFFE);
        u16(out, 0xE0DD);
        u32(out, 0);
        body += out;
        return *this;
    }

    QByteArray file(const QString &transferSyntax) const {
        Writer meta;
        QByteArray uid = transferSyntax.toLatin1();
        meta.add(Tag::TransferSyntax, "UI", uid);
        return QByteArray(128, 0) + "DICM" + meta.body + body;
    }
};

inline QByteArray le16(const QList<int> &values) {
    QByteArray out;
    for (int v : values) {
        char b[2];
        qToLittleEndian(quint16(v), b);
        out.append(b, 2);
    }
    return out;
}

inline bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

inline const QString ExplicitLE = "1.2.840.10008.1.2.1";

// An axial CT stack, 4 columns by 3 rows, 5 slices 3 mm apart from the feet
// up, with each pixel 100 × its slice + 10 × its row + its column, so a
// value says where it came from.
inline bool writeVolume(const QString &folder, const QByteArray &series = "1.2.9.1") {
    for (int k = 0; k < 5; ++k) {
        QList<int> values;
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 4; ++i)
                values << 100 * k + 10 * j + i;
        Writer w;
        w.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.2")
            .add(0x00080018, "UI", series + "." + QByteArray::number(k + 1))
            .add(Tag::Modality, "CS", "CT")
            .add(0x0020000D, "UI", "1.2.9")
            .add(0x00200052, "UI", "1.2.9.100")
            .image(3, 4, 16, 12, false)
            .add(0x0020000E, "UI", series)
            .add(Tag::InstanceNumber, "IS", QByteArray::number(k + 1))
            .add(Tag::ImagePosition, "DS", "0\\0\\" + QByteArray::number(k * 3))
            .add(Tag::ImageOrientation, "DS", "1\\0\\0\\0\\1\\0")
            .add(Tag::PixelSpacing, "DS", "1\\1")
            .pixels(le16(values));
        if (!write(QStringLiteral("%1/%2-%3").arg(folder, QString::fromLatin1(series)).arg(k), w.file(ExplicitLE)))
            return false;
    }
    return true;
}

// A series of n × n slices, each spacing mm a pixel and step mm apart from
// the feet up, in a study and frame of reference, with each pixel's stored
// value from at(column, row, slice).
inline bool writeSeries(const QString &folder, const QByteArray &modality, const QByteArray &series, const QByteArray &frame,
                 int n, int slices, double spacing, double step, const std::function<int(int, int, int)> &at,
                 double origin = 0) {
    for (int k = 0; k < slices; ++k) {
        QList<int> values;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                values << at(i, j, k);
        const QByteArray mm = QByteArray::number(spacing);
        Writer w;
        w.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.2")
            .add(0x00080018, "UI", series + "." + QByteArray::number(k + 1))
            .add(Tag::Modality, "CS", modality)
            .add(0x0020000D, "UI", "1.2.77")
            .add(0x00200052, "UI", frame)
            .image(n, n, 16, 16, false)
            .add(0x0020000E, "UI", series)
            .add(Tag::InstanceNumber, "IS", QByteArray::number(k + 1))
            .add(Tag::ImagePosition, "DS", QByteArray::number(origin) + "\\" + QByteArray::number(origin) + "\\" + QByteArray::number(k * step))
            .add(Tag::ImageOrientation, "DS", "1\\0\\0\\0\\1\\0")
            .add(Tag::PixelSpacing, "DS", mm + "\\" + mm);
        if (modality == "CT")
            w.add(0x00281052, "DS", "-1024").add(0x00281053, "DS", "1");
        w.pixels(le16(values));
        if (!write(QStringLiteral("%1/%2-%3").arg(folder, QString::fromLatin1(series)).arg(k), w.file(ExplicitLE)))
            return false;
    }
    return true;
}

// A stack of slices placed wherever a test wants them: columns × rows
// pixels, across and down mm apart, running along row and column (unit
// vectors at right angles), with the first pixel's centre of each slice at
// one of positions. Each pixel's stored value is from at(the patient point
// at its centre), so the stack is a picture of a shape that lies in the
// patient, however its slices cut it.
inline bool writeSlicesAt(const QString &folder, const QByteArray &modality, const QByteArray &series, const QByteArray &frame,
                          int columns, int rows, double across, double down, const double *row, const double *column,
                          const QList<std::array<double, 3>> &positions, const std::function<int(const double *)> &at) {
    // To the micrometre's thousandth, well inside a decimal string's 16
    // characters.
    auto mm = [](double v) { return QByteArray::number(v, 'f', 9); };
    for (int k = 0; k < positions.size(); ++k) {
        const auto &first = positions[k];
        QList<int> values;
        for (int j = 0; j < rows; ++j)
            for (int i = 0; i < columns; ++i) {
                double p[3];
                for (int a = 0; a < 3; ++a)
                    p[a] = first[a] + i * across * row[a] + j * down * column[a];
                values << at(p);
            }
        Writer w;
        w.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.2")
            .add(0x00080018, "UI", series + "." + QByteArray::number(k + 1))
            .add(Tag::Modality, "CS", modality)
            .add(0x0020000D, "UI", "1.2.77")
            .add(0x00200052, "UI", frame)
            .image(rows, columns, 16, 16, false)
            .add(0x0020000E, "UI", series)
            .add(Tag::InstanceNumber, "IS", QByteArray::number(k + 1))
            .add(Tag::ImagePosition, "DS", mm(first[0]) + "\\" + mm(first[1]) + "\\" + mm(first[2]))
            .add(Tag::ImageOrientation, "DS", mm(row[0]) + "\\" + mm(row[1]) + "\\" + mm(row[2]) + "\\" + mm(column[0]) + "\\"
                                                  + mm(column[1]) + "\\" + mm(column[2]))
            .add(Tag::PixelSpacing, "DS", mm(down) + "\\" + mm(across));
        if (modality == "CT")
            w.add(0x00281052, "DS", "-1024").add(0x00281053, "DS", "1");
        w.pixels(le16(values));
        if (!write(QStringLiteral("%1/%2-%3").arg(folder, QString::fromLatin1(series)).arg(k), w.file(ExplicitLE)))
            return false;
    }
    return true;
}

// A stack as a scanner takes it with its gantry leaning tilt degrees about
// the patient's left-right axis: the slices lean, their rows still running
// to the patient's left, while the table carries the patient straight
// through, so each slice's centre is step mm further along z than the one
// before. The slices are centred on the z axis, and the stack's middle is
// at z = 0. With no tilt it is an ordinary axial stack.
inline bool writeTiltedSeries(const QString &folder, const QByteArray &modality, const QByteArray &series,
                              const QByteArray &frame, int columns, int rows, int slices, double spacing, double step,
                              double tilt, const std::function<int(const double *)> &at) {
    const double radians = tilt * M_PI / 180;
    const double row[3] = {1, 0, 0}, column[3] = {0, std::cos(radians), std::sin(radians)};
    QList<std::array<double, 3>> positions;
    for (int k = 0; k < slices; ++k) {
        std::array<double, 3> first = {0, 0, (k - (slices - 1) / 2.0) * step};
        for (int a = 0; a < 3; ++a)
            first[a] -= (columns - 1) / 2.0 * spacing * row[a] + (rows - 1) / 2.0 * spacing * column[a];
        positions << first;
    }
    return writeSlicesAt(folder, modality, series, frame, columns, rows, spacing, spacing, row, column, positions, at);
}

// A picker that answers at once with a path the test gave it.
class FakePicker : public FilePicker {
public:
    QString answer;
    Purpose asked = Purpose::OpenScan;
    void choose(QObject *asker, Purpose purpose, const QString &) override {
        asked = purpose;
        // A dialog that is cancelled answers nobody.
        if (!answer.isEmpty())
            emit selected(asker, purpose, {QUrl::fromLocalFile(answer)});
    }
};

// The number at the end of what valueAt says, after the pointer's place.
inline double shownNumber(const QString &text) {
    const QRegularExpression number("(-?[0-9.]+)\\s*[A-Z]*$");
    return number.match(text.trimmed()).captured(1).toDouble();
}

// Encodes grey samples with ffmpeg, or returns nothing when it can't.
inline QByteArray encode(const QList<int> &values, int width, const QString &pixelFormat, const QStringList &codec) {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
        return {};
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", QStringList{"-v", "error", "-f", "rawvideo", "-pix_fmt", pixelFormat, "-s",
                                       QString("%1x%2").arg(width).arg(values.size() / width), "-i", "-"} + codec + QStringList{"-"});
    ffmpeg.write(le16(values));
    ffmpeg.closeWriteChannel();
    ffmpeg.waitForFinished();
    return ffmpeg.exitCode() == 0 ? ffmpeg.readAllStandardOutput() : QByteArray();
}

// One plane of bytes as an RLE segment (PS3.5 annex G): runs of a byte
// repeated, and stretches of bytes as they are.
inline QByteArray packBits(const QByteArray &plane) {
    QByteArray out;
    for (qsizetype i = 0; i < plane.size();) {
        qsizetype run = 1;
        while (i + run < plane.size() && run < 128 && plane[i + run] == plane[i])
            ++run;
        if (run > 1) {
            out += char(1 - run);
            out += plane[i];
            i += run;
            continue;
        }
        qsizetype n = 1;
        while (i + n < plane.size() && n < 128 && (i + n + 1 == plane.size() || plane[i + n] != plane[i + n + 1]))
            ++n;
        out += char(n - 1);
        out += plane.mid(i, n);
        i += n;
    }
    return out;
}

// An RLE frame: a header of where each segment starts, then the segments,
// one for each byte of each sample, the most significant first.
inline QByteArray rleFrame(const QList<QByteArray> &planes) {
    QByteArray header(64, 0), body;
    qToLittleEndian<quint32>(quint32(planes.size()), header.data());
    for (qsizetype i = 0; i < planes.size(); ++i) {
        qToLittleEndian<quint32>(quint32(64 + body.size()), header.data() + 4 + 4 * i);
        body += packBits(planes[i]);
        if (body.size() % 2)
            body += '\0';
    }
    return header + body;
}

// A plain lossless JPEG encoder (T.81 process 14), with one Huffman table
// giving each difference category a 5-bit code.
inline QByteArray losslessJpeg(const QVector<quint16> &samples, int width, int height, int components, int precision,
                        int predictor, int restartInterval) {
    QByteArray out("\xFF\xD8", 2);
    auto u16 = [&](int v) { out += char(v >> 8); out += char(v & 0xFF); };
    out += "\xFF\xC4";
    u16(2 + 1 + 16 + 17);
    out += char(0x00);
    for (int length = 1; length <= 16; ++length)
        out += char(length == 5 ? 17 : 0);
    for (int symbol = 0; symbol <= 16; ++symbol)
        out += char(symbol);
    if (restartInterval) {
        out += "\xFF\xDD";
        u16(4);
        u16(restartInterval);
    }
    out += "\xFF\xC3";
    u16(8 + 3 * components);
    out += char(precision);
    u16(height);
    u16(width);
    out += char(components);
    for (int c = 0; c < components; ++c)
        { out += char(c + 1); out += char(0x11); out += char(0); }
    out += "\xFF\xDA";
    u16(6 + 2 * components);
    out += char(components);
    for (int c = 0; c < components; ++c)
        { out += char(c + 1); out += char(0); }
    out += char(predictor);
    out += char(0);
    out += char(0);

    quint32 buffer = 0;
    int count = 0;
    auto put = [&](quint32 bits, int n) {
        for (int i = n - 1; i >= 0; --i) {
            buffer = (buffer << 1) | ((bits >> i) & 1);
            if (++count == 8) {
                out += char(buffer);
                if (uchar(buffer) == 0xFF)
                    out += char(0);
                buffer = 0;
                count = 0;
            }
        }
    };
    auto flush = [&] {
        while (count)
            put(1, 1);
    };
    int untilRestart = restartInterval, marker = 0;
    bool firstLine = true;
    auto at = [&](int x, int y, int c) { return int(samples[(y * width + x) * components + c]); };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (restartInterval && untilRestart == 0) {
                flush();
                out += char(0xFF);
                out += char(0xD0 + marker++ % 8);
                untilRestart = restartInterval;
                firstLine = true;
            }
            for (int c = 0; c < components; ++c) {
                int prediction;
                if (firstLine && x == 0)
                    prediction = 1 << (precision - 1);
                else if (firstLine)
                    prediction = at(x - 1, y, c);
                else if (x == 0)
                    prediction = at(x, y - 1, c);
                else {
                    const int a = at(x - 1, y, c), b = at(x, y - 1, c), d = at(x - 1, y - 1, c);
                    const int p[] = {0, a, b, d, a + b - d, a + ((b - d) >> 1), b + ((a - d) >> 1), (a + b) >> 1};
                    prediction = p[predictor];
                }
                // Differences are taken modulo 2^16, as the standard says.
                int diff = (at(x, y, c) - prediction) & 0xFFFF;
                if (diff >= 32768)
                    diff -= 65536;
                if (diff == -32768) {
                    put(16, 5);
                    continue;
                }
                int category = 0;
                while ((diff < 0 ? -diff : diff) >> category)
                    ++category;
                put(category, 5);
                if (category)
                    put(diff < 0 ? diff - 1 + (1 << category) : diff, category);
            }
            if (restartInterval)
                --untilRestart;
        }
        firstLine = false;
    }
    flush();
    out += "\xFF\xD9";
    return out;
}
