#include "backend.h"

#include <QRegularExpression>
#include <algorithm>
#include <cmath>

// What the user does to a pane's picture: cutting it another way, turning
// it, setting its window, putting a PET over it, measuring, and saving.
// Each hands the work to the piece that owns it, and says what changed.

void Backend::nextPlane() {
    if (!currentSeries())
        return;
    if (!canReformat()) {
        m_notices.flash([] { return tr("This series can't be cut another way"); });
        return;
    }
    const int before = plane();
    setPlane(before == 3 ? 0 : (before + 1) % 3);
    if (plane() != before)
        m_notices.flash([this] { return planeName(); });
}

void Backend::setPlane(int plane) {
    if (plane < 0 || plane > 2 || plane == m_planes.plane() || (plane != 0 && (!canReformat() || !volumeFits())))
        return;
    stop();
    if (!m_planes.setPlane(plane))
        return;
    m_turn = Turn();
    cutChanged();
    updateTags();
}

void Backend::tilt(int axis, double degrees) {
    if (!currentSeries())
        return;
    if (!canReformat()) {
        m_notices.flash([] { return tr("This series can't be cut another way"); });
        return;
    }
    if (!imagePlane().valid || !volumeFits())
        return;
    if (m_planes.tilt(axis, degrees)) {
        stop();
        cutChanged();
    }
    m_notices.flash([this] { return planeName(); });
}

void Backend::turnPlane(const double *axis, double radians) {
    if (!canReformat() || !volumeFits() || !m_planes.turn(axis, radians))
        return;
    stop();
    cutChanged();
}

void Backend::nextSlab() {
    if (!currentSeries())
        return;
    if (!canReformat()) {
        m_notices.flash([] { return tr("This series can't be made into slabs"); });
        return;
    }
    if (m_planes.slab() == 0 && !volumeFits())
        return;
    m_planes.nextSlab();
    updateInterest();
    build();
    updateInfo();
    emit sliceChanged();
    emit orientationChanged();
    showSlice();
    m_notices.flash([this] { return m_planes.slab() == 0 ? tr("Thin slices") : slabName(); });
}

void Backend::nextSlabThickness() {
    if (m_planes.slab() == 0) {
        nextSlab();
        return;
    }
    m_planes.nextThickness();
    build();
    updateInfo();
    emit orientationChanged();
    showSlice();
    m_notices.flash([this] { return slabName(); });
}

QVariantList Backend::pointAt(double x, double y) const {
    const ImagePlane p = imagePlane();
    if (!p.valid || p.frame.isEmpty() || x < -0.5 || y < -0.5 || x > p.width - 0.5 || y > p.height - 0.5)
        return {};
    double point[3];
    p.point(x, y, point);
    return {point[0], point[1], point[2], p.frame};
}

bool Backend::goThrough(const QVariantList &point) {
    const Series *s = currentSeries();
    if (!s || point.size() != 4 || s->frame.isEmpty() || point[3].toString() != s->frame)
        return false;
    const double p[3] = {point[0].toDouble(), point[1].toDouble(), point[2].toDouble()};
    const int before = sliceIndex();
    if (!m_planes.goThrough(p))
        return false;
    if (sliceIndex() != before || m_shown.isNull()) {
        sliceMoved();
    } else if (canReformat()) {
        // The same picture, with the point the planes meet at moved.
        updateInterest();
        emit sliceChanged();
        updateTags();
    }
    return true;
}

void Backend::rotate(int quarterTurns) {
    if (!currentSeries())
        return;
    m_turn.rotate(quarterTurns);
    emit orientationChanged();
}

void Backend::flipHorizontal() {
    if (!currentSeries())
        return;
    m_turn.flipHorizontal();
    emit orientationChanged();
}

void Backend::flipVertical() {
    if (!currentSeries())
        return;
    m_turn.flipVertical();
    emit orientationChanged();
}

void Backend::resetOrientation() {
    if (m_turn.isNone())
        return;
    m_turn = Turn();
    emit orientationChanged();
}

