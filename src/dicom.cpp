#include "dicom.h"

#include <QCoreApplication>
#include <QFile>
#include <QStringDecoder>
#include <QtEndian>
#include <climits>
#include <cstring>
#include <zlib.h>

namespace {

constexpr quint32 Undefined = 0xFFFFFFFF;

quint16 u16(const uchar *p, bool le) { return le ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p); }
quint32 u32(const uchar *p, bool le) { return le ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p); }

// VRs whose explicit header has two reserved bytes and a four-byte length.
bool longVr(const QByteArray &vr) {
    static const QList<QByteArray> vrs{"OB", "OD", "OF", "OL", "OV", "OW", "SQ", "UC", "UN", "UR", "UT", "SV", "UV"};
    return vrs.contains(vr);
}

bool plausibleVr(const uchar *p) { return p[0] >= 'A' && p[0] <= 'Z' && p[1] >= 'A' && p[1] <= 'Z'; }

bool binaryVr(const QByteArray &vr) {
    static const QList<QByteArray> vrs{"OB", "OD", "OF", "OL", "OV", "OW", "UN"};
    return vrs.contains(vr);
}

QString trimmed(QString value) {
    while (!value.isEmpty() && (value.back() == ' ' || value.back() == QChar(0)))
        value.chop(1);
    while (!value.isEmpty() && value.front() == ' ')
        value.remove(0, 1);
    return value;
}

QString tr(const char *text, int n = -1) { return QCoreApplication::translate("scans", text, nullptr, n); }

QByteArray inflate(const uchar *data, qint64 size) {
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return {};
    QByteArray out;
    stream.next_in = const_cast<Bytef *>(data);
    stream.avail_in = uInt(std::min<qint64>(size, UINT_MAX));
    char buffer[65536];
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof buffer;
        status = ::inflate(&stream, Z_NO_FLUSH);
        out.append(buffer, int(sizeof buffer - stream.avail_out));
    }
    inflateEnd(&stream);
    // Some writers leave the stream unterminated; what inflated is kept.
    return status == Z_STREAM_END || status == Z_BUF_ERROR ? out : QByteArray();
}

}

QString tagText(quint32 tag) {
    return QStringLiteral("(%1,%2)")
        .arg(tag >> 16, 4, 16, QLatin1Char('0'))
        .arg(tag & 0xFFFF, 4, 16, QLatin1Char('0'))
        .toUpper();
}

DicomFile DicomFile::read(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    // Mapping reads only the pages the parser touches, so pixel data a file
    // is mostly made of never leaves the disk.
    const qint64 size = file.size();
    DicomFile result;
    if (uchar *data = size > 0 ? file.map(0, size) : nullptr) {
        result = parse(data, size, error);
        file.unmap(data);
    } else {
        result = parse(file.readAll(), error);
    }
    result.m_path = path;
    return result;
}

DicomFile DicomFile::parse(const QByteArray &bytes, QString *error) {
    return parse(reinterpret_cast<const uchar *>(bytes.constData()), bytes.size(), error);
}

