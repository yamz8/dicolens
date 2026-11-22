#include "anonymize.h"

#include "pixels.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <cstring>

namespace {

// Dicolens' implementation class, a UUID-derived UID of its own.
const char *ImplementationClass = "2.25.294237494695696284811840147955721691334";
const char *ImplementationVersion = "DICOLENS";

const char *ExplicitLittle = "1.2.840.10008.1.2.1";
const char *ImplicitLittle = "1.2.840.10008.1.2";

// What stands where a value was. Each is valid for its VR, and none is
// looked for in a copy, or a copy of a copy would never pass.
const char *DummyName = "Anonymous";
const char *DummyText = "Anonymized";
const char *DummyCode = "ANONYMIZED";
const char *DummyDate = "19000101";

// --- The profile -------------------------------------------------------------

// The columns of PS3.15 Table E.1-1 after the tag, in the table's order.
enum Column {
    Basic, SafePrivate, RetainUids, DeviceIdentity, InstitutionIdentity, Characteristics, FullDates, ModifiedDates,
    Descriptors, StructuredContent, Graphics, Columns
};

struct Row {
    QByteArray action[Columns];
    QString name;
};

struct RowPattern {
    quint32 mask = 0;
    quint32 digits = 0;
    Row row;
};

struct Profile {
    QHash<quint32, Row> exact;
    QList<RowPattern> patterns;   // (50xx,xxxx) Curve Data, (60xx,3000) Overlay Data.
    Row privateRow;               // Every odd group.
    QString edition;
};

const Profile &profile() {
    static const Profile table = [] {
        Profile p;
        Dictionary::table(":/data/confidentiality.tsv", &p.edition, [&](const QList<QByteArrayView> &cells) {
            if (cells.size() < 1 + Columns + 3)
                return;
            Row row;
            for (int c = 0; c < Columns; ++c)
                row.action[c] = cells[1 + c].toByteArray();
            row.name = QString::fromUtf8(cells[1 + Columns + 2]);
            quint32 mask = 0, digits = 0;
            if (cells[0] == "private")
                p.privateRow = row;
            else if (!Dictionary::pattern(cells[0], &mask, &digits))
                return;
            else if (mask == 0xFFFFFFFF)
                p.exact.insert(digits, row);
            else
                p.patterns.append({mask, digits, row});
        });
        return p;
    }();
    return table;
}

const Row *rowFor(quint32 tag) {
    const Profile &p = profile();
    if ((tag >> 16) & 1)
        return &p.privateRow;
    if (const auto it = p.exact.constFind(tag); it != p.exact.constEnd())
        return &*it;
    for (const RowPattern &pattern : p.patterns)
        if ((tag & pattern.mask) == pattern.digits)
            return &pattern.row;
    return nullptr;
}

// What keepDescriptions keeps: of the texts the Clean Descriptors Option
// covers, the three that tell one series from another.
bool isDescription(quint32 tag) {
    return tag == Tag::StudyDescription || tag == Tag::SeriesDescription || tag == 0x00181030;
}

enum class Action { Leave, Keep, Remove, Empty, Dummy, Uid };

Action actionFor(const Row &row, quint32 tag, const Anonymize::Options &options) {
    QByteArray code = row.action[Basic];
    // An option's requirement overrides the profile's (PS3.15 E.1.1): the
    // two this applies always, in the table's order.
    for (const Column option : {Characteristics, FullDates})
        if (!row.action[option].isEmpty())
            code = row.action[option];
    // "Clean" asks for a text of similar meaning known to say nothing of
    // the patient, which takes someone reading it. What the profile does
    // without the option is the safe side of that.
    if (code == "C")
        code = row.action[Basic];
    if (options.keepDescriptions && isDescription(tag))
        code = "K";

    if (code == "K")
        return Action::Keep;
    // Where the choice depends on whether the file's IOD needs the
    // attribute (Type 3), needs it present (Type 2), or needs it to have a
    // value (Type 1), the one that's valid for all three is taken, as no
    // list of every IOD is carried here: a dummy value where one is
    // offered, else an empty one, and a sequence kept for its replaced UIDs.
    if (code == "U" || code == "X/Z/U*")
        return Action::Uid;
    if (code.contains('D'))
        return Action::Dummy;
    if (code.contains('Z'))
        return Action::Empty;
    return Action::Remove;
}

// What the table doesn't name and goes all the same: who issued an ID, the
// patient's dates in other calendars, the physician who asked by their
// codes, and the original values kept encrypted, which this copy has none of.
const QSet<quint32> &alsoRemoved() {
    static const QSet<quint32> tags{0x00080051, 0x00100024, 0x00100033, 0x00100034, 0x00321031, 0x04000500};
    return tags;
}

// What an earlier anonymizing said of itself; this one says its own.
bool isStatement(quint32 tag) {
    return tag == 0x00120062 || tag == 0x00120063 || tag == 0x00120064 || tag == 0x00280303;
}

// What's rebuilt for the copy, or no longer true of it: the file's meta
// group, every group's length, the length to the end, trailing padding.
bool isStructural(quint32 tag) {
    return (tag >> 16) == 0x0002 || (tag & 0xFFFF) == 0x0000 || tag == 0x00080001 || tag == 0xFFFCFFFC;
}

// --- Values ------------------------------------------------------------------

bool longVr(const QByteArray &vr) {
    static const QSet<QByteArray> vrs{"OB", "OD", "OF", "OL", "OV", "OW", "SQ", "SV", "UC", "UN", "UR", "UT", "UV"};
    return vrs.contains(vr);
}

// A file may not say a value's kind (implicit files), or say it doesn't
// know (UN); the registry does.
QByteArray vrOf(const DicomElement &e) {
    if (!e.vr.isEmpty() && e.vr != "UN")
        return e.vr;
    const QByteArray known = Dictionary::vr(e.tag);
    return known.isEmpty() ? QByteArray("UN") : known;
}

// By what the file says or the registry does, whichever names it: a file
// can be wrong about a tag's kind, and a name under the wrong one is still
// a name.
bool isName(const DicomElement &e) { return e.vr == "PN" || Dictionary::vr(e.tag) == "PN"; }
bool isUid(const DicomElement &e) { return e.vr == "UI" || Dictionary::vr(e.tag) == "UI"; }
bool isSequence(const DicomElement &e) { return e.vr == "SQ" || !e.items.isEmpty(); }

// A sequence that couldn't be read as one is bytes nothing here can look
// into: it starts with an item, in little-endian as copies are.
bool unreadSequence(const DicomElement &e) {
    return e.items.isEmpty() && e.value.startsWith(QByteArray("\xFE\xFF\x00\xE0", 4));
}

QByteArray padded(QByteArray value, char with) {
    if (value.size() % 2)
        value.append(with);
    return value;
}

// A new UID from a random UUID, as 2.25 and its 128 bits in decimal.
QString newUid() {
    QByteArray digits = QUuid::createUuid().toRfc4122();
    QString decimal;
    // Long division by ten, most significant byte first.
    while (std::any_of(digits.begin(), digits.end(), [](char c) { return c != 0; })) {
        int rest = 0;
        for (auto &byte : digits) {
            const int value = rest * 256 + quint8(byte);
            byte = char(value / 10);
            rest = value % 10;
        }
        decimal.prepend(QChar('0' + rest));
    }
    return "2.25." + (decimal.isEmpty() ? QStringLiteral("0") : decimal);
}

// Standard UIDs (SOP classes, transfer syntaxes, coding schemes) name kinds
// of things, not this patient's. Only digits and dots make one: a value
// that starts as the standard's and goes on in words is kept by nothing.
bool standardUid(const QString &uid) {
    return uid.startsWith("1.2.840.10008.")
           && std::all_of(uid.begin(), uid.end(), [](QChar c) { return (c >= '0' && c <= '9') || c == '.'; });
}

QString uidFor(const QString &old, Anonymize::Session &session) {
    if (old.isEmpty() || standardUid(old))
        return old;
    auto it = session.uids.find(old);
    if (it == session.uids.end())
        it = session.uids.insert(old, newUid());
    return *it;
}

QStringList uidValues(const QByteArray &value) {
    QStringList parts = QString::fromLatin1(value).remove(QChar('\0')).split('\\');
    for (QString &p : parts)
        p = p.trimmed();
    return parts;
}

QByteArray uids(const QByteArray &value, Anonymize::Session &session) {
    QStringList parts = uidValues(value);
    for (QString &p : parts)
        p = uidFor(p, session);
    return padded(parts.join('\\').toLatin1(), '\0');
}

DicomElement text(quint32 tag, const QByteArray &vr, const QByteArray &value) {
    DicomElement e;
    e.tag = tag;
    e.vr = vr;
    e.value = padded(value, vr == "UI" ? '\0' : ' ');
    e.length = e.value.size();
    return e;
}

DicomElement sequence(quint32 tag, const QList<QList<DicomElement>> &items) {
    DicomElement e;
    e.tag = tag;
    e.vr = "SQ";
    e.items = items;
    e.length = -1;
    return e;
}

QByteArray us(int value) {
    QByteArray out(2, '\0');
    out[0] = char(value & 0xFF);
    out[1] = char((value >> 8) & 0xFF);
    return out;
}

// A value that says nothing and is valid for its VR ("D" in the table).
QByteArray dummyValue(const QByteArray &vr) {
    static const QHash<QByteArray, QByteArray> values{
        {"AE", DummyCode}, {"AS", "000Y"}, {"CS", DummyCode}, {"DA", DummyDate}, {"DT", "19000101000000"},
        {"TM", "000000"},  {"DS", "0 "},   {"IS", "0 "},      {"LO", DummyText}, {"SH", DummyText},
        {"LT", DummyText}, {"ST", DummyText}, {"UT", DummyText}, {"UC", DummyText}, {"PN", DummyName},
        {"UR", "urn:uuid:00000000-0000-0000-0000-000000000000"},
    };
    if (const auto it = values.constFind(vr); it != values.constEnd())
        return padded(*it, ' ');
    // Binary numbers and bytes: one value's worth of zeros.
    static const QHash<QByteArray, int> widths{
        {"US", 2}, {"SS", 2}, {"UL", 4}, {"SL", 4}, {"FL", 4}, {"FD", 8}, {"SV", 8}, {"UV", 8},
        {"AT", 4}, {"OB", 2}, {"OW", 2}, {"OF", 4}, {"OL", 4}, {"OD", 8}, {"OV", 8},
    };
    return QByteArray(widths.value(vr, 2), '\0');
}

// --- Cleaning ----------------------------------------------------------------

struct Cleaner {
    const DicomFile &file;
    Anonymize::Session &session;
    const Anonymize::Options &options;
    Anonymize::Changes changes;

