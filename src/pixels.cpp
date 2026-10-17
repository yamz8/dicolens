#include "pixels.h"

#include "ljpeg.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>
#include <climits>
#include <cstring>
#include <cmath>

namespace {

const QString Jpeg = "jpeg_pipe", JpegLs = "jpegls_pipe", Jpeg2000 = "j2k_pipe";

// Which ffmpeg demuxer reads a compressed transfer syntax, if any does.
QString demuxerFor(const QString &uid) {
    static const QHash<QString, QString> demuxers{
        {"1.2.840.10008.1.2.4.50", Jpeg},     {"1.2.840.10008.1.2.4.51", Jpeg},
        {"1.2.840.10008.1.2.4.57", Jpeg},     {"1.2.840.10008.1.2.4.70", Jpeg},
        {"1.2.840.10008.1.2.4.80", JpegLs},   {"1.2.840.10008.1.2.4.81", JpegLs},
        {"1.2.840.10008.1.2.4.90", Jpeg2000}, {"1.2.840.10008.1.2.4.91", Jpeg2000},
        {"1.2.840.10008.1.2.4.92", Jpeg2000}, {"1.2.840.10008.1.2.4.93", Jpeg2000},
        {"1.2.840.10008.1.2.4.201", Jpeg2000}, {"1.2.840.10008.1.2.4.202", Jpeg2000},
        {"1.2.840.10008.1.2.4.203", Jpeg2000},
    };
    return demuxers.value(uid);
}

bool isRle(const QString &uid) { return uid == "1.2.840.10008.1.2.5"; }

QString ffmpeg() {
    static const QString path = QStandardPaths::findExecutable("ffmpeg");
    return path;
}

Frame failed(const QString &error, const QString &detail = {}) {
    Frame frame;
    frame.error = error;
    frame.detail = detail;
    return frame;
}

QByteArray readSpan(const QString &path, qint64 offset, qint64 length) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(offset))
        return {};
    return file.read(length);
}

quint8 clamp8(double v) { return quint8(std::clamp(v, 0.0, 255.0) + 0.5); }

void ybrToRgb(int y, int cb, int cr, uchar *out) {
    out[0] = clamp8(y + 1.402 * (cr - 128));
    out[1] = clamp8(y - 0.344136 * (cb - 128) - 0.714136 * (cr - 128));
    out[2] = clamp8(y + 1.772 * (cb - 128));
}

// Palette colour: stored values index three lookup tables.
Frame applyPalette(const Frame &indices, const DicomFile &file) {
    QList<QList<double>> descriptors;
    QList<QByteArray> tables;
    for (auto [d, t] : {std::pair{Tag::RedPaletteDescriptor, Tag::RedPaletteData},
                        std::pair{Tag::GreenPaletteDescriptor, Tag::GreenPaletteData},
                        std::pair{Tag::BluePaletteDescriptor, Tag::BluePaletteData}}) {
        const auto *table = file.element(t);
        descriptors << file.numbers(d);
        tables << (table ? table->value : QByteArray());
    }
    for (int c = 0; c < 3; ++c)
        if (descriptors[c].size() < 3 || tables[c].isEmpty())
            return failed("This palette colour image has no palette");

    Frame out;
    out.width = indices.width;
    out.height = indices.height;
    out.color = QImage(out.width, out.height, QImage::Format_RGB888);
    for (int y = 0; y < out.height; ++y) {
        uchar *line = out.color.scanLine(y);
        for (int x = 0; x < out.width; ++x) {
            const int index = indices.stored(x, y);
            for (int c = 0; c < 3; ++c) {
                const int entries = descriptors[c][0] == 0 ? 65536 : int(descriptors[c][0]);
                const int firstMapped = int(descriptors[c][1]);
                const int bits = int(descriptors[c][2]);
                const int i = std::clamp(index - firstMapped, 0, entries - 1);
                const QByteArray &t = tables[c];
                int v = 0;
                if (bits <= 8 && t.size() >= entries)
                    v = uchar(t[i]);
                else if (2 * i + 1 < t.size())
                    v = qFromLittleEndian<quint16>(t.constData() + 2 * i) >> 8;
                line[3 * x + c] = uchar(v);
            }
        }
    }
    return out;
}