DicomFile DicomFile::parse(const uchar *data, qint64 size, QString *error) {
    DicomFile file;
    auto fail = [&](const QString &reason) {
        if (error)
            *error = reason;
        return DicomFile();
    };

    qint64 pos = 0;
    if (size >= 132 && std::memcmp(data + 128, "DICM", 4) == 0)
        pos = 132;
    else if (size < 8)
        return fail("Not a DICOM file");

    // Files from before Part 10 have no preamble and start straight into
    // the data set, usually at group 0008; anything else isn't DICOM.
    const quint16 firstGroup = u16(data + pos, true);
    if (pos == 0 && firstGroup != 0x0002 && firstGroup != 0x0008)
        return fail("Not a DICOM file");

    // The file meta group is always explicit VR little endian.
    QList<DicomElement> meta;
    while (pos + 8 <= size && u16(data + pos, true) == 0x0002) {
        // Each element is parsed on its own, since group 0002 may not say
        // how long it is.
        if (!plausibleVr(data + pos + 4))
            break;
        const QByteArray vr(reinterpret_cast<const char *>(data + pos + 4), 2);
        const qint64 header = longVr(vr) ? 12 : 8;
        if (pos + header > size)
            return fail("The file is cut short");
        const quint32 length = longVr(vr) ? u32(data + pos + 8, true) : u16(data + pos + 6, true);
        const qint64 end = pos + header + length;
        if (length == Undefined || end > size)
            return fail("The file is cut short");
        if (!file.parseDataSet(data, size, pos, end, true, true, meta, 0, error))
            return {};
    }
    for (const auto &element : meta)
        if (element.tag == Tag::TransferSyntax)
            file.m_transferSyntax = trimmed(QString::fromLatin1(element.value));

    bool explicitVr = true, littleEndian = true;
    if (meta.isEmpty()) {
        explicitVr = pos + 6 <= size && plausibleVr(data + pos + 4);
        file.m_transferSyntax = explicitVr ? "1.2.840.10008.1.2.1" : "1.2.840.10008.1.2";
    } else if (file.m_transferSyntax == "1.2.840.10008.1.2") {
        explicitVr = false;
    } else if (file.m_transferSyntax == "1.2.840.10008.1.2.2") {
        littleEndian = false;
    }
    file.m_littleEndian = littleEndian;

    // A deflated file's data set is one raw deflate stream after the meta
    // group; it's read inflated, pixel data and all.
    const uchar *body = data;
    qint64 bodySize = size;
    if (file.m_transferSyntax == "1.2.840.10008.1.2.1.99") {
        file.m_inflated = inflate(data + pos, size - pos);
        if (file.m_inflated.isEmpty())
            return fail("This deflated file is damaged");
        body = reinterpret_cast<const uchar *>(file.m_inflated.constData());
        bodySize = file.m_inflated.size();
        pos = 0;
    }

    QList<DicomElement> elements;
    QString reason;
    const bool complete = file.parseDataSet(body, bodySize, pos, bodySize, explicitVr, littleEndian, elements, 0, &reason);
    // A file cut short after its image header still shows what it has; one
    // that breaks before saying what it is doesn't.
    if (!complete && elements.isEmpty())
        return fail(reason);

    file.m_elements = meta + elements;
    file.index();
    file.m_valid = true;
    return file;
}

