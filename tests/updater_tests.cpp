#include "updater.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

class UpdaterTests : public QObject {
    Q_OBJECT
private slots:
    void releaseValidation() {
        const QString name = "video-player-windows-x64.exe";
        const QString url = "https://github.com/indy256/video-player/releases/download/v1.2/" + name;
        const QJsonObject valid{{"name", name.toUpper()}, {"size", 123},
            {"digest", "sha256:" + QString(64, 'a')}, {"browser_download_url", url}};
        const auto release = [](const QJsonObject &asset) {
            return QJsonObject{{"tag_name", "v1.2"}, {"assets", QJsonArray{asset}}};
        };
        AppUpdate::Asset asset;
        QVERIFY(AppUpdate::releaseAsset(release(valid), name, asset));
        QCOMPARE(asset.size, 123);
        QCOMPARE(asset.sha256, QByteArray(64, 'a'));
        QVERIFY(!AppUpdate::releaseAsset(release(valid), "video-player-windows-arm64.exe", asset));
        for (const QString &badUrl : {"http://github.com/indy256/video-player/releases/download/v1.2/app.exe",
                "https://example.com/indy256/video-player/releases/download/v1.2/app.exe",
                "https://github.com/other/repo/releases/download/v1.2/app.exe",
                "https://user@github.com/indy256/video-player/releases/download/v1.2/app.exe",
                "https://github.com:444/indy256/video-player/releases/download/v1.2/app.exe"}) {
            auto bad = valid;
            bad["browser_download_url"] = badUrl;
            QVERIFY(!AppUpdate::releaseAsset(release(bad), name, asset));
        }
        for (const QString &badHash : {QString(), QString("sha256:abcd"), "sha512:" + QString(64, 'a')}) {
            auto bad = valid;
            bad["digest"] = badHash;
            QVERIFY(!AppUpdate::releaseAsset(release(bad), name, asset));
        }
        for (qint64 size : {qint64(0), qint64(-1), 1024LL * 1024 * 1024 + 1}) {
            auto bad = valid;
            bad["size"] = size;
            QVERIFY(!AppUpdate::releaseAsset(release(bad), name, asset));
        }
        for (const auto *key : {"draft", "prerelease"}) {
            auto bad = release(valid);
            bad[key] = true;
            QVERIFY(!AppUpdate::releaseAsset(bad, name, asset));
        }
        QVERIFY(!AppUpdate::releaseAsset({}, name, asset));
    }
    void windowsInstall() {
#ifndef Q_OS_WIN
        QSKIP("Windows helper integration test");
#else
        Q_INIT_RESOURCE(updater);
        QTemporaryDir root(QDir::tempPath() + "/player update ' & [test]-XXXXXX");
        QVERIFY(root.isValid());
        const QString stage = root.filePath(".video-player-update-test");
        QVERIFY(QDir().mkdir(stage));
        const QString target = root.filePath("Video Player.exe");
        const QString source = stage + "/download";
        const QString marker = root.filePath("video ' & [1].restarted");
        QFile old(target);
        QVERIFY(old.open(QIODevice::WriteOnly));
        old.write("old application");
        old.close();
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), source));
        QVERIFY(QFile::copy(":/updates/update-windows.ps1", stage + "/install.ps1"));
        QProcess sleeper;
        sleeper.start(QCoreApplication::applicationFilePath(), {"--wait-for-update"});
        QVERIFY(sleeper.waitForStarted());
        const QJsonObject plan{{"target", target}, {"source", source}, {"video", marker}, {"pid", sleeper.processId()}};
        QFile planFile(stage + "/plan.json");
        QVERIFY(planFile.open(QIODevice::WriteOnly));
        planFile.write(QJsonDocument(plan).toJson());
        planFile.close();
        QProcess installer;
        installer.start(qEnvironmentVariable("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe",
            {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", stage + "/install.ps1",
             "-PlanPath", stage + "/plan.json"});
        QVERIFY(installer.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(stage + "/ready"), 15000);
        QVERIFY(old.open(QIODevice::ReadOnly));
        QCOMPARE(old.readAll(), QByteArray("old application"));
        old.close();
        QFile commit(stage + "/commit");
        QVERIFY(commit.open(QIODevice::WriteOnly));
        commit.close();
        QTest::qWait(200);
        QVERIFY(!QFile::exists(marker)); // Must wait for the old player to exit.
        sleeper.kill();
        QVERIFY(sleeper.waitForFinished());
        QVERIFY(installer.waitForFinished(30000));
        QCOMPARE(installer.exitCode(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(marker), 10000);
        QFile recorded(marker);
        QVERIFY(recorded.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(recorded.readAll()), marker);
        QTest::qWait(200); // Let the restarted test executable release its file handle.
#endif
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--wait-for-update") {
        QTimer::singleShot(60000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    if (app.arguments().value(1).endsWith(".restarted")) {
        QFile marker(app.arguments().at(1));
        if (!marker.open(QIODevice::WriteOnly)) return 1;
        marker.write(app.arguments().at(1).toUtf8());
        return 0;
    }
    UpdaterTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "updater_tests.moc"
