#include <QDir>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

#include "anonymize.h"
#include "backend.h"
#include "dicom.h"
#include "library.h"
#include "measurestore.h"
#include "pixels.h"
#include "qmlmodule.h"
#include "sliceview.h"
#include "study.h"
#include "textsize.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"

#include "support.h"

// The anonymous copy as the window offers it: what a copy would do, shown
// before anything is written; its one choice; how it went; and the regions
// boxed on a picture to black out of its series. All through the real
// Main.qml and the real backends, with the keys, the mouse, and a chooser
// that answers at once.

namespace {

// The window's item with this objectName, wherever a Repeater or a Loader
// put it.
QQuickItem *item(QQuickItem *from, const QString &name) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == name)
            return child;
        if (auto *found = item(child, name))
            return found;
    }
    return nullptr;
}

// A chooser that counts how often it was asked.
class CountingPicker : public FakePicker {
public:
    int asks = 0;
    void choose(QObject *asker, Purpose purpose, const QString &name) override {
        ++asks;
        FakePicker::choose(asker, purpose, name);
    }
};

// What image() stores in a pixel, by its place counted along the rows:
// never 0, the black a region is blacked out with.
int valueAt(int pixel, int instance) { return 100 + (pixel * 7 + instance * 13) % 150; }

// One image of Jane Doe's, with what a hospital's files say of a patient
// around it: grey, of 8, 16, or 32 bits.
QByteArray image(const QByteArray &modality, const QByteArray &series, int instance, const QByteArray &description,
                 int columns, int rows, int bits, const std::function<void(Writer &)> &more = {}) {
    Writer w;
    if (more)
        more(w);
    w.add(Tag::SopClass, "UI", modality == "US" ? "1.2.840.10008.5.1.4.1.1.6.1" : "1.2.840.10008.5.1.4.1.1.2")
        .add(Tag::SopInstance, "UI", series + "." + QByteArray::number(instance))
        .add(Tag::StudyDate, "DA", "20250101")
        .add(Tag::Modality, "CS", modality)
        .add(Tag::Institution, "LO", "Saint Elsewhere")
        .add(Tag::StudyDescription, "LO", "Head and heart")
        .add(Tag::SeriesDescription, "LO", description)
        .add(Tag::PatientName, "PN", "Doe^Jane")
        .add(Tag::PatientId, "LO", "MRN42")
        .add(Tag::PatientBirthDate, "DA", "19700101")
        .add(Tag::StudyInstance, "UI", "1.2.7")
        .add(Tag::SeriesInstance, "UI", series)
        .add(Tag::SeriesNumber, "IS", series.mid(series.lastIndexOf('.') + 1))
        .add(Tag::InstanceNumber, "IS", QByteArray::number(instance))
        .us(Tag::SamplesPerPixel, 1)
        .add(Tag::Photometric, "CS", "MONOCHROME2")
        .us(Tag::Rows, rows)
        .us(Tag::Columns, columns)
        .us(Tag::BitsAllocated, bits)
        .us(Tag::BitsStored, bits)
        .us(Tag::HighBit, bits - 1)
        .us(Tag::PixelRepresentation, 0);
    QByteArray pixels;
    for (int i = 0; i < rows * columns; ++i) {
        pixels.append(char(valueAt(i, instance)));
        for (int byte = 1; byte < bits / 8; ++byte)
            pixels.append(char(0));
    }
    return w.pixels(pixels, bits == 8 ? "OB" : "OW").file(ExplicitLE);
}

// A structured report of the study's, and a presentation state for one of
// its series, as a CD carries them beside the images.
bool writeReportAndMarks(const QString &folder) {
    Writer text;
    text.add(Tag::ValueType, "CS", "TEXT").add(Tag::TextValue, "UT", "Unremarkable.");
    Writer report;
    report.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.88.11")
        .add(Tag::Modality, "CS", "SR")
        .add(Tag::StudyInstance, "UI", "1.2.7")
        .sequence(Tag::ContentSequence, {text});
    Writer series;
    series.add(Tag::SeriesInstance, "UI", "1.2.7.2");
    Writer state;
    state.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.11.1")
        .add(Tag::Modality, "CS", "PR")
        .sequence(Tag::ReferencedSeries, {series});
    return write(folder + "/SR1", report.file(ExplicitLE)) && write(folder + "/PR1", state.file(ExplicitLE));
}

// The names in the rows the panel is given.
QStringList names(const QVariantList &rows) {
    QStringList out;
    for (const QVariant &row : rows)
        out << row.toMap().value("name").toString();
    return out;
}

// The mean of a patch of the window, 0 black to 255 white.
double brightness(const QImage &picture, QPoint centre) {
    double sum = 0;
    int count = 0;
    for (int y = centre.y() - 6; y <= centre.y() + 6; ++y)
        for (int x = centre.x() - 6; x <= centre.x() + 6; ++x)
            if (picture.valid(x, y)) {
                sum += qGray(picture.pixel(x, y));
                ++count;
            }
    return count ? sum / count : 0;
}

