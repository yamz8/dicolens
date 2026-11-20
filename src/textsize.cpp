// Reading the shell's base-size follows Plain (MIT, David Heinemeier Hansson).
#include "textsize.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

TextSize::TextSize(const QString &shellConfig, const QString &themeDirectory, QObject *parent)
    : QObject(parent),
      m_shellConfig(shellConfig.isEmpty() ? QDir::homePath() + "/.config/omarchy/shell.toml" : shellConfig),
      m_themeDirectory(themeDirectory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : themeDirectory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &TextSize::reload);
    reload();
}

int TextSize::readBaseSize(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    bool inFont = false;
    static const QRegularExpression section("^\\s*\\[([^\\]]*)\\]");
    static const QRegularExpression baseSize("^\\s*base-size\\s*=\\s*([0-9]+)");
    for (const QString &line : QString::fromUtf8(file.readAll()).split('\n')) {
        if (const auto match = section.match(line); match.hasMatch())
            inFont = match.captured(1).trimmed() == "font";
        else if (const auto match = baseSize.match(line); inFont && match.hasMatch())
            return match.captured(1).toInt();
    }
    return 0;
}

void TextSize::reload() {
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // The slider replaces the file, and a theme switch its symlinks, which
    // drops them from the watch; a first use creates the file, and maybe its
    // folder. So the nearest folder that exists is watched too, and all of
    // it armed again every time.
    QStringList candidates{m_shellConfig, m_themeDirectory, m_themeDirectory + "/theme",
                           m_themeDirectory + "/theme/shell.toml"};
    for (const QString &start : {QFileInfo(m_shellConfig).absolutePath(), m_themeDirectory}) {
        QString ancestor = start;
        while (!QFileInfo::exists(ancestor) && ancestor != "/")
            ancestor = QFileInfo(ancestor).absolutePath();
        candidates << ancestor;
    }
    candidates.removeDuplicates();
    for (const QString &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    // The user's own size wins, then the theme's, then the shell's default,
    // the order the Omarchy shell resolves it in. Kept to sizes a window
    // can still be laid out at.
    int size = readBaseSize(m_shellConfig);
    if (size <= 0)
        size = readBaseSize(m_themeDirectory + "/theme/shell.toml");
    if (size <= 0)
        size = 12;
    size = std::clamp(size, 9, 24);
    if (size != m_size) {
        m_size = size;
        emit changed();
    }
}
