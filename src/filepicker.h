#pragma once

#include <QObject>
#include <QUrl>

// What the backend asks of a file chooser, so tests can hand it a fake.
class FilePicker : public QObject {
    Q_OBJECT

public:
    enum class Purpose { OpenScan, OpenFolder, SaveImage, SaveVideo, SaveFolder };

    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    // The panes share one picker, so each says who is asking and takes only
    // the answers that come back for it; a dialog that is cancelled answers
    // nobody. suggestedName is the file name a save starts with.
    virtual void choose(QObject *asker, Purpose purpose, const QString &suggestedName = {}) = 0;

signals:
    void selected(QObject *asker, FilePicker::Purpose purpose, const QList<QUrl> &urls);
    void failed(QObject *asker, const QString &message);
};