// Grey samples from a decoder into a frame. Codestreams hold the stored
// bit pattern; signed values are told apart by the top stored bit, as in
// uncompressed data. A decoder that shifted signed samples up to make them
// unsigned says by how much in levelShift.
Frame monoFrame(const ImageFormat &format, qint64 pixels, int levelShift, auto sampleAt) {
    const int bits = std::clamp(format.bitsStored, 1, 16);
    const quint32 mask = bits >= 16 ? 0xFFFF : (1u << bits) - 1;
    Frame frame;
    frame.width = format.columns;
    frame.height = format.rows;
    frame.mono.resize(pixels);
    frame.isSigned = format.isSigned;
    for (qint64 i = 0; i < pixels; ++i) {
        quint32 v = quint32(int(sampleAt(i)) - levelShift);
        v &= mask;
        if (format.isSigned && bits < 16 && (v & (1u << (bits - 1))))
            v |= ~mask;
        frame.mono[i] = quint16(v);
    }
    frame.slope = format.slope;
    frame.intercept = format.intercept;
    frame.inverted = format.photometric == "MONOCHROME1";
    return frame;
}

// JPEG 2000 marks signed components in its SIZ segment, and ffmpeg hands
// them back shifted up by half their range.
int jpeg2000Shift(const QByteArray &codestream) {
    const int siz = codestream.indexOf("\xFF\x51");
    if (siz < 0 || siz + 41 > codestream.size())
        return 0;
    const uchar ssiz = uchar(codestream[siz + 40]);
    return ssiz & 0x80 ? 1 << (ssiz & 0x7F) : 0;
}

Frame runFfmpeg(const QByteArray &codestream, const QString &demuxer, const ImageFormat &format) {
    if (ffmpeg().isEmpty())
        return failed("ffmpeg isn't installed, and this image needs it");

    QStringList arguments{"-v", "error", "-threads", "1"};
    if (!demuxer.isEmpty())
        arguments << "-f" << demuxer;
    arguments << "-i" << "-" << "-frames:v" << "1" << "-f" << "rawvideo";
    // Grey images come out in the decoder's own format so 12-bit values
    // aren't stretched to 16; colour is converted to RGB, which also undoes
    // the YCbCr a JPEG keeps colour in.
    if (format.samples > 1)
        arguments << "-pix_fmt" << "rgb24";
    arguments << "-";

    QProcess process;
    process.start(ffmpeg(), arguments);
    if (!process.waitForStarted(10000))
        return failed("ffmpeg couldn't be started", process.errorString());
    process.write(codestream);
    process.closeWriteChannel();
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(1000);
        return failed("ffmpeg took too long to decode this image");
    }
    const QByteArray output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || output.isEmpty())
        return failed("ffmpeg couldn't decode this image", QString::fromUtf8(process.readAllStandardError()).trimmed());

    const qint64 pixels = qint64(format.rows) * format.columns;
    Frame frame;
    frame.width = format.columns;
    frame.height = format.rows;
    if (format.samples > 1) {
        if (output.size() < pixels * 3)
            return failed("The decoded image is smaller than its header says");
        frame.color = QImage(frame.width, frame.height, QImage::Format_RGB888);
        for (int y = 0; y < frame.height; ++y)
            std::memcpy(frame.color.scanLine(y), output.constData() + qint64(y) * frame.width * 3, frame.width * 3);
        return frame;
    }

    int bytes = 0;
    if (output.size() == pixels)
        bytes = 1;
    else if (output.size() == pixels * 2)
        bytes = 2;
    else
        return failed("The decoded image is a different size than its header says");

    const auto *p = reinterpret_cast<const uchar *>(output.constData());
    const int shift = demuxer == Jpeg2000 ? jpeg2000Shift(codestream) : 0;
    if (bytes == 1)
        return monoFrame(format, pixels, shift, [p](qint64 i) { return p[i]; });
    return monoFrame(format, pixels, shift, [p](qint64 i) { return qFromLittleEndian<quint16>(p + 2 * i); });
}

