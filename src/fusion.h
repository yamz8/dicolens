#pragma once

#include <QObject>
#include <QPointer>
#include <memory>

#include "petgrids.h"
#include "volumerender.h"

// A PET over the CT or MR it was taken with, in one pane: on or off, which
// series it is, how strongly it's drawn, and its voxels once the library
// has them.
class Fusion : public QObject {
    Q_OBJECT

public:
    explicit Fusion(QObject *parent = nullptr) : QObject(parent) {}
    ~Fusion() override;

    // Where the PETs are kept; changing it turns the PET off.
    void setGrids(PetGrids *grids);

    bool on() const { return m_series >= 0; }
    int series() const { return m_series; }
    double opacity() const { return m_opacity; }
    // The PET's voxels, once read; null while it's off or still reading.
    std::shared_ptr<const VoxelGrid> grid() const;
    // How they're drawn over a picture.
    Overlay3D look() const;

    void start(int series);
    void stop();
    void nextOpacity();
    // Shows what another pane's does, as strongly; off when it's off.
    void follow(const Fusion &leader);

signals:
    void changed();
    // The PET can't be put over after all: it's unreadable, or too big.
    void failed(PetGrids::State why);

private:
    void check(int series);

    QPointer<PetGrids> m_grids;
    int m_series = -1;
    double m_opacity = 0.5;
};
