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

// The anonymous copy as the window offers it: the regions boxed on a
// picture to black out of its series, through the real Main.qml and the
// real backends, with the mouse and the keys.

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

// One image of Jane Doe's, with what a hospital's files say of a patient
// around it: grey, 8 or 16 bits, each pixel a value of its own that's
// never the black a region is blacked out with.
QByteArray image(const QByteArray &modality, const QByteArray &series, int instance, const QByteArray &description,
                 int columns, int rows, int bits) {
    Writer w;
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
        const int value = 100 + (i * 7 + instance * 13) % 150;
        pixels.append(char(value));
        for (int byte = 1; byte < bits / 8; ++byte)
            pixels.append(char(0));
    }
    return w.pixels(pixels, bits == 8 ? "OB" : "OW").file(ExplicitLE);
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

    // Where a point of a view's picture, in image pixels, is on the window.
    QPoint at(SliceView *view, QPointF image) const {
        return view->mapToScene(view->toItem(image.x(), image.y())).toPoint();
    }

    // With DICOLENS_TEST_SHOTS naming a folder, a picture of the window is
    // kept there: bin/looks reaches only what keys reach, and these are the
    // states the mouse and a chooser's answer lead to.
    void keep(const QString &name) {
        if (const QString folder = qEnvironmentVariable("DICOLENS_TEST_SHOTS"); !folder.isEmpty())
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
