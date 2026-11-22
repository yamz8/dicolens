#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

// The window a pane looks through: its centre and width, whether it's
// inverted, and the colour scale; each series' own, so going back to one
// finds it as it was left; and what was set before, to undo.
class Windowing : public QObject {
    Q_OBJECT

public:
    struct State {
        double center = 0;
        double width = 0;
        bool inverted = false;
        bool operator==(const State &o) const { return center == o.center && width == o.width && inverted == o.inverted; }
    };

    explicit Windowing(QObject *parent = nullptr) : QObject(parent) {}

    double center() const { return m_now.center; }
    double width() const { return m_now.width; }
    bool inverted() const { return m_now.inverted; }
    // Grey, or a colour scale for PET and nuclear medicine.
    int colorMap() const { return m_colorMap; }
    // Whether a slice has been seen to take the window from.
    bool isSet() const { return m_set; }
    // The CT preset the window is, in English, or null.
    const char *presetName() const;
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }

    // A new scan: no series has a window yet.
    void forget();
    // Leaves one series, keeping its window for a return, and takes up
    // the next one's; a PET starts in colour.
    void enter(int from, int to, bool pet);
    // The first slice seen sets the window for the whole series, so
    // scrolling doesn't flicker between each file's own.
    void seen(int series, double center, double width);

    void beginDrag();
    // Across sets contrast, and down brightness, in pixels dragged.
    void drag(double dx, double dy);
    bool preset(int number);
    void fileWindow();
    void set(double center, double width);
    void toggleInvert();
    void nextColorMap();
    void undo();
    void redo();
    // Takes another's, so views of one series look alike.
    void match(const Windowing &other);

signals:
    void changed();
    void historyChanged();

private:
    void push();
    void apply(const State &state);

    State m_now;
    int m_colorMap = 0;
    bool m_set = false;
    State m_file;
    State m_dragStart;
    QHash<int, State> m_series;
    // Each series' own window, which 0 goes back to after a return to it.
    QHash<int, State> m_seriesFile;
    QList<State> m_undo;
    QList<State> m_redo;
};
