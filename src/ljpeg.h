#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// Lossless JPEG (ITU T.81 process 14), the codec most hospital CDs carry
// CT and MR in. Decoded here rather than by ffmpeg, whose decoder drifts on
// 16-bit images. Samples come out interleaved, at full precision.
struct LosslessJpeg {
    int width = 0;
    int height = 0;
    int components = 0;
    int precision = 0;
    QVector<quint16> samples;
};

// Returns false with a reason when the stream is damaged, or when it isn't
// lossless JPEG at all (some files label baseline JPEG as lossless).
bool decodeLosslessJpeg(const QByteArray &stream, LosslessJpeg &out, QString *error, bool *notLossless = nullptr);
