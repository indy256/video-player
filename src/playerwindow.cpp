#include "playerwindow.h"
#include "seekslider.h"
#include "updater.h"
#include <QContextMenuEvent>
#include <QMenu>
#include <QAudioOutput>
#include <QApplication>
#include <QTimer>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QSettings>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVideoWidget>
#include <QVideoSink>
#include <QVideoFrame>
#include <QPalette>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
constexpr int timelineSteps = 1000000;
QString timestamp(qint64 milliseconds) {
    const qint64 seconds = qMax(qint64(0), milliseconds) / 1000;
    return QString("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
}

PlayerWindow::PlayerWindow(QWidget *parent) : QMainWindow(parent) {
    QPalette blackPalette = palette();
    blackPalette.setColor(QPalette::Window, Qt::black);
    blackPalette.setColor(QPalette::Base, Qt::black);
    setPalette(blackPalette);
    setAutoFillBackground(true);
    setWindowTitle(QStringLiteral("Video Player v" VIDEO_PLAYER_VERSION));
    resize(1100, 740);
    setMinimumSize(680, 440);
    setAcceptDrops(true);
    player = new QMediaPlayer(this);
    player->setObjectName("mediaPlayer");
    audio = new QAudioOutput(this);
    const int savedVolume = qBound(0, QSettings().value("audio/volume", 70).toInt(), 100);
    audio->setVolume(savedVolume / 100.f);
    player->setAudioOutput(audio);
    auto *page = new QWidget(this);
    setCentralWidget(page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 12);
    layout->setSpacing(16);
    auto *openButton = new QPushButton("Open video");
    openButton->setObjectName("primary");
    openButton->setToolTip("Open a video (Ctrl+O)");
    stage = new QStackedWidget;
    auto *empty = new QWidget;
    empty->installEventFilter(this);
    empty->setObjectName("emptyStage");
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->addStretch();
    auto *headline = new QLabel("Ready when you are");
    headline->setAlignment(Qt::AlignCenter);
    headline->setStyleSheet("font-size: 28px; color: #eef2ff; font-weight: 600;");
    auto *hint = new QLabel("Drop a video here or open a file to start watching");
    hint->setAlignment(Qt::AlignCenter);
    auto *emptyOpen = new QPushButton("Choose a video");
    emptyOpen->setFixedWidth(170);
    emptyLayout->addWidget(headline);
    emptyLayout->addWidget(hint);
    statusLabel = new QLabel;
    statusLabel->setObjectName("status");
    statusLabel->setTextFormat(Qt::PlainText);
    statusLabel->setWordWrap(true);
    statusLabel->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(statusLabel);
    emptyLayout->addSpacing(16);
    emptyLayout->addWidget(emptyOpen, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();
    stage->addWidget(empty);
    video = new QVideoWidget;
    clickTimer = new QTimer(this);
    clickTimer->setSingleShot(true);
    clickTimer->setTimerType(Qt::PreciseTimer);
    connect(clickTimer, &QTimer::timeout, this, &PlayerWindow::togglePlayback);
    video->installEventFilter(this);
    video->setAspectRatioMode(Qt::KeepAspectRatio);
    stage->addWidget(video);
    auto *loading = new QWidget;
    loading->installEventFilter(this);
    loading->setObjectName("loadingStage");
    loading->setPalette(blackPalette);
    loading->setAutoFillBackground(true);
    stage->addWidget(loading);
    connect(video->videoSink(), &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        if (frame.isValid() && !frameReady) {
            frameReady = true;
            updateFullscreen();
        }
    });
    player->setVideoOutput(video);
    layout->addWidget(stage, 1);
    timeline = new SeekSlider;
    timeline->setObjectName("timeline");
    timeline->setFixedHeight(14);
    timeline->setAccessibleName("Video position");
    timeline->setRange(0, timelineSteps);
    timeline->setToolTip("Click or drag to seek");
    layout->addWidget(timeline);
    auto *controls = new QHBoxLayout;
    controls->addWidget(openButton);
    controls->addStretch();
    volume = new QSlider(Qt::Horizontal);
    volume->setObjectName("volume");
    volume->setSingleStep(5);
    volume->installEventFilter(this);
    timeline->installEventFilter(this);
    volume->setAccessibleName("Volume");
    volume->setRange(0, 100);
    volume->setValue(savedVolume);
    volume->setFixedWidth(100);
    controls->addWidget(volume);
    layout->addLayout(controls);
    setStyleSheet(R"(
        QMainWindow, QWidget { background: #000000; color: #939eb4; font-family: 'Segoe UI'; font-size: 13px; }
        QWidget#emptyStage { background: #000000; border: 1px solid #252b3a; border-radius: 12px; }
        QWidget#emptyStage QLabel { background: transparent; border: none; }
        QPushButton { background: #242b3b; color: #e4eaf8; border: 1px solid #333e53; border-radius: 7px; padding: 9px 16px; }
        QPushButton:hover { background: #33415b; }
        QPushButton:focus { border-color: #94b4ff; }
        QPushButton:disabled { color: #596173; background: #1b202c; border-color: #252c39; }
        QPushButton#primary { background: #759eff; color: #0d1830; font-weight: 600; }
        QPushButton#primary:hover { background: #9bb8ff; }
        QSlider::groove:horizontal { background: #2b3243; height: 5px; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: #759eff; border-radius: 2px; }
        QSlider::handle:horizontal { background: #e5edff; width: 14px; margin: -5px 0; border-radius: 7px; }
        QSlider:disabled::sub-page:horizontal { background: #2b3243; }
        QSlider#timeline::groove:horizontal { height: 14px; margin: 0; border-radius: 0; }
        QSlider#timeline::sub-page:horizontal { height: 14px; margin: 0; border-radius: 0; }
        QSlider#timeline::handle:horizontal { margin: 0; }
        QToolTip { background: #242b3b; color: #e4eaf8; border: 1px solid #475575; }
        QMenu { background: #242b3b; color: #e4eaf8; border: 1px solid #475575; padding: 4px; }
        QMenu::item { padding: 7px 24px; }
        QMenu::item:selected { background: #33415b; }
        QMenu::item:disabled { color: #596173; }
    )");
    connect(openButton, &QPushButton::clicked, this, &PlayerWindow::chooseFile);
    connect(emptyOpen, &QPushButton::clicked, this, &PlayerWindow::chooseFile);
    connect(volume, &QSlider::valueChanged, this, [this](int value) {
        audio->setVolume(value / 100.f);
        QSettings settings;
        settings.setValue("audio/volume", value);
        settings.sync();
    });
    connect(timeline, &QSlider::valueChanged, this, &PlayerWindow::seekToSlider);
    connect(timeline, &QSlider::sliderReleased, this, &PlayerWindow::updateTimeline);
    connect(player, &QMediaPlayer::positionChanged, this, &PlayerWindow::updateTimeline);
    connect(player, &QMediaPlayer::durationChanged, this, [this] { updateControls(); updateTimeline(); });
    connect(player, &QMediaPlayer::seekableChanged, this, &PlayerWindow::updateControls);
    connect(player, &QMediaPlayer::playbackStateChanged, this, &PlayerWindow::updateControls);
    connect(player, &QMediaPlayer::mediaStatusChanged, this, &PlayerWindow::updateControls);
    connect(player, &QMediaPlayer::mediaStatusChanged, this, &PlayerWindow::restorePosition);
    connect(player, &QMediaPlayer::seekableChanged, this, &PlayerWindow::restorePosition);
    connect(player, &QMediaPlayer::durationChanged, this, &PlayerWindow::restorePosition);
    connect(player, &QMediaPlayer::hasVideoChanged, this, &PlayerWindow::updateFullscreen);
    connect(player, &QMediaPlayer::errorOccurred, this, &PlayerWindow::updateControls);
    auto shortcut = [this](const QKeySequence &key, auto callback) {
        auto *action = new QShortcut(key, this); connect(action, &QShortcut::activated, this, callback);
    };
    shortcut(QKeySequence::Open, &PlayerWindow::chooseFile);
    shortcut(QKeySequence(Qt::Key_Space), &PlayerWindow::togglePlayback);
    shortcut(QKeySequence(Qt::Key_Left), [this] { skip(-10000); });
    shortcut(QKeySequence(Qt::Key_Right), [this] { skip(10000); });
    shortcut(QKeySequence(Qt::Key_Up), [this] { adjustVolume(1); });
    shortcut(QKeySequence(Qt::Key_Down), [this] { adjustVolume(-1); });
    shortcut(QKeySequence(Qt::Key_F11), &PlayerWindow::toggleFullscreen);
    shortcut(QKeySequence(Qt::Key_F), &PlayerWindow::toggleFullscreen);
    shortcut(QKeySequence(Qt::Key_Escape), &PlayerWindow::close);
    updateControls();
    updateTimeline();
#ifdef Q_OS_WIN
    if (QApplication::platformName() == "windows") {
        // Create the HWND while still hidden, before Windows can expose a white surface.
        const HWND handle = reinterpret_cast<HWND>(winId());
        SetClassLongPtrW(handle, GCLP_HBRBACKGROUND,
            reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
    }
#endif
}

#ifdef Q_OS_WIN
bool PlayerWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
    const auto *msg = static_cast<MSG *>(message);
    if (msg->message == WM_ERASEBKGND) {
        RECT rect;
        GetClientRect(msg->hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(msg->wParam), &rect,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        *result = 1;
        return true;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void PlayerWindow::chooseFile() {
    const QString path = QFileDialog::getOpenFileName(this, "Open video", {},
        "Video files (*.mp4 *.mkv *.avi *.mov *.webm *.m4v *.wmv *.mpeg *.mpg *.ts *.m2ts *.ogv);;All files (*)");
    if (!path.isEmpty()) openFile(path);
}

void PlayerWindow::contextMenuEvent(QContextMenuEvent *event) {
    showPopupMenu(event->globalPos());
    event->accept();
}

void PlayerWindow::showPopupMenu(const QPoint &position) {
    clickTimer->stop();
    QMenu menu(this);
    menu.addAction("Open video", this, &PlayerWindow::chooseFile);
    auto *play = menu.addAction(player->isPlaying() ? "Pause" : "Play", this, &PlayerWindow::togglePlayback);
    play->setEnabled(playbackReady());
    menu.addAction(isFullScreen() ? "Leave fullscreen" : "Fullscreen", this, &PlayerWindow::toggleFullscreen);
    menu.addSeparator();
    auto *update = menu.addAction("Update to latest version", this, [this] {
        if (updating) return;
        updating = true;
        const bool installed = AppUpdate::installLatest(this, player->source().toLocalFile());
        updating = false;
        if (installed) {
            savePosition();
            QApplication::quit();
        }
    });
    update->setEnabled(!updating);
    menu.addSeparator();
    menu.addAction("Exit", this, &PlayerWindow::close);
    menu.exec(position);
}

void PlayerWindow::openFile(const QString &path) {
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable()) {
        statusLabel->setText("Cannot open file: " + path);
        return;
    }
    savePosition();
    pendingPosition = -1;
    clickTimer->stop();
    timeline->setSliderDown(false);
    player->stop();
    frameReady = false;
    QString identity = file.canonicalFilePath();
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    positionKey = "positions/" + QString::fromLatin1(QCryptographicHash::hash(
        identity.toUtf8(), QCryptographicHash::Sha256).toHex());
    pendingPosition = QSettings().value(positionKey, 0).toLongLong();
    stage->setCurrentIndex(2);
    setWindowTitle(file.fileName() + QStringLiteral(" - Video Player v" VIDEO_PLAYER_VERSION));
    player->setSource(QUrl::fromLocalFile(file.absoluteFilePath()));
    restorePosition();
    player->play();
}

void PlayerWindow::savePosition() {
    if (positionKey.isEmpty() || pendingPosition >= 0 || !player->isSeekable()
        || player->duration() <= 0 || player->error() != QMediaPlayer::NoError) return;
    QSettings settings;
    settings.setValue(positionKey, player->mediaStatus() == QMediaPlayer::EndOfMedia
        ? qint64(0) : player->position());
    settings.sync();
}

void PlayerWindow::restorePosition() {
    if (pendingPosition < 0 || !player->isSeekable() || player->duration() <= 0
        || player->mediaStatus() == QMediaPlayer::LoadingMedia
        || player->mediaStatus() == QMediaPlayer::NoMedia
        || player->error() != QMediaPlayer::NoError) return;
    const qint64 position = pendingPosition < player->duration() ? pendingPosition : 0;
    pendingPosition = -1;
    player->setPosition(position);
}

void PlayerWindow::closeEvent(QCloseEvent *event) {
    clickTimer->stop();
    savePosition();
    QMainWindow::closeEvent(event);
}

bool PlayerWindow::playbackReady() const {
    const auto status = player->mediaStatus();
    return !player->source().isEmpty() && player->error() == QMediaPlayer::NoError
        && status != QMediaPlayer::LoadingMedia && status != QMediaPlayer::InvalidMedia;
}

void PlayerWindow::togglePlayback() {
    clickTimer->stop();
    if (!playbackReady()) return;
    if (player->isPlaying()) player->pause();
    else {
        if (player->mediaStatus() == QMediaPlayer::EndOfMedia) player->setPosition(0);
        player->play();
    }
}

void PlayerWindow::updateControls() {
    const auto status = player->mediaStatus();
    const bool ready = playbackReady();
    const bool seekable = ready && player->isSeekable() && player->duration() > 0;
    timeline->setEnabled(seekable);
    if (player->error() != QMediaPlayer::NoError) {
        statusLabel->setText("Unable to play: " + player->errorString());
        stage->setCurrentIndex(isFullScreen() ? 1 : 0);
    } else if (status == QMediaPlayer::LoadingMedia) statusLabel->setText("Loading video...");
    else if (status == QMediaPlayer::StalledMedia) statusLabel->setText("Buffering...");
    else if (status == QMediaPlayer::EndOfMedia) statusLabel->setText("Finished - press Space to watch again");
    else if (ready) statusLabel->setText(player->isPlaying() ? "Playing" : "Paused");
    else statusLabel->setText("Open a local video to begin");
}

bool PlayerWindow::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::ContextMenu) {
        const auto *context = static_cast<QContextMenuEvent *>(event);
        showPopupMenu(context->globalPos());
        return true;
    }
    if ((watched == video || watched == timeline || watched == volume) && event->type() == QEvent::Wheel) {
        wheelEvent(static_cast<QWheelEvent *>(event));
        return true;
    }
    const bool dragSurface = watched == video || watched == stage->widget(0) || watched == stage->widget(2);
    if (dragSurface && event->type() == QEvent::MouseMove && mousePressed) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const QPoint delta = mouse->globalPosition().toPoint() - dragStartMouse;
        if (draggingWindow || delta.manhattanLength() >= QApplication::startDragDistance()) {
            if (!draggingWindow) {
                draggingWindow = true;
                clickTimer->stop();
            }
            if (!isFullScreen() && !isMaximized()) move(dragStartWindow + delta);
        }
        return true;
    }
    if (dragSurface && (event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick)) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            if (event->type() == QEvent::MouseButtonDblClick) {
                mousePressed = draggingWindow = false;
                clickTimer->stop();
                toggleFullscreen();
            } else if (event->type() == QEvent::MouseButtonPress) {
                mousePressed = true;
                draggingWindow = false;
                dragStartMouse = mouse->globalPosition().toPoint();
                dragStartWindow = pos();
            } else {
                const bool isClick = mousePressed && !draggingWindow
                    && (mouse->globalPosition().toPoint() - dragStartMouse).manhattanLength()
                        < QApplication::startDragDistance();
                if (watched == video && isClick) {
                    // Commit a single click only after a double-click has been ruled out.
                    clickTimer->start(QApplication::doubleClickInterval());
                }
                mousePressed = draggingWindow = false;
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void PlayerWindow::adjustVolume(int steps) {
    volume->setValue(volume->value() + steps * volume->singleStep());
}

void PlayerWindow::wheelEvent(QWheelEvent *event) {
    if (!event->angleDelta().isNull()) {
        wheelRemainder += event->angleDelta().y();
        const int steps = wheelRemainder / 120;
        wheelRemainder %= 120;
        adjustVolume(steps);
    } else if (event->pixelDelta().y() != 0) {
        adjustVolume(event->pixelDelta().y() > 0 ? 1 : -1);
    }
    event->accept();
}

void PlayerWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) updateFullscreen();
}

void PlayerWindow::toggleFullscreen() {
    if (isFullScreen()) showNormal();
    else showFullScreen();
}

void PlayerWindow::updateFullscreen() {
    const bool fullscreen = isFullScreen();
    auto *layout = centralWidget()->layout();
    // Walk the page layout only, preserving the video widget and its parent.
    const auto setControlsVisible = [this, fullscreen](auto &&self, QLayout *items) -> void {
        for (int i = 0; i < items->count(); ++i) {
            auto *item = items->itemAt(i);
            if (auto *widget = item->widget(); widget && widget != stage)
                widget->setVisible(!fullscreen);
            if (auto *childLayout = item->layout()) self(self, childLayout);
        }
    };
    setControlsVisible(setControlsVisible, layout);
    layout->setContentsMargins(fullscreen ? QMargins() : QMargins(0, 0, 0, 12));
    layout->setSpacing(fullscreen ? 0 : 16);
    const bool showMedia = fullscreen || (!player->source().isEmpty() && player->error() == QMediaPlayer::NoError);
    stage->setCurrentIndex(showMedia ? (frameReady ? 1 : 2) : 0);
}

void PlayerWindow::updateTimeline() {
    const qint64 duration = player->duration();
    qint64 position = player->position();
    if (timeline->isSliderDown()) position = qRound64(timeline->value() * (double(duration) / timelineSteps));
    else {
        const QSignalBlocker blocker(timeline);
        timeline->setValue(duration > 0 ? qRound(position * (double(timelineSteps) / duration)) : 0);
    }
    const QString positionText = timestamp(position);
    const QString durationText = timestamp(duration);
    timeline->setToolTip(positionText + " / " + durationText);
    timeline->setAccessibleDescription("Position " + positionText + " of " + durationText);
    if (duration > 0) {
        timeline->setSingleStep(qMax(1, qRound(5000.0 * timelineSteps / duration)));
        timeline->setPageStep(qMax(1, qRound(30000.0 * timelineSteps / duration)));
    }
}

void PlayerWindow::seekToSlider() {
    if (player->isSeekable()) player->setPosition(qRound64(timeline->value() * (double(player->duration()) / timelineSteps)));
}

void PlayerWindow::skip(qint64 delta) {
    if (timeline->isEnabled()) player->setPosition(qBound(qint64(0), player->position() + delta, player->duration()));
}

void PlayerWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty()
        && event->mimeData()->urls().first().isLocalFile()) event->acceptProposedAction();
}

void PlayerWindow::dropEvent(QDropEvent *event) {
    if (!event->mimeData()->urls().isEmpty() && event->mimeData()->urls().first().isLocalFile()) {
        openFile(event->mimeData()->urls().first().toLocalFile());
        event->acceptProposedAction();
    }
}
