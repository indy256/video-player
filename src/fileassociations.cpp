#include "fileassociations.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <shlobj.h>
#elif defined(Q_OS_MACOS)
#include <CoreServices/CoreServices.h>
#elif defined(Q_OS_LINUX)
#include <QImage>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#endif

static void initializeAssociationResources() { Q_INIT_RESOURCE(association_resources); }

namespace FileAssociations {
QStringList extensions() {
    return {"mp4", "mkv", "avi", "mov", "webm", "m4v", "wmv", "mpeg", "mpg", "ts", "m2ts", "ogv"};
}

QString executablePath() {
    QString path;
#ifdef Q_OS_WIN
    path = qEnvironmentVariable("VIDEO_PLAYER_LAUNCHER");
#elif defined(Q_OS_LINUX)
    path = qEnvironmentVariable("APPIMAGE");
#endif
    if (path.isEmpty()) path = QCoreApplication::applicationFilePath();
    return QFileInfo(path).absoluteFilePath();
}

QString writeWindowsRegistration(QSettings &registry, const QString &executable) {
    const QString path = QDir::toNativeSeparators(executable);
    const QString command = '"' + path + "\" \"%1\"";
    const QString icon = '"' + path + "\",0";
    const QString progId = "VideoPlayer.Video";
    const QString type = "Classes/" + progId;
    const QString capabilities = "VideoPlayer/Capabilities";
    registry.setValue(type + "/Default", "Video Player video");
    registry.setValue(type + "/DefaultIcon/Default", icon);
    registry.setValue(type + "/shell/open/command/Default", command);
    registry.setValue(capabilities + "/ApplicationName", "Video Player");
    registry.setValue(capabilities + "/ApplicationDescription", "Play local video files.");
    registry.setValue(capabilities + "/ApplicationIcon", icon);
    for (const QString &extension : extensions()) {
        registry.setValue("Classes/." + extension + "/OpenWithProgids/" + progId, QString());
        registry.setValue(capabilities + "/FileAssociations/." + extension, progId);
    }
    registry.setValue("RegisteredApplications/Video Player", "Software\\VideoPlayer\\Capabilities");
    registry.sync();
    return registry.status() == QSettings::NoError ? QString()
        : QString("Could not save file type registration to your user registry.");
}

QByteArray desktopEntry(const QString &executable) {
    // Desktop Exec quoting is followed by desktop-entry string escaping.
    QString path = executable;
    path.replace('\\', "\\\\");
    path.replace('"', "\\\"");
    path.replace('`', "\\`");
    path.replace('$', "\\$");
    path.replace('%', "%%");
    path.replace('\\', "\\\\");
    path.replace('\n', "\\n");
    path.replace('\r', "\\r");
    path.replace('\t', "\\t");
    initializeAssociationResources();
    QFile entry(":/packaging/video-player.desktop");
    if (!entry.open(QIODevice::ReadOnly)) return {};
    return entry.readAll().replace("\r\n", "\n").replace("Exec=VideoPlayer %f", ("Exec=\"" + path + "\" %f").toUtf8());
}

QString registerFileTypes() {
#ifdef Q_OS_WIN
    const QString executable = executablePath();
    if (!QFileInfo(executable).isFile()) return "The Video Player executable is unavailable.";
    QSettings registry("HKEY_CURRENT_USER\\Software", QSettings::NativeFormat);
    const QString error = writeWindowsRegistration(registry, executable);
    if (error.isEmpty()) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return error;
#elif defined(Q_OS_MACOS)
    if (!QCoreApplication::applicationDirPath().endsWith(".app/Contents/MacOS"))
        return "Run Video Player from its installed VideoPlayer.app bundle.";
    CFBundleRef bundle = CFBundleGetMainBundle();
    CFURLRef url = bundle ? CFBundleCopyBundleURL(bundle) : nullptr;
    if (!url) return "Could not locate VideoPlayer.app. Move it to Applications and try again.";
    const OSStatus status = LSRegisterURL(url, true);
    CFRelease(url);
    if (status != noErr) return QString("Could not register VideoPlayer.app (error %1).").arg(status);
    return {};
#elif defined(Q_OS_LINUX)
    const QString executable = executablePath();
    if (!QFileInfo(executable).isFile() || !QFileInfo(executable).isExecutable())
        return "The Video Player executable is unavailable or is not executable.";
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString applications = data + "/applications";
    const QString icons = data + "/icons/hicolor/512x512/apps";
    if (data.isEmpty() || !QDir().mkpath(applications) || !QDir().mkpath(icons))
        return "Could not create your application registration directories.";
    if (!QImage(":/assets/app-icon.png").save(icons + "/video-player.png"))
        return "Could not install the Video Player icon.";
    QSaveFile file(applications + "/video-player.desktop");
    const QByteArray entry = desktopEntry(executable);
    if (entry.isEmpty()) return "Could not load the desktop entry.";
    if (!file.open(QIODevice::WriteOnly) || file.write(entry) != entry.size() || !file.commit())
        return "Could not save the desktop entry: " + file.errorString();
    const QString updater = QStandardPaths::findExecutable("update-desktop-database");
    if (!updater.isEmpty()) {
        QProcess process;
        process.start(updater, {applications});
        if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            process.kill();
            process.waitForFinished(1000);
            return "Registration was saved, but refreshing Open With failed. Sign out and back in to refresh it.";
        }
    }
    return {};
#else
    return "File type registration is not supported on this platform.";
#endif
}
}
