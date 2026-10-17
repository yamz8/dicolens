#include "ljpeg.h"

#include <array>

namespace {

struct Huffman {
    // Canonical codes by length (T.81 annex C), and the values they stand for.
    std::array<int, 17> minCode{}, maxCode{}, valueIndex{};
    QVector<uchar> values;
    bool defined = false;

    void build(const uchar *counts, const uchar *symbols, int total) {
        values = QVector<uchar>(symbols, symbols + total);
        int code = 0, k = 0;
        for (int length = 1; length <= 16; ++length) {
            valueIndex[length] = k;
            minCode[length] = code;
            code += counts[length - 1];
            k += counts[length - 1];
            maxCode[length] = counts[length - 1] ? code - 1 : -1;
            code <<= 1;
        }
        defined = true;
    }
};

class Bits {
public:
    Bits(const uchar *data, int size, int pos) : m_data(data), m_size(size), m_pos(pos) {}

    int bit() {
        if (m_count == 0)
            fill();
        --m_count;
        return (m_buffer >> m_count) & 1;
    }

    int bits(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | bit();
        return v;
    }

    // Skips to the next restart marker, dropping the partial byte.
    void restart() {
        m_count = 0;
        m_marker = false;
        while (m_pos + 1 < m_size) {
            if (m_data[m_pos] == 0xFF && m_data[m_pos + 1] >= 0xD0 && m_data[m_pos + 1] <= 0xD7) {
                m_pos += 2;
                return;
            }
            ++m_pos;
        }
    }

    bool exhausted() const { return m_pos >= m_size && m_count == 0; }

private:
    void fill() {
        // A marker ends the entropy-coded data; past it, read zeros.
        uchar byte = 0;
        if (!m_marker && m_pos < m_size) {
            byte = m_data[m_pos];
            if (byte == 0xFF) {
                const uchar next = m_pos + 1 < m_size ? m_data[m_pos + 1] : 0;
                if (next == 0x00) {
                    m_pos += 2;
                } else {
                    m_marker = true;
                    byte = 0;
                }
            } else {
                ++m_pos;
            }
        }
        m_buffer = byte;
        m_count = 8;
    }

    const uchar *m_data;
    int m_size;
    int m_pos;
    int m_buffer = 0;
    int m_count = 0;
    bool m_marker = false;
};

int decodeSymbol(Bits &bits, const Huffman &table) {
    int code = 0;
    for (int length = 1; length <= 16; ++length) {
        code = (code << 1) | bits.bit();
        if (table.maxCode[length] >= 0 && code <= table.maxCode[length] && code >= table.minCode[length])
            return table.values.value(table.valueIndex[length] + code - table.minCode[length]);
    }
    return -1;
}

int u16(const uchar *p) { return p[0] << 8 | p[1]; }

}

