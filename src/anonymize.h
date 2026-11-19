#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <atomic>

#include "dicom.h"
#include "study.h"

// Copies of scans without what identifies the patient, to send for a
// second opinion or to a study.
namespace Anonymize {

// The UIDs and patient IDs given out so far, so the copies still refer to
// one another as the originals did.
struct Session {
    QHash<QString, QString> uids;
    QHash<QString, QString> patients;
};

// Writes a copy of a DICOM file under PS3.15's Basic Application Level
// Confidentiality Profile, keeping dates (Retain Longitudinal Temporal
// Information) and age, sex, size, and weight (Retain Patient
// Characteristics), which reading the scans needs. Fails with a reason in
// English.
bool write(const DicomFile &file, const QString &path, Session &session, QString *error = nullptr);

// Whether the picture itself may show the patient's details, as ultrasound
// and screen captures often do; no change to the tags takes those out.
bool mayShowDetails(const DicomFile &file);

struct Result {
    QString folder;       // Where the copy went.
    int written = 0;
    int skipped = 0;      // Reports, whose text names the patient.
    int showDetails = 0;  // Images whose picture may show the patient's details.
    QStringList errors;   // A file and why it wasn't copied.
};

// Copies every image and annotation of a scan into a new folder inside
// folder, named "Anonymous scans", a series to a folder.
Result copyScan(const Scan &scan, const QString &folder, const std::atomic<bool> *cancel = nullptr);

}
