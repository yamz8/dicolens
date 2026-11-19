#include "anonymize.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace {

// Dicolens' implementation class, a UUID-derived UID of its own.
const char *ImplementationClass = "2.25.294237494695696284811840147955721691334";
const char *ImplementationVersion = "DICOLENS";

const char *ExplicitLittle = "1.2.840.10008.1.2.1";
const char *ImplicitLittle = "1.2.840.10008.1.2";

// What the Basic Profile removes, among what CDs carry: who the patient is,
// who saw them, and where. Person names go too, whatever their tag.
const QSet<quint32> &removed() {
    static const QSet<quint32> tags{
        0x00080014, // Instance Creator UID
        0x00080080, 0x00080081, 0x00080082, // Institution name, address, code
        0x00080092, 0x00080094, 0x00080096, // Referring physician's address, phones, ID
        0x00081010, // Station Name
        0x00081040, 0x00081041, // Institutional department
        0x00081048, 0x00081049, 0x00081050, 0x00081052, // Physicians of record, performing physicians
        0x00081060, 0x00081062, 0x00081070, 0x00081072, // Reading physicians, operators
        0x00081080, 0x00081084, // Admitting diagnoses
        0x00081120, // Referenced Patient Sequence
        0x00100021, 0x00100032, 0x00101000, 0x00101001, 0x00101002, // Issuer, birth time, other IDs and names
        0x00101040, 0x00101050, 0x00101060, 0x00101080, 0x00101081, 0x00101090, // Address, insurance, mother, military, records
        0x00102000, 0x00102110, 0x00102150, 0x00102152, 0x00102154, // Alerts, allergies, country, region, phones
        0x00102160, 0x00102180, 0x001021B0, 0x001021D0, 0x001021F0, // Ethnic group, occupation, history, religion
        0x00102297, 0x00102299, 0x00104000, // Responsible person and organisation, comments
        0x00181000, 0x00181004, 0x00181005, // Device serial number, plate ID, generator ID
        0x00181400, // Acquisition Device Processing Description
        0x00204000, // Image Comments
        0x00321032, 0x00321033, 0x00324000, // Requesting physician and service, study comments
        0x00380010, 0x00380011, 0x00380300, 0x00380400, 0x00380500, 0x00384000, // Admission and visit
        0x00400006, 0x00400241, 0x00400242, 0x00400243, 0x00400253, 0x00400275, // Procedure steps and requests
        0x00401001, 0x00401010, 0x00401400, 0x00402400, 0x00403001, // Requested procedure details
        0x00880200, // Icon Image Sequence: a small picture of its own
        0x4008010C, 0x40080114, 0x4008011A, 0x40080202, // Interpretation authors
    };
    return tags;
}

// Kept, but emptied: the module needs them present.
const QSet<quint32> &emptied() {
    static const QSet<quint32> tags{
        0x00080050, // Accession Number
        0x00080090, // Referring Physician's Name
        0x00100030, // Patient's Birth Date
        0x00200010, // Study ID
    };
    return tags;
}

