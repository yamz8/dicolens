#include "readout.h"

#include <QColor>
#include <QCoreApplication>
#include <QDate>
#include <QLocale>
#include <QStringList>
#include <QTime>
#include <cmath>

namespace {

// Qt's own tr() signature, so a counted string reads the same here as in a class.
QString tr(const char *text, const char *disambiguation = nullptr, int n = -1) {
    return QCoreApplication::translate("dicolens", text, disambiguation, n);
}

QString age(const QString &value) {
    // "054Y", "006M", "003W", "012D".
    if (value.size() < 2)
        return value;
    bool ok = false;
    const int n = value.left(value.size() - 1).toInt(&ok);
    if (!ok)
        return value;
    switch (value.back().toUpper().unicode()) {
    case 'Y': return tr("%Ln years old", nullptr, n);
    case 'M': return tr("%Ln months old", nullptr, n);
    case 'W': return tr("%Ln weeks old", nullptr, n);
    case 'D': return tr("%Ln days old", nullptr, n);
    }
    return value;
}

QString sex(const QString &value) {
    if (value == "M")
        return tr("Male");
    if (value == "F")
        return tr("Female");
    if (value == "O")
        return tr("Other");
    return {};
}

QString unit(const Series &s) {
    if (s.suv > 0)
        return QStringLiteral("SUV");
    return s.modality == "CT" ? QStringLiteral("HU") : QString();
}

}

namespace Readout {

QString number(double value, int decimals) { return QLocale().toString(value, 'f', decimals); }

QString date(const QString &dicomDate) {
    const QDate date = QDate::fromString(DicomFile::formatDate(dicomDate), Qt::ISODate);
    return date.isValid() ? QLocale().toString(date, QLocale::ShortFormat) : dicomDate;
}

QVariantMap info(const Series &s, const Planes &planes, const QString &planeName, const QString &slabName) {
    QStringList details;
    if (!s.patientId.isEmpty())
        details << s.patientId;
    if (!s.patientBirthDate.isEmpty())
        details << tr("Born %1").arg(date(s.patientBirthDate));
    if (!s.patientAge.isEmpty())
        details << age(s.patientAge);
    if (!sex(s.patientSex).isEmpty())
        details << sex(s.patientSex);

    QString when = date(s.studyDate);
    const QTime time = QTime::fromString(DicomFile::formatTime(s.studyTime), "hh:mm:ss");
    if (time.isValid())
        when += "  " + QLocale().toString(time, QLocale::ShortFormat);

    QString series = s.description;
    if (!s.number.isEmpty())
        series = series.isEmpty() ? tr("Series %1").arg(s.number) : tr("Series %1: %2").arg(s.number, series);

    QStringList geometry;
    if (planes.plane() != 0) {
        geometry << planeName;
    } else {
        const Slice &slice = s.slices.value(planes.index());
        if (slice.hasLocation)
            geometry << tr("Location %1 mm").arg(number(slice.location, 1));
        if (s.thickness > 0 && planes.slab() == 0)
            geometry << tr("Thickness %1 mm").arg(number(s.thickness, 1));
    }
    if (planes.slab() != 0)
        geometry << slabName;
    const QSize size = planes.size();

    return {
        {"patient", s.patientName},
        {"patientDetails", details.join("  ·  ")},
        {"study", s.studyDescription},
        {"date", when.trimmed()},
        {"institution", s.institution},
        {"series", series},
        {"modality", s.modality},
        {"geometry", geometry.join("  ·  ")},
        {"size", QStringLiteral("%1 × %2").arg(QLocale().toString(size.width()), QLocale().toString(size.height()))},
    };
}

QString value(const Series &s, const Frame &frame, int x, int y) {
    const QString where = QStringLiteral("%1, %2").arg(QLocale().toString(x), QLocale().toString(y));
    if (frame.isColor()) {
        const QColor c = frame.color.pixelColor(x, y);
        return tr("%1   R %2  G %3  B %4").arg(where, QLocale().toString(c.red()), QLocale().toString(c.green()),
                                               QLocale().toString(c.blue()));
    }
    const double v = frame.value(x, y);
    const bool whole = frame.slope == std::floor(frame.slope) && frame.intercept == std::floor(frame.intercept);
    const QString shown = number(v, whole ? 0 : 2);
    if (s.suv > 0)
        return tr("%1   %2 SUV").arg(where, number(v * s.suv, 2));
    if (s.modality == "CT")
        return tr("%1   %2 HU").arg(where, shown);
    return QStringLiteral("%1   %2").arg(where, shown);
}

QString angle(double degrees) {
    return tr("%1°").arg(number(degrees, 1));
}

QString measure(const Series &s, const Measure &m, const MeasureScale &scale, const Frame *frame) {
    // An area's values come from the picture; without one there are none.
    if (m.kind == 2 && (!frame || frame->isNull() || !frame->error.isEmpty()))
        return {};
    const MeasureValues v = measureValues(m, scale, m.kind == 2 ? frame : nullptr, s.suv > 0 ? s.suv : 1);
    if (!v.valid)
        return {};
    if (m.kind == 3 || m.kind == 4)
        return m.kind == 4 ? tr("Cobb %1").arg(angle(v.angle)) : angle(v.angle);
    if (m.kind == 1) {
        if (!v.millimetres)
            return tr("%1 px").arg(number(v.length, 0));
        return v.length >= 100 ? tr("%1 cm").arg(number(v.length / 10, 1)) : tr("%1 mm").arg(number(v.length, 1));
    }
    const QString area = !v.millimetres ? tr("%1 px²").arg(number(v.area, 0))
                         : v.area >= 100 ? tr("%1 cm²").arg(number(v.area / 100, 2))
                                         : tr("%1 mm²").arg(number(v.area, 1));
    QStringList lines{tr("Area %1").arg(area)};
    if (v.count == 0)
        return lines.join('\n');
    const QString u = unit(s);
    const int decimals = u == "SUV" ? 2 : (u == "HU" || (frame->slope == 1 && frame->intercept == std::floor(frame->intercept))) ? 0 : 2;
    auto shown = [&](double x, int extra = 0) { return u.isEmpty() ? number(x, decimals + extra) : number(x, decimals + extra) + " " + u; };
    lines << tr("Mean %1  ·  SD %2").arg(shown(v.mean, decimals == 0 ? 1 : 0), number(v.sd, decimals == 0 ? 1 : decimals));
    lines << tr("Min %1  ·  Max %2").arg(shown(v.low), shown(v.high));
    return lines.join('\n');
}

}
