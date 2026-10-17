#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPointF>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <atomic>
#include <memory>

#include "filepicker.h"
#include "pixels.h"
#include "study.h"

struct DecodeJob;

// The viewer's state: what's open, which slice is showing and through what
// window, decoding in the background nearest the shown slice first.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasImages READ hasImages NOTIFY seriesChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(int scanDone READ scanDone NOTIFY scanProgress)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY scanProgress)
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(QVariantList series READ seriesList NOTIFY seriesChanged)
    Q_PROPERTY(int thumbnailsVersion READ thumbnailsVersion NOTIFY thumbnailsChanged)
    Q_PROPERTY(int seriesIndex READ seriesIndex WRITE selectSeries NOTIFY sliceChanged)
    Q_PROPERTY(int sliceIndex READ sliceIndex WRITE setSliceIndex NOTIFY sliceChanged)
    Q_PROPERTY(int sliceCount READ sliceCount NOTIFY sliceChanged)
    Q_PROPERTY(int decodedCount READ decodedCount NOTIFY decodedChanged)
    Q_PROPERTY(bool frameReady READ frameReady NOTIFY imageChanged)
    Q_PROPERTY(QString frameError READ frameError NOTIFY imageChanged)
    Q_PROPERTY(bool color READ color NOTIFY imageChanged)
    Q_PROPERTY(int imageWidth READ imageWidth NOTIFY imageChanged)
    Q_PROPERTY(int imageHeight READ imageHeight NOTIFY imageChanged)
    Q_PROPERTY(double pixelAspect READ pixelAspect NOTIFY sliceChanged)
    Q_PROPERTY(double windowCenter READ windowCenter NOTIFY windowChanged)
    Q_PROPERTY(double windowWidth READ windowWidth NOTIFY windowChanged)
    Q_PROPERTY(QString windowName READ windowName NOTIFY windowChanged)
    Q_PROPERTY(bool inverted READ inverted NOTIFY windowChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(QVariantMap info READ info NOTIFY sliceChanged)
    Q_PROPERTY(QStringList edges READ edges NOTIFY sliceChanged)
    Q_PROPERTY(bool tagsWanted READ tagsWanted WRITE setTagsWanted NOTIFY tagsChanged)
    Q_PROPERTY(QString tagFilter READ tagFilter WRITE setTagFilter NOTIFY tagsChanged)
    Q_PROPERTY(QVariantList tags READ tags NOTIFY tagsChanged)

public:
    explicit Backend(FilePicker *picker, QObject *parent = nullptr);
    ~Backend() override;

    bool hasImages() const { return !m_scan.series.isEmpty(); }
    bool scanning() const { return m_scanning; }
    int scanDone() const { return m_scanDone; }
    int scanTotal() const { return m_scanTotal; }
    QString problem() const { return m_problem; }
    QString notice() const { return m_notice; }
    QVariantList seriesList() const;
    int thumbnailsVersion() const { return m_thumbnailsVersion; }
    int seriesIndex() const { return m_seriesIndex; }
    int sliceIndex() const { return m_sliceIndex; }
    int sliceCount() const;
    int decodedCount() const { return m_decodedCount; }
    bool frameReady() const;
    QString frameError() const;
    bool color() const;
    int imageWidth() const { return m_image.width(); }
    int imageHeight() const { return m_image.height(); }
    double pixelAspect() const;
    double windowCenter() const { return m_center; }
    double windowWidth() const { return m_width; }
    QString windowName() const;
    bool inverted() const { return m_inverted; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    bool playing() const { return m_play.isActive(); }
    QVariantMap info() const { return m_info; }
    QStringList edges() const;
    bool tagsWanted() const { return m_tagsWanted; }
    void setTagsWanted(bool wanted);
    QString tagFilter() const { return m_tagFilter; }
    void setTagFilter(const QString &filter);
    QVariantList tags() const { return m_tags; }

    // The windowed picture of the shown slice, for the view and for export.
    QImage image() const { return m_image; }
    QImage thumbnail(int series) const;
    const Scan &scan() const { return m_scan; }

    Q_INVOKABLE void open();
    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void load(const QVariantList &urlsOrPaths);
    void loadPaths(const QStringList &paths);

    Q_INVOKABLE void selectSeries(int index);
    Q_INVOKABLE void stepSeries(int delta);
    Q_INVOKABLE void setSliceIndex(int index);
    Q_INVOKABLE void step(int delta);

    Q_INVOKABLE void beginWindowDrag();
    Q_INVOKABLE void dragWindow(double dx, double dy);
    Q_INVOKABLE void preset(int number);
    Q_INVOKABLE void fileWindow();
    Q_INVOKABLE void fullRange();
    Q_INVOKABLE void toggleInvert();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void stop();

    Q_INVOKABLE QString valueAt(double x, double y) const;
    Q_INVOKABLE QString distance(double x1, double y1, double x2, double y2) const;
    Q_INVOKABLE void exportImage();
    Q_INVOKABLE void copyImage();
    Q_INVOKABLE void dismissProblem();

    // Remakes the text made in C++ after a language switch.
    void retranslate();

    // For tests: waits until the scan and the shown slice are decoded.
    bool waitForSlice(int milliseconds);

signals:
    void opened();
    void seriesChanged();
    void scanningChanged();
    void scanProgress();
    void problemChanged();
    void noticeChanged();
    void thumbnailsChanged();
    void sliceChanged();
    void decodedChanged();
    void imageChanged();
    void windowChanged();
    void historyChanged();
    void playingChanged();
    void tagsChanged();

private:
    struct WindowState {
        double center = 0;
        double width = 0;
        bool inverted = false;
        bool operator==(const WindowState &o) const { return center == o.center && width == o.width && inverted == o.inverted; }
    };

    void scanFinished(int generation, const Scan &scan, const QString &focusPath);
    void startDecoding();
    void frameDecoded(int generation, int index, const Frame &frame);
    void thumbnailDecoded(int generation, int series, const QImage &image);
    void showSlice();
    void rerender();
    void updateInfo();
    void updateTags();
    void pushHistory();
    void applyWindow(const WindowState &state);
    void setProblem(const QString &problem);
    void flash(const QString &notice);
    void saveImage(const QString &path);
    const Series *currentSeries() const;
    const Frame *currentFrame() const;

    FilePicker *m_picker;
    QThreadPool m_pool;
    QThreadPool m_thumbnailPool;
    int m_scanGeneration = 0;
    int m_jobGeneration = 0;
    std::shared_ptr<std::atomic<bool>> m_scanCancel;
    std::shared_ptr<std::atomic<bool>> m_thumbnailCancel;

    Scan m_scan;
    bool m_scanning = false;
    int m_scanDone = 0;
    int m_scanTotal = 0;
    QString m_problem;
    QString m_notice;
    QTimer m_noticeTimer;

    mutable QMutex m_thumbnailMutex;
    QHash<int, QImage> m_thumbnails;
    int m_thumbnailsVersion = 0;

    int m_seriesIndex = -1;
    int m_sliceIndex = 0;
    std::shared_ptr<DecodeJob> m_job;
    QVector<Frame> m_frames;
    QVector<bool> m_decoded;
    int m_decodedCount = 0;
    QImage m_image;

    double m_center = 0;
    double m_width = 0;
    bool m_inverted = false;
    bool m_windowSet = false;
    WindowState m_fileWindow;
    WindowState m_dragStart;
    QHash<int, WindowState> m_seriesWindows;
    QList<WindowState> m_undo;
    QList<WindowState> m_redo;

    QTimer m_play;
    QVariantMap m_info;

    bool m_tagsWanted = false;
    QString m_tagFilter;
    QVariantList m_tags;
};
