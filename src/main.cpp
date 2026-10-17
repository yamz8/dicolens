// scans — a DICOM viewer: open a scan, a folder, or a hospital CD, and look
// through it slice by slice.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickStyle>
#include <QUrl>

#include "backend.h"
#include "cli.h"
#include "portalfilepicker.h"
#include "sliceview.h"
#include "theme.h"
#include "translator.h"

namespace {

// Series thumbnails, as image://thumbnails/<series>/<version>.
class ThumbnailProvider : public QQuickImageProvider {
public:
    explicit ThumbnailProvider(Backend *backend) : QQuickImageProvider(Image), m_backend(backend) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &) override {
        QImage image = m_backend->thumbnail(id.section('/', 0, 0).toInt());
        // Until it's decoded, a transparent pixel stands in.
        if (image.isNull()) {
            image = QImage(1, 1, QImage::Format_ARGB32);
            image.fill(Qt::transparent);
        }
        if (size)
            *size = image.size();
        return image;
    }

private:
    Backend *m_backend;
};

bool isCommand(int argc, char *argv[]) {
    if (argc < 2)
        return false;
    const QByteArray first = argv[1];
    return first == "info" || first == "tags" || first == "export" || first == "skill" || first == "help" ||
           first == "--help" || first == "-h" || first == "--version";
}

}

int main(int argc, char *argv[]) {
    // Commands run without a display.
    const bool command = isCommand(argc, argv);
    if (command)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("scans");
    app.setApplicationVersion("0.1.0");

    QStringList open;
    if (const int code = runCli(app.arguments(), open); code >= 0)
        return code;

    // Matches the window to scans.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("scans");
    app.setWindowIcon(QIcon::fromTheme("scans"));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    PortalFilePicker picker;
    Backend backend(&picker);

    qmlRegisterType<SliceView>("Scans", 1, 0, "SliceView");
    qmlRegisterUncreatableType<Backend>("Scans", 1, 0, "Backend", "The backend is given to QML as \"backend\"");

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    // Strings made in C++ are translated when made, so remake them too.
    QObject::connect(&translator, &Translator::changed, &backend, [&backend] { backend.retranslate(); });
    engine.addImageProvider("thumbnails", new ThumbnailProvider(&backend));
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (!open.isEmpty())
        backend.loadPaths(open);

    return app.exec();
}
