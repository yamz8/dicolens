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
        QCoreApplication::translate("dicolens", "DICOM images"),
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
    return {QCoreApplication::translate("dicolens", "All files"), {{0, QStringLiteral("*")}}};
}

PortalFileFilter mp4Filter() {
    return {QCoreApplication::translate("dicolens", "MP4 video"), {{1, QStringLiteral("video/mp4")}, {0, QStringLiteral("*.mp4")}}};
}

PortalFileFilter pngFilter() {
    return {QCoreApplication::translate("dicolens", "PNG image"), {{1, QStringLiteral("image/png")}, {0, QStringLiteral("*.png")}}};
}

QString portalToken() {
    return QStringLiteral("dicolens_%1").arg(QRandomGenerator::global()->generate());
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

void PortalFilePicker::choose(QObject *asker, Purpose purpose, const QString &suggestedName) {
    // A request whose Response never arrives (the portal crashed or was
    // killed with the dialog up) must not wedge every later dialog behind
    // it: close the stuck one best-effort and replace it.
    if (m_pending) {
        closePendingRequest();
        clearPending();
    }

    QVariantMap options;
    options.insert(QStringLiteral("modal"), true);
    QString title, method = QStringLiteral("OpenFile");
    if (purpose == Purpose::SaveImage) {
        method = QStringLiteral("SaveFile");
        title = QCoreApplication::translate("dicolens", "Save Image");
        options.insert(QStringLiteral("accept_label"), QCoreApplication::translate("dicolens", "Save"));
        options.insert(QStringLiteral("current_name"), suggestedName);
        options.insert(QStringLiteral("current_folder"), portalPathBytes(startFolder()));
        options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{pngFilter()}));
    } else if (purpose == Purpose::SaveVideo) {
        method = QStringLiteral("SaveFile");
        title = QCoreApplication::translate("dicolens", "Save the Series as a Video");
        options.insert(QStringLiteral("accept_label"), QCoreApplication::translate("dicolens", "Save"));
        options.insert(QStringLiteral("current_name"), suggestedName);
        options.insert(QStringLiteral("current_folder"), portalPathBytes(startFolder()));
        options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{mp4Filter()}));
    } else if (purpose == Purpose::SaveFolder) {
        // The copy goes into a new folder inside the one picked.
        title = QCoreApplication::translate("dicolens", "Save an Anonymous Copy");
        options.insert(QStringLiteral("accept_label"), QCoreApplication::translate("dicolens", "Save Here"));
        options.insert(QStringLiteral("directory"), true);
        options.insert(QStringLiteral("multiple"), false);
        options.insert(QStringLiteral("current_folder"), portalPathBytes(QDir::homePath()));
    } else if (purpose == Purpose::OpenFolder) {
        title = QCoreApplication::translate("dicolens", "Open a Folder of Scans");
        options.insert(QStringLiteral("accept_label"), QCoreApplication::translate("dicolens", "Open"));
        options.insert(QStringLiteral("directory"), true);
        options.insert(QStringLiteral("multiple"), false);
    } else {
        title = QCoreApplication::translate("dicolens", "Open a Scan");
        options.insert(QStringLiteral("accept_label"), QCoreApplication::translate("dicolens", "Open"));
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
        emit failed(asker, QCoreApplication::translate("dicolens", "The desktop's file chooser isn't available"));
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
    m_asker = asker;
    if (!connectToRequestPath(predictedPath)) {
        clearPending();
        emit failed(asker, QCoreApplication::translate("dicolens", "The desktop's file chooser didn't answer"));
        return;
    }

    const int generation = ++m_pendingGeneration;
    auto *watcher = new QDBusPendingCallWatcher(
        portal.asyncCall(method, QString(), title, options), this);
    m_pendingWatcher = watcher;

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation]() {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (m_pendingWatcher == watcher)
            m_pendingWatcher = nullptr;

        // Superseded by a newer request, or a response may already have
        // been handled while the reply was in flight.
        if (generation != m_pendingGeneration || !m_pending)
            return;

        if (reply.isError()) {
            QObject *asker = m_asker;
            clearPending();
            emit failed(asker, QCoreApplication::translate("dicolens", "The desktop's file chooser failed: %1").arg(reply.error().message()));
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
                QObject *asker = m_asker;
                clearPending();
                emit failed(asker, QCoreApplication::translate("dicolens", "The desktop's file chooser didn't answer"));
            }
        }
    });
}

void PortalFilePicker::closePendingRequest() {
    if (m_pendingPath.isEmpty())
        return;
    QDBusInterface request(QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
                           QStringLiteral("org.freedesktop.portal.Request"),
                           QDBusConnection::sessionBus());
    request.asyncCall(QStringLiteral("Close"));
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
    QObject *asker = m_asker;
    clearPending();

    if (response != 0)
        return;

    QList<QUrl> urls;
    for (const auto &uri : results.value(QStringLiteral("uris")).toStringList())
        urls << QUrl(uri);
    if (!urls.isEmpty())
        emit selected(asker, purpose, urls);
}

void PortalFilePicker::clearPending() {
    // A caller that never gets its reply must not outlive the request it
    // belonged to (repeated stuck requests would otherwise pile them up).
    if (m_pendingWatcher) {
        m_pendingWatcher->deleteLater();
        m_pendingWatcher = nullptr;
    }
    if (!m_pendingPath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(
            QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
            QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
            this, SLOT(handleResponse(uint,QVariantMap)));
    }

    m_pendingPath.clear();
    m_pending = false;
    m_asker.clear();
}
