#include "reports.h"

#include <QCoreApplication>
#include <functional>

namespace {

const char *const PdfClass = "1.2.840.10008.5.1.4.1.1.104.1";
// Every structured report class (Basic Text, Enhanced, Comprehensive, Key
// Object Selection, and the rest) lives under this root.
const char *const StructuredRoot = "1.2.840.10008.5.1.4.1.1.88.";

const DicomElement *in(const QList<DicomElement> &elements, quint32 tag) {
    for (const auto &e : elements)
        if (e.tag == tag)
            return &e;
    return nullptr;
}

QString valueOf(const DicomFile &file, const DicomElement *e) {
    if (!e)
        return {};
    QString value = file.decode(e->value);
    while (value.endsWith(QChar('\0')))
        value.chop(1);
    return value.trimmed();
}

// What a coded item says: its meaning, as the code's own words.
QString meaning(const DicomFile &file, const DicomElement *sequence) {
    if (!sequence || sequence->items.isEmpty())
        return {};
    return valueOf(file, in(sequence->items.first(), Tag::CodeMeaning));
}

QString units(const DicomFile &file, const QList<DicomElement> &measured) {
    const DicomElement *codes = in(measured, Tag::MeasurementUnits);
    if (!codes || codes->items.isEmpty())
        return {};
    // UCUM codes read well ("mm", "cm2") except the ones in braces, which
    // stand for unitless things ("{ratio}"); those read better as words.
    const QString code = valueOf(file, in(codes->items.first(), 0x00080100));
    if (code == "1" || code.isEmpty() || code.startsWith('{'))
        return code == "1" ? QString() : meaning(file, codes);
    return code;
}

} // namespace

namespace Reports {

bool isPdf(const DicomFile &file) {
    return file.first(Tag::SopClass) == PdfClass || file.has(Tag::EncapsulatedDocument);
}

bool isStructured(const DicomFile &file) {
    return file.first(Tag::SopClass).startsWith(StructuredRoot) && file.has(Tag::ContentSequence);
}

bool describe(const DicomFile &file, Report *report) {
    const bool pdf = isPdf(file), structured = !pdf && isStructured(file);
    if (!pdf && !structured)
        return false;
    report->kind = pdf ? Report::Kind::Pdf : Report::Kind::Structured;
    report->path = file.path();
    report->title = pdf ? file.string(Tag::DocumentTitle) : meaning(file, file.element(Tag::ConceptName));
    if (report->title.isEmpty())
        report->title = file.string(Tag::SeriesDescription);
    report->date = file.first(Tag::ContentDate);
    report->studyUid = file.first(Tag::StudyInstance);
    report->studyDescription = file.string(Tag::StudyDescription);
    report->studyDate = file.first(Tag::StudyDate);
    report->studyTime = file.first(Tag::StudyTime);
    report->patientName = DicomFile::formatName(file.string(Tag::PatientName));
    if (report->date.isEmpty())
        report->date = report->studyDate;
    return true;
}

QList<ReportLine> lines(const DicomFile &file) {
    QList<ReportLine> out;
    std::function<void(const QList<DicomElement> &, int)> walk = [&](const QList<DicomElement> &item, int depth) {
        const QString type = valueOf(file, in(item, Tag::ValueType)).toUpper();
        const QString name = meaning(file, in(item, Tag::ConceptName));
        QString value;
        if (type == "TEXT") {
            value = valueOf(file, in(item, Tag::TextValue));
        } else if (type == "CODE") {
            value = meaning(file, in(item, Tag::ConceptCode));
        } else if (type == "NUM") {
            if (const DicomElement *m = in(item, Tag::MeasuredValue); m && !m->items.isEmpty()) {
                const QString number = valueOf(file, in(m->items.first(), Tag::NumericValue));
                const QString unit = units(file, m->items.first());
                value = unit.isEmpty() ? number : number + " " + unit;
            }
        } else if (type == "DATE") {
            value = DicomFile::formatDate(valueOf(file, in(item, Tag::DateValue)));
        } else if (type == "TIME") {
            value = DicomFile::formatTime(valueOf(file, in(item, Tag::TimeValue)));
        } else if (type == "DATETIME") {
            const QString dt = valueOf(file, in(item, Tag::ContentDateTime));
            value = (DicomFile::formatDate(dt.left(8)) + " " + DicomFile::formatTime(dt.mid(8))).trimmed();
        } else if (type == "PNAME") {
            value = DicomFile::formatName(valueOf(file, in(item, Tag::PersonName)));
        }

        const DicomElement *children = in(item, Tag::ContentSequence);
        if (type == "CONTAINER" || (value.isEmpty() && children && !children->items.isEmpty())) {
            // The root's name is the report's title, shown above it.
            if (depth > 0 && !name.isEmpty())
                out << ReportLine{depth - 1, true, name, {}};
        } else if (!name.isEmpty() || !value.isEmpty()) {
            // Images, coordinates, and references have nothing to read.
            if (type != "IMAGE" && type != "COMPOSITE" && type != "WAVEFORM" && !type.startsWith("SCOORD")
                && type != "TCOORD" && type != "UIDREF")
                out << ReportLine{std::max(0, depth - 1), false, name, value};
        }
        if (children)
            for (const auto &child : children->items)
                walk(child, depth + 1);
    };
    walk(file.elements(), 0);
    return out;
}

QString text(const QList<ReportLine> &lines) {
    QStringList out;
    for (const auto &line : lines) {
        const QString indent(line.depth * 2, ' ');
        if (line.heading) {
            if (!out.isEmpty())
                out << QString();
            out << indent + line.name;
        } else if (line.name.isEmpty()) {
            out << indent + line.value;
        } else {
            out << indent + line.name + ": " + line.value;
        }
    }
    return out.join('\n');
}

QByteArray pdf(const DicomFile &file) {
    const DicomElement *e = file.element(Tag::EncapsulatedDocument);
    if (!e)
        return {};
    QByteArray bytes = e->value;
    // An odd-length document is padded to even with a NUL after %%EOF.
    if (bytes.endsWith('\0') && !bytes.endsWith(QByteArray("\0\0", 2)))
        bytes.chop(1);
    return bytes;
}

}
