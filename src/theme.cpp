// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

namespace {

// Without Omarchy: a dark surface and Omarchy's fallback yellow.
const QHash<QString, QColor> &fallbackColors() {
    static const QHash<QString, QColor> colors{
        {"accent", QColor("#FFD60A")},  {"background", QColor("#0e0e10")}, {"foreground", QColor("#e6e6e9")},
        {"selection", QColor("#3a3a40")}, {"muted", QColor("#5c5c66")},    {"red", QColor("#f7768e")},
        {"green", QColor("#9ece6a")},   {"yellow", QColor("#e0af68")},     {"orange", QColor("#ff9e64")},
        {"blue", QColor("#7aa2f7")},    {"magenta", QColor("#bb9af7")},    {"cyan", QColor("#7dcfff")},
    };
    return colors;
}

// Older and hand-made themes name some colors differently. Each key takes the
// first of its other names the theme has, in the order omarchy-theme-color
// (Omarchy's full resolver) tries them.
const QList<QStringList> &aliases() {
    static const QList<QStringList> aliases{
        {"background", "bg", "color0"}, {"foreground", "fg", "color7"}, {"red", "color1"},
        {"green", "color2"}, {"yellow", "color3"}, {"blue", "color4"}, {"magenta", "color5", "purple"},
        {"cyan", "color6"}, {"muted", "color8", "dark_foreground"},
        {"selection", "selection_background", "color8", "background"}, {"orange", "yellow"},
    };
    return aliases;
}

double luminance(const QColor &c) {
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}

} // namespace

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl, const QString &hyprlandEvents)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    // Without Hyprland the corners stay sharp, like Omarchy's default.
    const bool hyprland = directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE");
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl : hyprland ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code == 0 && status == QProcess::NormalExit && value.isDouble() && value.toInt(-1) >= 0
            && value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
        if (m_roundingAgain) {
            m_roundingAgain = false;
            refreshRounding();
        }
    });

    // Hyprland announces a reload on its event socket once the new values are
    // in effect (a theme switch, the user's own config), so ask again then
    // rather than polling.
    const QString events = !hyprlandEvents.isEmpty() ? hyprlandEvents
                         : hyprland ? qEnvironmentVariable("XDG_RUNTIME_DIR") + "/hypr/"
                                          + qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE") + "/.socket2.sock"
                                    : QString();
    connect(&m_events, &QLocalSocket::readyRead, this, [this] {
        while (m_events.canReadLine())
            if (m_events.readLine().startsWith("configreloaded>>"))
                refreshRounding();
    });
    // Asked again on every connection, since a reload may have come between.
    connect(&m_events, &QLocalSocket::connected, this, [this] {
        m_reconnect.setInterval(1000);
        refreshRounding();
    });
    // A socket that can't be reached or drops is tried again, less often each time.
    m_reconnect.setSingleShot(true);
    m_reconnect.setInterval(1000);
    connect(&m_events, &QLocalSocket::disconnected, &m_reconnect, qOverload<>(&QTimer::start));
    connect(&m_events, &QLocalSocket::errorOccurred, &m_reconnect, qOverload<>(&QTimer::start));
    connect(&m_reconnect, &QTimer::timeout, this, [this, events] {
        m_reconnect.setInterval(std::min(m_reconnect.interval() * 2, 60000));
        m_events.abort();
        m_events.connectToServer(events, QIODevice::ReadOnly);
    });
    if (!events.isEmpty())
        m_events.connectToServer(events, QIODevice::ReadOnly);
    refreshRounding();
    reload();
}

Theme::~Theme() {
    // Nothing reconnects or asks again while the members go.
    m_events.disconnect();
    m_roundingProcess.disconnect();
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty())
        return;
    // A query still running may have read the value from before the reload.
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingAgain = true;
        return;
    }
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

// Every `key = "#rrggbb"` in a colors.toml, quoted or bare, with or without a
// trailing comment, and whether the theme is light: its `mode`, a light.mode
// file beside it, or failing both a light background, as Omarchy decides it.
Theme::Palette Theme::readThemeColors(const QString &path) {
    Palette palette;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return palette;
    static const QRegularExpression entry(R"re(^\s*([a-z0-9_]+)\s*=\s*(["']?)(#[0-9a-fA-F]{6}|light|dark)\2\s*(#.*)?$)re");
    QHash<QString, QString> modes;
    while (!file.atEnd()) {
        const auto match = entry.match(QString::fromUtf8(file.readLine()).trimmed());
        if (!match.hasMatch())
            continue;
        const QString key = match.captured(1), value = match.captured(3);
        if (value.startsWith('#'))
            palette.colors.insert(key, QColor(value));
        else
            modes.insert(key, value);
    }
    for (const QStringList &names : aliases())
        for (const QString &name : names.mid(1))
            if (!palette.colors.contains(names.first()) && palette.colors.contains(name))
                palette.colors.insert(names.first(), palette.colors.value(name));

    QString mode = modes.value("mode", modes.value("theme_type"));
    if (mode.isEmpty() && QFileInfo::exists(QFileInfo(path).absolutePath() + "/light.mode"))
        mode = "light";
    if (mode.isEmpty() && palette.colors.contains("background")) {
        const QColor background = palette.colors.value("background");
        mode = background.red() + background.green() + background.blue() > 382 ? "light" : "dark";
    }
    palette.light = mode == "light";
    return palette;
}

QColor Theme::color(const QString &key) const {
    if (const auto it = m_colors.constFind(key); it != m_colors.constEnd())
        return *it;
    return fallbackColors().value(key, fallbackColors().value("foreground"));
}

QVariantMap Theme::colors() const {
    QVariantMap colors;
    for (auto it = fallbackColors().begin(); it != fallbackColors().end(); ++it)
        colors.insert(it.key(), it.value());
    for (auto it = m_colors.begin(); it != m_colors.end(); ++it)
        colors.insert(it.key(), it.value());
    return colors;
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QColor Theme::accentForeground() const {
    return luminance(accent()) > .179 ? QColor("black") : QColor("white");
}

void Theme::reload() {
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    const Palette palette = readThemeColors(m_directory + "/theme/colors.toml");
    const bool light = palette.colors.isEmpty() ? false : palette.light;
    if (palette.colors != m_colors || light != m_light) {
        m_colors = palette.colors;
        m_light = light;
        emit changed();
    }
}
