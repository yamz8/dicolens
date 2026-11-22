#include "framecache.h"

#include <QCoreApplication>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <climits>
#include <unistd.h>

#include "marks.h"

std::shared_ptr<DicomFile> OpenFiles::open(const QString &path, std::shared_ptr<QList<QByteArray>> &split, QString *error) {
    {
        QMutexLocker lock(&mutex);
        if (files.contains(path)) {
            split = frames.value(path);
            return files.value(path);
        }
    }
    auto file = std::make_shared<DicomFile>(DicomFile::read(path, error));
    // Only a file of many frames is worth keeping: the rest are read once.
    if (file->isValid() && file->integer(Tag::NumberOfFrames, 1) > 1) {
        if (file->encapsulated())
            split = std::make_shared<QList<QByteArray>>(readFrames(*file));
        QMutexLocker lock(&mutex);
        files.insert(path, file);
        frames.insert(path, split);
    }
    return file;
}

Frame decodeSlice(const Slice &slice, OpenFiles *files) {
    OpenFiles own;
    QString error;
    std::shared_ptr<QList<QByteArray>> split;
    const auto file = (files ? files : &own)->open(slice.path, split, &error);
    Frame frame;
    if (!file->isValid()) {
        frame.error = QT_TRANSLATE_NOOP("dicolens", "This file can't be read");
        frame.detail = error;
        return frame;
    }
    frame = decodeFrame(*file, slice.frame, split.get());
    if (frame.error.isEmpty() && !frame.isNull())
        frame.overlay = Marks::overlay(*file, slice.frame, frame.height, frame.width);
    return frame;
}

namespace {

qint64 bytesOf(const Frame &frame) {
    return qint64(frame.mono.size()) * 2 + frame.color.sizeInBytes() + frame.overlay.sizeInBytes();
}

}

FrameCache::FrameCache(QObject *parent)
    : QObject(parent), m_budget(defaultBudget()), m_cancel(std::make_shared<std::atomic<bool>>(false)) {
    m_pool.setMaxThreadCount(std::max(2, QThread::idealThreadCount()));
}

FrameCache::~FrameCache() {
    *m_cancel = true;
    m_pool.clear();
    m_pool.waitForDone();
}

qint64 FrameCache::defaultBudget() {
    bool ok = false;
    const qint64 megabytes = qEnvironmentVariable("DICOLENS_CACHE_MB").toLongLong(&ok);
    if (ok && megabytes > 0)
        return megabytes << 20;
    // A third leaves the rest for the pictures made from the slices, the
    // desktop, and whatever else is open; the floor keeps a small machine
    // from reading the same few slices over and over.
    const qint64 memory = qint64(sysconf(_SC_PHYS_PAGES)) * qint64(sysconf(_SC_PAGE_SIZE));
    return std::max<qint64>(512ll << 20, memory / 3);
}

void FrameCache::setBudget(qint64 bytes) {
    m_budget = std::max<qint64>(0, bytes);
    trim();
    pump();
}

void FrameCache::reset(const QList<Series> &series) {
    // Slices still being read are of the old scan: they stop where they
    // can, and what they hand back is dropped.
    ++m_generation;
    *m_cancel = true;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_entries.clear();
    m_entries.resize(series.size());
    for (int s = 0; s < series.size(); ++s) {
        Entry &e = m_entries[s];
        const int n = series[s].slices.size();
        e.series = series[s];
        e.frames.resize(n);
        e.state.fill(Missing, n);
        e.stamp.fill(0, n);
        e.bytes.fill(0, n);
        e.video = isVideo(series[s].transferSyntax);
    }
    m_recent.clear();
    m_bytes = 0;
    // What was wanted was of the old scan's series; the panes say again.
    m_interests.clear();
}

qint64 FrameCache::reserved() const {
    qint64 total = 0;
    for (const auto &i : m_interests)
        total += i.second.reserve;
    return total;
}

int FrameCache::size(int series) const {
    return series >= 0 && series < m_entries.size() ? m_entries[series].frames.size() : 0;
}

