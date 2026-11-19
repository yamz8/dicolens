#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QtTest>

#include "backend.h"
#include "dicom.h"
#include "ljpeg.h"
#include "pixels.h"
#include "sliceview.h"
#include "study.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"

namespace {

bool write(const QString &path, const QByteArray &contents);

int lines(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
}

// The window's item with this objectName. What a Repeater, ListView or Loader
// makes isn't a QObject child of the window, so findChild never finds it; walk
// the items instead. Popups aren't items: find those with findChild.
QQuickItem *item(QQuickItem *from, const QString &name) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == name)
            return child;
        if (auto *found = item(child, name))
            return found;
    }
    return nullptr;
}

// A hyprctl that answers with response.json beside it and counts its calls.
QString fakeHyprctl(const QString &directory) {
    const QString command = directory + "/hyprctl";
    write(command, "#!/bin/bash\necho >> '" + directory.toUtf8() + "/calls'\ncat '" + directory.toUtf8()
                       + "/response.json'\n");
    QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return command;
}

QJsonObject readCatalog(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// The placeholders in a text, sorted: %1 to %99, and the count (%n, %Ln).
QStringList placeholders(const QString &text) {
    static const QRegularExpression placeholder("%L?(?:[1-9][0-9]?|n)");
    QStringList found;
    for (auto it = placeholder.globalMatch(text); it.hasNext();)
        found << it.next().captured();
    found.sort();
    return found;
}

// Every text the sources hand to qsTr() or tr(), and whether a call passes it
// a count. It walks the code a token at a time, so a string never hides a
// comment or the other way round, C++'s adjacent literals join, and QML's
// single quotes count. A text that isn't a literal can't be in a catalog.
QMap<QString, bool> translatables(const QString &folder, QStringList *problems) {
    QMap<QString, bool> found;
    for (const QFileInfo &info : QDir(folder).entryInfoList({"*.qml", "*.js", "*.cpp", "*.h"}, QDir::Files)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QString code = QString::fromUtf8(file.readAll());
        qsizetype i = 0;
        auto literal = [&](QString *text) {
            const QChar quote = code[i++];
            for (; i < code.size() && code[i] != quote; ++i) {
                if (code[i] == '\\' && i + 1 < code.size()) {
                    const QChar escaped = code[++i];
                    const int digits = escaped == 'u' ? 4 : escaped == 'x' ? 2 : 0;
                    bool hex = false;
                    const ushort unit = code.mid(i + 1, digits).toUShort(&hex, 16);
                    if (digits && hex) {
                        *text += QChar(unit);
                        i += digits;
                    } else {
                        *text += escaped == 'n' ? QChar('\n') : escaped == 't' ? QChar('\t') : escaped;
                    }
                } else {
                    *text += code[i];
                }
            }
            ++i;
        };
        auto skipSpace = [&] {
            while (i < code.size() && code[i].isSpace())
                ++i;
        };
        while (i < code.size()) {
            const QChar c = code[i];
            if (code.mid(i, 2) == "//") {
                i = code.indexOf('\n', i);
                if (i < 0)
                    break;
            } else if (code.mid(i, 2) == "/*") {
                i = code.indexOf("*/", i + 2);
                if (i < 0)
                    break;
                i += 2;
            } else if (c == '"' || c == '\'') {
                QString skipped;
                literal(&skipped);
            } else if (c.isLetter() || c == '_') {
                const qsizetype start = i;
                while (i < code.size() && (code[i].isLetterOrNumber() || code[i] == '_'))
                    ++i;
                const QString word = code.mid(start, i - start);
                skipSpace();
                // Dicolens also translates through QCoreApplication::translate()
                // and QT_TRANSLATE_NOOP(), whose first argument is the context.
                const bool withContext = word == "translate" || word == "QT_TRANSLATE_NOOP";
                if ((word != "qsTr" && word != "tr" && !withContext) || i >= code.size() || code[i] != '(')
                    continue;
                ++i;
                skipSpace();
                // A free function's own tr(), declared like Qt's.
                if (code.mid(i, 10) == "const char")
                    continue;
                if (withContext) {
                    if (i >= code.size() || code[i] != '"')
                        continue;
                    QString context;
                    literal(&context);
                    skipSpace();
                    if (i >= code.size() || code[i] != ',')
                        continue;
                    ++i;
                    skipSpace();
                }
                if (i >= code.size() || (code[i] != '"' && code[i] != '\'')) {
                    // C++ translates some English kept from where it was made
                    // (a decoder's error), marked there with QT_TRANSLATE_NOOP.
                    if (info.suffix() == "cpp")
                        continue;
                    *problems << info.fileName() + ": not a literal: " + code.mid(start, 40);
                    continue;
                }
                QString text;
                do {
                    literal(&text);
                    skipSpace();
                } while (i < code.size() && code[i] == '"');
                // The arguments after the text, to the call's closing parenthesis.
                int depth = 0, commas = 0;
                while (i < code.size()) {
                    const QChar d = code[i];
                    if (d == '"' || d == '\'') {
                        QString skipped;
                        literal(&skipped);
                        continue;
                    }
                    if (d == '(' || d == '[' || d == '{')
                        ++depth;
                    else if ((d == ')' || d == ']' || d == '}') && depth-- == 0)
                        break;
                    else if (d == ',' && depth == 0)
                        ++commas;
                    ++i;
                }
                found[text] = found.value(text) || commas >= 2;
            } else {
                ++i;
            }
        }
    }
    return found;
}

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

// Encodes grey samples with ffmpeg, or returns nothing when it can't.
QByteArray encode(const QList<int> &values, int width, const QString &pixelFormat, const QStringList &codec) {
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

class DicolensTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

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
        // ffmpeg's encoder writes 12-bit input as a 16-bit codestream, so the
        // values go in as 16 bits; its decoder's widening of real 12-bit
        // streams is undone by the same code as JPEG 2000's.
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

    void decodesTwelveBitJpeg2000() {
        // ffmpeg widens 12-bit JPEG 2000 to 16 bits, and offsets signed
        // samples; both must be undone.
        QList<int> values;
        for (int i = 0; i < 32 * 32; ++i)
            values << (i * 997) % 4096;
        QByteArray stream = encode(values, 32, "gray12le", {"-c:v", "libopenjpeg", "-numresolution", "3", "-f", "image2", "-update", "1"});
        if (stream.isEmpty())
            QSKIP("ffmpeg can't encode JPEG 2000 here");
        QTemporaryDir dir;
        auto decode = [&](const QByteArray &codestream, bool isSigned) {
            Writer w;
            w.image(32, 32, 16, 12, isSigned).encapsulated({codestream});
            write(dir.filePath("j2k.dcm"), w.file("1.2.840.10008.1.2.4.90"));
            return decodeFrame(DicomFile::read(dir.filePath("j2k.dcm")), 0);
        };
        Frame frame = decode(stream, false);
        QVERIFY2(frame.error.isEmpty(), qPrintable(frame.error + " " + frame.detail));
        for (int i = 0; i < values.size(); ++i)
            QCOMPARE(frame.stored(i % 32, i / 32), values[i]);

        // Marked signed, the same codestream holds each value less 2048.
        const int siz = stream.indexOf("\xFF\x51");
        stream[siz + 40] = char(uchar(stream[siz + 40]) | 0x80);
        frame = decode(stream, true);
        QVERIFY2(frame.error.isEmpty(), qPrintable(frame.error + " " + frame.detail));
        for (int i = 0; i < values.size(); ++i)
            QCOMPARE(frame.stored(i % 32, i / 32), values[i] - 2048);
    }

    void decodesVideo() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg isn't installed");
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-v", "error", "-f", "lavfi", "-i", "testsrc=size=64x48:rate=10", "-frames:v", "6",
                                "-c:v", "libx264", "-pix_fmt", "yuv420p", "-f", "h264", "-"});
        QVERIFY(ffmpeg.waitForFinished());
        const QByteArray stream = ffmpeg.readAllStandardOutput();
        if (stream.isEmpty())
            QSKIP("ffmpeg can't encode H.264 here");
        QTemporaryDir dir;
        Writer w;
        w.image(48, 64, 8, 8, false, "YBR_PARTIAL_420").add(Tag::NumberOfFrames, "IS", "6").encapsulated({stream});
        QVERIFY(write(dir.filePath("video.dcm"), w.file("1.2.840.10008.1.2.4.102")));
        const Scan scan = scanFiles({dir.filePath("video.dcm")});
        QCOMPARE(scan.series.first().slices.size(), 6);
        const QList<Frame> frames = decodeVideo(DicomFile::read(dir.filePath("video.dcm")));
        QCOMPARE(frames.size(), 6);
        QVERIFY(frames.first().isColor());
        QCOMPARE(frames.first().color.size(), QSize(64, 48));
        QCOMPARE(decodeVideo(DicomFile::read(dir.filePath("video.dcm")), 1).size(), 1);
    }

    void readsDeflatedFiles() {
        Writer body;
        body.add(Tag::PatientName, "PN", "Deflated^Demo").image(1, 2, 16, 16, false).pixels(le16({7, 4095}));
        // qCompress gives a zlib stream after a 4-byte length; DICOM wants
        // the raw deflate inside it.
        const QByteArray zlib = qCompress(body.body, 9);
        const QByteArray raw = zlib.mid(4 + 2, zlib.size() - 4 - 2 - 4);
        Writer meta;
        meta.add(Tag::TransferSyntax, "UI", "1.2.840.10008.1.2.1.99");
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("deflated.dcm"), QByteArray(128, 0) + "DICM" + meta.body + raw));
        QString error;
        const DicomFile file = DicomFile::read(dir.filePath("deflated.dcm"), &error);
        QVERIFY2(file.isValid(), qPrintable(error));
        QCOMPARE(file.string(Tag::PatientName), QString("Deflated^Demo"));
        const Frame frame = decodeFrame(file, 0);
        QVERIFY(frame.error.isEmpty());
        QCOMPARE(frame.stored(1, 0), 4095);
    }

    void keepsThirtyTwoBitValues() {
        ImageFormat f;
        f.rows = 1;
        f.columns = 3;
        f.bitsAllocated = 32;
        f.bitsStored = 32;
        f.highBit = 31;
        f.isSigned = true;
        f.slope = 0.5;
        QByteArray data;
        for (qint32 v : {-100000, 0, 100000}) {
            char b[4];
            qToLittleEndian(v, b);
            data.append(b, 4);
        }
        const Frame frame = convertNative(reinterpret_cast<const uchar *>(data.constData()), data.size(), f);
        QVERIFY(frame.error.isEmpty());
        // Scaled into 16 bits, the values shown stay within a step of true.
        const double step = 200000.0 / 65535 * 0.5;
        QVERIFY(qAbs(frame.value(0, 0) + 50000) <= step);
        QVERIFY(qAbs(frame.value(1, 0)) <= step);
        QVERIFY(qAbs(frame.value(2, 0) - 50000) <= step);
    }

    void readsOneBitImages() {
        // Two 3x3 frames, packed across the byte boundary.
        QTemporaryDir dir;
        Writer w;
        w.image(3, 3, 1, 1, false).add(Tag::NumberOfFrames, "IS", "2").pixels(QByteArray("\x01\xFF\x02\x00", 4), "OB");
        QVERIFY(write(dir.filePath("bits.dcm"), w.file(ExplicitLE)));
        const DicomFile file = DicomFile::read(dir.filePath("bits.dcm"));
        const Frame first = decodeFrame(file, 0), second = decodeFrame(file, 1);
        QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
        QCOMPARE(first.stored(0, 0), 1);
        QCOMPARE(first.stored(1, 0), 0);
        QCOMPARE(first.stored(2, 2), 1);   // Bit 8.
        QCOMPARE(second.stored(0, 0), 1);  // Bit 9.
        QCOMPARE(second.stored(1, 2), 0);  // Bit 16, the lowest of 0x02.
        QCOMPARE(second.stored(2, 2), 1);  // Bit 17.
    }

    void leavesPaddingOutOfTheRange() {
        QTemporaryDir dir;
        Writer w;
        w.image(1, 3, 16, 16, true)
            .add(Tag::PixelPaddingValue, "SS", QByteArray("\x30\xF8", 2))
            .add(Tag::PresentationLutShape, "CS", "INVERSE")
            .pixels(le16({quint16(qint16(-2000)), 10, 30}));
        QVERIFY(write(dir.filePath("pad.dcm"), w.file(ExplicitLE)));
        const Frame frame = decodeFrame(DicomFile::read(dir.filePath("pad.dcm")), 0);
        QVERIFY(frame.hasPadding);
        double low = 0, high = 0;
        frame.range(low, high);
        QCOMPARE(low, 10.0);
        QCOMPARE(high, 30.0);
        QVERIFY(frame.inverted);
    }

    void windowsFromAVoiLut() {
        QTemporaryDir dir;
        Writer item;
        QByteArray descriptor;
        for (int v : {1024, 512, 12})
            descriptor += le16({v});
        item.add(Tag::LutDescriptor, "US", descriptor);
        Writer w;
        w.image(1, 2, 16, 12, false);
        QByteArray sequence;
        w.body += w.header(Tag::VoiLutSequence, "SQ", 0xFFFFFFFF);
        auto tag = [&](quint16 g, quint16 e, quint32 length) {
            QByteArray out;
            w.u16(out, g); w.u16(out, e); w.u32(out, length);
            return out;
        };
        w.body += tag(0xFFFE, 0xE000, item.body.size()) + item.body + tag(0xFFFE, 0xE0DD, 0);
        w.pixels(le16({0, 4095}));
        QVERIFY(write(dir.filePath("voi.dcm"), w.file(ExplicitLE)));
        const Frame frame = decodeFrame(DicomFile::read(dir.filePath("voi.dcm")), 0);
        QVERIFY(frame.error.isEmpty());
        QCOMPARE(frame.windowWidth, 1024.0);
        QCOMPARE(frame.windowCenter, 512 + 1023 / 2.0);
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

    void fileWindowBelongsToTheSeriesShown() {
        // Two series with windows of their own: after a visit to the second,
        // 0 on the first goes back to the first's, not the one seen last.
        QTemporaryDir dir;
        for (int series = 0; series < 2; ++series) {
            Writer w;
            w.image(2, 2, 16, 12, false)
                .add(Tag::Modality, "CT", "CT")
                .add(0x0020000E, "UI", series ? "1.2.3.2" : "1.2.3.1")
                .add(Tag::SeriesNumber, "IS", QByteArray::number(series + 1))
                .add(Tag::WindowCenter, "DS", series ? "500" : "40")
                .add(Tag::WindowWidth, "DS", series ? "2000" : "400")
                .pixels(le16({0, 1, 2, 3}));
            QVERIFY(write(dir.filePath(QString("IM%1").arg(series)), w.file(ExplicitLE)));
        }
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.windowCenter(), 40.0);
        backend.selectSeries(1);
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.windowCenter(), 500.0);
        backend.selectSeries(0);
        QVERIFY(backend.waitForSlice(10000));
        backend.preset(3);
        backend.fileWindow();
        QCOMPARE(backend.windowCenter(), 40.0);
        QCOMPARE(backend.windowWidth(), 400.0);
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
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QColor("black"));
        QVERIFY(!theme.light());
        QCOMPARE(theme.radius(), 0);
    }

    void themeReadsThePalette() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("theme"));
        QVERIFY(write(dir.path() + "/theme/colors.toml", "mode = \"light\"\n"
                                                         "theme_type = \"dark\"\n"
                                                         "accent = \"#112233\"\n"
                                                         "bg = #fafafa # paper\n"
                                                         "foreground = '#202020'\n"
                                                         "color1 = \"#aa0000\"\n"));

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.accentForeground(), QColor("white"));
        QCOMPARE(theme.background(), QColor("#fafafa"));
        QCOMPARE(theme.foreground(), QColor("#202020"));
        QCOMPARE(theme.colors().value("red").value<QColor>(), QColor("#aa0000"));
        QVERIFY(theme.light());
    }

    void themeAsksHyprlandAgainOnlyWhenItReloads() {
        // The fake Hyprland lives outside the theme directory, whose changes
        // the Theme also follows.
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));

        // Once on start, and once more when the event socket connects.
        Theme theme(dir.path(), nullptr, command, events.fullServerName());
        QTRY_VERIFY(events.hasPendingConnections());
        QLocalSocket *hyprland = events.nextPendingConnection();
        QTRY_COMPARE(lines(hypr.filePath("calls")), 2);
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":10}"));
        QTest::qWait(300);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        // Events arrive in pieces; only a whole configreloaded line counts.
        hyprland->write("activewindow>>kitty,~\nconfigrel");
        hyprland->flush();
        QTest::qWait(100);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        hyprland->write("oaded>>\n");
        QTRY_COMPARE(theme.radius(), 10);
        QCOMPARE(lines(hypr.filePath("calls")), 3);
    }

    void themeReachesHyprlandOnceItListens() {
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        Theme theme(dir.path(), nullptr, command, hypr.filePath("events.sock"));
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":8}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));
        QTRY_VERIFY_WITH_TIMEOUT(events.hasPendingConnections(), 5000);
        QTRY_COMPARE(theme.radius(), 8);
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

    void translatorSeesAFirstPick() {
        // Before the first pick there is no ~/.config/omarchy to watch.
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/da.json", R"({"Save": "Gem"})"));
        Translator translator(dir.path() + "/omarchy/language", dir.path());
        QVERIFY(QDir(dir.path()).mkpath("omarchy"));
        QVERIFY(write(dir.path() + "/omarchy/language", "da\n"));
        QTRY_COMPARE(translator.translate("Main", "Save"), QString("Gem"));
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "ru\n"));
        QVERIFY(write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"},
                                                   "%n files": {"one": "%n file", "other": "%n files"}})"));
        QVERIFY(write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        // An untranslated counted string falls back to English, with English rules.
        QCOMPARE(translator.translate("Main", "%n files", nullptr, 21), QString("%n files"));
    }

    void englishCatalogListsEveryString() {
        // en.json is what translators work from: every text the sources
        // translate is in it, as itself or, when counted, as its English
        // plural forms, and nothing else is.
        const QString sources = QFileInfo(QFINDTESTDATA("../src/Main.qml")).absolutePath();
        QStringList problems;
        const QMap<QString, bool> used = translatables(sources, &problems);
        const QJsonObject english = readCatalog(sources + "/i18n/en.json");
        for (auto it = used.begin(); it != used.end(); ++it) {
            const QJsonValue value = english.value(it.key());
            if (value.isUndefined())
                problems << "missing from en.json: " + it.key();
            else if (it.value() && (value.toObject().keys() != QStringList{"one", "other"}
                                    || value.toObject().value("other").toString() != it.key()))
                problems << "counted, so needs its one and other forms: " + it.key();
            else if (!it.value() && value.toString() != it.key())
                problems << "should be itself: " + it.key();
        }
        for (const QString &key : english.keys())
            if (!used.contains(key))
                problems << "in en.json but no source translates it: " + key;
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void catalogsMatchTheEnglishOne() {
        // A catalog may lack a string (it shows in English meanwhile), but what
        // it has translates something in en.json, with the same placeholders,
        // and a counted string has every form its language uses.
        const QString folder = QFileInfo(QFINDTESTDATA("../src/i18n/en.json")).absolutePath();
        const QJsonObject english = readCatalog(folder + "/en.json");
        QStringList problems;
        for (const QFileInfo &file : QDir(folder).entryInfoList({"*.json"}, QDir::Files)) {
            const QString code = file.completeBaseName();
            const QJsonObject catalog = readCatalog(file.filePath());
            if (catalog.isEmpty())
                problems << code + ": not a JSON object, or empty";
            // Some languages reach a form only at a million ("many" in French).
            QSet<QString> categories{"other", Translator::pluralCategory(code, 1000000)};
            for (long long n = 0; n <= 1000; ++n)
                categories << Translator::pluralCategory(code, n);
            for (auto it = catalog.begin(); it != catalog.end(); ++it) {
                const QString where = code + ": " + it.key() + ": ";
                const QJsonValue source = english.value(it.key());
                if (source.isUndefined()) {
                    problems << where + "not in en.json";
                } else if (source.isString()) {
                    if (!it.value().isString() || it.value().toString().isEmpty())
                        problems << where + "needs a translation";
                    else if (placeholders(it.value().toString()) != placeholders(it.key()))
                        problems << where + "has other placeholders than the English";
                } else {
                    const QJsonObject forms = it.value().toObject();
                    const QStringList wanted = placeholders(source.toObject().value("other").toString());
                    for (const QString &category : categories)
                        if (forms.value(category).toString().isEmpty())
                            problems << where + "needs its " + category + " form";
                    // A form other than "other" may say its number in words.
                    for (auto form = forms.begin(); form != forms.end(); ++form) {
                        const QStringList has = placeholders(form.value().toString());
                        QStringList needed = wanted;
                        if (form.key() != "other")
                            needed.removeIf([&](const QString &p) { return p.endsWith('n') && !has.contains(p); });
                        if (has != needed)
                            problems << where + form.key() + " has other placeholders than the English";
                    }
                }
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void windowLoadsAndAnswersTheKeyboard() {
        // A temp language and theme, so the test never depends on the desktop's.
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        Theme theme(dir.path());
        Backend backend(nullptr);
        qmlRegisterType<SliceView>("Dicolens", 1, 0, "SliceView");
        qmlRegisterUncreatableType<Backend>("Dicolens", 1, 0, "Backend", "The backend is given to QML as \"backend\"");

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.addImageProvider("thumbnails", new ThumbnailProvider(&backend));
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        // Nothing open: the window says how to open a scan, on the theme's colors.
        auto *empty = item(window->contentItem(), "emptyState");
        QVERIFY(empty);
        QVERIFY(empty->isVisible());
        QCOMPARE(window->color(), theme.background());

        // ? shows every shortcut, and Esc puts them away.
        auto *help = item(window->contentItem(), "help");
        QVERIFY(help);
        QVERIFY(!help->isVisible());
        QTest::keyClick(window, '?');
        QTRY_VERIFY(help->isVisible());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!help->isVisible());

        // A scan opens into the series list, and the arrows move through it.
        QTemporaryDir scans;
        for (int i = 0; i < 3; ++i) {
            Writer w;
            w.image(2, 2, 16, 12, false)
                .add(Tag::Modality, "CT", "CT")
                .add(Tag::InstanceNumber, "IS", QByteArray::number(i + 1))
                .pixels(le16({0, 1, 2, 3}));
            QVERIFY(write(scans.filePath(QString("IM%1").arg(i)), w.file(ExplicitLE)));
        }
        backend.loadPaths({scans.path()});
        QVERIFY(backend.waitForSlice(10000));
        QTRY_VERIFY(!empty->isVisible());
        auto *series = item(window->contentItem(), "seriesList");
        QVERIFY(series);
        QTRY_VERIFY(series->isVisible());
        QCOMPARE(backend.sliceIndex(), 0);
        QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(backend.sliceIndex(), 1);
        QTest::keyClick(window, Qt::Key_End);
        QCOMPARE(backend.sliceIndex(), 2);
        QTest::keyClick(window, 'S');
        QTRY_VERIFY(!series->isVisible());

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_MAIN(DicolensTests)
#include "dicolens_tests.moc"