Frame decodeLossless(const QByteArray &codestream, const ImageFormat &format) {
    LosslessJpeg jpeg;
    QString error;
    bool notLossless = false;
    if (!decodeLosslessJpeg(codestream, jpeg, &error, &notLossless))
        return notLossless ? runFfmpeg(codestream, Jpeg, format) : failed(error);
    if (jpeg.width != format.columns || jpeg.height != format.rows || jpeg.components != format.samples)
        return failed("The decoded image is a different size than its header says");
    const qint64 pixels = qint64(jpeg.width) * jpeg.height;
    const quint16 *samples = jpeg.samples.constData();
    if (jpeg.components == 1)
        return monoFrame(format, pixels, 0, [samples](qint64 i) { return samples[i]; });

    Frame frame;
    frame.width = jpeg.width;
    frame.height = jpeg.height;
    frame.color = QImage(jpeg.width, jpeg.height, QImage::Format_RGB888);
    const int down = std::max(0, jpeg.precision - 8);
    for (int y = 0; y < jpeg.height; ++y) {
        uchar *line = frame.color.scanLine(y);
        for (int x = 0; x < jpeg.width; ++x)
            for (int c = 0; c < 3; ++c)
                line[3 * x + c] = uchar(std::min(255, samples[(qint64(y) * jpeg.width + x) * jpeg.components + c] >> down));
    }
    return frame;
}

}

ImageFormat ImageFormat::from(const DicomFile &file, int frame) {
    ImageFormat f;
    f.rows = file.integer(Tag::Rows);
    f.columns = file.integer(Tag::Columns);
    f.frames = std::max(1, file.integer(Tag::NumberOfFrames, 1));
    f.samples = std::max(1, file.integer(Tag::SamplesPerPixel, 1));
    f.bitsAllocated = file.integer(Tag::BitsAllocated, 16);
    f.bitsStored = file.integer(Tag::BitsStored, f.bitsAllocated);
    f.highBit = file.integer(Tag::HighBit, f.bitsStored - 1);
    f.isSigned = file.integer(Tag::PixelRepresentation) == 1;
    f.planar = file.integer(Tag::PlanarConfiguration);
    f.photometric = file.first(Tag::Photometric).toUpper();
    if (f.photometric.isEmpty())
        f.photometric = f.samples > 1 ? "RGB" : "MONOCHROME2";
    const auto slope = file.numbers(file.find(Tag::RescaleSlope, frame));
    const auto intercept = file.numbers(file.find(Tag::RescaleIntercept, frame));
    f.slope = slope.isEmpty() || slope.first() == 0 ? 1 : slope.first();
    f.intercept = intercept.isEmpty() ? 0 : intercept.first();
    f.transferSyntax = file.transferSyntax();
    f.littleEndian = file.littleEndian();
    return f;
}

int Frame::stored(int x, int y) const {
    const quint16 v = mono.value(qint64(y) * width + x);
    return isSigned ? int(qint16(v)) : int(v);
}

double Frame::value(int x, int y) const { return stored(x, y) * slope + intercept; }

