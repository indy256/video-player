#include "updater.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>

static void initializeUpdateResources() { Q_INIT_RESOURCE(updater); }

namespace {
constexpr auto title = "Update to latest version";
bool writeFile(const QString &path, const QByteArray &data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.flush();
}
QByteArray checksum(const QString &path) {
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return hash.result().toHex();
}
QProcessEnvironment cleanEnvironment() {
    auto environment = QProcessEnvironment::systemEnvironment();
    // Detached helpers and the new AppImage must not inherit the old runtime.
    for (const char *key : {"APPIMAGE", "APPDIR", "OWD", "LD_LIBRARY_PATH",
                            "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                            "VIDEO_PLAYER_LAUNCHER"})
        environment.remove(QString::fromLatin1(key));
    return environment;
}
bool download(QNetworkAccessManager &network, QProgressDialog &progress,
              const QUrl &url, const QString &path, qint64 limit, QString &error) {
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly)) { error = output.errorString(); return false; }
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "VideoPlayer-Updater");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    auto *reply = network.get(request);
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    qint64 total = 0;
    const auto consume = [&] {
        const QByteArray bytes = reply->readAll();
        total += bytes.size();
        if (total > limit) error = "The download exceeds the expected size.";
        else if (output.write(bytes) != bytes.size()) error = output.errorString();
        if (!error.isEmpty()) reply->abort();
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, consume);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&progress, &QProgressDialog::canceled, &loop, [&] { reply->abort(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        error = "The download timed out.";
        reply->abort();
    });
    deadline.start(300000);
    if (!reply->isFinished()) loop.exec();
    if (error.isEmpty()) consume();
    if (error.isEmpty() && reply->error() != QNetworkReply::NoError) error = reply->errorString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (error.isEmpty() && status != 200) error = QString("The server returned HTTP %1.").arg(status);
    if (!output.flush() && error.isEmpty()) error = output.errorString();
    reply->deleteLater();
    return error.isEmpty() && !progress.wasCanceled();
}
#ifdef Q_OS_MACOS
bool runTool(const QString &program, const QStringList &args, QProgressDialog &progress, QString &error) {
    QProcess process;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&process, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    QObject::connect(&progress, &QProgressDialog::canceled, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    process.start(program, args);
    timer.start(60000);
    loop.exec();
    if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(1000); }
    if (!timer.isActive() || process.error() == QProcess::FailedToStart
        || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = "Could not prepare the application bundle: " + process.errorString()
            + "\n" + QString::fromUtf8(process.readAllStandardError()).left(2000);
        return false;
    }
    return !progress.wasCanceled();
}
#endif
}

bool AppUpdate::releaseAsset(const QJsonObject &release, const QString &name, Asset &asset) {
    asset = {};
    if (release.value("draft").toBool() || release.value("prerelease").toBool()
        || release.value("tag_name").toString().isEmpty()) return false;
    for (const auto &entry : release.value("assets").toArray()) {
        const auto object = entry.toObject();
        if (object.value("name").toString().compare(name, Qt::CaseInsensitive)) continue;
        const QString digest = object.value("digest").toString();
        const QUrl url(object.value("browser_download_url").toString());
        const qint64 size = object.value("size").toInteger();
        if (!QRegularExpression("^sha256:[0-9a-f]{64}$").match(digest).hasMatch()
            || size <= 0 || size > 1024LL * 1024 * 1024 || !url.isValid()
            || url.scheme() != "https" || url.host() != "github.com" || !url.userInfo().isEmpty()
            || (url.port() != -1 && url.port() != 443)
            || !url.path().startsWith("/indy256/video-player/releases/download/")) return false;
        asset = {url, digest.mid(7).toLatin1(), size};
        return true;
    }
    return false;
}

