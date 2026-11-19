#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>

#include "dicom.h"
#include "marks.h"
#include "reports.h"

// One picture in a series: a file, or one frame of a multi-frame file.
struct Slice {
    QString path;
    QString sop;                  // SOP Instance UID, which annotations name.
    int frame = 0;
    int instance = 0;
    bool hasPosition = false;
    double position[3] = {0, 0, 0};
    double location = 0;          // Along the series' normal, in mm.
    bool hasLocation = false;
};

// Part of an ultrasound image the scanner calibrated: the picture's area,
// with how far a pixel spans in it, which is what a ruler needs there.
struct Region {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // Pixels, inclusive.
    double deltaX = 0, deltaY = 0;        // Millimetres a pixel.
    bool contains(double x, double y) const { return x >= x0 && x <= x1 + 1 && y >= y0 && y <= y1 + 1; }
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
    QList<Region> regions;        // Ultrasound's calibrated areas.
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
    int skipped = 0;          // Files that weren't DICOM images or reports, such as DICOMDIR.
    QList<Report> reports;    // In study order, then by date.
    QList<Mark> marks;        // From presentation states, for the images they name.
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
