#include "claim.h"

Claim::~Claim() {
    if (m_cache)
        m_cache->dropInterest(this);
}

void Claim::setCache(FrameCache *cache) {
    if (cache == m_cache)
        return;
    if (m_cache)
        m_cache->dropInterest(this);
    m_cache = cache;
    m_series = -1;
    m_refused = false;
}

qint64 Claim::reserve(int series) const {
    // A 3D view packs the slices into a volume, and a quicker one an
    // eighth of that.
    return m_views.isEmpty() || !m_cache ? 0 : m_cache->volumeBytes(series) * 9 / 8;
}

qint64 Claim::bytes(bool with3d) const {
    if (!m_cache)
        return 0;
    return m_cache->wholeBytes(m_series) + (with3d ? m_cache->volumeBytes(m_series) * 9 / 8 : 0);
}

bool Claim::show(int series, int slice, bool whole) {
    m_series = series;
    m_slice = slice;
    m_whole = whole;
    m_refused = false;
    if (!m_cache)
        return true;
    if (series < 0) {
        m_cache->dropInterest(this);
        return true;
    }
    FrameCache::Interest interest;
    interest.series = series;
    interest.slice = slice;
    interest.whole = whole || !m_views.isEmpty();
    interest.reserve = reserve(series);
    if (m_cache->setInterest(this, interest))
        return true;
    if (!m_views.isEmpty()) {
        // No room for 3D; what the pane's own picture needs may still fit.
        m_refused = true;
        interest.reserve = 0;
        interest.whole = whole;
        if (m_cache->setInterest(this, interest))
            return true;
    }
    // Refused, the cache keeps the claim as the one slice and its
    // neighbours, which is what's left to show.
    m_whole = false;
    return false;
}

bool Claim::fits(int series, int slice) const {
    if (!m_cache || series < 0)
        return false;
    FrameCache::Interest interest;
    interest.series = series;
    interest.slice = slice;
    interest.whole = true;
    // A 3D view already refused its room doesn't count against the planes.
    if (!m_refused)
        interest.reserve = reserve(series);
    return m_cache->fits(this, interest);
}

void Claim::wantVolume(QObject *view, bool wanted) {
    if (!view || wanted == m_views.contains(view))
        return;
    if (wanted) {
        m_views.insert(view);
        // A view that's gone wants nothing any more.
        connect(view, &QObject::destroyed, this, [this, view] { wantVolume(view, false); });
    } else {
        m_views.remove(view);
        disconnect(view, &QObject::destroyed, this, nullptr);
    }
    show(m_series, m_slice, m_whole);
    emit changed();
}
