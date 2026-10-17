#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>

#include "dicom.h"

// One picture in a series: a file, or one frame of a multi-frame file.
struct Slice {
    QString path;
    int frame = 0;
    int instance = 0;
    bool hasPosition = false;
    double position[3] = {0, 0, 0};
    double location = 0;          // Along the series' normal, in mm.
    bool hasLocation = false;
};

struct Series {
    QString uid;
    QString number;
    QString description;
    QString modality;
    QString bodyPart;
    QString patientName;
    QString patientId;
    QString patientBirthDate;
    QString patientSex;
    QString patientAge;
    QString studyUid;
    QString studyDescription;
    QString studyDate;
    QString studyTime;
    QString institution;
    QString transferSyntax;
    int rows = 0;
    int columns = 0;
    bool color = false;
    bool hasSpacing = false;
    double spacing[2] = {1, 1};   // Row spacing (down), column spacing (across), in mm.
    double thickness = 0;
    bool hasOrientation = false;
    double orientation[6] = {1, 0, 0, 0, 1, 0};
    double frameRate = 0;         // Frames a second, when the file says.
    QList<Slice> slices;

    // Patient directions at the image's edges: left, right, top, bottom
    // ("R", "L", "A", "P", "H", "F", or combinations for oblique planes).
    QStringList edgeLabels() const;
    // Width over height of one pixel on screen.
    double pixelAspect() const { return hasSpacing && spacing[0] > 0 ? spacing[1] / spacing[0] : 1.0; }
};

struct Scan {
    QList<Series> series;
    int files = 0;            // DICOM files read.
    int skipped = 0;          // Files that weren't DICOM images, such as reports or DICOMDIR.
    QStringList unreadable;   // DICOM files that couldn't be read, with the reason.
};

// Files under the paths, recursing into folders, hidden files left out.
QStringList listFiles(const QStringList &paths, bool recursive);

// Reads every file's header and groups them into series, each sorted
// head to foot (or in instance order when positions aren't known).
Scan scanFiles(const QStringList &files, const std::function<void(int done, int total)> &progress = {},
               const std::atomic<bool> *cancel = nullptr);

// Adds one parsed file to a scan. Exposed for tests.
void addFile(Scan &scan, const DicomFile &file);
void sortScan(Scan &scan);