    // The stand-in for a patient's ID: one a patient, so a prior still goes
    // with its patient.
    QByteArray patientId(const QByteArray &value) {
        const QString id = file.decode(value).trimmed();
        auto it = session.patients.find(id);
        if (it == session.patients.end())
            it = session.patients.insert(id, QStringLiteral("ANON%1").arg(session.patients.size() + 1));
        return it->toLatin1();
    }

    // "D" for a sequence is for all it holds: every value in it becomes a
    // dummy, and the items keep their shape, so what the IOD requires of
    // them is still there.
    DicomElement dummy(DicomElement e) {
        const QByteArray vr = vrOf(e);
        if (isSequence(e)) {
            for (auto &item : e.items) {
                QList<DicomElement> out;
                for (const DicomElement &inner : item)
                    if (!isStructural(inner.tag) && !((inner.tag >> 16) & 1) && inner.tag != Tag::PixelData
                        && !unreadSequence(inner))
                        out << dummy(inner);
                item = out;
            }
            return e;
        }
        if (e.value.isEmpty())
            return e;
        e.vr = vr;
        e.value = e.tag == Tag::PatientId ? padded(patientId(e.value), ' ')
                  : vr == "UI" ? uids(e.value, session) : dummyValue(vr);
        e.length = e.value.size();
        return e;
    }

