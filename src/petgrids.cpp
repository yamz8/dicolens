#include "petgrids.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThreadPool>

#include "reformat.h"

PetGrids::PetGrids(FrameCache *cache, QObject *parent) : QObject(parent), m_cache(cache) {
    connect(m_cache, &FrameCache::arrived, this, [this](int series, int) {
        if (m_pets.contains(series))
            pack(series);
    });
}

void PetGrids::reset(const QList<Series> &series) {
    ++m_generation;
    const QList<int> held = m_pets.keys();
    for (int s : held)
        drop(s);
    m_series = series;
}

void PetGrids::range(int series, double &low, double &high) const {
    const Pet pet = m_pets.value(series);
    low = pet.low;
    high = pet.high;
}

void PetGrids::hold(QObject *who, int series) {
    if (series < 0 || series >= m_series.size()) {
        release(who);
        return;
    }
    if (m_pets.value(series).users.contains(who))
        return;
    release(who);
    Pet &pet = m_pets[series];
    pet.users.insert(who);
    if (pet.state != State::None)
        return;
    // Its slices are all needed until they're packed, and the grid made
    // of them takes as much again, or more when they lie tilted.
    pet.reader = new QObject(this);
    FrameCache::Interest interest;
    interest.series = series;
    interest.slice = m_series[series].slices.size() / 2;
    interest.whole = true;
    interest.reserve = m_cache->volumeBytes(series);
    pet.state = m_cache->setInterest(pet.reader, interest) ? State::Reading : State::TooBig;
    if (pet.state == State::Reading)
        pack(series);
}

void PetGrids::release(QObject *who) {
    for (auto it = m_pets.begin(); it != m_pets.end(); ++it)
        if (it->users.remove(who) && it->users.isEmpty()) {
            drop(it.key());
            return;
        }
}

void PetGrids::drop(int series) {
    const Pet pet = m_pets.take(series);
    if (pet.reader) {
        m_cache->dropInterest(pet.reader);
        delete pet.reader;
    }
}

void PetGrids::pack(int series) {
    Pet &pet = m_pets[series];
    if (pet.state != State::Reading || pet.packing || !m_cache->complete(series))
        return;
    pet.packing = true;
    const Series which = m_series[series];
    const QVector<Frame> frames = m_cache->frames(series);
    QPointer<PetGrids> self(this);
    QThreadPool::globalInstance()->start([self, which, frames, series, generation = m_generation] {
        auto grid = std::make_shared<VoxelGrid>(packVolume(makeVolume(which), frames, which.suv > 0 ? which.suv : 1));
        double low = 0, high = 1;
        if (grid->valid)
            petRange(*grid, which.suv > 0, low, high);
        QMetaObject::invokeMethod(qApp, [self, grid, low, high, series, generation] {
            // Let go of while it was packed, or of a scan since closed.
            if (!self || generation != self->m_generation || !self->m_pets.contains(series))
                return;
            Pet &pet = self->m_pets[series];
            pet.packing = false;
            FrameCache::Interest interest;
            if (grid->valid) {
                pet.state = State::Ready;
                pet.grid = grid;
                pet.low = low;
                pet.high = high;
                // The slices have done their work; the grid stays.
                interest.reserve = qint64(grid->data.size() + grid->blockLow.size() + grid->blockHigh.size()) * 2;
            } else {
                pet.state = State::Unreadable;
            }
            self->m_cache->setInterest(pet.reader, interest);
            emit self->changed(series);
        }, Qt::QueuedConnection);
    });
}
