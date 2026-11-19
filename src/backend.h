#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QThreadPool>
#include <QTimer>
#include <QTransform>
#include <QUrl>
#include <QVariant>
#include <atomic>
#include <functional>
#include <memory>

#include "filepicker.h"
#include "pixels.h"
#include "reformat.h"
#include "study.h"
#include "volumerender.h"

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
    Q_PROPERTY(QString colorMapName READ colorMapName NOTIFY windowChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(QVariantMap info READ info NOTIFY sliceChanged)
    Q_PROPERTY(QStringList edges READ edges NOTIFY orientationChanged)
    Q_PROPERTY(QString planeName READ planeName NOTIFY orientationChanged)
    Q_PROPERTY(bool canReformat READ canReformat NOTIFY sliceChanged)
    Q_PROPERTY(QString slabName READ slabName NOTIFY orientationChanged)
    Q_PROPERTY(int rotation READ rotation NOTIFY orientationChanged)
    Q_PROPERTY(bool flipped READ flipped NOTIFY orientationChanged)
    Q_PROPERTY(bool marksShown READ marksShown NOTIFY marksChanged)
    Q_PROPERTY(bool hasMarks READ hasMarks NOTIFY imageChanged)
    Q_PROPERTY(QVariantList reports READ reportList NOTIFY reportsChanged)
    Q_PROPERTY(int reportIndex READ reportIndex NOTIFY reportChanged)
    Q_PROPERTY(QString reportTitle READ reportTitle NOTIFY reportChanged)
    Q_PROPERTY(bool reportIsPdf READ reportIsPdf NOTIFY reportChanged)
    Q_PROPERTY(QVariantList reportLines READ reportLines NOTIFY reportChanged)
    Q_PROPERTY(bool tagsWanted READ tagsWanted WRITE setTagsWanted NOTIFY tagsChanged)
    Q_PROPERTY(QString tagFilter READ tagFilter WRITE setTagFilter NOTIFY tagsChanged)
    Q_PROPERTY(QVariantList tags READ tags NOTIFY tagsChanged)
    Q_PROPERTY(bool fused READ fused NOTIFY fusionChanged)
    Q_PROPERTY(QString fusionName READ fusionName NOTIFY fusionChanged)

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
    // Grey, or a colour scale for PET and nuclear medicine.
    int colorMap() const { return m_colorMap; }
    QString colorMapName() const;
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    bool playing() const { return m_play.isActive(); }
    QVariantMap info() const { return m_info; }
    QStringList edges() const;
    // Which way the shown picture cuts the body, translated.
    QString planeName() const;
    // A stack of even, parallel slices can be cut the other two ways.
    bool canReformat() const { return m_volume.valid; }
    // 0 is the plane as scanned; 1 and 2 the others.
    int plane() const { return m_plane; }
    // The slab the picture is made of, such as "MIP 10 mm", or nothing.
    QString slabName() const;
    // Where in the patient the shown picture lies, for reference lines.
    ImagePlane imagePlane() const;
    // Quarter turns clockwise, after a flip left to right when flipped.
    int rotation() const { return m_rotation; }
    bool flipped() const { return m_flipped; }
    // From the image's pixels to the picture as turned, for one of size.
    QTransform orientation(const QSizeF &size) const;
    bool marksShown() const { return m_marksShown; }
    bool hasMarks() const;
    // What was left on the shown slice: overlay bits as an alpha mask the
    // size of the image, and presentation states' outlines and notes.
    QImage overlay() const;
    QList<Mark> marks() const;
    QVariantList reportList() const;
    int reportIndex() const { return m_reportIndex; }
    QString reportTitle() const;
    bool reportIsPdf() const;
    QVariantList reportLines() const { return m_reportLines; }
    // Where a PDF report is opened; tests catch it rather than launch a viewer.
    void setUrlOpener(std::function<bool(const QUrl &)> opener) { m_openUrl = std::move(opener); }
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
    Q_INVOKABLE void rotate(int quarterTurns);
    Q_INVOKABLE void flipHorizontal();
    Q_INVOKABLE void flipVertical();
    Q_INVOKABLE void resetOrientation();
    // Cuts the series the next way: as scanned, then the other two.
    Q_INVOKABLE void nextPlane();
    Q_INVOKABLE void setPlane(int plane);
    // Thin, then a MIP, a MinIP, and an average of a slab.
    Q_INVOKABLE void nextSlab();
    Q_INVOKABLE void nextSlabThickness();
    Q_INVOKABLE void toggleMarks();
    Q_INVOKABLE void openReport(int index);
    Q_INVOKABLE void openStudyReport();
    Q_INVOKABLE void stepReport(int delta);
    Q_INVOKABLE void closeReport();

    // Side by side: shows the scan another backend has open, from the same
    // kind of series in another study or else the next series, and follows
    // it from slice to slice.
    Q_INVOKABLE void share(Backend *other);
    // Four views: shows what another backend shows, cut another way, from
    // the slices it reads rather than reading them again; its PET too.
    // Null stops.
    Q_INVOKABLE void mirror(Backend *leader, int plane);
    // Takes another's window, so views of one series look alike.
    Q_INVOKABLE void matchWindow(Backend *other);
    Q_INVOKABLE void follow(Backend *other);
    Q_INVOKABLE void stop();

    Q_INVOKABLE QString valueAt(double x, double y) const;
    Q_INVOKABLE QString distance(double x1, double y1, double x2, double y2) const;
    // The area of the ellipse in a box of image pixels, and what's in it.
    Q_INVOKABLE QString areaStats(double x, double y, double width, double height) const;
    Q_INVOKABLE QString angleText(double degrees) const;
    // The patient point at an image position, with the frame of reference
    // it's in, as [x, y, z, frame]; empty when the picture has no place.
    Q_INVOKABLE QVariantList pointAt(double x, double y) const;
    // Makes every plane go through a point, and shows the one through it
    // that this view cuts, when it's in the given frame of reference.
    Q_INVOKABLE bool goThrough(const QVariantList &point);
    // Tilts the cut by some degrees about the picture's up-down axis (0)
    // or its left-right one (1), making it oblique.
    Q_INVOKABLE void tilt(int axis, double degrees);

    // Where the planes meet, in this picture's pixels, when it was set by
    // a click and lies in this plane.
    bool crossing(QPointF &at) const;
    // Turning this plane about an axis through where the planes meet, from
    // where it was when the turn began: dragging another view's reference
    // line.
    void beginTurn();
    void turnPlane(const double *axis, double radians);
    // The patient point where the planes meet.
    void crossingPoint(double *point) const;
    // Millimetres between pixels across and down, where the picture has them.
    bool spacingAt(double x, double y, double &across, double &down) const;
    Q_INVOKABLE void nextColorMap();
    Q_INVOKABLE void exportImage();
    // The series (or plane) as an MP4, as shown: windowed, turned, coloured.
    Q_INVOKABLE void exportVideo();
    // A copy of every image without what identifies the patient.
    Q_INVOKABLE void exportAnonymous();
    Q_INVOKABLE void copyImage();
    Q_INVOKABLE void dismissProblem();
    // A word from the window about what it did, such as linking the panes.
    Q_INVOKABLE void announce(const QString &notice) { flash(notice); }

    // A PET over the CT or MR it was taken with, in colour: on or off, and
    // how strongly.
    bool fused() const { return m_fused; }
    // Such as "PET over CT  ·  50%", or nothing.
    QString fusionName() const;
    Q_INVOKABLE void toggleFusion();
    Q_INVOKABLE void nextFusionOpacity();
    // The PET's voxels, once read, and how they're drawn over.
    std::shared_ptr<const VoxelGrid> fusionGrid() const { return m_fused ? m_fusionGrid : nullptr; }
    Overlay3D fusionLook() const;

    // The series' decoded slices and its volume, for the 3D view.
    const QVector<Frame> &frames() const { return m_frames; }
    const Volume &volume() const { return m_volume; }
    bool allDecoded() const { return !m_frames.isEmpty() && m_decodedCount == m_frames.size(); }
    bool crossingShown() const { return m_crossingShown; }
    // Saves or copies a picture made elsewhere, such as the 3D view's.
    void exportPicture(const QImage &picture);
    void copyPicture(const QImage &picture);

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
    void orientationChanged();
    void marksChanged();
    void reportChanged();
    void reportsChanged();
    void fusionChanged();
    // A slice of the shown series has been read, for views mirroring this one.
    void frameArrived(int index);

private:
    struct WindowState {
        double center = 0;
        double width = 0;
        bool inverted = false;
        bool operator==(const WindowState &o) const { return center == o.center && width == o.width && inverted == o.inverted; }
    };

    void scanFinished(int generation, const Scan &scan, const QString &focusPath);
    void showScan(const Scan &scan, int series, int slice);
    void selectReport(int index, bool launch);
    void startDecoding();
    // Reads a series' slices on the pool, the one wanted first, each handed
    // to done on this thread with the job's generation.
    std::shared_ptr<DecodeJob> decode(const Series &series, int wanted, std::function<void(int, int, const Frame &)> done);
    // The series to fuse with the shown one: a PET with a CT or MR, or the
    // other way, lined up in one frame of reference; -1 if none.
    int fusionPartner(int series) const;
    void startFusion(int series);
    void fusionDecoded(int generation, int index, const Frame &frame);
    void dropFusion();
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
    void saveVideo(const QString &path);
    void saveAnonymous(const QString &folder);
    // Millimetres a pixel across and down at a point, if the image says.
    // What a value means: Hounsfield units, an SUV, or itself.
    double shownValue(double value) const;
    QString unit() const;
    QString valueText(const Frame &frame, int x, int y, const QString &where) const;
    const Series *currentSeries() const;
    const Frame *currentFrame() const;
    // Shows a picture built from the slices, not one of them.
    bool built() const { return m_plane != 0 || m_slab != 0; }
    void build();
    // The slice of the series behind what's shown: the one shown, or the
    // one through the cursor.
    int acquiredSlice() const;

    FilePicker *m_picker;
    bool m_asked = false;   // Waiting on the picker for this backend.
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

    Volume m_volume;
    Cut m_cut;
    int m_plane = 0;
    int m_slab = 0;            // 0 thin, then MIP, MinIP, and average.
    double m_slabMm = 10;
    int m_cursor[3] = {0, 0, 0};   // A voxel the planes meet at.
    bool m_crossingShown = false;  // Set by a click, so drawn.
    Oblique m_oblique;             // Plane 3: at any angle.
    double m_turnRow[3] = {1, 0, 0}, m_turnColumn[3] = {0, 1, 0};
    double m_low = 0, m_high = 0;  // The volume's values, once it's all read.
    bool m_rangeKnown = false;
    void becomeOblique(const double *center, const double *row, const double *column);
    void slabFor(double step, int &half, Projection &projection) const;
    Frame m_built;
    QString m_buildError;

    double m_center = 0;
    double m_width = 0;
    bool m_inverted = false;
    int m_colorMap = 0;
    bool m_windowSet = false;
    WindowState m_fileWindow;
    WindowState m_dragStart;
    QHash<int, WindowState> m_seriesWindows;
    // Each series' own window, which 0 goes back to after a return to it.
    QHash<int, WindowState> m_seriesFileWindows;
    QList<WindowState> m_undo;
    QList<WindowState> m_redo;

    QTimer m_play;
    QVariantMap m_info;

    int m_rotation = 0;
    bool m_flipped = false;
    bool m_marksShown = true;
    int m_reportIndex = -1;
    QVariantList m_reportLines;
    std::function<bool(const QUrl &)> m_openUrl;

    QPointer<Backend> m_leader;
    QList<QMetaObject::Connection> m_leaderConnections;
    void adoptFrame(int index);
    void adoptFusion();
    void storeFrame(int index, const Frame &frame);

    bool m_fused = false;
    int m_fusionSeries = -1;
    double m_fusionOpacity = 0.5;
    std::shared_ptr<DecodeJob> m_fusionJob;
    QVector<Frame> m_fusionFrames;
    int m_fusionDecoded = 0;
    std::shared_ptr<const VoxelGrid> m_fusionGrid;
    double m_fusionLow = 0, m_fusionHigh = 1;
    // A picture from elsewhere waiting on the picker to be saved.
    QImage m_picture;

    bool m_tagsWanted = false;
    QString m_tagFilter;
    QVariantList m_tags;
};