    QList<DicomElement> clean(const QList<DicomElement> &elements) {
        QList<DicomElement> out;
        for (DicomElement e : elements) {
            const quint16 group = e.tag >> 16;
            if (isStructural(e.tag) || isStatement(e.tag))
                continue;
            // Families the profile removes whole. An overlay plane goes
            // with its data: the profile removes the data, and what
            // describes a plane that isn't there would be wrong.
            const quint32 family = (group & 1) ? Anonymize::Family::Private
                                   : group == 0x0004 ? Anonymize::Family::Directory
                                   : (group & 0xFF00) == 0x5000 ? Anonymize::Family::Curves
                                   : (group & 0xFF00) == 0x6000 ? Anonymize::Family::Overlays : 0;
            if (family) {
                changes.removed.insert(family);
                continue;
            }
            if (e.tag == Tag::PixelData) {
                out << e;
                continue;
            }
            if (unreadSequence(e) || alsoRemoved().contains(e.tag) || group == 0x0000) {
                changes.removed.insert(e.tag);
                continue;
            }

            const Row *row = rowFor(e.tag);
            Action action = row ? actionFor(*row, e.tag, options) : Action::Leave;
            // Whatever the table says of the tag, a person's name isn't
            // left as it is, nor a UID that names this patient's things.
            if (action == Action::Leave || action == Action::Keep) {
                if (isName(e))
                    action = Action::Remove;
                else if (isUid(e))
                    action = Action::Uid;
            }
            const bool blank = e.value.isEmpty() && e.items.isEmpty();

            switch (action) {
            case Action::Remove:
                changes.removed.insert(e.tag);
                continue;
            case Action::Empty:
                if (blank)
                    break;
                if (e.tag == Tag::PatientName) {
                    // Empty would do; a name that says what this is reads
                    // better wherever the copy is opened.
                    e = text(e.tag, "PN", DummyName);
                    changes.replaced.insert(e.tag);
                    break;
                }
                e.value.clear();
                e.items.clear();
                e.length = 0;
                changes.emptied.insert(e.tag);
                break;
            case Action::Dummy:
                if (blank)
                    break;
                e = dummy(e);
                changes.replaced.insert(e.tag);
                break;
            case Action::Uid: {
                const QStringList before = uidValues(e.value);
                if (isSequence(e)) {
                    for (auto &item : e.items)
                        item = clean(item);
                    changes.replaced.insert(e.tag);
                    break;
                }
                e.value = uids(e.value, session);
                e.length = e.value.size();
                if (uidValues(e.value) != before)
                    changes.replaced.insert(e.tag);
                break;
            }
            case Action::Keep:
                if (!blank)
                    changes.kept.insert(e.tag);
                [[fallthrough]];
            case Action::Leave:
                // Keeping a sequence is keeping what the profile leaves of
                // each of its items.
                for (auto &item : e.items)
                    item = clean(item);
                break;
            }
            out << e;
        }
        return out;
    }
};

// --- Writing -----------------------------------------------------------------

void put16(QByteArray &out, quint16 v) {
    out.append(char(v & 0xFF));
    out.append(char(v >> 8));
}

void put32(QByteArray &out, quint32 v) {
    put16(out, quint16(v & 0xFFFF));
    put16(out, quint16(v >> 16));
}

void header(QByteArray &out, quint32 tag, const QByteArray &vr, quint32 length, bool explicitVr) {
    put16(out, quint16(tag >> 16));
    put16(out, quint16(tag & 0xFFFF));
    if (!explicitVr) {
        put32(out, length);
        return;
    }
    out.append(vr.left(2));
    if (longVr(vr)) {
        put16(out, 0);
        put32(out, length);
    } else {
        put16(out, quint16(length));
    }
}

// Where the copy's picture comes from: the original's bytes as they are,
// compressed fragments and all; pixels drawn again; or, for a plan, nowhere.
struct Picture {
    enum class From { Original, Redrawn, Nowhere };
    From from = From::Original;
    QByteArray redrawn;
    QByteArray vr;
};

void writeElements(QByteArray &out, const QList<DicomElement> &elements, bool explicitVr, const DicomFile &file,
                   const Picture &picture, bool top) {
    for (const auto &e : elements) {
        if (e.tag == Tag::PixelData) {
            // Only the image's own pixel data is kept on reading.
            if (!top || !file.hasPixelData() || picture.from == Picture::From::Nowhere)
                continue;
            if (picture.from == Picture::From::Redrawn) {
                header(out, e.tag, picture.vr, quint32(picture.redrawn.size()), explicitVr);
                out.append(picture.redrawn);
                continue;
            }
            const QByteArray pixels = file.readPixels(0, file.pixelLength());
            const QByteArray vr = file.pixelVr().isEmpty() ? QByteArray("OW") : file.pixelVr();
            header(out, e.tag, vr == "UN" ? QByteArray(file.encapsulated() ? "OB" : "OW") : vr,
                   file.encapsulated() ? 0xFFFFFFFF : quint32(pixels.size()), explicitVr);
            out.append(pixels);
            continue;
        }
        if (isSequence(e)) {
            header(out, e.tag, "SQ", 0xFFFFFFFF, explicitVr);
            for (const auto &item : e.items) {
                put16(out, 0xFFFE);
                put16(out, 0xE000);
                put32(out, 0xFFFFFFFF);
                writeElements(out, item, explicitVr, file, picture, false);
                put16(out, 0xFFFE);
                put16(out, 0xE00D);
                put32(out, 0);
            }
            put16(out, 0xFFFE);
            put16(out, 0xE0DD);
            put32(out, 0);
            continue;
        }
        QByteArray vr = e.vr.isEmpty() ? QByteArray("UN") : e.vr;
        // A short VR can't say a length past 64 KiB; explicitly it's UN.
        if (explicitVr && !longVr(vr) && e.value.size() > 0xFFFF)
            vr = "UN";
        header(out, e.tag, vr, quint32(e.value.size()), explicitVr);
        out.append(e.value);
    }
}

// The profile and the options it was applied with, as codes. Without the
// profile's own code when descriptions were kept: the profile removes them,
// and nothing cleaned them, so neither it nor the Clean Descriptors Option
// can be claimed. What was kept by an option is still said, as that claims
// less protection, never more.
QList<QList<DicomElement>> methodCodes(const Anonymize::Options &options) {
    auto code = [](const char *value, const char *meaning) {
        return QList<DicomElement>{text(0x00080100, "SH", value), text(0x00080102, "SH", "DCM"),
                                   text(0x00080104, "LO", meaning)};
    };
    QList<QList<DicomElement>> codes;
    if (!options.keepDescriptions)
        codes << code("113100", "Basic Application Confidentiality Profile");
    codes << code("113106", "Retain Longitudinal Temporal Information Full Dates Option")
          << code("113108", "Retain Patient Characteristics Option");
    return codes;
}

// --- Blacking out ------------------------------------------------------------

bool lossy(const QString &syntax) {
    static const QSet<QString> syntaxes{"1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.51", "1.2.840.10008.1.2.4.81",
                                         "1.2.840.10008.1.2.4.91", "1.2.840.10008.1.2.4.93", "1.2.840.10008.1.2.4.203"};
    return syntaxes.contains(syntax);
}

// Whether an overlay plane is kept in the pixels' spare bits, as files once
// did (its bits allocated are the image's, or its bit position isn't 0).
// Removing what describes the plane leaves those bits where they were, for
// anyone who looks; they hold the text a scanner drew over the picture.
bool overlayInPixels(const DicomFile &file) {
    if (!file.hasPixelData())
        return false;
    for (const DicomElement &e : file.elements()) {
        const quint16 number = e.tag & 0xFFFF;
        if (((e.tag >> 16) & 0xFF01) != 0x6000 || (number != 0x0100 && number != 0x0102))
            continue;
        if (file.numbers(&e).value(0) > (number == 0x0100 ? 1 : 0))
            return true;
    }
    return false;
}

// Why a file's picture can't be drawn again value for value, or nothing
// when it can, in the words a copy's error has.
QString cantRedraw(const DicomFile &file, const QList<QRect> &rectangles) {
    using Cant = Anonymize::Redraw::Cant;
    const Anonymize::Redraw redraw = Anonymize::canRedraw(file);
    switch (redraw.cant) {
    case Cant::NoPicture: return QStringLiteral("it has no picture to black out");
    case Cant::Video: return QStringLiteral("it's a video, which can't be written back frame for frame");
    case Cant::NoSize: return QStringLiteral("it has no image size");
    case Cant::Palette: return QStringLiteral("its colours come from a 16-bit palette, which is only shown in 8 bits");
    case Cant::GreyBits: return QStringLiteral("its pixels are %1-bit, which aren't kept value for value").arg(redraw.bits);
    case Cant::ColourBits: return QStringLiteral("its colours are %1-bit, which are only shown in 8 bits").arg(redraw.bits);
    case Cant::Nothing: break;
    }
    const ImageFormat f = ImageFormat::from(file);
    for (const QRect &r : rectangles)
        if (!r.isValid() || !r.intersects(QRect(0, 0, f.columns, f.rows)))
            return QStringLiteral("the rectangle %1,%2,%3,%4 lies outside its %5x%6 picture")
                .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).arg(f.columns).arg(f.rows);
    return {};
}

void set(QList<DicomElement> &elements, quint32 tag, const QByteArray &vr, const QByteArray &value) {
    const DicomElement e = text(tag, vr, value);
    for (DicomElement &old : elements)
        if (old.tag == tag) {
            old = e;
            return;
        }
    elements << e;
}

