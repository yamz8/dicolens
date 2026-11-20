#pragma once

#include <QtQml/qqmlregistration.h>
#include <QColor>
#include <QHash>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QTransform>

#include "backend.h"
#include "measurestore.h"

// Draws the shown slice, fitted to the view and then zoomed and panned,
// with pixels at their true shape, turned and flipped as asked, the marks
// left on it, and the ruler over it.
class SliceView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(Backend *source READ backend WRITE setBackend NOTIFY backendChanged)
    Q_PROPERTY(double zoom READ zoom NOTIFY viewChanged)
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY accentChanged)
    // The desktop's text size against its default, for what's written on
    // the picture.
    Q_PROPERTY(double textScale READ textScale WRITE setTextScale NOTIFY accentChanged)
    // 0 looks, 1 draws rulers, 2 areas, 3 angles, 4 Cobb angles.
    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged)
    // How many measurements the shown slice has, and whether one is
    // being drawn.
    Q_PROPERTY(int measureCount READ measureCount NOTIFY measuresChanged)
    Q_PROPERTY(bool drawing READ drawing NOTIFY measuresChanged)
    // Another view's backend: where its picture cuts this one is drawn as
    // a line, when the two lie in one frame of reference.
    Q_PROPERTY(Backend *reference READ reference WRITE setReference NOTIFY referenceChanged)
    // Of four views, the third plane's line too.
    Q_PROPERTY(Backend *secondReference READ secondReference WRITE setSecondReference NOTIFY referenceChanged)

public:
    explicit SliceView(QQuickItem *parent = nullptr);

    Backend *backend() const { return m_backend; }
    void setBackend(Backend *backend);
    double zoom() const { return m_zoom; }
    QColor accent() const { return m_accent; }
    double textScale() const { return m_textScale; }
    void setTextScale(double scale);
    void setAccent(const QColor &accent);
    Backend *reference() const { return m_reference; }
    void setReference(Backend *reference);
    Backend *secondReference() const { return m_secondReference; }
    void setSecondReference(Backend *reference);
    // The reference line's ends in image pixels, if there is one.
    bool referenceLine(QPointF &a, QPointF &b) const { return lineTo(m_reference, a, b); }
    // Where another view's plane cuts this picture, in image pixels.
    bool lineTo(Backend *other, QPointF &a, QPointF &b) const;
    int tool() const { return m_tool; }
    void setTool(int tool);
    int measureCount() const { return measures().size(); }
    bool drawing() const { return m_drawing; }
    // What each of the shown slice's measurements says, for tests.
    Q_INVOKABLE QStringList measureTexts() const;

    // Between the item's coordinates and the image's pixels.
    Q_INVOKABLE QPointF toImage(double x, double y) const;
    Q_INVOKABLE QPointF toItem(double x, double y) const;
    Q_INVOKABLE void zoomAt(double x, double y, double factor);
    Q_INVOKABLE void pan(double dx, double dy);
    Q_INVOKABLE void fit();
    // A press, drag, and release draw a ruler or an area; an angle takes
    // its second arm from a second press.
    Q_INVOKABLE void press(double x, double y);
    Q_INVOKABLE void drag(double x, double y);
    Q_INVOKABLE void release(double x, double y);
    Q_INVOKABLE void hover(double x, double y);
    // Whether a point is on the reference line, and dragging it, which
    // turns the other view's plane about where the planes meet.
    Q_INVOKABLE bool nearReference(double x, double y) const;
    Q_INVOKABLE void beginTurn(double x, double y);
    Q_INVOKABLE void turnTo(double x, double y);
    // Drops the one being drawn, or else the slice's last one.
    Q_INVOKABLE bool removeLast();
    Q_INVOKABLE void clearMeasures();

    void paint(QPainter *painter) override;

    // The image's rectangle in the item, for tests and the painter.
    QRectF imageRect() const;
    // From the image's pixels to the item.
    QTransform transform() const;

signals:
    void backendChanged();
    void viewChanged();
    void accentChanged();
    void referenceChanged();
    void toolChanged();
    void measuresChanged();

private:
    void paintMarks(QPainter *painter, const QTransform &t, const QSizeF &size);
    void paintLabel(QPainter *painter, QPointF at, const QString &text);

    QString text(const Measure &m) const;
    void paintMeasure(QPainter *painter, const QTransform &t, const Measure &m, bool active);
    // Measurements stay with the slice they were drawn on, and are kept
    // for when the scans are opened again.
    QString seriesUid() const;
    QList<Measure> measures() const;
    void keep(const Measure &m);

    QPointer<Backend> m_backend;
    QPointer<Backend> m_reference;
    QPointer<Backend> m_secondReference;
    QPointer<Backend> m_turnTarget;
    // The view whose line is under a point, if one that can turn is.
    Backend *referenceNear(double x, double y) const;
    double m_zoom = 1;
    QPointF m_pan;
    QColor m_accent = QColor("#FFD60A");
    double m_textScale = 1;
    int m_tool = 0;
    Measure m_current;
    bool m_drawing = false;
    bool m_secondArm = false;     // An angle waiting for its far end.
    bool m_secondLine = false;    // A Cobb angle drawing its second line.
    bool m_firstDone = false;     // A Cobb angle with its first line drawn.
    QPointF m_turnPivot, m_turnStart;
    double m_turnAxis[3] = {0, 0, 1};
    bool m_turning = false;
    // From image pixels to millimetres across and down, for angles.
    QPointF toMm(QPointF p) const;
};