bool DicomFile::parseDataSet(const uchar *data, qint64 size, qint64 &pos, qint64 end, bool explicitVr,
                             bool littleEndian, QList<DicomElement> &out, int depth, QString *error) {
    auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };
    if (depth > 32)
        return fail("Sequences nest too deeply");
    end = qMin(end, size);

    while (pos < end) {
        if (pos + 8 > end)
            return fail("The file is cut short");
        const quint16 group = u16(data + pos, littleEndian);
        const quint16 number = u16(data + pos + 2, littleEndian);
        const quint32 tag = quint32(group) << 16 | number;

        if (tag == Tag::ItemEnd) {
            pos += 8;
            return true;
        }
        if (tag == Tag::SequenceEnd || tag == Tag::Item) {
            // Out of place here; skip its header and go on.
            pos += 8;
            continue;
        }

        DicomElement element;
        element.tag = tag;
        quint32 length = 0;
        if (explicitVr && plausibleVr(data + pos + 4)) {
            element.vr = QByteArray(reinterpret_cast<const char *>(data + pos + 4), 2);
            if (longVr(element.vr)) {
                if (pos + 12 > end)
                    return fail("The file is cut short");
                length = u32(data + pos + 8, littleEndian);
                pos += 12;
            } else {
                length = u16(data + pos + 6, littleEndian);
                pos += 8;
            }
        } else {
            element.vr = Dictionary::vr(tag);
            if (element.vr.isEmpty())
                element.vr = "UN";
            length = u32(data + pos + 4, littleEndian);
            pos += 8;
        }
        element.length = length == Undefined ? -1 : length;

        if (tag == Tag::PixelData) {
            const qint64 start = pos;
            if (length == Undefined) {
                // Encapsulated: items of compressed fragments, then a
                // sequence delimiter.
                while (pos + 8 <= size) {
                    const quint32 itemTag = quint32(u16(data + pos, littleEndian)) << 16 | u16(data + pos + 2, littleEndian);
                    const quint32 itemLength = u32(data + pos + 4, littleEndian);
                    pos += 8;
                    if (itemTag == Tag::SequenceEnd)
                        break;
                    if (itemTag != Tag::Item || itemLength == Undefined)
                        break;
                    pos += itemLength;
                }
                pos = qMin(pos, size);
            } else {
                pos = qMin(pos + qint64(length), size);
            }
            // Icon images inside sequences have pixel data too; only the
            // top level's is the image.
            if (depth == 0) {
                m_pixelOffset = start;
                m_pixelLength = pos - start;
                m_encapsulated = length == Undefined;
                m_pixelVr = element.vr;
            }
            out.append(element);
            continue;
        }

        if (element.vr == "SQ" || length == Undefined) {
            // An undefined-length UN is a sequence written implicitly.
            const bool itemsExplicit = element.vr == "UN" ? false : explicitVr;
            const qint64 sequenceEnd = length == Undefined ? end : qMin(pos + qint64(length), end);
            while (pos + 8 <= sequenceEnd) {
                const quint32 itemTag = quint32(u16(data + pos, littleEndian)) << 16 | u16(data + pos + 2, littleEndian);
                const quint32 itemLength = u32(data + pos + 4, littleEndian);
                pos += 8;
                if (itemTag == Tag::SequenceEnd)
                    break;
                if (itemTag != Tag::Item)
                    return fail("A sequence is damaged");
                QList<DicomElement> item;
                if (itemLength == Undefined) {
                    if (!parseDataSet(data, size, pos, sequenceEnd, itemsExplicit, littleEndian, item, depth + 1, error))
                        return false;
                } else {
                    const qint64 itemEnd = pos + itemLength;
                    if (itemEnd > sequenceEnd)
                        return fail("The file is cut short");
                    if (!parseDataSet(data, size, pos, itemEnd, itemsExplicit, littleEndian, item, depth + 1, error))
                        return false;
                    pos = itemEnd;
                }
                element.items.append(item);
            }
            if (length != Undefined)
                pos = sequenceEnd;
            out.append(element);
            continue;
        }

        if (pos + length > end) {
            out.append(element);
            return fail("The file is cut short");
        }
        element.value = QByteArray(reinterpret_cast<const char *>(data + pos), length);
        pos += length;
        if (tag == Tag::SpecificCharacterSet && depth == 0)
            m_charset = element.value;
        out.append(element);
    }
    return true;
}

void DicomFile::index() {
    m_index.clear();
    for (int i = 0; i < m_elements.size(); ++i)
        m_index.insert(m_elements[i].tag, i);
}

QByteArray DicomFile::readPixels(qint64 offset, qint64 length) const {
    if (!m_inflated.isEmpty())
        return m_inflated.mid(m_pixelOffset + offset, length);
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(m_pixelOffset + offset))
        return {};
    return file.read(length);
}

const DicomElement *DicomFile::element(quint32 tag) const {
    const auto it = m_index.constFind(tag);
    return it == m_index.constEnd() ? nullptr : &m_elements[*it];
}