void drop(QList<DicomElement> &elements, quint32 tag) {
    elements.removeIf([tag](const DicomElement &e) { return e.tag == tag; });
}

// Decodes every frame, sets the rectangles to black, and gives the pixels
// uncompressed, with the image pixel module that's now true of them.
bool redraw(const DicomFile &file, const QList<QRect> &rectangles, QList<DicomElement> &elements, Picture &picture,
            QString *error) {
    const ImageFormat f = ImageFormat::from(file);
    const bool grey = !f.color();
    const int bytes = grey ? f.bitsAllocated / 8 : 3;
    const qint64 count = qint64(f.rows) * f.columns;
    const QRect whole(0, 0, f.columns, f.rows);
    const QList<QByteArray> fragments = readFrames(file);
    // The stored values that show as black: the lowest there is, or the
    // highest where low values are white (MONOCHROME1).
    const int bits = std::clamp(f.bitsStored, 1, f.bitsAllocated);
    const int lowest = f.isSigned ? -(1 << (bits - 1)) : 0;
    const int highest = f.isSigned ? (1 << (bits - 1)) - 1 : (1 << bits) - 1;

    QByteArray &out = picture.redrawn;
    // Room for what the header promises, within reason: a damaged one can
    // promise anything, and each frame is only added once it has decoded.
    out.reserve(qMin(count * bytes * qMin(f.frames, 4096), qint64(1) << 28));
    for (int index = 0; index < f.frames; ++index) {
        Frame frame = decodeFrame(file, index, file.encapsulated() ? &fragments : nullptr);
        if (frame.error.isEmpty() && (frame.width != f.columns || frame.height != f.rows
                                      || (grey ? frame.mono.size() != count : frame.color.isNull())))
            frame.error = QStringLiteral("The decoded image is a different size than its header says");
        if (!frame.error.isEmpty()) {
            *error = frame.detail.isEmpty() ? frame.error : QStringLiteral("%1 (%2)").arg(frame.error, frame.detail);
            return false;
        }
        if (grey) {
            const quint16 black = quint16(frame.inverted ? highest : lowest);
            for (const QRect &rectangle : rectangles) {
                const QRect r = rectangle & whole;
                for (int y = r.top(); y <= r.bottom(); ++y)
                    std::fill_n(frame.mono.begin() + qint64(y) * f.columns + r.left(), r.width(), black);
            }
            const qsizetype start = out.size();
            out.resize(start + count * bytes);
            char *to = out.data() + start;
            for (const quint16 v : std::as_const(frame.mono)) {
                *to++ = char(v & 0xFF);
                if (bytes == 2)
                    *to++ = char(v >> 8);
            }
        } else {
            QImage image = frame.color.convertToFormat(QImage::Format_RGB888);
            for (const QRect &rectangle : rectangles) {
                const QRect r = rectangle & whole;
                for (int y = r.top(); y <= r.bottom(); ++y)
                    std::memset(image.scanLine(y) + 3 * r.left(), 0, 3 * size_t(r.width()));
            }
            for (int y = 0; y < f.rows; ++y)
                out.append(reinterpret_cast<const char *>(image.constScanLine(y)), 3 * qsizetype(f.columns));
        }
    }
    if (out.size() % 2)
        out.append('\0');
    picture.from = Picture::From::Redrawn;
    picture.vr = grey && bytes == 2 ? "OW" : "OB";

    // The image pixel module, as the pixels now are: stored values from bit
    // 0 up, and colour as RGB a pixel at a time, whatever it was decoded from.
    set(elements, Tag::SamplesPerPixel, "US", us(grey ? 1 : 3));
    set(elements, Tag::Photometric, "CS", grey ? f.photometric.toLatin1() : QByteArray("RGB"));
    set(elements, Tag::BitsAllocated, "US", us(grey ? f.bitsAllocated : 8));
    set(elements, Tag::BitsStored, "US", us(grey ? bits : 8));
    set(elements, Tag::HighBit, "US", us(grey ? bits - 1 : 7));
    set(elements, Tag::PixelRepresentation, "US", us(grey && f.isSigned ? 1 : 0));
    if (grey) {
        drop(elements, Tag::PlanarConfiguration);
    } else {
        set(elements, Tag::PlanarConfiguration, "US", us(0));
        // A palette's colours are in the pixels now.
        for (const quint32 tag : {0x00281101u, 0x00281102u, 0x00281103u, 0x00281199u, 0x00281201u, 0x00281202u,
                                  0x00281203u, 0x00281221u, 0x00281222u, 0x00281223u})
            drop(elements, tag);
    }
    // The smallest and largest values were the original's, and the offset
    // tables its compressed frames'.
    for (const quint32 tag : {0x00280106u, 0x00280107u, 0x7FE00001u, 0x7FE00002u})
        drop(elements, tag);
    // Decoding doesn't bring back what a lossy compression took, and the
    // copy goes on saying so.
    if (lossy(file.transferSyntax()))
        set(elements, 0x00282110, "CS", "01");
    return true;
}

// --- Learning and looking ----------------------------------------------------

using Anonymize::Identifier;

// Words that say what kind of place or exam something is rather than which,
// and the standard's codes for a modality: a department called "MR" is in
// every MR scan's software version. Dicolens' own stand-ins are here too,
// so a copy of a copy passes.
bool generic(const QString &folded) {
    static const QSet<QString> words{
        "anonymous", "anonymized", "unknown", "none", "null", "n/a", "na", "test", "default", "hospital", "clinic",
        "radiology", "radiologie", "cardiology", "neurology", "oncology", "imaging", "ultrasound", "mammography",
        "emergency", "nuclear medicine", "x-ray", "xray", "mri", "pet", "pet-ct", "pet/ct", "xr", "dr", "md",
        // The small words of a name, which every other sentence has.
        "de", "la", "le", "du", "des", "van", "von", "der", "den", "di", "da", "del", "dos", "das", "el", "al",
        "bin", "ben", "ibn", "mac", "mc", "st", "san", "ter", "ten", "of", "the",
        // The modalities scans come in, as the standard codes them. Not
        // every code: LEN and REG are also what people are called.
        "cr", "ct", "dx", "mg", "mr", "nm", "ot", "pt", "rf", "sc", "sr", "us", "xa",
    };
    if (words.contains(folded))
        return true;
    // ANON1, ANON2: the stand-ins for a patient's ID.
    return folded.size() > 4 && folded.startsWith("anon")
           && std::all_of(folded.begin() + 4, folded.end(), [](QChar c) { return c.isDigit(); });
}

void add(Anonymize::Session &session, const QString &written, Identifier::Kind kind) {
    Identifier id;
    id.found = written.simplified();
    id.text = id.found.toCaseFolded();
    id.kind = kind;
    if (id.text.size() < 2 || generic(id.text))
        return;
    const bool digits = std::all_of(id.text.begin(), id.text.end(), [](QChar c) { return c.isDigit(); });
    const bool letters = std::any_of(id.text.begin(), id.text.end(), [](QChar c) { return c.isLetter(); });
    // Scripts without spaces between words have no word of their own to be.
    const bool spaced = std::all_of(id.text.begin(), id.text.end(), [](QChar c) { return c.unicode() < 0x0250; });
    if (digits) {
        // A number of two or three digits is in every other value.
        if (id.text.size() < 4)
            return;
        id.match = Identifier::Match::Number;
    } else if (!letters) {
        if (id.text.size() < 5)
            return;
    } else if (id.text.size() <= 4 && spaced) {
        id.match = Identifier::Match::Word;
    }
    const QString key = id.text + QChar(int(id.match));
    if (session.known.contains(key))
        return;
    session.known.insert(key);
    session.identifiers << id;
}

