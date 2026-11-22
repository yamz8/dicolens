// A file read as a report: what it is, a structured report's lines and
// their text, and the PDF it wraps.

#include "harness.h"
#include "reports.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const DicomFile file = DicomFile::parse(data, qint64(size));
    if (!file.isValid())
        return 0;
    Report report;
    (void)Reports::describe(file, &report);
    const QList<ReportLine> lines = Reports::lines(file);
    (void)Reports::text(lines);
    const QByteArray pdf = Reports::pdf(file);
    if (pdf.size() > qint64(size))
        abort();
    return 0;
}
