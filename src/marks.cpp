#include "marks.h"

namespace {

const DicomElement *in(const QList<DicomElement> &elements, quint32 tag) {
    for (const auto &e : elements)
        if (e.tag == tag)
            return &e;
    return nullptr;
}

QString word(const DicomFile &file, const DicomElement *e) {
    return e ? file.decode(e->value).remove(QChar('\0')).trimmed().toUpper() : QString();
}

QPointF point(const QList<double> &values, int i = 0) {
    return {values.value(i * 2), values.value(i * 2 + 1)};
}

// The images a sequence of references names, with their frames.
QList<QPair<QString, int>> references(const DicomFile &file, const DicomElement *images) {
    QList<QPair<QString, int>> out;
    if (!images)
        return out;
    for (const auto &item : images->items) {
        const DicomElement *sop = in(item, Tag::ReferencedSopInstance);
        if (!sop)
            continue;
        const QString uid = file.decode(sop->value).remove(QChar('\0')).trimmed();
        const QList<double> frames = file.numbers(in(item, Tag::ReferencedFrameNumber));
        if (frames.isEmpty())
            out.append({uid, 0});
        // (0 means every frame, so a frame that is no number is none.)
        for (double f : frames)
            out.append({uid, toInt(f, -1)});
    }
    return out;
}

} // namespace

bool Mark::appliesTo(const QString &sop, int frame) const {
    for (const auto &[uid, f] : images)
        if (uid == sop && (f == 0 || f == frame + 1))
            return true;
    return false;
}

