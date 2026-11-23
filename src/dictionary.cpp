// What a tag is called and how its value is written, from the standard's
// own registry: every data element of DICOM Part 6 and every UID, kept as
// tables in src/data (tools/standard.py writes them from the standard) and
// compiled in. The VR also tells the parser how to read implicit files,
// which say no value's kind, so the whole registry is here rather than the
// tags people look at.
#include "dicom.h"

#include <QFile>
#include <QHash>
#include <algorithm>
#include <vector>

// The tables are compiled into the library. A static library's resources
// are only linked in, and found, once something asks for them by name, and
// the asking has to be outside any namespace.
static void useTables() { Q_INIT_RESOURCE(data); }

namespace {

struct Element {
    quint32 tag = 0;
    QByteArrayView vrs;    // As the standard writes it: "US", or "US or SS".
    QByteArrayView name;
    bool retired = false;
};

// A tag that stands for many: (60xx,3000) is Overlay Data in every even
// group from 6000 to 60FE. A tag is one of them when its digits under the
// mask are the pattern's.
struct Pattern {
    quint32 mask = 0;
    quint32 digits = 0;
    Element element;
};

struct Elements {
    QByteArray text;                  // The table itself; names point into it.
    std::vector<Element> exact;       // Sorted by tag.
    std::vector<Pattern> patterns;
    QString edition;
};

struct Uid {
    QString kind;
    QString name;
};

// Read once, the first time anything asks: listing a CD of explicit files,
// which say their own VRs, never does. Decoding runs on worker threads, and
// a function's static is made by one of them while the others wait.
const Elements &elements() {
    static const Elements table = [] {
        Elements t;
        t.text = Dictionary::table(":/data/elements.tsv", &t.edition, [&](const QList<QByteArrayView> &cells) {
            quint32 mask = 0, digits = 0;
            if (cells.size() < 4 || !Dictionary::pattern(cells[0], &mask, &digits))
                return;
            const Element element{digits, cells[1], cells[3], cells[2] == "R"};
            if (mask == 0xFFFFFFFF)
                t.exact.push_back(element);
            else
                t.patterns.push_back({mask, digits, element});
        });
        std::sort(t.exact.begin(), t.exact.end(), [](const Element &a, const Element &b) { return a.tag < b.tag; });
        return t;
    }();
    return table;
}

const Element *find(quint32 tag) {
    const Elements &t = elements();
    const auto it = std::lower_bound(t.exact.begin(), t.exact.end(), tag,
                                     [](const Element &e, quint32 tag) { return e.tag < tag; });
    if (it != t.exact.end() && it->tag == tag)
        return &*it;
    // Odd groups are private, whatever the standard repeats beside them.
    if ((tag >> 16) & 1)
        return nullptr;
    for (const Pattern &p : t.patterns)
        if ((tag & p.mask) == p.digits)
            return &p.element;
    return nullptr;
}

const QHash<QString, Uid> &uids() {
    static const QHash<QString, Uid> table = [] {
        QHash<QString, Uid> t;
        Dictionary::table(":/data/uids.tsv", nullptr, [&](const QList<QByteArrayView> &cells) {
            if (cells.size() < 3)
                return;
            // The standard adds what a transfer syntax is the default for
            // after a colon, which isn't part of what it's called.
            QString name = QString::fromUtf8(cells[2]);
            if (const qsizetype colon = name.indexOf(": "); colon > 0)
                name.truncate(colon);
            t.insert(QString::fromLatin1(cells[0]), {QString::fromLatin1(cells[1]), name});
        });
        return t;
    }();
    return table;
}

QString uidOfKind(const QString &uid, QLatin1StringView kind, QLatin1StringView otherKind = {}) {
    const auto it = uids().constFind(uid);
    if (it == uids().constEnd() || (it->kind != kind && (otherKind.isEmpty() || it->kind != otherKind)))
        return {};
    return it->name;
}

}

