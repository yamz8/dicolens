// A whole file through the parser, and every element it found through the
// ways the app reads one: as text for the tag list, as numbers, and by
// frame.

#include "harness.h"

#include <climits>

namespace {

void walk(const DicomFile &file, const QList<DicomElement> &elements) {
    for (const DicomElement &e : elements) {
        (void)file.display(e);
        (void)file.numbers(&e);
        (void)file.decode(e.value);
        (void)tagText(e.tag);
        for (const auto &item : e.items)
            walk(file, item);
    }
}

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    QString error;
    // The bytes as libFuzzer gave them, so a read past their end is seen.
    const DicomFile file = DicomFile::parse(data, qint64(size), &error);
    if (!file.isValid())
        return 0;
    walk(file, file.elements());
    for (const DicomElement &e : file.elements()) {
        (void)file.string(e.tag);
        (void)file.first(e.tag);
        (void)file.number(e.tag);
        (void)file.integer(e.tag);
        for (int frame : {-1, 0, 1, 7, INT_MAX})
            (void)file.find(e.tag, frame);
    }
    for (int frame : {-1, 0, 1, INT_MAX}) {
        const ImageFormat format = ImageFormat::from(file, frame);
        (void)format.color();
        for (quint32 tag : {Tag::PixelSpacing, Tag::ImagePosition, Tag::RescaleSlope, Tag::WindowCenter})
            (void)file.numbers(file.find(tag, frame));
    }
    return 0;
}
