#include "playerwindow.h"
#include <QApplication>

int main(int argc, char *argv[]) {
    // Use the installed FFmpeg shared libraries through Qt Multimedia's backend.
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    QApplication app(argc, argv);
    app.setApplicationName("Video Player");
    app.setOrganizationName("LocalApps");
    app.setStyle("Fusion");
    PlayerWindow window;
    window.show();
    if (app.arguments().size() > 1) window.openFile(app.arguments().at(1));
    return app.exec();
}
