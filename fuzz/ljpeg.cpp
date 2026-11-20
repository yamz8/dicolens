// A lossless JPEG stream through Dicolens' own decoder.

#include "harness.h"
#include "ljpeg.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const Input in{data, size};
    LosslessJpeg jpeg;
    QString error;
    bool notLossless = false;
    if (!decodeLosslessJpeg(in.rest(), jpeg, &error, &notLossless))
        return 0;
    // What it says it decoded is what it has.
    if (jpeg.samples.size() != qint64(jpeg.width) * jpeg.height * jpeg.components)
        abort();
    return 0;
}
