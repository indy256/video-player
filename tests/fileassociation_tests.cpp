#include "fileassociations.h"
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QXmlStreamReader>

class FileAssociationTests : public QObject {
    Q_OBJECT
private slots:
    void windowsRegistration_data() {
        QTest::addColumn<bool>("native");
        QTest::newRow("ini") << false;
#ifdef Q_OS_WIN
        QTest::newRow("isolated-user-registry") << true;
#endif
    }
    void windowsRegistration() {
        QFETCH(bool, native);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        // Never write to the real Classes or RegisteredApplications keys in tests.
        const QString location = native
            ? "HKEY_CURRENT_USER\\Software\\LocalApps\\VideoPlayerTests\\" + QUuid::createUuid().toString(QUuid::Id128)
            : temp.filePath("registration.ini");
        struct Registry : QSettings {
            using QSettings::QSettings;
            ~Registry() { clear(); sync(); }
        } registry(location, native ? QSettings::NativeFormat : QSettings::IniFormat);
        registry.setValue("Classes/.mp4/Default", "OtherPlayer.Video");
        registry.setValue("Classes/.mp4/OpenWithProgids/OtherPlayer.Video", QString());
        registry.setValue("Microsoft/Windows/CurrentVersion/Explorer/FileExts/.mp4/UserChoice/ProgId", "OtherPlayer.Video");
        for (const QString &path : {QString("C:/Videos & tools/Video Player.exe"),
                                   QString::fromUtf8("C:/播放器/vp-windows-arm64.exe")}) {
            QCOMPARE(FileAssociations::writeWindowsRegistration(registry, path), QString());
            const QString nativePath = QDir::toNativeSeparators(path);
            QCOMPARE(registry.value("Classes/VideoPlayer.Video/shell/open/command/Default").toString(),
                     QString("\"%1\" \"%2\"").arg(nativePath, "%1"));
            QCOMPARE(registry.value("Classes/VideoPlayer.Video/DefaultIcon/Default").toString(),
                     QString("\"%1\",0").arg(nativePath));
            QCOMPARE(registry.value("RegisteredApplications/Video Player").toString(),
                     QString("Software\\VideoPlayer\\Capabilities"));
            for (const QString &extension : FileAssociations::extensions()) {
                QVERIFY(registry.contains("Classes/." + extension + "/OpenWithProgids/VideoPlayer.Video"));
                QCOMPARE(registry.value("VideoPlayer/Capabilities/FileAssociations/." + extension).toString(),
                         QString("VideoPlayer.Video"));
            }
        }
        QCOMPARE(registry.value("Classes/.mp4/Default").toString(), QString("OtherPlayer.Video"));
        QVERIFY(registry.contains("Classes/.mp4/OpenWithProgids/OtherPlayer.Video"));
        QCOMPARE(registry.value("Microsoft/Windows/CurrentVersion/Explorer/FileExts/.mp4/UserChoice/ProgId").toString(),
                 QString("OtherPlayer.Video"));
    }
    void portableExecutable() {
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
#ifdef Q_OS_WIN
        const char *name = "VIDEO_PLAYER_LAUNCHER";
#else
        const char *name = "APPIMAGE";
#endif
        struct Environment {
            const char *name;
            QByteArray previous;
            ~Environment() { if (previous.isNull()) qunsetenv(name); else qputenv(name, previous); }
        } environment{name, qgetenv(name)};
        QTemporaryDir temp;
        const QString portable = temp.filePath("portable player.exe");
        qputenv(name, portable.toUtf8());
        QCOMPARE(FileAssociations::executablePath(), portable);
        qunsetenv(name);
        QCOMPARE(FileAssociations::executablePath(), QCoreApplication::applicationFilePath());
#endif
    }
    void desktopEntryQuoting() {
        const QByteArray entry = FileAssociations::desktopEntry("/home/user/Video Player 100%/$`\"\\\n\r\t.AppImage");
        const QByteArray expected = "Exec=\"/home/user/Video Player 100%%/\\\\$\\\\`\\\\\"\\\\\\\\\\n\\r\\t.AppImage\" %f";
        const auto lines = entry.split('\n');
        QCOMPARE(lines.at(4), expected);
        QFile packaged(QFINDTESTDATA("../packaging/video-player.desktop"));
        QVERIFY(packaged.open(QIODevice::ReadOnly));
        QCOMPARE(entry, packaged.readAll().replace("\r\n", "\n").replace("Exec=VideoPlayer %f", expected));
    }
    void macDocumentTypes() {
        QFile plist(QFINDTESTDATA("../packaging/Info.plist.in"));
        QVERIFY(plist.open(QIODevice::ReadOnly));
        QXmlStreamReader xml(&plist);
        QStringList extensions;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement() || xml.name() != "key") continue;
            if (xml.readElementText() != "CFBundleTypeExtensions") continue;
            QVERIFY(xml.readNextStartElement());
            QCOMPARE(xml.name(), QString("array"));
            while (xml.readNextStartElement()) extensions.append(xml.readElementText());
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QCOMPARE(extensions, FileAssociations::extensions());
    }
};

QTEST_GUILESS_MAIN(FileAssociationTests)
#include "fileassociation_tests.moc"