bool decodeLosslessJpeg(const QByteArray &stream, LosslessJpeg &out, QString *error, bool *notLossless) {
    auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };
    const auto *data = reinterpret_cast<const uchar *>(stream.constData());
    const int size = stream.size();
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return fail("This JPEG image is damaged");

    Huffman tables[4];
    int restartInterval = 0;
    int componentIds[4] = {0, 0, 0, 0};
    bool frameSeen = false;
    int pos = 2;

    while (pos + 4 <= size) {
        if (data[pos] != 0xFF) {
            ++pos;
            continue;
        }
        const uchar marker = data[pos + 1];
        if (marker == 0xFF) {
            ++pos;
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        if (marker == 0xD9)
            break;
        const int length = u16(data + pos + 2);
        const uchar *segment = data + pos + 4;
        const int segmentSize = length - 2;
        if (length < 2 || pos + 2 + length > size)
            return fail("This JPEG image is damaged");

        if (marker == 0xC4) {
            int i = 0;
            while (i + 17 <= segmentSize) {
                const int id = segment[i] & 0x0F;
                int total = 0;
                for (int k = 0; k < 16; ++k)
                    total += segment[i + 1 + k];
                if (id > 3 || i + 17 + total > segmentSize)
                    return fail("This JPEG image is damaged");
                tables[id].build(segment + i + 1, segment + i + 17, total);
                i += 17 + total;
            }
        } else if (marker == 0xDD) {
            restartInterval = u16(segment);
        } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (marker != 0xC3) {
                if (notLossless)
                    *notLossless = true;
                return fail("This JPEG image isn't lossless");
            }
            out.precision = segment[0];
            out.height = u16(segment + 1);
            out.width = u16(segment + 3);
            out.components = segment[5];
            if (out.components < 1 || out.components > 4 || out.precision < 2 || out.precision > 16 ||
                out.width <= 0 || out.height <= 0)
                return fail("This JPEG image is damaged");
            for (int c = 0; c < out.components; ++c) {
                componentIds[c] = segment[6 + 3 * c];
                // Lossless images in DICOM aren't subsampled.
                if (segment[7 + 3 * c] != 0x11)
                    return fail("This JPEG image's layout isn't supported");
            }
            frameSeen = true;
        } else if (marker == 0xDA) {
            if (!frameSeen)
                return fail("This JPEG image is damaged");
            const int count = segment[0];
            if (count != out.components)
                return fail("This JPEG image's layout isn't supported");
            int tableFor[4];
            for (int s = 0; s < count; ++s) {
                const int id = segment[1 + 2 * s];
                int c = 0;
                while (c < out.components && componentIds[c] != id)
                    ++c;
                if (c != s)
                    return fail("This JPEG image's layout isn't supported");
                tableFor[s] = segment[2 + 2 * s] >> 4;
                if (tableFor[s] > 3 || !tables[tableFor[s]].defined)
                    return fail("This JPEG image is damaged");
            }
            const int predictor = segment[1 + 2 * count];
            const int pointTransform = segment[3 + 2 * count] & 0x0F;
            if (predictor < 1 || predictor > 7)
                return fail("This JPEG image is damaged");

            const int width = out.width, height = out.height, n = out.components;
            const int bits = out.precision - pointTransform;
            const int mask = (1 << out.precision) - 1;
            out.samples = QVector<quint16>(qint64(width) * height * n);
            quint16 *pixels = out.samples.data();
            Bits reader(data, size, pos + 2 + length);
            int untilRestart = restartInterval;
            bool firstLine = true;    // Also after each restart.

            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    if (restartInterval && untilRestart == 0) {
                        reader.restart();
                        untilRestart = restartInterval;
                        firstLine = true;
                    }
                    for (int c = 0; c < n; ++c) {
                        const int symbol = decodeSymbol(reader, tables[tableFor[c]]);
                        if (symbol < 0 || symbol > 16)
                            return fail("This JPEG image is damaged");
                        int diff = 0;
                        if (symbol == 16) {
                            diff = 32768;
                        } else if (symbol > 0) {
                            diff = reader.bits(symbol);
                            if (diff < (1 << (symbol - 1)))
                                diff -= (1 << symbol) - 1;
                        }
                        const qint64 i = (qint64(y) * width + x) * n + c;
                        // Neighbours are kept unshifted while predicting.
                        auto at = [&](qint64 k) { return int(pixels[k]) >> pointTransform; };
                        int prediction;
                        if (firstLine && x == 0)
                            prediction = 1 << (bits - 1);
                        else if (firstLine)
                            prediction = at(i - n);
                        else if (x == 0)
                            prediction = at(i - qint64(width) * n);
                        else {
                            const int a = at(i - n), b = at(i - qint64(width) * n), cc = at(i - qint64(width) * n - n);
                            switch (predictor) {
                            case 1: prediction = a; break;
                            case 2: prediction = b; break;
                            case 3: prediction = cc; break;
                            case 4: prediction = a + b - cc; break;
                            case 5: prediction = a + ((b - cc) >> 1); break;
                            case 6: prediction = b + ((a - cc) >> 1); break;
                            default: prediction = (a + b) >> 1; break;
                            }
                        }
                        const int value = (prediction + diff) & ((1 << bits) - 1);
                        pixels[i] = quint16((value << pointTransform) & mask);
                    }
                    if (restartInterval)
                        --untilRestart;
                }
                firstLine = false;
            }
            return true;
        }
        pos += 2 + length;
    }
    return fail("This JPEG image is damaged");
}
