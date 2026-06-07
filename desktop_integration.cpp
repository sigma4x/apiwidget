#include "desktop_integration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>

namespace {

constexpr auto kIconResource = ":/icons/apiwidget.png";
constexpr auto kDesktopName = "apiwidget.desktop";

QString applicationsDir() {
    return QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
}

QString autostartPath() {
    return QDir::homePath() + "/.config/autostart/" + kDesktopName;
}

QString canonicalExec() {
    return QDir::homePath() + "/.local/bin/apiwidget";
}

void installIcons() {
    const QImage img(kIconResource);
    if (img.isNull())
        return;

    for (int size : {48, 128, 256}) {
        const QString dir =
            QDir::homePath() + QString("/.local/share/icons/hicolor/%1x%1/apps").arg(size);
        QDir().mkpath(dir);
        img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            .save(dir + "/apiwidget.png", "PNG");
    }

    const QString cacheDir = QDir::homePath() + "/.local/share/icons/hicolor";
    QProcess::execute(QStringLiteral("gtk-update-icon-cache"),
                      {QStringLiteral("-f"), QStringLiteral("-t"), cacheDir});
}

void writeDesktopFile(const QString &path, bool autostart) {
    const QString exec = canonicalExec();
    QString content = QStringLiteral(
                          "[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=API Usage Widget\n"
                          "Comment=Claude and Cursor token usage\n"
                          "Exec=\"%1\"\n"
                          "Icon=apiwidget\n"
                          "Categories=Utility;\n"
                          "StartupNotify=true\n")
                          .arg(exec);
    if (autostart)
        content += QStringLiteral("X-GNOME-Autostart-enabled=true\n");

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(content.toUtf8());
}

} // namespace

void setupDesktopIntegration() {
    installIcons();

    const QString appsDir = applicationsDir();
    QDir().mkpath(appsDir);
    writeDesktopFile(appsDir + "/" + kDesktopName, false);

    if (QFile::exists(autostartPath()))
        writeDesktopFile(autostartPath(), true);
}

void writeAutostartDesktop(bool enabled) {
    if (enabled) {
        QDir().mkpath(QDir::homePath() + "/.config/autostart");
        writeDesktopFile(autostartPath(), true);
    } else {
        QFile::remove(autostartPath());
    }
}
