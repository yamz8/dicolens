#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>

#include "backend.h"

// Draws the shown slice, fitted to the view and then zoomed and panned,
// with pixels at their true shape, and the ruler over it.
class SliceView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(Backend *source READ backend WRITE setBackend NOTIFY backendChanged)
    Q_PROPERTY(double zoom READ zoom NOTIFY viewChanged)
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY accentChanged)
    Q_PROPERTY(bool rulerVisible READ rulerVisible NOTIFY rulerChanged)
    Q_PROPERTY(QString rulerText READ rulerText NOTIFY rulerChanged)

public:
    explicit SliceView(QQuickItem *parent = nullptr);

    Backend *backend() const { return m_backend; }
    void setBackend(Backend *backend);
    double zoom() const { return m_zoom; }
    QColor accent() const { return m_accent; }
    void setAccent(const QColor &accent);
    bool rulerVisible() const { return m_rulerVisible; }
    QString rulerText() const;

    // Between the item's coordinates and the image's pixels.
    Q_INVOKABLE QPointF toImage(double x, double y) const;
    Q_INVOKABLE QPointF toItem(double x, double y) const;
    Q_INVOKABLE void zoomAt(double x, double y, double factor);
    Q_INVOKABLE void pan(double dx, double dy);
    Q_INVOKABLE void fit();
    Q_INVOKABLE void startRuler(double x, double y);
    Q_INVOKABLE void moveRuler(double x, double y);
    Q_INVOKABLE void clearRuler();

    void paint(QPainter *painter) override;

    // The image's rectangle in the item, for tests and the painter.
    QRectF imageRect() const;

signals:
    void backendChanged();
    void viewChanged();
    void accentChanged();
    void rulerChanged();

private:
    QPointer<Backend> m_backend;
    double m_zoom = 1;
    QPointF m_pan;
    QColor m_accent = QColor("#FFD60A");
    bool m_rulerVisible = false;
    QPointF m_rulerA, m_rulerB;     // Image pixels.
};
