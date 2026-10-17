#pragma once

#include <QObject>
#include <QUrl>

// What the backend asks of a file chooser, so tests can hand it a fake.
class FilePicker : public QObject {
    Q_OBJECT

public:
    enum class Purpose { OpenScan, OpenFolder, SaveImage };

    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    // suggestedName is the file name a save starts with.
    virtual void choose(Purpose purpose, const QString &suggestedName = {}) = 0;

signals:
    void selected(FilePicker::Purpose purpose, const QList<QUrl> &urls);
    void failed(const QString &message);
};