QString DicomFile::decode(const QByteArray &text) const {
    // The first character set named applies to the whole value; ISO 2022
    // escapes inside a value are rare enough to read as their base set.
    static const QHash<QString, const char *> names{
        {"ISO_IR 6", "ISO-8859-1"},      {"ISO_IR 100", "ISO-8859-1"}, {"ISO_IR 101", "ISO-8859-2"},
        {"ISO_IR 109", "ISO-8859-3"},    {"ISO_IR 110", "ISO-8859-4"}, {"ISO_IR 144", "ISO-8859-5"},
        {"ISO_IR 127", "ISO-8859-6"},    {"ISO_IR 126", "ISO-8859-7"}, {"ISO_IR 138", "ISO-8859-8"},
        {"ISO_IR 148", "ISO-8859-9"},    {"ISO_IR 203", "ISO-8859-15"}, {"ISO_IR 166", "TIS-620"},
        {"ISO_IR 13", "Shift_JIS"},      {"ISO_IR 192", "UTF-8"},      {"GB18030", "GB18030"},
        {"GBK", "GBK"},                  {"ISO 2022 IR 6", "ISO-8859-1"}, {"ISO 2022 IR 100", "ISO-8859-1"},
        {"ISO 2022 IR 87", "ISO-2022-JP"}, {"ISO 2022 IR 13", "Shift_JIS"}, {"ISO 2022 IR 149", "EUC-KR"},
        {"ISO 2022 IR 58", "GB2312"},    {"ISO 2022 IR 144", "ISO-8859-5"}, {"ISO 2022 IR 127", "ISO-8859-6"},
        {"ISO 2022 IR 126", "ISO-8859-7"}, {"ISO 2022 IR 138", "ISO-8859-8"}, {"ISO 2022 IR 148", "ISO-8859-9"},
    };
    QString charset = trimmed(QString::fromLatin1(m_charset));
    charset = charset.section('\\', 0, 0).trimmed();
    if (charset.isEmpty())
        charset = trimmed(QString::fromLatin1(m_charset)).section('\\', 1, 1).trimmed();
    const char *name = names.value(charset, nullptr);
    if (name && qstrcmp(name, "UTF-8") == 0)
        return QString::fromUtf8(text);
    if (name && qstrcmp(name, "ISO-8859-1") != 0) {
        QStringDecoder decoder(name);
        if (decoder.isValid())
            return decoder.decode(text);
    }
    return QString::fromLatin1(text);
}

QString DicomFile::string(quint32 tag) const {
    const DicomElement *e = element(tag);
    if (!e)
        return {};
    QStringList values = decode(e->value).split('\\');
    for (auto &value : values)
        value = trimmed(value);
    return values.join('\\');
}

QString DicomFile::first(quint32 tag) const { return string(tag).section('\\', 0, 0); }

QList<double> DicomFile::numbers(quint32 tag) const { return numbers(element(tag)); }

const DicomElement *DicomFile::find(quint32 tag, int frame) const {
    // A functional group is a sequence holding one item of the tags it groups.
    auto inGroups = [tag](const QList<DicomElement> &groups) -> const DicomElement * {
        for (const auto &group : groups)
            for (const auto &item : group.items)
                for (const auto &e : item)
                    if (e.tag == tag)
                        return &e;
        return nullptr;
    };
    if (const DicomElement *perFrame = element(0x52009230))
        if (frame >= 0 && frame < perFrame->items.size())
            if (const DicomElement *e = inGroups(perFrame->items[frame]))
                return e;
    if (const DicomElement *shared = element(0x52009229))
        if (!shared->items.isEmpty())
            if (const DicomElement *e = inGroups(shared->items.first()))
                return e;
    return element(tag);
}

