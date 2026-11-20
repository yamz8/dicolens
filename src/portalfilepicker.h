#pragma once

#include <QPointer>
#include <QVariantMap>

#include "filepicker.h"

class QDBusPendingCallWatcher;

// Opens files and folders through xdg-desktop-portal, so the chooser is the
// desktop's own. Follows Omacut's portal picker (MIT, David Heinemeier Hansson).
class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void choose(QObject *asker, Purpose purpose, const QString &suggestedName = {}) override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    bool connectToRequestPath(const QString &path);
    void clearPending();
    // Best-effort Close of the pending portal request, if there is one.
    void closePendingRequest();

    QString m_pendingPath;
    bool m_pending = false;
    Purpose m_purpose = Purpose::OpenScan;
    QPointer<QObject> m_asker;
    // Bumped per request, so a late reply for a replaced request is ignored.
    int m_pendingGeneration = 0;
    QDBusPendingCallWatcher *m_pendingWatcher = nullptr;
};
