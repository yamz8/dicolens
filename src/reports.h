#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include "dicom.h"

// A report that came with the scans: a PDF wrapped in DICOM, or a
// structured report, the tree of findings a radiologist's system writes.
struct Report {
    enum class Kind { Pdf, Structured };

    Kind kind = Kind::Pdf;
    QString path;
    QString title;
    QString date;          // DICOM DA: the report's own, or its study's.
    QString studyUid;
    QString studyDescription;
    QString studyDate;
    QString studyTime;
    QString patientName;
};

// One line of a structured report as people read it: a heading, or a
// finding with its value, indented by how deep in the tree it is.
struct ReportLine {
    int depth = 0;
    bool heading = false;
    QString name;
    QString value;
};

namespace Reports {

bool isPdf(const DicomFile &file);
bool isStructured(const DicomFile &file);

// The report a file holds, or nothing when it holds none.
bool describe(const DicomFile &file, Report *report);

// A structured report's content, in reading order.
QList<ReportLine> lines(const DicomFile &file);

// A structured report as plain text, headings and indented findings.
QString text(const QList<ReportLine> &lines);

// The PDF a file wraps, without DICOM's padding byte.
QByteArray pdf(const DicomFile &file);

}