int FrameCache::count(int series) const {
    return series >= 0 && series < m_entries.size() ? m_entries[series].held : 0;
}

bool FrameCache::has(int series, int index) const {
    return index >= 0 && index < size(series) && m_entries[series].state[index] == Held;
}

const Frame *FrameCache::frame(int series, int index) const {
    return has(series, index) ? &m_entries[series].frames[index] : nullptr;
}

const QVector<Frame> &FrameCache::frames(int series) const {
    static const QVector<Frame> none;
    return series >= 0 && series < m_entries.size() ? m_entries[series].frames : none;
}

void FrameCache::touch(int series, int index) {
    if (!has(series, index))
        return;
    Entry &e = m_entries[series];
    m_recent.erase(e.stamp[index]);
    e.stamp[index] = ++m_clock;
    m_recent[e.stamp[index]] = {series, index};
}

qint64 FrameCache::sliceBytes(int series) const {
    if (series < 0 || series >= m_entries.size())
        return 0;
    const Entry &e = m_entries[series];
    // Grey slices keep 16 bits a pixel; colour ones, and a video's, 32.
    return qint64(e.series.rows) * e.series.columns * (e.series.color || e.video ? 4 : 2);
}

qint64 FrameCache::wholeBytes(int series) const {
    return sliceBytes(series) * size(series);
}

bool FrameCache::wanted(int series) const {
    for (const auto &i : m_interests)
        if (i.second.series == series)
            return true;
    return false;
}

bool FrameCache::pinned(int series, int index) const {
    if (series < 0 || series >= m_entries.size())
        return false;
    for (const auto &entry : m_interests) {
        const Interest &i = entry.second;
        if (i.series != series)
            continue;
        // A video is decoded in one go, so none of it is let go of while
        // it's shown: reading one frame again would mean reading them all.
        if (i.whole || m_entries[series].video || (i.slice >= 0 && std::abs(index - i.slice) <= Neighbours))
            return true;
    }
    return false;
}

qint64 FrameCache::pinnedBytes(const QObject *instead, const Interest *as) const {
    QSet<int> whole;
    QHash<int, QSet<int>> near;
    qint64 total = 0;
    auto add = [&](const Interest &i) {
        total += i.reserve;
        if (i.series < 0 || i.series >= m_entries.size())
            return;
        if (i.whole || m_entries[i.series].video) {
            whole.insert(i.series);
        } else if (i.slice >= 0) {
            for (int at = i.slice - Neighbours; at <= i.slice + Neighbours; ++at)
                if (at >= 0 && at < size(i.series))
                    near[i.series].insert(at);
        }
    };
    for (const auto &i : m_interests)
        if (i.first != instead)
            add(i.second);
    if (as)
        add(*as);
    // A series counts once, however many panes show it.
    for (int series : std::as_const(whole))
        total += wholeBytes(series);
    for (auto it = near.constBegin(); it != near.constEnd(); ++it)
        if (!whole.contains(it.key()))
            total += sliceBytes(it.key()) * it.value().size();
    return total;
}

bool FrameCache::fits(const QObject *who, const Interest &interest) const {
    // The slice looked at and its neighbours are always kept: without them
    // there is nothing to show.
    if (!interest.whole && interest.reserve == 0)
        return true;
    return pinnedBytes(who, &interest) <= m_budget;
}

bool FrameCache::setInterest(QObject *who, const Interest &asked) {
    Interest interest = asked;
    if (interest.series >= m_entries.size())
        interest.series = -1;
    if (interest.series < 0) {
        interest.slice = -1;
        interest.whole = false;
    }
    const bool granted = fits(who, interest);
    if (!granted) {
        interest.whole = false;
        interest.reserve = 0;
    }
    if (interest.series < 0 && interest.reserve == 0) {
        dropInterest(who);
        return granted;
    }
    bool known = false;
    for (auto &i : m_interests)
        if (i.first == who) {
            i.second = interest;
            known = true;
        }
    if (!known) {
        m_interests.append({who, interest});
        // Whoever is gone wants nothing any more.
        connect(who, &QObject::destroyed, this, [this, who] { dropInterest(who); });
    }
    for (int s = 0; s < m_entries.size(); ++s)
        if (m_entries[s].files && !wanted(s))
            m_entries[s].files.reset();
    trim();
    pump();
    return granted;
}