void Backend::toggleMarks() {
    m_marksShown = !m_marksShown;
    emit marksChanged();
    if (!hasMarks())
        m_notices.flash([] { return tr("This image has no marks"); });
    else if (m_marksShown)
        m_notices.flash([] { return tr("Showing marks"); });
    else
        m_notices.flash([] { return tr("Hiding marks"); });
}

QList<Mark> Backend::marks() const {
    // Marks belong to a slice, not to a plane cut across many.
    const Series *s = currentSeries();
    return s && plane() == 0 ? m_library->marksOn(s->slices.value(sliceIndex())) : QList<Mark>();
}

bool Backend::hasMarks() const {
    return !overlay().isNull() || !marks().isEmpty();
}

void Backend::openStudyReport() {
    if (m_reports.count() == 0) {
        m_notices.flash([] { return tr("No reports came with these scans"); });
        return;
    }
    const Series *s = currentSeries();
    m_reports.openFor(s ? s->studyUid : QString());
}

void Backend::nextColorMap() {
    if (!hasImages())
        return;
    if (color()) {
        m_notices.flash([] { return tr("Colour scales are for grey images"); });
        return;
    }
    m_window.nextColorMap();
    m_notices.flash([this] { return colorMap() == 0 ? tr("Grey") : colorMapName(); });
}

QString Backend::windowName() const {
    const char *name = m_window.presetName();
    return name ? tr(name) : QString();
}

void Backend::dragWindow(double dx, double dy) {
    if (!color())
        m_window.drag(dx, dy);
}

void Backend::preset(int n) {
    if (!color())
        m_window.preset(n);
}

void Backend::fullRange() {
    const Frame *frame = currentFrame();
    if (!frame || frame->isColor() || !frame->error.isEmpty())
        return;
    double low = 0, high = 0;
    frame->range(low, high);
    m_window.set((low + high) / 2, std::max(1.0, high - low + 1));
}

void Backend::toggleInvert() {
    if (hasImages())
        m_window.toggleInvert();
}

void Backend::togglePlay() {
    if (m_play.isActive()) {
        stop();
        return;
    }
    const Series *s = currentSeries();
    if (!s || s->slices.size() < 2)
        return;
    const double fps = s->frameRate > 0 ? std::clamp(s->frameRate, 1.0, 120.0) : 12.0;
    m_play.start(qRound(1000 / fps));
    emit playingChanged();
}

void Backend::stop() {
    if (!m_play.isActive())
        return;
    m_play.stop();
    emit playingChanged();
}

QString Backend::valueAt(double x, double y) const {
    const Series *s = currentSeries();
    const Frame *frame = currentFrame();
    if (!s || !frame || frame->isNull() || !frame->error.isEmpty())
        return {};
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    if (ix < 0 || iy < 0 || ix >= frame->width || iy >= frame->height)
        return {};
    QString text = Readout::value(*s, *frame, ix, iy);
    // The PET's value there too, when it's over the picture.
    const auto pet = fusionGrid();
    const ImagePlane plane = imagePlane();
    if (pet && plane.valid && !frame->isColor()) {
        double point[3];
        plane.point(x - 0.5, y - 0.5, point);
        text += Readout::petValue(*pet, point, scan().series.value(m_fusion.series()).suv > 0);
    }
    return text;
}

MeasureScale Backend::measureScale() const {
    return currentSeries() ? m_planes.scale() : MeasureScale();
}

bool Backend::spacingAt(double x, double y, double &across, double &down) const {
    return currentSeries() && measureScale().at(x, y, across, down);
}

QString Backend::measureText(const Measure &m) const {
    const Series *s = currentSeries();
    return s ? Readout::measure(*s, m, measureScale(), currentFrame()) : QString();
}

QString Backend::distance(double x1, double y1, double x2, double y2) const {
    Measure m;
    m.kind = 1;
    m.a = QPointF(x1, y1);
    m.b = QPointF(x2, y2);
    return measureText(m);
}

