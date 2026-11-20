#include "study.h"

#include <QDate>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <algorithm>
#include <cmath>

#include "pixels.h"

namespace {

void cross(const double *a, const double *b, double *out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A decimal string may spell "nan" or "inf", and a binary value be one.
// Geometry made of those would send every index computed from it anywhere.
bool finite(const QList<double> &values, int count) {
    return values.size() >= count && std::all_of(values.begin(), values.begin() + count, [](double v) { return std::isfinite(v); });
}

// The patient direction a vector points towards, strongest axis first, as
// radiologists label an image's edges.
QString direction(const double *v) {
    QString label;
    const char *positive = "LPH", *negative = "RAF";
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [v](int a, int b) { return std::abs(v[a]) > std::abs(v[b]); });
    for (int axis : order)
        if (std::abs(v[axis]) > 0.25)
            label += QChar(v[axis] > 0 ? positive[axis] : negative[axis]);
    return label;
}

}

QStringList Series::edgeLabels() const {
    if (!hasOrientation)
        return {};
    return ::edgeLabels(orientation, orientation + 3);
}

QStringList edgeLabels(const double *row, const double *column) {
    const double left[3] = {-row[0], -row[1], -row[2]};
    const double top[3] = {-column[0], -column[1], -column[2]};
    return {direction(left), direction(row), direction(top), direction(column)};
}

namespace {

// Seconds into the day of a DICOM time, HHMMSS.FFFFFF, or -1.
double seconds(QString time) {
    time = time.trimmed().remove(':');
    if (time.size() < 4)
        return -1;
    bool ok = true;
    const double h = time.mid(0, 2).toDouble(&ok), m = time.mid(2, 2).toDouble();
    const double s = time.size() > 4 ? time.mid(4).toDouble() : 0;
    return ok ? h * 3600 + m * 60 + s : -1;
}

}

double suvFactor(const DicomFile &file) {
    const QString units = file.first(0x00541001).toUpper();
    if (units == "GML")
        return 1;   // Already SUVs.
    if (units != "BQML")
        return 0;
    const double weight = file.numbers(0x00101030).value(0);   // kg
    const DicomElement *drugs = file.element(0x00540016);
    if (weight <= 0 || !drugs || drugs->items.isEmpty())
        return 0;
    double dose = 0, halfLife = 0;
    QString injected, injectedDate;
    for (const auto &e : drugs->items.first()) {
        if (e.tag == 0x00181074)
            dose = file.numbers(&e).value(0);   // Bq
        else if (e.tag == 0x00181075)
            halfLife = file.numbers(&e).value(0);   // s
        else if (e.tag == 0x00181072 && injected.isEmpty())
            injected = file.decode(e.value).trimmed();
        else if (e.tag == 0x00181078) {
            // A date and time, which beats the time alone.
            const QString dt = file.decode(e.value).trimmed();
            if (dt.size() >= 12) {
                injectedDate = dt.left(8);
                injected = dt.mid(8);
            }
        }
    }
    if (dose <= 0 || halfLife <= 0)
        return 0;
    const QString correction = file.first(0x00541102).toUpper();
    if (correction == "ADMIN")
        return weight * 1000 / dose;
    if (!correction.isEmpty() && correction != "START")
        return 0;   // Uncorrected values decay slice by slice.
    // Corrected to the series' start: the dose as it had decayed by then.
    const double start = seconds(file.first(0x00080031)), at = seconds(injected);
    if (start < 0 || at < 0)
        return 0;
    double elapsed = start - at;
    const QDate day = QDate::fromString(file.first(0x00080021), "yyyyMMdd");
    const QDate then = QDate::fromString(injectedDate, "yyyyMMdd");
    if (day.isValid() && then.isValid())
        elapsed += then.daysTo(day) * 86400.0;
    else if (elapsed < 0)
        elapsed += 86400;   // Injected before midnight.
    if (elapsed < 0)
        return 0;
    return weight * 1000 / (dose * std::pow(0.5, elapsed / halfLife));
}

QStringList listFiles(const QStringList &paths, bool recursive) {
    QStringList files;
    for (const auto &path : paths) {
        const QFileInfo info(path);
        if (info.isFile()) {
            files << info.absoluteFilePath();
        } else if (info.isDir()) {
            QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot,
                            recursive ? QDirIterator::Subdirectories | QDirIterator::FollowSymlinks
                                      : QDirIterator::NoIteratorFlags);
            while (it.hasNext())
                files << it.next();
        }
    }
    std::sort(files.begin(), files.end());
    files.removeDuplicates();
    return files;
}

