#include "playerwindow.h"
#include "seekslider.h"
#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVideoSink>
#include <QVideoFrame>

class PlayerTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir temp;
    QString clip;
private slots:
    void initTestCase() {
        QVERIFY(temp.isValid());
        clip = temp.filePath("test video.mp4");
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
            "testsrc2=size=320x180:rate=25", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
            "-t", "8", "-c:v", "mpeg4", "-g", "25", "-c:a", "aac", "-y", clip});
        QVERIFY2(ffmpeg.waitForFinished(30000), "ffmpeg must be on PATH to generate the test fixture");
        QCOMPARE(ffmpeg.exitCode(), 0);
    }
    void emptyAndMissingFile() {
        PlayerWindow window;
        QVERIFY(!window.findChild<SeekSlider *>("timeline")->isEnabled());
        QVERIFY(!window.findChild<QPushButton *>("playButton")->isEnabled());
        window.openFile(temp.filePath("missing.mp4"));
        QVERIFY(window.findChild<QLabel *>("status")->text().startsWith("Cannot open file:"));
    }
    void playbackAndSeeking() {
        PlayerWindow window;
        window.show();
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *slider = window.findChild<SeekSlider *>("timeline");
        auto *button = window.findChild<QPushButton *>("playButton");
        QSignalSpy frames(player->videoSink(), &QVideoSink::videoFrameChanged);
        window.openFile(clip);
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && player->hasVideo() && player->hasAudio(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > 2, 10000);
        QTRY_VERIFY(slider->isEnabled());
        QVERIFY(qAbs(player->duration() - 8000) < 100);
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
        const int oldFrames = frames.count();
        QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() * 3 / 4, slider->height() / 2));
        QTRY_VERIFY(qAbs(player->position() - 6000) < 250);
        QTRY_VERIFY(frames.count() > oldFrames);
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QTRY_VERIFY(qAbs(player->videoSink()->videoFrame().startTime() / 1000 - 6000) < 300);
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_VERIFY(player->isPlaying());
        QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() / 2, slider->height() / 2));
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QTest::mouseMove(slider, QPoint(slider->width() / 4, slider->height() / 2));
        QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() / 4, slider->height() / 2));
        QTRY_VERIFY(player->isPlaying());
        QVERIFY(qAbs(player->position() - 2000) < 400);
        player->setPosition(7700);
        QTRY_COMPARE_WITH_TIMEOUT(player->mediaStatus(), QMediaPlayer::EndOfMedia, 5000);
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_VERIFY(player->isPlaying());
        QVERIFY(player->position() < 2000);
    }
    void invalidMediaAndRecovery() {
        const QString invalid = temp.filePath("invalid.mp4");
        QFile file(invalid);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("This is not a video");
        file.close();
        PlayerWindow window;
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        window.openFile(invalid);
        QTRY_VERIFY_WITH_TIMEOUT(player->error() != QMediaPlayer::NoError, 10000);
        QVERIFY(!window.findChild<SeekSlider *>("timeline")->isEnabled());
        QVERIFY(window.findChild<QLabel *>("status")->text().startsWith("Unable to play:"));
        window.openFile(clip);
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && player->hasVideo(), 10000);
        QCOMPARE(player->error(), QMediaPlayer::NoError);
    }
};

int main(int argc, char **argv) {
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    PlayerTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "player_tests.moc"
