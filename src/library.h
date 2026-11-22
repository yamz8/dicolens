#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <atomic>
#include <memory>

#include "framecache.h"
#include "notices.h"
#include "petgrids.h"
#include "seriesmodel.h"
#include "study.h"

// What every pane of the window shares: the scan that's open, with its
// series, reports, and marks; how far a scan being read has got; the series
// list and its thumbnails; every slice decoded so far; and the PETs packed
// to put over other series. A scan opened from any pane is open in all of
// them, and a series two panes show is read once.
class Library : public QObject {
    Q_OBJECT

public:
    explicit Library(QObject *parent = nullptr);
    ~Library() override;

    const Scan &scan() const { return m_scan; }
    // Counts the scans opened, so a pane can tell that the one it was
    // showing has been replaced.
    int serial() const { return m_serial; }
    bool scanning() const { return m_scanning; }
    int scanDone() const { return m_scanDone; }
    int scanTotal() const { return m_scanTotal; }

    FrameCache *cache() { return &m_cache; }
    PetGrids *grids() { return &m_grids; }
    SeriesModel *seriesModel() { return &m_seriesModel; }
    // Null until it's made. Asked for from the thread that loads the
    // list's pictures.
    QImage thumbnail(int series) const;

    // Reads the scans under some paths, on a worker. One file opens with
    // the rest of its folder, and a zip as the folder it holds. Whoever
    // asked is told of any problem, and is the pane that shows the slice
    // that was asked for.
    void open(const QStringList &paths, QObject *asker);

    void retranslate() { m_seriesModel.retranslate(); }

signals:
    void scanningChanged();
    void scanProgress();
    // A scan has taken the place of the one before, in every pane.
    void opened(QObject *asker, int series, int slice);
    // Nothing to say clears what was said before.
    void problem(QObject *asker, const Words &words);

private:
    void finished(int generation, const Scan &scan, const QString &focusPath, QObject *asker);
    void makeThumbnails();

    Scan m_scan;
    int m_serial = 0;
    bool m_scanning = false;
    int m_scanDone = 0;
    int m_scanTotal = 0;
    int m_generation = 0;
    std::shared_ptr<std::atomic<bool>> m_scanCancel;
    std::shared_ptr<std::atomic<bool>> m_thumbnailCancel;

    FrameCache m_cache;
    PetGrids m_grids;
    SeriesModel m_seriesModel;
    mutable QMutex m_thumbnailMutex;
    QHash<int, QImage> m_thumbnails;
    // Scans are read on a thread of their own, and thumbnails on two more,
    // so neither waits for the shown series to finish decoding.
    QThreadPool m_scanPool;
    QThreadPool m_thumbnailPool;
};