bool longVr(const QByteArray &vr) {
    static const QSet<QByteArray> vrs{"OB", "OD", "OF", "OL", "OV", "OW", "SQ", "SV", "UC", "UN", "UR", "UT", "UV"};
    return vrs.contains(vr);
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

QString uidFor(const QString &old, Anonymize::Session &session) {
    // Standard UIDs (SOP classes, transfer syntaxes, coding schemes) name
    // kinds of things, not this patient's.
    if (old.isEmpty() || old.startsWith("1.2.840.10008."))
        return old;
    auto it = session.uids.find(old);
    if (it == session.uids.end())
        it = session.uids.insert(old, newUid());
    return *it;
}

QByteArray uids(const QByteArray &value, Anonymize::Session &session) {
    QStringList parts = QString::fromLatin1(value).remove(QChar('\0')).split('\\');
    for (QString &p : parts)
        p = uidFor(p.trimmed(), session);
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

QList<DicomElement> clean(const QList<DicomElement> &elements, Anonymize::Session &session, const DicomFile &file) {
    QList<DicomElement> out;
    for (DicomElement e : elements) {
        const quint16 group = e.tag >> 16;
        if (group == 0x0002 || (group & 1) || (group & 0xFF00) == 0x5000 || removed().contains(e.tag))
            continue;
        if (e.tag == 0x00100010) {
            out << text(e.tag, "PN", "Anonymous");
            continue;
        }
        if (e.tag == 0x00100020) {
            // One stand-in a patient, so a prior still goes with its patient.
            const QString id = file.decode(e.value).trimmed();
            auto it = session.patients.find(id);
            if (it == session.patients.end())
                it = session.patients.insert(id, QStringLiteral("ANON%1").arg(session.patients.size() + 1));
            out << text(e.tag, "LO", it->toLatin1());
            continue;
        }
        if (e.vr == "PN" || emptied().contains(e.tag)) {
            if (emptied().contains(e.tag))
                out << text(e.tag, e.vr, {});
            continue;
        }
        if (e.vr == "UI")
            e.value = uids(e.value, session);
        for (auto &item : e.items)
            item = clean(item, session, file);
        out << e;
    }
    return out;
}

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

void writeElements(QByteArray &out, const QList<DicomElement> &elements, bool explicitVr, const DicomFile &file, bool top) {
    for (const auto &e : elements) {
        if (e.tag == Tag::PixelData) {
            // Only the image's own pixel data is kept on reading; it's
            // copied byte for byte, compressed fragments and all.
            if (!top || !file.hasPixelData())
                continue;
            const QByteArray pixels = file.readPixels(0, file.pixelLength());
            const QByteArray vr = file.pixelVr().isEmpty() ? QByteArray("OW") : file.pixelVr();
            header(out, e.tag, vr == "UN" ? QByteArray(file.encapsulated() ? "OB" : "OW") : vr,
                   file.encapsulated() ? 0xFFFFFFFF : quint32(pixels.size()), explicitVr);
            out.append(pixels);
            continue;
        }
        if (e.vr == "SQ" || !e.items.isEmpty()) {
            header(out, e.tag, "SQ", 0xFFFFFFFF, explicitVr);
            for (const auto &item : e.items) {
                put16(out, 0xFFFE);
                put16(out, 0xE000);
                put32(out, 0xFFFFFFFF);
                writeElements(out, item, explicitVr, file, false);
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

QList<QList<DicomElement>> methodCodes() {
    // The profile and the options it was applied with, as codes.
    auto code = [](const char *value, const char *meaning) {
        return QList<DicomElement>{text(0x00080100, "SH", value), text(0x00080102, "SH", "DCM"),
                                   text(0x00080104, "LO", meaning)};
    };
    return {code("113100", "Basic Application Confidentiality Profile"),
            code("113106", "Retain Longitudinal Temporal Information Full Dates Option"),
            code("113108", "Retain Patient Characteristics Option")};
}

}

namespace Anonymize {

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

bool write(const DicomFile &file, const QString &path, Session &session, QString *error) {
    auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };
    if (!file.isValid())
        return fail(QT_TRANSLATE_NOOP("dicolens", "This file can't be read"));
    if (!file.littleEndian())
        return fail(QT_TRANSLATE_NOOP("dicolens", "Big-endian files can't be copied"));

    // Values are kept as stored, so the copy keeps the original's encoding;
    // a deflated one is written inflated.
    QString syntax = file.transferSyntax();
    if (syntax == "1.2.840.10008.1.2.1.99")
        syntax = ExplicitLittle;
    const bool explicitVr = syntax != ImplicitLittle;

    QList<DicomElement> elements = clean(file.elements(), session, file);
    elements << text(0x00120062, "CS", "YES")
             << text(0x00120063, "LO", "PS3.15 Basic Profile; dates and characteristics kept")
             << sequence(0x00120064, methodCodes())
             << text(0x00280303, "CS", "UNMODIFIED");
    // Replacing an element keeps its place; the added ones are sorted in.
    std::stable_sort(elements.begin(), elements.end(), [](const DicomElement &a, const DicomElement &b) { return a.tag < b.tag; });
    elements.erase(std::unique(elements.begin(), elements.end(),
                               [](const DicomElement &a, const DicomElement &b) { return a.tag == b.tag; }),
                   elements.end());

    QString sopClass, sopInstance;
    for (const auto &e : elements) {
        if (e.tag == Tag::SopClass)
            sopClass = QString::fromLatin1(e.value).remove(QChar('\0')).trimmed();
        else if (e.tag == 0x00080018)
            sopInstance = QString::fromLatin1(e.value).remove(QChar('\0')).trimmed();
    }

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
    writeElements(out, elements, explicitVr, file, true);

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile save(path);
    save.setDirectWriteFallback(false);
    if (!save.open(QIODevice::WriteOnly) || save.write(out) != out.size() || !save.commit())
        return fail(QT_TRANSLATE_NOOP("dicolens", "The copy couldn't be written"));
    return true;
}

Result copyScan(const Scan &scan, const QString &folder, const std::atomic<bool> *cancel) {
    Result result;
    QString root = QDir(folder).filePath("Anonymous scans");
    for (int n = 2; QFileInfo::exists(root); ++n)
        root = QDir(folder).filePath(QStringLiteral("Anonymous scans %1").arg(n));
    if (!QDir().mkpath(root)) {
        result.errors << QStringLiteral("%1: can't be created").arg(root);
        return result;
    }
    result.folder = root;
    result.skipped = scan.reports.size();

    Session session;
    auto copy = [&](const QString &from, const QString &to) {
        QString error;
        const DicomFile file = DicomFile::read(from, &error);
        if (file.isValid() && write(file, to, session, &error)) {
            ++result.written;
            if (mayShowDetails(file))
                ++result.showDetails;
        } else {
            result.errors << QStringLiteral("%1: %2").arg(from, error);
        }
    };
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
        for (int i = 0; i < paths.size(); ++i) {
            if (cancel && *cancel)
                return result;
            copy(paths[i], QStringLiteral("%1/%2.dcm").arg(dir).arg(i + 1, 4, 10, QChar('0')));
        }
    }
    for (int i = 0; i < scan.annotations.size(); ++i)
        copy(scan.annotations[i], QStringLiteral("%1/Annotations/%2.dcm").arg(root).arg(i + 1, 4, 10, QChar('0')));
    return result;
}

}
