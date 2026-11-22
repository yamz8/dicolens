#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

#include "notices.h"
#include "picture.h"
#include "planes.h"
#include "study.h"

// Saves what a pane shows: a picture, the series as a video, or a copy of
// every file without what identifies the patient. Each is made and written
// on a worker, and says how it went when it's done.
class Exporter : public QObject {
    Q_OBJECT

public:
    // Everything a video's pictures are made from, copied for the worker,
    // which renders them as shown and feeds them to ffmpeg.
    struct Video {
        Planes::Recipe recipe;
        // The series' slices held when the video was asked for. Slices as
        // scanned that aren't among them are read by the worker as it goes.
        QVector<Frame> slices;
        double center = 0, width = 1;
        bool inverted = false;
        int colorMap = 0;
        std::shared_ptr<const VoxelGrid> pet;
        Overlay3D petLook;
        double aspect = 1;
        int rotation = 0;
        bool flipped = false;
        double fps = 12;
    };

    explicit Exporter(QObject *parent = nullptr) : QObject(parent) {}

    // The picture is made by the worker too, so a big one being windowed
    // and turned doesn't hold the window up.
    void savePicture(const QString &path, const std::function<QImage()> &picture);
    void saveVideo(const QString &path, const Video &video);
    void saveAnonymous(const QString &folder, const Scan &scan);
    void copy(const QImage &picture);

signals:
    // A moment's word that it worked, or what went wrong.
    void said(const Words &words);
    void failed(const Words &words);
};
