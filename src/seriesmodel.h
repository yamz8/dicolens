#pragma once

#include <QtQml/qqmlregistration.h>
#include <QAbstractListModel>
#include <QList>
#include <QVector>

#include "study.h"

// The open scan's series for the list beside the pictures: a row a series,
// the first of each study carrying the study's heading. A thumbnail
// arriving changes its own row and nothing else.
class SeriesModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The series list comes from the backend")
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        Title = Qt::UserRole + 1,
        Modality,
        Images,         // How many pictures the series has.
        Number,
        Study,          // The study's heading, on its first series only.
        StudyDate,
        Decodable,
        Thumbnail,      // Where the list's picture of it is, once there is one.
    };

    explicit SeriesModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_series.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return m_series.size(); }
    // A row's title, for whoever looks a series up by name.
    QString title(int row) const { return data(index(row), Title).toString(); }

    void reset(const QList<Series> &series);
    void thumbnailArrived(int row);
    // The titles and dates are words and numbers of the language.
    void retranslate();

signals:
    void countChanged();

private:
    QList<Series> m_series;
    // Counts each row's thumbnails, so a new one has a new address.
    QVector<int> m_thumbnails;
    int m_scans = 0;
};
