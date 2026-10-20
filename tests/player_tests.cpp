#include "playerwindow.h"
#include "seekslider.h"
#include "keyframeindex.h"
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
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QVideoSink>
#include <QVideoFrame>
#include "videosurface.h"
#include <QQuickView>
#include <QMutex>
#include <atomic>
#include <memory>
#include <QQuickItem>
#include <QScopeGuard>
#include <cmath>
#include <QStackedWidget>
#include <QAudioOutput>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAudioBufferOutput>
#include <QAudioBuffer>
#endif
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QCursor>
#include <QMenu>
#include <QKeySequence>
#include <QMediaMetaData>
#include <QDir>
#include <QPainter>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
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

static QString subtitleText(const PlayerWindow &window) {
    return window.findChild<QLabel *>("subtitleOverlay")->text();
}

static bool waitForVideoExposed(PlayerWindow &window) {
    const auto *video = window.findChild<VideoSurface *>();
    // A fullscreen native child can cover the entire QWidget window.
    return video->quickView() ? QTest::qWaitForWindowExposed(video->quickView())
                             : QTest::qWaitForWindowExposed(&window);
}

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
    bool withPopupMenu(QWidget &surface, Callback callback, QWindow *input = nullptr) {
        bool opened = false;
        QTimer timer;
        timer.setInterval(10);
        connect(&timer, &QTimer::timeout, &surface, [&] {
            auto *menu = surface.window()->findChild<QMenu *>("playerMenu");
            if (!menu || !menu->isVisible()) return;
            timer.stop();
            opened = true;
            callback(*menu);
            menu->close();
        });
        timer.start();
        if (input) {
            QTest::mouseClick(input, Qt::RightButton, Qt::NoModifier, QPoint(10, 10));
        } else {
            QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(10, 10), surface.mapToGlobal(QPoint(10, 10)));
            QApplication::sendEvent(&surface, &event);
        }
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
                static_cast<QWidget *>(window.findChild<VideoSurface *>())}) {
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
        auto *video = window.findChild<VideoSurface *>();
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
        QTRY_VERIFY(player->isPlaying());
        QVERIFY(activate("playPause"));
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
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
        // Seekability can precede startup preparation and its automatic resume.
        QTRY_VERIFY(video->isSeekable() && video->isPlaying());
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
        QTRY_COMPARE(subtitleText(window), QString("first"));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QTRY_COMPARE(subtitleText(window), QString("second"));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QCOMPARE(player->activeSubtitleTrack(), -1);
        QVERIFY(subtitleText(window).isEmpty());
        QVERIFY(toggleTrackCheck(window, 0, Qt::Key_S));
        QVERIFY(toggleTrackCheck(window, first, Qt::Key_S));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QCOMPARE(subtitleText(window), QString("second"));
        QVERIFY(toggleTrackCheck(window, -1, Qt::Key_S, true));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QCOMPARE(subtitleText(window), QString("second"));
        QVERIFY(toggleTrackCheck(window, second, Qt::Key_S));
        QVERIFY(cycleTrack(window, Qt::Key_S));
        QCOMPARE(subtitleText(window), QString("second"));
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QVERIFY(qAbs(player->position() - position) < 100);
        QVERIFY(subtitleChanges.isEmpty());

        // Unseen videos start checked; reopening restores each video's exclusions.
        window.openFile(clip);
        QTRY_COMPARE(player->audioTracks().size(), 1);
        for (auto key : {Qt::Key_A, Qt::Key_S}) {
            QVERIFY(withTrackMenu(window, key, [&](QMenu &menu) {
                for (auto *action : menu.actions())
                    if (action->isCheckable()) QVERIFY(action->isChecked());
            }));
        }
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
            QCOMPARE(checked, key == Qt::Key_S ? 0 : 1);
        }
        window.close();
        PlayerWindow reopened;
        reopened.show();
        reopened.openFile(movie);
        auto *reopenedPlayer = reopened.findChild<QMediaPlayer *>("mediaPlayer");
        QTRY_VERIFY(reopenedPlayer->isPlaying());
        for (auto key : {Qt::Key_A, Qt::Key_S}) {
            QVERIFY(withTrackMenu(reopened, key, [&](QMenu &menu) {
                for (auto *action : menu.actions()) {
                    if (!action->isCheckable()) continue;
                    QCOMPARE(action->isChecked(), key == Qt::Key_A && action->data() == QVariant(1));
                }
            }));
        }
        // Re-enabling a saved exclusion must also survive reopening.
        QVERIFY(toggleTrackCheck(reopened, audio, Qt::Key_A));
        QVERIFY(toggleTrackCheck(reopened, -1, Qt::Key_S));
        reopened.openFile(clip);
        reopened.openFile(movie);
        QTRY_VERIFY(reopenedPlayer->isPlaying());
        for (auto key : {Qt::Key_A, Qt::Key_S}) {
            QVERIFY(withTrackMenu(reopened, key, [&](QMenu &menu) {
                for (auto *action : menu.actions()) {
                    if (!action->isCheckable()) continue;
                    const bool expected = key == Qt::Key_A
                        ? action->data() == QVariant(1) || action->data() == QVariant(audio)
                        : action->data() == QVariant(-1);
                    QCOMPARE(action->isChecked(), expected);
                }
            }));
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
        auto *sink = window.findChild<VideoSurface *>()->videoSink();
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && sink->videoFrame().isValid(), 10000);
        QTRY_COMPARE(player->subtitleTracks().size(), 1);
        player->pause();
        player->setPosition(1500);
        QTRY_VERIFY(qAbs(sink->videoFrame().startTime() / 1000 - 1500) < 100);
        QVERIFY(selectSubtitle(window, path));
        QCOMPARE(player->activeSubtitleTrack(), -1);
        QCOMPARE(subtitleText(window), QString("First external"));
        QVERIFY(sink->subtitleText().isEmpty());
        QVERIFY(sink->videoFrame().subtitleText().isEmpty());
        auto *caption = window.findChild<QLabel *>("subtitleOverlay");
        QTRY_VERIFY(caption->isVisible());
        const QImage background = caption->grab().toImage();
        QCOMPARE(background.pixelColor(0, 0).alpha(), 0);
        QVERIFY(background.pixelColor(background.width() / 2, 0).alpha() > 0);
        // Expiring an action notification must leave the paused caption visible.
        auto *notification = window.findChild<QLabel *>("actionNotification");
        QVERIFY(notification->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!notification->isVisible(), 4000);
        QVERIFY(caption->isVisible());
        QCOMPARE(subtitleText(window), QString("First external"));
        auto *gamma = window.findChild<QSlider *>("gamma");
        gamma->setValue(20);
        QCOMPARE(subtitleText(window), QString("First external"));
        gamma->setValue(10);
        QCOMPARE(subtitleText(window), QString("First external"));
        player->setPosition(2200);
        QTRY_COMPARE(subtitleText(window), QString("Second external"));
        player->setPosition(3500);
        QTRY_VERIFY(subtitleText(window).isEmpty());
        player->setPosition(1500);
        QTRY_COMPARE(subtitleText(window), QString("First external"));
        window.showFullScreen();
        QVERIFY(selectSubtitle(window, -1));
        QTRY_VERIFY(subtitleText(window).isEmpty());
        QVERIFY(selectSubtitle(window, 0));
        QCOMPARE(window.findChild<EmbeddedSubtitles *>()->activeTrack(), 0);
        QCOMPARE(player->activeSubtitleTrack(), -1);
        player->setPosition(500);
        player->play();
        QTRY_COMPARE_WITH_TIMEOUT(subtitleText(window), QString("First external"), 10000);
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
        const QString pausedCaption = subtitleText(window);
        QVERIFY(!pausedCaption.isEmpty());
        gamma->setValue(16);
        QCOMPARE(subtitleText(window), pausedCaption);
        QVERIFY(sink->videoFrame().subtitleText().isEmpty());
        gamma->setValue(10);
        QCOMPARE(subtitleText(window), pausedCaption);
        player->setPosition(2200);
        QCOMPARE(reader->position(), player->position());
        player->setPlaybackRate(1.5);
        QCOMPARE(reader->playbackRate(), 1.5);
        player->play();
        QTRY_COMPARE(subtitleText(window), QString("Second external"));
        QTRY_COMPARE(reader->mediaStatus(), QMediaPlayer::EndOfMedia);
        QVERIFY(player->isPlaying());
        QSignalSpy subtitleStates(reader, &QMediaPlayer::playbackStateChanged);
        QTest::qWait(250);
        QVERIFY(subtitleStates.isEmpty()); // No repeated restarts after the final caption.
        player->setPlaybackRate(1);
        player->setPosition(1500);
        QTRY_COMPARE(subtitleText(window), QString("First external"));
        player->pause();
        QVERIFY(selectSubtitle(window, path));
        QCOMPARE(player->activeSubtitleTrack(), -1);
        player->play();
        QTRY_COMPARE_WITH_TIMEOUT(subtitleText(window), QString("Second external"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(subtitleText(window).isEmpty(), 5000);
        window.openFile(clip);
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && sink->videoFrame().isValid(), 10000);
        QTRY_VERIFY(subtitleText(window).isEmpty());
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
        auto *sink = window.findChild<VideoSurface *>()->videoSink();
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
        auto *sink = window.findChild<VideoSurface *>()->videoSink();
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && sink->videoFrame().isValid(), 20000);
        player->pause();
        player->setPosition(57000);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(sink->videoFrame().startTime() / 1000 - 57000) < 1000, 10000);
        for (const auto &file : files) {
            QVERIFY(selectSubtitle(window, folder.filePath(file)));
            QVERIFY(!subtitleText(window).isEmpty());
            if (file.contains("Eng")) QVERIFY(subtitleText(window).contains("What are you doing here?"));
            else QVERIFY(subtitleText(window).contains(QString::fromUtf8("Скарлетт")));
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
        auto *video = window.findChild<VideoSurface *>();
        QCOMPARE(stage->currentWidget(), video);
        QVERIFY(!video->videoSink()->videoFrame().isValid());
        const QImage snapshot = stage->currentWidget()->grab().toImage();
        QVERIFY(!snapshot.isNull());
        QCOMPARE(snapshot.pixelColor(snapshot.width() / 2, snapshot.height() / 2), QColor(Qt::black));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(video->isVisible() && video->videoSink()->videoFrame().isValid(), 10000);
        // A subsequent open must initialize on black rather than showing a cached frame.
        window.openFile(clip);
        QVERIFY(!video->videoSink()->videoFrame().isValid());
        QTRY_VERIFY_WITH_TIMEOUT(video->videoSink()->videoFrame().isValid(), 10000);
    }
    void startupWaitsForFirstFrame() {
        PlayerWindow window;
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *video = window.findChild<VideoSurface *>();
        bool prepared = false;
        bool startedWithFrame = false;
        connect(video, &VideoSurface::videoFrameChanged, &window, [&](const QVideoFrame &frame) {
            if (frame.isValid()) prepared = true;
        });
        connect(player, &QMediaPlayer::playbackStateChanged, &window, [&](auto state) {
            if (state == QMediaPlayer::PlayingState)
                startedWithFrame = prepared && video->videoSink()->videoFrame().isValid();
        });
        window.openFile(clip);
        QVERIFY(!player->isPlaying());
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
        QVERIFY(startedWithFrame);
    }
    void pauseDuringStartup() {
        PlayerWindow window;
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *video = window.findChild<VideoSurface *>();
        window.openFile(clip);
        window.findChild<QAction *>("playPause")->trigger();
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(video->videoSink()->videoFrame().isValid(), 10000);
        QTest::qWait(100);
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        window.findChild<QAction *>("playPause")->trigger();
        QTRY_VERIFY(player->isPlaying());
    }
    void audioOnlyStartup() {
        const QString audio = temp.filePath("startup-audio.m4a");
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-v", "error", "-i", clip, "-vn", "-c:a", "copy", "-y", audio});
        QVERIFY(ffmpeg.waitForFinished(30000));
        QCOMPARE(ffmpeg.exitCode(), 0);
        PlayerWindow window;
        window.openFile(audio);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && player->hasAudio(), 10000);
        QVERIFY(!player->hasVideo());
    }
    void keyframeTimelineSeeking() {
        QSettings().remove("seeking/keyframes");
        {
            PlayerWindow window;
            window.show();
            auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
            auto *slider = window.findChild<SeekSlider *>("timeline");
            auto *mode = window.findChild<QAction *>("keyframeSeeking");
            auto *index = window.findChild<KeyframeIndex *>();
            QVERIFY(!mode->isChecked());
            window.openFile(clip);
            QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying() && slider->isEnabled(), 10000);
            player->pause();
            QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
            QSignalSpy indexed(index, &KeyframeIndex::finished);
            mode->trigger();
            QTRY_COMPARE_WITH_TIMEOUT(indexed.count(), 1, 10000);
            QVERIFY(index->isReady());
            QCOMPARE(index->seekPosition(2350), qint64(2000));
            QCOMPARE(index->seekPosition(0), qint64(0));
            QCOMPARE(index->seekPosition(8000), qint64(7000));
            slider->setSliderDown(true);
            const auto setTime = [&](int milliseconds) {
                slider->setValue(qRound(milliseconds * double(slider->maximum()) / player->duration()));
            };
            setTime(2350);
            QCOMPARE(player->position(), qint64(2000));
            QTRY_VERIFY(qAbs(player->videoSink()->videoFrame().startTime() / 1000 - 2000) < 80);
            QSignalSpy positions(player, &QMediaPlayer::positionChanged);
            setTime(2700);
            QVERIFY(positions.isEmpty());
            setTime(3650);
            QCOMPARE(player->position(), qint64(3000));
            slider->setSliderDown(false);
            QCOMPARE(player->position(), qint64(3000));
            QVERIFY(qAbs(slider->value() * double(player->duration()) / slider->maximum() - 3000) < 10);
            QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
            mode->trigger();
            setTime(2350);
            QVERIFY(qAbs(player->position() - 2350) < 10);
            mode->trigger();
            player->play();
            slider->setSliderDown(true);
            setTime(4350);
            QTRY_VERIFY(player->isPlaying());
            const auto start = player->position();
            QTRY_VERIFY(player->position() > start + 100);
            const auto beforeRelease = player->position();
            setTime(4700); // Same keyframe: playback should keep advancing.
            QVERIFY(player->position() >= beforeRelease);
            slider->setSliderDown(false);
        }
        {
            PlayerWindow reopened;
            QVERIFY(reopened.findChild<QAction *>("keyframeSeeking")->isChecked());
        }
        QSettings().remove("seeking/keyframes");
    }
    void keyframeArrowSeeking() {
        QSettings().remove("seeking/keyframes");
        PlayerWindow window;
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *mode = window.findChild<QAction *>("keyframeSeeking");
        auto *index = window.findChild<KeyframeIndex *>();
        QTRY_VERIFY(player->isPlaying() && window.findChild<SeekSlider *>("timeline")->isEnabled());
        player->pause();
        mode->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(index->isReady(), 10000);
        player->setPosition(2350);
        QTest::keyClick(&window, Qt::Key_Right);
        QCOMPARE(player->position(), qint64(6000));
        QTRY_VERIFY(qAbs(player->videoSink()->videoFrame().startTime() / 1000 - 6000) < 80);
        player->setPosition(5350);
        QTest::keyClick(&window, Qt::Key_Left);
        QCOMPARE(player->position(), qint64(2000));
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QTest::keyClick(&window, Qt::Key_Left, Qt::ShiftModifier);
        QCOMPARE(player->position(), qint64(0));
        QTest::keyClick(&window, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(player->position(), player->duration());
        // The tail after the last keyframe still allows forward progress.
        player->setPosition(4500);
        QTest::keyClick(&window, Qt::Key_Right);
        QCOMPARE(player->position(), qint64(7500));
        mode->trigger();
        player->setPosition(2350);
        QTest::keyClick(&window, Qt::Key_Right);
        QCOMPARE(player->position(), qint64(5350));
        mode->trigger();
        player->setPosition(1000);
        player->play();
        QTest::keyClick(&window, Qt::Key_Right);
        QTRY_VERIFY(player->isPlaying());
        QVERIFY(player->position() >= 4000);
        mode->trigger();
        QSettings().remove("seeking/keyframes");
    }
    void keyframeIndexSourceChanges() {
        KeyframeIndex index;
        QSignalSpy finished(&index, &KeyframeIndex::finished);
        index.setSource(QUrl::fromLocalFile(clip));
        index.load();
        index.setSource(QUrl::fromLocalFile(temp.filePath("missing.mp4")));
        QVERIFY(!index.isReady());
        QCOMPARE(index.seekPosition(2350), qint64(2350));
        index.load();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(!finished.first().first().toBool());
        index.setSource(QUrl::fromLocalFile(clip));
        index.load();
        QTRY_VERIFY_WITH_TIMEOUT(index.isReady(), 10000);
        QCOMPARE(index.seekPosition(2350), qint64(2000));
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
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        QVERIFY(qAbs(player->position() - 4000) < 250);
        const qint64 positionWhileDragging = player->position();
        QTRY_VERIFY(player->position() > positionWhileDragging);
        QTest::mouseMove(slider, QPoint(slider->width() / 4, slider->height() / 2));
        QTRY_VERIFY(qAbs(player->position() - 2000) < 250);
        QTRY_VERIFY2(player->videoSink()->videoFrame().startTime() / 1000 >= 2000
            && player->videoSink()->videoFrame().startTime() / 1000 < 3000,
            qPrintable(QString("Position %1 ms, frame %2 ms, state %3, slider %4")
                .arg(player->position()).arg(player->videoSink()->videoFrame().startTime() / 1000)
                .arg(int(player->playbackState())).arg(slider->value())));
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
    void seekWaitsForLatestFrame_data() {
        QTest::addColumn<bool>("playing");
        QTest::addColumn<int>("gammaValue");
        QTest::addColumn<qint64>("target");
        for (bool playing : {false, true})
            for (int gammaValue : {10, 16})
                for (qint64 target : {qint64(6000), qint64(1920)})
                    QTest::newRow(qPrintable(QString("%1-gamma-%2-target-%3")
                        .arg(playing ? "playing" : "paused").arg(gammaValue).arg(target)))
                        << playing << gammaValue << target;
    }
    void seekWaitsForLatestFrame() {
        QFETCH(bool, playing);
        QFETCH(int, gammaValue);
        QFETCH(qint64, target);
        QSettings().remove("seeking/keyframes");
        PlayerWindow window;
        window.show();
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *video = window.findChild<VideoSurface *>();
        auto *slider = window.findChild<SeekSlider *>("timeline");
        QTRY_VERIFY(player->isPlaying() && video->videoSink()->videoFrame().isValid());
        auto *gamma = window.findChild<QSlider *>("gamma");
        const auto resetGamma = qScopeGuard([&] { gamma->setValue(10); });
        gamma->setValue(gammaValue);
        if (!playing) player->pause();
        QVideoFrame staleFrame(QVideoFrameFormat(QSize(4, 4), QVideoFrameFormat::Format_RGBA8888));
        QVERIFY(staleFrame.map(QVideoFrame::WriteOnly));
        std::fill(staleFrame.bits(0), staleFrame.bits(0) + staleFrame.mappedBytes(0), uchar(0));
        staleFrame.unmap();
        staleFrame.setStartTime(1000000);
        staleFrame.setEndTime(1040000);
        qint64 resumedFrame = -1;
        QObject observer;
        connect(player, &QMediaPlayer::playbackStateChanged, &observer, [&](auto state) {
            if (state == QMediaPlayer::PlayingState)
                resumedFrame = video->videoSink()->videoFrame().startTime() / 1000;
        });
        slider->setSliderDown(true);
        slider->setValue(qRound(slider->maximum() * 0.125));
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        // Supersede that seek before its frame is delivered, then simulate a late old frame.
        slider->setValue(qRound(target * double(slider->maximum()) / player->duration()));
        video->videoSink()->setVideoFrame(staleFrame);
        slider->setSliderDown(false);
        QTRY_VERIFY(qAbs(video->videoSink()->videoFrame().startTime() / 1000 - target) < 80);
        if (playing) {
            QTRY_VERIFY(player->isPlaying());
            QVERIFY(qAbs(resumedFrame - target) < 80);
            const qint64 position = player->position();
            QTRY_VERIFY(player->position() > position + 100);
        } else {
            QTest::qWait(100);
            QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
            QCOMPARE(resumedFrame, qint64(-1));
        }
    }
    void pauseDuringSeek() {
        QSettings().remove("seeking/keyframes");
        PlayerWindow window;
        window.show();
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *slider = window.findChild<SeekSlider *>("timeline");
        auto *video = window.findChild<VideoSurface *>();
        QTRY_VERIFY(player->isPlaying());
        slider->setValue(qRound(slider->maximum() * 0.25));
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        window.findChild<QAction *>("playPause")->trigger();
        QTRY_VERIFY(qAbs(video->videoSink()->videoFrame().startTime() / 1000 - 2000) < 80);
        QTest::qWait(100);
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        window.findChild<QAction *>("playPause")->trigger();
        QTRY_VERIFY(player->isPlaying());
    }
    void seekBackFromEnd_data() {
        QTest::addColumn<bool>("keyframes");
        QTest::addColumn<bool>("arrow");
        QTest::addColumn<bool>("naturalEnd");
        for (bool keyframes : {false, true})
            for (bool arrow : {false, true})
                for (bool naturalEnd : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3")
                        .arg(keyframes ? "keyframe" : "precise")
                        .arg(arrow ? "arrow" : "timeline")
                        .arg(naturalEnd ? "finished" : "seek-end"))) << keyframes << arrow << naturalEnd;
    }
    void seekBackFromEnd() {
        QFETCH(bool, keyframes);
        QFETCH(bool, arrow);
        QFETCH(bool, naturalEnd);
        QSettings().setValue("seeking/keyframes", keyframes);
        PlayerWindow window;
        window.show();
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *slider = window.findChild<SeekSlider *>("timeline");
        QTRY_VERIFY(player->isPlaying() && slider->isEnabled());
        if (keyframes) QTRY_VERIFY(window.findChild<KeyframeIndex *>()->isReady());
        int timelineTarget = slider->maximum() / 2;
        const auto seekBack = [&] {
            if (arrow) window.findChild<QAction *>("back3")->trigger();
            else {
                slider->setSliderDown(true);
                slider->setValue(timelineTarget);
                slider->setSliderDown(false);
            }
        };
        player->pause();
        player->setPosition(6500);
        seekBack();
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        if (naturalEnd) {
            if (keyframes && !arrow) {
                // Resume even when a held drag stays within the last keyframe interval.
                slider->setSliderDown(true);
                slider->setValue(qRound(slider->maximum() * 0.95));
                timelineTarget = qRound(slider->maximum() * 0.9);
            }
            player->setPosition(7700);
            player->play();
            QTRY_COMPARE_WITH_TIMEOUT(player->mediaStatus(), QMediaPlayer::EndOfMedia, 5000);
        } else {
            window.findChild<QAction *>("forward30")->trigger();
            QCOMPARE(player->position(), player->duration());
        }
        seekBack();
        QTRY_VERIFY(player->isPlaying());
        const qint64 target = arrow ? 5000 : (keyframes && naturalEnd ? 7000 : 4000);
        QVERIFY(qAbs(player->position() - target) < 250);
        const auto position = player->position();
        QTRY_VERIFY(player->position() > position + 100);
        QSettings().remove("seeking/keyframes");
    }
    void draggingMovesWindow() {
        PlayerWindow window;
        window.show();
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *video = window.findChild<VideoSurface *>();
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
        auto *video = window.findChild<VideoSurface *>();
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
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::qWait(0); // Process native focus delivery before sending shortcut keys.
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
        wheel(window.findChild<VideoSurface *>(), 120);
        QCOMPARE(volume->value(), 75);
        wheel(volume, -120);
        QCOMPARE(volume->value(), 70);
        wheel(&window, 120 * 30);
        QCOMPARE(volume->value(), 100);
        wheel(&window, -120 * 30);
        QCOMPARE(volume->value(), 0);
        QCOMPARE(audio->volume(), 0.f);
        for (int expected = 1; expected <= 5; ++expected) {
            QTest::keyClick(&window, Qt::Key_Up);
            QCOMPARE(volume->value(), expected);
        }
        QTest::keyClick(&window, Qt::Key_Up);
        QCOMPARE(volume->value(), 10);
        QTest::keyClick(&window, Qt::Key_Down);
        QCOMPARE(volume->value(), 5);
        for (int expected = 4; expected >= 0; --expected) {
            QTest::keyClick(&window, Qt::Key_Down);
            QCOMPARE(volume->value(), expected);
        }
        wheel(volume, 120 * 7);
        QCOMPARE(volume->value(), 15);
        wheel(volume, -120 * 4);
        QCOMPARE(volume->value(), 3);
        volume->setValue(6);
        wheel(volume, -120);
        QCOMPARE(volume->value(), 5);
        wheel(volume, -120);
        QCOMPARE(volume->value(), 4);
        QVERIFY(qAbs(audio->volume() - 0.04f) < 0.001f);
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
        QString identity = QFileInfo(first).canonicalFilePath();
#ifdef Q_OS_WIN
        identity = identity.toCaseFolded();
#endif
        const QString legacyVolume = "videos/" + QString::fromLatin1(QCryptographicHash::hash(
            identity.toUtf8(), QCryptographicHash::Sha256).toHex()) + "/volume";
        QSettings().setValue(legacyVolume, 12);
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
            QCOMPARE(volume->value(), 85);
            QCOMPARE(gamma->value(), 23);
            QCOMPARE(window.findChild<QAudioOutput *>()->volume(), 0.85f);
            QCOMPARE(window.findChild<QLabel *>("volumeReadout")->text(), QString("Volume 85"));
            QCOMPARE(window.findChild<QLabel *>("gammaReadout")->text(), QString("Gamma 2.3"));
            // A missing file must not change the active video's settings.
            window.openFile(temp.filePath("missing settings.mp4"));
            QCOMPARE(volume->value(), 85);
            QCOMPARE(gamma->value(), 23);
            window.close();
        }
        {
            PlayerWindow window;
            auto *volume = window.findChild<QSlider *>("volume");
            auto *gamma = window.findChild<QSlider *>("gamma");
            auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
            auto *video = window.findChild<VideoSurface *>();
            window.openFile(first);
            QCOMPARE(volume->value(), 85);
            QCOMPARE(gamma->value(), 23);
            QCOMPARE(player->videoSink() == video->videoSink(), video->usesGpuGamma());
            window.openFile(second);
            QCOMPARE(volume->value(), 85);
            QCOMPARE(gamma->value(), 10);
            QCOMPARE(player->videoSink(), video->videoSink());
            QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
            window.openFile(first);
            QCOMPARE(volume->value(), 85);
            QCOMPARE(gamma->value(), 23);
            gamma->setValue(10);
            window.close();
        }
        PlayerWindow reopened;
        reopened.openFile(first);
        QCOMPARE(reopened.findChild<QSlider *>("gamma")->value(), 10);
        QCOMPARE(reopened.findChild<QSlider *>("volume")->value(), 85);
        QCOMPARE(QSettings().value(legacyVolume).toInt(), 12); // Old per-video values are neither read nor updated.
        QCOMPARE(reopened.findChild<QMediaPlayer *>("mediaPlayer")->videoSink(),
            reopened.findChild<VideoSurface *>()->videoSink());
        QSettings().remove("audio/volume");
    }
    void trackSelectionsPersist() {
        const QString movie = temp.filePath("remember tracks.mkv");
        const QString captions = temp.filePath("remember tracks.srt");
        const QString audioFile = temp.filePath("remember tracks.wav");
        QFile srt(captions);
        QVERIFY(srt.open(QIODevice::WriteOnly));
        srt.write("1\n00:00:00,000 --> 00:00:08,000\nRemembered caption\n");
        srt.close();
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-i", clip, "-i", captions,
            "-map", "0:v", "-map", "0:a", "-map", "0:a", "-map", "1:0", "-c", "copy",
            "-c:s", "srt", "-y", movie});
        QVERIFY(ffmpeg.waitForFinished(30000));
        QCOMPARE(ffmpeg.exitCode(), 0);
        ffmpeg.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-i", clip,
            "-vn", "-c:a", "pcm_s16le", "-y", audioFile});
        QVERIFY(ffmpeg.waitForFinished(30000));
        QCOMPARE(ffmpeg.exitCode(), 0);
        QString identity = QFileInfo(movie).canonicalFilePath();
