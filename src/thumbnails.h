#pragma once

#include <QQuickImageProvider>

#include "library.h"

// Series thumbnails, as image://thumbnails/<series>/<which>, where the
// series list's rows say which: a new one has a new address.
class ThumbnailProvider : public QQuickImageProvider {
public:
    explicit ThumbnailProvider(Library *library) : QQuickImageProvider(Image), m_library(library) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &) override {
        QImage image = m_library->thumbnail(id.section('/', 0, 0).toInt());
        // Until it's decoded, a transparent pixel stands in.
        if (image.isNull()) {
            image = QImage(1, 1, QImage::Format_ARGB32);
            image.fill(Qt::transparent);
        }
        if (size)
            *size = image.size();
        return image;
    }

private:
    Library *m_library;
};
