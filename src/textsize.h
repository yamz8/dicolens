#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

// The desktop's text size, followed live: what `omarchy display text size`
// sets as the shell's `base-size` under `[font]` in
// ~/.config/omarchy/shell.toml, over the theme's own shell.toml, over the
// shell's default of 12 px. The window's type scales with it.
class TextSize : public QObject {
    Q_OBJECT
    Q_PROPERTY(int size READ size NOTIFY changed)
    Q_PROPERTY(double scale READ scale NOTIFY changed)

public:
    // Tests pass a config file and a theme directory of their own.
    explicit TextSize(const QString &shellConfig = {}, const QString &themeDirectory = {}, QObject *parent = nullptr);

    int size() const { return m_size; }
    // Against the default, so 1 is the window as it was drawn.
    double scale() const { return m_size / 12.0; }

    // `base-size` under `[font]` in a shell.toml, or 0 when it isn't set.
    static int readBaseSize(const QString &path);

signals:
    void changed();

private:
    void reload();

    QString m_shellConfig;
    QString m_themeDirectory;
    int m_size = 12;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