// Every part of a name: family, given and middle names, each whole and
// word by word. Titles and suffixes ("Dr", "Jr") name nobody.
void addName(Anonymize::Session &session, const QString &value) {
    for (const QString &form : value.split('=')) {
        const QStringList parts = form.split('^');
        for (int i = 0; i < parts.size() && i < 3; ++i) {
            add(session, parts[i], Identifier::Kind::Name);
            QString word;
            for (const QChar c : parts[i] + ' ') {
                if (c.isLetterOrNumber() || c == '\'') {
                    word += c;
                    continue;
                }
                add(session, word, Identifier::Kind::Name);
                word.clear();
            }
        }
    }
}

// A birth date in the ways it's written: as DICOM does, with separators,
// and day or month first.
void addBirthDate(Anonymize::Session &session, const QString &value) {
    QString digits;
    for (const QChar c : value)
        if (c.isDigit())
            digits += c;
    if (digits.size() != 8 || digits == DummyDate || digits == "00000000")
        return;
    const QString y = digits.left(4), m = digits.mid(4, 2), d = digits.mid(6, 2);
    add(session, digits, Identifier::Kind::BirthDate);
    for (const char separator : {'-', '.', '/'}) {
        const QChar s(separator);
        for (const QString &form : {y + s + m + s + d, d + s + m + s + y, m + s + d + s + y})
            add(session, form, Identifier::Kind::BirthDate);
    }
}

void addPhone(Anonymize::Session &session, const QString &value) {
    add(session, value, Identifier::Kind::Phone);
    Identifier id;
    id.found = value.simplified();
    for (const QChar c : value)
        if (c.isDigit())
            id.text += c;
    id.kind = Identifier::Kind::Phone;
    id.match = Identifier::Match::Squeezed;
    const QString key = id.text + QChar(int(id.match));
    if (id.text.size() < 6 || session.known.contains(key))
        return;
    session.known.insert(key);
    session.identifiers << id;
}

enum class Source { None, Id, BirthDate, Place, Address, Phone, Device };

Source sourceOf(quint32 tag) {
    static const QHash<quint32, Source> tags = [] {
        QHash<quint32, Source> t;
        // Patient, accession, study, admission, procedure, order, episode,
        // and trial subject IDs; the medical record's locator and insurance.
        for (const quint32 tag : {0x00100020u, 0x00101000u, 0x00080050u, 0x00200010u, 0x00380010u, 0x00101090u,
                                  0x00101050u, 0x00400009u, 0x00400253u, 0x00401001u, 0x00402016u, 0x00402017u,
                                  0x00380060u, 0x00120040u, 0x00120042u})
            t.insert(tag, Source::Id);
        t.insert(Tag::PatientBirthDate, Source::BirthDate);
        // The institution and its department, who issued the ID, where the
        // patient is and lives, and who is responsible or asked.
        for (const quint32 tag : {0x00080080u, 0x00081040u, 0x00100021u, 0x00400243u, 0x00380300u, 0x00380400u,
                                  0x00102299u, 0x00321033u})
            t.insert(tag, Source::Place);
        for (const quint32 tag : {0x00080081u, 0x00101040u, 0x00080092u, 0x00401102u, 0x00102152u})
            t.insert(tag, Source::Address);
        for (const quint32 tag : {0x00102154u, 0x00080094u, 0x00401103u, 0x00102155u, 0x00401104u})
            t.insert(tag, Source::Phone);
        // Serial numbers and the IDs of plates, generators, cassettes,
        // gantries and detectors; and what a site calls its stations.
        for (const quint32 tag : {0x00181000u, 0x00181004u, 0x00181005u, 0x00181007u, 0x00181008u, 0x0018700Au,
                                  0x00081010u, 0x00400242u, 0x00400010u})
            t.insert(tag, Source::Device);
        return t;
    }();
    return tags.value(tag, Source::None);
}

void learnFrom(const DicomFile &file, const QList<DicomElement> &elements, const QString &make,
               Anonymize::Session &session) {
    for (const DicomElement &e : elements) {
        for (const auto &item : e.items)
            learnFrom(file, item, make, session);
        // The meta group says what wrote the file, not whose it is, and a
        // copy has its own.
        if (e.value.isEmpty() || e.tag == Tag::PixelData || (e.tag >> 16) == 0x0002)
            continue;
        if (isUid(e)) {
            for (const QString &uid : uidValues(e.value))
                if (!uid.isEmpty() && !standardUid(uid))
                    session.sourceUids.insert(uid);
            continue;
        }
        const Source source = isName(e) ? Source::None : sourceOf(e.tag);
        if (!isName(e) && source == Source::None)
            continue;
        for (const QString &value : file.decode(e.value).split('\\')) {
            if (isName(e)) {
                addName(session, value);
                continue;
            }
            switch (source) {
            case Source::Id:
                add(session, value, Identifier::Kind::Id);
                break;
            case Source::BirthDate:
                addBirthDate(session, value);
                break;
            case Source::Phone:
                addPhone(session, value);
                break;
            case Source::Place:
            case Source::Address:
            case Source::Device: {
                // What names the make of machine, which the copy keeps,
                // isn't what identifies this one: a station is often
                // called after its model.
                const QString folded = value.simplified().toCaseFolded();
                if (folded.isEmpty() || make.contains(folded))
                    break;
                const auto kind = source == Source::Device ? Identifier::Kind::Device : Identifier::Kind::Place;
                add(session, value, kind);
                // An address also line by line: a street or a town alone.
                if (source == Source::Address) {
                    QString part = value;
                    for (const QChar separator : {QChar('\n'), QChar('\r'), QChar(';'), QChar('^')})
                        part.replace(separator, ',');
                    for (const QString &line : part.split(','))
                        if (line.simplified().size() >= 6)
                            add(session, line, kind);
                }
                break;
            }
            case Source::None:
                break;
            }
        }
    }
}

bool binaryVr(const QByteArray &vr) {
    static const QSet<QByteArray> vrs{"OB", "OD", "OF", "OL", "OV", "OW", "UN", "US", "SS", "UL", "SL", "FL", "FD",
                                      "SV", "UV", "AT"};
    return vrs.contains(vr);
}

// Whether bytes of no known kind are text: nothing in them but what text
// is made of, bar the padding at their end.
bool looksLikeText(const QByteArray &vr, QByteArray value) {
    if (vr != "UN" && vr != "OB")
        return false;
    while (value.endsWith('\0'))
        value.chop(1);
    return !value.isEmpty() && std::all_of(value.begin(), value.end(), [](char c) {
        const uchar u = uchar(c);
        return u >= 0x20 || u == '\t' || u == '\n' || u == '\r' || u == '\f' || u == 0x1B;
    });
}

