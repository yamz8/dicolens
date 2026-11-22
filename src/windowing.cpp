#include "windowing.h"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

struct Preset {
    const char *name;
    double center;
    double width;
};

// The CT windows radiologists reach for, in Hounsfield units.
const Preset Presets[] = {
    {QT_TRANSLATE_NOOP("dicolens", "Brain"), 40, 80},
    {QT_TRANSLATE_NOOP("dicolens", "Soft tissue"), 40, 400},
    {QT_TRANSLATE_NOOP("dicolens", "Lung"), -600, 1500},
    {QT_TRANSLATE_NOOP("dicolens", "Bone"), 400, 1800},
};

}

const char *Windowing::presetName() const {
    for (const auto &p : Presets)
        if (p.center == m_now.center && p.width == m_now.width)
            return p.name;
    return nullptr;
}

void Windowing::forget() {
    m_series.clear();
    m_seriesFile.clear();
    m_set = false;
}

void Windowing::enter(int from, int to, bool pet) {
    if (from >= 0 && m_set)
        m_series.insert(from, m_now);
    m_undo.clear();
    m_redo.clear();
    emit historyChanged();
    m_set = m_series.contains(to);
    if (m_set) {
        m_now = m_series.value(to);
        m_file = m_seriesFile.value(to);
    }
    m_colorMap = pet ? 2 : 0;
    emit changed();
}

void Windowing::seen(int series, double center, double width) {
    if (m_set)
        return;
    m_file = {center, width, false};
    m_seriesFile.insert(series, m_file);
    m_now.center = center;
    m_now.width = width;
    m_set = true;
    emit changed();
}

void Windowing::push() {
    m_undo.append(m_now);
    if (m_undo.size() > 200)
        m_undo.removeFirst();
    m_redo.clear();
    emit historyChanged();
}

void Windowing::apply(const State &state) {
    m_now = state;
    m_now.width = std::max(1.0, state.width);
    m_set = true;
    emit changed();
}

void Windowing::beginDrag() {
    push();
    m_dragStart = m_now;
}

void Windowing::drag(double dx, double dy) {
    // Proportional, so the same drag suits a 0–255 ultrasound and a
    // 4000-unit CT alike.
    State state = m_dragStart;
    state.width = std::max(1.0, m_dragStart.width * std::exp(dx * 0.005));
    state.center = m_dragStart.center + dy * m_dragStart.width * 0.0025;
    apply(state);
}

bool Windowing::preset(int n) {
    if (n < 1 || n > int(std::size(Presets)))
        return false;
    push();
    apply({Presets[n - 1].center, Presets[n - 1].width, m_now.inverted});
    return true;
}

void Windowing::fileWindow() {
    if (!m_set)
        return;
    push();
    apply({m_file.center, m_file.width, m_now.inverted});
}

void Windowing::set(double center, double width) {
    push();
    apply({center, width, m_now.inverted});
}

void Windowing::toggleInvert() {
    push();
    apply({m_now.center, m_now.width, !m_now.inverted});
}

void Windowing::nextColorMap() {
    m_colorMap = (m_colorMap + 1) % 4;
    emit changed();
}

void Windowing::undo() {
    const State now = m_now;
    while (!m_undo.isEmpty() && m_undo.last() == now)
        m_undo.removeLast();
    if (m_undo.isEmpty()) {
        emit historyChanged();
        return;
    }
    m_redo.append(now);
    apply(m_undo.takeLast());
    emit historyChanged();
}

void Windowing::redo() {
    if (m_redo.isEmpty())
        return;
    m_undo.append(m_now);
    apply(m_redo.takeLast());
    emit historyChanged();
}

void Windowing::match(const Windowing &other) {
    if (&other == this || !other.m_set)
        return;
    if (m_set && m_now == other.m_now && m_colorMap == other.m_colorMap)
        return;
    m_now = other.m_now;
    m_colorMap = other.m_colorMap;
    m_file = other.m_file;
    m_set = true;
    emit changed();
}