QString Backend::areaStats(double x, double y, double width, double height) const {
    Measure m;
    m.kind = 2;
    m.a = QPointF(x, y);
    m.b = QPointF(x + width, y + height);
    return measureText(m);
}

QString Backend::exportName(bool withPlane) const {
    const Series *s = currentSeries();
    QString name = !s ? QString() : s->description.isEmpty() ? s->modality : s->description;
    if (s && withPlane && plane() != 0)
        name += "-" + QString::fromLatin1(::planeName(imagePlane()));
    name.replace(QRegularExpression("[^\\w\\-]+"), "-");
    return name.isEmpty() ? QStringLiteral("scan") : name;
}

void Backend::exportImage() {
    // As shown: windowed, at its true shape, and turned.
    if (currentSeries() && !m_shown.isNull())
        m_exporter.askPicture(QStringLiteral("%1-%2.png").arg(exportName(false)).arg(sliceIndex() + 1),
                              [shown = picture(), aspect = pixelAspect(), turn = m_turn] {
                                  return asShown(shown.render(), aspect, turn);
                              });
}

void Backend::exportPicture(const QImage &picture) {
    if (currentSeries() && !picture.isNull())
        m_exporter.askPicture(exportName(false) + "-3D.png", [picture] { return picture; });
}

void Backend::copyImage() {
    if (!m_shown.isNull())
        m_exporter.copy(asShown(image(), pixelAspect(), m_turn));
}

void Backend::exportVideo() {
    const Series *s = currentSeries();
    if (!s)
        return;
    if (sliceCount() < 2) {
        m_notices.flash([] { return tr("A video needs more than one image"); });
        return;
    }
    // A plane or a slab is made from every slice, and a video file's
    // frames come all at once; slices as scanned that aren't in memory
    // are read as the video is saved.
    if ((built() || isVideo(s->transferSyntax)) && !allDecoded()) {
        m_notices.flash([] { return tr("The series is still being read"); });
        return;
    }
    Exporter::Video v;
    v.recipe = m_planes.recipe(frames());
    v.slices = frames();
    v.center = m_window.center();
    v.width = m_window.width();
    v.inverted = m_window.inverted();
    v.colorMap = m_window.colorMap();
    v.pet = fusionGrid();
    v.petLook = fusionLook();
    v.aspect = pixelAspect();
    v.turn = m_turn;
    v.fps = s->frameRate > 0 && plane() == 0 ? std::clamp(s->frameRate, 1.0, 60.0) : 12.0;
    m_exporter.askVideo(exportName(true) + ".mp4", v);
}

QString Backend::fusionName() const {
    if (!fused())
        return {};
    const Series *s = currentSeries();
    const QString under = s ? s->modality : QString();
    if (!fusionGrid())
        return tr("PET over %1  ·  reading the PET…").arg(under);
    return tr("PET over %1  ·  %2%").arg(under, Readout::number(m_fusion.opacity() * 100, 0));
}

void Backend::toggleFusion() {
    if (fused()) {
        m_fusion.stop();
        m_notices.flash([] { return tr("PET off"); });
        return;
    }
    const Series *s = currentSeries();
    if (!s)
        return;
    const int partner = ::fusionPartner(scan().series, m_seriesIndex);
    if (partner < 0) {
        if (s->modality == "PT")
            m_notices.flash([] { return tr("There's no CT or MR taken with this PET to put it over"); });
        else
            m_notices.flash([] { return tr("There's no PET taken with this series to put over it"); });
        return;
    }
    // A PET goes over its CT, so the CT is shown.
    const int pet = s->modality == "PT" ? m_seriesIndex : partner;
    if (pet != partner)
        selectSeries(partner);
    m_fusion.start(pet);
    if (fused())
        m_notices.flash([this] { return fusionName(); });
}

void Backend::nextFusionOpacity() {
    if (!fused()) {
        toggleFusion();
        return;
    }
    m_fusion.nextOpacity();
    m_notices.flash([this] { return fusionName(); });
}