void Frame::range(double &low, double &high) const {
    if (mono.isEmpty()) {
        low = 0;
        high = 255;
        return;
    }
    int lo = INT_MAX, hi = INT_MIN;
    for (quint16 raw : mono) {
        const int v = isSigned ? int(qint16(raw)) : int(raw);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const double a = lo * slope + intercept, b = hi * slope + intercept;
    low = std::min(a, b);
    high = std::max(a, b);
}

void defaultWindow(const DicomFile &file, const Frame &frame, int frameIndex, double &center, double &width) {
    const auto centers = file.numbers(file.find(Tag::WindowCenter, frameIndex));
    const auto widths = file.numbers(file.find(Tag::WindowWidth, frameIndex));
    if (!centers.isEmpty() && !widths.isEmpty() && widths.first() >= 1) {
        center = centers.first();
        width = widths.first();
        return;
    }
    double low = 0, high = 255;
    frame.range(low, high);
    center = (low + high) / 2;
    width = std::max(1.0, high - low + 1);
}

QImage render(const Frame &frame, double center, double width, bool invert) {
    if (frame.isNull())
        return {};
    if (frame.isColor()) {
        QImage image = frame.color;
        if (invert)
            image.invertPixels();
        return image;
    }

    // DICOM's linear window (PS3.3 C.11.2.1.2), one table entry for every
    // possible stored value, so drawing is a lookup a pixel.
    width = std::max(1.0, width);
    const bool flip = frame.inverted != invert;
    static thread_local QVector<uchar> lut(65536);
    for (int k = 0; k < 65536; ++k) {
        const int stored = frame.isSigned ? int(qint16(quint16(k))) : k;
        const double v = stored * frame.slope + frame.intercept;
        double y;
        if (v <= center - 0.5 - (width - 1) / 2)
            y = 0;
        else if (v > center - 0.5 + (width - 1) / 2)
            y = 255;
        else
            y = ((v - (center - 0.5)) / (width - 1) + 0.5) * 255;
        const uchar g = uchar(std::clamp(y, 0.0, 255.0) + 0.5);
        lut[k] = flip ? 255 - g : g;
    }
    QImage image(frame.width, frame.height, QImage::Format_Grayscale8);
    const quint16 *src = frame.mono.constData();
    for (int y = 0; y < frame.height; ++y) {
        uchar *line = image.scanLine(y);
        const quint16 *row = src + qint64(y) * frame.width;
        for (int x = 0; x < frame.width; ++x)
            line[x] = lut[row[x]];
    }
    return image;
}

Frame convertNative(const uchar *data, qint64 size, const ImageFormat &f) {
    const qint64 pixels = qint64(f.rows) * f.columns;
    if (pixels <= 0)
        return failed("This file has no image size");
    if (f.bitsAllocated != 8 && f.bitsAllocated != 16)
        return failed("This image's pixel format isn't supported", QStringLiteral("%1-bit").arg(f.bitsAllocated));

    const int bytes = f.bitsAllocated / 8;
    Frame frame;
    frame.width = f.columns;
    frame.height = f.rows;

    if (f.samples == 1) {
        if (size < pixels * bytes)
            return failed("The image data is cut short");
        const int bits = std::clamp(f.bitsStored, 1, f.bitsAllocated);
        const int shift = std::max(0, f.highBit + 1 - bits);
        const quint32 mask = bits >= 32 ? 0xFFFFFFFF : (1u << bits) - 1;
        frame.mono.resize(pixels);
        for (qint64 i = 0; i < pixels; ++i) {
            quint32 w = bytes == 1 ? data[i]
                        : f.littleEndian ? qFromLittleEndian<quint16>(data + 2 * i)
                                         : qFromBigEndian<quint16>(data + 2 * i);
            quint32 v = (w >> shift) & mask;
            if (f.isSigned && (v & (1u << (bits - 1))))
                v |= ~mask;
            frame.mono[i] = quint16(v);
        }
        frame.isSigned = f.isSigned;
        frame.slope = f.slope;
        frame.intercept = f.intercept;
        frame.inverted = f.photometric == "MONOCHROME1";
        return frame;
    }

    if (f.samples != 3)
        return failed("This image's pixel format isn't supported", QStringLiteral("%1 samples").arg(f.samples));

    frame.color = QImage(f.columns, f.rows, QImage::Format_RGB888);
    if (f.photometric == "YBR_FULL_422" || f.photometric == "YBR_PARTIAL_422") {
        // Two pixels share their colour: Y1 Y2 Cb Cr.
        if (bytes != 1 || size < pixels * 2)
            return failed("The image data is cut short");
        for (int y = 0; y < f.rows; ++y) {
            uchar *line = frame.color.scanLine(y);
            for (int x = 0; x < f.columns; x += 2) {
                const uchar *q = data + (qint64(y) * f.columns + x) * 2;
                ybrToRgb(q[0], q[2], q[3], line + 3 * x);
                if (x + 1 < f.columns)
                    ybrToRgb(q[1], q[2], q[3], line + 3 * (x + 1));
            }
        }
        return frame;
    }
    if (size < pixels * 3 * bytes)
        return failed("The image data is cut short");
    const bool ybr = f.photometric.startsWith("YBR");
    const int down = std::max(0, std::clamp(f.bitsStored, 8, 16) - 8);
    auto sample = [&](qint64 pixel, int s) -> int {
        const qint64 i = f.planar == 1 ? s * pixels + pixel : pixel * 3 + s;
        const int v = bytes == 1 ? data[i]
                      : f.littleEndian ? qFromLittleEndian<quint16>(data + 2 * i) : qFromBigEndian<quint16>(data + 2 * i);
        return bytes == 1 ? v : std::min(255, v >> down);
    };
    for (int y = 0; y < f.rows; ++y) {
        uchar *line = frame.color.scanLine(y);
        for (int x = 0; x < f.columns; ++x) {
            const qint64 p = qint64(y) * f.columns + x;
            if (ybr)
                ybrToRgb(sample(p, 0), sample(p, 1), sample(p, 2), line + 3 * x);
            else
                for (int s = 0; s < 3; ++s)
                    line[3 * x + s] = uchar(sample(p, s));
        }
    }
    return frame;
}

QByteArray decodeRle(const QByteArray &data, const ImageFormat &f, QString *error) {
    // PS3.5 annex G: a header of up to 15 segment offsets, then one
    // PackBits segment per byte of each sample, most significant first.
    const qint64 pixels = qint64(f.rows) * f.columns;
    const int bytes = f.bitsAllocated / 8;
    if (data.size() < 64 || bytes < 1) {
        if (error)
            *error = "The image data is cut short";
        return {};
    }
    const auto *p = reinterpret_cast<const uchar *>(data.constData());
    const quint32 count = qFromLittleEndian<quint32>(p);
    if (count != quint32(f.samples * bytes) || count > 15) {
        if (error)
            *error = "This RLE image is damaged";
        return {};
    }
    // Rebuilt as planar, little-endian samples for convertNative.
    QByteArray out(pixels * f.samples * bytes, 0);
    auto *o = reinterpret_cast<uchar *>(out.data());
    for (quint32 segment = 0; segment < count; ++segment) {
        const qint64 start = qFromLittleEndian<quint32>(p + 4 + 4 * segment);
        const qint64 end = segment + 1 < count ? qFromLittleEndian<quint32>(p + 8 + 4 * segment) : data.size();
        const int sample = segment / bytes;
        const int byte = bytes - 1 - segment % bytes;
        qint64 i = start, n = 0;
        auto put = [&](uchar v) {
            if (n < pixels)
                o[(sample * pixels + n) * bytes + byte] = v;
            ++n;
        };
        while (i < std::min<qint64>(end, data.size()) && n < pixels) {
            const int header = qint8(p[i++]);
            if (header >= 0) {
                for (int k = 0; k <= header && i < data.size(); ++k)
                    put(p[i++]);
            } else if (header != -128 && i < data.size()) {
                const uchar v = p[i++];
                for (int k = 0; k < 1 - header; ++k)
                    put(v);
            }
        }
    }
    return out;
}

bool isVideo(const QString &uid) {
    return uid.startsWith("1.2.840.10008.1.2.4.10");
}

bool canDecode(const QString &uid) {
    return uid == "1.2.840.10008.1.2" || uid == "1.2.840.10008.1.2.1" || uid == "1.2.840.10008.1.2.2" ||
           isRle(uid) || !demuxerFor(uid).isEmpty() || isVideo(uid);
}

Frame decodeFrame(const DicomFile &file, int frameIndex, const QList<QByteArray> *split) {
    if (!file.hasPixelData())
        return failed("This file has no image");
    ImageFormat f = ImageFormat::from(file, frameIndex);
    if (f.rows <= 0 || f.columns <= 0)
        return failed("This file has no image size");
    if (frameIndex < 0 || frameIndex >= f.frames)
        return failed("The image data is cut short");
    const QString uid = f.transferSyntax;
    const bool palette = f.photometric == "PALETTE COLOR";

    Frame frame;
    if (!file.encapsulated()) {
        const qint64 pixels = qint64(f.rows) * f.columns;
        const bool subsampled = f.photometric.endsWith("_422");
        const qint64 frameBytes = subsampled ? pixels * 2 : pixels * f.samples * std::max(1, f.bitsAllocated / 8);
        const qint64 offset = qint64(frameIndex) * frameBytes;
        if (offset + frameBytes > file.pixelLength())
            return failed("The image data is cut short");
        const QByteArray data = readSpan(file.path(), file.pixelOffset() + offset, frameBytes);
        // Big-endian files store 8-bit pixels as bytes, which need no swap.
        ImageFormat native = f;
        if (file.pixelVr() == "OB")
            native.littleEndian = true;
        frame = convertNative(reinterpret_cast<const uchar *>(data.constData()), data.size(), native);
    } else {
        const QList<QByteArray> frames = split ? *split
            : DicomFile::splitFrames(readSpan(file.path(), file.pixelOffset(), file.pixelLength()), f.frames);
        if (frameIndex >= frames.size() || frames[frameIndex].isEmpty())
            return failed("The image data is cut short");
        const QByteArray &codestream = frames[frameIndex];
        if (isRle(uid)) {
            QString error;
            const QByteArray planes = decodeRle(codestream, f, &error);
            if (planes.isEmpty())
                return failed(error);
            ImageFormat planar = f;
            planar.planar = 1;
            planar.littleEndian = true;
            // RLE colour is stored as RGB even when the header says YBR.
            if (planar.photometric.startsWith("YBR"))
                planar.photometric = "RGB";
            frame = convertNative(reinterpret_cast<const uchar *>(planes.constData()), planes.size(), planar);
        } else if (uid == "1.2.840.10008.1.2.4.57" || uid == "1.2.840.10008.1.2.4.70") {
            frame = decodeLossless(codestream, f);
        } else if (const QString demuxer = demuxerFor(uid); !demuxer.isEmpty()) {
            frame = runFfmpeg(codestream, demuxer, f);
        } else {
            QString name = Dictionary::transferSyntaxName(uid);
            return failed("This image is compressed in a way scans can't read", name.isEmpty() ? uid : name);
        }
    }
    if (frame.error.isEmpty() && palette)
        frame = applyPalette(frame, file);
    if (frame.error.isEmpty())
        defaultWindow(file, frame, frameIndex, frame.windowCenter, frame.windowWidth);
    return frame;
}

QList<QByteArray> readFrames(const DicomFile &file) {
    if (!file.encapsulated())
        return {};
    return DicomFile::splitFrames(readSpan(file.path(), file.pixelOffset(), file.pixelLength()),
                                  std::max(1, file.integer(Tag::NumberOfFrames, 1)));
}

QList<Frame> decodeVideo(const DicomFile &file) {
    const ImageFormat f = ImageFormat::from(file);
    const QByteArray span = readSpan(file.path(), file.pixelOffset(), file.pixelLength());
    QByteArray stream;
    for (const auto &fragment : DicomFile::splitFrames(span, 1))
        stream += fragment;
    if (ffmpeg().isEmpty())
        return {failed("ffmpeg isn't installed, and this image needs it")};

    QProcess process;
    process.start(ffmpeg(), {"-v", "error", "-i", "-", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"});
    if (!process.waitForStarted(10000))
        return {failed("ffmpeg couldn't be started", process.errorString())};
    process.write(stream);
    process.closeWriteChannel();
    if (!process.waitForFinished(300000)) {
        process.kill();
        process.waitForFinished(1000);
        return {failed("ffmpeg took too long to decode this image")};
    }
    const QByteArray output = process.readAllStandardOutput();
    const qint64 frameBytes = qint64(f.rows) * f.columns * 3;
    if (process.exitCode() != 0 || frameBytes <= 0 || output.size() < frameBytes)
        return {failed("ffmpeg couldn't decode this image", QString::fromUtf8(process.readAllStandardError()).trimmed())};

    QList<Frame> frames;
    for (qint64 offset = 0; offset + frameBytes <= output.size(); offset += frameBytes) {
        Frame frame;
        frame.width = f.columns;
        frame.height = f.rows;
        frame.color = QImage(f.columns, f.rows, QImage::Format_RGB888);
        for (int y = 0; y < f.rows; ++y)
            std::memcpy(frame.color.scanLine(y), output.constData() + offset + qint64(y) * f.columns * 3, f.columns * 3);
        frames << frame;
    }
    return frames;
}
