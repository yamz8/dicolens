#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <QJsonDocument>
#include <QJsonArray>
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

#include "anonymize.h"
#include "archive.h"
#include "backend.h"
#include "dicom.h"
#include "ljpeg.h"
#include "marks.h"
#include "pixels.h"
#include "reformat.h"
#include "reports.h"
#include "sliceview.h"
#include "qmlmodule.h"
#include "volumeview.h"
#include "measurestore.h"
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

// An axial CT stack, 4 columns by 3 rows, 5 slices 3 mm apart from the feet
// up, with each pixel 100 × its slice + 10 × its row + its column, so a
// value says where it came from.
bool writeVolume(const QString &folder, const QByteArray &series = "1.2.9.1") {
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
bool writeSeries(const QString &folder, const QByteArray &modality, const QByteArray &series, const QByteArray &frame,
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
double shownNumber(const QString &text) {
    const QRegularExpression number("(-?[0-9.]+)\\s*[A-Z]*$");
    return number.match(text.trimmed()).captured(1).toDouble();
}

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
    QTemporaryDir m_data;

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        // Measurements are kept in a folder of the tests' own, never the user's.
        QVERIFY(m_data.isValid());
        MeasureStore::instance().setFolder(m_data.filePath("measurements"));
    }

    // Each test measures from nothing.
    void init() {
        static int test = 0;
        MeasureStore::instance().setFolder(m_data.filePath(QStringLiteral("measurements-%1").arg(++test)));
    }

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

    void opensAFileAtItsOwnSlice() {
        // One file opens with the rest of its folder, at the slice it is.
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        Backend backend(nullptr);
        backend.loadPaths({dir.filePath("1.2.9.1-3")});
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.sliceCount(), 5);
        QCOMPARE(backend.sliceIndex(), 3);
        QVERIFY(backend.valueAt(0, 0).contains("300"));

        // And again over a scan already open, whose slices are other ones.
        QTemporaryDir other;
        QVERIFY(writeVolume(other.path(), "1.2.9.2"));
        backend.loadPaths({other.filePath("1.2.9.2-1")});
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.sliceIndex(), 1);
    }

    void panesTakeOnlyTheirOwnAnswers() {
        // Two panes share the file chooser. One asks and cancels; what the
        // other then picks is the other's alone.
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        FakePicker picker;
        Backend left(&picker), right(&picker);
        left.open();   // Cancelled: the fake has no answer yet.
        picker.answer = dir.path();
        right.openFolder();
        QVERIFY(right.waitForSlice(10000));
        QVERIFY(right.hasImages());
        QVERIFY(!left.hasImages() && !left.scanning());

        // A save from one pane isn't written by the other as well.
        left.loadPaths({dir.path()});
        QVERIFY(left.waitForSlice(10000));
        left.setSliceIndex(4);
        QVERIFY(left.waitForSlice(10000));
        picker.answer.clear();
        left.exportImage();   // Cancelled.
        picker.answer = dir.filePath("right.png");
        right.exportImage();
        QTRY_COMPARE(right.notice(), QString("Saved right.png"));
        QCOMPARE(left.notice(), QString());
    }

    void commandsNeedNoDisplay() {
        // Every command runs where there is no display to open a window on,
        // as an agent's shell or a server has none; without one, an
        // application made the usual way aborts before it reads a word.
        const QString app = QCoreApplication::applicationDirPath() + "/dicolens";
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *name : {"QT_QPA_PLATFORM", "WAYLAND_DISPLAY", "DISPLAY"})
            env.remove(name);
        const QList<QStringList> commands{
            {"--version"}, {"--help"}, {"help"}, {"--no-such-option"}, {"skill"},
            {"info", dir.path()}, {"tags", dir.filePath("1.2.9.1-0")},
            {"export", dir.path(), dir.filePath("slice.png")}, {"report", dir.path()},
            {"measurements", dir.path()}, {"anonymize", dir.path(), "--out", dir.filePath("copy")},
        };
        for (const QStringList &arguments : commands) {
            QProcess p;
            p.setProcessEnvironment(env);
            p.start(app, arguments);
            QVERIFY(p.waitForFinished(30000));
            const QByteArray said = arguments.join(' ').toUtf8() + ": " + p.readAllStandardError();
            QVERIFY2(p.exitStatus() == QProcess::NormalExit, said.constData());
            QVERIFY2(p.exitCode() == 0 || p.exitCode() == 1, said.constData());
        }
    }

    void tagsOfOneFrame() {
        // An enhanced file's per-frame groups hold an item a frame; --frame
        // gives the one asked for.
        const QString app = QCoreApplication::applicationDirPath() + "/dicolens";
        QTemporaryDir dir;
        QList<Writer> frames;
        for (const char *position : {"0\\0\\0", "0\\0\\5"}) {
            Writer plane, group;
            plane.add(Tag::ImagePosition, "DS", position);
            group.sequence(0x00209113, {plane});
            frames << group;
        }
        Writer w;
        w.image(2, 2, 16, 12, false)
            .add(Tag::NumberOfFrames, "IS", "2")
            .sequence(Tag::PerFrameGroups, frames)
            .pixels(le16({1, 2, 3, 4, 5, 6, 7, 8}));
        QVERIFY(write(dir.filePath("ENHANCED"), w.file(ExplicitLE)));

        auto run = [&](const QStringList &arguments, int *code) {
            QProcess p;
            p.start(app, arguments);
            p.waitForFinished(20000);
            *code = p.exitCode();
            for (const QJsonValue &v : QJsonDocument::fromJson(p.readAllStandardOutput()).array())
                if (v.toObject().value("tag").toString() == "(5200,9230)")
                    return v.toObject().value("items").toArray();
            return QJsonArray();
        };
        int code = -1;
        QCOMPARE(run({"tags", dir.filePath("ENHANCED"), "--json"}, &code).size(), 2);
        QCOMPARE(code, 0);
        const QJsonArray second = run({"tags", dir.filePath("ENHANCED"), "--frame", "2", "--json"}, &code);
        QCOMPARE(code, 0);
        QCOMPARE(second.size(), 1);
        QVERIFY(QJsonDocument(second).toJson().contains("0 \\\\ 0 \\\\ 5"));
        run({"tags", dir.filePath("ENHANCED"), "--frame", "3", "--json"}, &code);
        QCOMPARE(code, 1);
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

    void readsStructuredReports() {
        auto item = [](const QByteArray &type, const QByteArray &name) {
            Writer w;
            w.add(0x0040A010, "CS", "CONTAINS").add(Tag::ValueType, "CS", type).sequence(Tag::ConceptName, {Writer::code("1", name)});
            return w;
        };
        Writer finding = item("TEXT", "Finding");
        finding.add(Tag::TextValue, "UT", "No acute abnormality.");
        Writer diameter = item("NUM", "Diameter");
        Writer measured;
        measured.add(Tag::NumericValue, "DS", "12.5").sequence(Tag::MeasurementUnits, {Writer::code("mm", "millimeter")});
        diameter.sequence(Tag::MeasuredValue, {measured});
        Writer findings = item("CONTAINER", "Findings");
        findings.sequence(Tag::ContentSequence, {finding, diameter});
        Writer impression = item("CODE", "Impression");
        impression.sequence(Tag::ConceptCode, {Writer::code("2", "Normal")});
        Writer image = item("IMAGE", "Key image");

        Writer w;
        w.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.88.33")
            .add(Tag::ContentDate, "DA", "20260301")
            .add(Tag::StudyInstance, "UI", "1.2.9")
            .add(Tag::ValueType, "CS", "CONTAINER")
            .sequence(Tag::ConceptName, {Writer::code("3", "Imaging Report")})
            .sequence(Tag::ContentSequence, {findings, impression, image});
        const DicomFile file = DicomFile::parse(w.file(ExplicitLE));
        QVERIFY(file.isValid());
        Report report;
        QVERIFY(Reports::describe(file, &report));
        QCOMPARE(report.kind, Report::Kind::Structured);
        QCOMPARE(report.title, QString("Imaging Report"));
        QCOMPARE(report.date, QString("20260301"));
        QCOMPARE(Reports::text(Reports::lines(file)),
                 QString("Findings\n  Finding: No acute abnormality.\n  Diameter: 12.5 mm\nImpression: Normal"));
    }

    void scansKeepReportsAndOpenPdfs() {
        QTemporaryDir dir;
        Writer image;
        image.image(2, 2, 16, 12, false).add(Tag::StudyInstance, "UI", "1.2.9").pixels(le16({0, 1, 2, 3}));
        QVERIFY(write(dir.filePath("IM1"), image.file(ExplicitLE)));
        // An odd-length PDF, padded to even with a NUL that isn't the PDF's.
        const QByteArray pdf = "%PDF-1.4\n%%EOF\n";
        Writer document;
        document.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.104.1")
            .add(Tag::StudyInstance, "UI", "1.2.9")
            .add(Tag::DocumentTitle, "ST", "Radiology report")
            .add(Tag::DocumentMimeType, "LO", "application/pdf")
            .add(Tag::EncapsulatedDocument, "OB", pdf + "x");
        QVERIFY(write(dir.filePath("DOC1"), document.file(ExplicitLE)));

        Backend backend(nullptr);
        QUrl opened;
        backend.setUrlOpener([&](const QUrl &url) { opened = url; return true; });
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.scan().reports.size(), 1);
        QCOMPARE(backend.reportList().first().toMap().value("title").toString(), QString("Radiology report"));
        QVERIFY(backend.reportList().first().toMap().value("current").toBool());

        // P shows it in the panel; it opens in a PDF viewer only when asked.
        backend.openStudyReport();
        QCOMPARE(backend.reportIndex(), 0);
        QVERIFY(backend.reportIsPdf());
        QVERIFY(opened.isEmpty());
        backend.openReport(0);
        QVERIFY(opened.isLocalFile());
        QFile copy(opened.toLocalFile());
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), pdf + "x");
        QCOMPARE(int(QFileInfo(QFileInfo(copy).absolutePath()).permissions() & (QFile::ReadOther | QFile::ReadGroup)), 0);
        copy.remove();
        backend.closeReport();
        QCOMPARE(backend.reportIndex(), -1);
    }

    void commandLineReadsReports() {
        // The app binary sits beside the tests in the build folder.
        const QString app = QCoreApplication::applicationDirPath() + "/dicolens";
        QVERIFY2(QFileInfo::exists(app), qPrintable(app));
        QTemporaryDir dir;
        Writer finding;
        finding.add(Tag::ValueType, "CS", "TEXT").sequence(Tag::ConceptName, {Writer::code("1", "Finding")})
            .add(Tag::TextValue, "UT", "Clear.");
        Writer sr;
        sr.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.88.11")
            .add(Tag::ValueType, "CS", "CONTAINER")
            .sequence(Tag::ConceptName, {Writer::code("2", "Chest Report")})
            .sequence(Tag::ContentSequence, {finding});
        QVERIFY(write(dir.filePath("SR1"), sr.file(ExplicitLE)));
        Writer pdf;
        pdf.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.104.1")
            .add(Tag::StudyDate, "DA", "20990101")
            .add(Tag::DocumentTitle, "ST", "Letter")
            .add(Tag::EncapsulatedDocument, "OB", "%PDF-1.4\n%%EOF\n");
        QVERIFY(write(dir.filePath("DOC1"), pdf.file(ExplicitLE)));

        auto run = [&](const QStringList &args, int *code = nullptr) {
            QProcess p;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("QT_QPA_PLATFORM", "offscreen");
            p.setProcessEnvironment(env);
            p.start(app, args);
            p.waitForFinished(20000);
            if (code)
                *code = p.exitCode();
            return QJsonDocument::fromJson(p.readAllStandardOutput()).object();
        };
        int code = -1;
        const QJsonObject first = run({"report", dir.path(), "--json"}, &code);
        QCOMPARE(code, 0);
        QCOMPARE(first.value("title").toString(), QString("Chest Report"));
        QCOMPARE(first.value("content").toArray().first().toObject().value("value").toString(), QString("Clear."));

        run({"report", dir.path(), "--report", "2", "--json"}, &code);
        QCOMPARE(code, 1);   // A PDF needs somewhere to go.
        const QJsonObject saved = run({"report", dir.path(), "--report", "2", "--out", dir.filePath("letter.pdf"), "--json"}, &code);
        QCOMPARE(code, 0);
        QFile letter(saved.value("saved").toString());
        QVERIFY(letter.open(QIODevice::ReadOnly));
        QCOMPARE(letter.readAll(), QByteArray("%PDF-1.4\n%%EOF\n"));
    }

    void readsOverlayPlanes() {
        // A 4 by 4 image with an overlay in group 6002 marking its diagonal,
        // and one starting a row down and a column across in 6000.
        QTemporaryDir dir;
        auto bits = [](const QList<int> &set, int size) {
            QByteArray out((size + 15) / 16 * 2, 0);
            for (int i : set)
                out[i / 8] = char(out[i / 8] | (1 << (i % 8)));
            return out;
        };
        Writer w;
        w.image(4, 4, 16, 12, false)
            .us(0x60000010, 2).us(0x60000011, 2).add(0x60000050, "SS", QByteArray("\x02\x00\x02\x00", 4))
            .us(0x60000100, 1).add(0x60003000, "OW", bits({3}, 4))
            .us(0x60020010, 4).us(0x60020011, 4).add(0x60020050, "SS", QByteArray("\x01\x00\x01\x00", 4))
            .us(0x60020100, 1).add(0x60023000, "OW", bits({0, 5, 10, 15}, 16))
            .pixels(le16(QList<int>(16, 0)));
        QVERIFY(write(dir.filePath("IM1"), w.file(ExplicitLE)));
        const DicomFile file = DicomFile::read(dir.filePath("IM1"));
        QCOMPARE(Dictionary::name(0x60023000), QString("Overlay Data"));
        const QImage mask = Marks::overlay(file, 0, 4, 4);
        QVERIFY(!mask.isNull());
        QList<QPoint> set;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                if (mask.pixelColor(x, y).alpha())
                    set << QPoint(x, y);
        QCOMPARE(set, (QList<QPoint>{{0, 0}, {1, 1}, {2, 2}, {3, 3}}));

        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        QVERIFY(!backend.overlay().isNull());
        QVERIFY(backend.hasMarks());
        backend.toggleMarks();
        QVERIFY(!backend.marksShown());
    }

    void survivesFilesThatLie() {
        // Sizes a file gives are its own word, and a damaged or hostile one
        // may say anything: none of it is read past what's there.
        QTemporaryDir dir;
        // An overlay "two thousand million" rows and columns big, of 16 bytes.
        Writer w;
        w.image(4, 4, 16, 12, false)
            .add(Tag::NumberOfFrames, "IS", "8")
            .ul(0x60000010, 0x7FFFFFFF).ul(0x60000011, 0x7FFFFFFF).add(0x60000015, "IS", "8")
            .us(0x60000100, 1).add(0x60003000, "OW", QByteArray(16, char(0xFF)))
            .pixels(le16(QList<int>(16 * 8, 0)));
        const DicomFile overlaid = DicomFile::parse(w.file(ExplicitLE));
        QVERIFY(overlaid.isValid());
        for (int frame : {0, 3, 7})
            QVERIFY(Marks::overlay(overlaid, frame, 4, 4).isNull());

        // A lossless JPEG that ends in the middle of its frame header, and
        // one whose header promises far more than its few bytes hold.
        const QByteArray whole = losslessJpeg(QVector<quint16>(20, 7), 5, 4, 1, 16, 1, 0);
        LosslessJpeg out;
        QString error;
        QVERIFY(decodeLosslessJpeg(whole, out, &error));
        const qsizetype frame = whole.indexOf(QByteArray("\xFF\xC3", 2));
        QVERIFY(frame > 0);
        QByteArray cut = whole.left(frame + 2) + QByteArray("\x00\x02", 2);
        QVERIFY(!decodeLosslessJpeg(cut, out, &error));
        QByteArray huge = whole;
        huge.replace(frame + 5, 4, QByteArray("\xFF\xFF\xFF\xFF", 4));   // 65535 by 65535.
        QVERIFY(!decodeLosslessJpeg(huge, out, &error));

        // A position that isn't a number places nothing: the series still
        // opens, as a plain stack that can't be cut another way.
        for (int k = 0; k < 4; ++k) {
            Writer slice;
            slice.add(Tag::Modality, "CS", "CT")
                .image(2, 2, 16, 12, false)
                .add(Tag::InstanceNumber, "IS", QByteArray::number(k + 1))
                .add(Tag::ImagePosition, "DS", k == 2 ? QByteArray("nan\\0\\inf") : "0\\0\\" + QByteArray::number(k))
                .add(Tag::ImageOrientation, "DS", "1\\0\\0\\0\\1\\0")
                .add(Tag::PixelSpacing, "DS", "1\\1")
                .pixels(le16({1, 2, 3, 4}));
            QVERIFY(write(dir.filePath(QString("IM%1").arg(k)), slice.file(ExplicitLE)));
        }
        const Scan scan = scanFiles(listFiles({dir.path()}, true));
        QCOMPARE(scan.series.size(), 1);
        QCOMPARE(scan.series.first().slices.size(), 4);
        QVERIFY(!makeVolume(scan.series.first()).valid);
    }

    void leavesATiltedGantryUncut() {
        // A CT taken with its gantry tilted steps sideways from slice to
        // slice. Cut as a block it would come out sheared, so it isn't cut.
        for (double aside : {0.0, 1.5}) {
            QTemporaryDir dir;
            for (int k = 0; k < 5; ++k) {
                Writer w;
                w.add(Tag::Modality, "CS", "CT")
                    .image(3, 4, 16, 12, false)
                    .add(Tag::InstanceNumber, "IS", QByteArray::number(k + 1))
                    .add(Tag::ImagePosition, "DS", "0\\" + QByteArray::number(k * aside) + "\\" + QByteArray::number(k * 3))
                    .add(Tag::ImageOrientation, "DS", "1\\0\\0\\0\\1\\0")
                    .add(Tag::PixelSpacing, "DS", "1\\1")
                    .pixels(le16(QList<int>(12, k)));
                QVERIFY(write(dir.filePath(QString("IM%1").arg(k)), w.file(ExplicitLE)));
            }
            const Scan scan = scanFiles(listFiles({dir.path()}, true));
            QCOMPARE(scan.series.size(), 1);
            QCOMPARE(makeVolume(scan.series.first()).valid, aside == 0.0);
        }
    }

    void keepsHalfAMeasurementToItsSlice() {
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        SliceView view;
        view.setSize({400, 300});
        view.setBackend(&backend);
        auto at = [&](double x, double y) { return view.toItem(x, y); };
        auto line = [&](QPointF a, QPointF b) {
            view.press(a.x(), a.y());
            view.drag(b.x(), b.y());
            view.release(b.x(), b.y());
        };
        // A Cobb angle's first line, then another slice: the half is
        // dropped, and the next two lines there make a whole angle.
        view.setTool(4);
        line(at(0, 0), at(3, 0));
        QVERIFY(view.drawing());
        backend.setSliceIndex(2);
        QVERIFY(!view.drawing());
        line(at(0, 0), at(3, 0));
        line(at(0, 2), at(2, 0));
        QCOMPARE(view.measureCount(), 1);
        QCOMPARE(view.measureTexts(), QStringList{"Cobb 45.0°"});

        // What's measured on a tilted plane belongs to that cut: tilted
        // further, the picture is of somewhere else, and doesn't show it.
        backend.tilt(0, 20);
        QVERIFY(backend.waitForSlice(10000));
        view.setTool(1);
        const int slice = backend.sliceIndex();
        line(at(1, 1), at(3, 1));
        QCOMPARE(view.measureCount(), 1);
        backend.tilt(0, 20);
        QVERIFY(backend.waitForSlice(10000));
        backend.setSliceIndex(slice);
        QCOMPARE(view.measureCount(), 0);
    }

    void readsPresentationStateMarks() {
        QTemporaryDir dir;
        for (int i = 1; i <= 2; ++i) {
            Writer w;
            w.add(Tag::SopInstance, "UI", "1.2.5." + QByteArray::number(i))
                .add(Tag::InstanceNumber, "IS", QByteArray::number(i))
                .image(10, 10, 16, 12, false)
                .pixels(le16(QList<int>(100, 0)));
            QVERIFY(write(dir.filePath(QString("IM%1").arg(i)), w.file(ExplicitLE)));
        }
        Writer reference;
        reference.add(0x00081150, "UI", "1.2.840.10008.5.1.4.1.1.2").add(Tag::ReferencedSopInstance, "UI", "1.2.5.2");
        Writer series;
        series.add(0x0020000E, "UI", "1.2.3.4").sequence(Tag::ReferencedImage, {reference});
        Writer line;
        line.add(Tag::GraphicUnits, "CS", "PIXEL").us(0x00700020, 2).us(0x00700021, 2)
            .floats(Tag::GraphicData, {1, 1, 8, 8}).add(Tag::GraphicType, "CS", "POLYLINE").add(Tag::GraphicFilled, "CS", "N");
        Writer circle;
        circle.add(Tag::GraphicUnits, "CS", "PIXEL").floats(Tag::GraphicData, {5, 5, 7, 5}).add(Tag::GraphicType, "CS", "CIRCLE");
        Writer note;
        note.add(Tag::AnchorPointUnits, "CS", "PIXEL").add(Tag::UnformattedText, "ST", "Nodule")
            .floats(Tag::AnchorPoint, {5, 5}).add(Tag::AnchorPointVisibility, "CS", "Y");
        Writer annotation;
        annotation.add(0x00700002, "CS", "LAYER1").sequence(Tag::TextObjects, {note}).sequence(Tag::GraphicObjects, {line, circle});
        Writer state;
        state.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.11.1")
            .add(Tag::Modality, "CS", "PR")
            .sequence(Tag::ReferencedSeries, {series})
            .sequence(Tag::GraphicAnnotations, {annotation});
        QVERIFY(write(dir.filePath("PR1"), state.file(ExplicitLE)));

        const Scan scan = scanFiles(listFiles({dir.path()}, true));
        QCOMPARE(scan.series.size(), 1);
        QCOMPARE(scan.reports.size(), 0);
        QCOMPARE(scan.marks.size(), 3);
        QCOMPARE(scan.marks[0].kind, Mark::Kind::Polyline);
        QCOMPARE(scan.marks[0].points, (QList<QPointF>{{1, 1}, {8, 8}}));
        QCOMPARE(scan.marks[1].kind, Mark::Kind::Circle);
        QCOMPARE(scan.marks[2].text, QString("Nodule"));
        QVERIFY(scan.marks[2].anchorLine);

        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.marks().size(), 0);
        QVERIFY(!backend.hasMarks());
        backend.step(1);
        QVERIFY(backend.waitForSlice(5000));
        QCOMPARE(backend.marks().size(), 3);
        QVERIFY(backend.hasMarks());
    }

    void measuresInUltrasoundRegions() {
        // No pixel spacing: only the region's calibration, 0.01 cm a pixel
        // across its area from 10,10 to 89,89.
        QTemporaryDir dir;
        Writer region;
        region.us(Tag::RegionSpatialFormat, 1)
            .ul(Tag::RegionMinX, 10).ul(Tag::RegionMinY, 10).ul(Tag::RegionMaxX, 89).ul(Tag::RegionMaxY, 89)
            .us(Tag::PhysicalUnitsX, 3).us(Tag::PhysicalUnitsY, 3)
            .fd(Tag::PhysicalDeltaX, 0.01).fd(Tag::PhysicalDeltaY, 0.01);
        Writer timeline;
        timeline.us(Tag::RegionSpatialFormat, 3)
            .ul(Tag::RegionMinX, 0).ul(Tag::RegionMinY, 90).ul(Tag::RegionMaxX, 99).ul(Tag::RegionMaxY, 99)
            .us(Tag::PhysicalUnitsX, 4).us(Tag::PhysicalUnitsY, 7)
            .fd(Tag::PhysicalDeltaX, 0.01).fd(Tag::PhysicalDeltaY, 0.5);
        Writer w;
        w.add(Tag::Modality, "CS", "US").image(100, 100, 8, 8, false).sequence(Tag::UltrasoundRegions, {region, timeline})
            .pixels(QByteArray(100 * 100, 0), "OB");
        QVERIFY(write(dir.filePath("US1"), w.file(ExplicitLE)));

        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.scan().series.first().regions.size(), 1);
        QCOMPARE(backend.distance(20, 20, 50, 60), QString("5.0 mm"));
        QCOMPARE(backend.distance(20, 20, 20, 95), QString("75 px"));
    }

    void turnsAndFlips() {
        QTemporaryDir dir;
        Writer w;
        w.image(2, 4, 16, 12, false).add(Tag::ImageOrientation, "DS", "1\\0\\0\\0\\1\\0").pixels(le16(QList<int>(8, 0)));
        QVERIFY(write(dir.filePath("IM1"), w.file(ExplicitLE)));
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.edges(), (QStringList{"R", "L", "A", "P"}));
        backend.rotate(1);
        QCOMPARE(backend.edges(), (QStringList{"P", "A", "R", "L"}));
        backend.rotate(-1);
        backend.flipVertical();
        QCOMPARE(backend.edges(), (QStringList{"R", "L", "P", "A"}));
        backend.flipVertical();
        backend.flipHorizontal();
        QCOMPARE(backend.edges(), (QStringList{"L", "R", "A", "P"}));
        // Turning a flipped picture turns what's seen, the same way.
        backend.rotate(1);
        QCOMPARE(backend.edges(), (QStringList{"P", "A", "L", "R"}));
        backend.resetOrientation();
        QCOMPARE(backend.edges(), (QStringList{"R", "L", "A", "P"}));

        // The view, a quarter turn round: 4 wide and 2 tall becomes 2 by 4,
        // and points map back to the pixels they came from.
        SliceView view;
        view.setSize({400, 400});
        view.setBackend(&backend);
        QCOMPARE(view.imageRect().size(), QSizeF(400, 200));
        backend.rotate(1);
        QCOMPARE(view.imageRect().size(), QSizeF(200, 400));
        const QPointF item = view.toItem(0.5, 0.5);
        QVERIFY(item.x() > 200 && item.y() < 200);   // The top left pixel is now at the top right.
        const QPointF back = view.toImage(item.x(), item.y());
        QVERIFY(std::abs(back.x() - 0.5) < 1e-9 && std::abs(back.y() - 0.5) < 1e-9);
    }

    void comparesSideBySide() {
        // Two axial series of the same body: the second pane follows the
        // first by where its slices are, not by their numbers.
        QTemporaryDir dir;
        for (int series = 0; series < 2; ++series)
            for (int i = 0; i < (series ? 10 : 5); ++i) {
                const double z = series ? i * 2.5 : i * 5.0;
                Writer w;
                w.image(2, 2, 16, 12, false)
                    .add(0x0020000E, "UI", series ? "1.2.3.2" : "1.2.3.1")
                    .add(Tag::SeriesNumber, "IS", QByteArray::number(series + 1))
                    .add(Tag::InstanceNumber, "IS", QByteArray::number(i + 1))
                    .add(Tag::ImagePosition, "DS", "0\\0\\" + QByteArray::number(z))
                    .add(Tag::ImageOrientation, "DS", "1\\0\\0\\0\\1\\0")
                    .pixels(le16({0, 1, 2, 3}));
                QVERIFY(write(dir.filePath(QString("S%1-%2").arg(series).arg(i)), w.file(ExplicitLE)));
            }
        Backend left(nullptr), right(nullptr);
        left.loadPaths({dir.path()});
        QVERIFY(left.waitForSlice(10000));
        right.share(&left);
        QVERIFY(right.waitForSlice(10000));
        QCOMPARE(right.seriesIndex(), 1);
        QCOMPARE(right.sliceCount(), 10);

        auto z = [](const Backend &b) { return b.scan().series[b.seriesIndex()].slices[b.sliceIndex()].position[2]; };
        left.setSliceIndex(3);
        right.follow(&left);
        QCOMPARE(z(right), z(left));
        const int followed = right.sliceIndex();
        // Sharing the same scan again keeps what it shows.
        right.share(&left);
        QCOMPARE(right.sliceIndex(), followed);

        // With an earlier study of the same kind, that's what goes beside.
        for (int i = 0; i < 3; ++i) {
            Writer w;
            w.image(2, 2, 16, 12, false)
                .add(0x0020000E, "UI", "1.2.3.9")
                .add(Tag::StudyInstance, "UI", "1.2.8")
                .add(Tag::StudyDate, "DA", "20200101")
                .add(Tag::InstanceNumber, "IS", QByteArray::number(i + 1))
                .pixels(le16({0, 1, 2, 3}));
            QVERIFY(write(dir.filePath(QString("P%1").arg(i)), w.file(ExplicitLE)));
        }
        Backend first(nullptr), beside(nullptr);
        first.loadPaths({dir.path()});
        QVERIFY(first.waitForSlice(10000));
        beside.share(&first);
        QCOMPARE(beside.scan().series[beside.seriesIndex()].studyUid, QString("1.2.8"));
    }

    void cutsTheSeriesOtherWays() {
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        QVERIFY(backend.canReformat());
        QCOMPARE(backend.planeName(), QString("Axial"));
        QCOMPARE(backend.sliceCount(), 5);
        backend.setSliceIndex(2);

        // Coronal, through the middle row: head at the top, the patient's
        // right on the left, a pixel 1 mm across and 3 mm down.
        backend.nextPlane();
        QCOMPARE(backend.planeName(), QString("Coronal"));
        QCOMPARE(backend.sliceCount(), 3);
        QCOMPARE(backend.sliceIndex(), 1);
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.imageWidth(), 4);
        QCOMPARE(backend.imageHeight(), 5);
        QCOMPARE(backend.edges(), (QStringList{"R", "L", "H", "F"}));
        QCOMPARE(shownNumber(backend.valueAt(0.5, 0.5)), 410.0);
        QCOMPARE(shownNumber(backend.valueAt(3.5, 4.5)), 13.0);
        QCOMPARE(backend.pixelAspect(), 1.0 / 3);
        QCOMPARE(backend.distance(0, 0, 0, 2), QString("6.0 mm"));
        QVERIFY(backend.overlay().isNull() && backend.marks().isEmpty());

        // Sagittal, through the middle column: the front on the left.
        backend.nextPlane();
        QCOMPARE(backend.planeName(), QString("Sagittal"));
        QCOMPARE(backend.sliceCount(), 4);
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.imageWidth(), 3);
        QCOMPARE(backend.edges(), (QStringList{"A", "P", "H", "F"}));
        QCOMPARE(shownNumber(backend.valueAt(2.5, 0.5)), 422.0);

        // Back as scanned, at the slice the planes met at.
        backend.nextPlane();
        QCOMPARE(backend.planeName(), QString("Axial"));
        QCOMPARE(backend.sliceIndex(), 2);
        QVERIFY(backend.waitForSlice(10000));

        // A 10 mm slab is two slices either side: its brightest, darkest,
        // and average value at the top left.
        backend.nextSlab();
        QCOMPARE(backend.slabName(), QString("MIP 10 mm"));
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(shownNumber(backend.valueAt(0.5, 0.5)), 400.0);
        backend.nextSlab();
        QCOMPARE(shownNumber(backend.valueAt(0.5, 0.5)), 0.0);
        backend.nextSlab();
        QCOMPARE(shownNumber(backend.valueAt(1.5, 0.5)), 201.0);
        backend.nextSlab();
        QCOMPARE(backend.slabName(), QString());
        QCOMPARE(shownNumber(backend.valueAt(1.5, 0.5)), 201.0);
    }

    void drawsReferenceLines() {
        // Side by side, the axial slice shows where the coronal plane cuts
        // it: along its middle row.
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        Backend axial(nullptr), coronal(nullptr);
        axial.loadPaths({dir.path()});
        QVERIFY(axial.waitForSlice(10000));
        coronal.loadPaths({dir.path()});
        QVERIFY(coronal.waitForSlice(10000));
        coronal.nextPlane();
        QVERIFY(coronal.waitForSlice(10000));
        SliceView view;
        view.setSize({400, 300});
        view.setBackend(&axial);
        view.setReference(&coronal);
        QPointF a, b;
        QVERIFY(view.referenceLine(a, b));
        QCOMPARE(a.y(), 1.0);
        QCOMPARE(b.y(), 1.0);
        QCOMPARE(std::abs(b.x() - a.x()), 4.0);
        // Two slices of one stack never meet.
        view.setReference(&axial);
        QVERIFY(!view.referenceLine(a, b));

        // Following goes by place: the coronal side moves to the row the
        // axial one is on, and back.
        coronal.setSliceIndex(2);
        axial.setPlane(1);
        axial.follow(&coronal);
        QCOMPARE(axial.sliceIndex(), 2);
    }

    void setsWhereThePlanesMeetAndTiltsThem() {
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        backend.setSliceIndex(2);

        // A click on column 1, row 2 of the middle slice: the coronal and
        // sagittal planes go through it.
        const QVariantList point = backend.pointAt(1, 2);
        QCOMPARE(point.size(), 4);
        QCOMPARE(point[2].toDouble(), 6.0);
        QVERIFY(backend.goThrough(point));
        QPointF at;
        QVERIFY(backend.crossing(at));
        QCOMPARE(at, QPointF(1, 2));
        backend.setPlane(1);
        QCOMPARE(backend.sliceIndex(), 2);
        QVERIFY(backend.crossing(at));
        QCOMPARE(at.y(), 2.0);   // Slice 2, at 3 mm a row.
        backend.setPlane(2);
        QCOMPARE(backend.sliceIndex(), 1);
        backend.setPlane(0);
        QCOMPARE(backend.sliceIndex(), 2);
        // Another frame of reference is somewhere else.
        QVERIFY(!backend.goThrough({0.0, 0.0, 0.0, QString("1.2.3")}));

        // Not tilted at all, an oblique plane is the slice itself.
        backend.tilt(0, 0);
        QCOMPARE(backend.planeName(), QString("Axial"));
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.imageWidth(), 4);
        QCOMPARE(backend.imageHeight(), 3);
        for (const QPoint p : {QPoint(0, 0), QPoint(3, 2), QPoint(1, 1)})
            QVERIFY2(std::abs(shownNumber(backend.valueAt(p.x() + 0.5, p.y() + 0.5)) - (200 + 10 * p.y() + p.x())) < 0.05,
                     qPrintable(backend.valueAt(p.x() + 0.5, p.y() + 0.5)));
        // A step is a slice.
        backend.step(1);
        QVERIFY(backend.waitForSlice(10000));
        QVERIFY(std::abs(shownNumber(backend.valueAt(1.5, 1.5)) - 311) < 0.05);
        backend.step(-1);

        // 30° about the up-down axis, through where the planes meet.
        backend.tilt(0, 30);
        QCOMPARE(backend.planeName(), QString("Oblique"));
        QVERIFY(backend.waitForSlice(10000));
        QVERIFY(backend.frameReady());
        double n[3];
        backend.imagePlane().normal(n);
        QVERIFY(std::abs(std::abs(n[2]) - std::cos(M_PI / 6)) < 1e-6);
        QVERIFY(backend.crossing(at));
        // Slabs work on it too.
        backend.nextSlab();
        QVERIFY(backend.waitForSlice(10000));
        QVERIFY(backend.frameReady());
        backend.nextSlab();
        backend.nextSlab();
        backend.nextSlab();

        // 60° more is a quarter turn: sagittal.
        backend.tilt(0, 60);
        QCOMPARE(backend.planeName(), QString("Sagittal"));
        QVERIFY(backend.waitForSlice(10000));
        // Shown as a sagittal is, not on its side: the front on the left
        // and the head up.
        QCOMPARE(backend.edges(), (QStringList{"A", "P", "H", "F"}));
        // Tipped forward a quarter turn, the axial slice is coronal, the
        // head up rather than down.
        backend.nextPlane();
        backend.tilt(1, 90);
        QCOMPARE(backend.planeName(), QString("Coronal"));
        QCOMPARE(backend.edges(), (QStringList{"R", "L", "H", "F"}));
        backend.tilt(1, -90);
        QCOMPARE(backend.edges(), (QStringList{"R", "L", "A", "P"}));
        // V goes back to the slices as scanned.
        backend.nextPlane();
        QCOMPARE(backend.planeName(), QString("Axial"));

        // Dragging a reference line turns the other side's plane about
        // the first's normal: a coronal plane a quarter turn about the
        // body's long axis is sagittal.
        backend.setPlane(1);
        backend.beginTurn();
        const double z[3] = {0, 0, 1};
        backend.turnPlane(z, M_PI / 2);
        QCOMPARE(backend.planeName(), QString("Sagittal"));
        backend.turnPlane(z, M_PI / 4);
        QCOMPARE(backend.planeName(), QString("Oblique"));
        QVERIFY(backend.waitForSlice(10000));

        // A series that can't be cut says so.
        QTemporaryDir flat;
        Writer w;
        w.add(Tag::Modality, "CS", "CT").image(2, 2, 16, 12, false).pixels(le16({1, 2, 3, 4}));
        QVERIFY(write(flat.filePath("one"), w.file(ExplicitLE)));
        Backend single(nullptr);
        single.loadPaths({flat.path()});
        QVERIFY(single.waitForSlice(10000));
        single.tilt(0, 10);
        QCOMPARE(single.notice(), QString("This series can't be cut another way"));
    }

    void rendersAVolumeIn3d() {
        // A bone ball, 8 mm across, in the middle of 24 mm of air.
        Volume volume;
        volume.valid = true;
        for (int a = 0; a < 3; ++a)
            volume.size[a] = 24;
        QVector<Frame> slices;
        for (int k = 0; k < 24; ++k) {
            Frame f;
            f.width = f.height = 24;
            f.intercept = -1024;
            for (int j = 0; j < 24; ++j)
                for (int i = 0; i < 24; ++i)
                    f.mono << quint16(std::hypot(i - 11.5, j - 11.5, k - 11.5) < 4 ? 1024 + 900 : 24);
            slices << f;
        }
        const VoxelGrid grid = packVolume(volume, slices);
        QVERIFY(grid.valid);
        QVERIFY(std::abs(grid.low + 1000) < 1e-6);
        QVERIFY(std::abs(grid.high - 900) < 1e-6);
        QVERIFY(std::abs(grid.center[0] - 11.5) < 1e-9 && std::abs(grid.center[2] - 11.5) < 1e-9);
        double value;
        const double middle[3] = {11.5, 11.5, 11.5}, outside[3] = {30, 0, 0};
        QVERIFY(sampleGrid(grid, middle, value));
        QVERIFY(std::abs(value - 900) < 1);
        QVERIFY(!sampleGrid(grid, outside, value));

        // From the front, bone in the middle and nothing round it.
        View3D view;
        const QSize size(64, 64);
        QImage picture = renderVolume(grid, view, size);
        QCOMPARE(picture.size(), size);
        QVERIFY(qGray(picture.pixel(32, 32)) > 120);
        QCOMPARE(picture.pixel(2, 2), qRgb(0, 0, 0));
        // Lit from the front, the middle of the ball is brighter than its edge.
        QVERIFY(qGray(picture.pixel(32, 32)) > qGray(picture.pixel(32 + 9, 32)));
        // A MIP through it, in grey through a window.
        view.look = Look::Mip;
        view.center = 0;
        view.width = 1800;
        picture = renderVolume(grid, view, size);
        QVERIFY(qGray(picture.pixel(32, 32)) > 240);
        QVERIFY(qGray(picture.pixel(10, 32)) < 140);
        // Cut away past the ball, nothing's left in front of the back.
        view.look = Look::Bone;
        view.cut = grid.radius + 6;
        QCOMPARE(renderVolume(grid, view, size).pixel(32, 32), qRgb(0, 0, 0));
        view.cut = 0;

        // A click on it finds the ball's front, toward the viewer (-y).
        double point[3];
        QVERIFY(pickVolume(grid, view, size, QPointF(32, 32), point));
        QVERIFY2(std::abs(point[1] - 7.5) < 1.5, qPrintable(QString::number(point[1])));
        QVERIFY(std::abs(point[0] - 11.5) < 1 && std::abs(point[2] - 11.5) < 1);
        QVERIFY(!pickVolume(grid, view, size, QPointF(1, 1), point));
        // And the centre is shown in the middle, wherever it's turned.
        QPointF at = projectPoint(grid, view, size, grid.center);
        QVERIFY(std::abs(at.x() - 32) < 1e-6 && std::abs(at.y() - 32) < 1e-6);
        // The patient's left is on the right, and the head is up.
        const double left[3] = {21.5, 11.5, 11.5}, head[3] = {11.5, 11.5, 21.5};
        QVERIFY(projectPoint(grid, view, size, left).x() > 32);
        QVERIFY(projectPoint(grid, view, size, head).y() < 32);

        // Turning keeps the view square, and a quarter turn shows the side.
        view.turn(M_PI / 2, 0);
        const double *r = view.right, *u = view.up, *t = view.toward;
        QVERIFY(std::abs(r[0] * u[0] + r[1] * u[1] + r[2] * u[2]) < 1e-9);
        QVERIFY(std::abs(r[0] * t[0] + r[1] * t[1] + r[2] * t[2]) < 1e-9);
        QVERIFY(std::abs(std::abs(view.toward[0]) - 1) < 1e-9);
        view.face(4);
        QCOMPARE(view.toward[2], 1.0);

        // Half the voxels each way, fitted the same.
        const VoxelGrid half = halve(grid);
        QVERIFY(half.valid);
        QCOMPARE(half.volume.size[0], 12);
        QCOMPARE(half.volume.spacing[2], 2.0);
        QCOMPARE(half.radius, grid.radius);
        QVERIFY(std::abs(half.volume.origin[0] - 0.5) < 1e-9);
        view.face(0);
        QVERIFY(qGray(renderVolume(half, view, size).pixel(32, 32)) > 120);

        // A colour slice can't be packed.
        slices[3].color = QImage(24, 24, QImage::Format_RGB32);
        QVERIFY(!packVolume(volume, slices).valid);
    }

    void showsTheSeriesIn3d() {
        QTemporaryDir dir;
        QVERIFY(writeSeries(dir.path(), "CT", "1.2.77.1", "1.2.77.100", 16, 16, 1, 1,
                            [](int i, int j, int k) { return std::hypot(i - 7.5, j - 7.5, k - 7.5) < 4 ? 1024 + 900 : 24; }));
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        VolumeView view;
        view.setSize({200, 200});
        view.setBackend(&backend);
        view.setActive(true);
        QVERIFY(view.waitForPicture(10000));
        QVERIFY(view.ready());
        QCOMPARE(view.status(), QString());
        QCOMPARE(view.lookName(), QString("Bone"));
        QCOMPARE(view.edges(), (QStringList{"R", "L", "H", "F"}));
        QImage picture = view.picture();
        QVERIFY(!picture.isNull());
        QVERIFY(qGray(picture.pixel(picture.width() / 2, picture.height() / 2)) > 120);
        // Looks in turn, a MIP following the window.
        for (const char *name : {"Vessels", "Skin", "Lungs", "MIP", "Bone"}) {
            view.nextLook();
            QCOMPARE(view.lookName(), QString(name));
        }
        // A click on the ball sets where the planes meet, in its front.
        const QVariantList point = view.pointAt(100, 100);
        QCOMPARE(point.size(), 4);
        QCOMPARE(point[3].toString(), QString("1.2.77.100"));
        QVERIFY(backend.goThrough(point));
        QVERIFY(backend.crossingShown());
        QVERIFY(std::abs(point[1].toDouble() - 3.5) < 1.5);
        // Cut in from the front, and turned to the side and back.
        view.cutDeeper(5);
        QCOMPARE(view.cut(), 5.0);
        view.setCut(-3);
        QCOMPARE(view.cut(), 0.0);
        view.nextFace();
        QCOMPARE(view.edges(), (QStringList{"A", "P", "H", "F"}));
        view.reset();
        QCOMPARE(view.edges(), (QStringList{"R", "L", "H", "F"}));
        view.toggleSpin();
        QVERIFY(view.spinning());
        view.stopSpin();
        QVERIFY(!view.spinning());
        QVERIFY(view.waitForPicture(10000));

        // A single picture can't be shown in 3D.
        QTemporaryDir flat;
        QVERIFY(writeSeries(flat.path(), "CT", "1.2.77.2", "1.2.77.100", 4, 1, 1, 1, [](int, int, int) { return 0; }));
        Backend other(nullptr);
        other.loadPaths({flat.path()});
        QVERIFY(other.waitForSlice(10000));
        view.setBackend(&other);
        QCOMPARE(view.status(), QString("This series can't be shown in 3D"));
        QVERIFY(!view.busy());
    }

    void fusesAPetOverItsCt() {
        QTemporaryDir dir;
        // A CT, brighter to the right, and a PET at half its resolution, lined up,
        // with a hot spot at (12, 12, 6) mm, less hot round it.
        QVERIFY(writeSeries(dir.path(), "CT", "1.2.77.10", "1.2.77.200", 16, 8, 1.5, 1.5,
                            [](int i, int, int) { return 1024 + 10 * i; }, 0));
        QVERIFY(writeSeries(dir.path(), "PT", "1.2.77.11", "1.2.77.200", 8, 4, 3, 3,
                            [](int i, int j, int k) {
                                const int away = std::abs(i - 4) + std::abs(j - 4) + std::abs(k - 2);
                                return away == 0 ? 1000 : away == 1 ? 500 : 20;
                            }));
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        int ct = -1, pet = -1;
        for (int i = 0; i < backend.scan().series.size(); ++i)
            (backend.scan().series[i].modality == "CT" ? ct : pet) = i;
        QVERIFY(ct >= 0 && pet >= 0);

        // From the PET, it goes over the CT, which is shown.
        backend.selectSeries(pet);
        QVERIFY(backend.waitForSlice(10000));
        QSignalSpy fusion(&backend, &Backend::fusionChanged);
        backend.toggleFusion();
        QCOMPARE(backend.seriesIndex(), ct);
        QVERIFY(backend.fused());
        QTRY_VERIFY_WITH_TIMEOUT(backend.fusionGrid() != nullptr, 10000);
        QCOMPARE(backend.fusionName(), QString("PET over CT  ·  50%"));
        backend.setSliceIndex(4);   // 6 mm up, the hot spot's.
        QVERIFY(backend.waitForSlice(10000));
        // Colour over the hot spot, grey elsewhere.
        const QImage image = backend.image();
        const QRgb hot = image.pixel(8, 8), warm = image.pixel(9, 8), cold = image.pixel(1, 14);
        QVERIFY2(qRed(hot) > qRed(cold) + 60, qPrintable(QString::number(hot, 16)));
        QVERIFY2(qRed(warm) > qBlue(warm) + 40, qPrintable(QString::number(warm, 16)));
        QCOMPARE(qRed(cold), qBlue(cold));
        QVERIFY2(backend.valueAt(8.5, 8.5).contains("PET 1,000") || backend.valueAt(8.5, 8.5).contains("PET 1000"),
                 qPrintable(backend.valueAt(8.5, 8.5)));
        // In every plane of the CT, here a coronal one.
        backend.setPlane(1);
        QVERIFY(backend.waitForSlice(10000));
        bool coloured = false;
        const QImage coronalImage = backend.image();
        for (int y = 0; y < coronalImage.height(); ++y)
            for (int x = 0; x < coronalImage.width(); ++x)
                coloured = coloured || qRed(coronalImage.pixel(x, y)) > qBlue(coronalImage.pixel(x, y)) + 40;
        QVERIFY(coloured);
        backend.setPlane(0);
        // Stronger, then off.
        backend.nextFusionOpacity();
        QCOMPARE(backend.fusionName(), QString("PET over CT  ·  70%"));
        backend.toggleFusion();
        QVERIFY(!backend.fused());
        QCOMPARE(backend.fusionName(), QString());
        QCOMPARE(backend.notice(), QString("PET off"));
        QVERIFY(fusion.count() >= 3);

        // Another series drops the PET; one with none to fuse says so.
        backend.toggleFusion();
        QVERIFY(backend.fused());
        backend.selectSeries(pet);
        QVERIFY(!backend.fused());
        QTemporaryDir lone;
        QVERIFY(writeSeries(lone.path(), "CT", "1.2.77.12", "1.2.77.300", 4, 3, 1, 1, [](int, int, int) { return 0; }));
        Backend alone(nullptr);
        alone.loadPaths({lone.path()});
        QVERIFY(alone.waitForSlice(10000));
        alone.toggleFusion();
        QVERIFY(!alone.fused());
        QCOMPARE(alone.notice(), QString("There's no PET taken with this series to put over it"));
    }

    void mirrorsTheSeriesInFourViews() {
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        QTRY_COMPARE_WITH_TIMEOUT(backend.decodedCount(), 5, 10000);
        backend.preset(3);

        // The coronal view reads nothing itself: the slices come from the first.
        Backend coronal(nullptr);
        coronal.mirror(&backend, 1);
        QCOMPARE(coronal.seriesIndex(), backend.seriesIndex());
        QCOMPARE(coronal.decodedCount(), 5);
        QCOMPARE(coronal.plane(), 1);
        QVERIFY(coronal.waitForSlice(10000));
        QCOMPARE(coronal.planeName(), QString("Coronal"));
        QCOMPARE(coronal.windowName(), QString("Lung"));
        // A window set on one is taken by the other.
        coronal.preset(4);
        backend.matchWindow(&coronal);
        QCOMPARE(backend.windowName(), QString("Bone"));
        // Where the planes meet is shared by a click.
        QVERIFY(backend.goThrough(backend.pointAt(1, 2)));
        QVERIFY(coronal.goThrough(backend.pointAt(1, 2)));
        QCOMPARE(coronal.sliceIndex(), 2);
        // Stopped, it's on its own.
        coronal.mirror(nullptr, 0);
        backend.preset(1);
        QCOMPARE(coronal.windowName(), QString("Bone"));
    }

    void answersMeasurementsAndDrawsIn3dFromTheCommandLine() {
        QTemporaryDir dir, data;
        // A CT, 10 HU brighter each pixel to the right, and its PET.
        QVERIFY(writeSeries(dir.path(), "CT", "1.2.77.10", "1.2.77.200", 16, 8, 1.5, 1.5,
                            [](int i, int, int) { return 1024 + 10 * i; }, 0));
        QVERIFY(writeSeries(dir.path(), "PT", "1.2.77.11", "1.2.77.200", 8, 4, 3, 3,
                            [](int i, int j, int k) {
                                const int away = std::abs(i - 4) + std::abs(j - 4) + std::abs(k - 2);
                                return away == 0 ? 1000 : away == 1 ? 500 : 20;
                            }));
        // Measurements as the viewer keeps them, where the command looks.
        MeasureStore::instance().setFolder(data.filePath("dicolens/measurements"));
        Measure ruler, area, cobb;
        ruler.b = QPointF(4, 0);
        area.kind = 2;
        area.b = QPointF(4, 4);
        cobb.kind = 4;
        cobb.b = QPointF(4, 0);
        cobb.d = QPointF(4, 4);
        MeasureStore::instance().add("1.2.77.10", 1, 3, cobb);   // On a coronal plane.
        MeasureStore::instance().add("1.2.77.10", 0, 2, ruler);
        MeasureStore::instance().add("1.2.77.10", 0, 2, area);

        const QString app = QCoreApplication::applicationDirPath() + "/dicolens";
        QByteArray errors;
        auto run = [&](const QStringList &args, int *code, QByteArray *output = nullptr) {
            QProcess p;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("QT_QPA_PLATFORM", "offscreen");
            env.insert("XDG_DATA_HOME", data.path());
            p.setProcessEnvironment(env);
            p.start(app, args);
            p.waitForFinished(30000);
            *code = p.exitCode();
            errors = p.readAllStandardError();
            const QByteArray text = p.readAllStandardOutput();
            if (output)
                *output = text;
            return QJsonDocument::fromJson(text).object();
        };
        int code = -1;
        int ct = 0, pet = 0;
        for (const QJsonValue &v : run({"info", dir.path(), "--json"}, &code).value("series").toArray())
            (v.toObject().value("modality").toString() == "CT" ? ct : pet) = v.toObject().value("index").toInt();
        QVERIFY(ct > 0 && pet > 0);

        // Every measurement, with what it comes to; only the CT has any.
        const QJsonObject kept = run({"measurements", dir.path(), "--json"}, &code);
        QCOMPARE(code, 0);
        const QJsonArray series = kept.value("series").toArray();
        QCOMPARE(series.size(), 1);
        QCOMPARE(series[0].toObject().value("index").toInt(), ct);
        const QJsonArray list = series[0].toObject().value("measurements").toArray();
        QCOMPARE(list.size(), 3);
        const QJsonObject first = list[0].toObject(), second = list[1].toObject(), third = list[2].toObject();
        QCOMPARE(first.value("kind").toString(), QString("ruler"));
        QCOMPARE(first.value("slice").toInt(), 3);
        QCOMPARE(first.value("plane").toString(), QString("scanned"));
        QCOMPARE(first.value("length").toDouble(), 6.0);   // 4 pixels of 1.5 mm.
        QCOMPARE(first.value("unit").toString(), QString("mm"));
        QCOMPARE(second.value("kind").toString(), QString("area"));
        QVERIFY(std::abs(second.value("area").toDouble() - M_PI * 3 * 3) < 1e-6);
        QCOMPARE(second.value("mean").toDouble(), 15.0);   // 0 to 30 HU, evenly.
        QCOMPARE(second.value("valueUnit").toString(), QString("HU"));
        QCOMPARE(third.value("kind").toString(), QString("cobb"));
        QCOMPARE(third.value("plane").toString(), QString("coronal"));
        QVERIFY(std::abs(third.value("degrees").toDouble() - 45) < 1e-6);
        QByteArray text;
        run({"measurements", dir.path()}, &code, &text);
        QCOMPARE(code, 0);
        QVERIFY2(text.contains("ruler  6.0 mm"), text.constData());
        QVERIFY2(text.contains("slice 4 (coronal)  Cobb angle  45.0°"), text.constData());
        const QJsonObject none = run({"measurements", dir.path(), "--series", QString::number(pet), "--json"}, &code);
        QCOMPARE(code, 0);
        QVERIFY(none.value("series").toArray()[0].toObject().value("measurements").toArray().isEmpty());

        // The CT in 3D, with the PET over it, from the PET's own number too.
        const QString picture = dir.filePath("3d.png");
        const QJsonObject drawn = run({"export", dir.path(), picture, "--3d", "--series", QString::number(pet),
                                       "--pet", "--from", "left", "--size", "64x48", "--json"}, &code);
        QCOMPARE(code, 0);
        QCOMPARE(drawn.value("series").toInt(), ct);
        QCOMPARE(drawn.value("pet").toInt(), pet);
        QCOMPARE(drawn.value("look").toString(), QString("bone"));
        QCOMPARE(QImage(picture).size(), QSize(64, 48));
        run({"export", dir.path(), picture, "--3d", "--look", "glass", "--json"}, &code);
        QCOMPARE(code, 1);
        QVERIFY(QJsonDocument::fromJson(errors).object().value("error").toString().contains("--look"));

        // A slice with the PET over it, 6 mm up: colour beside the hot spot
        // (whose middle is white hot), grey away from it.
        const QString slice = dir.filePath("slice.png");
        run({"export", dir.path(), slice, "--series", QString::number(ct), "--slice", "5", "--pet", "--json"}, &code);
        QCOMPARE(code, 0);
        const QImage fused(slice);
        const QColor hot = fused.pixelColor(6, 8), cold = fused.pixelColor(1, 14);
        QVERIFY(hot.red() != hot.blue());
        QCOMPARE(cold.red(), cold.blue());
        // Over the PET's own slices it can't go; it says which series to use.
        run({"export", dir.path(), slice, "--series", QString::number(pet), "--pet"}, &code);
        QCOMPARE(code, 1);
        QVERIFY(errors.contains(QStringLiteral("--series %1").arg(ct).toUtf8()));
    }

    void keepsMeasurementsWithTheSeries() {
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        {
            Backend backend(nullptr);
            backend.loadPaths({dir.path()});
            QVERIFY(backend.waitForSlice(10000));
            backend.setSliceIndex(3);
            SliceView view;
            view.setSize({400, 300});
            view.setBackend(&backend);
            view.setTool(1);
            const QPointF a = view.toItem(0, 0), b = view.toItem(3, 0);
            view.press(a.x(), a.y());
            view.drag(b.x(), b.y());
            view.release(b.x(), b.y());
            QCOMPARE(view.measureCount(), 1);
            // Another view of the series shows it too.
            SliceView other;
            other.setSize({400, 300});
            other.setBackend(&backend);
            QCOMPARE(other.measureCount(), 1);
        }
        // Saved beside nothing of the user's: in the measurements folder.
        const QDir folder(MeasureStore::instance().folder());
        QCOMPARE(folder.entryList({"*.json"}, QDir::Files).size(), 1);

        // Opened again, it's back on its slice, and only there.
        MeasureStore::instance().setFolder(folder.path());
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        SliceView view;
        view.setSize({400, 300});
        view.setBackend(&backend);
        QCOMPARE(view.measureCount(), 0);
        backend.setSliceIndex(3);
        QCOMPARE(view.measureCount(), 1);
        QCOMPARE(view.measureTexts(), QStringList{"3.0 mm"});
        // Removed, the file goes too.
        QVERIFY(view.removeLast());
        QCOMPARE(folder.entryList({"*.json"}, QDir::Files).size(), 0);
    }

    void measuresAreasAndAngles() {
        QTemporaryDir dir;
        for (int k = 0; k < 2; ++k) {
            QList<int> values(16, 100);
            values[0] = values[3] = values[12] = values[15] = 1000;   // Corners, outside the circle.
            Writer w;
            w.add(Tag::Modality, "CS", "CT")
                .image(4, 4, 16, 16, false)
                .add(Tag::InstanceNumber, "IS", QByteArray::number(k + 1))
                .add(Tag::PixelSpacing, "DS", "0.5\\0.5")
                .pixels(le16(values));
            QVERIFY(write(dir.filePath(QString("IM%1").arg(k)), w.file(ExplicitLE)));
        }
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        const QString stats = backend.areaStats(0, 0, 4, 4);
        QVERIFY2(stats.startsWith("Area 3.1 mm²"), qPrintable(stats));
        QVERIFY2(stats.contains("Mean 100.0 HU  ·  SD 0.0"), qPrintable(stats));
        QVERIFY2(stats.contains("Min 100 HU  ·  Max 100 HU"), qPrintable(stats));

        SliceView view;
        view.setSize({400, 400});
        view.setBackend(&backend);
        auto at = [&](double x, double y) { return view.toItem(x, y); };
        // A ruler two pixels long, then an area, kept on their slice.
        view.setTool(1);
        view.press(at(0, 0).x(), at(0, 0).y());
        view.drag(at(1, 0).x(), at(1, 0).y());
        view.release(at(2, 0).x(), at(2, 0).y());
        view.setTool(2);
        view.press(at(0, 0).x(), at(0, 0).y());
        view.release(at(4, 4).x(), at(4, 4).y());
        QCOMPARE(view.measureCount(), 2);
        QCOMPARE(view.measureTexts().first(), QString("1.0 mm"));
        backend.step(1);
        QCOMPARE(view.measureCount(), 0);
        backend.step(-1);
        QCOMPARE(view.measureCount(), 2);
        QVERIFY(view.removeLast());
        QCOMPARE(view.measureCount(), 1);

        // An angle: one arm dragged, the other's end clicked.
        view.setTool(3);
        view.press(at(3, 2).x(), at(3, 2).y());
        view.release(at(1, 2).x(), at(1, 2).y());
        QVERIFY(view.drawing());
        view.hover(at(1, 1).x(), at(1, 1).y());
        view.press(at(1, 0).x(), at(1, 0).y());
        QVERIFY(!view.drawing());
        QCOMPARE(view.measureTexts().last(), QString("90.0°"));

        // A Cobb angle: two lines dragged, whichever way, the angle
        // between them.
        view.setTool(4);
        view.press(at(0, 0).x(), at(0, 0).y());
        view.release(at(4, 0).x(), at(4, 0).y());
        QVERIFY(view.drawing());
        view.press(at(4, 3).x(), at(4, 3).y());
        view.drag(at(2, 2).x(), at(2, 2).y());
        view.release(at(0, 1).x(), at(0, 1).y());
        QVERIFY(!view.drawing());
        QCOMPARE(view.measureTexts().last(), QString("Cobb 26.6°"));
        view.clearMeasures();
        QCOMPARE(view.measureCount(), 0);
    }

    void readsPetUptake() {
        // 70 kg, 370 MBq of FDG at 10:00, the series corrected to 11:00.
        Writer drug;
        drug.add(0x00181072, "TM", "100000").add(0x00181074, "DS", "370000000").add(0x00181075, "DS", "6586.2");
        Writer w;
        w.add(0x00080021, "DA", "20250101")
            .add(0x00080031, "TM", "110000")
            .add(Tag::Modality, "CS", "PT")
            .add(0x00101030, "DS", "70")
            .sequence(0x00540016, {drug})
            .add(0x00541001, "CS", "BQML")
            .add(0x00541102, "CS", "START")
            .image(2, 2, 16, 16, false)
            .add(Tag::RescaleSlope, "DS", "10")
            .pixels(le16({0, 1000, 2000, 3000}));
        QString error;
        const DicomFile file = DicomFile::parse(w.file(ExplicitLE), &error);
        QVERIFY2(file.isValid(), qPrintable(error));
        const double expected = 70000 / (370e6 * std::pow(0.5, 3600 / 6586.2));
        QVERIFY(std::abs(suvFactor(file) - expected) < 1e-9);

        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("PET1"), w.file(ExplicitLE)));
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        // 30 000 Bq/ml there makes an SUV of about 4.15.
        const QString value = backend.valueAt(1.5, 1.5);
        QVERIFY2(value.endsWith(QString("%1 SUV").arg(QLocale().toString(30000 * expected, 'f', 2))), qPrintable(value));
        QVERIFY(backend.areaStats(0, 0, 2, 2).contains("SUV"));
        // PET starts in its colours, and G goes on through the others.
        QCOMPARE(backend.colorMapName(), QString("PET colours"));
        QVERIFY(!backend.image().isGrayscale());
        backend.nextColorMap();
        QCOMPARE(backend.colorMapName(), QString("Rainbow"));
        backend.nextColorMap();
        QCOMPARE(backend.colorMapName(), QString());
        QCOMPARE(backend.image().format(), QImage::Format_Grayscale8);
    }

    void anonymizesACopy() {
        QTemporaryDir dir;
        Writer reference;
        reference.add(0x00081150, "UI", "1.2.840.10008.5.1.4.1.1.2").add(0x00081155, "UI", "1.2.7.1");
        Writer w;
        w.add(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.2")
            .add(0x00080018, "UI", "1.2.7.2")
            .add(Tag::StudyDate, "DA", "20250101")
            .add(0x00080050, "SH", "ACC123")
            .add(Tag::Modality, "CS", "CT")
            .add(0x00080080, "LO", "Saint Elsewhere")
            .add(0x00080090, "PN", "House^Gregory")
            .sequence(0x00081140, {reference})
            .add(0x00090010, "LO", "VENDOR")
            .add(0x00091001, "LO", "secret")
            .add(0x00100010, "PN", "Doe^Jane")
            .add(0x00100020, "LO", "MRN42")
            .add(0x00100030, "DA", "19700101")
            .add(0x00100040, "CS", "F")
            .add(0x00101030, "DS", "70")
            .add(0x0020000D, "UI", "1.2.7")
            .image(2, 2, 16, 12, false)
            .add(0x0020000E, "UI", "1.2.7.3")
            .pixels(le16({1, 2, 3, 4}));
        QVERIFY(write(dir.filePath("IM1"), w.file(ExplicitLE)));
        // The same patient's other image, written implicitly, as older
        // systems do.
        Writer implicit;
        implicit.explicitVr = false;
        implicit.add(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.2")
            .add(0x00080018, "UI", "1.2.7.1")
            .add(Tag::Modality, "CS", "CT")
            .add(0x00100010, "PN", "Doe^Jane")
            .add(0x00100020, "LO", "MRN42")
            .add(0x0020000D, "UI", "1.2.7")
            .image(2, 2, 16, 12, false)
            .add(0x0020000E, "UI", "1.2.7.3")
            .add(Tag::InstanceNumber, "IS", "2")
            .pixels(le16({5, 6, 7, 8}));
        QVERIFY(write(dir.filePath("IM2"), implicit.file("1.2.840.10008.1.2")));

        const Scan scan = scanFiles(listFiles({dir.path()}, true));
        QTemporaryDir out;
        const Anonymize::Result result = Anonymize::copyScan(scan, out.path());
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join("\n")));
        QCOMPARE(result.written, 2);
        QCOMPARE(result.folder, out.filePath("Anonymous scans"));

        const Scan copied = scanFiles(listFiles({result.folder}, true));
        QCOMPARE(copied.series.size(), 1);
        QCOMPARE(copied.series.first().slices.size(), 2);
        const DicomFile a = DicomFile::read(copied.series.first().slices[0].path);
        const DicomFile b = DicomFile::read(copied.series.first().slices[1].path);
        for (const DicomFile *f : {&a, &b}) {
            QVERIFY(f->isValid());
            QCOMPARE(f->first(0x00100010), QString("Anonymous"));
            QCOMPARE(f->first(0x00100020), QString("ANON1"));
            QVERIFY(!f->has(0x00080080) && !f->has(0x00091001) && !f->has(0x00090010));
            QCOMPARE(f->first(0x00120062), QString("YES"));
            QCOMPARE(f->first(0x00080016), QString("1.2.840.10008.5.1.4.1.1.2"));
            QVERIFY(f->first(0x0020000D).startsWith("2.25."));
            QCOMPARE(f->first(0x0020000D), a.first(0x0020000D));
        }
        QCOMPARE(a.first(0x00080050), QString());
        QCOMPARE(a.first(0x00080090), QString());
        QCOMPARE(a.first(0x00100030), QString());
        QCOMPARE(a.first(0x00100040), QString("F"));
        QCOMPARE(a.first(Tag::StudyDate), QString("20250101"));
        // The reference to the other image still finds it, by its new UID.
        const DicomFile &withReference = a.has(0x00081140) ? a : b;
        const DicomFile &referenced = a.has(0x00081140) ? b : a;
        const DicomElement *refs = withReference.element(0x00081140);
        QVERIFY(refs && refs->items.size() == 1);
        QString uid;
        for (const auto &e : refs->items.first())
            if (e.tag == 0x00081155)
                uid = QString::fromLatin1(e.value).remove(QChar('\0')).trimmed();
        QCOMPARE(uid, referenced.first(0x00080018));
        // The pictures are untouched.
        const Frame pixels = decodeFrame(withReference, 0);
        QCOMPARE(pixels.mono, (QVector<quint16>{1, 2, 3, 4}));
        QCOMPARE(decodeFrame(referenced, 0).mono, (QVector<quint16>{5, 6, 7, 8}));
        QCOMPARE(referenced.transferSyntax(), QString("1.2.840.10008.1.2"));

        // Ultrasound may have the name in the picture, which is said.
        Writer us;
        us.add(Tag::Modality, "CS", "US");
        QVERIFY(Anonymize::mayShowDetails(DicomFile::parse(us.file(ExplicitLE))));
        QVERIFY(!Anonymize::mayShowDetails(a));

        // And from the command line, with what it did as JSON.
        const QString app = QCoreApplication::applicationDirPath() + "/dicolens";
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "offscreen");
        p.setProcessEnvironment(env);
        p.start(app, {"anonymize", dir.path(), "--out", out.path(), "--json"});
        QVERIFY(p.waitForFinished(20000));
        QCOMPARE(p.exitCode(), 0);
        const QJsonObject json = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
        QCOMPARE(json.value("written").toInt(), 2);
        QCOMPARE(json.value("folder").toString(), QFileInfo(out.filePath("Anonymous scans 2")).absoluteFilePath());
    }

    void anonymizesWhatImplicitFilesDontName() {
        // An implicit file says no value's kind, so a name, a UID, or a
        // sequence under a tag the bundled list doesn't have arrives as
        // bytes. None of the patient may leave in them.
        QTemporaryDir dir;
        Writer was, modified, original;
        was.explicitVr = modified.explicitVr = original.explicitVr = false;
        was.add(0x00100010, "PN", "Doe^Jane").add(0x00100020, "LO", "MRN42");
        modified.sequence(0x04000550, {was});
        // A sequence the list doesn't have, holding a name it does.
        Writer unlisted;
        unlisted.explicitVr = false;
        unlisted.add(0x00100010, "PN", "Poe^Jane");
        QVERIFY(Dictionary::vr(0x00082228).isEmpty());
        Writer w;
        w.explicitVr = false;
        w.add(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.2")
            .add(0x00080018, "UI", "1.2.7.1")
            .add(Tag::Modality, "CS", "CT")
            .add(0x0008009C, "PN", "House^Gregory")
            .sequence(0x00082228, {unlisted})
            .add(0x00100010, "PN", "Doe^Jane")
            .add(0x00100020, "LO", "MRN42")
            .add(0x00101005, "PN", "Roe^Jane")
            .add(0x00181002, "UI", "1.2.7.99")
            .add(0x00184000, "LT", "Mrs Doe moved")
            .add(0x0020000D, "UI", "1.2.7")
            .image(2, 2, 16, 12, false)
            .add(0x0020000E, "UI", "1.2.7.3")
            .sequence(0x04000561, {modified})
            .pixels(le16({5, 6, 7, 8}));
        QVERIFY(write(dir.filePath("IM1"), w.file("1.2.840.10008.1.2")));

        // The sequence reads as one, though nothing said it was.
        const DicomFile source = DicomFile::read(dir.filePath("IM1"));
        QVERIFY(source.isValid());
        QVERIFY(source.element(0x00082228));
        QCOMPARE(source.element(0x00082228)->items.size(), 1);

        const Scan scan = scanFiles(listFiles({dir.path()}, true));
        QTemporaryDir out;
        const Anonymize::Result result = Anonymize::copyScan(scan, out.path());
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join("\n")));
        QCOMPARE(result.written, 1);
        const QStringList copies = listFiles({result.folder}, true);
        QCOMPARE(copies.size(), 1);
        QFile copy(copies.first());
        QVERIFY(copy.open(QIODevice::ReadOnly));
        const QByteArray bytes = copy.readAll();
        for (const char *theirs : {"Doe", "Roe", "Poe", "House", "MRN42", "1.2.7.99"})
            QVERIFY2(!bytes.contains(theirs), theirs);
        // It still reads, and shows the same picture.
        const DicomFile read = DicomFile::read(copies.first());
        QVERIFY(read.isValid());
        QCOMPARE(read.first(0x00100010), QString("Anonymous"));
        QVERIFY(read.first(0x00181002).startsWith("2.25."));
        QCOMPARE(decodeFrame(read, 0).mono, (QVector<quint16>{5, 6, 7, 8}));
    }

    void opensZipFiles() {
        const QString bsdtar = QStandardPaths::findExecutable("bsdtar");
        QVERIFY2(!bsdtar.isEmpty(), "bsdtar (libarchive) is needed");
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("cd/DICOM"));
        QVERIFY(writeVolume(dir.filePath("cd/DICOM")));
        const QString zip = dir.filePath("scans.zip");
        QProcess pack;
        pack.start(bsdtar, {"-a", "-c", "-f", zip, "-C", dir.filePath("cd"), "DICOM"});
        QVERIFY(pack.waitForFinished(20000) && pack.exitCode() == 0);
        QVERIFY(Archive::isZip(zip));
        QVERIFY(!Archive::isZip(dir.filePath("cd/DICOM/1.2.9.1-0")));

        Backend backend(nullptr);
        backend.loadPaths({zip});
        QVERIFY(backend.waitForSlice(20000));
        QCOMPARE(backend.sliceCount(), 5);
        const QString unpacked = backend.scan().series.first().slices.first().path;
        QVERIFY(!unpacked.startsWith(dir.path()));
        Archive::cleanUp(true);
        QVERIFY(!QFileInfo::exists(unpacked));
    }

    void savesTheSeriesAsAVideo() {
        QVERIFY2(!QStandardPaths::findExecutable("ffprobe").isEmpty(), "ffmpeg is needed");
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        FakePicker picker;
        picker.answer = dir.filePath("loop.mp4");
        Backend backend(&picker);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        while (backend.decodedCount() < backend.sliceCount())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        backend.nextPlane();   // The coronal planes, 3 of them.
        QVERIFY(backend.waitForSlice(10000));
        backend.exportVideo();
        QCOMPARE(picker.asked, FilePicker::Purpose::SaveVideo);
        QTRY_VERIFY_WITH_TIMEOUT(backend.notice().startsWith("Saved") || !backend.problem().isEmpty(), 20000);
        QVERIFY2(backend.problem().isEmpty(), qPrintable(backend.problem()));
        QProcess probe;
        probe.start("ffprobe", {"-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                                "stream=nb_read_frames,width,height", "-of", "csv=p=0", picker.answer});
        QVERIFY(probe.waitForFinished(20000));
        // 4 wide; 5 tall at 3 mm a pixel, shown 15 tall, padded even.
        QCOMPARE(QString::fromUtf8(probe.readAllStandardOutput()).trimmed(), QString("4,16,3"));
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
        Backend partner(nullptr);
        Backend coronal(nullptr);
        Backend sagittal(nullptr);

        keepQmlModule();

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
        engine.rootContext()->setContextProperty("partner", &partner);
        engine.rootContext()->setContextProperty("coronal", &coronal);
        engine.rootContext()->setContextProperty("sagittal", &sagittal);
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
        // Two series of three slices, and a structured report.
        QTemporaryDir scans;
        for (int s = 0; s < 2; ++s)
            for (int i = 0; i < 3; ++i) {
                Writer w;
                w.image(2, 2, 16, 12, false)
                    .add(0x0020000E, "UI", s ? "1.2.3.2" : "1.2.3.1")
                    .add(Tag::SeriesNumber, "IS", QByteArray::number(s + 1))
                    .add(Tag::Modality, "CT", "CT")
                    .add(Tag::InstanceNumber, "IS", QByteArray::number(i + 1))
                    .add(Tag::ImagePosition, "DS", "0\\0\\" + QByteArray::number(i * 5))
                    .add(Tag::ImageOrientation, "DS", "1\\0\\0\\0\\1\\0")
                    .add(Tag::PixelSpacing, "DS", "1\\1")
                    .pixels(le16({0, 1, 2, 3}));
                QVERIFY(write(scans.filePath(QString("IM%1-%2").arg(s).arg(i)), w.file(ExplicitLE)));
            }
        Writer text;
        text.add(Tag::ValueType, "CS", "TEXT").sequence(Tag::ConceptName, {Writer::code("1", "Finding")})
            .add(Tag::TextValue, "UT", "Nothing to see.");
        Writer report;
        report.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.88.11")
            .add(Tag::ValueType, "CS", "CONTAINER")
            .sequence(Tag::ConceptName, {Writer::code("2", "CT Report")})
            .sequence(Tag::ContentSequence, {text});
        QVERIFY(write(scans.filePath("SR1"), report.file(ExplicitLE)));
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

        // Under the shortcut list the keys move through the list, and
        // leave the scan it covers as it is.
        QTest::keyClick(window, '?');
        QTRY_VERIFY(help->isVisible());
        QTest::keyClick(window, Qt::Key_Home);
        QTest::keyClick(window, Qt::Key_Up);
        QTest::keyClick(window, ']');
        QCOMPARE(backend.sliceIndex(), 2);
        QCOMPARE(backend.rotation(), 0);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!help->isVisible());

        // ] and X turn and flip the picture; R puts it back.
        QTest::keyClick(window, ']');
        QCOMPARE(backend.rotation(), 1);
        QTest::keyClick(window, 'X');
        QVERIFY(backend.flipped());
        QTest::keyClick(window, 'R');
        QCOMPARE(backend.rotation(), 0);
        QVERIFY(!backend.flipped());

        // V cuts the series the other ways and back; B makes slabs of it.
        QTest::keyClick(window, 'V');
        QCOMPARE(backend.plane(), 1);
        QTRY_VERIFY(backend.frameReady());
        QTest::keyClick(window, 'V');
        QTest::keyClick(window, 'V');
        QCOMPARE(backend.plane(), 0);
        // Ctrl and an arrow tilt it; V straightens it.
        QTest::keyClick(window, Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(backend.plane(), 3);
        QTRY_VERIFY(backend.frameReady());
        QTest::keyClick(window, Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(backend.planeName(), QString("Oblique"));
        QTest::keyClick(window, 'V');
        QCOMPARE(backend.plane(), 0);
        QTest::keyClick(window, 'B');
        QCOMPARE(backend.slabName(), QString("MIP 10 mm"));
        QTest::keyClick(window, 'B', Qt::ShiftModifier);
        QCOMPARE(backend.slabName(), QString("MIP 20 mm"));
        for (int i = 0; i < 3; ++i)
            QTest::keyClick(window, 'B');
        QCOMPARE(backend.slabName(), QString());
        QTest::keyClick(window, 'G');
        QCOMPARE(backend.colorMapName(), QString("Hot iron"));
        QTest::keyClick(window, 'R');

        // M, E, and Shift+M pick the measuring tools; Esc puts them down.
        auto *view = qobject_cast<SliceView *>(item(window->contentItem(), "view"));
        QVERIFY(view);
        QTest::keyClick(window, 'E');
        QCOMPARE(view->tool(), 2);
        QTest::keyClick(window, 'M', Qt::ShiftModifier);
        QCOMPARE(view->tool(), 3);
        QTest::keyClick(window, 'C', Qt::ShiftModifier);
        QCOMPARE(view->tool(), 4);
        QTest::keyClick(window, 'M');
        QCOMPARE(view->tool(), 1);
        QTest::keyClick(window, Qt::Key_Escape);
        QCOMPARE(view->tool(), 0);
        QTest::keyClick(window, Qt::Key_Home);

        // C puts the next series beside this one, following it; Tab and the
        // arrows then move the other side.
        auto *right = item(window->contentItem(), "rightPane");
        QVERIFY(right);
        QVERIFY(!right->isVisible());
        QTest::keyClick(window, Qt::Key_End);
        QTest::keyClick(window, 'C');
        QTRY_VERIFY(right->isVisible());
        QCOMPARE(partner.seriesIndex(), 1);
        QCOMPARE(partner.sliceIndex(), 2);   // Where the first side is.
        QTest::keyClick(window, Qt::Key_Home);
        QVERIFY(partner.waitForSlice(10000));
        QTest::keyClick(window, Qt::Key_End);
        QCOMPARE(partner.sliceIndex(), 2);
        QCOMPARE(backend.sliceIndex(), 2);   // Linked: the other side follows.
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Home);
        QCOMPARE(backend.sliceIndex(), 0);
        QCOMPARE(partner.sliceIndex(), 0);
        // Another series picked on one side starts where the other side
        // is, rather than taking it back to the start.
        QTest::keyClick(window, Qt::Key_End);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Left);
        QCOMPARE(partner.seriesIndex(), 0);
        QCOMPARE(backend.sliceIndex(), 2);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(partner.seriesIndex(), 1);
        QCOMPARE(backend.sliceIndex(), 2);
        QCOMPARE(partner.sliceIndex(), 2);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Home);
        QTest::keyClick(window, 'L');
        QTest::keyClick(window, Qt::Key_End);
        QCOMPARE(backend.sliceIndex(), 2);
        QCOMPARE(partner.sliceIndex(), 0);
        QTest::keyClick(window, 'C');
        QTRY_VERIFY(!right->isVisible());

        // D shows the series in 3D, where the keys turn it and cut into it
        // rather than move through slices; D again goes back to slices.
        auto *leftPane = item(window->contentItem(), "leftPane");
        auto *volume = qobject_cast<VolumeView *>(item(window->contentItem(), "view3d"));
        QVERIFY(leftPane && volume);
        QVERIFY(!volume->isVisible());
        QTest::keyClick(window, 'D');
        QTRY_VERIFY(volume->isVisible());
        QVERIFY(leftPane->property("threeD").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(volume->ready(), 10000);
        QTest::keyClick(window, Qt::Key_Down);
        QVERIFY(volume->cut() > 0);
        QCOMPARE(backend.sliceIndex(), 2);
        const QString look = volume->lookName();
        QTest::keyClick(window, 'B');
        QVERIFY(volume->lookName() != look);
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(volume->spinning());
        // It stops turning while another window is in front, and goes on
        // once this one is again.
        {
            QWindow other;
            other.resize(100, 100);
            other.show();
            other.requestActivate();
            QTRY_VERIFY(!window->isActive());
            const double right = volume->view().right[0];
            QTest::qWait(200);
            QCOMPARE(volume->view().right[0], right);
            QVERIFY(volume->spinning());
        }
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        const double turned = volume->view().right[0];
        QTRY_VERIFY(volume->view().right[0] != turned);
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!volume->spinning());
        QTest::keyClick(window, 'D');
        QTRY_VERIFY(!volume->isVisible());

        // Shift+V shows four views: as scanned, cut the other two ways, and
        // in 3D. Tab takes the keys from one to the next.
        auto *coronalPane = item(window->contentItem(), "coronalPane");
        auto *volumePane = item(window->contentItem(), "volumePane");
        QVERIFY(coronalPane && volumePane);
        QTest::keyClick(window, 'V', Qt::ShiftModifier);
        QTRY_VERIFY(coronalPane->isVisible());
        QVERIFY(volumePane->isVisible());
        QCOMPARE(coronal.seriesIndex(), backend.seriesIndex());
        QCOMPARE(coronal.plane(), 1);
        QCOMPARE(sagittal.plane(), 2);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Home);
        QCOMPARE(coronal.sliceIndex(), 0);
        QCOMPARE(backend.sliceIndex(), 2);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, 'B');   // The 3D view's look, not a slab.
        QCOMPARE(backend.slabName(), QString());
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, 'V', Qt::ShiftModifier);
        QTRY_VERIFY(!coronalPane->isVisible());

        // U puts a PET over the series when one was taken with it; here none was.
        QTest::keyClick(window, 'U');
        QVERIFY(!backend.fused());
        QCOMPARE(backend.notice(), QString("There's no PET taken with this series to put over it"));

        // P reads the report beside the scans; Esc goes back to them.
        auto *reportPanel = item(window->contentItem(), "reportPanel");
        QVERIFY(reportPanel);
        QTest::keyClick(window, 'P');
        QTRY_VERIFY(reportPanel->isVisible());
        QCOMPARE(backend.reportTitle(), QString("CT Report"));
        QCOMPARE(backend.reportLines().first().toMap().value("value").toString(), QString("Nothing to see."));
        QTest::keyClick(window, Qt::Key_Down);   // Scrolls the report, not the slices.
        QCOMPARE(backend.sliceIndex(), 2);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!reportPanel->isVisible());

        QTest::keyClick(window, 'S');
        QTRY_VERIFY(!series->isVisible());

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_MAIN(DicolensTests)
#include "dicolens_tests.moc"
