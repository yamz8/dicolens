#include "exporter.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoopLocker>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUrl>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include "anonymize.h"
#include "framecache.h"

namespace {

// Tells the exporter how a save went, on its own thread, if it's still there.
void report(const QPointer<Exporter> &self, const Words &said, const Words &failed) {
    QMetaObject::invokeMethod(qApp, [self, said, failed] {
        if (!self)
            return;
        if (said)
            emit self->said(said);
        if (failed)
            emit self->failed(failed);
    }, Qt::QueuedConnection);
}

}

Exporter::Exporter(FilePicker *picker, QObject *parent) : QObject(parent), m_picker(picker) {
    if (!m_picker)
        return;
    // The panes share the chooser; each takes only the answers to what
    // it asked.
    connect(m_picker, &FilePicker::selected, this,
            [this](QObject *asker, FilePicker::Purpose purpose, const QList<QUrl> &urls) {
        if (asker != this || urls.isEmpty())
            return;
        const QString path = urls.first().toLocalFile();
        if (purpose == FilePicker::Purpose::SaveImage)
            savePicture(path, std::exchange(m_picture, nullptr));
        else if (purpose == FilePicker::Purpose::SaveVideo && m_video)
            saveVideo(path, std::exchange(m_video, nullptr)());
        else if (purpose == FilePicker::Purpose::SaveFolder)
            saveAnonymous(path, std::exchange(m_scan, Scan()));
    });
    connect(m_picker, &FilePicker::failed, this, [this](QObject *asker, const QString &message) {
        if (asker == this)
            emit failed([message] { return message; });
    });
}

void Exporter::askPicture(const QString &name, const std::function<QImage()> &picture) {
    if (!m_picker || !picture)
        return;
    m_picture = picture;
    m_picker->choose(this, FilePicker::Purpose::SaveImage, name);
}

void Exporter::askVideo(const QString &name, const std::function<Video()> &video) {
    if (!m_picker || !video)
        return;
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
        emit failed([] { return tr("Saving a video needs ffmpeg"); });
        return;
    }
    m_video = video;
    m_picker->choose(this, FilePicker::Purpose::SaveVideo, name);
}

void Exporter::askAnonymous(const Scan &scan) {
    if (!m_picker || scan.series.isEmpty())
        return;
    m_scan = scan;
    m_picker->choose(this, FilePicker::Purpose::SaveFolder);
}

void Exporter::savePicture(const QString &path, const std::function<QImage()> &picture) {
    if (path.isEmpty() || !picture)
        return;
    QPointer<Exporter> self(this);
    // Q may quit under a save still being written: it finishes first.
    auto locker = std::make_shared<QEventLoopLocker>();
    QThreadPool::globalInstance()->start([self, path, picture, locker] {
        // Written beside the target and renamed over it, so a failure never
        // leaves half a file.
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || !picture().save(&file, "PNG") || !file.commit())
            report(self, {}, [error = file.errorString()] { return tr("The image couldn't be saved: %1").arg(error); });
        else
            report(self, [name = QFileInfo(path).fileName()] { return tr("Saved %1").arg(name); }, {});
    });
}

void Exporter::copy(const QImage &picture) {
    if (picture.isNull())
        return;
    QGuiApplication::clipboard()->setImage(picture);
    emit said([] { return tr("Copied the image"); });
}