// Whether a value has only the characters its kind is written with, bar
// the padding at its end.
bool madeOf(const QString &text, const char *characters) {
    const QLatin1StringView allowed(characters);
    return std::all_of(text.begin(), text.end(), [&](QChar c) { return c.isNull() || allowed.contains(c); });
}

// Whether a text holds an identifier, by the identifier's rule.
bool holds(const QString &text, const Identifier &id) {
    if (id.match == Identifier::Match::Anywhere)
        return text.contains(id.text);
    QString squeezed;
    if (id.match == Identifier::Match::Squeezed)
        for (const QChar c : text)
            if (c.isLetterOrNumber())
                squeezed += c;
    const QString &in = id.match == Identifier::Match::Squeezed ? squeezed : text;
    const bool word = id.match == Identifier::Match::Word;
    auto continues = [word](QChar c) { return word ? c.isLetterOrNumber() : c.isDigit(); };
    for (qsizetype at = in.indexOf(id.text); at >= 0; at = in.indexOf(id.text, at + 1)) {
        const qsizetype after = at + id.text.size();
        if ((at == 0 || !continues(in[at - 1])) && (after >= in.size() || !continues(in[after])))
            return true;
    }
    return false;
}

// In English, as a copy's error says it; the window translates these where
// it shows them.
QString whatIs(Identifier::Kind kind) {
    switch (kind) {
    case Identifier::Kind::Name: return QT_TRANSLATE_NOOP("dicolens", "a person's name");
    case Identifier::Kind::Id: return QT_TRANSLATE_NOOP("dicolens", "an ID");
    case Identifier::Kind::BirthDate: return QT_TRANSLATE_NOOP("dicolens", "the birth date");
    case Identifier::Kind::Place: return QT_TRANSLATE_NOOP("dicolens", "an institution or address");
    case Identifier::Kind::Phone: return QT_TRANSLATE_NOOP("dicolens", "a phone number");
    case Identifier::Kind::Device: return QT_TRANSLATE_NOOP("dicolens", "a device's name or serial number");
    }
    return {};
}

// A date or time the Retain Longitudinal Temporal Information option keeps.
bool keptDate(quint32 tag) {
    const Row *row = rowFor(tag);
    return row && row->action[FullDates] == "K";
}

