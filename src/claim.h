#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>

#include "framecache.h"

// What one pane asks the cache to keep for it: the slice behind its
// picture and that slice's neighbours; every slice of the series when its
// picture is made from them all, or a 3D view of it is open; and room for
// the volume a 3D view packs. The cache grants the last two only when they
// fit its budget, and this remembers what it was refused.
class Claim : public QObject {
    Q_OBJECT

public:
    explicit Claim(QObject *parent = nullptr) : QObject(parent) {}
    ~Claim() override;

    // Which cache to ask; what was claimed from another is let go of.
    void setCache(FrameCache *cache);

    // What the pane shows now: a series, the slice of it behind the
    // picture, and whether the picture needs every slice. False when it
    // does and there's no room, which leaves the claim at the one slice.
    bool show(int series, int slice, bool whole);
    // Whether there would be room for every slice of a series at once.
    bool fits(int series, int slice) const;

    // A 3D view of the pane's series, open or closed.
    void wantVolume(QObject *view, bool wanted);
    // There was no room for the 3D views' volume.
    bool volumeRefused() const { return m_refused; }
    // What the series takes whole, and with a 3D view's volume beside it.
    qint64 bytes(bool with3d) const;

signals:
    // A 3D view came or went, and what's kept changed with it.
    void changed();

private:
    qint64 reserve(int series) const;

    QPointer<FrameCache> m_cache;
    QSet<QObject *> m_views;
    int m_series = -1;
    int m_slice = -1;
    bool m_whole = false;
    bool m_refused = false;
};
