#include "playerwindow.h"
#include "seekslider.h"
#include "gammafilter.h"
#include "externalaudio.h"
#include "embeddedsubtitles.h"
#include "folderpanel.h"
#include <QApplication>
#include <QAbstractButton>
#include <QFile>
#include <QFileOpenEvent>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QSignalSpy>
#include <QSettings>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QVideoSink>
#include <QVideoFrame>
#include <QVideoWidget>
#include <QStackedWidget>
#include <QAudioOutput>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAudioBufferOutput>
#include <QAudioBuffer>
#endif
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QMenu>
#include <QKeySequence>
#include <QMediaMetaData>
#include <QDir>
#include <QPainter>
#include <QTimer>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
struct AudioSamples {
    int count = 0;
    int gaps = 0;
    float peak = 0;
    qint64 end = -1;

    void add(const QAudioBuffer &buffer) {
        if (!buffer.isValid()) return;
        ++count;
        if (end >= 0 && qAbs(buffer.startTime() - end) > 2000) ++gaps;
        end = buffer.startTime() + buffer.duration();
        if (buffer.format().sampleFormat() == QAudioFormat::Float)
            for (int i = 0; i < buffer.sampleCount(); ++i)
                peak = qMax(peak, qAbs(buffer.constData<float>()[i]));
    }
};
#endif

class DiagnosticApplication : public QApplication {
public:
    using QApplication::QApplication;
    qint64 videoEventNs = 0;
    int videoEvents = 0;
    bool measuring = false;
    bool notify(QObject *receiver, QEvent *event) override {
        if (!measuring || QByteArray(receiver->metaObject()->className()) != "QVideoWindow")
            return QApplication::notify(receiver, event);
        QElapsedTimer timer;
        timer.start();
        const bool result = QApplication::notify(receiver, event);
        videoEventNs += timer.nsecsElapsed();
        ++videoEvents;
        return result;
    }
};

class DiagnosticWindow : public PlayerWindow {
public:
    bool skipErase = false;
    qint64 eraseNs = 0;
    int eraseCount = 0;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override {
        if (static_cast<MSG *>(message)->message != WM_ERASEBKGND)
            return PlayerWindow::nativeEvent(type, message, result);
        ++eraseCount;
        if (skipErase) { *result = 1; return true; }
        QElapsedTimer timer;
        timer.start();
        const bool handled = PlayerWindow::nativeEvent(type, message, result);
        eraseNs += timer.nsecsElapsed();
        return handled;
    }
#endif
};

class PlayerTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir temp;
    QString clip;
    template <typename Callback>
    bool withPopupMenu(QWidget &surface, Callback callback) {
        bool opened = false;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, &surface, [&] {
            auto *menu = surface.window()->findChild<QMenu *>("playerMenu");
            if (!menu || !menu->isVisible()) return;
            opened = true;
            callback(*menu);
            menu->close();
        });
        timer.start(0);
        QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(10, 10), surface.mapToGlobal(QPoint(10, 10)));
        QApplication::sendEvent(&surface, &event);
        return opened;
    }
    template <typename Callback>
    bool withTrackMenu(PlayerWindow &window, Qt::Key key, Callback callback) {
        window.activateWindow();
        if (!QTest::qWaitForWindowActive(&window)) return false;
        bool opened = false;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, &window, [&] {
            auto *menu = window.findChild<QMenu *>(key == Qt::Key_S ? "subtitleMenu" : "audioTrackMenu");
            if (!menu) return;
            opened = true;
            callback(*menu);
            menu->close();
        });
        timer.start(0);
        QTest::keyClick(&window, key, Qt::ShiftModifier);
        return opened;
    }
    bool selectTrack(PlayerWindow &window, const QVariant &choice, Qt::Key key = Qt::Key_S, bool mouse = false) {
        bool found = false;
        return withTrackMenu(window, key, [&](QMenu &menu) {
            for (auto *action : menu.actions()) {
                if (!action->isCheckable() || action->data() != choice) continue;
                found = true;
                if (mouse) {
                    const QRect row = menu.actionGeometry(action);
                    QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier, QPoint(row.right() - 16, row.center().y()));
                } else {
                    menu.setActiveAction(action);
                    QTest::keyClick(&menu, Qt::Key_Return);
                }
                return;
            }
        }) && found;
    }
    bool selectSubtitle(PlayerWindow &window, const QVariant &choice) {
        return selectTrack(window, choice);
    }
    bool toggleTrackCheck(PlayerWindow &window, const QVariant &choice, Qt::Key key, bool keyboard = false) {
        bool toggled = false;
        return withTrackMenu(window, key, [&](QMenu &menu) {
            for (auto *action : menu.actions()) {
                if (!action->isCheckable() || action->data() != choice) continue;
                const bool checked = action->isChecked();
                if (keyboard) {
                    menu.setActiveAction(action);
                    QTest::keyClick(&menu, Qt::Key_Space);
                } else {
                    const QRect row = menu.actionGeometry(action);
                    QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier, QPoint(row.left() + 10, row.center().y()));
                }
                toggled = menu.isVisible() && action->isChecked() != checked;
                break;
            }
        }) && toggled;
    }
    bool cycleTrack(PlayerWindow &window, Qt::Key key) {
        window.activateWindow();
        if (!QTest::qWaitForWindowActive(&window)) return false;
        bool popup = false;
        QTimer guard;
        guard.setSingleShot(true);
        connect(&guard, &QTimer::timeout, &window, [&] {
            for (auto *menu : window.findChildren<QMenu *>()) {
                if (!menu->isVisible()) continue;
                popup = true;
                menu->close();
            }
        });
        guard.start(0);
        QTest::keyClick(&window, key);
        return !popup;
    }