bool AppUpdate::installLatest(QWidget *parent, const QString &videoPath) {
    // Explicitly reference the resource initializer because player_ui is static.
    initializeUpdateResources();
    const auto fail = [parent](const QString &message) {
        QMessageBox::warning(parent, title, message);
        return false;
    };
    QString target = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    QString name;
#if defined(Q_OS_WIN)
#if defined(Q_PROCESSOR_X86_64)
    name = "video-player-windows-x64.exe";
#elif defined(Q_PROCESSOR_ARM_64)
    name = "video-player-windows-arm64.exe";
#endif
    const QString launcher = qEnvironmentVariable("VIDEO_PLAYER_LAUNCHER");
    if (!launcher.isEmpty()) target = QFileInfo(launcher).canonicalFilePath();
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    name = "video-player-linux-x64.AppImage";
    target = QFileInfo(qEnvironmentVariable("APPIMAGE")).canonicalFilePath();
    if (qEnvironmentVariableIsEmpty("APPIMAGE"))
        return fail("Run the AppImage release to update automatically.");
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
    name = "video-player-macos-arm64.dmg";
    QDir bundle(QCoreApplication::applicationDirPath());
    if (!bundle.cdUp() || !bundle.cdUp() || !bundle.dirName().endsWith(".app"))
        return fail("Run the installed VideoPlayer.app to update automatically.");
    target = QFileInfo(bundle.absolutePath()).canonicalFilePath();
#endif
    if (name.isEmpty()) return fail("No release is available for this operating system and architecture.");
    if (target.isEmpty() || !QFileInfo::exists(target)) return fail("Cannot locate the installed application.");
    QTemporaryDir stage(QFileInfo(target).absolutePath() + "/.video-player-update-XXXXXX");
    if (!stage.isValid()) return fail("The application folder is not writable. Move the app to a writable folder and try again.");
    QProgressDialog progress("Checking the latest release...", "Cancel", 0, 0, parent);
    progress.setWindowTitle(title);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.show();
    QNetworkAccessManager network;
    QString error;
    const auto failed = [&] { progress.hide(); return progress.wasCanceled() ? false : fail(error); };
    const QString metadataPath = stage.filePath("release.json");
    if (!download(network, progress, QUrl("https://api.github.com/repos/indy256/video-player/releases/latest"),
                  metadataPath, 2 * 1024 * 1024, error)) return failed();
    QFile metadata(metadataPath);
    if (!metadata.open(QIODevice::ReadOnly)) return fail("Cannot read the release information.");
    const auto release = QJsonDocument::fromJson(metadata.readAll()).object();
    metadata.close();
    Asset asset;
    if (!releaseAsset(release, name, asset))
        return fail("The latest release has no verified download for " + name + ". Try again after its builds finish.");
#ifndef Q_OS_MACOS
    if (checksum(target) == asset.sha256) {
        progress.hide();
        QMessageBox::information(parent, title, "You already have the latest version.");
        return false;
    }
#endif
    progress.setLabelText("Downloading " + release.value("tag_name").toString() + "...");
    QString source = stage.filePath("download");
    if (!download(network, progress, asset.url, source, asset.size, error)) return failed();
    progress.setLabelText("Verifying the download...");
    if (QFileInfo(source).size() != asset.size || checksum(source) != asset.sha256)
        return fail("The download failed verification. Your application has not been changed.");
#ifdef Q_OS_MACOS
    const QString mount = stage.filePath("mount");
    QDir().mkpath(mount);
    const bool mounted = runTool("/usr/bin/hdiutil", {"attach", "-readonly", "-nobrowse", "-mountpoint", mount, source}, progress, error);
    source = stage.filePath("new.app");
    const bool copied = mounted && runTool("/usr/bin/ditto", {mount + "/VideoPlayer.app", source}, progress, error);
    QProcess detach;
    detach.start("/usr/bin/hdiutil", {"detach", mount});
    if (!detach.waitForFinished(15000)) { detach.kill(); detach.waitForFinished(1000); }
    if (detach.exitStatus() != QProcess::NormalExit || detach.exitCode() != 0) {
        stage.setAutoRemove(false); // Never recursively delete a mounted image.
        return fail("Could not detach the disk image. Update files remain in " + stage.path());
    }
    if (!copied) return failed();
    if (!QFileInfo(source + "/Contents/MacOS/VideoPlayer").isExecutable())
        return fail("The release does not contain a valid VideoPlayer.app.");
#else
    if (!QFile::setPermissions(source, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
            | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther))
        return fail("Cannot prepare the downloaded application.");
#endif
    if (progress.wasCanceled()) return false;
    const QString ready = stage.filePath("ready");
    const QString commit = stage.filePath("commit");
    QProcess installer;
#ifdef Q_OS_WIN
    const QString script = stage.filePath("install.ps1");
    const QString plan = stage.filePath("plan.json");
    const QJsonObject data{{"target", target}, {"source", source}, {"video", videoPath},
                           {"pid", QCoreApplication::applicationPid()}};
    if (!QFile::copy(":/updates/update-windows.ps1", script) || !writeFile(plan, QJsonDocument(data).toJson()))
        return fail("Cannot prepare the update helper.");
    installer.setProgram(qEnvironmentVariable("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe");
    installer.setArguments({"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                           "-WindowStyle", "Hidden", "-File", script, "-PlanPath", plan});
    installer.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#else
    const QString script = stage.filePath("install.sh");
    if (!QFile::copy(":/updates/update-unix.sh", script)) return fail("Cannot prepare the update helper.");
    installer.setProgram("/bin/sh");
    installer.setArguments({script, target, source, QString::number(QCoreApplication::applicationPid()), videoPath});
#endif
    installer.setWorkingDirectory(stage.path());
    installer.setProcessEnvironment(cleanEnvironment());
    installer.setProcessChannelMode(QProcess::MergedChannels);
    installer.setStandardOutputFile(stage.filePath("helper.log"));
    if (!installer.startDetached()) return fail("Cannot start the update helper. Your application has not been changed.");
    stage.setAutoRemove(false);
    progress.setCancelButton(nullptr);
    progress.setLabelText("Preparing to restart...");
    QEventLoop loop;
    QTimer poll, deadline;
    poll.setInterval(50);
    deadline.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (QFileInfo::exists(ready)) loop.quit(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    deadline.start(15000);
    loop.exec();
    progress.hide();
    if (!QFileInfo::exists(ready) || !writeFile(commit, "install"))
        return fail("The update helper could not prepare the installation. Your app is still running. Logs: " + stage.path());
    return true;
}