namespace Dictionary {

QByteArray table(const char *resource, QString *edition, const std::function<void(const QList<QByteArrayView> &)> &row) {
    static const QByteArray marker("DICOM PS3.");
    static const bool linked = (useTables(), true);
    Q_UNUSED(linked)
    QFile file(QString::fromLatin1(resource));
    // Without its tables every implicit file would be read as unknown values
    // and the anonymizer would have no profile, each quietly: better to stop.
    if (!file.open(QIODevice::ReadOnly))
        qFatal("The standard's table %s isn't compiled in", resource);
    const QByteArray text = file.readAll();
    QList<QByteArrayView> cells;
    for (qsizetype at = 0; at < text.size();) {
        qsizetype end = text.indexOf('\n', at);
        if (end < 0)
            end = text.size();
        const QByteArrayView line(text.constData() + at, end - at);
        at = end + 1;
        if (line.startsWith('#')) {
            // The first comment says which edition this is: "DICOM PS3.6 2026d, …".
            const qsizetype part = line.indexOf(marker);
            if (edition && edition->isEmpty() && part >= 0) {
                const qsizetype from = line.indexOf(' ', part + marker.size()) + 1;
                const qsizetype to = line.indexOf(',', from);
                if (from > 0 && to > from)
                    *edition = QString::fromLatin1(line.sliced(from, to - from));
            }
            continue;
        }
        cells.clear();
        for (qsizetype cell = 0; cell <= line.size();) {
            qsizetype tab = line.indexOf('\t', cell);
            if (tab < 0)
                tab = line.size();
            cells << line.sliced(cell, tab - cell);
            cell = tab + 1;
        }
        row(cells);
    }
    return text;
}

bool pattern(QByteArrayView text, quint32 *mask, quint32 *digits) {
    if (text.size() != 8)
        return false;
    *mask = *digits = 0;
    for (const char c : text) {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c == 'x' ? 0 : -1;
        if (digit < 0)
            return false;
        *mask = *mask << 4 | (c == 'x' ? 0x0 : 0xF);
        *digits = *digits << 4 | quint32(digit);
    }
    return true;
}

QString name(quint32 tag) {
    if (const Element *e = find(tag))
        return QString::fromUtf8(e->name);
    // Every group's element 0000 is its length; odd groups are private.
    if ((tag & 0xFFFF) == 0)
        return QStringLiteral("Group Length");
    if ((tag >> 16) & 1)
        return QStringLiteral("Private");
    return {};
}

QByteArray vr(quint32 tag) {
    if (const Element *e = find(tag)) {
        // Where the standard allows several, one has to be chosen to read
        // an implicit file by. A value that may be OW is a table or
        // samples, bytes whichever it is; otherwise it's "US or SS", which
        // only the file's Pixel Representation settles, so it's read as US
        // and those who use the value (the padding, a LUT's first entry)
        // make it signed when the pixels are.
        if (e->vrs.size() > 2)
            return e->vrs.contains("OW") ? QByteArray("OW") : e->vrs.first(2).toByteArray();
        return e->vrs.toByteArray();
    }
    if ((tag & 0xFFFF) == 0)
        return "UL";
    return {};
}

QString vrs(quint32 tag) {
    const Element *e = find(tag);
    return e ? QString::fromLatin1(e->vrs) : QString();
}

bool retired(quint32 tag) {
    const Element *e = find(tag);
    return e && e->retired;
}

int count() { return int(elements().exact.size() + elements().patterns.size()); }

QString edition() { return elements().edition; }

QString transferSyntaxName(const QString &uid) { return uidOfKind(uid, QLatin1StringView("Transfer Syntax")); }

QString sopClassName(const QString &uid) {
    return uidOfKind(uid, QLatin1StringView("SOP Class"), QLatin1StringView("Meta SOP Class"));
}

QString uidName(const QString &uid) { return uids().value(uid).name; }

}
