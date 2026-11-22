// Encapsulated pixel data split into its frames, for a Number of Frames out
// of the input's first four bytes.

#include "harness.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Input in{data, size};
    const int frameCount = in.int32();
    const QList<QByteArray> frames = DicomFile::splitFrames(in.rest(), frameCount);
    // Frames are pieces of the data, so together they are no bigger than it.
    qint64 total = 0;
    for (const QByteArray &frame : frames)
        total += frame.size();
    if (total > qint64(in.size))
        abort();
    return 0;
}
