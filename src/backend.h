#pragma once

#include <QtQml/qqmlregistration.h>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QTimer>
#include <QTransform>
#include <QUrl>
#include <QVariant>
#include <functional>
#include <memory>

#include "claim.h"
#include "exporter.h"
#include "filepicker.h"
#include "fusion.h"
#include "library.h"
#include "measure.h"
#include "notices.h"
#include "picture.h"
#include "pixels.h"
#include "planes.h"
#include "readout.h"
#include "reformat.h"
#include "reportreader.h"
#include "seriesmodel.h"
#include "study.h"
#include "tagmodel.h"
#include "volumerender.h"
#include "windowing.h"

// One pane of the window, as QML talks to it: which series and slice it
// shows and how. What's open and every decoded slice belong to the library
// the panes share; the window, the planes, the PET over the picture, the
// reports, the tags, and saving each have a piece of their own, and this
// puts them together and says what changed.
class Backend : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The backend is given to QML as \"backend\"")
    Q_PROPERTY(bool hasImages READ hasImages NOTIFY seriesChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(int scanDone READ scanDone NOTIFY scanProgress)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY scanProgress)
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(SeriesModel *series READ seriesModel NOTIFY seriesChanged)
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
    Q_PROPERTY(ReportModel *reports READ reportModel CONSTANT)
    Q_PROPERTY(int reportCount READ reportCount NOTIFY reportsChanged)
    Q_PROPERTY(int reportIndex READ reportIndex NOTIFY reportChanged)
    Q_PROPERTY(QString reportTitle READ reportTitle NOTIFY reportChanged)
    Q_PROPERTY(QString reportStudy READ reportStudy NOTIFY reportChanged)
    Q_PROPERTY(QString reportDate READ reportDate NOTIFY reportChanged)
    Q_PROPERTY(bool reportIsPdf READ reportIsPdf NOTIFY reportChanged)
    Q_PROPERTY(QVariantList reportLines READ reportLines NOTIFY reportChanged)
    Q_PROPERTY(bool tagsWanted READ tagsWanted WRITE setTagsWanted NOTIFY tagsChanged)
    Q_PROPERTY(QString tagFilter READ tagFilter WRITE setTagFilter NOTIFY tagsChanged)
    Q_PROPERTY(TagModel *tags READ tagModel CONSTANT)
    Q_PROPERTY(bool fused READ fused NOTIFY fusionChanged)
    Q_PROPERTY(QString fusionName READ fusionName NOTIFY fusionChanged)

