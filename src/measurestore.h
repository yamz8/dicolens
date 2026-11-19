#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QString>

#include "measure.h"

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

    // Every measurement kept with a series, by plane, then slice, then in
    // the order they were drawn.
    struct Placed {
        int plane = 0;
        int slice = 0;
        Measure measure;
    };
    QList<Placed> all(const QString &series);

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
