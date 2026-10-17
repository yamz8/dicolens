#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "backend.h"
#include "dicom.h"
#include "ljpeg.h"
#include "pixels.h"
#include "study.h"
#include "theme.h"
#include "translator.h"

namespace {

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

QByteArray le16(const QList<int> &values) {
    QByteArray out;
    for (int v : values) {
        char b[2];
        qToLittleEndian(quint16(v), b);
        out.append(b, 2);
    }
    return out;
}

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

const QString ExplicitLE = "1.2.840.10008.1.2.1";

// A plain lossless JPEG encoder (T.81 process 14), with one Huffman table
// giving each difference category a 5-bit code.
QByteArray losslessJpeg(const QVector<quint16> &samples, int width, int height, int components, int precision,
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

}

class ScansTests : public QObject {
    Q_OBJECT

private slots:
    void parsesExplicitLittleEndian() {
        Writer w;
        w.add(Tag::PatientName, "PN", "Doe^Jane^Q^Dr")
            .add(Tag::StudyDate, "DA", "20260314")
            .add(Tag::PixelSpacing, "DS", "0.5\\0.75")
            .us(Tag::Rows, 512);
        QString error;
        const DicomFile file = DicomFile::parse(w.file(ExplicitLE), &error);
        QVERIFY2(file.isValid(), qPrintable(error));
        QCOMPARE(file.transferSyntax(), ExplicitLE);
        QCOMPARE(file.string(Tag::PatientName), QString("Doe^Jane^Q^Dr"));
        QCOMPARE(DicomFile::formatName(file.string(Tag::PatientName)), QString("Doe, Dr Jane Q"));
        QCOMPARE(DicomFile::formatDate(file.string(Tag::StudyDate)), QString("2026-03-14"));
        QCOMPARE(file.numbers(Tag::PixelSpacing), (QList<double>{0.5, 0.75}));
        QCOMPARE(file.integer(Tag::Rows), 512);
        QCOMPARE(Dictionary::name(Tag::PatientName), QString("Patient's Name"));
    }

    void parsesImplicitWithSequences() {
        // An implicit file with an undefined-length sequence of two items,
        // one of them undefined length too, then an ordinary tag after it.
        Writer w;
        w.explicitVr = false;
        Writer item;
        item.explicitVr = false;
        item.add(0x00081150, "UI", "1.2.840.10008.5.1.4.1.1.2");
        QByteArray sequence;
        auto u16 = [&](quint16 v) { char b[2]; qToLittleEndian(v, b); sequence.append(b, 2); };
        auto u32 = [&](quint32 v) { char b[4]; qToLittleEndian(v, b); sequence.append(b, 4); };
        u16(0x0008); u16(0x1140); u32(0xFFFFFFFF);
        u16(0xFFFE); u16(0xE000); u32(item.body.size()); sequence += item.body;
        u16(0xFFFE); u16(0xE000); u32(0xFFFFFFFF); sequence += item.body; u16(0xFFFE); u16(0xE00D); u32(0);
        u16(0xFFFE); u16(0xE0DD); u32(0);
        w.body += sequence;
        w.add(Tag::Modality, "CS", "CT");

        QString error;
        const DicomFile file = DicomFile::parse(w.file("1.2.840.10008.1.2"), &error);
        QVERIFY2(file.isValid(), qPrintable(error));
        const DicomElement *e = file.element(0x00081140);
        QVERIFY(e);
        QCOMPARE(e->items.size(), 2);
        QCOMPARE(e->items[1].first().tag, quint32(0x00081150));
        QCOMPARE(file.string(Tag::Modality), QString("CT"));
    }

