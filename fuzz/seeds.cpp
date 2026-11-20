// Writes each fuzz target's seed corpus: small files of every kind the
// decoders read, made with the tests' own DICOM writer, so no sample files
// are kept in the repository. Usage: dicolens_fuzz_seeds <folder>, which
// gets a folder of seeds for each target.

#include <QDir>
#include <QTemporaryDir>

#include "harness.h"
#include "support.h"

namespace {

const QString ImplicitLE = "1.2.840.10008.1.2", ExplicitBE = "1.2.840.10008.1.2.2", Deflated = "1.2.840.10008.1.2.1.99",
              Rle = "1.2.840.10008.1.2.5", JpegLossless = "1.2.840.10008.1.2.4.70", JpegBaseline = "1.2.840.10008.1.2.4.50",
              Jpeg2000 = "1.2.840.10008.1.2.4.90", Mpeg4 = "1.2.840.10008.1.2.4.102";

QString root;

void save(const char *target, const char *name, const QByteArray &bytes) {
    QDir().mkpath(root + "/" + target);
    if (!write(QStringLiteral("%1/%2/seed-%3").arg(root, target, name), bytes))
        qFatal("Couldn't write the seed %s/%s", target, name);
}

QByteArray int32(int v) {
    char b[4];
    qToLittleEndian<qint32>(v, b);
    return QByteArray(b, 4);
}

// The 32 bytes formatFrom() reads a pixel layout out of.
QByteArray layout(int rows, int columns, int samples, int allocated, int stored, bool isSigned = false, int planar = 0,
                  bool littleEndian = true, int photometric = 0) {
    return int32(rows) + int32(columns) + int32(samples) + int32(allocated) + int32(stored) + int32(stored - 1)
           + int32(planar) + int32((isSigned ? 1 : 0) | (littleEndian ? 2 : 0) | photometric << 8);
}

QByteArray ramp(int count, int step = 1) {
    QList<int> values;
    for (int i = 0; i < count; ++i)
        values << (i * step) % 4096;
    return le16(values);
}

QByteArray bytes(int count, int step = 7) {
    QByteArray out;
    for (int i = 0; i < count; ++i)
        out += char(i * step);
    return out;
}

QVector<quint16> samples(int count, int mask) {
    QVector<quint16> out;
    for (int i = 0; i < count; ++i)
        out << quint16((i * 37 + (i % 5) * 300) & mask);
    return out;
}

// The planes of an RLE frame from samples as convertNative takes them:
// little-endian, a pixel after another.
QList<QByteArray> planesOf(const QByteArray &pixels, int samplesPerPixel, int bytesPerSample) {
    QList<QByteArray> planes;
    const int stride = samplesPerPixel * bytesPerSample;
    for (int s = 0; s < samplesPerPixel; ++s)
        for (int b = bytesPerSample - 1; b >= 0; --b) {
            QByteArray plane;
            for (qsizetype i = s * bytesPerSample + b; i < pixels.size(); i += stride)
                plane += pixels[i];
            planes << plane;
        }
    return planes;
}

QByteArray deflated(const Writer &body) {
    const QByteArray zlib = qCompress(body.body, 9);
    Writer meta;
    meta.add(Tag::TransferSyntax, "UI", Deflated.toLatin1());
    return QByteArray(128, 0) + "DICM" + meta.body + zlib.mid(6, zlib.size() - 10);
}

Writer patient() {
    Writer w;
    w.add(Tag::SpecificCharacterSet, "CS", "ISO_IR 192")
        .add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.2")
        .add(Tag::SopInstance, "UI", "1.2.3.4.5")
        .add(Tag::StudyDate, "DA", "20240131")
        .add(Tag::StudyTime, "TM", "101530.5")
        .add(Tag::Modality, "CS", "CT")
        .add(Tag::PatientName, "PN", "Doe^Jane^^Dr=\xE5\xB1\xB1\xE7\x94\xB0")
        .add(Tag::PatientId, "LO", "42")
        .add(Tag::StudyInstance, "UI", "1.2.3");
    return w;
}

// A multi-frame file with what varies by frame in functional groups.
Writer enhanced(int frames) {
    QList<Writer> perFrame;
    for (int k = 0; k < frames; ++k) {
        Writer plane, pixelValues, group;
        plane.add(Tag::ImagePosition, "DS", "0\\0\\" + QByteArray::number(k * 2));
        pixelValues.add(Tag::RescaleIntercept, "DS", QByteArray::number(-k)).add(Tag::RescaleSlope, "DS", "2");
        group.sequence(0x00209113, {plane}).sequence(0x00289145, {pixelValues});
        perFrame << group;
    }
    Writer orientation, measures, shared;
    orientation.add(Tag::ImageOrientation, "DS", "1\\0\\0\\0\\1\\0");
    measures.add(Tag::PixelSpacing, "DS", "0.5\\0.5").add(Tag::SliceThickness, "DS", "2");
    shared.sequence(0x00209116, {orientation}).sequence(0x00289110, {measures});
    Writer w = patient();
    w.image(3, 4, 16, 12, true)
        .add(Tag::NumberOfFrames, "IS", QByteArray::number(frames))
        .sequence(Tag::SharedGroups, {shared})
        .sequence(Tag::PerFrameGroups, perFrame)
        .pixels(ramp(12 * frames, 5));
    return w;
}

Writer structuredReport() {
    Writer text, number, measured, container, date, person;
    text.add(Tag::ValueType, "CS", "TEXT").sequence(Tag::ConceptName, {Writer::code("121071", "Finding")})
        .add(Tag::TextValue, "UT", "Nothing to see.");
    measured.add(Tag::NumericValue, "DS", "12.5").sequence(Tag::MeasurementUnits, {Writer::code("mm", "millimeter")});
    number.add(Tag::ValueType, "CS", "NUM").sequence(Tag::ConceptName, {Writer::code("410668003", "Length")})
        .sequence(Tag::MeasuredValue, {measured});
    date.add(Tag::ValueType, "CS", "DATETIME").sequence(Tag::ConceptName, {Writer::code("1", "Seen")})
        .add(Tag::ContentDateTime, "DT", "20240131101530");
    person.add(Tag::ValueType, "CS", "PNAME").sequence(Tag::ConceptName, {Writer::code("2", "Observer")})
        .add(Tag::PersonName, "PN", "Roe^Richard");
    container.add(Tag::ValueType, "CS", "CONTAINER").sequence(Tag::ConceptName, {Writer::code("121070", "Findings")})
        .sequence(Tag::ContentSequence, {text, number, date, person});
    Writer w = patient();
    w.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.88.11")
        .add(Tag::ContentDate, "DA", "20240201")
        .add(Tag::ValueType, "CS", "CONTAINER")
        .sequence(Tag::ConceptName, {Writer::code("18748-4", "Imaging Report")})
        .sequence(Tag::ContentSequence, {container, text});
    return w;
}

Writer presentationState() {
    Writer reference, series, line, circle, note, annotation;
    reference.add(0x00081150, "UI", "1.2.840.10008.5.1.4.1.1.2").add(Tag::ReferencedSopInstance, "UI", "1.2.5.2")
        .add(Tag::ReferencedFrameNumber, "IS", "1\\2");
    series.add(Tag::SeriesInstance, "UI", "1.2.3.4").sequence(Tag::ReferencedImage, {reference});
    line.add(Tag::GraphicUnits, "CS", "PIXEL").floats(Tag::GraphicData, {1, 1, 8, 8})
        .add(Tag::GraphicType, "CS", "POLYLINE").add(Tag::GraphicFilled, "CS", "N");
    circle.add(Tag::GraphicUnits, "CS", "DISPLAY").floats(Tag::GraphicData, {0.5f, 0.5f, 0.7f, 0.5f})
        .add(Tag::GraphicType, "CS", "CIRCLE");
    note.add(Tag::BoundingBoxUnits, "CS", "PIXEL").add(Tag::AnchorPointUnits, "CS", "PIXEL")
        .add(Tag::UnformattedText, "ST", "Nodule").floats(Tag::BoundingBoxTopLeft, {1, 2})
        .floats(Tag::BoundingBoxBottomRight, {6, 4}).floats(Tag::AnchorPoint, {5, 5})
        .add(Tag::AnchorPointVisibility, "CS", "Y");
    annotation.sequence(Tag::ReferencedImage, {reference}).sequence(Tag::TextObjects, {note})
        .sequence(Tag::GraphicObjects, {line, circle});
    Writer w = patient();
    w.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.11.1")
        .sequence(Tag::ReferencedSeries, {series})
        .sequence(Tag::GraphicAnnotations, {annotation});
    return w;
}

Writer overlaid(int frames) {
    Writer w = patient();
    w.image(4, 6, 8, 8, false)
        .add(Tag::NumberOfFrames, "IS", QByteArray::number(frames))
        .us(0x60000010, 3).us(0x60000011, 5).add(0x60000015, "IS", QByteArray::number(frames))
        .add(0x60000050, "SS", QByteArray("\x02\x00\x01\x00", 4)).us(0x60000051, 1).us(0x60000100, 1)
        .add(0x60003000, "OW", bytes((15 * frames + 15) / 16 * 2, 73))
        .us(0x60020010, 4).us(0x60020011, 6).us(0x60020100, 1).add(0x60023000, "OB", bytes(4, 91))
        .pixels(bytes(24 * frames), "OB");
    return w;
}

// The files a folder was given, joined into one input for the scan target.
QByteArray joined(const QString &folder) {
    QByteArray out;
    for (const QString &name : QDir(folder).entryList(QDir::Files, QDir::Name)) {
        QFile file(folder + "/" + name);
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Couldn't read back %s", qPrintable(name));
        if (!out.isEmpty())
            out += NextFile;
        out += file.readAll();
    }
    return out;
}

void files() {
    // Uncompressed, in each encoding and pixel layout.
    Writer grey = patient();
    grey.image(3, 4, 16, 12, false)
        .add(Tag::WindowCenter, "DS", "40\\400").add(Tag::WindowWidth, "DS", "400\\2000")
        .add(Tag::RescaleIntercept, "DS", "-1024").add(Tag::RescaleSlope, "DS", "1")
        .add(Tag::PixelSpacing, "DS", "0.5\\0.5").add(Tag::PixelPaddingValue, "US", QByteArray("\x00\x00", 2))
        .pixels(ramp(12, 300));
    Writer implicit, big;
    implicit.explicitVr = false;
    implicit.add(Tag::PatientName, "PN", "Implicit^Demo").image(2, 2, 16, 16, true).pixels(le16({1, 65535, 3, 4}));
    big.bigEndian = true;
    big.add(Tag::PatientName, "PN", "Big^Endian").image(2, 2, 16, 12, false).pixels(QByteArray("\x00\x01\x00\x02\x0F\xFF\x00\x04", 8));
    Writer wide, bits, rgb, planar, ybr, palette, inverse, signed8;
    wide.image(2, 2, 32, 32, true).pixels(QByteArray("\x00\x00\x01\x00\xFF\xFF\xFF\xFF\x10\x27\x00\x00\x00\x00\x00\x80", 16), "OB");
    bits.image(3, 5, 1, 1, false).add(Tag::NumberOfFrames, "IS", "2").pixels(QByteArray("\xA5\x5A\xC3\x3C", 4), "OB");
    rgb.image(2, 3, 8, 8, false, "RGB").us(Tag::PlanarConfiguration, 0).pixels(bytes(18), "OB");
    planar.image(2, 3, 8, 8, false, "RGB").us(Tag::PlanarConfiguration, 1).pixels(bytes(18, 11), "OB");
    ybr.image(2, 4, 8, 8, false, "YBR_FULL_422").pixels(bytes(16, 13), "OB");
    palette.us(Tag::SamplesPerPixel, 1).add(Tag::Photometric, "CS", "PALETTE COLOR").us(Tag::Rows, 2).us(Tag::Columns, 2)
        .us(Tag::BitsAllocated, 8).us(Tag::BitsStored, 8).us(Tag::HighBit, 7).us(Tag::PixelRepresentation, 0);
    for (quint32 c = 0; c < 3; ++c)
        palette.add(Tag::RedPaletteDescriptor + c, "US", QByteArray("\x04\x00\x00\x00\x10\x00", 6))
            .add(Tag::RedPaletteData + c, "OW", le16({0, 0x4000, 0x8000, 0xFFFF}));
    palette.pixels(QByteArray("\x00\x01\x02\x03", 4), "OB");
    Writer lut;
    lut.add(Tag::LutDescriptor, "US", QByteArray("\x00\x01\x10\x00\x0C\x00", 6));
    inverse.image(2, 2, 16, 10, false, "MONOCHROME1").add(Tag::PresentationLutShape, "CS", "INVERSE")
        .sequence(Tag::VoiLutSequence, {lut}).pixels(le16({0, 100, 500, 1023}));
    signed8.image(2, 2, 8, 7, true).pixels(QByteArray("\x00\x40\x7F\x3F", 4), "OB");

    // Compressed with what Dicolens decodes itself, and what it would hand
    // to ffmpeg (which fuzzing leaves out).
    Writer rleGrey, rleColour, lossless, losslessColour, losslessFrames, baseline, j2k, video;
    rleGrey.image(3, 4, 16, 12, false).encapsulated({rleFrame(planesOf(ramp(12, 300), 1, 2))});
    rleColour.image(2, 3, 8, 8, false, "RGB").add(Tag::NumberOfFrames, "IS", "2")
        .encapsulated({rleFrame(planesOf(bytes(18), 3, 1)), rleFrame(planesOf(bytes(18, 5), 3, 1))});
    lossless.image(4, 5, 16, 16, false).encapsulated({losslessJpeg(samples(20, 0xFFFF), 5, 4, 1, 16, 1, 0)});
    losslessColour.image(3, 4, 8, 8, false, "RGB").encapsulated({losslessJpeg(samples(36, 0xFF), 4, 3, 3, 8, 4, 4)});
    const QByteArray one = losslessJpeg(samples(12, 0xFFF), 4, 3, 1, 12, 6, 0), two = losslessJpeg(samples(12, 0xFF), 4, 3, 1, 12, 2, 4);
    QByteArray table(8, 0);
    qToLittleEndian<quint32>(quint32(8 + one.size() + one.size() % 2), table.data() + 4);
    losslessFrames.image(3, 4, 16, 12, true).add(Tag::NumberOfFrames, "IS", "2").encapsulated({one, two}, table);
    // Enough of a baseline JPEG and a JPEG 2000 codestream to be told apart.
    const QByteArray jpeg = QByteArray::fromHex("ffd8ffe000044a46ffc0000b080002000201011100ffda0008010100003f001234ffd9");
    baseline.image(2, 2, 8, 8, false).encapsulated({jpeg});
    j2k.image(2, 2, 16, 12, false).encapsulated({QByteArray("\xFF\x4F\xFF\x51\x00\x29", 6) + QByteArray(39, 1) + QByteArray("\xFF\xD9", 2)});
    video.image(2, 2, 8, 8, false, "YBR_PARTIAL_420").add(Tag::NumberOfFrames, "IS", "3")
        .encapsulated({QByteArray("\x00\x00\x00\x18" "ftypmp42", 12), QByteArray(16, 2)});

    const QList<std::pair<const char *, QByteArray>> images{
        {"grey", grey.file(ExplicitLE)},
        {"implicit", implicit.file(ImplicitLE)},
        {"bare", implicit.body},
        {"big-endian", big.file(ExplicitBE)},
        {"deflated", deflated(grey)},
        {"32-bit", wide.file(ExplicitLE)},
        {"1-bit", bits.file(ExplicitLE)},
        {"rgb", rgb.file(ExplicitLE)},
        {"rgb-planar", planar.file(ExplicitLE)},
        {"ybr-422", ybr.file(ExplicitLE)},
        {"palette", palette.file(ExplicitLE)},
        {"inverse", inverse.file(ExplicitLE)},
        {"signed-8-bit", signed8.file(ExplicitLE)},
        {"enhanced", enhanced(3).file(ExplicitLE)},
        {"overlay", overlaid(1).file(ExplicitLE)},
        {"overlay-frames", overlaid(3).file(ExplicitLE)},
        {"rle-grey", rleGrey.file(Rle)},
        {"rle-colour", rleColour.file(Rle)},
        {"lossless", lossless.file(JpegLossless)},
        {"lossless-colour", losslessColour.file(JpegLossless)},
        {"lossless-frames", losslessFrames.file(JpegLossless)},
        {"baseline", baseline.file(JpegBaseline)},
        {"jpeg-2000", j2k.file(Jpeg2000)},
        {"video", video.file(Mpeg4)},
    };
    for (const auto &[name, file] : images) {
        save("parse", name, file);
        save("decode", name, file);
    }

    // A sequence under a private tag, as an implicit file and as UN.
    Writer inner, privateSequence;
    inner.add(Tag::PatientName, "PN", "Hidden^Name");
    privateSequence.add(Tag::Modality, "CS", "OT");
    QByteArray item;
    privateSequence.u16(item, 0xFFFE);
    privateSequence.u16(item, 0xE000);
    privateSequence.u32(item, quint32(inner.body.size()));
    privateSequence.add(0x00091001, "UN", item + inner.body);
    save("parse", "private-sequence", privateSequence.file(ExplicitLE));

    const QByteArray report = structuredReport().file(ExplicitLE), state = presentationState().file(ExplicitLE);
    Writer pdf = patient();
    pdf.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.104.1").add(Tag::DocumentTitle, "ST", "Report")
        .add(Tag::EncapsulatedDocument, "OB", "%PDF-1.4\n%%EOF\n").add(Tag::DocumentMimeType, "LO", "application/pdf");
    save("parse", "report", report);
    save("parse", "presentation-state", state);
    save("reports", "structured", report);
    save("reports", "pdf", pdf.file(ExplicitLE));
    save("reports", "deflated", deflated(structuredReport()));
    save("marks", "presentation-state", state);
    save("marks", "overlay", overlaid(1).file(ExplicitLE));
    save("marks", "overlay-frames", overlaid(3).file(ExplicitLE));

    // Whole scans: a CT stack, a PET with what its SUVs take over the CT it
    // was taken with, one multi-frame file, and a study with its report and
    // marks.
    QTemporaryDir ct, pet, study;
    if (!writeVolume(ct.path()))
        qFatal("Couldn't write the CT stack");
    save("scan", "ct", joined(ct.path()));
    auto ball = [](int i, int j, int k) { return 100 * k + 10 * j + i; };
    if (!writeSeries(pet.path(), "CT", "1.2.77.1", "1.2.77.100", 4, 4, 2, 3, ball)
        || !writeSeries(pet.path(), "PT", "1.2.77.2", "1.2.77.100", 2, 3, 4, 4, ball))
        qFatal("Couldn't write the PET and its CT");
    save("scan", "pet-ct", joined(pet.path()));
    Writer drug, tracer = patient();
    drug.add(0x00181072, "TM", "090000").add(0x00181074, "DS", "370000000").add(0x00181075, "DS", "6586.2");
    tracer.add(Tag::Modality, "CS", "PT").add(0x00080031, "TM", "100000").add(0x00101030, "DS", "70")
        .add(0x00541001, "CS", "BQML").add(0x00541102, "CS", "START").sequence(0x00540016, {drug})
        .image(2, 2, 16, 16, false).pixels(le16({1, 2, 3, 4}));
    save("scan", "pet", tracer.file(ExplicitLE));
    save("scan", "enhanced", enhanced(4).file(ExplicitLE));
    if (!writeVolume(study.path()) || !write(study.path() + "/report", report) || !write(study.path() + "/state", state))
        qFatal("Couldn't write the study");
    save("scan", "study", joined(study.path()));
}

void streams() {
    // Lossless JPEG: every predictor, grey and colour, with and without
    // restart markers.
    for (int predictor = 1; predictor <= 7; ++predictor)
        save("ljpeg", qPrintable(QStringLiteral("grey-%1").arg(predictor)),
             losslessJpeg(samples(30, 0xFFFF), 6, 5, 1, 16, predictor, predictor % 2 ? 0 : 6));
    save("ljpeg", "12-bit", losslessJpeg(samples(12, 0xFFF), 4, 3, 1, 12, 1, 0));
    save("ljpeg", "colour", losslessJpeg(samples(36, 0xFF), 4, 3, 3, 8, 4, 0));
    save("ljpeg", "colour-restarts", losslessJpeg(samples(36, 0xFF), 4, 3, 3, 8, 7, 4));
    save("ljpeg", "widest-difference", losslessJpeg({0, 32768, 0, 65535}, 2, 2, 1, 16, 1, 0));

    // RLE frames and uncompressed ones, each after the layout it's read with.
    save("rle", "grey-8", layout(2, 3, 1, 8, 8) + rleFrame(planesOf(bytes(6), 1, 1)));
    save("rle", "grey-16", layout(3, 4, 1, 16, 12, true) + rleFrame(planesOf(ramp(12, 300), 1, 2)));
    save("rle", "colour", layout(2, 3, 3, 8, 8, false, 0, true, 2) + rleFrame(planesOf(bytes(18), 3, 1)));
    save("rle", "colour-16", layout(1, 2, 3, 16, 16, false, 0, true, 2) + rleFrame(planesOf(ramp(6, 999), 3, 2)));
    save("rle", "runs", layout(4, 64, 1, 8, 8) + rleFrame({QByteArray(200, 'a') + bytes(56)}));
    save("native", "grey-8", layout(2, 3, 1, 8, 8) + bytes(6));
    save("native", "grey-16", layout(3, 4, 1, 16, 12, true) + ramp(12, 300));
    save("native", "grey-16-big", layout(3, 4, 1, 16, 16, false, 0, false, 1) + ramp(12, 300));
    save("native", "grey-32", layout(2, 2, 1, 32, 32, true) + bytes(16, 51));
    save("native", "rgb", layout(2, 3, 3, 8, 8, false, 0, true, 2) + bytes(18));
    save("native", "rgb-planar", layout(2, 3, 3, 8, 8, false, 1, true, 2) + bytes(18));
    save("native", "rgb-16", layout(1, 2, 3, 16, 12, false, 0, true, 2) + ramp(6, 500));
    save("native", "ybr", layout(2, 3, 3, 8, 8, false, 0, true, 3) + bytes(18));
    save("native", "ybr-422", layout(2, 4, 3, 8, 8, false, 0, true, 4) + bytes(16));

    // Encapsulated pixel data, after the Number of Frames it's split for.
    Writer plain, tabled, jpegs;
    plain.encapsulated({"AAAA", "BB", "CCCCCC"});
    tabled.encapsulated({"AAAA", "BB", "CCCCCC"}, QByteArray("\x00\x00\x00\x00\x16\x00\x00\x00", 8));
    jpegs.encapsulated({QByteArray("\xFF\xD8" "aa", 4), "bbbb", QByteArray("\xFF\xD8" "cc", 4), QByteArray("\xFF\x4F" "dd", 4), "ee"});
    save("frames", "one-frame", int32(1) + plain.body.mid(12));
    save("frames", "a-fragment-each", int32(3) + plain.body.mid(12));
    save("frames", "offset-table", int32(2) + tabled.body.mid(12));
    save("frames", "by-marker", int32(3) + jpegs.body.mid(12));
}

}

int main(int argc, char **argv) {
    if (argc != 2)
        qFatal("Usage: %s <folder>", argv[0]);
    root = QString::fromLocal8Bit(argv[1]);
    files();
    streams();
    return 0;
}
