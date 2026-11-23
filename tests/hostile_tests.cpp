// What damaged and hostile files do to the decoders: a test for each bug
// that fuzzing (bin/fuzz), the sanitizers (bin/sanitize), or reading the
// decoders for the same mistakes found, built from the smallest file that
// shows it. Several only fail under the sanitizers, where a read past the
// end or a number out of range stops the test; bin/sanitize runs them so.

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest>
#include <climits>
#include <cmath>

#include "dicom.h"
#include "ljpeg.h"
#include "marks.h"
#include "pixels.h"
#include "reformat.h"
#include "reports.h"
#include "study.h"
#include "volumerender.h"

#include "support.h"

namespace {

const QString Rle = "1.2.840.10008.1.2.5", JpegLossless = "1.2.840.10008.1.2.4.70";
const char *const CutShort = "The image data is cut short";

// The layout of a grey picture, as ImageFormat::from would read it.
ImageFormat grey(int rows, int columns, int bits) {
    ImageFormat f;
    f.rows = rows;
    f.columns = columns;
    f.bitsAllocated = f.bitsStored = bits;
    f.highBit = bits - 1;
    return f;
}

QByteArray counting(int count) {
    QByteArray out;
    for (int i = 0; i < count; ++i)
        out += char(i);
    return out;
}

}

class HostileTests : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    int written = 0;

    // Pixel data is read from the file on disk, so a file to decode is one.
    DicomFile onDisk(const QByteArray &contents) {
        const QString path = dir.filePath(QString::number(++written));
        if (!write(path, contents))
            return {};
        return DicomFile::read(path);
    }