QList<double> DicomFile::numbers(const DicomElement *e) const {
    QList<double> result;
    if (!e)
        return result;
    const auto *p = reinterpret_cast<const uchar *>(e->value.constData());
    const qint64 n = e->value.size();
    const bool le = m_littleEndian;
    auto each = [&](int width, auto read) {
        for (qint64 i = 0; i + width <= n; i += width)
            result.append(read(p + i));
    };
    if (e->vr == "US")
        each(2, [&](const uchar *q) { return double(u16(q, le)); });
    else if (e->vr == "SS")
        each(2, [&](const uchar *q) { return double(qint16(u16(q, le))); });
    else if (e->vr == "UL")
        each(4, [&](const uchar *q) { return double(u32(q, le)); });
    else if (e->vr == "SL")
        each(4, [&](const uchar *q) { return double(qint32(u32(q, le))); });
    else if (e->vr == "FL")
        each(4, [&](const uchar *q) { quint32 v = u32(q, le); float f; std::memcpy(&f, &v, 4); return double(f); });
    else if (e->vr == "FD")
        each(8, [&](const uchar *q) {
            quint64 v = le ? qFromLittleEndian<quint64>(q) : qFromBigEndian<quint64>(q);
            double d;
            std::memcpy(&d, &v, 8);
            return d;
        });
    else if (e->vr == "UN" && n == 2)
        // Implicit files without a dictionary entry, usually a US.
        result.append(u16(p, le));
    else
        for (const auto &part : QString::fromLatin1(e->value).split('\\')) {
            bool ok = false;
            const double v = trimmed(part).toDouble(&ok);
            if (ok)
                result.append(v);
        }
    return result;
}

double DicomFile::number(quint32 tag, double fallback) const {
    const auto values = numbers(tag);
    return values.isEmpty() ? fallback : values.first();
}

int DicomFile::integer(quint32 tag, int fallback) const { return int(number(tag, fallback)); }

QString DicomFile::formatName(const QString &value) {
    // Family^Given^Middle^Prefix^Suffix, with "=" between alphabetic,
    // ideographic, and phonetic forms; the first form that's there wins.
    QString form;
    for (const auto &part : value.split('='))
        if (!part.trimmed().isEmpty()) {
            form = part;
            break;
        }
    const QStringList parts = form.split('^');
    auto at = [&](int i) { return i < parts.size() ? parts[i].trimmed() : QString(); };
    QStringList given;
    for (int i : {3, 1, 2})
        if (!at(i).isEmpty())
            given << at(i);
    QString result = at(0);
    if (!given.isEmpty())
        result += (result.isEmpty() ? "" : ", ") + given.join(' ');
    if (!at(4).isEmpty())
        result += ' ' + at(4);
    return result;
}

QString DicomFile::formatDate(const QString &value) {
    const QString d = value.trimmed();
    if (d.size() == 8 && d.at(0).isDigit())
        return d.left(4) + '-' + d.mid(4, 2) + '-' + d.mid(6, 2);
    if (d.size() == 10 && d.at(4) == '.')
        return d.left(4) + '-' + d.mid(5, 2) + '-' + d.mid(8, 2);
    return d;
}

QString DicomFile::formatTime(const QString &value) {
    QString t = value.trimmed().section('.', 0, 0);
    if (t.contains(':'))
        return t;
    if (t.size() >= 4)
        return t.left(2) + ':' + t.mid(2, 2) + (t.size() >= 6 ? ':' + t.mid(4, 2) : QString());
    return t;
}