// The window as the app makes it: one library for four panes, on a
// language, a theme, and a text size of the test's own, with a chooser
// that answers at once. Any QML warning is kept, and fails the test.
struct Window {
    QTemporaryDir dir;
    Translator translator{dir.path() + "/language"};
    Theme theme{dir.path()};
    TextSize textSize{dir.path() + "/shell.toml", dir.path()};
    CountingPicker picker;
    std::shared_ptr<Library> library = std::make_shared<Library>();
    Backend backend{&picker, library};
    Backend partner{&picker, library};
    Backend coronal{&picker, library};
    Backend sagittal{&picker, library};
    QStringList warnings;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;

    Window() { QCoreApplication::installTranslator(&translator); }
    ~Window() { QCoreApplication::removeTranslator(&translator); }

    bool show() {
        keepQmlModule();
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.addImageProvider("thumbnails", new ThumbnailProvider(library.get()));
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("textSize", &textSize);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("partner", &partner);
        engine.rootContext()->setContextProperty("coronal", &coronal);
        engine.rootContext()->setContextProperty("sagittal", &sagittal);
        engine.load(QUrl("qrc:/Main.qml"));
        if (engine.rootObjects().isEmpty())
            return false;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!window || !QTest::qWaitForWindowExposed(window))
            return false;
        window->requestActivate();
        return QTest::qWaitForWindowActive(window);
    }

    bool open(const QString &folder) {
        backend.loadPaths({folder});
        return backend.waitForSlice(10000);
    }

    // The place in the open scan of the series described so.
    int series(const QString &description) const {
        for (int i = 0; i < backend.scan().series.size(); ++i)
            if (backend.scan().series[i].description == description)
                return i;
        return -1;
    }

    SliceView *view(const QString &name = "view") const {
        return qobject_cast<SliceView *>(item(window->contentItem(), name));
    }

    AnonymousCopy *copy() { return backend.anonymous(); }
    QQuickItem *part(const QString &name) const { return item(window->contentItem(), name); }
    // What a label of the window says, when it's there to be read.
    QString says(const QString &name) const {
        QQuickItem *label = part(name);
        return !label ? QStringLiteral("(no such item)") : !label->isVisible() ? QStringLiteral("(hidden)")
                                                                               : label->property("text").toString();
    }
    // Ctrl+Shift+S, and the plan it asks for, once that's worked out.
    bool plan() {
        QTest::keyClick(window, 'S', Qt::ControlModifier | Qt::ShiftModifier);
        return planned();
    }
    bool planned() {
        return QTest::qWaitFor([this] { return copy()->stage() == AnonymousCopy::Planned; }, 10000);
    }
    bool saved() {
        return QTest::qWaitFor([this] { return copy()->stage() == AnonymousCopy::Saved; }, 10000);
    }

    // Where a point of a view's picture, in image pixels, is on the window.
    QPoint at(SliceView *view, QPointF image) const {
        return view->mapToScene(view->toItem(image.x(), image.y())).toPoint();
    }

    // With DICOLENS_TEST_SHOTS naming a folder, a picture of the window is
    // kept there: bin/looks reaches only what keys reach, and these are the
    // states the mouse and a chooser's answer lead to.
    void keep(const QString &name) {
        const QString folder = qEnvironmentVariable("DICOLENS_TEST_SHOTS");
        if (folder.isEmpty())
            return;
        // Once what moves has settled: a switch slides, a notice fades in.
        QTest::qWait(400);
        window->grabWindow().save(QStringLiteral("%1/%2.png").arg(folder, name));
    }

    // A drag with the left button between two points of a view's picture.
    void drag(SliceView *view, QPointF from, QPointF to) {
        const QPoint a = at(view, from), b = at(view, to);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, a);
        QTest::mouseMove(window, (a + b) / 2);
        QTest::mouseMove(window, b);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, b);
    }
};

}

