// An RLE frame through decodeRle and on through convertNative, as
// decodeFrame passes it, with a pixel layout out of the input's first 32
// bytes.

#include "harness.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Input in{data, size};
    const ImageFormat format = formatFrom(in);
    QString error;
    const QByteArray planes = decodeRle(in.rest(), format, &error);
    if (planes.isEmpty())
        return 0;
    ImageFormat planar = format;
    planar.planar = 1;
    planar.littleEndian = true;
    touch(convertNative(reinterpret_cast<const uchar *>(planes.constData()), planes.size(), planar));
    return 0;
}
