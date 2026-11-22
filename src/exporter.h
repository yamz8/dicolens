#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

#include "filepicker.h"
#include "notices.h"
#include "picture.h"
#include "planes.h"
#include "study.h"

// Saves what a pane shows: a picture, the series as a video, or a copy of
// every file without what identifies the patient. It asks the file chooser
// where, taking only the answers to what it asked itself; each is then made
// and written on a worker, and says how it went when it's done.
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
        Turn turn;
        double fps = 12;
    };

    // Without a chooser there's nowhere to ask, and nothing is saved.
    explicit Exporter(FilePicker *picker, QObject *parent = nullptr);

    // Each asks where to save, under a name to start with, and saves
    // what it was given once the chooser answers. The picture is made by
    // the worker too, so a big one being windowed and turned doesn't hold
    // the window up.
    void askPicture(const QString &name, const std::function<QImage()> &picture);
    // The video is put together once there's somewhere to save it, so the
    // slices aren't held on to while the chooser is open, or after it's
    // cancelled.
    void askVideo(const QString &name, const std::function<Video()> &video);
    void askAnonymous(const Scan &scan);
    void copy(const QImage &picture);

    void savePicture(const QString &path, const std::function<QImage()> &picture);
    void saveVideo(const QString &path, const Video &video);
    void saveAnonymous(const QString &folder, const Scan &scan);

signals:
    // A moment's word that it worked, or what went wrong.
    void said(const Words &words);
    void failed(const Words &words);

private:
    FilePicker *m_picker;
    // What waits on the chooser's answer.
    std::function<QImage()> m_picture;
    std::function<Video()> m_video;
    Scan m_scan;
};
