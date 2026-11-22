#include "fusion.h"

Fusion::~Fusion() {
    if (m_grids)
        m_grids->release(this);
}

void Fusion::setGrids(PetGrids *grids) {
    if (grids == m_grids)
        return;
    stop();
    if (m_grids)
        disconnect(m_grids, nullptr, this, nullptr);
    m_grids = grids;
    if (m_grids)
        connect(m_grids, &PetGrids::changed, this, &Fusion::check);
}

std::shared_ptr<const VoxelGrid> Fusion::grid() const {
    return on() && m_grids ? m_grids->grid(m_series) : nullptr;
}

Overlay3D Fusion::look() const {
    Overlay3D look;
    const auto pet = grid();
    look.grid = pet.get();
    if (m_grids && on())
        m_grids->range(m_series, look.low, look.high);
    look.colorMap = 1;
    look.opacity = m_opacity;
    return look;
}

void Fusion::start(int series) {
    if (!m_grids || series < 0)
        return;
    if (series == m_series)
        return;
    m_series = series;
    m_grids->hold(this, series);
    emit changed();
    check(series);
}

void Fusion::stop() {
    if (!on())
        return;
    m_series = -1;
    if (m_grids)
        m_grids->release(this);
    emit changed();
}

void Fusion::nextOpacity() {
    m_opacity = m_opacity < 0.4 ? 0.5 : m_opacity < 0.6 ? 0.7 : m_opacity < 0.8 ? 1.0 : 0.3;
    emit changed();
}

void Fusion::follow(const Fusion &leader) {
    if (!leader.on()) {
        stop();
        return;
    }
    const bool same = m_series == leader.m_series && m_opacity == leader.m_opacity;
    m_opacity = leader.m_opacity;
    if (m_series != leader.m_series)
        start(leader.m_series);
    else if (!same)
        emit changed();
}

void Fusion::check(int series) {
    if (series != m_series || !m_grids)
        return;
    const PetGrids::State state = m_grids->state(series);
    if (state == PetGrids::State::Unreadable || state == PetGrids::State::TooBig) {
        stop();
        emit failed(state);
    } else if (state == PetGrids::State::Ready) {
        emit changed();
    }
}
