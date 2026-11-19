#pragma once

#include <QQuickImageProvider>

#include "backend.h"

// Series thumbnails, as image://thumbnails/<series>/<version>.
class ThumbnailProvider : public QQuickImageProvider {
public:
    explicit ThumbnailProvider(Backend *backend) : QQuickImageProvider(Image), m_backend(backend) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &) override {
        QImage image = m_backend->thumbnail(id.section('/', 0, 0).toInt());
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
    Backend *m_backend;
};
