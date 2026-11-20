// Prints what Dicolens decodes from a file, a line of JSON for each frame,
// for tools/conformance.py to hold against a reference decoder's values. A
// development tool, never installed.
//
//   dicolens_pixeldump [--raw FOLDER] FILE
//
// A grey frame's values are its stored ones, as the 16-bit patterns the
// viewer keeps (little-endian in the raw file, and in the hash); a colour
// frame's are its red, green and blue bytes, a pixel after another.

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <cstdio>

#include "dicom.h"
#include "pixels.h"

namespace {

QByteArray valuesOf(const Frame &frame) {
    QByteArray out;
    if (frame.isColor()) {
        const QImage image = frame.color.convertToFormat(QImage::Format_RGB888);
        for (int y = 0; y < image.height(); ++y)
            out.append(reinterpret_cast<const char *>(image.constScanLine(y)), qsizetype(image.width()) * 3);
        return out;
    }
    out.resize(frame.mono.size() * 2);
    for (qsizetype i = 0; i < frame.mono.size(); ++i)
        qToLittleEndian<quint16>(frame.mono[i], out.data() + 2 * i);
    return out;
}

QJsonObject describe(const Frame &frame, int index, const QString &rawFolder) {
    QJsonObject o{{"frame", index}};
    if (!frame.error.isEmpty() || frame.isNull()) {
        o["error"] = frame.error.isEmpty() ? QStringLiteral("Nothing was decoded") : frame.error;
        o["detail"] = frame.detail;
        return o;
    }
    const QByteArray values = valuesOf(frame);
    o["width"] = frame.width;
    o["height"] = frame.height;
    o["kind"] = frame.isColor() ? "rgb" : "grey";
    o["sha256"] = QString::fromLatin1(QCryptographicHash::hash(values, QCryptographicHash::Sha256).toHex());
    if (frame.isColor()) {
        qint64 sum = 0;
        for (char c : values)
            sum += uchar(c);
        o["sum"] = double(sum);
    } else {
        qint64 sum = 0;
        int low = frame.stored(0, 0), high = low;
        for (int y = 0; y < frame.height; ++y)
            for (int x = 0; x < frame.width; ++x) {
                const int v = frame.stored(x, y);
                sum += v;
                low = std::min(low, v);
                high = std::max(high, v);
            }
        o["signed"] = frame.isSigned;
        o["inverted"] = frame.inverted;
        o["slope"] = frame.slope;
        o["intercept"] = frame.intercept;
        o["min"] = low;
        o["max"] = high;
        o["sum"] = double(sum);
    }
    if (!rawFolder.isEmpty()) {
        const QString path = QStringLiteral("%1/%2.raw").arg(rawFolder).arg(index);
        QFile raw(path);
        if (raw.open(QIODevice::WriteOnly) && raw.write(values) == values.size())
            o["raw"] = path;
    }
    return o;
}

void print(const QJsonObject &o) {
    std::puts(QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
}

}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QStringList arguments = app.arguments().mid(1);
    QString rawFolder;
    if (arguments.size() >= 2 && arguments.first() == "--raw") {
        rawFolder = arguments[1];
        arguments = arguments.mid(2);
        QDir().mkpath(rawFolder);
    }
    if (arguments.size() != 1) {
        std::fputs("Usage: dicolens_pixeldump [--raw FOLDER] FILE\n", stderr);
        return 2;
    }

    QString error;
    const DicomFile file = DicomFile::read(arguments.first(), &error);
    if (!file.isValid()) {
        print({{"error", error}});
        return 1;
    }
    const ImageFormat format = ImageFormat::from(file);
    print({{"transferSyntax", file.transferSyntax()}, {"frames", format.frames}, {"photometric", format.photometric},
           {"video", isVideo(file.transferSyntax())}, {"canDecode", canDecode(file.transferSyntax())}});
    // A video's frames come from one stream, which the reference decoders
    // don't read; its first is enough to say it decodes.
    if (isVideo(file.transferSyntax())) {
        print(describe(decodeVideo(file, 1).value(0), 0, rawFolder));
        return 0;
    }
    const QList<QByteArray> split = readFrames(file);
    for (int index = 0; index < format.frames; ++index)
        print(describe(decodeFrame(file, index, file.encapsulated() ? &split : nullptr), index, rawFolder));
    return 0;
}
