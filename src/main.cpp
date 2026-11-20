// Dicolens — a DICOM viewer: open a scan, a folder, or a hospital CD, and look
// through it slice by slice.

#include <QGuiApplication>
#include <QIcon>
#include <QKeySequence>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include "archive.h"
#include "backend.h"
#include "cli.h"
#include "portalfilepicker.h"
#include "qmlmodule.h"
#include "textsize.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // Commands run without a display, which QGuiApplication would abort
    // for before the arguments are read.
    if (needsNoWindow(argc, argv))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("dicolens");
    app.setApplicationVersion("0.7.0");

    QStringList open;
    if (const int code = runCli(app.arguments(), open); code >= 0) {
        Archive::cleanUp(true);
        return code;
    }

    // Matches the window to dicolens.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon. The compiled-in copy covers
    // a run from build/, and needs qt6-svg's icon engine.
    app.setDesktopFileName("dicolens");
    app.setWindowIcon(QIcon::fromTheme("dicolens", QIcon(":/dicolens.svg")));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    TextSize textSize;
    PortalFilePicker picker;
    Backend backend(&picker);
    // The second pane, side by side, showing the same scans.
    Backend partner(&picker);
    // Four views: the series cut the other two ways beside it, and in 3D.
    Backend coronal(&picker);
    Backend sagittal(&picker);

    keepQmlModule();

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    // Strings made in C++ are translated when made, so remake them too.
    QObject::connect(&translator, &Translator::changed, &backend, [&backend, &partner, &coronal, &sagittal] {
        backend.retranslate();
        partner.retranslate();
        coronal.retranslate();
        sagittal.retranslate();
    });
    engine.addImageProvider("thumbnails", new ThumbnailProvider(&backend));
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("textSize", &textSize);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("partner", &partner);
    engine.rootContext()->setContextProperty("coronal", &coronal);
    engine.rootContext()->setContextProperty("sagittal", &sagittal);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (!open.isEmpty())
        backend.loadPaths(open);

    // `omarchy agent look app` runs the app offscreen with these set, to see
    // the window without a display: size it to OMARCHY_LOOK_SIZE when given,
    // press the keys in OMARCHY_LOOK_KEYS through the window's Shortcuts,
    // save it once it has settled, and quit.
    if (const QString shot = qEnvironmentVariable("OMARCHY_LOOK_SCREENSHOT"); !shot.isEmpty()) {
        const QStringList size = qEnvironmentVariable("OMARCHY_LOOK_SIZE").split('x');
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()); window && size.size() == 2)
            window->resize(size[0].toInt(), size[1].toInt());
        const QStringList keys = qEnvironmentVariable("OMARCHY_LOOK_KEYS").split(' ', Qt::SkipEmptyParts);
        for (int i = 0; i < keys.size(); ++i) {
            QTimer::singleShot(800 + i * 300, &app, [&engine, key = QKeySequence(keys[i])] {
                for (QObject *shortcut : engine.rootObjects().constFirst()->findChildren<QObject *>()) {
                    if (qstrcmp(shortcut->metaObject()->className(), "QQuickShortcut") == 0
                        && shortcut->property("enabled").toBool()
                        && QKeySequence(shortcut->property("sequence").toString()) == key) {
                        QMetaObject::invokeMethod(shortcut, "activated");
                        break;
                    }
                }
            });
        }
        const int delay = qEnvironmentVariableIsSet("OMARCHY_LOOK_DELAY_MS") ? qEnvironmentVariableIntValue("OMARCHY_LOOK_DELAY_MS") : 1500;
        QTimer::singleShot(800 + keys.size() * 300 + delay, &app, [&engine, shot] {
            if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()))
                window->grabWindow().save(shot);
            QCoreApplication::quit();
        });
    }

    Archive::cleanUp(false);
    const int status = app.exec();
    // Q quits under a video or an anonymous copy still being saved, which
    // may be reading a zip's unpacked files: they finish before those go.
    QThreadPool::globalInstance()->waitForDone();
    Archive::cleanUp(true);
    return status;
}
