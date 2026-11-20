// Pictures that decoded wrongly: a test for each difference the comparison
// with a reference decoder (bin/conformance) found in Dicolens, built from
// the tests' own files rather than the ones that showed it.

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "dicom.h"
#include "pixels.h"

#include "support.h"

namespace {

const QString ExplicitBE = "1.2.840.10008.1.2.2", JpegBaseline = "1.2.840.10008.1.2.4.50", JpegLs = "1.2.840.10008.1.2.4.80";

// What ffmpeg makes of raw pixels, or nothing when it isn't there.
QByteArray ffmpegEncode(const QByteArray &pixels, const QString &pixelFormat, int width, int height, const QStringList &codec) {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
        return {};
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", QStringList{"-v", "error", "-f", "rawvideo", "-pix_fmt", pixelFormat, "-s",
                                       QStringLiteral("%1x%2").arg(width).arg(height), "-i", "-"} + codec
                               + QStringList{"-f", "image2pipe", "-"});
    ffmpeg.write(pixels);
    ffmpeg.closeWriteChannel();
    ffmpeg.waitForFinished();
    return ffmpeg.exitCode() == 0 ? ffmpeg.readAllStandardOutput() : QByteArray();
}

bool near(QRgb pixel, int red, int green, int blue, int within) {
    return std::abs(qRed(pixel) - red) <= within && std::abs(qGreen(pixel) - green) <= within && std::abs(qBlue(pixel) - blue) <= within;
}

}

class DecodeTests : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    int written = 0;

    DicomFile onDisk(const QByteArray &contents) {
        const QString path = dir.filePath(QString::number(++written));
        if (!write(path, contents))
            return {};
        return DicomFile::read(path);
    }