class AnonymousTests : public QObject {
    Q_OBJECT
    QTemporaryDir m_data;

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        // Measurements are kept in a folder of the tests' own, never the user's.
        QVERIFY(m_data.isValid());
        MeasureStore::instance().setFolder(m_data.filePath("measurements"));
    }

    void showsWhatACopyWouldDoBeforeAnythingIsWritten() {
        // A CT and an ultrasound of two images each, with a report and a
        // presentation state beside them, and a vendor's private attribute.
        QTemporaryDir scans, out;
        for (int i = 1; i <= 2; ++i) {
            QVERIFY(write(scans.filePath(QString("CT%1").arg(i)),
                          image("CT", "1.2.7.2", i, "Axial", 16, 12, 16, [](Writer &w) { w.add(0x00090010, "LO", "VENDOR"); })));
            QVERIFY(write(scans.filePath(QString("US%1").arg(i)), image("US", "1.2.7.3", i, "Apical", 16, 12, 8)));
        }
        QVERIFY(writeReportAndMarks(scans.path()));
        Window w;
        QVERIFY(w.show());
        w.window->resize(900, 520);
        QVERIFY(w.open(scans.path()));
        QCOMPARE(w.backend.scan().series.size(), 2);
        AnonymousCopy *copy = w.copy();
        QQuickItem *panel = w.part("anonymousPanel");
        QVERIFY(panel && !panel->isVisible());

        // What the shortcut calls comes back with the plan still to come:
        // every file is read on a worker, and the window says it's working
        // it out. Nothing can be saved meanwhile, by Enter or the button.
        w.backend.exportAnonymous();
        QCOMPARE(copy->stage(), AnonymousCopy::Planning);
        QVERIFY(copy->shown() && panel->isVisible());
        QCOMPARE(w.says("anonymousWorking"), QString("Working out what a copy would do…"));
        QVERIFY(!copy->canSave() && !w.part("anonymousMain")->isEnabled());
        copy->choose();
        QCOMPARE(w.picker.asks, 0);
        QVERIFY(w.planned());
        copy->close();
        QVERIFY(!panel->isVisible());

        // Ctrl+Shift+S shows it, with the plan once it's there: how many
        // files, what becomes of each attribute and in how many files,
        // what's left out, which pictures may show the patient, that
        // nothing is blacked out, and the edition it goes by.
        QVERIFY(w.plan());
        QVERIFY(panel->isVisible());
        QCOMPARE(copy->files(), 4);
        QCOMPARE(w.says("anonymousHeadline"), QString("4 files would be copied"));
        QVERIFY(names(copy->removed()).contains("Study Description") && names(copy->removed()).contains("Series Description"));
        QCOMPARE(copy->removed().last().toMap().value("name").toString(), QString("Private attributes"));
        QCOMPARE(copy->removed().last().toMap().value("files").toInt(), 2);
        QVERIFY(names(copy->emptied()).contains("Patient's Birth Date"));
        QVERIFY(names(copy->replaced()).contains("Patient's Name") && names(copy->replaced()).contains("Series Instance UID"));
        QCOMPARE(names(copy->kept()), (QStringList{"Study Date"}));
        QCOMPARE(copy->kept().first().toMap().value("files").toInt(), 4);
        QCOMPARE(copy->reports(), 1);
        QCOMPARE(copy->annotations(), 1);
        QCOMPARE(copy->pictures().size(), 1);
        const QVariantMap picture = copy->pictures().first().toMap();
        QCOMPARE(picture.value("name").toString(), QString("Apical"));
        QCOMPARE(picture.value("modality").toString(), QString("US"));
        QCOMPARE(picture.value("flagged").toInt(), 2);
        QCOMPARE(picture.value("regions").toInt(), 0);
        QCOMPARE(copy->regions(), 0);
        QCOMPARE(w.says("anonymousRegions"), QString("No region is marked to black out."));
        QCOMPARE(copy->refusedFiles(), 0);
        QCOMPARE(copy->uncopiedFiles(), 0);
        QVERIFY(!w.part("anonymousAttention")->isVisible());
        QVERIFY(!copy->edition().isEmpty());
        QVERIFY2(w.says("anonymousMethod").contains("DICOM PS3.15 " + copy->edition() + ", the Basic Application Level Confidentiality Profile"),
                 qPrintable(w.says("anonymousMethod")));
        QVERIFY(copy->canSave() && w.part("anonymousMain")->isEnabled());

        // The keys are the panel's while it's up: they scroll it, and leave
        // the scan it covers as it is.
        QQuickItem *flick = w.part("anonymousFlick");
        QVERIFY(flick);
        // Longer than the window, once it's laid out.
        QTRY_VERIFY(flick->property("contentHeight").toReal() > flick->height());
        const int slice = w.backend.sliceIndex();
        QTest::keyClick(w.window, Qt::Key_Down);
        QVERIFY(flick->property("contentY").toReal() > 0);
        QTest::keyClick(w.window, Qt::Key_PageDown);
        QTest::keyClick(w.window, Qt::Key_PageUp);
        QTest::keyClick(w.window, Qt::Key_Up);
        QCOMPARE(flick->property("contentY").toReal(), 0.0);
        QTest::keyClick(w.window, Qt::Key_End);
        QTest::keyClick(w.window, ']');
        QTest::keyClick(w.window, 'M');
        QTest::keyClick(w.window, 'P');
        QCOMPARE(w.backend.sliceIndex(), slice);
        QCOMPARE(w.backend.rotation(), 0);
        QCOMPARE(w.view()->tool(), 0);
        QCOMPARE(w.backend.reportIndex(), -1);

        // Its one choice: Space keeps the descriptions, and the plan is
        // worked out again, as what's kept and what the copy claims change.
        QQuickItem *keep = w.part("anonymousKeep");
        QVERIFY(keep && !keep->property("checked").toBool());
        QTest::keyClick(w.window, Qt::Key_Space);
        QVERIFY(copy->keepDescriptions());
        QVERIFY(w.planned());
        QVERIFY(keep->property("checked").toBool());
        QCOMPARE(names(copy->kept()), (QStringList{"Study Date", "Study Description", "Series Description"}));
        QVERIFY(!names(copy->removed()).contains("Series Description"));
        QCOMPARE(copy->files(), 4);
        QVERIFY2(w.says("anonymousMethod").contains("keeps the study, series, and protocol descriptions as they are"),
                 qPrintable(w.says("anonymousMethod")));
        // A click on the switch is the same choice.
        QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, keep->mapToScene(QPointF(keep->width() / 2, keep->height() / 2)).toPoint());
        QVERIFY(!copy->keepDescriptions());
        QVERIFY(w.planned());
        QVERIFY(!keep->property("checked").toBool());
        QVERIFY(names(copy->removed()).contains("Series Description"));

        // Esc puts it away, and nothing was asked or written anywhere.
        QTest::keyClick(w.window, Qt::Key_Escape);
        QVERIFY(!copy->shown() && !panel->isVisible());
        QCOMPARE(copy->stage(), AnonymousCopy::Closed);
        QCOMPARE(w.picker.asks, 0);
        QCOMPARE(listFiles({scans.path()}, true).size(), 6);
        QVERIFY(QDir(out.path()).isEmpty());
        // The keys are the scans' again.
        QTest::keyClick(w.window, Qt::Key_End);
        QCOMPARE(w.backend.sliceIndex(), 1);

        // The choice starts off each time. A chooser that's cancelled
        // answers nobody: the plan stays, and nothing is written.
        QVERIFY(w.plan());
        QTest::keyClick(w.window, Qt::Key_Space);
        QVERIFY(w.planned());
        QTest::keyClick(w.window, Qt::Key_Escape);
        QVERIFY(w.plan());
        QVERIFY(!copy->keepDescriptions());
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(w.picker.asks, 1);
        QCOMPARE(w.picker.asked, FilePicker::Purpose::SaveFolder);
        QCOMPARE(copy->stage(), AnonymousCopy::Planned);
        // The button asks as Enter does.
        QQuickItem *main = w.part("anonymousMain");
        QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, main->mapToScene(QPointF(main->width() / 2, main->height() / 2)).toPoint());
        QCOMPARE(w.picker.asks, 2);
        QTest::keyClick(w.window, Qt::Key_Escape);
        QVERIFY(QDir(out.path()).isEmpty());

        // Another scan opened over a plan leaves nothing to save.
        QVERIFY(w.plan());
        QTemporaryDir other;
        QVERIFY(write(other.filePath("CT1"), image("CT", "1.2.8.1", 1, "Another", 16, 12, 16)));
        QVERIFY(w.open(other.path()));
        QVERIFY(!copy->shown());
        QCOMPARE(copy->stage(), AnonymousCopy::Closed);

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    void savesWithTheChoiceMade() {
        QTemporaryDir scans, out;
        for (int i = 1; i <= 2; ++i) {
            QVERIFY(write(scans.filePath(QString("CT%1").arg(i)), image("CT", "1.2.7.2", i, "Axial", 16, 12, 16)));
            QVERIFY(write(scans.filePath(QString("US%1").arg(i)), image("US", "1.2.7.3", i, "Apical", 16, 12, 8)));
        }
        QVERIFY(writeReportAndMarks(scans.path()));
        Window w;
        QVERIFY(w.show());
        QVERIFY(w.open(scans.path()));
        AnonymousCopy *copy = w.copy();
        QQuickItem *panel = w.part("anonymousPanel");
        w.picker.answer = out.path();

        // What each copy says of the study and its series, by its number.
        auto descriptions = [](const QString &folder) {
            QStringList said;
            for (const QString &path : listFiles({folder}, true)) {
                const DicomFile file = DicomFile::read(path);
                said << QStringLiteral("%1/%2/%3").arg(file.first(Tag::Modality), file.first(Tag::StudyDescription),
                                                        file.first(Tag::SeriesDescription));
                if (file.string(Tag::PatientName).contains("Doe", Qt::CaseInsensitive))
                    said << "names the patient";
            }
            said.sort();
            return said;
        };

        // Enter asks where, and saves what was looked at: as the profile
        // has it, without the descriptions.
        QVERIFY(w.plan());
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(w.picker.asks, 1);
        QVERIFY(w.saved());
        QCOMPARE(copy->files(), 4);
        QCOMPARE(copy->folder(), out.filePath("Anonymous scans"));
        QCOMPARE(descriptions(copy->folder()), (QStringList{"CT//", "CT//", "US//", "US//"}));
        // How many and where, in a notice and on the panel, which stays
        // up with what the copy leaves to whoever shares it: the pictures
        // that may show the patient, and what was left out.
        QCOMPARE(w.backend.notice(), QString("Saved 4 files in Anonymous scans"));
        QVERIFY(copy->shown() && panel->isVisible());
        QCOMPARE(w.says("anonymousHeadline"), QString("Saved 4 files in ⁨Anonymous scans⁩"));
        QVERIFY(w.part("anonymousAttention")->isVisible());
        QCOMPARE(copy->unhidden(), 2);
        QCOMPARE(copy->reports(), 1);
        QCOMPARE(copy->annotations(), 1);
        QCOMPARE(copy->refusedFiles(), 0);
        QVERIFY(!w.part("anonymousRefused")->isVisible());
        w.keep("anonymous-saved");
        // Enter is done with it.
        QTest::keyClick(w.window, Qt::Key_Return);
        QVERIFY(!copy->shown() && !panel->isVisible());
        QCOMPARE(copy->stage(), AnonymousCopy::Closed);
        QCOMPARE(w.picker.asks, 1);

        // With the choice made, the descriptions are in the copy, in a
        // folder of its own beside the first.
        QVERIFY(w.plan());
        QTest::keyClick(w.window, Qt::Key_Space);
        QVERIFY(w.planned());
        QTest::keyClick(w.window, Qt::Key_Return);
        QVERIFY(w.saved());
        QCOMPARE(copy->folder(), out.filePath("Anonymous scans 2"));
        QCOMPARE(descriptions(copy->folder()),
                 (QStringList{"CT/Head and heart/Axial", "CT/Head and heart/Axial", "US/Head and heart/Apical", "US/Head and heart/Apical"}));
        QTest::keyClick(w.window, Qt::Key_Escape);
        QVERIFY(!copy->shown());

        // Put away while it's being written, the copy goes on, and comes
        // back when there's something to say that a notice doesn't. Both
        // calls are made in one go here, as the keys would be before the
        // worker is done.
        QVERIFY(w.plan());
        copy->save(out.path());
        QCOMPARE(copy->stage(), AnonymousCopy::Saving);
        QCOMPARE(w.says("anonymousWorking"), QString("Saving an anonymous copy…"));
        copy->close();
        QVERIFY(!copy->shown() && !panel->isVisible());
        QCOMPARE(copy->stage(), AnonymousCopy::Saving);
        QVERIFY(w.saved());
        QVERIFY(copy->shown() && panel->isVisible());
        QCOMPARE(copy->folder(), out.filePath("Anonymous scans 3"));
        QTest::keyClick(w.window, Qt::Key_Escape);

        // A copy with nothing to look into says so; put away meanwhile, it
        // leaves only its notice.
        QTemporaryDir plain;
        for (int i = 1; i <= 2; ++i)
            QVERIFY(write(plain.filePath(QString("CT%1").arg(i)), image("CT", "1.2.9.2", i, "Axial", 16, 12, 16)));
        QVERIFY(w.open(plain.path()));
        QVERIFY(w.plan());
        QTest::keyClick(w.window, Qt::Key_Return);
        QVERIFY(w.saved());
        QVERIFY(copy->shown());
        QVERIFY(!w.part("anonymousAttention")->isVisible());
        QTest::keyClick(w.window, Qt::Key_Return);
        QVERIFY(w.plan());
        copy->save(out.path());
        copy->close();
        QTRY_COMPARE(copy->stage(), AnonymousCopy::Closed);
        QVERIFY(!copy->shown());
        QCOMPARE(w.backend.notice(), QString("Saved 2 files in Anonymous scans 5"));
        QCOMPARE(listFiles({out.filePath("Anonymous scans 5")}, true).size(), 2);

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    void keepsARefusedCopyReported() {
        // An ultrasound whose description names the patient, which only
        // matters once the descriptions are kept.
        QTemporaryDir scans, out;
        for (int i = 1; i <= 2; ++i) {
            QVERIFY(write(scans.filePath(QString("CT%1").arg(i)), image("CT", "1.2.7.2", i, "Axial", 16, 12, 16)));
            QVERIFY(write(scans.filePath(QString("US%1").arg(i)), image("US", "1.2.7.3", i, "Mrs DOE echo", 16, 12, 8)));
        }
        Window w;
        QVERIFY(w.show());
        QVERIFY(w.open(scans.path()));
        AnonymousCopy *copy = w.copy();
        QQuickItem *panel = w.part("anonymousPanel");
        w.picker.answer = out.path();

        QVERIFY(w.plan());
        QCOMPARE(copy->files(), 4);
        QCOMPARE(copy->refusedFiles(), 0);

        // The plan says what the check would refuse before a folder is
        // chosen: how many copies, and what would be found where, once.
        QTest::keyClick(w.window, Qt::Key_Space);
        QVERIFY(w.planned());
        QCOMPARE(copy->files(), 2);
        QCOMPARE(copy->refusedFiles(), 2);
        QCOMPARE(copy->uncopiedFiles(), 0);
        QCOMPARE(copy->refused().size(), 1);
        const QVariantMap found = copy->refused().first().toMap();
        QCOMPARE(found.value("name").toString(), QString("Series Description"));
        QCOMPARE(found.value("tag").toString(), QString("(0008,103E)"));
        QCOMPARE(found.value("what").toString(), QString("a person's name"));
        QCOMPARE(found.value("found").toString(), QString("Doe"));
        QCOMPARE(found.value("files").toInt(), 2);
        QCOMPARE(found.value("series").toString(), QString("Mrs DOE echo"));
        QVERIFY(w.part("anonymousAttention")->isVisible());
        QCOMPARE(w.says("anonymousRefused"), QString("2 copies would still name the patient, so they wouldn't be kept:"));
        QCOMPARE(w.says("anonymousFound"), QString("a person's name: ⁨Doe⁩"));
        QCOMPARE(w.says("anonymousCaution"), QString("(hidden)"));
        w.keep("anonymous-would-refuse");

        // Saved so, the copies that name the patient aren't kept, and the
        // panel says how many and why, and that the folder isn't to be
        // taken for an anonymous copy.
        QTest::keyClick(w.window, Qt::Key_Return);
        QVERIFY(w.saved());
        QCOMPARE(copy->files(), 2);
        QCOMPARE(copy->refusedFiles(), 2);
        const QStringList copies = listFiles({copy->folder()}, true);
        QCOMPARE(copies.size(), 2);
        for (const QString &path : copies)
            QCOMPARE(DicomFile::read(path).first(Tag::Modality), QString("CT"));
        QCOMPARE(w.says("anonymousHeadline"), QString("Saved 2 files in ⁨Anonymous scans⁩"));
        QCOMPARE(w.says("anonymousRefused"), QString("2 copies still named the patient, so they weren't kept:"));
        QCOMPARE(w.says("anonymousCaution"), QString("So don't take this folder for an anonymous copy until you know why."));
        w.keep("anonymous-refused");

        // It stays said: past the notice, and whatever else is pressed,
        // until it's dismissed.
        QCOMPARE(w.backend.notice(), QString("Saved 2 files in Anonymous scans"));
        QTRY_COMPARE_WITH_TIMEOUT(w.backend.notice(), QString(), 5000);
        QTest::keyClick(w.window, Qt::Key_Down);
        QTest::keyClick(w.window, Qt::Key_Space);
        QTest::keyClick(w.window, 'S', Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(copy->stage(), AnonymousCopy::Saved);
        QVERIFY(copy->shown() && panel->isVisible());
        QCOMPARE(copy->refusedFiles(), 2);
        QCOMPARE(w.says("anonymousCaution"), QString("So don't take this folder for an anonymous copy until you know why."));
        QTest::keyClick(w.window, Qt::Key_Escape);
        QVERIFY(!copy->shown() && !panel->isVisible());
        QCOMPARE(copy->stage(), AnonymousCopy::Closed);

        // With the patient's details hidden, what was found isn't printed.
        QTest::keyClick(w.window, 'H');
        QVERIFY(w.plan());
        QTest::keyClick(w.window, Qt::Key_Space);
        QVERIFY(w.planned());
        QCOMPARE(w.says("anonymousFound"), QString("a person's name"));
        QTest::keyClick(w.window, Qt::Key_Escape);

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    void blacksOutWhatWasBoxedAndNothingElse() {
        // An ultrasound and a CT, two images each, no pixel of them black.
        QTemporaryDir scans, out;
        for (int i = 1; i <= 2; ++i) {
            QVERIFY(write(scans.filePath(QString("US%1").arg(i)), image("US", "1.2.7.1", i, "Apical", 16, 12, 8)));
            QVERIFY(write(scans.filePath(QString("CT%1").arg(i)), image("CT", "1.2.7.2", i, "Axial", 16, 12, 16)));
        }
        Window w;
        QVERIFY(w.show());
        QVERIFY(w.open(scans.path()));
        w.backend.selectSeries(w.series("Apical"));
        QVERIFY(w.backend.waitForSlice(10000));
        SliceView *view = w.view();
        QTRY_VERIFY(view->width() > 0 && !view->imageRect().isEmpty());

        // A region boxed on the ultrasound, turned on its side.
        const QRect region(5, 4, 5, 4);
        QTest::keyClick(w.window, ']');
        QTest::keyClick(w.window, 'H', Qt::ShiftModifier);
        w.drag(view, QPointF(5.5, 4.5), QPointF(9.5, 7.5));
        QCOMPARE(w.backend.blackouts(), QList<QRect>{region});
        QTest::keyClick(w.window, Qt::Key_Escape);

        // The plan counts it, in its series, which is no longer one whose
        // pictures show the patient with nothing done about it.
        AnonymousCopy *copy = w.copy();
        QVERIFY(w.plan());
        QCOMPARE(copy->regions(), 1);
        QCOMPARE(w.says("anonymousRegions"), QString("1 region will be blacked out, in every image of its series."));
        QCOMPARE(copy->pictures().size(), 1);
        QCOMPARE(copy->pictures().first().toMap().value("name").toString(), QString("Apical"));
        QCOMPARE(copy->pictures().first().toMap().value("regions").toInt(), 1);
        QCOMPARE(copy->unhidden(), 0);
        w.keep("anonymous-regions");

        w.picker.answer = out.path();
        QTest::keyClick(w.window, Qt::Key_Return);
        QVERIFY(w.saved());
        QCOMPARE(copy->files(), 4);
        QCOMPARE(copy->blackedOut(), 2);
        QCOMPARE(copy->uncopiedFiles(), 0);

        // In the copy, both of the ultrasound's images are black there and
        // as they were everywhere else, and the CT's are as they were.
        int ultrasounds = 0, cts = 0;
        for (const QString &path : listFiles({copy->folder()}, true)) {
            const DicomFile file = DicomFile::read(path);
            const int instance = file.integer(Tag::InstanceNumber);
            const Frame frame = decodeFrame(file, 0);
            QVERIFY2(frame.error.isEmpty(), qPrintable(frame.error));
            QCOMPARE(frame.mono.size(), 16 * 12);
            const bool boxed = file.first(Tag::Modality) == "US";
            boxed ? ++ultrasounds : ++cts;
            for (int y = 0; y < 12; ++y)
                for (int x = 0; x < 16; ++x) {
                    const int expected = boxed && region.contains(x, y) ? 0 : valueAt(y * 16 + x, instance);
                    if (frame.mono[y * 16 + x] != expected)
                        QFAIL(qPrintable(QStringLiteral("%1 %2,%3 is %4, not %5").arg(path).arg(x).arg(y)
                                             .arg(frame.mono[y * 16 + x]).arg(expected)));
                }
        }
        QCOMPARE(ultrasounds, 2);
        QCOMPARE(cts, 2);
        QTest::keyClick(w.window, Qt::Key_Return);

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    void boxesARegionInTheImagesOwnPixels() {
        // An ultrasound of two images, 16 across and 12 down, and a CT.
        QTemporaryDir scans;
        for (int i = 1; i <= 2; ++i) {
            QVERIFY(write(scans.filePath(QString("US%1").arg(i)), image("US", "1.2.7.1", i, "Apical", 16, 12, 8)));
            QVERIFY(write(scans.filePath(QString("CT%1").arg(i)), image("CT", "1.2.7.2", i, "Axial", 16, 12, 16)));
        }
        Window w;
        QVERIFY(w.show());
        QVERIFY(w.open(scans.path()));
        const int ultrasound = w.series("Apical"), ct = w.series("Axial");
        QVERIFY(ultrasound >= 0 && ct >= 0);
        w.backend.selectSeries(ultrasound);
        QVERIFY(w.backend.waitForSlice(10000));
        SliceView *view = w.view();
        QVERIFY(view);
        QTRY_VERIFY(view->width() > 0 && !view->imageRect().isEmpty());

        // Shift+H picks the tool up, and a series like this one takes it.
        QTest::keyClick(w.window, 'H', Qt::ShiftModifier);
        QCOMPARE(view->tool(), 5);
        QCOMPARE(w.backend.blackoutRefusal(), QString());

        // A box from the middle of one pixel to the middle of another is
        // every pixel it touches, in the image's own columns and rows,
        // however the picture is zoomed, moved, turned, and flipped.
        const QPointF from(5.5, 4.5), to(9.5, 7.5);
        const QRect touched(5, 4, 5, 4);
        for (int turns = 0; turns < 4; ++turns)
            for (const bool flipped : {false, true}) {
                QTest::keyClick(w.window, 'R');
                for (int i = 0; i < turns; ++i)
                    QTest::keyClick(w.window, ']');
                if (flipped)
                    QTest::keyClick(w.window, 'X');
                QCOMPARE(w.backend.rotation(), turns);
                QCOMPARE(w.backend.flipped(), flipped);
                view->zoomAt(view->width() / 2, view->height() / 2, 2);
                view->pan(-40, 25);
                // Both corners are in the view, to be dragged between.
                const QRectF shown = view->mapRectToScene(view->boundingRect());
                QVERIFY(shown.contains(w.at(view, from)) && shown.contains(w.at(view, to)));
                // From either corner: the box is the same.
                turns % 2 ? w.drag(view, to, from) : w.drag(view, from, to);
                QCOMPARE(w.backend.blackouts(), QList<QRect>{touched});
                QCOMPARE(w.backend.notice(), QString("1 region will be blacked out of every image of this series"));
                // Delete takes the last one away while the tool is in hand.
                QTest::keyClick(w.window, Qt::Key_Delete);
                QVERIFY(w.backend.blackouts().isEmpty());
                QCOMPARE(w.backend.notice(), QString("Nothing will be blacked out of this series"));
            }
        QTest::keyClick(w.window, 'R');

        // A click with no drag boxes nothing, and a box that runs off the
        // picture is cut to it.
        w.drag(view, from, from);
        QVERIFY(w.backend.blackouts().isEmpty());
        view->zoomAt(view->width() / 2, view->height() / 2, 0.5);
        w.drag(view, QPointF(13.5, 9.5), QPointF(17.5, 13.5));
        QCOMPARE(w.backend.blackouts(), QList<QRect>{QRect(13, 9, 3, 3)});
        QTest::keyClick(w.window, Qt::Key_Delete);
        QTest::keyClick(w.window, 'R');

        // It's drawn black on the picture, where the picture was bright.
        const QPoint middle = w.at(view, QPointF(7.5, 6.5));
        const double before = brightness(w.window->grabWindow(), middle);
        QVERIFY2(before > 60, qPrintable(QString::number(before)));
        w.drag(view, from, to);
        QCOMPARE(w.backend.blackouts(), QList<QRect>{touched});
        QVERIFY(brightness(w.window->grabWindow(), middle) < before / 3);

        // A region is the series': the other pane has it once it shows the
        // series, drawn on its picture too, and one boxed there shows here.
        QTest::keyClick(w.window, Qt::Key_Escape);
        QCOMPARE(view->tool(), 0);
        QTest::keyClick(w.window, 'C');
        QCOMPARE(w.partner.seriesIndex(), ct);
        QVERIFY(w.partner.blackouts().isEmpty());
        w.partner.selectSeries(ultrasound);
        QVERIFY(w.partner.waitForSlice(10000));
        QCOMPARE(w.partner.blackouts(), QList<QRect>{touched});
        SliceView *right = w.view("rightView");
        QVERIFY(right);
        QTRY_VERIFY(right->isVisible() && !right->imageRect().isEmpty());
        QTRY_VERIFY(brightness(w.window->grabWindow(), w.at(right, QPointF(7.5, 6.5))) < before / 3);
        QTest::keyClick(w.window, 'H', Qt::ShiftModifier);
        w.drag(right, QPointF(0.5, 0.5), QPointF(2.5, 1.5));
        const QList<QRect> both{touched, QRect(0, 0, 3, 2)};
        QCOMPARE(w.partner.blackouts(), both);
        QCOMPARE(w.backend.blackouts(), both);
        w.keep("blackout");
        QTRY_VERIFY(brightness(w.window->grabWindow(), w.at(view, QPointF(1.5, 1.0))) < before / 3);
        // And not the other series, in either pane.
        w.partner.selectSeries(ct);
        QVERIFY(w.partner.blackouts().isEmpty());
        QCOMPARE(w.library->blackouts()->all().keys(), QList<int>{ultrasound});

        // Shift+Delete clears the series' regions, from whichever pane.
        QTest::keyClick(w.window, Qt::Key_Tab);
        QTest::keyClick(w.window, Qt::Key_Delete, Qt::ShiftModifier);
        QVERIFY(w.backend.blackouts().isEmpty());
        QVERIFY(w.library->blackouts()->all().isEmpty());

        // They stay with the series for as long as the app is open: there
        // again when its scan is opened after another.
        w.drag(view, from, to);
        QCOMPARE(w.backend.blackouts(), QList<QRect>{touched});
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTemporaryDir other;
        QVERIFY(write(other.filePath("CT1"), image("CT", "1.2.8.1", 1, "Another", 16, 12, 16)));
        QVERIFY(w.open(other.path()));
        QVERIFY(w.library->blackouts()->all().isEmpty());
        QVERIFY(w.open(scans.path()));
        QCOMPARE(w.library->blackouts()->all().value(w.series("Apical")), QList<QRect>{touched});

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    void refusesARegionWhereACopyCantBlackItOut() {
        // A CT that can be cut the other ways, and a series of 32-bit
        // pictures, which a copy can't write back value for value.
        QTemporaryDir scans;
        QVERIFY(writeVolume(scans.path()));
        for (int i = 1; i <= 2; ++i)
            QVERIFY(write(scans.filePath(QString("DEEP%1").arg(i)), image("MR", "1.2.7.5", i, "Deep", 4, 3, 32)));
        Window w;
        QVERIFY(w.show());
        QVERIFY(w.open(scans.path()));
        const int deep = w.series("Deep");
        QVERIFY(deep >= 0);
        const int volume = deep == 0 ? 1 : 0;
        w.backend.selectSeries(volume);
        QVERIFY(w.backend.waitForSlice(10000));
        QVERIFY(w.backend.canReformat());
        SliceView *view = w.view();
        QVERIFY(view);
        QTRY_VERIFY(view->width() > 0 && !view->imageRect().isEmpty());
        QTest::keyClick(w.window, 'H', Qt::ShiftModifier);
        QCOMPARE(view->tool(), 5);

        // As scanned, a region can be boxed.
        QCOMPARE(w.backend.blackoutRefusal(), QString());
        w.drag(view, QPointF(0.5, 0.5), QPointF(1.5, 1.5));
        QCOMPARE(w.backend.blackouts(), QList<QRect>{QRect(0, 0, 2, 2)});

        // Cut another way, the picture has none of the slices' own pixels:
        // the tool says so, a drag boxes nothing, and what the series has
        // isn't drawn where it would mean nothing.
        const QString asScanned = "A region is blacked out of the picture as it was scanned. V goes back to it.";
        QTest::keyClick(w.window, 'V');
        QCOMPARE(w.backend.plane(), 1);
        QTRY_VERIFY(w.backend.frameReady());
        QCOMPARE(w.backend.blackoutRefusal(), asScanned);
        w.drag(view, QPointF(0.5, 0.5), QPointF(2.5, 2.5));
        QCOMPARE(w.backend.blackouts().size(), 1);
        QVERIFY(!view->drawing());
        QCOMPARE(w.backend.notice(), asScanned);
        // Tilted, the same.
        QTest::keyClick(w.window, 'V');
        QTest::keyClick(w.window, 'V');
        QCOMPARE(w.backend.plane(), 0);
        QCOMPARE(w.backend.blackoutRefusal(), QString());
        QTest::keyClick(w.window, Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(w.backend.plane(), 3);
        QTRY_VERIFY(w.backend.frameReady());
        QCOMPARE(w.backend.blackoutRefusal(), asScanned);
        w.drag(view, QPointF(0.5, 0.5), QPointF(2.5, 2.5));
        QCOMPARE(w.backend.blackouts().size(), 1);
        QTest::keyClick(w.window, 'V');
        QCOMPARE(w.backend.plane(), 0);

        // What the anonymizer can't draw again is refused in its words,
        // made the user's.
        w.backend.selectSeries(deep);
        QVERIFY(w.backend.waitForSlice(10000));
        const QString tooDeep = "A region can't be blacked out of this series: its pixels are 32-bit, which can't be written "
                                "back value for value.";
        QCOMPARE(w.backend.blackoutRefusal(), tooDeep);
        w.drag(view, QPointF(0.5, 0.5), QPointF(2.5, 2.5));
        QVERIFY(w.backend.blackouts().isEmpty());
        QCOMPARE(w.backend.notice(), tooDeep);
        // The anonymizer's own answer, which a copy would give as well.
        QVERIFY(!Anonymize::canRedraw(DicomFile::read(scans.filePath("DEEP1"))).possible());

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }
};

QTEST_MAIN(AnonymousTests)
#include "anonymous_tests.moc"
