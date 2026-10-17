// Portal file chooser follows Omacut (MIT, David Heinemeier Hansson).
#include "portalfilepicker.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace {
struct PortalFilterRule {
    uint type;
    QString pattern;
};

using PortalFilterRules = QList<PortalFilterRule>;

struct PortalFileFilter {
    QString name;
    PortalFilterRules rules;
};

using PortalFileFilters = QList<PortalFileFilter>;

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFilterRule &rule) {
    argument.beginStructure();
    argument << rule.type << rule.pattern;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFilterRule &rule) {
    argument.beginStructure();
    argument >> rule.type >> rule.pattern;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFileFilter &filter) {
    argument.beginStructure();
    argument << filter.name << filter.rules;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFileFilter &filter) {
    argument.beginStructure();
    argument >> filter.name >> filter.rules;
    argument.endStructure();
    return argument;
}

void registerPortalFilterTypes() {
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalFilterRule>();
        qDBusRegisterMetaType<PortalFilterRules>();
        qDBusRegisterMetaType<PortalFileFilter>();
        qDBusRegisterMetaType<PortalFileFilters>();
        return true;
    }();
    Q_UNUSED(registered);
}

// DICOM files on CDs often have no extension; the portal still knows them
// by the "DICM" magic, as application/dicom.
PortalFileFilter dicomFilter() {
    return {
        QCoreApplication::translate("scans", "DICOM images"),
        {
            {1, QStringLiteral("application/dicom")},
            {0, QStringLiteral("*.dcm")},
            {0, QStringLiteral("*.DCM")},
            {0, QStringLiteral("*.dicom")},
            {0, QStringLiteral("*.ima")},
            {0, QStringLiteral("*.IMA")},
        },
    };
}

PortalFileFilter allFilter() {
    return {QCoreApplication::translate("scans", "All files"), {{0, QStringLiteral("*")}}};
}

PortalFileFilter pngFilter() {
    return {QCoreApplication::translate("scans", "PNG image"), {{1, QStringLiteral("image/png")}, {0, QStringLiteral("*.png")}}};
}

QString portalToken() {
    return QStringLiteral("scans_%1").arg(QRandomGenerator::global()->generate());
}

QByteArray portalPathBytes(const QString &path) {
    QByteArray bytes = path.toUtf8();
    bytes.append('\0');
    return bytes;
}

QString startFolder() {
    const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    return QDir(pictures).exists() ? pictures : QDir::homePath();
}
}

PortalFilePicker::PortalFilePicker(QObject *parent) : FilePicker(parent) {
    registerPortalFilterTypes();
}

void PortalFilePicker::choose(Purpose purpose, const QString &suggestedName) {
    if (m_pending)
        return;

    QVariantMap options;
    options.insert(QStringLiteral("modal"), true);
    QString title, method = QStringLiteral("OpenFile");
    if (purpose == Purpose::SaveImage) {
        method = QStringLiteral("SaveFile");
        title = QCoreApplication::translate("scans", "Save Image");
        options.insert(QStringLiteral("accept_label"), QCoreApplication::translate("scans", "Save"));
        options.insert(QStringLiteral("current_name"), suggestedName);
        options.insert(QStringLiteral("current_folder"), portalPathBytes(startFolder()));
        options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{pngFilter()}));
    } else if (purpose == Purpose::OpenFolder) {
        title = QCoreApplication::translate("scans", "Open a Folder of Scans");
        options.insert(QStringLiteral("accept_label"), QCoreApplication::translate("scans", "Open"));
        options.insert(QStringLiteral("directory"), true);
        options.insert(QStringLiteral("multiple"), false);
    } else {
        title = QCoreApplication::translate("scans", "Open a Scan");
        options.insert(QStringLiteral("accept_label"), QCoreApplication::translate("scans", "Open"));
        options.insert(QStringLiteral("multiple"), true);
        options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{dicomFilter(), allFilter()}));
        options.insert(QStringLiteral("current_filter"), QVariant::fromValue(dicomFilter()));
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusInterface portal(QStringLiteral("org.freedesktop.portal.Desktop"),
                          QStringLiteral("/org/freedesktop/portal/desktop"),
                          QStringLiteral("org.freedesktop.portal.FileChooser"),
                          bus);
    if (!portal.isValid()) {
        emit failed(QCoreApplication::translate("scans", "The desktop's file chooser isn't available"));
        return;
    }

    // Subscribe to the Response signal at the request path the portal will
    // derive from our handle_token *before* making the call, so a response
    // can't slip past while our match rule is still being installed.
    const QString token = portalToken();
    options.insert(QStringLiteral("handle_token"), token);
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString predictedPath =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

    m_pending = true;
    m_purpose = purpose;
    if (!connectToRequestPath(predictedPath)) {
        clearPending();
        emit failed(QCoreApplication::translate("scans", "The desktop's file chooser didn't answer"));
        return;
    }

    auto *watcher = new QDBusPendingCallWatcher(
        portal.asyncCall(method, QString(), title, options), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();

        // A response may already have been handled while the reply was in flight.
        if (!m_pending)
            return;

        if (reply.isError()) {
            clearPending();
            emit failed(QCoreApplication::translate("scans", "The desktop's file chooser failed: %1").arg(reply.error().message()));
            return;
        }

        // Old portal versions can hand back a different request path than the
        // handle_token predicts; move our subscription over if so.
        const QString actualPath = reply.value().path();
        if (actualPath != m_pendingPath) {
            QDBusConnection::sessionBus().disconnect(
                QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                this, SLOT(handleResponse(uint,QVariantMap)));
            if (!connectToRequestPath(actualPath)) {
                clearPending();
                emit failed(QCoreApplication::translate("scans", "The desktop's file chooser didn't answer"));
            }
        }
    });
}

bool PortalFilePicker::connectToRequestPath(const QString &path) {
    m_pendingPath = path;
    return QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
        QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
        this, SLOT(handleResponse(uint,QVariantMap)));
}

void PortalFilePicker::handleResponse(uint response, const QVariantMap &results) {
    const Purpose purpose = m_purpose;
    clearPending();

    if (response != 0)
        return;

    QList<QUrl> urls;
    for (const auto &uri : results.value(QStringLiteral("uris")).toStringList())
        urls << QUrl(uri);
    if (!urls.isEmpty())
        emit selected(purpose, urls);
}

void PortalFilePicker::clearPending() {
    if (!m_pendingPath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(
            QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
            QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
            this, SLOT(handleResponse(uint,QVariantMap)));
    }

    m_pendingPath.clear();
    m_pending = false;
}
