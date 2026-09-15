#include "playerwindow.h"
#include "singleinstance.h"
#include <QApplication>
#include <QPalette>
#include <QIcon>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QMediaPlayer>
#include <QVideoSink>
#include <QVideoFrame>
#include <QTimer>

int main(int argc, char *argv[]) {
    // Use the installed FFmpeg shared libraries through Qt Multimedia's backend.
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    QApplication app(argc, argv);
    app.setApplicationName("Video Player");
    app.setWindowIcon(QIcon(":/assets/app-icon.png"));
    app.setOrganizationName("LocalApps");
    // Exercise the deployed platform plugin and FFmpeg decoder without opening a
    // window, changing saved settings, or forwarding to an already running player.
    if (app.arguments().value(1) == "--runtime-check") {
        if (app.arguments().size() != 3 || !QFileInfo::exists(app.arguments().at(2))) return 2;
        QMediaPlayer player;
        QVideoSink sink;
        player.setVideoSink(&sink);
        QObject::connect(&sink, &QVideoSink::videoFrameChanged, &app, [&app](const QVideoFrame &frame) {
            if (frame.isValid()) app.exit(0);
        });
        QObject::connect(&player, &QMediaPlayer::errorOccurred, &app, [&app] { app.exit(3); });
        QTimer::singleShot(15000, &app, [&app] { app.exit(4); });
        player.setSource(QUrl::fromLocalFile(QFileInfo(app.arguments().at(2)).absoluteFilePath()));
        QTimer::singleShot(0, &player, &QMediaPlayer::play);
        return app.exec();
    }
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