private slots:
    void initTestCase() {
        QVERIFY(temp.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
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
        window.openFile(temp.filePath("missing.mp4"));
        QVERIFY(window.findChild<QLabel *>("status")->text().startsWith("Cannot open file:"));
    }
    void popupMenu() {
        PlayerWindow window;
        window.show();
        for (QWidget *surface : {static_cast<QWidget *>(&window),
                window.findChild<QWidget *>("emptyStage"),
                static_cast<QWidget *>(window.findChild<QVideoWidget *>())}) {
            QVERIFY(withPopupMenu(*surface, [&](QMenu &menu) {
                QVERIFY(menu.findChild<QAction *>("update")->isEnabled());
                QVERIFY(menu.findChild<QAction *>("registerFileTypes")->isEnabled());
                for (const auto &name : {"playPause", "back3", "forward30", "audioTracks", "subtitles"})
                    QVERIFY(!menu.findChild<QAction *>(name)->isEnabled());
                QCOMPARE(menu.findChild<QAction *>("nextAudio")->text().section('\t', 1), QString("A"));
                QCOMPARE(menu.findChild<QAction *>("audioTracks")->text().section('\t', 1),
                    QKeySequence(Qt::SHIFT | Qt::Key_A).toString(QKeySequence::NativeText));
                QCOMPARE(menu.findChild<QAction *>("nextSubtitles")->text().section('\t', 1), QString("S"));
                QCOMPARE(menu.findChild<QAction *>("subtitles")->text().section('\t', 1),
                    QKeySequence(Qt::SHIFT | Qt::Key_S).toString(QKeySequence::NativeText));
                QTest::keyClick(&menu, Qt::Key_Escape);
            }));
            QVERIFY(window.isVisible());
        }
    }
    void nativeFileOpen() {
        PlayerWindow window;
        window.showMinimized();
        QFileOpenEvent event(QUrl::fromLocalFile(clip));
        QVERIFY(QApplication::sendEvent(qApp, &event));
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        QCOMPARE(player->source(), QUrl::fromLocalFile(clip));
        QVERIFY(window.isVisible());
        QVERIFY(!window.isMinimized());
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
        QSignalSpy sources(player, &QMediaPlayer::sourceChanged);
        QFileOpenEvent remote(QUrl("https://example.com/video.mp4"));
        QApplication::sendEvent(qApp, &remote);
        QObject unrelated;
        QFileOpenEvent other(temp.filePath("other.mp4"));
        QApplication::sendEvent(&unrelated, &other);
        window.openAndActivate({});
        QVERIFY(sources.isEmpty());
        QFileOpenEvent local(clip);
        QVERIFY(QApplication::sendEvent(qApp, &local));
        QCOMPARE(player->source(), QUrl::fromLocalFile(clip));
    }
    void folderFiles() {
        const QString folder = temp.filePath("folder browser");
        QVERIFY(QDir().mkpath(folder));
        const QString first = folder + "/a first.mp4";
        const QString second = folder + "/b second.MP4";
        QVERIFY(QFile::copy(clip, first));
        QVERIFY(QFile::copy(clip, second));
        QFile other(folder + "/notes.txt");
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.close();
        PlayerWindow window;
        window.resize(900, 600);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        window.openFile(first);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *panel = window.findChild<FolderPanel *>();
        auto *files = window.findChild<QListWidget *>("folderFiles");
        auto *video = window.findChild<QVideoWidget *>();
        QVERIFY(panel->isHidden());
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && video->isVisible(), 10000);
        player->pause();
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QTest::keyClick(&window, Qt::Key_L);
        QVERIFY(panel->isVisible());
        QCOMPARE(files->count(), 2);
        QCOMPARE(files->item(0)->text(), QString("a first.mp4"));
        QCOMPARE(files->item(1)->text(), QString("b second.MP4"));
        QCOMPARE(files->currentRow(), 0);
        auto *volume = window.findChild<QSlider *>("volume");
        volume->setValue(50);
        for (QWidget *surface : {files->viewport(), static_cast<QWidget *>(panel)}) {
            for (int delta : {-120, 120}) {
                QWheelEvent wheel(QPointF(10, 10), surface->mapToGlobal(QPoint(10, 10)), {},
                    QPoint(0, delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                QApplication::sendEvent(surface, &wheel);
                QCOMPARE(volume->value(), 50);
            }
        }
        const auto firstIcon = files->item(0)->icon().cacheKey();
        const auto secondIcon = files->item(1)->icon().cacheKey();
        QTRY_VERIFY(panel->mapToGlobal(QPoint()).x() >= video->mapToGlobal(QPoint(video->width(), 0)).x());
        QTRY_VERIFY_WITH_TIMEOUT(files->item(0)->icon().cacheKey() != firstIcon, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(files->item(1)->icon().cacheKey() != secondIcon, 10000);
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QCOMPARE(player->source(), QUrl::fromLocalFile(first));
        QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, files->visualItemRect(files->item(1)).center());
        QCOMPARE(player->source(), QUrl::fromLocalFile(second));
        QCOMPARE(files->currentRow(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && video->isVisible(), 10000);
        QTest::keyClick(&window, Qt::Key_L);
        QVERIFY(panel->isHidden());
        window.showFullScreen();
        QTest::keyClick(&window, Qt::Key_L);
        QVERIFY(panel->isVisible());
        QCOMPARE(files->currentRow(), 1);
        QTRY_VERIFY(panel->mapToGlobal(QPoint()).x() >= video->mapToGlobal(QPoint(video->width(), 0)).x());
        window.openFile(clip); // Changing folders refreshes the panel.
        QCOMPARE(files->currentItem()->data(Qt::UserRole).toString(), clip);
        QTest::keyClick(&window, Qt::Key_L); // Cancel pending thumbnail work.
        QVERIFY(panel->isHidden());
    }
    void popupCommands() {
        PlayerWindow window;
        window.show();
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        QTRY_VERIFY(player->isPlaying() && window.findChild<SeekSlider *>("timeline")->isEnabled());
        auto activate = [&](const QString &name) {
            bool clicked = false;
            return withPopupMenu(window, [&](QMenu &menu) {
                auto *action = menu.findChild<QAction *>(name);
                if (!action || !action->isEnabled()) return;
                clicked = true;
                QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier, menu.actionGeometry(action).center());
            }) && clicked;
        };
        QVERIFY(withPopupMenu(window, [&](QMenu &menu) {
            QCOMPARE(menu.findChild<QAction *>("playPause")->text().section('\t', 0, 0), QString("Pause"));
            const QString capture = qEnvironmentVariable("VIDEO_PLAYER_COMMAND_MENU_CAPTURE");
            if (!capture.isEmpty()) QVERIFY(menu.grab().save(capture));
        }));
        QVERIFY(activate("playPause"));
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QVERIFY(withPopupMenu(window, [&](QMenu &menu) {
            QCOMPARE(menu.findChild<QAction *>("playPause")->text().section('\t', 0, 0), QString("Play"));
        }));
        player->setPosition(1000);
        QVERIFY(activate("forward3"));
        QCOMPARE(player->position(), qint64(4000));
        QVERIFY(activate("back3"));
        QCOMPARE(player->position(), qint64(1000));
        QVERIFY(activate("forward30"));
        QCOMPARE(player->position(), player->duration());
        QVERIFY(activate("back30"));
        QCOMPARE(player->position(), qint64(0));
        auto *volume = window.findChild<QSlider *>("volume");
        volume->setValue(70);
        QVERIFY(activate("volumeUp"));
        QCOMPARE(volume->value(), 75);
        QVERIFY(activate("volumeDown"));
        QCOMPARE(volume->value(), 70);
        auto *gamma = window.findChild<QSlider *>("gamma");
        gamma->setValue(16);
        QVERIFY(activate("resetGamma"));
        QCOMPARE(gamma->value(), 10);
        QVERIFY(activate("fullscreen"));
        QVERIFY(window.isFullScreen());
        QVERIFY(withPopupMenu(window, [&](QMenu &menu) {
            QVERIFY(menu.findChild<QAction *>("fullscreen")->text().startsWith("Leave fullscreen\t"));
        }));
        QVERIFY(activate("fullscreen"));
        QVERIFY(!window.isFullScreen());
        for (const auto &name : {"audioTracks", "subtitles"}) {
            bool trackMenuOpened = false;
            QVERIFY(withPopupMenu(window, [&](QMenu &menu) {
                QTimer::singleShot(0, &window, [&] {
                    for (auto *popup : window.findChildren<QMenu *>()) {
                        if (popup == &menu || !popup->isVisible()) continue;
                        trackMenuOpened = true;
                        QTest::keyClick(popup, Qt::Key_Escape);
                    }
                });
                auto *action = menu.findChild<QAction *>(name);
                QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier, menu.actionGeometry(action).center());
            }));
            QVERIFY(trackMenuOpened);
        }
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QVERIFY(activate("exit"));
        QVERIFY(!window.isVisible());
    }
    void externalAudioSelection() {
        const QString directory = temp.filePath("external audio");
        QVERIFY(QDir().mkpath(directory));
        const QString movie = directory + "/movie.test.mp4";
        const QString track = directory + "/movie.test.ENG.AC3";
        QVERIFY(QFile::copy(clip, movie));
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-i", clip,
            "-vn", "-c:a", "ac3", "-t", "5", "-y", track});
        QVERIFY(ffmpeg.waitForFinished(30000));
        QCOMPARE(ffmpeg.exitCode(), 0);
        for (const QString &name : {"other.ac3", "movie.test.srt", "movie.test.txt"}) {
            QFile file(directory + "/" + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QCOMPARE(ExternalAudio::matchingFiles(QUrl::fromLocalFile(movie)), QStringList{track});
        PlayerWindow window;
        window.show();
        window.openFile(movie);
        auto *video = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *audio = window.findChild<QMediaPlayer *>("externalAudioPlayer");
        auto *external = window.findChild<ExternalAudio *>();
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QAudioBufferOutput buffers;
        audio->setAudioBufferOutput(&buffers);
        AudioSamples samples;
        connect(&buffers, &QAudioBufferOutput::audioBufferReceived, &buffers,
            [&](const QAudioBuffer &buffer) { samples.add(buffer); });
#endif
        QTRY_VERIFY(video->isSeekable());
        video->pause();
        external->seek(2000);
        QSignalSpy trackChanges(video, &QMediaPlayer::activeTracksChanged);
        QSignalSpy rateChanges(audio, &QMediaPlayer::playbackRateChanged);
        QVERIFY(selectTrack(window, track, Qt::Key_A));
        QTRY_COMPARE(audio->playbackState(), QMediaPlayer::PausedState);
        QCOMPARE(video->activeAudioTrack(), 0);
        QVERIFY(!video->audioOutput()->isMuted());
        QVERIFY(audio->audioOutput()->isMuted());
        QVERIFY(trackChanges.isEmpty());
        QCOMPARE(audio->position(), video->position());
        QCOMPARE(external->filePath(), track);
        external->seek(1000);
        QTRY_COMPARE(audio->position(), qint64(1000));
        window.findChild<QSlider *>("volume")->setValue(23);
        QCOMPARE(audio->audioOutput()->volume(), 0.23f);
        video->play();
        QTRY_VERIFY(audio->isPlaying());
        QTRY_VERIFY(video->audioOutput()->isMuted());
        QVERIFY(!audio->audioOutput()->isMuted());
        QVERIFY(trackChanges.isEmpty());
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QTRY_VERIFY(samples.count >= 5);
#endif
        QTest::qWait(600);
        // Device startup and position notifications settle asynchronously on CI.
        // Keep the drift limit, but do not require convergence on one exact tick.
        QTRY_VERIFY2_WITH_TIMEOUT(video->isPlaying() && audio->isPlaying()
            && qAbs(audio->position() - video->position()) < 250,
            qPrintable(QString("Audio position %1 ms, video position %2 ms")
                .arg(audio->position()).arg(video->position())), 2000);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        // Give the independent drift test a full interval before this short track ends.
        external->seek(1000);
        QTRY_VERIFY(qAbs(audio->position() - video->position()) < 250);
        // Introduce clock drift. Correcting it must not skip decoded audio.
        audio->setPosition(video->position() - 400);
        QTest::qWait(100);
        samples = {};
        QTest::qWait(1500);
        QVERIFY(samples.count > 0);
        QCOMPARE(samples.gaps, 0);
#endif
        QVERIFY(rateChanges.isEmpty());
        external->seek(3500);
        QTRY_VERIFY(qAbs(audio->position() - video->position()) < 250);
        video->pause();
        QTRY_COMPARE(audio->playbackState(), QMediaPlayer::PausedState);
        QTRY_COMPARE(audio->position(), video->position());
        auto *timeline = window.findChild<SeekSlider *>("timeline");
        timeline->setValue(timeline->maximum() / 4);
        QTRY_COMPARE(audio->position(), video->duration() / 4);
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::keyClick(&window, Qt::Key_Left);
        QTRY_COMPARE(video->position(), qint64(0));
        QTRY_COMPARE(audio->position(), qint64(0));
        external->seek(6000);
        video->play();
        QTest::qWait(200);
        QVERIFY(!audio->isPlaying());
        external->seek(1000);
        QTRY_VERIFY(audio->isPlaying());
        QVERIFY(selectTrack(window, 0, Qt::Key_A));
        QCOMPARE(video->activeAudioTrack(), 0);
        QVERIFY(!video->audioOutput()->isMuted());
        QVERIFY(audio->audioOutput()->isMuted());
        QCOMPARE(video->playbackRate(), qreal(1));
        QVERIFY(audio->source().isEmpty());
        QVERIFY(external->filePath().isEmpty());
        QVERIFY(selectTrack(window, track, Qt::Key_A));
        QTRY_VERIFY(audio->isPlaying());
        window.openFile(clip);
        QVERIFY(audio->source().isEmpty());
        QVERIFY(external->filePath().isEmpty());
        QTRY_VERIFY(video->isPlaying());
        QTest::qWait(250);
        QVERIFY(!video->audioOutput()->isMuted());
        window.close();
        QVERIFY(!audio->isPlaying());
    }
    void externalAudioFailure() {
        QMediaPlayer video;
        QAudioOutput output;
        video.setAudioOutput(&output);
        ExternalAudio external(&video);
        QSignalSpy errors(&external, &ExternalAudio::failed);
        video.setSource(QUrl::fromLocalFile(clip));
        QTRY_VERIFY(!video.audioTracks().isEmpty());
        const int track = video.activeAudioTrack();
        external.select(temp.filePath("missing.ac3"));
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(external.filePath().isEmpty());
        QCOMPARE(video.activeAudioTrack(), track);
    }
    void externalAudioExample() {
        const QString path = qEnvironmentVariable("VIDEO_PLAYER_AUDIO_EXAMPLE");
        if (path.isEmpty()) QSKIP("Set VIDEO_PLAYER_AUDIO_EXAMPLE to a video with matching external audio");
        const auto files = ExternalAudio::matchingFiles(QUrl::fromLocalFile(path));
        QVERIFY(!files.isEmpty());
        PlayerWindow window;
        window.show();
        window.openFile(path);
        auto *video = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *audio = window.findChild<QMediaPlayer *>("externalAudioPlayer");
        auto *external = window.findChild<ExternalAudio *>();
        QSignalSpy rateChanges(audio, &QMediaPlayer::playbackRateChanged);
        QSignalSpy trackChanges(video, &QMediaPlayer::activeTracksChanged);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QAudioBufferOutput buffers;
        const bool probe = !qEnvironmentVariableIsSet("VIDEO_PLAYER_AUDIO_NO_PROBE");
        if (probe) audio->setAudioBufferOutput(&buffers);
        AudioSamples samples;
        connect(&buffers, &QAudioBufferOutput::audioBufferReceived, &buffers,
            [&](const QAudioBuffer &buffer) { samples.add(buffer); });
#endif
        QTRY_VERIFY_WITH_TIMEOUT(video->isSeekable(), 15000);
        window.findChild<QSlider *>("gamma")->setValue(16);
        window.findChild<QSlider *>("volume")->setValue(35);
        external->seek(3840000);
        rateChanges.clear();
        trackChanges.clear();
        QVERIFY(selectTrack(window, files.first(), Qt::Key_A));
        QTRY_COMPARE_WITH_TIMEOUT(audio->playbackState(), QMediaPlayer::PlayingState, 15000);
        QCOMPARE(video->activeAudioTrack(), 0);
        QTRY_VERIFY(audio->isPlaying());
        QTRY_VERIFY(video->audioOutput()->isMuted());
        QVERIFY(!audio->audioOutput()->isMuted());
        QTest::qWait(11000);
        QVERIFY(rateChanges.isEmpty());
        QVERIFY(trackChanges.isEmpty());
        QVERIFY(qAbs(audio->position() - video->position()) < 250);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QCOMPARE(samples.gaps, 0);
        QVERIFY2(!probe || (samples.count > 100 && samples.peak > 0.001f),
            "External audio must deliver non-silent samples continuously after selecting at 64 minutes");
        samples = {};
#endif
        external->seek(1800000);
        QTest::qWait(1500);
        QVERIFY(qAbs(audio->position() - video->position()) < 250);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QVERIFY2(!probe || (samples.count > 20 && samples.peak > 0.001f),
            "External audio must deliver non-silent samples after seeking to 30 minutes");
#endif
        QVERIFY(selectTrack(window, 0, Qt::Key_A));
        QVERIFY(audio->source().isEmpty());
        window.close();
    }
    void audioTrackSelection() {
        const QString multiAudio = temp.filePath("two audio tracks.mkv");
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-i", clip,
            "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000",
            "-map", "0:v", "-map", "0:a", "-map", "1:a", "-c:v", "copy", "-c:a", "aac",
            "-metadata:s:a:0", "language=eng", "-metadata:s:a:0", "title=Original",
            "-metadata:s:a:1", "language=spa", "-metadata:s:a:1", "title=Commentary",
            "-t", "8", "-y", multiAudio});
        QVERIFY(ffmpeg.waitForFinished(30000));
        QCOMPARE(ffmpeg.exitCode(), 0);
        PlayerWindow window;
        window.show();
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        bool emptyMessage = false;
        QVERIFY(withTrackMenu(window, Qt::Key_A, [&](QMenu &menu) {
            for (auto *action : menu.actions())
                if (action->text() == "No audio tracks available") emptyMessage = !action->isEnabled();
        }));
        QVERIFY(emptyMessage);
        window.openFile(multiAudio);
        QTRY_COMPARE_WITH_TIMEOUT(player->audioTracks().size(), 2, 10000);
        QTRY_VERIFY(player->isPlaying());
        player->pause();
        QTest::qWait(100);
        const qint64 position = player->position();
        for (int chosen : {1, 0, -1}) {
            if (chosen == 0) {
                window.showFullScreen();
                QTest::qWait(100);
            }
            const int previous = player->activeAudioTrack();
            int count = 0;
            int checked = 0;
            int current = -1;
            bool named = false;
            QVERIFY(withTrackMenu(window, Qt::Key_A, [&](QMenu &menu) {
                QAction *selection = nullptr;
                for (auto *action : menu.actions()) {
                    if (!action->data().isValid()) continue;
                    ++count;
                    if (action->isChecked()) ++checked;
                    if (action->font().bold()) current = action->data().toInt();
                    if (action->data().toInt() == 1) named = action->text().contains("Commentary");
                    if (action->data().toInt() == chosen) selection = action;
                }
                if (selection) {
                    menu.setActiveAction(selection);
                    QTest::keyClick(&menu, Qt::Key_Return);
                } else QTest::keyClick(&menu, Qt::Key_Escape);
            }));
            QCOMPARE(count, 2);
            QCOMPARE(checked, 2);
            QCOMPARE(current, previous);
            QVERIFY(named);
            QCOMPARE(player->activeAudioTrack(), chosen < 0 ? previous : chosen);
            QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
            QVERIFY(qAbs(player->position() - position) < 100);
            QVERIFY(window.isVisible());
        }
        player->play();
        QTRY_VERIFY(player->position() > position + 300);
    }
    void trackCycling() {
        const QString directory = temp.filePath("track cycling");
        QVERIFY(QDir().mkpath(directory));
        const QString first = directory + "/first.srt";
        const QString second = directory + "/second.srt";
        for (const auto &path : {first, second}) {
            QFile srt(path);
            QVERIFY(srt.open(QIODevice::WriteOnly));
            srt.write("1\n00:00:00,000 --> 00:00:08,000\n" + QFileInfo(path).baseName().toUtf8() + "\n");
        }
        const QString movie = directory + "/movie.mkv";
        const QString audio = directory + "/movie.ENG.ac3";
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-i", clip,
            "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000", "-i", first,
            "-map", "0:v", "-map", "0:a", "-map", "1:a", "-map", "2:0",
            "-c:v", "copy", "-c:a", "aac", "-c:s", "srt", "-t", "8", "-y", movie,
            "-map", "0:a", "-c:a", "ac3", "-y", audio});
        QVERIFY(ffmpeg.waitForFinished(30000));
        QCOMPARE(ffmpeg.exitCode(), 0);
        PlayerWindow window;
        window.show();
        QVERIFY(cycleTrack(window, Qt::Key_A));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        window.openFile(movie);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *external = window.findChild<ExternalAudio *>();
        auto *audioPlayer = window.findChild<QMediaPlayer *>("externalAudioPlayer");
        auto *sink = player->videoSink();
        QTRY_COMPARE_WITH_TIMEOUT(player->audioTracks().size(), 2, 10000);
        QTRY_COMPARE(player->subtitleTracks().size(), 1);
        QTRY_VERIFY(player->isPlaying() && sink->videoFrame().isValid());
        player->pause();
        external->seek(1500);
        QTRY_VERIFY(qAbs(sink->videoFrame().startTime() / 1000 - 1500) < 100);
        const qint64 position = player->position();

        QVERIFY(selectTrack(window, 0, Qt::Key_A));
        QVERIFY(cycleTrack(window, Qt::Key_A));
        QCOMPARE(player->activeAudioTrack(), 1);
        QVERIFY(cycleTrack(window, Qt::Key_A));
        QCOMPARE(external->filePath(), audio);
        QTRY_COMPARE(audioPlayer->playbackState(), QMediaPlayer::PausedState);
        QVERIFY(cycleTrack(window, Qt::Key_A));
        QVERIFY(external->filePath().isEmpty());
        QCOMPARE(player->activeAudioTrack(), 0);

        // Checkbox clicks and Space leave the popup open and playback untouched.
        QVERIFY(toggleTrackCheck(window, 1, Qt::Key_A));
        QVERIFY(toggleTrackCheck(window, audio, Qt::Key_A, true));
        QCOMPARE(player->activeAudioTrack(), 0);
        QVERIFY(external->filePath().isEmpty());
        QVERIFY(cycleTrack(window, Qt::Key_A)); // Only the current track is checked.
        QCOMPARE(player->activeAudioTrack(), 0);
        QVERIFY(toggleTrackCheck(window, 0, Qt::Key_A));
        QVERIFY(cycleTrack(window, Qt::Key_A)); // No checked tracks.
        QCOMPARE(player->activeAudioTrack(), 0);
        QVERIFY(toggleTrackCheck(window, 1, Qt::Key_A));
        QVERIFY(cycleTrack(window, Qt::Key_A));
        QCOMPARE(player->activeAudioTrack(), 1);
        QVERIFY(selectTrack(window, 0, Qt::Key_A, true)); // Unchecked tracks can still be selected directly.
        QCOMPARE(player->activeAudioTrack(), 0);
        QVERIFY(cycleTrack(window, Qt::Key_A));
        QCOMPARE(player->activeAudioTrack(), 1);
        QVERIFY(cycleTrack(window, Qt::Key_A)); // Selecting the unchecked label did not re-enable it.
        QCOMPARE(player->activeAudioTrack(), 1);

        window.showFullScreen();
        QSignalSpy subtitleChanges(player, &QMediaPlayer::activeTracksChanged);
        QVERIFY(selectSubtitle(window, -1));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QCOMPARE(window.findChild<EmbeddedSubtitles *>()->activeTrack(), 0);
        QCOMPARE(player->activeSubtitleTrack(), -1);
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QCOMPARE(player->activeSubtitleTrack(), -1);
        QTRY_COMPARE(sink->subtitleText(), QString("first"));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QTRY_COMPARE(sink->subtitleText(), QString("second"));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QCOMPARE(player->activeSubtitleTrack(), -1);
        QVERIFY(sink->subtitleText().isEmpty());
        QVERIFY(toggleTrackCheck(window, 0, Qt::Key_S));
        QVERIFY(toggleTrackCheck(window, first, Qt::Key_S));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QCOMPARE(sink->subtitleText(), QString("second"));
        QVERIFY(toggleTrackCheck(window, -1, Qt::Key_S, true));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QCOMPARE(sink->subtitleText(), QString("second"));
        QVERIFY(toggleTrackCheck(window, second, Qt::Key_S));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QCOMPARE(sink->subtitleText(), QString("second"));
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QVERIFY(qAbs(player->position() - position) < 100);
        QVERIFY(subtitleChanges.isEmpty());

        // A new video starts with every item checked again, including Off.
        window.openFile(clip);
        QTRY_COMPARE(player->audioTracks().size(), 1);
        window.openFile(movie);
        QTRY_COMPARE(player->audioTracks().size(), 2);
        QTRY_VERIFY(player->isPlaying());
        player->pause();
        for (auto key : {Qt::Key_A, Qt::Key_S}) {
            int checked = 0;
            QVERIFY(withTrackMenu(window, key, [&](QMenu &menu) {
                for (auto *action : menu.actions())
                    if (action->isCheckable() && action->isChecked()) ++checked;
                const QString capture = qEnvironmentVariable("VIDEO_PLAYER_TRACK_MENU_CAPTURE");
                if (!capture.isEmpty()) menu.grab().save(capture + (key == Qt::Key_S ? "-subtitles.png" : "-audio.png"));
            }));
            QCOMPARE(checked, key == Qt::Key_S ? 4 : 3);
        }
    }
    void subtitleParsing() {
        const QString path = temp.filePath("parser.srt");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("\xEF\xBB\xBF"
            "1\r\n00:00:01,000 --> 00:00:03,000\r\n<i>First</i> &amp; second\r\n<<literal>>\r\n\r\n"
            "2\n00:00:02.000 --> 00:00:04.000\nOverlap\n\n"
            "3\n00:00:05,000 --> 00:00:04,000\nInvalid duration\n");
        file.close();
        SubtitleTrack track;
        QString error;
        QVERIFY2(track.load(path, error), qPrintable(error));
        QCOMPARE(track.textAt(999), QString());
        QCOMPARE(track.textAt(1000), QString("First & second\n<<literal>>"));
        QCOMPARE(track.textAt(2000), QString("First & second\n<<literal>>\nOverlap"));
        QCOMPARE(track.textAt(3000), QString("Overlap"));
        QCOMPARE(track.textAt(4000), QString());
        QCOMPARE(track.textAt(1500), QString("First & second\n<<literal>>"));
        QVERIFY(!track.load(temp.filePath("missing.srt"), error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(track.textAt(3000), QString("Overlap"));
        track.clear();
        QVERIFY(track.textAt(1500).isEmpty());
    }
    void subtitleSelection() {
        const QString directory = temp.filePath("subtitles");
        QVERIFY(QDir().mkpath(directory));
        const QString path = directory + "/external.SRT";
        QFile srt(path);
        QVERIFY(srt.open(QIODevice::WriteOnly));
        srt.write("1\n00:00:01,000 --> 00:00:02,000\nFirst external\n\n"
            "2\n00:00:02,000 --> 00:00:03,000\nSecond external\n");
        srt.close();
        const QString videoPath = directory + "/subtitled.mkv";
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-i", clip, "-i", path,
            "-map", "0:v", "-map", "0:a", "-map", "1:0", "-c", "copy", "-c:s", "srt",
            "-metadata:s:s:0", "language=eng", "-metadata:s:s:0", "title=Embedded", "-y", videoPath});
        QVERIFY(ffmpeg.waitForFinished(30000));
        QCOMPARE(ffmpeg.exitCode(), 0);
        PlayerWindow window;
        window.show();
        QVERIFY(selectSubtitle(window, -1));
        window.openFile(videoPath);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *sink = window.findChild<QVideoWidget *>()->videoSink();
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && sink->videoFrame().isValid(), 10000);
        QTRY_COMPARE(player->subtitleTracks().size(), 1);
        player->pause();
        player->setPosition(1500);
        QTRY_VERIFY(qAbs(sink->videoFrame().startTime() / 1000 - 1500) < 100);
        QVERIFY(selectSubtitle(window, path));
        QCOMPARE(player->activeSubtitleTrack(), -1);
        QCOMPARE(sink->subtitleText(), QString("First external"));
        QCOMPARE(sink->videoFrame().subtitleText(), QString("First external"));
        auto *gamma = window.findChild<QSlider *>("gamma");
        gamma->setValue(20);
        QCOMPARE(sink->subtitleText(), QString("First external"));
        gamma->setValue(10);
        QCOMPARE(sink->subtitleText(), QString("First external"));
        player->setPosition(2200);
        QTRY_COMPARE(sink->videoFrame().subtitleText(), QString("Second external"));
        player->setPosition(3500);
        QTRY_VERIFY(sink->videoFrame().subtitleText().isEmpty());
        player->setPosition(1500);
        QTRY_COMPARE(sink->videoFrame().subtitleText(), QString("First external"));
        window.showFullScreen();
        QVERIFY(selectSubtitle(window, -1));
        QTRY_VERIFY(sink->videoFrame().subtitleText().isEmpty());
        QVERIFY(selectSubtitle(window, 0));
        QCOMPARE(window.findChild<EmbeddedSubtitles *>()->activeTrack(), 0);
        QCOMPARE(player->activeSubtitleTrack(), -1);
        player->setPosition(500);
        player->play();
        QTRY_COMPARE_WITH_TIMEOUT(sink->videoFrame().subtitleText(), QString("First external"), 10000);
        auto *reader = window.findChild<QMediaPlayer *>("subtitlePlayer");
        QCOMPARE(reader->activeAudioTrack(), -1);
        QCOMPARE(reader->activeVideoTrack(), -1);
        QTRY_VERIFY(player->position() >= 1600);
        QVERIFY(reader->isPlaying());
        // Pausing must retain the caption without seeking the subtitle decoder.
        // Older Qt backends cannot redraw a subtitle-only seek until playback resumes.
        QSignalSpy pauseStatuses(reader, &QMediaPlayer::mediaStatusChanged);
        player->pause();
        QTRY_COMPARE(reader->playbackState(), QMediaPlayer::PausedState);
        for (const auto &status : pauseStatuses)
            QVERIFY(status.first().value<QMediaPlayer::MediaStatus>() != QMediaPlayer::LoadedMedia);
        const QString pausedCaption = sink->subtitleText();
        QVERIFY(!pausedCaption.isEmpty());
        QTRY_COMPARE(sink->videoFrame().subtitleText(), pausedCaption);
        gamma->setValue(16);
        QCOMPARE(sink->subtitleText(), pausedCaption);
        QCOMPARE(sink->videoFrame().subtitleText(), pausedCaption);
        gamma->setValue(10);
        QCOMPARE(sink->subtitleText(), pausedCaption);
        player->setPosition(2200);
        QCOMPARE(reader->position(), player->position());
        player->setPlaybackRate(1.5);
        QCOMPARE(reader->playbackRate(), 1.5);
        player->play();
        QTRY_COMPARE(sink->subtitleText(), QString("Second external"));
        QTRY_COMPARE(reader->mediaStatus(), QMediaPlayer::EndOfMedia);
        QVERIFY(player->isPlaying());
        QSignalSpy subtitleStates(reader, &QMediaPlayer::playbackStateChanged);
        QTest::qWait(250);
        QVERIFY(subtitleStates.isEmpty()); // No repeated restarts after the final caption.
        player->setPlaybackRate(1);
        player->setPosition(1500);
        QTRY_COMPARE(sink->subtitleText(), QString("First external"));
        player->pause();
        QVERIFY(selectSubtitle(window, path));
        QCOMPARE(player->activeSubtitleTrack(), -1);
        player->play();
        QTRY_COMPARE_WITH_TIMEOUT(sink->subtitleText(), QString("Second external"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(sink->subtitleText().isEmpty(), 5000);
        window.openFile(clip);
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && sink->videoFrame().isValid(), 10000);
        QTRY_VERIFY(sink->subtitleText().isEmpty());
        QVERIFY(reader->source().isEmpty());
    }
    void subtitleCyclingExample() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        const QString path = qEnvironmentVariable("VIDEO_PLAYER_SUBTITLE_CYCLING_EXAMPLE");
        if (path.isEmpty()) QSKIP("Optional local subtitle cycling example");
        PlayerWindow window;
        window.show();
        window.openFile(path);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *sink = window.findChild<QVideoWidget *>()->videoSink();
        QAudioBufferOutput buffers;
        player->setAudioBufferOutput(&buffers);
        QTRY_VERIFY_WITH_TIMEOUT(player->isSeekable() && player->subtitleTracks().size() >= 2, 15000);
        const int gamma = qEnvironmentVariableIntValue("VIDEO_PLAYER_SUBTITLE_CYCLING_GAMMA");
        window.findChild<QSlider *>("gamma")->setValue(gamma > 0 ? gamma : 10);
        window.findChild<QSlider *>("volume")->setValue(35);
        player->setPosition(1800000);
        QTRY_VERIFY_WITH_TIMEOUT(sink->videoFrame().startTime() / 1000 >= 1800000, 15000);
        QTest::qWait(1000);
        AudioSamples samples;
        QElapsedTimer frameTimer, audioTimer;
        frameTimer.start();
        audioTimer.start();
        qint64 frameGap = 0, audioGap = 0, keyTime = 0;
        bool captionSeen = false;
        QObject monitor;
        connect(sink, &QVideoSink::subtitleTextChanged, &monitor, [&](const QString &text) {
            captionSeen |= !text.isEmpty();
        });
        connect(sink, &QVideoSink::videoFrameChanged, &monitor, [&](const QVideoFrame &frame) {
            if (frame.isValid()) frameGap = qMax(frameGap, frameTimer.restart());
        });
        connect(&buffers, &QAudioBufferOutput::audioBufferReceived, &monitor, [&](const QAudioBuffer &buffer) {
            if (buffer.isValid()) {
                audioGap = qMax(audioGap, audioTimer.restart());
                samples.add(buffer);
            }
        });
        QSignalSpy trackChanges(player, &QMediaPlayer::activeTracksChanged);
        for (int i = 0; i < 9; ++i) {
            QElapsedTimer timer;
            timer.start();
            QVERIFY(cycleTrack(window, Qt::Key_S));
            keyTime = qMax(keyTime, timer.elapsed());
            QTest::qWait(900);
        }
        qInfo() << "subtitle cycling gaps: video" << frameGap << "ms audio" << audioGap
                << "ms key" << keyTime << "ms audio discontinuities" << samples.gaps;
        QVERIFY(samples.count > 100);
        QVERIFY(captionSeen);
        QCOMPARE(samples.gaps, 0);
        QVERIFY(frameGap < 250);
        QVERIFY(audioGap < 250);
        QVERIFY(trackChanges.isEmpty());
