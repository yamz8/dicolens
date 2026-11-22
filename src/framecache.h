#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QThreadPool>
#include <QVector>
#include <atomic>
#include <map>
#include <memory>

#include "pixels.h"
#include "study.h"

// The multi-frame files of one series already parsed, shared by the threads
// reading it, so a cine loop is read from disk once rather than once a frame.
struct OpenFiles {
    QMutex mutex;
    QHash<QString, std::shared_ptr<DicomFile>> files;
    QHash<QString, std::shared_ptr<QList<QByteArray>>> frames;

    std::shared_ptr<DicomFile> open(const QString &path, std::shared_ptr<QList<QByteArray>> &split, QString *error);
};

// One slice read from its file, with the overlay its file carries. The
// cache reads its slices with this, and so does a video being saved for
// the slices the cache has let go of.
Frame decodeSlice(const Slice &slice, OpenFiles *files = nullptr);

// Every decoded slice of the open scan, kept once however many panes show
// it, within a budget of memory. Whoever shows a series says which slice it
// is looking at; the cache reads that series nearest those slices first,
// keeps what's being looked at, and lets go of the slices used longest ago
// once the budget is full. Everything here but the reading itself happens
// on the thread the cache lives on.
class FrameCache : public QObject {
    Q_OBJECT

public:
    // What one pane wants of a series.
    struct Interest {
        int series = -1;
        // The slice looked at: it and its neighbours are kept, and the rest
        // are read outward from it. -1 looks at none in particular.
        int slice = -1;
        // Every slice at once, as a plane cut across them, a slab, the 3D
        // view, and a PET to put over another series all need.
        bool whole = false;
        // Bytes held elsewhere that are made from the slices, such as the
        // 3D view's packed volume, which count against the budget too.
        qint64 reserve = 0;
    };
    // How many slices either side of the one looked at are kept with it,
    // so a step either way shows at once.
    static constexpr int Neighbours = 2;

    explicit FrameCache(QObject *parent = nullptr);
    ~FrameCache() override;

    // A third of the machine's memory, and at least 512 MB, unless
    // DICOLENS_CACHE_MB says how many megabytes.
    static qint64 defaultBudget();
    qint64 budget() const { return m_budget; }
    void setBudget(qint64 bytes);
    // Bytes of slices held, and of what's reserved beside them.
    qint64 used() const { return m_bytes + reserved(); }

    // A new scan: everything held is of the one before.
    void reset(const QList<Series> &series);

    // Replaces what someone wanted before. A whole series, or a reserve,
    // is granted only if it fits the budget beside what everyone else must
    // keep; refused, the interest stands without them and this is false.
    bool setInterest(QObject *who, const Interest &interest);
    void dropInterest(QObject *who);
    // Whether setInterest would grant it.
    bool fits(const QObject *who, const Interest &interest) const;
    // What a series takes read whole, by its size before it's read.
    qint64 wholeBytes(int series) const;

    int size(int series) const;
    // How many of a series' slices are held now.
    int count(int series) const;
    bool complete(int series) const { return size(series) > 0 && count(series) == size(series); }
    bool has(int series, int index) const;
    // A held slice, or null: one that failed to decode is held with its error.
    const Frame *frame(int series, int index) const;
    // A series' slices by index, null where one isn't held.
    const QVector<Frame> &frames(int series) const;
    // Marks a slice as just used, so it's the last to be let go.
    void touch(int series, int index);
    // Whether someone needs a slice kept.
    bool pinned(int series, int index) const;

    // For tests: slices read since the cache was made, slices let go of,
    // and whether anything is being read now.
    int decodes() const { return m_decodes; }
    int evictions() const { return m_evictions; }
    bool idle() const { return m_running == 0; }

signals:
    void arrived(int series, int index);
    // Some of a series' slices were let go of.
    void evicted(int series);

private:
    enum State : quint8 { Missing, Reading, Held };
    struct Entry {
        Series series;
        QVector<Frame> frames;
        QVector<quint8> state;
        QVector<quint64> stamp;
        QVector<qint64> bytes;
        int held = 0;
        qint64 heldBytes = 0;
        int reading = 0;
        bool video = false;
        std::shared_ptr<OpenFiles> files;
    };

    qint64 reserved() const;
    qint64 sliceBytes(int series) const;
    // What must be kept, by the sizes of the series before they're read,
    // with one interest counted as if it were another.
    qint64 pinnedBytes(const QObject *instead, const Interest *as) const;
    bool wanted(int series) const;
    // The next slice to read, if any should be.
    bool choose(int &series, int &index);
    // The slice to let go of next, if any may be.
    bool victim(int &series, int &index) const;
    void pump();
    void start(int series, int index);
    void startVideo(int series);
    void store(int generation, int series, int index, const Frame &frame);
    void finished();
    void release(int series, int index);
    void trim();

    qint64 m_budget = 0;
    qint64 m_bytes = 0;
    QVector<Entry> m_entries;
    QList<QPair<QObject *, Interest>> m_interests;
    // Held slices from the one used longest ago to the one used last.
    std::map<quint64, QPair<int, int>> m_recent;
    quint64 m_clock = 0;
    int m_generation = 0;
    int m_running = 0;
    int m_decodes = 0;
    int m_evictions = 0;
    QThreadPool m_pool;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};