#ifdef Q_OS_WIN
        identity = identity.toCaseFolded();
#endif
        const QString settingsKey = "videos/" + QString::fromLatin1(QCryptographicHash::hash(
            identity.toUtf8(), QCryptographicHash::Sha256).toHex());
        {
            PlayerWindow window;
            window.show();
            auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
            auto *subtitles = window.findChild<EmbeddedSubtitles *>();
            window.openFile(movie);
            QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
            QTRY_COMPARE(player->audioTracks().size(), 2);
            QTRY_COMPARE(player->subtitleTracks().size(), 1);
            QVERIFY(selectTrack(window, 1, Qt::Key_A));
            QVERIFY(selectSubtitle(window, 0));
            window.openFile(clip);
            QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
            QCOMPARE(player->activeAudioTrack(), 0);
            QCOMPARE(subtitles->activeTrack(), -1);
            window.openFile(movie);
            QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
            QTRY_COMPARE(player->activeAudioTrack(), 1);
            QTRY_COMPARE(subtitles->activeTrack(), 0);
            window.close();
        }
        // INI files do not preserve the numeric type across application restarts.
        QSettings().setValue(settingsKey + "/audioTrack", "1");
        QSettings().setValue(settingsKey + "/subtitleTrack", "0");
        {
            PlayerWindow window;
            window.show();
            auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
            window.openFile(movie);
            QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
            QTRY_COMPARE(player->activeAudioTrack(), 1);
            QTRY_COMPARE(window.findChild<EmbeddedSubtitles *>()->activeTrack(), 0);
            QVERIFY(selectTrack(window, audioFile, Qt::Key_A));
            QVERIFY(selectSubtitle(window, captions));
            window.close();
        }
        {
            PlayerWindow window;
            window.show();
            auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
            window.openFile(movie);
            QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
            QTRY_COMPARE(window.findChild<ExternalAudio *>()->filePath(), audioFile);
            QTRY_COMPARE(subtitleText(window), QString("Remembered caption"));
            QCOMPARE(window.findChild<EmbeddedSubtitles *>()->activeTrack(), -1);
            QVERIFY(selectSubtitle(window, -1));
            window.close();
        }
        QVERIFY(QFile::remove(audioFile));
        {
            PlayerWindow window;
            window.show();
            auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
            window.openFile(movie);
            QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
            QVERIFY(window.findChild<ExternalAudio *>()->filePath().isEmpty());
            QCOMPARE(player->activeAudioTrack(), 0);
            QCOMPARE(window.findChild<EmbeddedSubtitles *>()->activeTrack(), -1);
            QVERIFY(subtitleText(window).isEmpty());
            QVERIFY(selectSubtitle(window, captions));
            window.close();
        }
        QVERIFY(QFile::remove(captions));
        PlayerWindow window;
        window.openFile(movie);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 10000);
        QCOMPARE(window.findChild<EmbeddedSubtitles *>()->activeTrack(), -1);
        QVERIFY(subtitleText(window).isEmpty());
        window.close();
        QSettings().setValue(settingsKey + "/audioTrack", 99);
        QSettings().setValue(settingsKey + "/subtitleTrack", 99);
        PlayerWindow changed;
        changed.openFile(movie);
        auto *changedPlayer = changed.findChild<QMediaPlayer *>("mediaPlayer");
        QTRY_VERIFY_WITH_TIMEOUT(changedPlayer->isPlaying(), 10000);
        QCOMPARE(changedPlayer->activeAudioTrack(), 0);
        QCOMPARE(changed.findChild<EmbeddedSubtitles *>()->activeTrack(), -1);
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
        if (window.findChild<VideoSurface *>()->usesGpuGamma())
            QSKIP("GPU rendering is checked by gpuGammaPlayback");
        window.show();
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *video = window.findChild<VideoSurface *>();
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
    void cpuGammaFallback() {
        const QByteArray previous = qgetenv("VIDEO_PLAYER_CPU_GAMMA");
        const auto restore = qScopeGuard([&] {
            if (previous.isNull()) qunsetenv("VIDEO_PLAYER_CPU_GAMMA");
            else qputenv("VIDEO_PLAYER_CPU_GAMMA", previous);
        });
        qputenv("VIDEO_PLAYER_CPU_GAMMA", "1");
        VideoSurface video;
        QMediaPlayer player;
        video.setPlayer(&player);
        QVERIFY(!video.usesGpuGamma());
        QVideoFrame frame(QVideoFrameFormat(QSize(4, 4), QVideoFrameFormat::Format_RGBA8888));
        QVERIFY(frame.map(QVideoFrame::WriteOnly));
        std::fill(frame.bits(0), frame.bits(0) + frame.mappedBytes(0), uchar(64));
        frame.unmap();
        video.videoSink()->setVideoFrame(frame);
        video.setGamma(20);
        QVERIFY(player.videoSink() != video.videoSink());
        QVERIFY(video.videoSink()->videoFrame().toImage() != frame.toImage());
        video.setGamma(10);
        QCOMPARE(player.videoSink(), video.videoSink());
        QCOMPARE(video.videoSink()->videoFrame().toImage(), frame.toImage());
    }
    void gpuDownscalePlayback() {
        const QString example = qEnvironmentVariable("VIDEO_PLAYER_GPU_SCALE_EXAMPLE");
        if (example.isEmpty()) QSKIP("Set VIDEO_PLAYER_GPU_SCALE_EXAMPLE to a 4K SDR video");
        const QByteArray previous = qgetenv("VIDEO_PLAYER_GPU_MAX_WIDTH");
        const int cap = previous.toInt() > 0 ? previous.toInt() : 1920;
        qputenv("VIDEO_PLAYER_GPU_MAX_WIDTH", QByteArray::number(cap));
        const auto restore = qScopeGuard([&] {
            if (previous.isNull()) qunsetenv("VIDEO_PLAYER_GPU_MAX_WIDTH");
            else qputenv("VIDEO_PLAYER_GPU_MAX_WIDTH", previous);
        });
        PlayerWindow window;
        auto *video = window.findChild<VideoSurface *>();
        if (!video->usesGpuGamma()) QSKIP("A GPU-backed desktop is required");
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        window.openFile(example);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 15000);
        player->pause();
        player->setPosition(1234);
        QTRY_VERIFY_WITH_TIMEOUT(video->videoSink()->videoFrame().width() == cap
            && video->videoSink()->videoFrame().startTime() <= 1234000
            && video->videoSink()->videoFrame().endTime() >= 1234000, 10000);
        const QVideoFrame scaled = video->videoSink()->videoFrame();
        const QImage image = scaled.toImage();
        QCOMPARE(image.size(), scaled.size());
        QVERIFY(!image.isNull());
        qputenv("VIDEO_PLAYER_GPU_MAX_WIDTH", "0");
        qint64 insideFrame = scaled.startTime() / 1000 + 1;
        if (insideFrame == player->position()) ++insideFrame;
        player->setPosition(insideFrame);
        QTRY_VERIFY_WITH_TIMEOUT(video->videoSink()->videoFrame().width() > scaled.width(), 10000);
        const QVideoFrame original = video->videoSink()->videoFrame();
        QCOMPARE(original.startTime(), scaled.startTime());
        const QImage reference = original.toImage().scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        double error = 0;
        int samples = 0;
        for (int y = 0; y < image.height(); y += 3)
            for (int x = 0; x < image.width(); x += 3) {
                const QColor a = image.pixelColor(x, y), b = reference.pixelColor(x, y);
                error += qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue());
                samples += 3;
            }
        qInfo() << "GPU downscale RGB mean absolute error:" << error / samples;
        QVERIFY2(error / samples < 4, "GPU downscale must preserve SDR colors and image detail");
    }
    void startupDiagnostics() {
        const QString example = qEnvironmentVariable("VIDEO_PLAYER_STARTUP_EXAMPLE");
        if (example.isEmpty()) QSKIP("Set VIDEO_PLAYER_STARTUP_EXAMPLE to a local video");
        PlayerWindow window;
        auto *video = window.findChild<VideoSurface *>();
        auto *quick = video->quickView();
        if (!quick) QSKIP("A GPU-backed desktop is required");
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        QElapsedTimer clock;
        clock.start();
        QJsonArray frames, events, pulses;
        struct RenderLog {
            QMutex mutex;
            QJsonArray renders;
            double start = 0;
            std::atomic<qint64> pts{-1};
        };
        // Direct render callbacks run on the scene graph thread. Keep their
        // storage alive through disconnect and read frame timestamps atomically.
        const auto renderLog = std::make_shared<RenderLog>();
        const auto now = [&] { return clock.nsecsElapsed() / 1e6; };
        QObject observer; // Disconnect before the report arrays are destroyed.
        connect(video, &VideoSurface::videoFrameChanged, &observer, [&](const QVideoFrame &frame) {
            renderLog->pts = frame.startTime();
            if (frame.isValid()) frames.append(QJsonObject{{"ms", now()}, {"pts_ms", frame.startTime() / 1000.0},
                {"width", frame.width()}, {"height", frame.height()},
                {"texture", frame.handleType() == QVideoFrame::RhiTextureHandle}});
        });
        connect(quick, &QQuickWindow::beforeFrameBegin, &observer, [renderLog, clock] {
            renderLog->start = clock.nsecsElapsed() / 1e6;
        }, Qt::DirectConnection);
        connect(quick, &QQuickWindow::afterFrameEnd, &observer, [renderLog, clock] {
            const double end = clock.nsecsElapsed() / 1e6;
            QMutexLocker lock(&renderLog->mutex);
            renderLog->renders.append(QJsonObject{{"ms", end}, {"render_ms", end - renderLog->start},
                {"pts_ms", renderLog->pts.load() / 1000.0}});
        }, Qt::DirectConnection);
        connect(player, &QMediaPlayer::mediaStatusChanged, &observer, [&](auto status) {
            events.append(QJsonObject{{"ms", now()}, {"status", int(status)}, {"position", player->position()}});
        });
        connect(player, &QMediaPlayer::playbackStateChanged, &observer, [&](auto state) {
            events.append(QJsonObject{{"ms", now()}, {"state", int(state)}, {"position", player->position()}});
        });
        connect(quick, &QQuickWindow::sceneGraphInitialized, &observer, [&] {
            events.append(QJsonObject{{"ms", now()}, {"event", "sceneGraphInitialized"}});
        }, Qt::QueuedConnection);
        QTimer pulse;
        pulse.setTimerType(Qt::PreciseTimer);
        pulse.setInterval(10);
        connect(&pulse, &QTimer::timeout, &window, [&] { pulses.append(now()); });
        pulse.start();
        if (qEnvironmentVariableIntValue("VIDEO_PLAYER_STARTUP_ZERO")) {
            QString identity = QFileInfo(example).canonicalFilePath();
#ifdef Q_OS_WIN
            identity = identity.toCaseFolded();
#endif
            const QString key = QString::fromLatin1(QCryptographicHash::hash(
                identity.toUtf8(), QCryptographicHash::Sha256).toHex());
            QSettings().setValue("positions/" + key, 0);
        }
        window.openFile(example);
        if (qEnvironmentVariableIsSet("VIDEO_PLAYER_STARTUP_GAMMA"))
            window.findChild<QSlider *>("gamma")->setValue(qEnvironmentVariableIntValue("VIDEO_PLAYER_STARTUP_GAMMA"));
        window.show();
        if (qEnvironmentVariableIsSet("VIDEO_PLAYER_SEEK_DIAGNOSTIC_MS")) {
            const qint64 target = qEnvironmentVariable("VIDEO_PLAYER_SEEK_DIAGNOSTIC_MS").toLongLong();
            QTimer::singleShot(3000, &observer, [&, target] {
                events.append(QJsonObject{{"ms", now()}, {"event", "seekRequested"}, {"target_ms", target}});
                auto *slider = window.findChild<SeekSlider *>("timeline");
                slider->setValue(qRound(target * double(slider->maximum()) / player->duration()));
            });
        }
        QEventLoop loop;
        QTimer::singleShot(9000, &loop, &QEventLoop::quit);
        loop.exec();
        pulse.stop();
        QJsonArray renders;
        {
            QMutexLocker lock(&renderLog->mutex);
            renders = renderLog->renders;
        }
        const QJsonObject report{{"frames", frames}, {"renders", renders}, {"events", events}, {"pulses_ms", pulses},
            {"surface_width", video->width()}, {"surface_height", video->height()},
            {"device_pixel_ratio", video->devicePixelRatioF()}};
        const QString path = qEnvironmentVariable("VIDEO_PLAYER_STARTUP_REPORT");
        QVERIFY(!frames.isEmpty());
        if (!path.isEmpty()) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write(QJsonDocument(report).toJson()) > 0);
        }
    }
    void gpuGammaPlayback() {
        PlayerWindow window;
        auto *video = window.findChild<VideoSurface *>();
        auto *renderer = video->quickView();
        if (!renderer) QSKIP("A GPU-backed desktop is required");
        window.show();
        const QString example = qEnvironmentVariable("VIDEO_PLAYER_GPU_GAMMA_EXAMPLE");
        window.openFile(example.isEmpty() ? clip : example);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *gamma = window.findChild<QSlider *>("gamma");
        QTRY_VERIFY(player->isPlaying() && video->isVisible());
        player->pause();
        gamma->setValue(10);
        QTest::qWait(150);
        QVERIFY(video->usesGpuGamma());
        const QImage original = video->grabFrame();
        QVERIFY(!original.isNull());
        const qint64 position = player->position();
        gamma->setValue(16);
        QTest::qWait(150);
        const QImage corrected = video->grabFrame();
        QCOMPARE(corrected.size(), original.size());
        QVERIFY(corrected != original);
        double error = 0;
        int samples = 0;
        for (int y = original.height() / 4; y < original.height() * 3 / 4; y += 7)
            for (int x = original.width() / 4; x < original.width() * 3 / 4; x += 7) {
                const QColor before = original.pixelColor(x, y), after = corrected.pixelColor(x, y);
                for (auto channel : {&QColor::red, &QColor::green, &QColor::blue}) {
                    error += qAbs((after.*channel)()
                        - qRound(255 * std::pow((before.*channel)() / 255.0, 1 / 1.6)));
                    ++samples;
                }
            }
        QVERIFY2(error / samples < 3, "GPU gamma must match the existing SDR gamma formula");
        QCOMPARE(player->position(), position);
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QCOMPARE(player->videoSink(), video->videoSink());
        gamma->setValue(10);
        QTRY_COMPARE(video->grabFrame(), original);
        player->setPosition(1000);
        QTRY_VERIFY(qAbs(video->videoSink()->videoFrame().startTime() / 1000 - 1000) < 100);
        // Window shortcuts require an active top-level window. Xvfb has no
        // window manager to activate it automatically after show().
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::keyClick(renderer, Qt::Key_F);
        QTRY_VERIFY(window.isFullScreen());
        const QSize size = video->videoSink()->videoSize();
        QTRY_VERIFY(qAbs(double(video->contentRect().width()) / video->contentRect().height()
            - double(size.width()) / size.height()) < 0.01);
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::keyClick(renderer, Qt::Key_F);
        QTRY_VERIFY(!window.isFullScreen());
        player->play();
        QTRY_VERIFY(player->isPlaying());
        gamma->setValue(16);
        QTRY_VERIFY(player->position() > 1300);
        QVERIFY(video->usesGpuGamma());
        const qint64 beforeFallback = player->position();
        QVERIFY(QMetaObject::invokeMethod(renderer->rootObject(), "shaderError"));
        QTRY_VERIFY(!video->usesGpuGamma());
        QVERIFY(player->isPlaying());
        QVERIFY(player->position() >= beforeFallback);
        QVERIFY(player->videoSink() != video->videoSink());
        QTRY_VERIFY(video->videoSink()->videoFrame().isValid());
        gamma->setValue(10);
    }
    void nativeVideoInput() {
        const QPoint cursor = QCursor::pos();
        const auto restoreCursor = qScopeGuard([&] { QCursor::setPos(cursor); });
        PlayerWindow window;
        auto *video = window.findChild<VideoSurface *>();
        auto *view = video->quickView();
        if (!view) QSKIP("A GPU-backed desktop is required");
        window.openFile(clip);
        window.show();
        QVERIFY(waitForVideoExposed(window));
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        QTRY_VERIFY(player->isPlaying());
        const auto menuOpens = [&] {
            return withPopupMenu(*video, [&](QMenu &menu) {
                QVERIFY(menu.findChild<QAction *>("playPause")->isEnabled());
                QTest::keyClick(&menu, Qt::Key_Escape);
            }, view);
        };
        QVERIFY(menuOpens());
        QVERIFY(player->isPlaying());
        QTest::keyClick(view, Qt::Key_Space);
        QTRY_VERIFY(!player->isPlaying());
        QTest::keyClick(view, Qt::Key_Space);
        QTRY_VERIFY(player->isPlaying());
        QTest::mouseClick(view, Qt::LeftButton);
        QTRY_VERIFY(!player->isPlaying());
        auto *volume = window.findChild<QSlider *>("volume");
        volume->setValue(50);
        QWheelEvent wheel(QPointF(10, 10), view->mapToGlobal(QPoint(10, 10)), {},
            QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view, &wheel);
        QCOMPARE(volume->value(), 55);
        QVERIFY(menuOpens());
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QTest::mouseDClick(view, Qt::LeftButton);
        QTRY_VERIFY(window.isFullScreen());
        QVERIFY(menuOpens());
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        QTest::mouseDClick(view, Qt::LeftButton);
        QTRY_VERIFY(!window.isFullScreen());

        const QString dropped = temp.filePath("native-drop.mp4");
        QVERIFY(QFile::copy(clip, dropped));
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(dropped)});
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view, &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(player->source(), QUrl::fromLocalFile(dropped));
        QTRY_VERIFY(player->isPlaying());
    }
    void fullscreenControlsAtBottom() {
        PlayerWindow window;
        window.openFile(clip);
        window.showFullScreen();
        QVERIFY(waitForVideoExposed(window));
        auto *video = window.findChild<VideoSurface *>();
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
        QVERIFY(waitForVideoExposed(window));
        auto *video = window.findChild<VideoSurface *>();
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
        const QPoint cursor = QCursor::pos();
        const auto restoreCursor = qScopeGuard([&] { QCursor::setPos(cursor); });
        PlayerWindow window;
        window.setGeometry(40, 50, 720, 480);
        window.show();
        QCursor::setPos(window.mapToGlobal(window.rect().center()));
        window.openFile(clip);
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        auto *video = window.findChild<VideoSurface *>();
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
            // Keep a short fixture from ending during native fullscreen transitions.
            if (player->isPlaying()) player->setPosition(0);
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
    void initialPlaybackPosition_data() {
        QTest::addColumn<qint64>("saved");
        QTest::addColumn<qint64>("expected");
        QTest::newRow("new-video") << qint64(-1) << qint64(4000);
        QTest::newRow("saved-zero") << qint64(0) << qint64(0);
        QTest::newRow("saved-position") << qint64(2500) << qint64(2500);
        QTest::newRow("saved-end") << qint64(8000) << qint64(0);
        QTest::newRow("saved-beyond-end") << qint64(12000) << qint64(0);
    }
    void initialPlaybackPosition() {
        QFETCH(qint64, saved);
        QFETCH(qint64, expected);
        const QString path = temp.filePath(QString("initial-%1.mp4").arg(saved));
        QVERIFY(QFile::copy(clip, path));
        QString identity = QFileInfo(path).canonicalFilePath();
#ifdef Q_OS_WIN
        identity = identity.toCaseFolded();
#endif
        const QString key = "positions/" + QString::fromLatin1(QCryptographicHash::hash(
            identity.toUtf8(), QCryptographicHash::Sha256).toHex());
        if (saved >= 0) QSettings().setValue(key, saved);
        {
            PlayerWindow window;
            window.openFile(path);
            auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
            QTRY_VERIFY(player->isPlaying() && player->position() >= expected
                && player->videoSink()->videoFrame().startTime() / 1000 >= expected);
            QVERIFY(player->position() < expected + 500);
            player->pause();
            player->setPosition(1250);
            window.close();
        }
        PlayerWindow reopened;
        reopened.openFile(path);
        auto *player = reopened.findChild<QMediaPlayer *>("mediaPlayer");
        QTRY_VERIFY(player->isPlaying() && player->position() >= 1250);
        QVERIFY(player->position() < 1750);
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