void FrameCache::dropInterest(QObject *who) {
    for (int i = 0; i < m_interests.size(); ++i)
        if (m_interests[i].first == who) {
            m_interests.removeAt(i);
            disconnect(who, &QObject::destroyed, this, nullptr);
            break;
        }
    for (int s = 0; s < m_entries.size(); ++s)
        if (m_entries[s].files && !wanted(s))
            m_entries[s].files.reset();
    pump();
}

bool FrameCache::victim(int &series, int &index) const {
    // Slices of a series nobody is looking at go before any of one that
    // someone is; among either, the one used longest ago.
    bool idleHeld = false;
    for (int s = 0; s < m_entries.size() && !idleHeld; ++s)
        idleHeld = m_entries[s].held > 0 && !wanted(s);
    bool found = false;
    for (const auto &entry : m_recent) {
        const QPair<int, int> &where = entry.second;
        if (pinned(where.first, where.second))
            continue;
        const bool idle = !wanted(where.first);
        if (!found || idle) {
            series = where.first;
            index = where.second;
            found = true;
        }
        if (idle || !idleHeld)
            break;
    }
    return found;
}

void FrameCache::release(int series, int index) {
    Entry &e = m_entries[series];
    if (e.state[index] != Held)
        return;
    m_recent.erase(e.stamp[index]);
    m_bytes -= e.bytes[index];
    e.heldBytes -= e.bytes[index];
    e.frames[index] = Frame();
    e.state[index] = Missing;
    e.bytes[index] = 0;
    --e.held;
}

void FrameCache::trim() {
    QSet<int> lost;
    int series = 0, index = 0;
    while (used() > m_budget && victim(series, index)) {
        release(series, index);
        ++m_evictions;
        lost.insert(series);
    }
    for (int s : std::as_const(lost))
        emit evicted(s);
}

bool FrameCache::choose(int &series, int &index) {
    // First what someone needs: the slice a pane is looking at and its
    // neighbours, or all of a series wanted whole, nearest first.
    auto nearest = [this](const Interest &i, int from, int reach, int &at) {
        const Entry &e = m_entries[i.series];
        const int n = e.frames.size(), center = std::clamp(i.slice, 0, std::max(0, n - 1));
        for (int d = from; d <= reach && d < n; ++d)
            for (int candidate : {center + d, center - d})
                if (candidate >= 0 && candidate < n && e.state[candidate] == Missing) {
                    at = candidate;
                    return d;
                }
        return INT_MAX;
    };
    int best = INT_MAX;
    for (const auto &entry : std::as_const(m_interests)) {
        const Interest &i = entry.second;
        if (i.series < 0 || (i.slice < 0 && !i.whole))
            continue;
        const Entry &e = m_entries[i.series];
        if (e.held + e.reading == e.frames.size())
            continue;
        int at = 0;
        // A video comes whole or not at all, in one read.
        const int d = e.video ? (e.reading == 0 ? 0 : INT_MAX)
                              : nearest(i, 0, i.whole ? e.frames.size() : Neighbours, at);
        if (d < best) {
            best = d;
            series = i.series;
            index = at;
        }
    }
    if (best != INT_MAX)
        return true;

    // Then the rest of what's being looked at, so scrolling on finds it
    // read already: while there is room, or in place of a series nobody
    // is looking at any more. Never in place of the same series' slices,
    // which would read a series bigger than the budget round and round.
    for (const auto &entry : std::as_const(m_interests)) {
        const Interest &i = entry.second;
        if (i.series < 0 || i.slice < 0)
            continue;
        const Entry &e = m_entries[i.series];
        if (e.video || e.held + e.reading == e.frames.size())
            continue;
        int at = 0;
        const int d = nearest(i, Neighbours + 1, e.frames.size(), at);
        if (d < best) {
            best = d;
            series = i.series;
            index = at;
        }
    }
    if (best == INT_MAX)
        return false;
    // Slices being read take their room when they arrive, and those of a
    // series nobody is looking at give theirs up.
    qint64 coming = 0, idle = 0;
    for (int s = 0; s < m_entries.size(); ++s) {
        coming += sliceBytes(s) * m_entries[s].reading;
        if (!wanted(s))
            idle += m_entries[s].heldBytes;
    }
    return used() + coming + sliceBytes(series) - idle <= m_budget;
}