#else
        QSKIP("Audio buffer monitoring requires Qt 6.8");
#endif
    }
    void subtitleExample() {
        const QString directory = qEnvironmentVariable("VIDEO_PLAYER_SUBTITLE_EXAMPLE");
        if (directory.isEmpty()) QSKIP("Optional local subtitle example");
        const QDir folder(directory);
        const QStringList movies = folder.entryList({"*.mkv"}, QDir::Files);
        const QStringList files = folder.entryList({"*.srt"}, QDir::Files);
        QVERIFY(!movies.isEmpty());
        QCOMPARE(files.size(), 2);
        PlayerWindow window;
        window.show();
        window.openFile(folder.filePath(movies.first()));
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *sink = window.findChild<QVideoWidget *>()->videoSink();
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && sink->videoFrame().isValid(), 20000);
        player->pause();
        player->setPosition(57000);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(sink->videoFrame().startTime() / 1000 - 57000) < 1000, 10000);
        for (const auto &file : files) {
            QVERIFY(selectSubtitle(window, folder.filePath(file)));
            QVERIFY(!sink->subtitleText().isEmpty());
            if (file.contains("Eng")) QVERIFY(sink->subtitleText().contains("What are you doing here?"));
            else QVERIFY(sink->subtitleText().contains(QString::fromUtf8("Скарлетт")));
        }
        QVERIFY(selectSubtitle(window, folder.filePath(files.first())));
        const QString capture = qEnvironmentVariable("VIDEO_PLAYER_SUBTITLE_CAPTURE");
        if (!capture.isEmpty()) {
            QVideoFrame frame = sink->videoFrame();
            QImage rendered(frame.size(), QImage::Format_ARGB32);
            rendered.fill(Qt::black);
            QPainter painter(&rendered);
            frame.paint(&painter, QRectF(QPointF(), rendered.size()), {});
            painter.end();
            QVERIFY(rendered.save(capture));
        }
    }
    void nativeStartupBackground() {
#ifdef Q_OS_WIN
        if (QApplication::platformName() != "windows") QSKIP("Requires the native Windows platform");
        PlayerWindow window;
        const HWND handle = reinterpret_cast<HWND>(window.winId());
        QCOMPARE(GetClassLongPtrW(handle, GCLP_HBRBACKGROUND),
            reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
        // Exercise Windows' pre-paint erase on a white bitmap without showing the window.
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 16, 16);
        HGDIOBJ previous = SelectObject(memory, bitmap);
        RECT rect{0, 0, 16, 16};
        FillRect(memory, &rect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        const LRESULT handled = SendMessageW(handle, WM_ERASEBKGND, reinterpret_cast<WPARAM>(memory), 0);
        const COLORREF color = GetPixel(memory, 8, 8);
        SelectObject(memory, previous);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        QCOMPARE(handled, LRESULT(1));
        QCOMPARE(color, RGB(0, 0, 0));
#else
        QSKIP("Windows only");
#endif
    }
    void startupLoadingBackground() {
        PlayerWindow window;
        window.openFile(clip);
        auto *stage = window.findChild<QStackedWidget *>();
        QCOMPARE(stage->currentWidget()->objectName(), QString("loadingStage"));
        const QImage snapshot = stage->currentWidget()->grab().toImage();
        QVERIFY(!snapshot.isNull());
        QCOMPARE(snapshot.pixelColor(snapshot.width() / 2, snapshot.height() / 2), QColor(Qt::black));
        window.show();
        auto *video = window.findChild<QVideoWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(video->isVisible(), 10000);
        QVERIFY(video->videoSink()->videoFrame().isValid());
    }
    void playbackAndSeeking() {
        PlayerWindow window;
        window.show();
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *slider = window.findChild<SeekSlider *>("timeline");
        QSignalSpy frames(player->videoSink(), &QVideoSink::videoFrameChanged);
        window.openFile(clip);
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && player->hasVideo() && player->hasAudio(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > 2, 10000);
        QTRY_VERIFY(slider->isEnabled());
        QVERIFY(qAbs(player->duration() - 8000) < 100);
        QTest::keyClick(&window, Qt::Key_Space);
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
        const int oldFrames = frames.count();
        QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() * 3 / 4, slider->height() / 2));
        QTRY_VERIFY(qAbs(player->position() - 6000) < 250);
        QTRY_VERIFY(frames.count() > oldFrames);
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QTRY_VERIFY(qAbs(player->videoSink()->videoFrame().startTime() / 1000 - 6000) < 300);
        QTest::keyClick(&window, Qt::Key_Space);
        QTRY_VERIFY(player->isPlaying());
        QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() / 2, slider->height() / 2));
        QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        QVERIFY(qAbs(player->position() - 4000) < 250);
        const qint64 positionWhileDragging = player->position();
        QTRY_VERIFY(player->position() > positionWhileDragging);
        QTest::mouseMove(slider, QPoint(slider->width() / 4, slider->height() / 2));
        QTRY_VERIFY(qAbs(player->position() - 2000) < 250);
        QTRY_VERIFY(player->videoSink()->videoFrame().startTime() / 1000 >= 2000
            && player->videoSink()->videoFrame().startTime() / 1000 < 3000);
        const qint64 latestSeek = player->position();
        QTRY_VERIFY(player->position() > latestSeek + 150);
        const qint64 beforeRelease = player->position();
        QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() / 4, slider->height() / 2));
        QTRY_VERIFY(player->isPlaying());
        QVERIFY(player->position() >= beforeRelease);
        player->setPosition(7700);
        QTRY_COMPARE_WITH_TIMEOUT(player->mediaStatus(), QMediaPlayer::EndOfMedia, 5000);
        QTest::keyClick(&window, Qt::Key_Space);
        QTRY_VERIFY(player->isPlaying());
        QVERIFY(player->position() < 2000);
    }
    void draggingMovesWindow() {
        PlayerWindow window;
        window.show();
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *video = window.findChild<QVideoWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && video->isVisible(), 10000);
        const QPoint origin = window.pos();
        const QPoint local(100, 100);
        const QPoint global = video->mapToGlobal(local);
        QSignalSpy states(player, &QMediaPlayer::playbackStateChanged);
        QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(video, &press);
        QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        const QPoint delta(70, 50);
        QMouseEvent drag(QEvent::MouseMove, local + delta, global + delta, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(video, &drag);
        QCOMPARE(window.pos(), origin + delta);
        QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global + delta, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(video, &release);
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        QCOMPARE(states.count(), 0);
    }
    void resizeDiagnostics() {
        if (!qEnvironmentVariableIsSet("VIDEO_PLAYER_RESIZE_BENCHMARK"))
            QSKIP("Opt-in native resize diagnostic");
        DiagnosticWindow window;
        window.show();
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *video = window.findChild<QVideoWidget *>();
        auto *app = static_cast<DiagnosticApplication *>(qApp);
        const auto measure = [&](const char *label, bool grow) {
            window.resize(grow ? QSize(700, 450) : QSize(1300, 750));
            QTest::qWait(100);
            app->videoEventNs = app->videoEvents = 0;
            window.eraseNs = window.eraseCount = 0;
            app->measuring = true;
            QElapsedTimer total;
            total.start();
            qint64 worst = 0;
            qint64 resizeNs = 0;
            for (int i = 1; i <= 60; ++i) {
                QElapsedTimer step;
                step.start();
                const int n = grow ? i : 60 - i;
                window.resize(700 + n * 10, 450 + n * 5);
                resizeNs += step.nsecsElapsed();
                QApplication::processEvents();
                worst = qMax(worst, step.elapsed());
            }
            app->measuring = false;
            qInfo("Resize %s %s: total=%lld ms resizeCalls=%lld ms videoEvents=%lld ms (%d) erase=%lld us (%d) worst=%lld ms",
                label, grow ? "grow" : "shrink", total.elapsed(), resizeNs / 1000000,
                app->videoEventNs / 1000000, app->videoEvents, window.eraseNs / 1000, window.eraseCount, worst);
        };
        const auto both = [&](const char *label) { for (int repeat = 0; repeat < 2; ++repeat) { measure(label, true); measure(label, false); } };
        const bool pausedOnly = qEnvironmentVariableIsSet("VIDEO_PLAYER_RESIZE_PAUSED_ONLY");
        if (!pausedOnly) both("empty");
        window.openFile(clip);
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && video->isVisible(), 10000);
        player->pause();
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QTest::qWait(250);
        QSignalSpy pausedFrames(video->videoSink(), &QVideoSink::videoFrameChanged);
        const qint64 pausedPosition = player->position();
        both("paused video");
        qInfo("Paused resize: new frames=%lld, position delta=%lld ms", qint64(pausedFrames.count()), player->position() - pausedPosition);
        if (pausedOnly) {
            window.skipErase = true;
            both("paused no erase");
            video->hide();
            both("paused hidden");
            return;
        }
        player->setLoops(QMediaPlayer::Infinite);
        player->play();
        both("playing video");
        window.skipErase = true;
        both("playing no erase");
        video->hide();
        both("video hidden");
    }
    void volumeControls() {
        QSettings().remove("audio/volume");
        PlayerWindow window;
        window.show();
        window.activateWindow();
        QTRY_VERIFY(window.isActiveWindow());
        auto *volume = window.findChild<QSlider *>("volume");
        auto *audio = window.findChild<QAudioOutput *>();
        QTest::keyClick(&window, Qt::Key_Up);
        QCOMPARE(volume->value(), 75);
        QVERIFY(qAbs(audio->volume() - 0.75f) < 0.001f);
        QTest::keyClick(&window, Qt::Key_Down);
        QCOMPARE(volume->value(), 70);
        const auto wheel = [](QWidget *target, int delta) {
            QWheelEvent event(QPointF(10, 10), target->mapToGlobal(QPoint(10, 10)), {},
                QPoint(0, delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(target, &event);
        };
        window.showFullScreen();
        wheel(window.findChild<QVideoWidget *>(), 120);
        QCOMPARE(volume->value(), 75);
        wheel(volume, -120);
        QCOMPARE(volume->value(), 70);
        wheel(&window, 120 * 30);
        QCOMPARE(volume->value(), 100);
        wheel(&window, -120 * 30);
        QCOMPARE(volume->value(), 0);
        QCOMPARE(audio->volume(), 0.f);
    }
    void volumePersists() {
        for (int saved : {35, 0, 100}) {
            {
                PlayerWindow window;
                window.findChild<QSlider *>("volume")->setValue(saved);
                window.close();
            }
            PlayerWindow reopened;
            QCOMPARE(reopened.findChild<QSlider *>("volume")->value(), saved);
            QVERIFY(qAbs(reopened.findChild<QAudioOutput *>()->volume() - saved / 100.f) < 0.001f);
        }
        QSettings().remove("audio/volume");
    }
    void perVideoSettingsPersist() {
        const QString first = temp.filePath("settings first.mp4");
        const QString second = temp.filePath("settings second.mp4");
        QVERIFY(QFile::copy(clip, first));
        QVERIFY(QFile::copy(clip, second));
        {
            PlayerWindow window;
            auto *volume = window.findChild<QSlider *>("volume");
            auto *gamma = window.findChild<QSlider *>("gamma");
            window.openFile(first);
            volume->setValue(0);
            gamma->setValue(23);
            window.openFile(second);
            QCOMPARE(gamma->value(), 10);
            volume->setValue(85);
            window.openFile(first);
            QCOMPARE(volume->value(), 0);
            QCOMPARE(gamma->value(), 23);
            QCOMPARE(window.findChild<QAudioOutput *>()->volume(), 0.f);
            QCOMPARE(window.findChild<QLabel *>("volumeReadout")->text(), QString("Volume 0"));
            QCOMPARE(window.findChild<QLabel *>("gammaReadout")->text(), QString("Gamma 2.3"));
            // A missing file must not change the active video's settings.
            window.openFile(temp.filePath("missing settings.mp4"));
            QCOMPARE(volume->value(), 0);
            QCOMPARE(gamma->value(), 23);
            window.close();
        }
        {
            PlayerWindow window;
            auto *volume = window.findChild<QSlider *>("volume");
            auto *gamma = window.findChild<QSlider *>("gamma");
            auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
            auto *video = window.findChild<QVideoWidget *>();
            window.openFile(first);
            QCOMPARE(volume->value(), 0);
            QCOMPARE(gamma->value(), 23);
            QVERIFY(player->videoSink() != video->videoSink());
            window.openFile(second);
            QCOMPARE(volume->value(), 85);
            QCOMPARE(gamma->value(), 10);
            QCOMPARE(player->videoSink(), video->videoSink());
            QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
            window.openFile(first);
            QCOMPARE(volume->value(), 0);
            QCOMPARE(gamma->value(), 23);
            gamma->setValue(10);
            window.close();
        }
        PlayerWindow reopened;
        reopened.openFile(first);
        QCOMPARE(reopened.findChild<QSlider *>("gamma")->value(), 10);
        QCOMPARE(reopened.findChild<QSlider *>("volume")->value(), 0);
        QCOMPARE(reopened.findChild<QMediaPlayer *>("mediaPlayer")->videoSink(),
            reopened.findChild<QVideoWidget *>()->videoSink());
        QSettings().remove("audio/volume");
    }
    void gammaPixels() {
        QVideoSink output;
        GammaFilter filter(&output);
        QVideoFrame frame(QVideoFrameFormat(QSize(3, 1), QVideoFrameFormat::Format_RGBA8888));
        QVERIFY(frame.map(QVideoFrame::WriteOnly));
        const uchar pixels[] = {0, 0, 0, 255, 64, 128, 192, 255, 255, 255, 255, 255};
        std::copy(std::begin(pixels), std::end(pixels), frame.bits(0));
        frame.unmap();
        frame.setStartTime(123000);
        frame.setEndTime(163000);
        frame.setMirrored(true);
        filter.input()->setVideoFrame(frame);
        QCOMPARE(output.videoFrame(), frame);
        const QImage original = frame.toImage();
        filter.setGamma(20);
        const QVideoFrame bright = output.videoFrame();
        const QImage image = bright.toImage();
        QCOMPARE(image.pixelColor(0, 0), QColor(Qt::black));
        QCOMPARE(image.pixelColor(2, 0), QColor(Qt::white));
        const QColor middle = image.pixelColor(1, 0);
        QVERIFY(qAbs(middle.red() - 128) <= 1);
        QVERIFY(qAbs(middle.green() - 181) <= 1);
        QVERIFY(qAbs(middle.blue() - 221) <= 1);
        QCOMPARE(bright.startTime(), frame.startTime());
        QCOMPARE(bright.endTime(), frame.endTime());
        QCOMPARE(bright.mirrored(), frame.mirrored());
        QCOMPARE(frame.toImage(), original);
        filter.setGamma(5);
        QVERIFY(output.videoFrame().toImage().pixelColor(1, 0).red() < 64);
        filter.setGamma(10);
        QCOMPARE(output.videoFrame(), frame);
        filter.setGamma(20);
        QCOMPARE(output.videoFrame().toImage(), image);
        filter.input()->setVideoFrame({});
        QVERIFY(!output.videoFrame().isValid());
    }
    void gammaPlayback() {
        PlayerWindow window;
        window.show();
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *video = window.findChild<QVideoWidget *>();
        auto *gamma = window.findChild<QSlider *>("gamma");
        auto *volume = window.findChild<QSlider *>("volume");
        QVERIFY(gamma);
        QCOMPARE(gamma->value(), 10);
        QCOMPARE(player->videoSink(), video->videoSink());
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && video->isVisible(), 10000);
        player->pause();
        QTest::qWait(100);
        const QImage original = video->videoSink()->videoFrame().toImage();
        const qint64 position = player->position();
        const int initialVolume = volume->value();
        gamma->setValue(20);
        QVERIFY(player->videoSink() != video->videoSink());
        auto *filterInput = player->videoSink();
        QVERIFY(video->videoSink()->videoFrame().toImage() != original);
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QCOMPARE(player->position(), position);
        QCOMPARE(volume->value(), initialVolume);
        QCOMPARE(window.findChild<QLabel *>("gammaReadout")->text(), QString("Gamma 2.0"));
        QTest::mouseDClick(gamma, Qt::LeftButton);
        QTest::mouseRelease(gamma, Qt::LeftButton);
        QCOMPARE(gamma->value(), 10);
        QCOMPARE(player->videoSink(), video->videoSink());
        QVERIFY(!filterInput->videoFrame().isValid());
        QCOMPARE(video->videoSink()->videoFrame().toImage(), original);
        gamma->setValue(15);
        QCOMPARE(player->videoSink(), filterInput);
        player->play();
        QTRY_VERIFY(player->position() > position + 300);
        QVERIFY(video->videoSink()->videoFrame().isValid());
        gamma->setValue(10);
        QCOMPARE(player->videoSink(), video->videoSink());
        const qint64 resetPosition = player->position();
        QTRY_VERIFY(player->position() > resetPosition + 300);
        QVERIFY(video->videoSink()->videoFrame().isValid());
        QVERIFY(!filterInput->videoFrame().isValid());
    }
    void fullscreenControlsAtBottom() {
        PlayerWindow window;
        window.openFile(clip);
        window.showFullScreen();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *video = window.findChild<QVideoWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(video->isVisible(), 10000);
        auto *panel = window.findChild<QWidget *>("controlsPanel");
        auto *volume = window.findChild<QSlider *>("volume");
        const QPoint savedCursor = QCursor::pos();
        QCursor::setPos(window.mapToGlobal(window.rect().center()));
        QTRY_VERIFY(!panel->isVisible());
        const QRect videoGeometry(video->mapToGlobal(QPoint()), video->size());
        const QPoint panelTop = window.centralWidget()->mapToGlobal(QPoint(
            window.centralWidget()->width() / 2, window.centralWidget()->height() - panel->sizeHint().height()));
        QCursor::setPos(panelTop - QPoint(0, 1));
        QTest::qWait(150);
        QVERIFY(!panel->isVisible());
        QCursor::setPos(panelTop);
        QTRY_VERIFY(panel->isVisible());
        QCursor::setPos(volume->mapToGlobal(volume->rect().center()));
        QTest::qWait(150);
        QVERIFY(panel->isVisible());
        QCOMPARE(QRect(video->mapToGlobal(QPoint()), video->size()), videoGeometry);
        QVERIFY(panel->isWindow());
        QCOMPARE(panel->geometry().bottom(), window.centralWidget()->mapToGlobal(
            window.centralWidget()->rect().bottomLeft()).y());
        volume->setSliderDown(true);
        QCursor::setPos(window.mapToGlobal(window.rect().center()));
        QTest::qWait(150);
        QVERIFY(panel->isVisible());
        volume->setSliderDown(false);
        QTRY_VERIFY(!panel->isVisible());
        auto *gamma = window.findChild<QSlider *>("gamma");
        QCursor::setPos(window.mapToGlobal(QPoint(window.width() / 2, window.height() - 1)));
        QTRY_VERIFY(gamma->isVisible());
        gamma->setSliderDown(true);
        QCursor::setPos(window.mapToGlobal(window.rect().center()));
        QTest::qWait(150);
        QVERIFY(panel->isVisible());
        gamma->setSliderDown(false);
        QTRY_VERIFY(!panel->isVisible());
        QCOMPARE(QRect(video->mapToGlobal(QPoint()), video->size()), videoGeometry);
        window.showNormal();
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(!panel->isWindow());
        QTRY_VERIFY(panel->y() >= video->mapTo(window.centralWidget(), QPoint(0, video->height())).y());
        QCursor::setPos(savedCursor);
    }
    void fullscreenCloseButton() {
        PlayerWindow window;
        window.show();
        auto *button = window.findChild<QAbstractButton *>("fullscreenClose");
        QVERIFY(button);
        QVERIFY(!button->isVisible());
        const QPoint savedCursor = QCursor::pos();
        window.openFile(clip);
        window.showFullScreen();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *video = window.findChild<QVideoWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(video->isVisible(), 10000);
        QCursor::setPos(window.mapToGlobal(window.rect().center()));
        QTRY_VERIFY(!button->isVisible());
        const QRect videoGeometry(video->mapToGlobal(QPoint()), video->size());
        QCursor::setPos(window.mapToGlobal(QPoint(window.width() - 1, 1)));
        QTRY_VERIFY(button->isVisible());
        QVERIFY(button->isWindow());
        QCOMPARE(button->geometry().topRight(), window.centralWidget()->mapToGlobal(
            window.centralWidget()->rect().topRight()));
        QCOMPARE(QRect(video->mapToGlobal(QPoint()), video->size()), videoGeometry);
        QCursor::setPos(button->mapToGlobal(button->rect().center()));
        QTest::qWait(150);
        QVERIFY(button->isVisible());
        QCursor::setPos(window.mapToGlobal(window.rect().center()));
        QTRY_VERIFY(!button->isVisible());
        QCursor::setPos(window.mapToGlobal(QPoint(window.width() - 1, 1)));
        QTRY_VERIFY(button->isVisible());
        window.showNormal();
        QTRY_VERIFY(!button->isVisible());
        window.showFullScreen();
        QCursor::setPos(window.mapToGlobal(QPoint(window.width() - 1, 1)));
        QTRY_VERIFY(button->isVisible());
        QTest::mousePress(button, Qt::LeftButton, Qt::NoModifier, button->rect().center());
        QTest::qWait(150);
        QVERIFY(button->isVisible());
        QTest::mouseRelease(button, Qt::LeftButton, Qt::NoModifier, button->rect().center());
        QTRY_VERIFY(!window.isVisible());
        QVERIFY(!button->isVisible());
        QCursor::setPos(savedCursor);
    }
    void mousePlaybackAndFullscreen() {
        PlayerWindow window;
        window.setGeometry(40, 50, 720, 480);
        window.show();
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *video = window.findChild<QVideoWidget *>();
        auto *slider = window.findChild<SeekSlider *>("timeline");
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && video->isVisible(), 10000);
        const QRect windowedGeometry = window.geometry();
        QTest::mousePress(video, Qt::LeftButton);
        QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        QTest::mouseRelease(video, Qt::LeftButton);
        QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
        // Native double-click sequence: first click, double-click press, release.
        const auto doubleClick = [&] {
            QSignalSpy states(player, &QMediaPlayer::playbackStateChanged);
            QTest::mouseClick(video, Qt::LeftButton);
            QTest::qWait(qMin(50, QApplication::doubleClickInterval() / 2));
            QTest::mouseDClick(video, Qt::LeftButton);
            QTest::mouseRelease(video, Qt::LeftButton);
            QTest::qWait(QApplication::doubleClickInterval() + 100);
            QCOMPARE(states.count(), 0);
        };
        doubleClick();
        QVERIFY(window.isFullScreen());
        QVERIFY(!slider->isVisible());
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QTest::mouseClick(video, Qt::LeftButton);
        QTRY_VERIFY(player->isPlaying());
        doubleClick();
        QVERIFY(!window.isFullScreen());
        QVERIFY(slider->isVisible());
        QTRY_COMPARE(window.geometry(), windowedGeometry);
        QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        window.showMaximized();
        QTRY_VERIFY(window.isMaximized());
        doubleClick();
        QVERIFY(window.isFullScreen());
        doubleClick();
        QVERIFY(!window.isFullScreen());
        QTRY_VERIFY(window.isMaximized());
        window.showNormal();
        QTRY_COMPARE(window.geometry(), windowedGeometry);
    }
    void positionSurvivesClosing() {
        {
            PlayerWindow window;
            window.show();
            window.openFile(clip);
            auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
            QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && player->isSeekable(), 10000);
            player->pause();
            player->setPosition(4000);
            QTRY_COMPARE(player->position(), qint64(4000));
            QTest::keyClick(&window, Qt::Key_Escape);
            QTRY_VERIFY(!window.isVisible());
        }
        {
            PlayerWindow window;
            window.openFile(clip);
            auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
            QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && player->position() >= 4000, 10000);
            QVERIFY(player->position() < 5000);
            player->setPosition(7800);
            QTRY_COMPARE_WITH_TIMEOUT(player->mediaStatus(), QMediaPlayer::EndOfMedia, 5000);
            window.close();
        }
        PlayerWindow window;
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
        QVERIFY(player->position() < 1000);
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
    DiagnosticApplication app(argc, argv);
    app.setOrganizationName("VideoPlayerTests");
    app.setApplicationName("PositionTests");
    app.setStyle("Fusion");
    PlayerTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "player_tests.moc"
