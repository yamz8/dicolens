#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QString>

// A ruler, area, angle, or Cobb angle drawn on a picture.
struct Measure {
    int kind = 1;            // As the tool that drew it.
    QPointF a, b, c, d;      // Image pixels; c is an angle's far end,
                             // and c to d a Cobb angle's second line.
};

// Every measurement, kept with the series it was drawn on, by its UID, so
// they come back when the scans are opened again. Each series' are saved
// in a file of its own under the app's data folder, never beside the scans.
// Those on a plane tilted to any angle last only until the app closes,
// since the plane itself does.
class MeasureStore : public QObject {
    Q_OBJECT

public:
    static MeasureStore &instance();

    // Where the files go; tests and the demo keep theirs apart.
    void setFolder(const QString &folder);
    QString folder() const { return m_folder; }

    // A picture's measurements: a series, a plane of it (0 as scanned, 3
    // tilted), and a slice of that plane.
    QList<Measure> at(const QString &series, int plane, int slice);
    void add(const QString &series, int plane, int slice, const Measure &measure);
    bool removeLast(const QString &series, int plane, int slice);
    void clear(const QString &series, int plane, int slice);

signals:
    void changed();

private:
    MeasureStore();
    QString path(const QString &series) const;
    void load(const QString &series);
    void save(const QString &series);
    static QString slot(int plane, int slice) { return QStringLiteral("%1/%2").arg(plane).arg(slice); }

    QString m_folder;
    QHash<QString, QHash<QString, QList<Measure>>> m_series;
    QSet<QString> m_loaded;
};