void look(const DicomFile &copy, const QList<DicomElement> &elements, const Anonymize::Session &session,
          QList<Anonymize::Failure> &failures) {
    for (const DicomElement &e : elements) {
        // The first few say enough of a copy that isn't kept.
        if (failures.size() >= 20)
            return;
        // What the copy says of itself is Dicolens' own words, and says
        // nothing of the source: a patient called Free, or an accession
        // number that is one of the method's codes, isn't found in them.
        if (isStatement(e.tag) || e.tag == 0x00020012 || e.tag == 0x00020013)
            continue;
        for (const auto &item : e.items)
            look(copy, item, session, failures);
        // The pixels themselves aren't searched, as numbers or as floats.
        if (e.value.isEmpty() || e.tag == Tag::PixelData || e.tag == 0x7FE00008 || e.tag == 0x7FE00009)
            continue;
        auto fail = [&](const QString &found, const QString &what) {
            QString name = Dictionary::name(e.tag);
            failures << Anonymize::Failure{{}, e.tag,
                                           name.isEmpty() ? QString(QT_TRANSLATE_NOOP("dicolens", "an unnamed attribute")) : name,
                                           found, what};
        };
        // Three ways to read a value: text people wrote, a coded value,
        // and bytes. In bytes only what's long enough not to be there by
        // chance is looked for. In coded values no names are: the body
        // part of Mr Colon's scan is COLON; and nothing short, where MR and
        // PA are codes. Numbers and dates aren't looked for among numbers
        // and dates, where an ID of 1024 would be every other matrix size.
        // A value is only read as a code, a number, or a UID when it's
        // made of nothing else: a damaged length can put the elements that
        // followed, names and all, inside one that claims to be any of them.
        const QByteArray vr = vrOf(e);
        const bool bytes = binaryVr(vr) && !looksLikeText(vr, e.value);
        if (bytes && e.value.size() < 5)
            continue;
        const QString written = bytes ? QString::fromLatin1(e.value) : copy.decode(e.value);
        const QString text = written.toCaseFolded();
        const bool coded = vr == "CS" && madeOf(written, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_ \\");
        const bool numeric = (vr == "DS" || vr == "IS" || vr == "DA" || vr == "DT" || vr == "TM" || vr == "AS")
                             && madeOf(text, "0123456789+-.:e dwmy\\");
        if (isUid(e)) {
            for (const QString &uid : uidValues(e.value))
                if (session.sourceUids.contains(uid)) {
                    fail(uid, QT_TRANSLATE_NOOP("dicolens", "one of the original's UIDs"));
                    break;
                }
            if (madeOf(text, "0123456789. \\"))
                continue;
        }
        for (const Identifier &id : session.identifiers) {
            const bool number = id.match == Identifier::Match::Number || id.match == Identifier::Match::Squeezed;
            if (bytes && (id.text.size() < 5 || id.match == Identifier::Match::Squeezed))
                continue;
            if (coded && (id.kind == Identifier::Kind::Name || id.match == Identifier::Match::Word
                          || id.match == Identifier::Match::Squeezed))
                continue;
            // The day of a study stays, by the option that keeps dates, even
            // when the patient was born on it.
            if (numeric && id.kind == Identifier::Kind::BirthDate && keptDate(e.tag))
                continue;
            if (numeric && number) {
                // But a birth date left in another date of its own is one.
                const bool date = id.kind == Identifier::Kind::BirthDate && (vr == "DA" || vr == "DT") && id.text.size() == 8;
                if (!date || !text.contains(id.text))
                    continue;
                fail(id.found, whatIs(id.kind));
                break;
            }
            if (holds(text, id)) {
                fail(id.found, whatIs(id.kind));
                break;
            }
        }
    }
}

// --- One file ----------------------------------------------------------------

bool process(const DicomFile &file, const QString &path, Anonymize::Session &session, const Anonymize::Options &options,
             const QList<QRect> &blackout, Anonymize::Outcome *outcome, QString *error) {
    Anonymize::Outcome unused;
    Anonymize::Outcome &o = outcome ? *outcome : unused;
    o = {};
    auto fail = [&](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };
    if (!file.isValid())
        return fail(QT_TRANSLATE_NOOP("dicolens", "This file can't be read"));
    if (!file.littleEndian())
        return fail(QT_TRANSLATE_NOOP("dicolens", "Big-endian files can't be copied"));
    const bool planned = path.isEmpty();
    // The picture is drawn again to black a region out of it, and to clear
    // an overlay out of its pixels' spare bits: the decoder reads only the
    // bits that are the picture's.
    const bool redrawn = !blackout.isEmpty() || overlayInPixels(file);
    auto cant = [&](const QString &reason) {
        return fail((blackout.isEmpty() ? QStringLiteral("This image keeps an overlay in its pixels, which can't be taken out: %1")
                                        : QStringLiteral("A region can't be blacked out of this image: %1")).arg(reason));
    };
    if (redrawn)
        if (const QString reason = cantRedraw(file, blackout); !reason.isEmpty())
            return cant(reason);

    Anonymize::learn(file, session);

    // Values are kept as stored, so the copy keeps the original's encoding;
    // a deflated one is written inflated, and one drawn again uncompressed.
    QString syntax = file.transferSyntax();
    if (syntax == "1.2.840.10008.1.2.1.99" || redrawn)
        syntax = ExplicitLittle;
    const bool explicitVr = syntax != ImplicitLittle;

    Cleaner cleaner{file, session, options, {}};
    QList<DicomElement> elements = cleaner.clean(file.elements());
    QStringList said = Anonymize::method(options);
    if (!blackout.isEmpty())
        said << QStringLiteral("Regions of the picture blacked out: %1").arg(blackout.size());
    elements << text(0x00120062, "CS", "YES") << text(0x00120063, "LO", said.join('\\').toLatin1())
             << text(0x00280303, "CS", "UNMODIFIED");
    elements << sequence(0x00120064, methodCodes(options));

    Picture picture;
    if (planned) {
        picture.from = Picture::From::Nowhere;
    } else if (redrawn) {
        QString reason;
        if (!redraw(file, blackout, elements, picture, &reason))
            return cant(reason);
    }
    std::stable_sort(elements.begin(), elements.end(), [](const DicomElement &a, const DicomElement &b) { return a.tag < b.tag; });
    elements.erase(std::unique(elements.begin(), elements.end(),
                               [](const DicomElement &a, const DicomElement &b) { return a.tag == b.tag; }),
                   elements.end());

    QString sopClass, sopInstance;
    for (const auto &e : elements) {
        if (e.tag == Tag::SopClass)
            sopClass = uidValues(e.value).value(0);
        else if (e.tag == Tag::SopInstance)
            sopInstance = uidValues(e.value).value(0);
    }

    // The preamble and the meta group are the copy's own: the original's
    // say what wrote it and where it was sent from.
    QByteArray meta;
    auto metaElement = [&](quint32 tag, const QByteArray &vr, const QByteArray &value) {
        const QByteArray v = padded(value, vr == "UI" || vr == "OB" ? '\0' : ' ');
        header(meta, tag, vr, quint32(v.size()), true);
        meta.append(v);
    };
    metaElement(0x00020001, "OB", QByteArray("\x00\x01", 2));
    metaElement(0x00020002, "UI", sopClass.toLatin1());
    metaElement(0x00020003, "UI", sopInstance.toLatin1());
    metaElement(0x00020010, "UI", syntax.toLatin1());
    metaElement(0x00020012, "UI", ImplementationClass);
    metaElement(0x00020013, "SH", ImplementationVersion);

    QByteArray out(128, '\0');
    out.append("DICM");
    header(out, 0x00020000, "UL", 4, true);
    put32(out, quint32(meta.size()));
    out.append(meta);
    writeElements(out, elements, explicitVr, file, picture, true);

    // The copy is read back as anything else would read it, and looked
    // through for what the sources said of the patient. Until it has
    // passed it isn't under its own name.
    const QString unchecked = path + ".unchecked";
    DicomFile copy;
    if (planned) {
        copy = DicomFile::parse(out);
    } else {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile save(unchecked);
        save.setDirectWriteFallback(false);
        if (!save.open(QIODevice::WriteOnly) || save.write(out) != out.size() || !save.commit())
            return fail(QT_TRANSLATE_NOOP("dicolens", "The copy couldn't be written"));
        copy = DicomFile::read(unchecked);
    }
    if (copy.isValid())
        o.failures = Anonymize::verify(copy, session);
    for (Anonymize::Failure &failure : o.failures)
        failure.path = file.path();
    const bool passed = copy.isValid() && o.failures.isEmpty();
    if (!planned) {
        QFile::remove(path);
        if (!passed)
            QFile::remove(unchecked);
        else if (!QFile::rename(unchecked, path)) {
            QFile::remove(unchecked);
            return fail(QT_TRANSLATE_NOOP("dicolens", "The copy couldn't be written"));
        }
    }
    if (!copy.isValid())
        return fail(QStringLiteral("The copy couldn't be read back"));
    if (!passed) {
        QString reason = o.failures.first().text();
        if (o.failures.size() > 1)
            reason += QStringLiteral(", and %1 more like it").arg(o.failures.size() - 1);
        return fail(QStringLiteral("The copy wasn't kept: %1").arg(reason));
    }
    o.changes = cleaner.changes;
    o.blackedOut = !blackout.isEmpty();
    return true;
}

// --- A whole scan ------------------------------------------------------------

struct Tally {
    QMap<quint32, int> removed, emptied, replaced, kept;

    void add(const Anonymize::Changes &changes) {
        for (const quint32 tag : changes.removed)
            ++removed[tag];
        for (const quint32 tag : changes.emptied)
            ++emptied[tag];
        for (const quint32 tag : changes.replaced)
            ++replaced[tag];
        for (const quint32 tag : changes.kept)
            ++kept[tag];
    }

    static QList<Anonymize::Count> counts(const QMap<quint32, int> &files) {
        QList<Anonymize::Count> list;
        for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
            const quint32 tag = it.key();
            QString name = Dictionary::name(tag);
            if (tag == Anonymize::Family::Private)
                continue;
            // The names that aren't the standard's are marked for the
            // window to translate, and say so.
            if (tag == Anonymize::Family::Curves)
                list << Anonymize::Count{"(50xx,xxxx)", "Curve Data", it.value()};
            else if (tag == Anonymize::Family::Overlays)
                list << Anonymize::Count{"(60xx,xxxx)", QT_TRANSLATE_NOOP("dicolens", "Overlay planes"), it.value(), false};
            else if (tag == Anonymize::Family::Directory)
                list << Anonymize::Count{"(0004,xxxx)", QT_TRANSLATE_NOOP("dicolens", "File-set directory attributes"),
                                         it.value(), false};
            else if (name.isEmpty())
                list << Anonymize::Count{tagText(tag), QT_TRANSLATE_NOOP("dicolens", "Unnamed attribute"), it.value(), false};
            else
                list << Anonymize::Count{tagText(tag), name, it.value()};
        }
        // Private attributes have no tag to sort by, and come last.
        if (const int n = files.value(Anonymize::Family::Private))
            list << Anonymize::Count{{}, QT_TRANSLATE_NOOP("dicolens", "Private attributes"), n, false};
        return list;
    }

    Anonymize::Summary summary() const { return {counts(removed), counts(emptied), counts(replaced), counts(kept)}; }
};