public:
    // The panes of one window are given one library; a backend made
    // without one makes its own.
    explicit Backend(FilePicker *picker, std::shared_ptr<Library> library = {}, QObject *parent = nullptr);
    ~Backend() override;

    Library *library() const { return m_library.get(); }
    const Scan &scan() const { return m_library->scan(); }
    bool hasImages() const { return !scan().series.isEmpty(); }
    bool scanning() const { return m_library->scanning(); }
    int scanDone() const { return m_library->scanDone(); }
    int scanTotal() const { return m_library->scanTotal(); }
    QString problem() const { return m_notices.problem(); }
    QString notice() const { return m_notices.notice(); }
    SeriesModel *seriesModel() const { return m_library->seriesModel(); }
    int seriesIndex() const { return m_seriesIndex; }
    int sliceIndex() const { return m_planes.index(); }
    int sliceCount() const;
    // How many of the series' slices are in memory now.
    int decodedCount() const;
    bool frameReady() const;
    QString frameError() const;
    bool color() const;
    // The shown picture's size, known without making the picture.
    int imageWidth() const { return m_shown.width; }
    int imageHeight() const { return m_shown.height; }
    double pixelAspect() const;
    double windowCenter() const { return m_window.center(); }
    double windowWidth() const { return m_window.width(); }
    QString windowName() const;
    bool inverted() const { return m_window.inverted(); }
    // Grey, or a colour scale for PET and nuclear medicine.
    int colorMap() const { return m_window.colorMap(); }
    QString colorMapName() const { return Readout::colorMapName(colorMap()); }
    bool canUndo() const { return m_window.canUndo(); }
    bool canRedo() const { return m_window.canRedo(); }
    bool playing() const { return m_play.isActive(); }
    QVariantMap info() const { return m_info; }
    QStringList edges() const;
    // Which way the shown picture cuts the body, translated.
    QString planeName() const { return Readout::planeName(imagePlane(), plane() == 3); }
    // A stack of even, parallel slices can be cut the other two ways.
    bool canReformat() const { return m_planes.volume().valid; }
    // 0 is the plane as scanned; 1 and 2 the others; 3 a tilted one.
    int plane() const { return m_planes.plane(); }
    // Which cut a tilted plane is, told from any other; empty otherwise.
    QString cutKey() const { return m_planes.cutKey(); }
    // The slab the picture is made of, such as "MIP 10 mm", or nothing.
    QString slabName() const { return Readout::slabName(m_planes.slab(), m_planes.slabMm()); }
    // Where in the patient the shown picture lies, for reference lines.
    ImagePlane imagePlane() const;
    // Quarter turns clockwise, after a flip left to right when flipped.
    int rotation() const { return m_turn.rotation; }
    bool flipped() const { return m_turn.flipped; }
    // From the image's pixels to the picture as turned, for one of size.
    QTransform orientation(const QSizeF &size) const { return m_turn.transform(size); }
    bool marksShown() const { return m_marksShown; }
    bool hasMarks() const;
    // What was left on the shown slice: overlay bits as an alpha mask the
    // size of the image, and presentation states' outlines and notes.
    QImage overlay() const { return m_shown.overlay; }
    QList<Mark> marks() const;
    ReportModel *reportModel() { return m_reports.model(); }
    int reportCount() const { return m_reports.count(); }
    int reportIndex() const { return m_reports.index(); }
    QString reportTitle() const { return m_reports.title(); }
    QString reportStudy() const { return m_reports.study(); }
    QString reportDate() const { return m_reports.date(); }
    bool reportIsPdf() const { return m_reports.isPdf(); }
    QVariantList reportLines() const { return m_reports.lines(); }
    // Where a PDF report is opened; tests catch it rather than launch a viewer.
    void setUrlOpener(std::function<bool(const QUrl &)> opener) { m_reports.setUrlOpener(std::move(opener)); }
    bool tagsWanted() const { return m_tags.wanted(); }
    void setTagsWanted(bool wanted);
    QString tagFilter() const { return m_tags.filter(); }
    void setTagFilter(const QString &filter);
    TagModel *tagModel() { return &m_tags; }

    // What the shown picture is drawn from: the slice's values with their
    // size, sign, and rescale, the window, the colour scale, and the PET
    // over it with where the picture lies. A shader needs no more.
    Picture picture() const;
    // The same made into a picture on the CPU, for the view that paints
    // in software, for saving, and for copying; made when it's asked for.
    QImage image() const;
    // For tests: how many times that picture has been made.
    int renders() const { return m_renders; }

    // The pieces, for another view of the same series to follow.
    const Windowing &window() const { return m_window; }
    const Planes &planes() const { return m_planes; }
    const Fusion &fusion() const { return m_fusion; }

    Q_INVOKABLE void open();
    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void load(const QVariantList &urlsOrPaths);
    void loadPaths(const QStringList &paths);

    Q_INVOKABLE void selectSeries(int index);
    Q_INVOKABLE void stepSeries(int delta);
    Q_INVOKABLE void setSliceIndex(int index);
    Q_INVOKABLE void step(int delta);

    Q_INVOKABLE void beginWindowDrag() { m_window.beginDrag(); }
    Q_INVOKABLE void dragWindow(double dx, double dy);
    Q_INVOKABLE void preset(int number);
    Q_INVOKABLE void fileWindow() { m_window.fileWindow(); }
    Q_INVOKABLE void fullRange();
    Q_INVOKABLE void toggleInvert();
    Q_INVOKABLE void undo() { m_window.undo(); }
    Q_INVOKABLE void redo() { m_window.redo(); }
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
    Q_INVOKABLE void openReport(int index) { m_reports.open(index, true); }
    Q_INVOKABLE void openStudyReport();
    Q_INVOKABLE void stepReport(int delta) { m_reports.step(delta); }
    Q_INVOKABLE void closeReport() { m_reports.close(); }

    // Side by side: joins the library another backend shows its scans
    // from, and shows the same kind of series from another study or else
    // the next series, unless it's showing one of that scan already.
    Q_INVOKABLE void share(Backend *other);
    // Four views: shows what another backend shows, cut another way, from
    // the same slices; its PET too. Null stops.
    Q_INVOKABLE void mirror(Backend *leader, int plane);
    // Takes another's window, so views of one series look alike.
    Q_INVOKABLE void matchWindow(Backend *other);
    Q_INVOKABLE void follow(Backend *other);
    Q_INVOKABLE void stop();

    // Whether the whole series can be kept in memory at once, as cutting
    // it another way, a slab, and four views need; says so when it can't.
    Q_INVOKABLE bool volumeFits();
    // A 3D view of this pane's series asks for the whole series, and room
    // beside it for the volume it packs.
    void wantVolume(QObject *view, bool wanted) { m_claim.wantVolume(view, wanted); }
    // Why there's no room for that, or nothing when there is.
    QString volumeRefusal() const;

    Q_INVOKABLE QString valueAt(double x, double y) const;
    Q_INVOKABLE QString distance(double x1, double y1, double x2, double y2) const;
    // The area of the ellipse in a box of image pixels, and what's in it.
    Q_INVOKABLE QString areaStats(double x, double y, double width, double height) const;
    Q_INVOKABLE QString angleText(double degrees) const { return Readout::angle(degrees); }
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
    bool crossing(QPointF &at) const { return m_planes.crossing(at); }
    // Turning this plane about an axis through where the planes meet, from
    // where it was when the turn began: dragging another view's reference
    // line.
    void beginTurn() { m_planes.beginTurn(); }
    void turnPlane(const double *axis, double radians);
    // The patient point where the planes meet.
    void crossingPoint(double *point) const { m_planes.crossingPoint(point); }
    // Millimetres between pixels across and down, where the picture has them.
    bool spacingAt(double x, double y, double &across, double &down) const;
    // How big the shown picture's pixels are, and what a measurement drawn
    // on it comes to, as the window shows it.
    MeasureScale measureScale() const;
    QString measureText(const Measure &m) const;
    Q_INVOKABLE void nextColorMap();
    Q_INVOKABLE void exportImage();
    // The series (or plane) as an MP4, as shown: windowed, turned, coloured.
    Q_INVOKABLE void exportVideo();
    // A copy of every image without what identifies the patient.
    Q_INVOKABLE void exportAnonymous() { m_exporter.askAnonymous(scan()); }
    Q_INVOKABLE void copyImage();
    Q_INVOKABLE void dismissProblem() { m_notices.setProblem({}); }
    // A word from the window about what it did, such as linking the panes.
    Q_INVOKABLE void announce(const QString &notice) { m_notices.flashText(notice); }

    // A PET over the CT or MR it was taken with, in colour: on or off, and
    // how strongly.
    bool fused() const { return m_fusion.on(); }
    // Such as "PET over CT  ·  50%", or nothing.
    QString fusionName() const;
    Q_INVOKABLE void toggleFusion();
    Q_INVOKABLE void nextFusionOpacity();
    // The PET's voxels, once read, and how they're drawn over.
    std::shared_ptr<const VoxelGrid> fusionGrid() const { return m_fusion.grid(); }
    Overlay3D fusionLook() const { return m_fusion.look(); }

    // The series' slices by index, null where one isn't in memory, and its
    // volume, for the 3D view.
    const QVector<Frame> &frames() const;
    const Volume &volume() const { return m_planes.volume(); }
    bool allDecoded() const;
    bool crossingShown() const { return m_planes.crossingShown(); }
    // Saves or copies a picture made elsewhere, such as the 3D view's.
    void exportPicture(const QImage &picture);
    void copyPicture(const QImage &picture) { m_exporter.copy(picture); }

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
    void sliceChanged();
    void decodedChanged();
    // Something the picture is made from changed: the slice, the window,
    // or the PET over it.
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