void Exporter::saveVideo(const QString &path, const Video &v) {
    if (path.isEmpty())
        return;
    emit said([] { return tr("Saving the video…"); });
    QPointer<Exporter> self(this);
    auto locker = std::make_shared<QEventLoopLocker>();
    QThreadPool::globalInstance()->start([self, v, path, locker] {
        OpenFiles files;
        auto picture = [&v, &files](int i) {
            Picture p;
            if (v.recipe.built) {
                p.frame = v.recipe.make(i, v.slices);
            } else {
                // A slice the cache had let go of is read here, and kept
                // only for as long as its picture takes.
                p.frame = v.slices.value(i);
                if (p.frame.isNull() && p.frame.error.isEmpty() && i < v.recipe.series.slices.size())
                    p.frame = decodeSlice(std::as_const(v.recipe.series.slices)[i], &files);
            }
            if (p.isNull())
                return QImage();
            p.center = v.center;
            p.width = v.width;
            p.inverted = v.inverted;
            p.colorMap = v.colorMap;
            p.pet = v.pet;
            p.petLook = v.petLook;
            p.plane = v.recipe.where(i);
            return asShown(p.render(), v.aspect, v.turn).convertToFormat(QImage::Format_RGB32);
        };
        QString error;
        const QImage first = picture(0);
        const QFileInfo target(path);
        const QString part = target.absolutePath() + "/." + target.fileName() + ".part";
        if (first.isNull()) {
            error = QStringLiteral("the first image can't be read");
        } else {
            QProcess ffmpeg;
            ffmpeg.setProcessChannelMode(QProcess::SeparateChannels);
            // Video wants even sides; the padding is black.
            ffmpeg.start("ffmpeg", {"-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgra", "-s",
                                    QStringLiteral("%1x%2").arg(first.width()).arg(first.height()), "-r",
                                    QString::number(v.fps), "-i", "-", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-c:v",
                                    "libx264", "-pix_fmt", "yuv420p", "-crf", "18", "-movflags", "+faststart", "-f",
                                    "mp4", part});
            if (!ffmpeg.waitForStarted()) {
                error = QStringLiteral("ffmpeg didn't start");
            } else {
                for (int i = 0; i < v.recipe.count; ++i) {
                    QImage image = i == 0 ? first : picture(i);
                    if (image.size() != first.size()) {
                        image = QImage(first.size(), QImage::Format_RGB32);
                        image.fill(Qt::black);
                    }
                    ffmpeg.write(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
                    ffmpeg.waitForBytesWritten(-1);
                }
                ffmpeg.closeWriteChannel();
                ffmpeg.waitForFinished(-1);
                if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0)
                    error = QString::fromUtf8(ffmpeg.readAllStandardError()).trimmed().section('\n', -1);
            }
        }
        // Written beside the target and renamed over it, so a failure never
        // leaves half a video.
        if (error.isEmpty() && std::rename(QFile::encodeName(part).constData(), QFile::encodeName(path).constData()) != 0)
            error = QString::fromLocal8Bit(std::strerror(errno));
        if (!error.isEmpty()) {
            QFile::remove(part);
            report(self, {}, [error] { return tr("The video couldn't be saved: %1").arg(error); });
        } else {
            report(self, [name = target.fileName()] { return tr("Saved %1").arg(name); }, {});
        }
    });
}

void Exporter::saveAnonymous(const QString &folder, const Scan &scan) {
    if (folder.isEmpty() || scan.series.isEmpty())
        return;
    emit said([] { return tr("Saving an anonymous copy…"); });
    QPointer<Exporter> self(this);
    auto locker = std::make_shared<QEventLoopLocker>();
    QThreadPool::globalInstance()->start([self, scan, folder, locker] {
        const Anonymize::Result result = Anonymize::copyScan(scan, folder);
        if (result.written == 0) {
            report(self, {}, [error = result.errors.value(0)] {
                return tr("The anonymous copy couldn't be saved: %1").arg(error);
            });
            return;
        }
        const int written = result.written, showDetails = result.showDetails, skipped = result.skipped,
                  errors = result.errors.size();
        const QString name = QFileInfo(result.folder).fileName();
        Words warnings;
        // What the copy can't promise is said where it stays up.
        if (showDetails > 0 || skipped > 0 || errors > 0)
            warnings = [showDetails, skipped, errors] {
                QStringList parts;
                if (showDetails > 0)
                    parts << tr("%Ln of the images may show the patient's details in the picture itself, which a copy can't take out. Look at them before sharing.", nullptr, showDetails);
                if (skipped > 0)
                    parts << tr("Reports were left out, as their text names the patient.");
                if (errors > 0)
                    parts << tr("%Ln files couldn't be copied.", nullptr, errors);
                return parts.join(' ');
            };
        report(self, [written, name] { return tr("Saved %Ln files in %1", nullptr, written).arg(name); }, warnings);
    });
}