namespace Marks {

bool isPresentationState(const DicomFile &file) {
    // Grayscale, colour, pseudo-colour, and blending presentation states.
    return file.first(Tag::SopClass).startsWith("1.2.840.10008.5.1.4.1.1.11.") && file.has(Tag::ReferencedSeries);
}

QList<Mark> read(const DicomFile &file) {
    QList<Mark> out;
    // Every image the presentation state applies to; an annotation that
    // names none of its own applies to them all.
    QList<QPair<QString, int>> all;
    if (const DicomElement *series = file.element(Tag::ReferencedSeries))
        for (const auto &item : series->items)
            all += references(file, in(item, Tag::ReferencedImage));

    const DicomElement *annotations = file.element(Tag::GraphicAnnotations);
    if (!annotations)
        return out;
    for (const auto &annotation : annotations->items) {
        QList<QPair<QString, int>> images = references(file, in(annotation, Tag::ReferencedImage));
        if (images.isEmpty())
            images = all;

        if (const DicomElement *graphics = in(annotation, Tag::GraphicObjects))
            for (const auto &g : graphics->items) {
                Mark m;
                m.images = images;
                m.display = word(file, in(g, Tag::GraphicUnits)) == "DISPLAY";
                m.filled = word(file, in(g, Tag::GraphicFilled)) == "Y";
                const QList<double> data = file.numbers(in(g, Tag::GraphicData));
                for (int i = 0; i + 1 < data.size(); i += 2)
                    m.points << QPointF(data[i], data[i + 1]);
                const QString type = word(file, in(g, Tag::GraphicType));
                if (type == "CIRCLE" && m.points.size() >= 2)
                    m.kind = Mark::Kind::Circle;
                else if (type == "ELLIPSE" && m.points.size() >= 4)
                    m.kind = Mark::Kind::Ellipse;
                else if (type == "POINT" || type == "MULTIPOINT")
                    m.kind = Mark::Kind::Points;
                else if (m.points.size() >= 2)
                    m.kind = Mark::Kind::Polyline;  // POLYLINE, and INTERPOLATED drawn straight.
                else
                    continue;
                out << m;
            }

        if (const DicomElement *texts = in(annotation, Tag::TextObjects))
            for (const auto &t : texts->items) {
                Mark m;
                m.kind = Mark::Kind::Text;
                m.images = images;
                m.text = file.decode(in(t, Tag::UnformattedText) ? in(t, Tag::UnformattedText)->value : QByteArray())
                             .remove(QChar('\0'))
                             .trimmed();
                if (m.text.isEmpty())
                    continue;
                const QList<double> tl = file.numbers(in(t, Tag::BoundingBoxTopLeft));
                const QList<double> br = file.numbers(in(t, Tag::BoundingBoxBottomRight));
                if (tl.size() >= 2 && br.size() >= 2) {
                    m.hasBox = true;
                    m.box = QRectF(point(tl), point(br)).normalized();
                    m.boxDisplay = word(file, in(t, Tag::BoundingBoxUnits)) == "DISPLAY";
                }
                const QList<double> anchor = file.numbers(in(t, Tag::AnchorPoint));
                if (anchor.size() >= 2) {
                    m.hasAnchor = true;
                    m.anchor = point(anchor);
                    m.anchorDisplay = word(file, in(t, Tag::AnchorPointUnits)) == "DISPLAY";
                    m.anchorLine = word(file, in(t, Tag::AnchorPointVisibility)) == "Y";
                }
                if (!m.hasBox && !m.hasAnchor)
                    continue;
                out << m;
            }
    }
    return out;
}

QImage overlay(const DicomFile &file, int frame, int rows, int columns) {
    QImage mask;
    for (int group = 0x6000; group <= 0x601E; group += 2) {
        const quint32 g = quint32(group) << 16;
        const DicomElement *data = file.element(g | (Tag::OverlayData & 0xFFFF));
        if (!data || data->value.isEmpty())
            continue;
        const int oRows = file.integer(g | (Tag::OverlayRows & 0xFFFF));
        const int oColumns = file.integer(g | (Tag::OverlayColumns & 0xFFFF));
        // Rows and columns are 16-bit in the standard; a file that says more
        // has no overlay worth walking through.
        if (oRows <= 0 || oColumns <= 0 || oRows > 65535 || oColumns > 65535)
            continue;
        // Overlays of a multi-frame image start at a frame of their own.
        const int frames = std::max(1, file.integer(g | (Tag::OverlayFrames & 0xFFFF), 1));
        const int first = std::max(1, file.integer(g | (Tag::OverlayFrameOrigin & 0xFFFF), 1)) - 1;
        const qint64 index = frames > 1 ? qint64(frame) - first : 0;
        if (index < 0 || index >= frames)
            continue;
        const QList<double> origin = file.numbers(file.element(g | (Tag::OverlayOrigin & 0xFFFF)));
        // Where the overlay starts is the file's word too: kept wide, so
        // that a row or a column added to it can't overflow.
        const qint64 top = qint64(toInt(origin.value(0, 1))) - 1, left = qint64(toInt(origin.value(1, 1))) - 1;

        if (mask.isNull()) {
            mask = QImage(columns, rows, QImage::Format_Alpha8);
            if (mask.isNull())
                return mask;
            mask.fill(0);
        }
        // Bits run along each row, the lowest bit of each byte first.
        const auto *bits = reinterpret_cast<const uchar *>(data->value.constData());
        const qint64 available = qint64(data->value.size()) * 8;
        const qint64 plane = qint64(oRows) * oColumns;
        // A frame the data doesn't reach has nothing to show.
        if (index >= available / plane)
            continue;
        const qint64 start = index * plane;
        for (int r = 0; r < oRows && start + qint64(r) * oColumns < available; ++r) {
            const qint64 y = r + top;
            if (y < 0 || y >= rows)
                continue;
            uchar *line = mask.scanLine(int(y));
            for (int c = 0; c < oColumns; ++c) {
                const qint64 bit = start + qint64(r) * oColumns + c;
                if (bit >= available)
                    break;
                const qint64 x = c + left;
                if (x >= 0 && x < columns && (bits[bit >> 3] >> (bit & 7)) & 1)
                    line[x] = 255;
            }
        }
    }
    return mask;
}

}
