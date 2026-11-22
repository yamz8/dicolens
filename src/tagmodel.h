#pragma once

#include <QtQml/qqmlregistration.h>
#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <memory>

#include "dicom.h"

// Every tag of one file, for the panel beside the pictures: sequences
// indented, an enhanced file's per-frame groups cut down to the shown
// frame's, and a search over tags, names, and values.
//
// The file is read and its rows are made on a worker, one read at a time:
// asked for another slice meanwhile, it finishes the read it's on, drops
// that result as stale, and reads only the latest. A file of many frames
// is parsed once and kept while its frames are stepped through, and the
// search goes through rows already made, so neither reads the file again.
class TagModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The tag list comes from the backend")
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { Depth = Qt::UserRole + 1, Tag, Name, Vr, Value, Item };

    struct Row {
        int depth = 0;
        QString tag;
        QString name;
        QString vr;
        QString value;
        // The heading of one item of a sequence, numbered from one, rather
        // than an element; perFrame when the item is a frame's.
        bool item = false;
        bool perFrame = false;
        int number = 0;
    };
    // How many rows are shown at most, and how many are searched.
    static constexpr int MostShown = 3000;
    static constexpr int MostRead = 100000;

    explicit TagModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_rows.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return m_rows.size(); }

    // Nothing is read while the panel is closed.
    bool wanted() const { return m_wanted; }
    void setWanted(bool wanted);
    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);
    // Whose tags to show: a file, and which of its frames.
    void show(const QString &path, int frame);
    // "Item 2" and "Frame 3" are words of the language.
    void retranslate();

    // Every row of a file as one frame sees it. Runs on the worker.
    static QList<Row> read(const DicomFile &file, int frame);

    // For tests: files read from disk, results dropped as stale, and
    // whether a read is under way.
    int reads() const { return m_reads; }
    int dropped() const { return m_dropped; }
    bool reading() const { return m_reading; }

signals:
    void countChanged();

private:
    void refresh();
    void start();
    void filterRows();
    void setRows(const QList<Row> &rows);

    bool m_wanted = false;
    QString m_filter;
    QString m_path;
    int m_frame = 0;
    // The file last read, kept so its other frames need no second read.
    std::shared_ptr<const DicomFile> m_file;
    QString m_filePath;
    QList<Row> m_all;
    QList<Row> m_rows;
    int m_generation = 0;
    bool m_reading = false;
    int m_reads = 0;
    int m_dropped = 0;
};
