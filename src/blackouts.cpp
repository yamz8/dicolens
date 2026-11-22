#include "blackouts.h"

void Blackouts::reset(const QList<Series> &series, const QList<Anonymize::Redraw> &redraws) {
    m_keys.clear();
    for (int i = 0; i < series.size(); ++i) {
        const Series &s = series[i];
        // A series without a UID still keeps its regions while it's open.
        m_keys << (s.uid.isEmpty() ? QStringLiteral("unnamed:%1:%2").arg(i).arg(s.slices.value(0).path) : s.uid);
    }
    m_redraws = redraws;
    emit changed();
}

QList<QRect> Blackouts::of(int series) const {
    return series >= 0 && series < m_keys.size() ? m_regions.value(m_keys[series]) : QList<QRect>();
}

void Blackouts::add(int series, const QRect &region) {
    if (series < 0 || series >= m_keys.size() || !region.isValid())
        return;
    m_regions[m_keys[series]] << region;
    emit changed();
}

bool Blackouts::removeLast(int series) {
    if (of(series).isEmpty())
        return false;
    QList<QRect> &regions = m_regions[m_keys[series]];
    regions.removeLast();
    if (regions.isEmpty())
        m_regions.remove(m_keys[series]);
    emit changed();
    return true;
}

bool Blackouts::clear(int series) {
    if (of(series).isEmpty())
        return false;
    m_regions.remove(m_keys[series]);
    emit changed();
    return true;
}

QHash<int, QList<QRect>> Blackouts::all() const {
    QHash<int, QList<QRect>> out;
    for (int i = 0; i < m_keys.size(); ++i)
        if (const QList<QRect> regions = m_regions.value(m_keys[i]); !regions.isEmpty())
            out.insert(i, regions);
    return out;
}
