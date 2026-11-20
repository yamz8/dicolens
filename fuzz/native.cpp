// Uncompressed pixel data through convertNative, with a pixel layout out
// of the input's first 32 bytes.

#include "harness.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Input in{data, size};
    const ImageFormat format = formatFrom(in);
    touch(convertNative(in.data, qint64(in.size), format));
    return 0;
}
