#include "study.h"

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
    const double *row = orientation, *column = orientation + 3;
    const double left[3] = {-row[0], -row[1], -row[2]};
    const double top[3] = {-column[0], -column[1], -column[2]};
    return {direction(left), direction(row), direction(top), direction(column)};
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
        ++scan.skipped;
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
        s.transferSyntax = file.transferSyntax();
        s.color = ImageFormat::from(file).color();

        auto spacing = file.numbers(file.find(Tag::PixelSpacing, 0));
        if (spacing.size() < 2)
            spacing = file.numbers(Tag::ImagerPixelSpacing);
        if (spacing.size() >= 2 && spacing[0] > 0 && spacing[1] > 0) {
            s.hasSpacing = true;
            s.spacing[0] = spacing[0];
            s.spacing[1] = spacing[1];
        }
        s.thickness = file.numbers(file.find(Tag::SliceThickness, 0)).value(0);
        const auto orientation = file.numbers(file.find(Tag::ImageOrientation, 0));
        if (orientation.size() >= 6) {
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
        scan.series.append(s);
        it = scan.series.end() - 1;
    }

    const int frames = std::max(1, file.integer(Tag::NumberOfFrames, 1));
    const int instance = file.integer(Tag::InstanceNumber);
    for (int frame = 0; frame < frames; ++frame) {
        Slice slice;
        slice.path = file.path();
        slice.frame = frame;
        slice.instance = instance;
        const auto position = file.numbers(file.find(Tag::ImagePosition, frame));
        if (position.size() >= 3) {
            slice.hasPosition = true;
            for (int i = 0; i < 3; ++i)
                slice.position[i] = position[i];
        }
        if (const auto *location = file.find(Tag::SliceLocation, frame)) {
            const auto values = file.numbers(location);
            if (!values.isEmpty()) {
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
