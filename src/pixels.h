#pragma once

#include <QImage>
#include <QString>
#include <QVector>

#include "dicom.h"

// How a file's pixels are laid out, for one frame of it. Enhanced
// multi-frame files can rescale each frame differently.
struct ImageFormat {
    int rows = 0;
    int columns = 0;
    int frames = 1;
    int samples = 1;
    int bitsAllocated = 16;
    int bitsStored = 16;
    int highBit = 15;
    bool isSigned = false;
    int planar = 0;
    QString photometric = "MONOCHROME2";
    double slope = 1;
    double intercept = 0;
    QString transferSyntax;
    bool littleEndian = true;

    static ImageFormat from(const DicomFile &file, int frame = 0);
    bool color() const { return samples > 1 || photometric == "PALETTE COLOR"; }
};

// One decoded picture. Grey images keep their stored values, 16 bits a
// pixel, so window and level can be changed without decoding again; colour
// images are ready to show.
struct Frame {
    int width = 0;
    int height = 0;
    QVector<quint16> mono;        // Signed values are kept as their 16-bit pattern.
    bool isSigned = false;
    bool inverted = false;        // MONOCHROME1: low values are white.
    double slope = 1;
    double intercept = 0;
    QImage color;
    QString error;                // English; translated where it's shown.
    QString detail;               // Untranslated specifics, such as what ffmpeg said.
    double windowCenter = 0;      // The window the file suggests, or the full range.
    double windowWidth = 0;

    bool isNull() const { return width == 0 || height == 0; }
    bool isColor() const { return !color.isNull(); }
    int stored(int x, int y) const;
    double value(int x, int y) const;   // The modality value: Hounsfield units on CT.
    void range(double &low, double &high) const;
};

// The default window and level for a frame: the file's own, or its full range.
void defaultWindow(const DicomFile &file, const Frame &frame, int frameIndex, double &center, double &width);

// Shows a frame through a window, as 8-bit grey (or the colour image).
QImage render(const Frame &frame, double center, double width, bool invert);

// Encapsulated multi-frame files can pass their frames already split, so
// a cine loop isn't read from disk once a frame.
Frame decodeFrame(const DicomFile &file, int frameIndex, const QList<QByteArray> *frames = nullptr);
bool isVideo(const QString &transferSyntax);
QList<QByteArray> readFrames(const DicomFile &file);
QList<Frame> decodeVideo(const DicomFile &file);
bool canDecode(const QString &transferSyntax);

// For tests and the decoder alike.
QByteArray decodeRle(const QByteArray &data, const ImageFormat &format, QString *error);
Frame convertNative(const uchar *data, qint64 size, const ImageFormat &format);
