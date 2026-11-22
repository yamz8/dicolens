#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <memory>

#include "framecache.h"
#include "volumerender.h"

// The PETs packed into voxels for putting over the series they were taken
// with. Each is read and packed once, on a worker, however many panes put
// it over their pictures, and is kept for as long as one of them does.
class PetGrids : public QObject {
    Q_OBJECT

public:
    enum class State { None, Reading, Ready, Unreadable, TooBig };

    explicit PetGrids(FrameCache *cache, QObject *parent = nullptr);

    // A new scan: every grid is of the one before.
    void reset(const QList<Series> &series);

    // Keeps a series' grid for someone until they let go or hold another.
    void hold(QObject *who, int series);
    void release(QObject *who);

    State state(int series) const { return m_pets.value(series).state; }
    // Null until it's read.
    std::shared_ptr<const VoxelGrid> grid(int series) const { return m_pets.value(series).grid; }
    // The values from clear to full colour.
    void range(int series, double &low, double &high) const;

signals:
    void changed(int series);

private:
    struct Pet {
        State state = State::None;
        std::shared_ptr<const VoxelGrid> grid;
        double low = 0, high = 1;
        bool packing = false;
        QSet<QObject *> users;
        // Who the cache knows this PET's slices are wanted by.
        QObject *reader = nullptr;
    };

    void pack(int series);
    void drop(int series);

    FrameCache *m_cache;
    QList<Series> m_series;
    QHash<int, Pet> m_pets;
    int m_generation = 0;
};
