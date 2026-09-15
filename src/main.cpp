#include "playerwindow.h"
#include "singleinstance.h"
#include <QApplication>
#include <QPalette>
#include <QIcon>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>

int main(int argc, char *argv[]) {
    // Use the installed FFmpeg shared libraries through Qt Multimedia's backend.
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    QApplication app(argc, argv);
    app.setApplicationName("Video Player");
    app.setWindowIcon(QIcon(":/assets/app-icon.png"));
    app.setOrganizationName("LocalApps");
    const QString file = app.arguments().size() > 1 ? QFileInfo(app.arguments().at(1)).absoluteFilePath() : QString();
    const QString userKey = QString::fromLatin1(QCryptographicHash::hash(
        QDir::homePath().toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    SingleInstance instance("LocalApps.VideoPlayer." + userKey);
    const auto result = instance.start(file);
    if (result == SingleInstance::Result::Forwarded) return 0;
    if (result == SingleInstance::Result::Failed) {
        QMessageBox::warning(nullptr, "Video Player", instance.errorString());
        return 1;
    }
    app.setStyle("Fusion");
    QPalette palette = app.palette();
    palette.setColor(QPalette::Window, Qt::black);
    palette.setColor(QPalette::Base, Qt::black);
    app.setPalette(palette);
    PlayerWindow window;
    QObject::connect(&instance, &SingleInstance::openRequested, &window, [&window](const QString &path) {
        if (!path.isEmpty()) window.openFile(path);
        if (window.isMinimized()) window.setWindowState(window.windowState() & ~Qt::WindowMinimized);
        window.show();
        window.raise();
        window.activateWindow();
    });
    if (!file.isEmpty()) window.openFile(file);
    window.show();
    return app.exec();
}
