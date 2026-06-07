#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include "MainWindow.h"
#include "desktop_integration.h"

namespace {

constexpr auto kIconResource = ":/icons/apiwidget.png";

} // namespace

int main(int argc, char *argv[]) {
    // GNOME/Mutter ignores always-on-top and per-window opacity for native Wayland
    // surfaces; both are honored under XWayland. Force xcb unless overridden.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");

    QApplication app(argc, argv);
    // Toggling window flags briefly destroys the toplevel; without this the app
    // would quit on the transient "last window closed". Only Quit/close ends it.
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setOrganizationName("apiwidget");
    QApplication::setApplicationName("apiwidget");
    QApplication::setWindowIcon(QIcon(kIconResource));
    QGuiApplication::setDesktopFileName(QStringLiteral("apiwidget"));
    setupDesktopIntegration();

    MainWindow w;

    // dev: render current state to PNG and exit (visual check without a display)
    const QStringList args = app.arguments();
    const int shotIdx = args.indexOf(QStringLiteral("--shot"));
    if (shotIdx > 0 && shotIdx + 1 < args.size()) {
        w.resize(300, 320);
        w.show();
        return w.grab().save(args.at(shotIdx + 1)) ? 0 : 1;
    }

    w.show();
    return app.exec();
}