    void parsesBigEndian() {
        Writer w;
        w.bigEndian = true;
        w.image(2, 2, 16, 16, false).pixels(QByteArray("\x01\x00\x00\x02\x00\x03\x12\x34", 8));
        // The meta group is little endian whatever the body is.
        Writer meta;
        meta.add(Tag::TransferSyntax, "UI", "1.2.840.10008.1.2.2");
        const QByteArray bytes = QByteArray(128, 0) + "DICM" + meta.body + w.body;
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("be.dcm"), bytes));
        const DicomFile file = DicomFile::read(dir.filePath("be.dcm"));
        QVERIFY(file.isValid());
        QVERIFY(!file.littleEndian());
        QCOMPARE(file.integer(Tag::Rows), 2);
        const Frame frame = decodeFrame(file, 0);
        QCOMPARE(frame.stored(0, 0), 256);
        QCOMPARE(frame.stored(1, 1), 0x1234);
    }

    void rejectsOtherFiles() {
        QString error;
        QVERIFY(!DicomFile::parse(QByteArray("%PDF-1.7 not a scan at all"), &error).isValid());
        QCOMPARE(error, QString("Not a DICOM file"));
    }

    void decodesStoredBits() {
        // 12 bits stored, signed, in 16-bit words with junk in the top bits.
        ImageFormat f;
        f.rows = 1;
        f.columns = 3;
        f.bitsAllocated = 16;
        f.bitsStored = 12;
        f.highBit = 11;
        f.isSigned = true;
        f.slope = 1;
        f.intercept = -1024;
        const QByteArray data = le16({0xF000 | 0x07FF, 0x0800, 0x0FFF});
        const Frame frame = convertNative(reinterpret_cast<const uchar *>(data.constData()), data.size(), f);
        QVERIFY(frame.error.isEmpty());
        QCOMPARE(frame.stored(0, 0), 2047);
        QCOMPARE(frame.stored(1, 0), -2048);
        QCOMPARE(frame.stored(2, 0), -1);
        QCOMPARE(frame.value(0, 0), 1023.0);
    }

    void decodesPlanarColour() {
        ImageFormat f;
        f.rows = 1;
        f.columns = 2;
        f.samples = 3;
        f.bitsAllocated = 8;
        f.bitsStored = 8;
        f.highBit = 7;
        f.planar = 1;
        f.photometric = "RGB";
        const QByteArray data("\xFF\x00\x10\x20\x00\xFF", 6);
        const Frame frame = convertNative(reinterpret_cast<const uchar *>(data.constData()), data.size(), f);
        QVERIFY(frame.isColor());
        QCOMPARE(frame.color.pixelColor(0, 0), QColor(255, 16, 0));
        QCOMPARE(frame.color.pixelColor(1, 0), QColor(0, 32, 255));
    }

    void rendersThroughTheWindow() {
        Frame frame;
        frame.width = 4;
        frame.height = 1;
        frame.isSigned = true;
        frame.mono = {quint16(qint16(-1000)), 40, 300, quint16(qint16(-160))};
        const QImage image = render(frame, 40, 400, false);
        QCOMPARE(qGray(image.pixel(0, 0)), 0);
        QVERIFY(qAbs(qGray(image.pixel(1, 0)) - 128) <= 1);
        QCOMPARE(qGray(image.pixel(2, 0)), 255);
        QVERIFY(qGray(image.pixel(3, 0)) <= 1);
        // MONOCHROME1 shows low values white, and invert flips that back.
        frame.inverted = true;
        QCOMPARE(qGray(render(frame, 40, 400, false).pixel(0, 0)), 255);
        QCOMPARE(qGray(render(frame, 40, 400, true).pixel(0, 0)), 0);
    }

    void decodesRle() {
        // One 8-bit sample: a run of four 7s, then the literal bytes 1, 2.
        ImageFormat f;
        f.rows = 2;
        f.columns = 3;
        f.bitsAllocated = 8;
        f.bitsStored = 8;
        f.highBit = 7;
        QByteArray data(64, 0);
        qToLittleEndian<quint32>(1, data.data());
        qToLittleEndian<quint32>(64, data.data() + 4);
        data += QByteArray("\xFD\x07\x01\x01\x02", 5);
        QString error;
        const QByteArray planes = decodeRle(data, f, &error);
        QVERIFY2(!planes.isEmpty(), qPrintable(error));
        QCOMPARE(planes, QByteArray("\x07\x07\x07\x07\x01\x02", 6));
    }

    void splitsEncapsulatedFrames() {
        Writer w;
        w.encapsulated({"AAAA", "BB", "CCCCCC"});
        const QByteArray items = w.body.mid(12);
        // Without an offset table, three fragments for three frames.
        QCOMPARE(DicomFile::splitFrames(items, 3), (QList<QByteArray>{"AAAA", "BB", "CCCCCC"}));
        // One frame takes every fragment.
        QCOMPARE(DicomFile::splitFrames(items, 1), (QList<QByteArray>{"AAAABBCCCCCC"}));
        // An offset table can join fragments into a frame.
        Writer withTable;
        withTable.encapsulated({"AAAA", "BB", "CCCCCC"}, QByteArray("\x00\x00\x00\x00\x16\x00\x00\x00", 8));
        QCOMPARE(DicomFile::splitFrames(withTable.body.mid(12), 2), (QList<QByteArray>{"AAAABB", "CCCCCC"}));
    }

    void decodesLosslessJpeg() {
        // Every predictor, grey at 16 bits (whose biggest difference has
        // its own code) and interleaved colour, with and without restarts.
        for (int predictor = 1; predictor <= 7; ++predictor)
            for (int components : {1, 3})
                for (int restart : {0, 5}) {
                    const int width = 5, height = 4, precision = components == 1 ? 16 : 8;
                    QVector<quint16> samples;
                    for (int i = 0; i < width * height * components; ++i)
                        samples << quint16(i == 3 ? (1 << precision) - 1 : i == 4 ? 0 : (i * 7919 + predictor * 31) % (1 << precision));
                    const QByteArray stream = losslessJpeg(samples, width, height, components, precision, predictor, restart);
                    LosslessJpeg out;
                    QString error;
                    QVERIFY2(decodeLosslessJpeg(stream, out, &error), qPrintable(error));
                    QCOMPARE(out.components, components);
                    QCOMPARE(out.samples, samples);
                }
    }

    void decodesJpegLsThroughFfmpeg() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg isn't installed");
        QList<int> values;
        for (int i = 0; i < 64; ++i)
            values << (i * 61) % 4096;
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-v", "error", "-f", "rawvideo", "-pix_fmt", "gray16le", "-s", "8x8", "-i", "-",
                                "-c:v", "jpegls", "-f", "image2pipe", "-"});
        ffmpeg.write(le16(values));
        ffmpeg.closeWriteChannel();
        QVERIFY(ffmpeg.waitForFinished());
        const QByteArray stream = ffmpeg.readAllStandardOutput();

        QTemporaryDir dir;
        Writer w;
        w.image(8, 8, 16, 12, false).encapsulated({stream});
        const QString path = dir.filePath("ls.dcm");
        QVERIFY(write(path, w.file("1.2.840.10008.1.2.4.80")));
        const Frame frame = decodeFrame(DicomFile::read(path), 0);
        QVERIFY2(frame.error.isEmpty(), qPrintable(frame.error + " " + frame.detail));
        for (int i = 0; i < 64; ++i)
            QCOMPARE(frame.stored(i % 8, i / 8), values[i]);
    }

    void sortsSlicesAlongTheNormal() {
        // Instance numbers count down the body; positions are out of order.
        QTemporaryDir dir;
        const QList<std::pair<int, double>> slices{{2, -5}, {1, 0}, {3, -10}};
        for (const auto &[instance, z] : slices) {
            Writer w;
            w.image(2, 2, 16, 16, false)
                .add(Tag::InstanceNumber, "IS", QByteArray::number(instance))
                .add(Tag::ImagePosition, "DS", "0\\0\\" + QByteArray::number(z))
                .add(Tag::ImageOrientation, "DS", "1\\0\\0\\0\\1\\0")
                .pixels(le16({0, 0, 0, 0}));
            QVERIFY(write(dir.filePath(QString("slice%1").arg(instance * 7 % 5)), w.file(ExplicitLE)));
        }
        const Scan scan = scanFiles(listFiles({dir.path()}, true));
        QCOMPARE(scan.series.size(), 1);
        QList<int> order;
        for (const auto &slice : scan.series.first().slices)
            order << slice.instance;
        QCOMPARE(order, (QList<int>{1, 2, 3}));
        QCOMPARE(scan.series.first().edgeLabels(), (QStringList{"R", "L", "A", "P"}));
    }

    void backendShowsAndWindows() {
        QTemporaryDir dir;
        for (int i = 0; i < 4; ++i) {
            Writer w;
            w.image(2, 2, 16, 12, false)
                .add(Tag::Modality, "CT", "CT")
                .add(Tag::InstanceNumber, "IS", QByteArray::number(i + 1))
                .add(Tag::PixelSpacing, "DS", "0.5\\0.5")
                .add(Tag::WindowCenter, "DS", "40")
                .add(Tag::WindowWidth, "DS", "400")
                .add(Tag::RescaleIntercept, "DS", "-1024")
                .add(Tag::RescaleSlope, "DS", "1")
                .pixels(le16({1024 + i, 0, 2048, 1024}));
            QVERIFY(write(dir.filePath(QString("IM%1").arg(i)), w.file(ExplicitLE)));
        }
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        QVERIFY(backend.frameReady());
        QCOMPARE(backend.sliceCount(), 4);
        QCOMPARE(backend.windowCenter(), 40.0);
        QCOMPARE(backend.windowWidth(), 400.0);
        QVERIFY(backend.valueAt(0.5, 0.5).endsWith(" 0 HU"));
        QVERIFY(backend.valueAt(1.5, 1.5).endsWith(" 0 HU"));
        QVERIFY(backend.valueAt(2.5, 0.5).isEmpty());
        QCOMPARE(backend.distance(0, 0, 3, 4), QString("2.5 mm"));

        backend.step(2);
        QCOMPARE(backend.sliceIndex(), 2);
        QVERIFY(backend.waitForSlice(5000));
        QVERIFY(backend.valueAt(0, 0).endsWith(" 2 HU"));

        backend.preset(3);
        QCOMPARE(backend.windowName(), QString("Lung"));
        backend.toggleInvert();
        QVERIFY(backend.inverted());
        backend.undo();
        QVERIFY(!backend.inverted());
        backend.undo();
        QCOMPARE(backend.windowCenter(), 40.0);
        QVERIFY(!backend.canUndo());
        backend.redo();
        QCOMPARE(backend.windowCenter(), -600.0);
    }

    void backendSaysWhenNothingIsThere() {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("notes.txt"), "not a scan"));
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QTRY_VERIFY(!backend.scanning());
        QVERIFY(!backend.hasImages());
        QCOMPARE(backend.problem(), QString("No DICOM images were found there"));
    }

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QVERIFY(write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n"));
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding() {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(response, "{\"int\":0}"));
        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(write(response, "{\"int\":6}"));
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "pt-BR\n"));
        QVERIFY(write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})"));
        QVERIFY(write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})"));
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "ru\n"));
        QVERIFY(write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})"));
        QVERIFY(write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    void catalogsAreValid() {
        // Every shipped catalog parses, and keeps each %1 its English has.
        QDir catalogs(":/i18n");
        const QStringList files = catalogs.entryList({"*.json"});
        QVERIFY(files.contains("en.json"));
        for (const auto &name : files) {
            QFile file(catalogs.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QJsonParseError error;
            const QJsonObject object = QJsonDocument::fromJson(file.readAll(), &error).object();
            QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(name + ": " + error.errorString()));
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (!it.value().isString())
                    continue;
                for (const char *placeholder : {"%1", "%2", "%3", "%4"})
                    if (it.key().contains(placeholder))
                        QVERIFY2(it.value().toString().contains(placeholder),
                                 qPrintable(name + ": \"" + it.key() + "\" loses " + placeholder));
            }
        }
    }
};

QTEST_MAIN(ScansTests)
#include "scans_tests.moc"