private slots:
    void pinsNumbersAnIntCannotHold() {
        // A number is whatever the file spells. One past an int's range is
        // pinned to the nearest an int holds, and one that isn't a number
        // is as good as missing.
        Writer w;
        w.add(Tag::Rows, "DS", "1e30").add(Tag::Columns, "DS", "-1e30").add(Tag::InstanceNumber, "DS", "nan")
            .fd(Tag::SliceThickness, 3758160000.0);
        const DicomFile file = DicomFile::parse(w.file(ExplicitLE));
        QVERIFY(file.isValid());
        QCOMPARE(file.integer(Tag::Rows), INT_MAX);
        QCOMPARE(file.integer(Tag::Columns), INT_MIN);
        QCOMPARE(file.integer(Tag::InstanceNumber, 7), 7);
        QCOMPARE(file.integer(Tag::SliceThickness), INT_MAX);
        QCOMPARE(toInt(-0.9), 0);
        QCOMPARE(toInt(std::nan(""), -1), -1);
    }

    void takesNoMoreFramesThanTheDataHolds() {
        // Number of Frames is the file's word. A slice is listed for each,
        // so two thousand million of them would be listed for ever; only
        // those the pixel data has room for are.
        Writer w;
        w.image(2, 2, 16, 12, false).add(Tag::NumberOfFrames, "IS", "2000000000").pixels(le16({1, 2, 3, 4, 5, 6, 7, 8}));
        const DicomFile file = onDisk(w.file(ExplicitLE));
        QCOMPARE(ImageFormat::from(file).frames, 2);
        Scan scan;
        addFile(scan, file);
        QCOMPARE(scan.series.size(), 1);
        QCOMPARE(scan.series.first().slices.size(), 2);
        QCOMPARE(decodeFrame(file, 1).stored(1, 1), 8);
        QCOMPARE(decodeFrame(file, 2).error, QString(CutShort));

        // Bits packed eight to a byte: 32 of them hold three frames of 3 by 3.
        Writer bits;
        bits.image(3, 3, 1, 1, false).add(Tag::NumberOfFrames, "IS", "999999").pixels(QByteArray("\xFF\x01\xF0\x00", 4), "OB");
        QCOMPARE(ImageFormat::from(onDisk(bits.file(ExplicitLE))).frames, 3);

        // Compressed frames are an item each, 8 bytes of header at least.
        Writer packed;
        packed.image(2, 2, 8, 8, false).add(Tag::NumberOfFrames, "IS", "2000000000")
            .encapsulated({rleFrame({counting(4)}), rleFrame({counting(4)})});
        const DicomFile compressed = onDisk(packed.file(Rle));
        QVERIFY(ImageFormat::from(compressed).frames <= compressed.pixelLength() / 8);
    }

    void measuresNoFrameOfSizesPastTheStandards() {
        // Samples and bits multiply into a frame's size. Past what the
        // standard has (four samples, 64 bits, 16-bit rows and columns) the
        // product overflowed; now there is no such picture.
        Writer w;
        w.us(Tag::SamplesPerPixel, 65535).us(Tag::Rows, 2).us(Tag::Columns, 2).us(Tag::BitsAllocated, 65528)
            .pixels(QByteArray(64, 1), "OB");
        QCOMPARE(decodeFrame(onDisk(w.file(ExplicitLE)), 0).error, QString("This image's pixel format isn't supported"));
        Writer wide;
        wide.add(Tag::Rows, "UL", QByteArray("\xFF\xFF\xFF\x7F", 4)).add(Tag::Columns, "UL", QByteArray("\xFF\xFF\xFF\x7F", 4))
            .us(Tag::BitsAllocated, 16).pixels(QByteArray(64, 1));
        QCOMPARE(decodeFrame(onDisk(wide.file(ExplicitLE)), 0).error, QString("This image's pixel format isn't supported"));
        // One bit a pixel, 65535 pixels a side, of two bytes: nothing is
        // set aside for the 536 MB it says it has.
        Writer bits;
        bits.image(65535, 65535, 1, 1, false).pixels(QByteArray(2, 1), "OB");
        QCOMPARE(decodeFrame(onDisk(bits.file(ExplicitLE)), 0).error, QString(CutShort));
    }

    void turnsAwayPixelsTheDataDoesNotHave() {
        // convertNative takes a header's word for the size. A colour
        // picture was set aside in full (gigabytes of it) before the data
        // was found too short, and sizes of 32 bits overflowed.
        const QByteArray data = counting(48);
        const auto *bytes = reinterpret_cast<const uchar *>(data.constData());
        ImageFormat colour = grey(134217728, 4, 8);
        colour.samples = 3;
        colour.photometric = "RGB";
        QCOMPARE(convertNative(bytes, data.size(), colour).error, QString(CutShort));
        ImageFormat huge = grey(INT_MAX, INT_MAX, 32);
        QCOMPARE(convertNative(bytes, data.size(), huge).error, QString(CutShort));
        QCOMPARE(convertNative(bytes, data.size(), grey(-1, -1, 8)).error, QString("This file has no image size"));
        // A high bit of any size shifts by no more than the sample has.
        ImageFormat shifted = grey(2, 2, 16);
        shifted.highBit = INT_MAX;
        QVERIFY(convertNative(bytes, data.size(), shifted).error.isEmpty());
    }

    void readsAnOddRowOfSharedColour() {
        // YBR_FULL_422 keeps two pixels in four bytes, Y1 Y2 Cb Cr. A row
        // of three ends in a group with one pixel in it, which was read two
        // bytes past the data's end.
        ImageFormat f = grey(1, 3, 8);
        f.samples = 3;
        f.photometric = "YBR_FULL_422";
        const QByteArray data("\x10\x20\x80\x80\x30\x00\x80\x80", 8);
        const auto *bytes = reinterpret_cast<const uchar *>(data.constData());
        QCOMPARE(convertNative(bytes, 6, f).error, QString(CutShort));
        const Frame frame = convertNative(bytes, 8, f);
        QVERIFY2(frame.error.isEmpty(), qPrintable(frame.error));
        QCOMPARE(qRed(frame.color.pixel(0, 0)), 0x10);
        QCOMPARE(qRed(frame.color.pixel(1, 0)), 0x20);
        QCOMPARE(qRed(frame.color.pixel(2, 0)), 0x30);
        QCOMPARE(f.frameBytes(), 8);
    }

    void failsOnUncompressedDataCutShort() {
        // A file that ends inside its pixels has no picture to show, in any
        // layout: grey, colour, or a bit a pixel.
        Writer greyFile, colourFile, bitsFile;
        greyFile.image(4, 4, 16, 12, false).pixels(QByteArray(30, 1));
        colourFile.image(2, 4, 8, 8, false, "RGB").pixels(QByteArray(22, 1), "OB");
        bitsFile.image(5, 5, 1, 1, false).pixels(QByteArray(2, 1), "OB");
        for (const Writer *w : {&greyFile, &colourFile, &bitsFile})
            QCOMPARE(decodeFrame(onDisk(w->file(ExplicitLE)), 0).error, QString(CutShort));
        // The element says 32 bytes and the file stops after 20 of them.
        Writer whole;
        whole.image(4, 4, 16, 12, false).pixels(QByteArray(32, 1));
        const QByteArray file = whole.file(ExplicitLE);
        QVERIFY(decodeFrame(onDisk(file), 0).error.isEmpty());
        QCOMPARE(decodeFrame(onDisk(file.left(file.size() - 12)), 0).error, QString(CutShort));
    }

    void failsOnRleCutShort() {
        // A segment that ends before its plane is full used to leave the
        // rest black, as if the picture were whole.
        const ImageFormat f = grey(4, 4, 8);
        const QByteArray whole = rleFrame({counting(16)});
        QString error;
        QCOMPARE(decodeRle(whole, f, &error), counting(16));
        for (qsizetype length = 64; length < whole.size() - 1; ++length) {
            error.clear();
            QVERIFY2(decodeRle(whole.left(length), f, &error).isEmpty(), qPrintable(QString::number(length)));
            QCOMPARE(error, QString(CutShort));
        }
        // The second of two planes missing, and a segment that starts past
        // the end of the data.
        const ImageFormat deep = grey(4, 4, 16);
        QByteArray two = rleFrame({counting(16), counting(16)});
        QVERIFY(!decodeRle(two, deep, &error).isEmpty());
        qToLittleEndian<quint32>(quint32(two.size()), two.data() + 8);
        QVERIFY(decodeRle(two, deep, &error).isEmpty());
        QCOMPARE(error, QString(CutShort));
        qToLittleEndian<quint32>(0xFFFFFFF0u, two.data() + 8);
        QVERIFY(decodeRle(two, deep, &error).isEmpty());
        // Bytes after a plane is full (padding, or a decoder's slack) are
        // not an error.
        QCOMPARE(decodeRle(whole + QByteArray(6, 0), f, &error), counting(16));
    }

    void sizesAnRlePictureByItsData() {
        // The number of segments is samples times bytes, which overflowed
        // an int; and the planes were set aside for whatever size the
        // header said, 4 GB for a file of a hundred bytes.
        QByteArray data = rleFrame({counting(6)});
        QString error;
        ImageFormat many = grey(2, 3, 8);
        many.samples = 65536;
        many.bitsAllocated = 65536 * 8;
        QVERIFY(decodeRle(data, many, &error).isEmpty());
        QCOMPARE(error, QString("This RLE image is damaged"));
        // A run is 128 bytes out of two at most, so 100 bytes hold no more
        // than 6400 pixels.
        QVERIFY(decodeRle(data, grey(65535, 65535, 8), &error).isEmpty());
        QCOMPARE(error, QString(CutShort));
        QVERIFY(decodeRle(data, grey(0, 5, 8), &error).isEmpty());
        QVERIFY(decodeRle(data, grey(-3, -2, 8), &error).isEmpty());
    }

    void failsOnLosslessJpegCutShort() {
        // Past the end of its data the decoder read zeros, which decode to
        // something: a picture cut short came out whole, its tail a smear.
        const QVector<quint16> samples{100, 2000, 30000, 4, 65535, 600, 7, 8000, 900, 10, 11000, 12,
                                       1300, 14, 15000, 16, 170, 18000, 19, 20};
        for (int restart : {0, 5, 10}) {
            const QByteArray whole = losslessJpeg(samples, 5, 4, 1, 16, 1, restart);
            LosslessJpeg out;
            QString error;
            QVERIFY2(decodeLosslessJpeg(whole, out, &error), qPrintable(error));
            QCOMPARE(out.samples, samples);
            // Without its end-of-image marker it is still all there.
            QVERIFY(decodeLosslessJpeg(whole.left(whole.size() - 2), out, &error));
            QCOMPARE(out.samples, samples);
            // Cut anywhere in its data, it isn't.
            const qsizetype data = whole.indexOf(QByteArray("\xFF\xDA", 2)) + 10;
            for (qsizetype length = data; length < whole.size() - 2; ++length) {
                error.clear();
                QVERIFY2(!decodeLosslessJpeg(whole.left(length), out, &error), qPrintable(QString::number(length)));
                QCOMPARE(error, QString(CutShort));
            }
            // Nor with a stretch between two restart markers missing, or
            // an end-of-image where more data should be.
            if (restart) {
                const qsizetype first = whole.indexOf(QByteArray("\xFF\xD0", 2), data);
                QVERIFY(first > 0);
                QVERIFY(!decodeLosslessJpeg(whole.left(data + 2) + whole.mid(first), out, &error));
                QCOMPARE(error, QString(CutShort));
                QVERIFY(!decodeLosslessJpeg(whole.left(first) + QByteArray("\xFF\xD9", 2), out, &error));
                QCOMPARE(error, QString(CutShort));
            }
        }
        // In a file, the frame says so rather than showing.
        Writer w;
        const QByteArray whole = losslessJpeg(samples, 5, 4, 1, 16, 1, 0);
        w.image(4, 5, 16, 16, false).encapsulated({whole.left(whole.size() - 12)});
        QCOMPARE(decodeFrame(onDisk(w.file(JpegLossless)), 0).error, QString(CutShort));
    }

    void showsNoLosslessJpegOfTwoSamples() {
        // Colour was read as three samples a pixel whatever the stream had:
        // with two, the last pixel's third was past the end of them.
        const QByteArray stream = losslessJpeg(QVector<quint16>(8, 9), 2, 2, 2, 8, 1, 0);
        Writer w;
        w.us(Tag::SamplesPerPixel, 2).us(Tag::Rows, 2).us(Tag::Columns, 2).us(Tag::BitsAllocated, 8).us(Tag::BitsStored, 8)
            .encapsulated({stream});
        QCOMPARE(decodeFrame(onDisk(w.file(JpegLossless)), 0).error, QString("This JPEG image's layout isn't supported"));
    }

    void splitsFramesNoBiggerThanTheirData() {
        // An offset table whose offsets go back and forth handed every
        // fragment to frame after frame: thousands of copies of the data.
        // Offsets only grow; a table where they don't is passed over.
        QByteArray table;
        for (int i = 0; i < 6; ++i) {
            char b[4];
            qToLittleEndian<quint32>(i % 2 ? 0x7FFFFFFF : 0, b);
            table.append(b, 4);
        }
        Writer w;
        w.encapsulated({"AAAA", "BB", "CCCCCC", "DD", "EE", "FF"}, table);
        const QByteArray items = w.body.mid(12);
        qsizetype total = 0;
        const QList<QByteArray> frames = DicomFile::splitFrames(items, 6);
        for (const QByteArray &frame : frames)
            total += frame.size();
        QVERIFY2(total <= items.size(), qPrintable(QString::number(total)));
        QCOMPARE(frames, (QList<QByteArray>{"AAAA", "BB", "CCCCCC", "DD", "EE", "FF"}));
        // A table that grows still joins a frame's fragments.
        Writer joined;
        joined.encapsulated({"AAAA", "BB", "CCCCCC"}, QByteArray("\x00\x00\x00\x00\x16\x00\x00\x00", 8));
        QCOMPARE(DicomFile::splitFrames(joined.body.mid(12), 2), (QList<QByteArray>{"AAAABB", "CCCCCC"}));
        // And a count no table could be long enough for is no trouble.
        QCOMPARE(DicomFile::splitFrames(items, INT_MAX), (QList<QByteArray>{"AAAABBCCCCCCDDEEFF"}));
    }

    void survivesAPaletteThatLies() {
        // A palette's descriptor (how many entries, the first value mapped,
        // their bits) is three numbers the file gives; any of them may be
        // past an int, or no number.
        for (const char *descriptor : {"1e30\\-1e30\\16", "nan\\nan\\nan", "4\\2147483647\\8", "-5\\-2147483648\\1e30"}) {
            Writer w;
            w.us(Tag::SamplesPerPixel, 1).add(Tag::Photometric, "CS", "PALETTE COLOR").us(Tag::Rows, 2).us(Tag::Columns, 2)
                .us(Tag::BitsAllocated, 16).us(Tag::BitsStored, 16).us(Tag::HighBit, 15).us(Tag::PixelRepresentation, 1);
            for (quint32 c = 0; c < 3; ++c)
                w.add(Tag::RedPaletteDescriptor + c, "DS", descriptor)
                    .add(Tag::RedPaletteData + c, "OW", le16({0, 0x4000, 0x8000, 0xFFFF}));
            w.pixels(le16({0, 1, 0x8000, 0xFFFF}));
            const Frame frame = decodeFrame(onDisk(w.file(ExplicitLE)), 0);
            QVERIFY2(frame.error.isEmpty(), qPrintable(frame.error));
            QCOMPARE(frame.color.size(), QSize(2, 2));
        }
    }

    void keepsItsSumsWhenTheBitsAreNoNumberOfBits() {
        // Bits Stored is the file's word, and as a decimal string it can be
        // the least number an int holds: taking one from it for the high
        // bit, when the file gives none, overflowed. The fuzzer found it.
        for (const char *bits : {"-1e30", "-2147483648", "1e30", "nan"}) {
            Writer w;
            w.us(Tag::SamplesPerPixel, 1).add(Tag::Photometric, "CS", "MONOCHROME2").us(Tag::Rows, 2).us(Tag::Columns, 2)
                .add(Tag::BitsAllocated, "DS", bits).add(Tag::BitsStored, "DS", bits).us(Tag::PixelRepresentation, 0);
            w.pixels(le16({0, 1, 2, 3}));
            const DicomFile file = onDisk(w.file(ExplicitLE));
            const ImageFormat format = ImageFormat::from(file);
            QVERIFY(format.highBit >= -1 && format.highBit <= 65536);
            // Whether it's shown or refused, it's one or the other.
            const Frame frame = decodeFrame(file, 0);
            QVERIFY(!frame.error.isEmpty() || frame.width == 2);
        }
    }

    void keepsWindowsAndRescalesNumbers() {
        // "nan" is a decimal string too. A window or a rescale made of it
        // turned every grey level into one that isn't a number.
        Writer lut;
        lut.add(Tag::LutDescriptor, "SS", QByteArray("\x00\x01\xFB\xFF\x0C\x00", 6));
        for (const char *value : {"nan", "inf", "-inf"}) {
            Writer w;
            w.image(2, 2, 16, 16, true).add(Tag::WindowCenter, "DS", value).add(Tag::WindowWidth, "DS", value)
                .add(Tag::RescaleSlope, "DS", value).add(Tag::RescaleIntercept, "DS", value)
                .sequence(Tag::VoiLutSequence, {lut}).pixels(le16({0, 1, 0x8000, 0xFFFF}));
            const DicomFile file = onDisk(w.file(ExplicitLE));
            const ImageFormat format = ImageFormat::from(file);
            QCOMPARE(format.slope, 1.0);
            QCOMPARE(format.intercept, 0.0);
            const Frame frame = decodeFrame(file, 0);
            QVERIFY2(frame.error.isEmpty(), qPrintable(frame.error));
            QVERIFY(std::isfinite(frame.windowCenter) && std::isfinite(frame.windowWidth));
            // The lookup table's range stands in: 256 values from -5.
            QCOMPARE(frame.windowCenter, 122.5);
            QCOMPARE(frame.windowWidth, 256.0);
            QCOMPARE(render(frame, frame.windowCenter, frame.windowWidth, false).size(), QSize(2, 2));
        }
        // Drawn through a window that isn't a number, a picture is still one.
        Frame frame;
        frame.width = frame.height = 1;
        frame.mono = {7};
        QCOMPARE(render(frame, std::nan(""), std::nan(""), false).size(), QSize(1, 1));
        // A rescale that is a number, however large, is kept.
        Writer steep;
        steep.image(1, 1, 16, 16, false).add(Tag::RescaleSlope, "DS", "1e200").add(Tag::RescaleIntercept, "DS", "-1e200")
            .pixels(le16({3}));
        QCOMPARE(ImageFormat::from(onDisk(steep.file(ExplicitLE))).slope, 1e200);
    }

    void survivesOverlaysAndMarksThatLie() {
        // Where an overlay starts, and which frames a mark is on, are
        // numbers from the file: added to a row, or turned into an int,
        // they overflowed.
        for (const char *origin : {"2147483647\\2147483647", "-2147483648\\-2147483648", "1e30\\nan"}) {
            Writer w;
            w.image(4, 4, 8, 8, false).us(0x60000010, 4).us(0x60000011, 4).add(0x60000050, "DS", origin).us(0x60000100, 1)
                .add(0x60003000, "OW", QByteArray(2, char(0xFF))).pixels(QByteArray(16, 0), "OB");
            const DicomFile file = DicomFile::parse(w.file(ExplicitLE));
            QVERIFY(file.isValid());
            const QImage mask = Marks::overlay(file, 0, 4, 4);
            QCOMPARE(mask.size(), QSize(4, 4));
            QCOMPARE(mask.pixelColor(0, 0).alpha(), 0);
            // A frame before the first, and one far past the last.
            (void)Marks::overlay(file, INT_MIN, 4, 4);
            (void)Marks::overlay(file, INT_MAX, 4, 4);
        }
        Writer reference, series, line, annotation, state;
        reference.add(Tag::ReferencedSopInstance, "UI", "1.2.5.2").add(Tag::ReferencedFrameNumber, "DS", "1e30\\nan\\-1e30\\2");
        series.sequence(Tag::ReferencedImage, {reference});
        line.floats(Tag::GraphicData, {1, 1, 8, 8}).add(Tag::GraphicType, "CS", "POLYLINE");
        annotation.sequence(Tag::GraphicObjects, {line});
        state.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.11.1").sequence(Tag::ReferencedSeries, {series})
            .sequence(Tag::GraphicAnnotations, {annotation});
        const QList<Mark> marks = Marks::read(DicomFile::parse(state.file(ExplicitLE)));
        QCOMPARE(marks.size(), 1);
        QCOMPARE(marks.first().images.size(), 4);
        QVERIFY(marks.first().appliesTo("1.2.5.2", 1));
        QVERIFY(!marks.first().appliesTo("1.2.5.2", 2));
    }

    void placesNothingByNumbersNoScanHas() {
        // Positions and spacings are millimetres around a patient, and an
        // orientation is two unit directions at right angles. Numbers that
        // are neither place nothing: the series opens as a plain stack.
        auto stack = [&](const QByteArray &orientation, const QByteArray &spacing, const QByteArray &x) {
            Scan scan;
            for (int k = 0; k < 4; ++k) {
                Writer w;
                w.add(Tag::Modality, "CS", "CT").image(2, 2, 16, 12, false)
                    .add(Tag::InstanceNumber, "IS", QByteArray::number(k + 1))
                    .add(Tag::ImagePosition, "DS", x + "\\0\\" + QByteArray::number(k))
                    .add(Tag::ImageOrientation, "DS", orientation).add(Tag::PixelSpacing, "DS", spacing)
                    .pixels(le16({1, 2, 3, 4}));
                addFile(scan, onDisk(w.file(ExplicitLE)));
            }
            sortScan(scan);
            return scan.series.value(0);
        };
        const QByteArray axial = "1\\0\\0\\0\\1\\0";
        QVERIFY(makeVolume(stack(axial, "1\\1", "0")).valid);
        // Rounded as scanners round them, directions still count.
        QVERIFY(makeVolume(stack("0.707\\0.707\\0\\-0.707\\0.707\\0", "1\\1", "0")).valid);
        QVERIFY(!stack("1e150\\0\\0\\0\\1e150\\0", "1\\1", "0").hasOrientation);
        QVERIFY(!stack("1\\0\\0\\1\\0\\0", "1\\1", "0").hasOrientation);
        QVERIFY(!stack("0\\0\\0\\0\\0\\0", "1\\1", "0").hasOrientation);
        QVERIFY(!stack(axial, "1e-300\\1", "0").hasSpacing);
        QVERIFY(!stack(axial, "1\\1e300", "0").hasSpacing);
        const Series far = stack(axial, "1\\1", "1e300");
        QVERIFY(!far.slices.first().hasPosition);
        QVERIFY(!makeVolume(far).valid);
    }

    void scalesNoPetByAFactorThatIsNotOne() {
        // The SUV factor is weight over dose, both the file's; it scales
        // every value of the volume, which a factor of infinity turned
        // into values that are no numbers.
        auto pet = [&](const QByteArray &weight, const QByteArray &dose, const QByteArray &correction) {
            Writer drug, w;
            drug.add(0x00181072, "TM", "090000").add(0x00181074, "DS", dose).add(0x00181075, "DS", "6586.2");
            w.add(Tag::Modality, "CS", "PT").add(0x00080031, "TM", "100000").add(0x00101030, "DS", weight)
                .add(0x00541001, "CS", "BQML").add(0x00541102, "CS", correction).sequence(0x00540016, {drug});
            return suvFactor(DicomFile::parse(w.file(ExplicitLE)));
        };
        QVERIFY(pet("70", "370000000", "START") > 0);
        for (const char *correction : {"START", "ADMIN"}) {
            QCOMPARE(pet("1e300", "1e-300", correction), 0.0);
            QCOMPARE(pet("nan", "370000000", correction), 0.0);
            QCOMPARE(pet("70", "nan", correction), 0.0);
        }
        // And a volume isn't packed by a scale that takes it out of range.
        Volume volume;
        volume.valid = true;
        volume.size[0] = volume.size[1] = 2;
        volume.size[2] = 3;
        QVector<Frame> slices(3);
        for (Frame &f : slices) {
            f.width = f.height = 2;
            f.mono = {1, 2, 3, 4};
        }
        QVERIFY(packVolume(volume, slices).valid);
        QVERIFY(!packVolume(volume, slices, INFINITY).valid);
        QVERIFY(!packVolume(volume, slices, 1e308).valid);
        for (Frame &f : slices)
            f.intercept = 1e300;
        QVERIFY(packVolume(volume, slices).valid);
    }

    void drawsAStackOfFarApartSlicesInTime() {
        // A ray was sampled at the pace of the finest voxel. Slices a
        // metre apart with pixels a micrometre across made that thousands
        // of millions of samples a ray; now it is two to each voxel crossed.
        Volume volume;
        volume.valid = true;
        volume.size[0] = volume.size[1] = 8;
        volume.size[2] = 4;
        volume.spacing[0] = volume.spacing[1] = 1e-6;
        volume.spacing[2] = 1000;
        QVector<Frame> slices(4);
        for (int k = 0; k < 4; ++k) {
            slices[k].width = slices[k].height = 8;
            slices[k].mono = QVector<quint16>(64, quint16(1000 * k));
        }
        const VoxelGrid grid = packVolume(volume, slices);
        QVERIFY(grid.valid);
        View3D view;
        QElapsedTimer timer;
        timer.start();
        for (Look look : {Look::Mip, Look::Surface}) {
            view.look = look;
            view.width = 3000;
            view.center = 1500;
            QCOMPARE(renderVolume(grid, view, QSize(8, 8)).size(), QSize(8, 8));
        }
        double point[3];
        (void)pickVolume(grid, view, QSize(8, 8), QPointF(4, 4), point);
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
        // An ordinary volume is sampled as finely as before: a bright slab
        // one voxel thick still shows in a MIP from the side.
        Volume plain = volume;
        plain.spacing[0] = plain.spacing[1] = plain.spacing[2] = 1;
        view.look = Look::Mip;
        const QImage mip = renderVolume(packVolume(plain, slices), view, QSize(16, 16));
        QVERIFY(qGray(mip.pixel(8, 8)) > 0);
    }
};

QTEST_GUILESS_MAIN(HostileTests)
#include "hostile_tests.moc"