void addFile(Scan &scan, const DicomFile &file) {
    const int rows = file.integer(Tag::Rows), columns = file.integer(Tag::Columns);
    if (!file.hasPixelData() || rows <= 0 || columns <= 0) {
        Report report;
        if (Reports::describe(file, &report)) {
            ++scan.files;
            scan.reports << report;
        } else if (Marks::isPresentationState(file)) {
            ++scan.files;
            scan.marks += Marks::read(file);
            scan.annotations << file.path();
        } else {
            ++scan.skipped;
        }
        return;
    }
    ++scan.files;

    // Localizers and scout views share a series with differently sized
    // images often enough that size is part of what makes a series.
    QString uid = file.first(Tag::SeriesInstance);
    if (uid.isEmpty())
        uid = QFileInfo(file.path()).absolutePath();
    const QString key = QStringLiteral("%1/%2x%3").arg(uid).arg(columns).arg(rows);

    auto it = std::find_if(scan.series.begin(), scan.series.end(), [&](const Series &s) {
        return QStringLiteral("%1/%2x%3").arg(s.uid).arg(s.columns).arg(s.rows) == key;
    });
    if (it == scan.series.end()) {
        Series s;
        s.uid = uid;
        s.rows = rows;
        s.columns = columns;
        s.number = file.first(Tag::SeriesNumber);
        s.description = file.string(Tag::SeriesDescription);
        s.modality = file.first(Tag::Modality);
        s.bodyPart = file.first(Tag::BodyPart);
        s.patientName = DicomFile::formatName(file.string(Tag::PatientName));
        s.patientId = file.string(Tag::PatientId);
        s.patientBirthDate = file.first(Tag::PatientBirthDate);
        s.patientSex = file.first(Tag::PatientSex);
        s.patientAge = file.first(Tag::PatientAge);
        s.studyUid = file.first(Tag::StudyInstance);
        s.studyDescription = file.string(Tag::StudyDescription);
        s.studyDate = file.first(Tag::StudyDate);
        if (s.studyDate.isEmpty())
            s.studyDate = file.first(Tag::SeriesDate);
        s.studyTime = file.first(Tag::StudyTime);
        s.institution = file.string(Tag::Institution);
        s.frame = file.first(0x00200052);
        if (s.modality == "PT")
            s.suv = suvFactor(file);
        s.transferSyntax = file.transferSyntax();
        s.color = ImageFormat::from(file).color();

        auto spacing = file.numbers(file.find(Tag::PixelSpacing, 0));
        if (spacing.size() < 2)
            spacing = file.numbers(Tag::ImagerPixelSpacing);
        if (finite(spacing, 2) && spacing[0] > 0 && spacing[1] > 0) {
            s.hasSpacing = true;
            s.spacing[0] = spacing[0];
            s.spacing[1] = spacing[1];
        }
        const auto thickness = file.numbers(file.find(Tag::SliceThickness, 0));
        s.thickness = finite(thickness, 1) ? thickness.first() : 0;
        const auto orientation = file.numbers(file.find(Tag::ImageOrientation, 0));
        if (finite(orientation, 6)) {
            s.hasOrientation = true;
            for (int i = 0; i < 6; ++i)
                s.orientation[i] = orientation[i];
        }
        if (const double rate = file.number(Tag::RecommendedFrameRate); rate > 0)
            s.frameRate = rate;
        else if (const double cine = file.number(Tag::CineRate); cine > 0)
            s.frameRate = cine;
        else if (const double ms = file.number(Tag::FrameTime); ms > 0)
            s.frameRate = 1000.0 / ms;
        // Only areas measured in centimetres (units 3) both ways give a
        // length; others are time, velocity, or not calibrated at all.
        if (const DicomElement *regions = file.element(Tag::UltrasoundRegions))
            for (const auto &item : regions->items) {
                auto value = [&](quint32 tag) {
                    for (const auto &e : item)
                        if (e.tag == tag)
                            return file.numbers(&e).value(0, -1);
                    return -1.0;
                };
                if (value(Tag::PhysicalUnitsX) != 3 || value(Tag::PhysicalUnitsY) != 3)
                    continue;
                Region r;
                r.x0 = int(value(Tag::RegionMinX));
                r.y0 = int(value(Tag::RegionMinY));
                r.x1 = int(value(Tag::RegionMaxX));
                r.y1 = int(value(Tag::RegionMaxY));
                r.deltaX = std::abs(value(Tag::PhysicalDeltaX)) * 10;
                r.deltaY = std::abs(value(Tag::PhysicalDeltaY)) * 10;
                if (std::isfinite(r.deltaX) && std::isfinite(r.deltaY) && r.deltaX > 0 && r.deltaY > 0 && r.x1 >= r.x0
                    && r.y1 >= r.y0)
                    s.regions << r;
            }
        scan.series.append(s);
        it = scan.series.end() - 1;
    }

    const int frames = std::max(1, file.integer(Tag::NumberOfFrames, 1));
    const int instance = file.integer(Tag::InstanceNumber);
    for (int frame = 0; frame < frames; ++frame) {
        Slice slice;
        slice.path = file.path();
        slice.sop = file.first(Tag::SopInstance);
        slice.frame = frame;
        slice.instance = instance;
        const auto position = file.numbers(file.find(Tag::ImagePosition, frame));
        if (finite(position, 3)) {
            slice.hasPosition = true;
            for (int i = 0; i < 3; ++i)
                slice.position[i] = position[i];
        }
        if (const auto *location = file.find(Tag::SliceLocation, frame)) {
            const auto values = file.numbers(location);
            if (finite(values, 1)) {
                slice.location = values.first();
                slice.hasLocation = true;
            }
        }
        it->slices.append(slice);
    }
}