void FrameCache::pump() {
    int series = 0, index = 0;
    while (m_running < m_pool.maxThreadCount() && choose(series, index)) {
        if (m_entries[series].video)
            startVideo(series);
        else
            start(series, index);
    }
}

void FrameCache::start(int series, int index) {
    Entry &e = m_entries[series];
    e.state[index] = Reading;
    ++e.reading;
    ++m_running;
    ++m_decodes;
    if (!e.files)
        e.files = std::make_shared<OpenFiles>();
    const Slice slice = std::as_const(e.series.slices)[index];
    m_pool.start([this, slice, files = e.files, cancel = m_cancel, generation = m_generation, series, index] {
        const Frame frame = *cancel ? Frame() : decodeSlice(slice, files.get());
        QMetaObject::invokeMethod(this, [this, generation, series, index, frame] {
            store(generation, series, index, frame);
            finished();
            if (generation == m_generation && has(series, index))
                emit arrived(series, index);
        }, Qt::QueuedConnection);
    });
}

void FrameCache::startVideo(int series) {
    Entry &e = m_entries[series];
    const int n = e.frames.size();
    // Frames left from when it was last shown are read again with the rest.
    for (int i = 0; i < n; ++i)
        release(series, i);
    e.state.fill(Reading);
    e.reading = n;
    ++m_running;
    m_decodes += n;
    // A video is one stream, which ffmpeg decodes whole, and all of it is
    // kept while it's shown. One longer than the budget has room for is
    // decoded only as far as fits, and says so on the frames past that,
    // rather than taking memory until the system stops the app.
    const qint64 each = std::max<qint64>(1, sliceBytes(series));
    const qint64 room = m_budget - (pinnedBytes(nullptr, nullptr) - wholeBytes(series));
    const int keep = int(std::clamp<qint64>(room / each, 1, n));
    const QList<Slice> slices = e.series.slices;
    m_pool.start([this, slices, keep, cancel = m_cancel, generation = m_generation, series] {
        QList<Frame> frames;
        if (!*cancel) {
            QString error;
            const DicomFile file = DicomFile::read(slices.first().path, &error);
            const QList<Frame> decoded = decodeVideo(file, keep < slices.size() ? keep : 0);
            for (const Slice &slice : slices) {
                Frame frame;
                if (keep < slices.size() && slice.frame >= keep)
                    frame.error = QT_TRANSLATE_NOOP("dicolens", "This video is too long to keep in memory, so only its start is shown");
                else
                    frame = decoded.value(std::min<int>(slice.frame, decoded.size() - 1));
                frames << frame;
            }
        }
        QMetaObject::invokeMethod(this, [this, generation, series, frames] {
            for (int i = 0; i < frames.size(); ++i)
                store(generation, series, i, frames[i]);
            finished();
            for (int i = 0; i < frames.size() && generation == m_generation; ++i)
                if (has(series, i))
                    emit arrived(series, i);
        }, Qt::QueuedConnection);
    });
}

void FrameCache::store(int generation, int series, int index, const Frame &frame) {
    if (generation != m_generation || series >= m_entries.size())
        return;
    Entry &e = m_entries[series];
    if (index < 0 || index >= e.frames.size())
        return;
    if (e.state[index] == Reading)
        --e.reading;
    release(series, index);
    e.frames[index] = frame;
    e.state[index] = Held;
    e.bytes[index] = bytesOf(frame);
    e.stamp[index] = ++m_clock;
    m_recent[e.stamp[index]] = {series, index};
    m_bytes += e.bytes[index];
    e.heldBytes += e.bytes[index];
    ++e.held;
}

void FrameCache::finished() {
    --m_running;
    trim();
    pump();
}
