// A file's overlay planes and, read as a presentation state, its marks.

#include "harness.h"
#include "marks.h"

#include <climits>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const DicomFile file = DicomFile::parse(data, qint64(size));
    if (!file.isValid())
        return 0;
    (void)Marks::isPresentationState(file);
    for (const Mark &mark : Marks::read(file)) {
        (void)mark.appliesTo(QStringLiteral("1.2.3"), 0);
        (void)mark.box.width();
    }
    // The viewer asks for a mask the size of the picture it decoded; here
    // the size is the file's word, kept small enough to make many of.
    const int rows = std::clamp(file.integer(Tag::Rows), 0, 256), columns = std::clamp(file.integer(Tag::Columns), 0, 256);
    for (int frame : {-1, 0, 1, 2, 9, INT_MAX}) {
        const QImage mask = Marks::overlay(file, frame, rows, columns);
        if (!mask.isNull() && (mask.width() != columns || mask.height() != rows))
            abort();
    }
    return 0;
}
