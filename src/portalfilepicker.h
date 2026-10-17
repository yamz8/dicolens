#pragma once

#include <QVariantMap>

#include "filepicker.h"

// Opens files and folders through xdg-desktop-portal, so the chooser is the
// desktop's own. Follows Omacut's portal picker (MIT, David Heinemeier Hansson).
class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void choose(Purpose purpose, const QString &suggestedName = {}) override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    bool connectToRequestPath(const QString &path);
    void clearPending();

    QString m_pendingPath;
    bool m_pending = false;
    Purpose m_purpose = Purpose::OpenScan;
};