private slots:
    void swapsEightBitPixelsInBigEndianWords() {
        // 8-bit pixels written as 16-bit words (OW): a big-endian file
        // swaps each word's bytes, so the pixels lie in pairs the wrong way
        // round. They were read as they lay.
        Writer words;
        words.bigEndian = true;
        words.image(2, 3, 8, 8, false).pixels(QByteArray("\x01\x00\x03\x02\x05\x04", 6), "OW");
        const Frame swapped = decodeFrame(onDisk(words.file(ExplicitBE)), 0);
        QVERIFY2(swapped.error.isEmpty(), qPrintable(swapped.error));
        for (int i = 0; i < 6; ++i)
            QCOMPARE(swapped.stored(i % 3, i / 3), i);

        // Written as bytes (OB), they are in order.
        Writer bytes;
        bytes.bigEndian = true;
        bytes.image(2, 3, 8, 8, false).pixels(QByteArray("\x00\x01\x02\x03\x04\x05", 6), "OB");
        const Frame plain = decodeFrame(onDisk(bytes.file(ExplicitBE)), 0);
        for (int i = 0; i < 6; ++i)
            QCOMPARE(plain.stored(i % 3, i / 3), i);

        // Frames of an odd size start in the middle of a word.
        Writer frames;
        frames.bigEndian = true;
        frames.image(1, 3, 8, 8, false).add(Tag::NumberOfFrames, "IS", "2").pixels(QByteArray("\x01\x00\x03\x02\x05\x04", 6), "OW");
        const DicomFile two = onDisk(frames.file(ExplicitBE));
        for (int frame = 0; frame < 2; ++frame)
            for (int x = 0; x < 3; ++x)
                QCOMPARE(decodeFrame(two, frame).stored(x, 0), frame * 3 + x);

        // And colour, three bytes a pixel: nine bytes and one of padding.
        Writer colour;
        colour.bigEndian = true;
        colour.image(1, 3, 8, 8, false, "RGB").pixels(QByteArray("\x14\x0A\x28\x1E\x3C\x32\x50\x46\x00\x5A", 10), "OW");
        const Frame rgb = decodeFrame(onDisk(colour.file(ExplicitBE)), 0);
        QVERIFY2(rgb.error.isEmpty(), qPrintable(rgb.error));
        QCOMPARE(rgb.color.pixel(0, 0), qRgb(10, 20, 30));
        QCOMPARE(rgb.color.pixel(2, 0), qRgb(70, 80, 90));
    }

    void keepsTheColoursOfAJpegThatStoresRgb() {
        // A JPEG's three components are Y, Cb and Cr unless something says
        // otherwise. When it's the file that says so (its picture is RGB),
        // ffmpeg doesn't know, and turned red, green and blue into other
        // colours as if they had been YCbCr.
        QByteArray planes;
        for (int value : {200, 50, 120})
            planes += QByteArray(16 * 16, char(value));
        const QByteArray stream = ffmpegEncode(planes, "yuvj444p", 16, 16, {"-c:v", "mjpeg", "-q:v", "2"});
        if (stream.isEmpty())
            QSKIP("ffmpeg can't encode JPEG here");
        // (As ffmpeg writes it, the stream says nothing of its colours.)
        QVERIFY(!stream.contains("JFIF") && !stream.contains("Adobe"));

        Writer stored;
        stored.image(16, 16, 8, 8, false, "RGB").encapsulated({stream});
        const Frame rgb = decodeFrame(onDisk(stored.file(JpegBaseline)), 0);
        QVERIFY2(rgb.error.isEmpty(), qPrintable(rgb.error + " " + rgb.detail));
        QVERIFY2(near(rgb.color.pixel(8, 8), 200, 50, 120, 4), qPrintable(QString::number(rgb.color.pixel(8, 8), 16)));

        // The same stream in a file that says YCbCr is converted.
        Writer converted;
        converted.image(16, 16, 8, 8, false, "YBR_FULL").encapsulated({stream});
        const Frame ybr = decodeFrame(onDisk(converted.file(JpegBaseline)), 0);
        QVERIFY2(ybr.error.isEmpty(), qPrintable(ybr.error + " " + ybr.detail));
        QVERIFY2(near(ybr.color.pixel(8, 8), 189, 233, 62, 4), qPrintable(QString::number(ybr.color.pixel(8, 8), 16)));

        // A JFIF stream is YCbCr whatever the file says, and so converted.
        QByteArray jfif = stream;
        jfif.insert(2, QByteArray("\xFF\xE0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00", 18));
        Writer mislabelled;
        mislabelled.image(16, 16, 8, 8, false, "RGB").encapsulated({jfif});
        const Frame trusted = decodeFrame(onDisk(mislabelled.file(JpegBaseline)), 0);
        QVERIFY2(trusted.error.isEmpty(), qPrintable(trusted.error + " " + trusted.detail));
        QVERIFY2(near(trusted.color.pixel(8, 8), 189, 233, 62, 4), qPrintable(QString::number(trusted.color.pixel(8, 8), 16)));
    }

    void holdsLossyValuesAtTheTopOfTheirRange() {
        // Ten bits of X-ray in a deeper lossy stream: a white of 1023 comes
        // back as 1024 here and there, and the bits above the ten stored
        // were masked off, leaving 0: black specks in the white.
        QList<int> values;
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                values << (((x / 4) + (y / 4)) % 2 ? 1023 : 0);
        const QByteArray stream = encode(values, 32, "gray16le", {"-c:v", "jpeg2000", "-q:v", "24", "-f", "image2pipe"});
        if (stream.isEmpty())
            QSKIP("ffmpeg can't encode JPEG 2000 here");
        auto decoded = [&](int stored) {
            Writer w;
            w.image(32, 32, 16, stored, false).encapsulated({stream});
            return decodeFrame(onDisk(w.file("1.2.840.10008.1.2.4.91")), 0);
        };
        // Read as 16 bits, the stream shows what the decoder made of it.
        const Frame raw = decoded(16);
        QVERIFY2(raw.error.isEmpty(), qPrintable(raw.error + " " + raw.detail));
        int most = 0;
        for (quint16 v : raw.mono)
            most = std::max<int>(most, v);
        if (most <= 1023)
            QSKIP("This ffmpeg's encoder didn't overshoot");
        const Frame frame = decoded(10);
        for (int i = 0; i < values.size(); ++i)
            if (raw.mono[i] > 1023)
                QCOMPARE(frame.stored(i % 32, i / 32), 1023);
            else
                QCOMPARE(frame.stored(i % 32, i / 32), int(raw.mono[i]));
    }

    void saysWhenFfmpegHasNoDecoderForAStream() {
        // ffmpeg has no decoder for JPEG-LS with its colours interleaved
        // sample by sample; it said so quietly and handed back a black
        // picture, which was shown as the file's.
        QByteArray pixels;
        for (int i = 0; i < 8 * 8; ++i)
            pixels += QByteArray("\xC8\x32\x78", 3);
        const QByteArray stream = ffmpegEncode(pixels, "rgb24", 8, 8, {"-c:v", "jpegls"});
        if (stream.isEmpty())
            QSKIP("ffmpeg can't encode JPEG-LS here");
        Writer w;
        w.image(8, 8, 8, 8, false, "RGB").encapsulated({stream});
        const Frame whole = decodeFrame(onDisk(w.file(JpegLs)), 0);
        QVERIFY2(whole.error.isEmpty(), qPrintable(whole.error + " " + whole.detail));
        QCOMPARE(whole.color.pixel(3, 3), qRgb(200, 50, 120));

        // The same scan, said to be interleaved by sample: the byte after
        // its components' and the one for how lossy it is.
        const qsizetype scan = stream.indexOf(QByteArray("\xFF\xDA", 2));
        QVERIFY(scan > 0);
        QByteArray bySample = stream;
        const int components = uchar(stream[scan + 4]);
        bySample[scan + 5 + 2 * components + 1] = 2;
        Writer patched;
        patched.image(8, 8, 8, 8, false, "RGB").encapsulated({bySample});
        const Frame frame = decodeFrame(onDisk(patched.file(JpegLs)), 0);
        QCOMPARE(frame.error, QString("ffmpeg couldn't decode this image"));
    }
};

QTEST_GUILESS_MAIN(DecodeTests)
#include "decode_tests.moc"
