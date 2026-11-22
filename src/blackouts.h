#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>

#include "anonymize.h"
#include "study.h"

// The regions an anonymous copy blacks out of the pictures: rectangles in
// image pixels, kept by series, as a name burned into an ultrasound is in
// the same place in every image of it. Every pane showing a series sees its
// regions. They're kept by the series' UID until the app closes, so opening
// the copy to look at it and then the scans again finds them still there;
// they aren't saved, as they're part of making one copy.
class Blackouts : public QObject {
    Q_OBJECT

public:
    explicit Blackouts(QObject *parent = nullptr) : QObject(parent) {}

    // A scan has been opened: its series, and whether each one's pictures
    // can be drawn again, which the anonymizer said from one of its files.
    void reset(const QList<Series> &series, const QList<Anonymize::Redraw> &redraws);

    // By a series' place in the open scan.
    QList<QRect> of(int series) const;
    Anonymize::Redraw redraw(int series) const { return m_redraws.value(series); }
    void add(int series, const QRect &region);
    bool removeLast(int series);
    bool clear(int series);

    // Every region of the open scan, as a copy's options take them.
    QHash<int, QList<QRect>> all() const;

signals:
    void changed();

private:
    QStringList m_keys;                       // Each series of the open scan, as it's kept by.
    QList<Anonymize::Redraw> m_redraws;
    QHash<QString, QList<QRect>> m_regions;   // Of every scan opened so far.
};