private:
    void join(const std::shared_ptr<Library> &library);
    // Catches up with a scan the library has opened since this pane last
    // looked: nothing of the one before is shown any more.
    void sync();
    void scanOpened(QObject *asker, int series, int slice);
    void frameArrived(int series, int index);
    // Tells the cache what this pane needs kept.
    void updateInterest();
    void sliceMoved();
    void cutChanged();
    void showSlice();
    // The picture's inputs changed; it's made again when next asked for.
    void repaint();
    void updateInfo();
    void updateTags();
    void adoptFusion();
    // How to say a series is too big to keep whole, at so many bytes.
    Words tooBig(qint64 bytes) const;
    // What a saved file is called to start with: the series, and for a
    // video the plane it's cut in.
    QString exportName(bool withPlane) const;
    const Series *currentSeries() const;
    const Frame *currentFrame() const;
    // Shows a picture built from the slices, not one of them.
    bool built() const { return m_planes.built(); }
    void build();

    FilePicker *m_picker;
    std::shared_ptr<Library> m_library;
    int m_serial = -1;             // The library's scan this pane shows.
    Notices m_notices;
    Claim m_claim;
    Windowing m_window;
    Planes m_planes;
    Fusion m_fusion;
    ReportReader m_reports;
    TagModel m_tags;
    Exporter m_exporter;

    int m_seriesIndex = -1;
    Frame m_built;
    QString m_buildError;
    // The last slice shown, which stays up until the next one arrives,
    // and where it lies.
    Frame m_shown;
    ImagePlane m_shownPlane;
    mutable QImage m_image;
    mutable bool m_stale = true;
    mutable int m_renders = 0;

    QTimer m_play;
    QVariantMap m_info;
    Turn m_turn;
    bool m_marksShown = true;

    QPointer<Backend> m_leader;
    QList<QMetaObject::Connection> m_leaderConnections;
};
