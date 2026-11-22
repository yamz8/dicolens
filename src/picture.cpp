#include "picture.h"

#include <array>
#include <cmath>

QImage Picture::render() const {
    if (isNull())
        return {};
    QImage image = ::render(frame, center, width, inverted);
    if (colorMap != 0 && image.format() == QImage::Format_Grayscale8)
        image = colorize(image, colorMap);
    if (pet && !frame.isColor()) {
        Overlay3D look = petLook;
        look.grid = pet.get();
        image = fuseSlice(image, plane, *pet, look);
    }
    return image;
}

QImage colorize(const QImage &grey, int map) {
    // Made once, and only read after: a video being saved colours its
    // pictures on a worker while the window colours its own.
    static const std::array<QVector<QRgb>, 4> tables = [] {
        std::array<QVector<QRgb>, 4> made;
        for (int m = 0; m < 4; ++m) {
            made[m].resize(256);
            for (int g = 0; g < 256; ++g)
                made[m][g] = colorScale(m, g);
        }
        return made;
    }();
    QImage indexed = grey.copy();
    indexed.reinterpretAsFormat(QImage::Format_Indexed8);
    indexed.setColorTable(tables[std::clamp(map, 0, 3)]);
    return indexed.convertToFormat(QImage::Format_RGB32);
}

QTransform turned(const QSizeF &size, int rotation, bool flipped) {
    const bool sideways = rotation % 2;
    QTransform t;
    t.translate((sideways ? size.height() : size.width()) / 2, (sideways ? size.width() : size.height()) / 2);
    if (flipped)
        t.scale(-1, 1);
    t.rotate(90 * rotation);
    t.translate(-size.width() / 2, -size.height() / 2);
    return t;
}

QImage trueShape(const QImage &image, double aspect) {
    if (image.isNull() || std::abs(aspect - 1) < 0.01)
        return image;
    return image.scaled(qRound(image.width() * std::max(1.0, aspect)), qRound(image.height() * std::max(1.0, 1 / aspect)),
                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QImage asShown(const QImage &image, double aspect, int rotation, bool flipped) {
    const QImage shaped = trueShape(image, aspect);
    return shaped.isNull() ? shaped : shaped.transformed(turned(shaped.size(), rotation, flipped));
}
