#include "singleinstance.h"
#include <QCoreApplication>
#include <QProcess>
#include <QScopeGuard>
#include <QTest>
#include <QTextStream>
#include <QUuid>

class InstanceTests : public QObject {
    Q_OBJECT
private slots:
    void forwardsAndRecovers() {
        const QString name = "VideoPlayerTest." + QUuid::createUuid().toString(QUuid::Id128);
        QProcess primary;
        const auto cleanup = qScopeGuard([&] { primary.kill(); primary.waitForFinished(3000); });
        const auto startPrimary = [&] {
            primary.start(QCoreApplication::applicationFilePath(), {"--helper", name, ""});
            return primary.waitForStarted() && primary.waitForReadyRead(5000)
                && primary.readLine().trimmed() == "READY";
        };
        QVERIFY(startPrimary());
        const QStringList paths{QString::fromUtf8("C:/Videos/фильм with spaces.mp4"), "C:/Videos/second.mkv", ""};
        for (const QString &path : paths) {
            QProcess secondary;
            secondary.start(QCoreApplication::applicationFilePath(), {"--helper", name, path});
            QVERIFY(secondary.waitForFinished(7000));
            QCOMPARE(secondary.exitCode(), 0);
            QCOMPARE(secondary.readAllStandardOutput().trimmed(), QByteArray("FORWARDED"));
            if (!primary.canReadLine()) QVERIFY(primary.waitForReadyRead(3000));
            QCOMPARE(primary.readLine().trimmed(), QByteArray("FILE:") + path.toUtf8().toBase64());
            QCOMPARE(primary.state(), QProcess::Running);
        }
        primary.kill();
        QVERIFY(primary.waitForFinished(3000));
        // A dead process's lock must not prevent the next launch from listening.
        QVERIFY(startPrimary());
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--helper") {
        SingleInstance instance(app.arguments().value(2));
        QObject::connect(&instance, &SingleInstance::openRequested, &app, [](const QString &file) {
            QTextStream(stdout) << "FILE:" << file.toUtf8().toBase64() << Qt::endl;
        });
        const auto result = instance.start(app.arguments().value(3));
        if (result == SingleInstance::Result::Forwarded) {
            QTextStream(stdout) << "FORWARDED" << Qt::endl;
            return 0;
        }
        if (result == SingleInstance::Result::Failed) return 1;
        QTextStream(stdout) << "READY" << Qt::endl;
        return app.exec();
    }
    InstanceTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "instance_tests.moc"
