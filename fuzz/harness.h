#pragma once

// What the fuzz targets share. Each target is one LLVMFuzzerTestOneInput
// that hands libFuzzer's bytes to a decoder as a file would; a crash, a
// sanitizer report, a hang, or more memory than the limits bin/fuzz sets is a
// bug in src/.

#include <QByteArray>
#include <QStandardPaths>
#include <QString>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "dicom.h"
#include "pixels.h"

// Where one file ends and the next starts in an input of several.
inline constexpr char NextFile[] = "#next-file#\n";

// Fuzzing never runs ffmpeg: it would be fuzzing ffmpeg, a process a go.
// The decoder looks ffmpeg up on PATH once, so a PATH with nothing on it,
// set before the first input, leaves it without.
extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    setenv("PATH", "/nowhere", 1);
    if (!QStandardPaths::findExecutable("ffmpeg").isEmpty())
        abort();
    return 0;
}

// A file that lives in memory but has a path, since pixel data is read from
// the file on disk when a frame is decoded.
class MemoryFile {
public:
    MemoryFile() : m_fd(memfd_create("dicolens-fuzz", 0)) {
        if (m_fd < 0)
            abort();
    }
    ~MemoryFile() { close(m_fd); }
    MemoryFile(const MemoryFile &) = delete;
    MemoryFile &operator=(const MemoryFile &) = delete;

    QString path() const { return QStringLiteral("/proc/self/fd/%1").arg(m_fd); }

    void fill(const uint8_t *data, size_t size) {
        if (ftruncate(m_fd, 0) != 0)
            abort();
        for (size_t done = 0; done < size;) {
            const ssize_t n = pwrite(m_fd, data + done, size - done, off_t(done));
            if (n <= 0)
                abort();
            done += size_t(n);
        }
    }

private:
    int m_fd;
};

// Reads a fixed-size header off the front of the input, for the targets
// whose function takes more than bytes. Short inputs read as zeros.
struct Input {
    const uint8_t *data;
    size_t size;

    int32_t int32() {
        uint8_t bytes[4] = {0, 0, 0, 0};
        const size_t n = size < 4 ? size : 4;
        std::memcpy(bytes, data, n);
        data += n;
        size -= n;
        return int32_t(uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24);
    }
    // The rest, in place: one byte past its end is one byte past the input,
    // which AddressSanitizer sees.
    QByteArray rest() const { return QByteArray::fromRawData(reinterpret_cast<const char *>(data), qsizetype(size)); }
};

// A pixel layout out of 32 bytes of input: whatever a header could say.
// (decodeFrame turns away sizes that aren't positive before any decoder
// sees them; these go straight to the decoders, which must do as well.)
inline ImageFormat formatFrom(Input &in) {
    static const char *const photometrics[] = {"MONOCHROME2", "MONOCHROME1", "RGB", "YBR_FULL", "YBR_FULL_422",
                                               "YBR_PARTIAL_422", "PALETTE COLOR", "YBR_RCT"};
    ImageFormat f;
    f.rows = in.int32();
    f.columns = in.int32();
    f.samples = in.int32();
    f.bitsAllocated = in.int32();
    f.bitsStored = in.int32();
    f.highBit = in.int32();
    f.planar = in.int32();
    const uint32_t flags = uint32_t(in.int32());
    f.isSigned = flags & 1;
    f.littleEndian = flags & 2;
    f.photometric = photometrics[(flags >> 8) & 7];
    return f;
}

// Looks at every pixel a decoded frame says it has, and draws it, as the
// views do: a frame whose size and data disagree doesn't survive this.
inline void touch(const Frame &frame) {
    if (frame.isNull() || !frame.error.isEmpty())
        return;
    double low = 0, high = 0;
    frame.range(low, high);
    const QImage image = render(frame, (low + high) / 2, high - low + 1, false);
    if (image.width() != frame.width || image.height() != frame.height)
        abort();
    volatile double sum = 0;
    for (int y = 0; y < frame.height; y += 1 + frame.height / 8)
        for (int x = 0; x < frame.width; x += 1 + frame.width / 8)
            sum = sum + (frame.isColor() ? double(frame.color.pixel(x, y)) : frame.value(x, y));
}
