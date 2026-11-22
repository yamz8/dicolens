// A whole file read from its path, and its frames decoded and drawn with
// their overlay planes, the way the viewer opens one.

#include "harness.h"
#include "marks.h"

#include <QSet>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static MemoryFile memory;
    memory.fill(data, size);
    const DicomFile file = DicomFile::read(memory.path());
    if (!file.isValid())
        return 0;

    const ImageFormat format = ImageFormat::from(file);
    // A cine loop's frames are split once, as the viewer does it.
    const QList<QByteArray> split = readFrames(file);
    // Every frame of a file of a few; of thousands, the first, the last,
    // and some between, which is as much as fits in one input's time.
    QSet<int> indices{-1, 0, format.frames - 1, format.frames};
    for (int i = 0; i < 32; ++i)
        indices << int(qint64(format.frames) * i / 32);
    for (int index : std::as_const(indices)) {
        const Frame frame = decodeFrame(file, index, file.encapsulated() ? &split : nullptr);
        touch(frame);
        if (!frame.isNull() && frame.error.isEmpty())
            (void)Marks::overlay(file, index, frame.height, frame.width);
    }
    // Once without the frames split ahead, which reads them itself.
    touch(decodeFrame(file, 0));
    if (isVideo(file.transferSyntax()))
        (void)decodeVideo(file, 1);
    return 0;
}