void sortScan(Scan &scan) {
    for (auto &s : scan.series) {
        double normal[3] = {0, 0, 1};
        cross(s.orientation, s.orientation + 3, normal);
        const bool positioned = s.hasOrientation && std::all_of(s.slices.begin(), s.slices.end(),
                                                                [](const Slice &x) { return x.hasPosition; });
        if (positioned)
            for (auto &slice : s.slices) {
                slice.location = dot(slice.position, normal);
                slice.hasLocation = true;
            }

        // Frames of one file stay in their order; across files, sort by
        // where the slices are, in the direction the scanner numbered them.
        auto byInstance = [](const Slice &a, const Slice &b) {
            if (a.instance != b.instance)
                return a.instance < b.instance;
            if (a.path != b.path)
                return a.path < b.path;
            return a.frame < b.frame;
        };
        std::stable_sort(s.slices.begin(), s.slices.end(), byInstance);
        if (positioned && s.slices.size() > 1) {
            int agree = 0;
            for (int i = 1; i < s.slices.size(); ++i)
                agree += s.slices[i].location > s.slices[i - 1].location ? 1
                         : s.slices[i].location < s.slices[i - 1].location ? -1 : 0;
            const bool ascending = agree >= 0;
            std::stable_sort(s.slices.begin(), s.slices.end(), [&](const Slice &a, const Slice &b) {
                if (a.location != b.location)
                    return ascending ? a.location < b.location : a.location > b.location;
                return byInstance(a, b);
            });
        }
    }

    // Studies in date order, and their series in the scanner's order.
    std::stable_sort(scan.series.begin(), scan.series.end(), [](const Series &a, const Series &b) {
        const QString ka = a.studyDate + a.studyTime + a.studyUid, kb = b.studyDate + b.studyTime + b.studyUid;
        if (ka != kb)
            return ka < kb;
        bool okA = false, okB = false;
        const int na = a.number.toInt(&okA), nb = b.number.toInt(&okB);
        if (okA && okB && na != nb)
            return na < nb;
        return a.description < b.description;
    });
    std::stable_sort(scan.reports.begin(), scan.reports.end(), [](const Report &a, const Report &b) {
        const QString ka = a.studyDate + a.studyTime + a.studyUid, kb = b.studyDate + b.studyTime + b.studyUid;
        return ka != kb ? ka < kb : a.date < b.date;
    });
}

Scan scanFiles(const QStringList &files, const std::function<void(int, int)> &progress, const std::atomic<bool> *cancel) {
    Scan scan;
    for (int i = 0; i < files.size(); ++i) {
        if (cancel && *cancel)
            break;
        if (progress && (i % 16 == 0))
            progress(i, files.size());
        const QFileInfo info(files[i]);
        if (info.fileName().compare("DICOMDIR", Qt::CaseInsensitive) == 0) {
            ++scan.skipped;
            continue;
        }
        QString error;
        const DicomFile file = DicomFile::read(files[i], &error);
        if (!file.isValid()) {
            // A folder holds other files too; only those that claim to be
            // DICOM and fail are worth reporting.
            if (error != "Not a DICOM file")
                scan.unreadable << info.fileName() + ": " + error;
            ++scan.skipped;
            continue;
        }
        addFile(scan, file);
    }
    if (progress)
        progress(files.size(), files.size());
    sortScan(scan);
    return scan;
}