QString DicomFile::display(const DicomElement &e) const {
    if (e.vr == "SQ" || !e.items.isEmpty())
        return tr("%n items", e.items.size());
    if (e.tag == Tag::PixelData) {
        return m_encapsulated ? tr("Compressed image data, %1").arg(Dictionary::transferSyntaxName(m_transferSyntax))
                              : tr("Image data, %n bytes", int(qMin<qint64>(m_pixelLength, INT_MAX)));
    }

    static const QList<QByteArray> numeric{"US", "SS", "UL", "SL", "FL", "FD"};
    if (numeric.contains(e.vr)) {
        const auto values = numbers(&e);
        QStringList parts;
        for (int i = 0; i < values.size() && i < 16; ++i)
            parts << QString::number(values[i], 'g', 10);
        if (values.size() > 16)
            parts << "…";
        return parts.join(" \\ ");
    }
    if (e.vr == "AT") {
        QStringList parts;
        const auto *p = reinterpret_cast<const uchar *>(e.value.constData());
        for (int i = 0; i + 4 <= e.value.size(); i += 4)
            parts << tagText(quint32(u16(p + i, m_littleEndian)) << 16 | u16(p + i + 2, m_littleEndian));
        return parts.join(" \\ ");
    }
    if (binaryVr(e.vr)) {
        if (e.value.isEmpty())
            return {};
        if (e.value.size() <= 12)
            return QString::fromLatin1(e.value.toHex(' '));
        return tr("%n bytes", int(e.value.size()));
    }

    QStringList values = decode(e.value).split('\\');
    for (auto &value : values) {
        value = trimmed(value);
        if (e.vr == "PN")
            value = formatName(value);
        else if (e.vr == "DA")
            value = formatDate(value);
        else if (e.vr == "TM")
            value = formatTime(value);
        else if (e.vr == "UI") {
            const QString name = Dictionary::transferSyntaxName(value).isEmpty() ? Dictionary::sopClassName(value)
                                                                                 : Dictionary::transferSyntaxName(value);
            if (!name.isEmpty())
                value += " (" + name + ")";
        }
    }
    QString text = values.join(" \\ ");
    if (text.size() > 400)
        text = text.left(400) + "…";
    return text;
}

QList<QByteArray> DicomFile::splitFrames(const QByteArray &encapsulated, int frameCount) {
    struct Fragment { qint64 offset; QByteArray data; };
    QList<Fragment> fragments;
    QByteArray offsetTable;
    const auto *p = reinterpret_cast<const uchar *>(encapsulated.constData());
    const qint64 size = encapsulated.size();
    qint64 pos = 0;
    bool first = true;
    qint64 fragmentBase = -1;
    while (pos + 8 <= size) {
        const quint32 tag = quint32(u16(p + pos, true)) << 16 | u16(p + pos + 2, true);
        const quint32 length = u32(p + pos + 4, true);
        if (tag == Tag::SequenceEnd || tag != Tag::Item || length == Undefined)
            break;
        const qint64 itemStart = pos;
        pos += 8;
        const QByteArray data = encapsulated.mid(pos, qMin<qint64>(length, size - pos));
        pos += length;
        if (first) {
            offsetTable = data;
            first = false;
            continue;
        }
        if (fragmentBase < 0)
            fragmentBase = itemStart;
        fragments.append({itemStart - fragmentBase, data});
    }

    QList<QByteArray> frames;
    if (fragments.isEmpty())
        return frames;
    if (frameCount <= 1) {
        QByteArray all;
        for (const auto &f : fragments)
            all += f.data;
        return {all};
    }

    // The basic offset table says where each frame's first fragment starts.
    if (offsetTable.size() >= frameCount * 4) {
        QList<qint64> starts;
        for (int i = 0; i < frameCount; ++i)
            starts << qFromLittleEndian<quint32>(offsetTable.constData() + i * 4);
        for (int i = 0; i < frameCount; ++i) {
            const qint64 from = starts[i], to = i + 1 < frameCount ? starts[i + 1] : LLONG_MAX;
            QByteArray frame;
            for (const auto &f : fragments)
                if (f.offset >= from && f.offset < to)
                    frame += f.data;
            frames << frame;
        }
        return frames;
    }
    if (fragments.size() == frameCount) {
        for (const auto &f : fragments)
            frames << f.data;
        return frames;
    }
    // No table and more fragments than frames: a frame starts at each JPEG
    // start-of-image or JPEG 2000 start-of-codestream marker.
    for (const auto &f : fragments) {
        const bool starts = f.data.size() >= 2 && uchar(f.data[0]) == 0xFF &&
                            (uchar(f.data[1]) == 0xD8 || uchar(f.data[1]) == 0x4F);
        if (starts || frames.isEmpty())
            frames << f.data;
        else
            frames.last() += f.data;
    }
    return frames;
}
