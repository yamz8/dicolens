#pragma once

#include <QtQml/qqmlregistration.h>
#include <QColor>
#include <QImage>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <atomic>
#include <memory>

#include "backend.h"
#include "volumerender.h"

// The shown series in 3D: turned, zoomed, cut into, and drawn as bone,
// vessels, skin, lungs, a surface, or a MIP, with a PET over it when one
// is. Drawn on a worker, quickly while it moves and finely once it stops.
class VolumeView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(Backend *source READ backend WRITE setBackend NOTIFY backendChanged)
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY accentChanged)
    // Only a shown view reads the volume and draws.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    // Why there's no picture yet, or can't be one.
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    // Still reading or packing the series, rather than unable to.
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString lookName READ lookName NOTIFY viewChanged)
    // A surface and a MIP are drawn through the series' window.
    Q_PROPERTY(bool followsWindow READ followsWindow NOTIFY viewChanged)
    Q_PROPERTY(double cut READ cut NOTIFY viewChanged)
    Q_PROPERTY(double zoom READ zoom NOTIFY viewChanged)
    Q_PROPERTY(QStringList edges READ edges NOTIFY viewChanged)
    Q_PROPERTY(bool spinning READ spinning NOTIFY spinningChanged)

public:
    explicit VolumeView(QQuickItem *parent = nullptr);
    ~VolumeView() override;

    Backend *backend() const { return m_backend; }
    void setBackend(Backend *backend);
    QColor accent() const { return m_accent; }
    void setAccent(const QColor &accent);
    bool active() const { return m_active; }
    void setActive(bool active);
    bool ready() const { return m_full != nullptr; }
    QString status() const;
    bool busy() const { return !status().isEmpty() && m_backend && m_backend->volume().valid && !m_unpackable; }
    QString lookName() const;
    bool followsWindow() const { return m_view.look == Look::Mip || m_view.look == Look::Surface; }
    double cut() const { return m_view.cut; }
    double zoom() const { return m_view.zoom; }
    QStringList edges() const;
    bool spinning() const { return m_spin.isActive() || m_spinPaused; }

    // Dragging turns it, by pixels across and down.
    Q_INVOKABLE void turn(double dx, double dy);
    Q_INVOKABLE void turnBy(double acrossDegrees, double downDegrees);
    Q_INVOKABLE void zoomBy(double factor);
    Q_INVOKABLE void pan(double dx, double dy);
    // Cuts away more of the front, or less, in millimetres.
    Q_INVOKABLE void cutDeeper(double mm);
    Q_INVOKABLE void setCut(double mm);
    // A drag with Ctrl moves what values the look shows.
    Q_INVOKABLE void beginShift();
    Q_INVOKABLE void shiftLook(double dx);
    Q_INVOKABLE void nextLook();
    Q_INVOKABLE void face(int side);
    // From the front, the patient's left, the back, the right, and above,
    // in turn.
    Q_INVOKABLE void nextFace();
    Q_INVOKABLE void fit();
    Q_INVOKABLE void reset();
    Q_INVOKABLE void toggleSpin();
    Q_INVOKABLE void stopSpin();
    // The patient point at a place in the item, as Backend::pointAt gives
    // it: [x, y, z, frame], or empty.
    Q_INVOKABLE QVariantList pointAt(double x, double y) const;
    Q_INVOKABLE void exportImage();
    Q_INVOKABLE void copyImage();

    void paint(QPainter *painter) override;

    // For tests: the last fine picture, and waiting for it.
    QImage picture() const { return m_fine ? m_image : QImage(); }
    bool waitForPicture(int milliseconds);
    const View3D &view() const { return m_view; }

signals:
    void backendChanged();
    void accentChanged();
    void activeChanged();
    void stateChanged();
    void viewChanged();
    void spinningChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;

private:
    // Packs the series once it's all read, if it isn't packed already.
    void prepare();
    void defaultLook();
    // Asks for a picture: quick while something moves, fine after.
    void request(bool quick);
    void startRender();
    QSize renderSize() const;

    QPointer<Backend> m_backend;
    QColor m_accent = QColor("#FFD60A");
    bool m_active = false;
    QString m_key;                 // What's packed: the series and its first file.
    bool m_packing = false;
    bool m_unpackable = false;
    std::shared_ptr<const VoxelGrid> m_full, m_half;
    View3D m_view;
    double m_shiftStart = 0;
    int m_face = 0;
    QImage m_image;
    bool m_fine = false;
    bool m_dirty = false;
    bool m_quickWanted = false;
    bool m_rendering = false;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    QTimer m_settle;
    QTimer m_spin;
    // A spin pauses while the window is in the background, rather than
    // drawing a picture nobody sees on every core 25 times a second.
    bool m_spinPaused = false;
    QMetaObject::Connection m_windowActive;
    void followWindow();
    QThreadPool m_pool;
    int m_generation = 0;
};