Anonymize::Result run(const Scan &scan, const QString *folder, const Anonymize::Options &options,
                      const std::atomic<bool> *cancel) {
    Anonymize::Result result;
    result.method = Anonymize::method(options);
    QString root;
    if (folder) {
        root = QDir(*folder).filePath("Anonymous scans");
        for (int n = 2; QFileInfo::exists(root); ++n)
            root = QDir(*folder).filePath(QStringLiteral("Anonymous scans %1").arg(n));
        if (!QDir().mkpath(root)) {
            result.errors << QStringLiteral("%1: can't be created").arg(root);
            return result;
        }
        result.folder = root;
    }
    result.skipped = scan.reports.size();
    result.annotations = scan.annotations.size();

    struct Job {
        QString from, to;
        int series = 0;
    };
    QList<Job> jobs;
    for (int s = 0; s < scan.series.size(); ++s) {
        // A multi-frame file is a slice a frame, and copied once.
        QStringList paths;
        QSet<QString> seen;
        for (const auto &slice : scan.series[s].slices)
            if (!seen.contains(slice.path)) {
                seen.insert(slice.path);
                paths << slice.path;
            }
        const QString dir = QStringLiteral("%1/Series %2").arg(root).arg(s + 1, 3, 10, QChar('0'));
        for (int i = 0; i < paths.size(); ++i)
            jobs << Job{paths[i], QStringLiteral("%1/%2.dcm").arg(dir).arg(i + 1, 4, 10, QChar('0')), s};
    }

    // Everything the scan says of the patient is learned before the first
    // copy is made, from the files left out as well: a name that only a
    // report spells out is still not to be in an image's description.
    Anonymize::Session session;
    QStringList sources = scan.annotations;
    for (const Job &job : std::as_const(jobs))
        sources << job.from;
    for (const Report &report : scan.reports)
        sources << report.path;
    for (const QString &source : std::as_const(sources)) {
        if (cancel && *cancel)
            return result;
        if (const DicomFile file = DicomFile::read(source); file.isValid())
            Anonymize::learn(file, session);
    }

    Tally tally;
    for (const Job &job : std::as_const(jobs)) {
        if (cancel && *cancel)
            break;
        QString error;
        Anonymize::Outcome outcome;
        const DicomFile file = DicomFile::read(job.from, &error);
        const QList<QRect> rectangles = options.blackout.value(job.series);
        const bool done = file.isValid()
                          && (folder ? Anonymize::write(file, job.to, session, &error, options, rectangles, &outcome)
                                     : Anonymize::check(file, session, &error, options, rectangles, &outcome));
        if (!done) {
            result.errors << QStringLiteral("%1: %2").arg(job.from, error);
            result.failures << outcome.failures;
            result.uncopied << Anonymize::Uncopied{job.series, job.from, error, !outcome.failures.isEmpty()};
            continue;
        }
        ++result.written;
        tally.add(outcome.changes);
        if (outcome.blackedOut)
            ++result.blackedOut;
        if (Anonymize::mayShowDetails(file)) {
            ++result.showDetails;
            ++result.showDetailsIn[job.series];
        }
    }
    result.summary = tally.summary();
    return result;
}

}

namespace Anonymize {

QString Failure::text() const {
    return QStringLiteral("%1 %2 still holds %3: %4").arg(tagText(tag), name, what, found);
}

QString edition() { return profile().edition; }

QStringList method(const Options &options) {
    // Each a value of De-identification Method, which holds 64 characters.
    QStringList said;
    if (options.keepDescriptions)
        said << QStringLiteral("Per DICOM PS3.15 %1 Table E.1-1 (Basic Profile), except:").arg(edition())
             << QStringLiteral("Study Description, Series Description, Protocol Name kept as is");
    else
        said << QStringLiteral("DICOM PS3.15 %1 Basic Application Confidentiality Profile").arg(edition());
    said << QStringLiteral("Retain Longitudinal Temporal Information Full Dates Option")
         << QStringLiteral("Retain Patient Characteristics Option, free text removed")
         << QStringLiteral("Private attributes removed; UIDs replaced");
    for (QString &value : said)
        value.truncate(64);
    return said;
}

void learn(const DicomFile &file, Session &session) {
    const QString make = QStringLiteral("%1 %2 %3")
                             .arg(file.string(Tag::Manufacturer), file.string(Tag::ManufacturerModel), file.string(Tag::Modality))
                             .toCaseFolded();
    learnFrom(file, file.elements(), make, session);
}

QList<Failure> verify(const DicomFile &copy, const Session &session) {
    QList<Failure> failures;
    look(copy, copy.elements(), session, failures);
    return failures;
}

// The decoder keeps grey values of 8 and 16 bits as stored and colour of 8
// bits a sample; anything else it rescales to show, which is no way to
// write a copy.
Redraw canRedraw(const DicomFile &file) {
    using Cant = Redraw::Cant;
    if (!file.hasPixelData())
        return {Cant::NoPicture};
    if (isVideo(file.transferSyntax()))
        return {Cant::Video};
    const ImageFormat f = ImageFormat::from(file);
    if (f.rows <= 0 || f.columns <= 0)
        return {Cant::NoSize};
    if (f.photometric == "PALETTE COLOR") {
        // A palette of 16-bit entries is shown through their top 8 bits;
        // that loses nothing when the bottom ones repeat them or are zero.
        for (auto [descriptor, data] : {std::pair{Tag::RedPaletteDescriptor, Tag::RedPaletteData},
                                        std::pair{Tag::GreenPaletteDescriptor, Tag::GreenPaletteData},
                                        std::pair{Tag::BluePaletteDescriptor, Tag::BluePaletteData}}) {
            const DicomElement *table = file.element(data);
            if (file.numbers(descriptor).value(2) <= 8 || !table)
                continue;
            for (qsizetype i = 0; i + 1 < table->value.size(); i += 2)
                if (table->value[i] != 0 && table->value[i] != table->value[i + 1])
                    return {Cant::Palette};
        }
        if (f.samples != 1 || (f.bitsAllocated != 8 && f.bitsAllocated != 16))
            return {Cant::GreyBits, f.bitsAllocated};
    } else if (f.samples == 1) {
        if (f.bitsAllocated != 8 && f.bitsAllocated != 16)
            return {Cant::GreyBits, f.bitsAllocated};
    } else if (f.samples != 3 || f.bitsAllocated != 8) {
        return {Cant::ColourBits, f.bitsAllocated};
    }
    return {};
}

bool mayShowDetails(const DicomFile &file) {
    const QString burned = file.first(0x00280301).toUpper();
    if (burned == "YES")
        return true;
    if (burned == "NO")
        return false;
    static const QSet<QString> modalities{"US", "SC", "XC", "OT", "ES", "GM", "SM", "DOC", "IVUS"};
    return modalities.contains(file.first(Tag::Modality))
           || file.first(Tag::SopClass).startsWith("1.2.840.10008.5.1.4.1.1.7");
}

bool write(const DicomFile &file, const QString &path, Session &session, QString *error, const Options &options,
           const QList<QRect> &blackout, Outcome *outcome) {
    if (path.isEmpty()) {
        if (error)
            *error = QT_TRANSLATE_NOOP("dicolens", "The copy couldn't be written");
        return false;
    }
    return process(file, path, session, options, blackout, outcome, error);
}

bool check(const DicomFile &file, Session &session, QString *error, const Options &options,
           const QList<QRect> &blackout, Outcome *outcome) {
    return process(file, {}, session, options, blackout, outcome, error);
}

Result copyScan(const Scan &scan, const QString &folder, const std::atomic<bool> *cancel, const Options &options) {
    return run(scan, &folder, options, cancel);
}

Result plan(const Scan &scan, const Options &options, const std::atomic<bool> *cancel) {
    return run(scan, nullptr, options, cancel);
}

}
