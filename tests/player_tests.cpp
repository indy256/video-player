#include "playerwindow.h"
#include "seekslider.h"
#include "gammafilter.h"
#include <QApplication>
#include <QAbstractButton>
#include <QFile>
#include <QLabel>
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
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QMenu>
#include <QMediaMetaData>
#include <QTimer>
#ifdef Q_OS_WIN
#include <qt_windows.h>
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
            bool found = false;
            QTimer::singleShot(0, &window, [&] {
                auto *menu = window.findChild<QMenu *>();
                if (menu) {
                    for (auto *action : menu->actions())
                        if (action->text() == "Update to latest version") found = action->isEnabled();
                    menu->close();
                }
            });
            QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(10, 10), surface->mapToGlobal(QPoint(10, 10)));
            QApplication::sendEvent(surface, &event);
            QVERIFY(found);
        }
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
        window.activateWindow();
        QTRY_VERIFY(window.isActiveWindow());
        auto *player = window.findChild<QMediaPlayer *>("mediaPlayer");
        bool emptyMessage = false;
        QTimer::singleShot(0, &window, [&] {
            if (auto *menu = window.findChild<QMenu *>("audioTrackMenu")) {
                for (auto *action : menu->actions())
                    if (action->text() == "No audio tracks available") emptyMessage = !action->isEnabled();
                menu->close();
            }
        });
        QTest::keyClick(&window, Qt::Key_A);
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
            window.activateWindow();
            QTRY_VERIFY(window.isActiveWindow());
            const int previous = player->activeAudioTrack();
            int count = 0;
            int checked = -1;
            bool named = false;
            QTimer::singleShot(0, &window, [&] {
                auto *menu = window.findChild<QMenu *>("audioTrackMenu");
                if (!menu) return;
                QAction *selection = nullptr;
                for (auto *action : menu->actions()) {
                    if (!action->data().isValid()) continue;
                    ++count;
                    if (action->isChecked()) checked = action->data().toInt();
                    if (action->data().toInt() == 1) named = action->text().contains("Commentary");
                    if (action->data().toInt() == chosen) selection = action;
                }
                if (selection) {
                    menu->setActiveAction(selection);
                    QTest::keyClick(menu, Qt::Key_Return);
                } else QTest::keyClick(menu, Qt::Key_Escape);
            });
            QTest::keyClick(&window, Qt::Key_A);
            QCOMPARE(count, 2);
            QCOMPARE(checked, previous);
            QVERIFY(named);
            QCOMPARE(player->activeAudioTrack(), chosen < 0 ? previous : chosen);
            QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
            QVERIFY(qAbs(player->position() - position) < 100);
            QVERIFY(window.isVisible());
        }
        player->play();
        QTRY_VERIFY(player->position() > position + 300);
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
        QCursor::setPos(window.mapToGlobal(QPoint(window.width() / 2, window.height() - 1)));
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
