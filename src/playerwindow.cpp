#include "playerwindow.h"
#include "seekslider.h"
#include <QAudioOutput>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVideoWidget>

namespace {
constexpr int timelineSteps = 1000000;
QString timestamp(qint64 milliseconds) {
    const qint64 seconds = qMax(qint64(0), milliseconds) / 1000;
    return QString("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
}

PlayerWindow::PlayerWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Video Player");
    resize(1100, 740);
    setMinimumSize(680, 440);
    setAcceptDrops(true);
    player = new QMediaPlayer(this);
    player->setObjectName("mediaPlayer");
    audio = new QAudioOutput(this);
    audio->setVolume(0.7f);
    player->setAudioOutput(audio);
    auto *page = new QWidget(this);
    setCentralWidget(page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(16);
    auto *header = new QHBoxLayout;
    auto *brand = new QLabel("VIDEO PLAYER");
    brand->setStyleSheet("font-size: 15px; font-weight: 700; letter-spacing: 2px; color: #f0f3fc;");
    fileLabel = new QLabel("Your cinema, one file away");
    fileLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    fileLabel->setTextFormat(Qt::PlainText);
    auto *openButton = new QPushButton("Open video");
    openButton->setObjectName("primary");
    openButton->setToolTip("Open a video (Ctrl+O)");
    header->addWidget(brand);
    header->addSpacing(20);
    header->addWidget(fileLabel, 1);
    header->addWidget(openButton);
    layout->addLayout(header);
    stage = new QStackedWidget;
    auto *empty = new QWidget;
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
    emptyLayout->addSpacing(16);
    emptyLayout->addWidget(emptyOpen, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();
    stage->addWidget(empty);
    video = new QVideoWidget;
    video->setAspectRatioMode(Qt::KeepAspectRatio);
    stage->addWidget(video);
    player->setVideoOutput(video);
    layout->addWidget(stage, 1);
    timeline = new SeekSlider;
    timeline->setObjectName("timeline");
    timeline->setAccessibleName("Video position");
    timeline->setRange(0, timelineSteps);
    timeline->setToolTip("Click or drag to seek");
    layout->addWidget(timeline);
    auto *controls = new QHBoxLayout;
    backButton = new QPushButton("-10 s");
    playButton = new QPushButton("Play");
    playButton->setObjectName("playButton");
    playButton->setMinimumWidth(90);
    playButton->setToolTip("Play / pause (Space)");
    forwardButton = new QPushButton("+10 s");
    controls->addWidget(backButton);
    controls->addWidget(playButton);
    controls->addWidget(forwardButton);
    timeLabel = new QLabel;
    timeLabel->setStyleSheet("font-family: Consolas; color: #e2e8f7;");
    controls->addSpacing(12);
    controls->addWidget(timeLabel);
    controls->addStretch();
    auto *mute = new QPushButton("Mute");
    mute->setCheckable(true);
    controls->addWidget(mute);
    auto *volume = new QSlider(Qt::Horizontal);
    volume->setAccessibleName("Volume");
    volume->setRange(0, 100);
    volume->setValue(70);
    volume->setFixedWidth(100);
    controls->addWidget(volume);
    layout->addLayout(controls);
    auto *footer = new QHBoxLayout;
    statusLabel = new QLabel("Open a local video to begin");
    statusLabel->setObjectName("status");
    statusLabel->setTextFormat(Qt::PlainText);
    statusLabel->setWordWrap(true);
    footer->addWidget(statusLabel, 1);
    footer->addWidget(new QLabel("Space  Play / pause    |    F11  Fullscreen"));
    layout->addLayout(footer);
    setStyleSheet(R"(
        QMainWindow, QWidget { background: #12151d; color: #939eb4; font-family: 'Segoe UI'; font-size: 13px; }
        QWidget#emptyStage { background: #0b0e14; border: 1px solid #252b3a; border-radius: 12px; }
        QWidget#emptyStage QLabel { background: transparent; border: none; }
        QPushButton { background: #242b3b; color: #e4eaf8; border: 1px solid #333e53; border-radius: 7px; padding: 9px 16px; }
        QPushButton:hover { background: #33415b; }
        QPushButton:focus { border-color: #94b4ff; }
        QPushButton:disabled { color: #596173; background: #1b202c; border-color: #252c39; }
        QPushButton#primary, QPushButton#playButton { background: #759eff; color: #0d1830; font-weight: 600; }
        QPushButton#primary:hover, QPushButton#playButton:hover { background: #9bb8ff; }
        QPushButton#playButton:disabled { background: #2b3752; color: #72809b; }
        QSlider::groove:horizontal { background: #2b3243; height: 5px; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: #759eff; border-radius: 2px; }
        QSlider::handle:horizontal { background: #e5edff; width: 14px; margin: -5px 0; border-radius: 7px; }
        QSlider:disabled::sub-page:horizontal { background: #2b3243; }
        QToolTip { background: #242b3b; color: #e4eaf8; border: 1px solid #475575; }
    )");
    connect(openButton, &QPushButton::clicked, this, &PlayerWindow::chooseFile);
    connect(emptyOpen, &QPushButton::clicked, this, &PlayerWindow::chooseFile);
    connect(playButton, &QPushButton::clicked, this, &PlayerWindow::togglePlayback);
    connect(backButton, &QPushButton::clicked, this, [this] { skip(-10000); });
    connect(forwardButton, &QPushButton::clicked, this, [this] { skip(10000); });
    connect(volume, &QSlider::valueChanged, this, [this](int value) { audio->setVolume(value / 100.f); });
    connect(mute, &QPushButton::toggled, this, [this, mute](bool muted) {
        audio->setMuted(muted); mute->setText(muted ? "Unmute" : "Mute");
    });
    connect(timeline, &QSlider::sliderPressed, this, [this] {
        scrubbing = true;
        resumeAfterScrub = player->isPlaying();
        player->pause();
    });
    connect(timeline, &QSlider::valueChanged, this, [this] {
        if (scrubbing) updateTimeline(); else seekToSlider();
    });
    connect(timeline, &QSlider::sliderReleased, this, [this] {
        seekToSlider();
        scrubbing = false;
        if (resumeAfterScrub) player->play();
    });
    connect(player, &QMediaPlayer::positionChanged, this, &PlayerWindow::updateTimeline);
    connect(player, &QMediaPlayer::durationChanged, this, [this] { updateControls(); updateTimeline(); });
    connect(player, &QMediaPlayer::seekableChanged, this, &PlayerWindow::updateControls);
    connect(player, &QMediaPlayer::playbackStateChanged, this, &PlayerWindow::updateControls);
    connect(player, &QMediaPlayer::mediaStatusChanged, this, &PlayerWindow::updateControls);
    connect(player, &QMediaPlayer::hasVideoChanged, this, [this](bool hasVideo) { stage->setCurrentIndex(hasVideo ? 1 : 0); });
    connect(player, &QMediaPlayer::errorOccurred, this, [this] { updateControls(); });
    auto shortcut = [this](const QKeySequence &key, auto callback) {
        auto *action = new QShortcut(key, this); connect(action, &QShortcut::activated, this, callback);
    };
    shortcut(QKeySequence::Open, [this] { chooseFile(); });
    shortcut(QKeySequence(Qt::Key_Space), [this] { togglePlayback(); });
    shortcut(QKeySequence(Qt::Key_Left), [this] { skip(-10000); });
    shortcut(QKeySequence(Qt::Key_Right), [this] { skip(10000); });
    shortcut(QKeySequence(Qt::Key_F11), [this] { isFullScreen() ? showNormal() : showFullScreen(); });
    shortcut(QKeySequence(Qt::Key_Escape), [this] { if (isFullScreen()) showNormal(); });
    updateControls();
    updateTimeline();
}

void PlayerWindow::chooseFile() {
    const QString path = QFileDialog::getOpenFileName(this, "Open video", {},
        "Video files (*.mp4 *.mkv *.avi *.mov *.webm *.m4v *.wmv *.mpeg *.mpg *.ts *.m2ts *.ogv);;All files (*)");
    if (!path.isEmpty()) openFile(path);
}

void PlayerWindow::openFile(const QString &path) {
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable()) {
        statusLabel->setText("Cannot open file: " + path);
        return;
    }
    scrubbing = resumeAfterScrub = false;
    timeline->setSliderDown(false);
    player->stop();
    stage->setCurrentIndex(0);
    fileLabel->setText(file.fileName());
    fileLabel->setToolTip(file.absoluteFilePath());
    setWindowTitle(file.fileName() + " - Video Player");
    player->setSource(QUrl::fromLocalFile(file.absoluteFilePath()));
    player->play();
}

void PlayerWindow::togglePlayback() {
    if (!playButton->isEnabled()) return;
    if (player->isPlaying()) player->pause();
    else { if (player->mediaStatus() == QMediaPlayer::EndOfMedia) player->setPosition(0); player->play(); }
}

void PlayerWindow::updateControls() {
    const auto status = player->mediaStatus();
    const bool ready = !player->source().isEmpty() && player->error() == QMediaPlayer::NoError
        && status != QMediaPlayer::LoadingMedia && status != QMediaPlayer::InvalidMedia;
    playButton->setEnabled(ready);
    playButton->setText(player->isPlaying() ? "Pause" : status == QMediaPlayer::EndOfMedia ? "Replay" : "Play");
    const bool seekable = ready && player->isSeekable() && player->duration() > 0;
    timeline->setEnabled(seekable);
    backButton->setEnabled(seekable);
    forwardButton->setEnabled(seekable);
    if (player->error() != QMediaPlayer::NoError) {
        statusLabel->setText("Unable to play: " + player->errorString());
        stage->setCurrentIndex(0);
    } else if (status == QMediaPlayer::LoadingMedia) statusLabel->setText("Loading video...");
    else if (status == QMediaPlayer::StalledMedia) statusLabel->setText("Buffering...");
    else if (status == QMediaPlayer::EndOfMedia) statusLabel->setText("Finished - press Replay to watch again");
    else if (ready) statusLabel->setText(player->isPlaying() ? "Playing" : "Paused");
    else statusLabel->setText("Open a local video to begin");
}

void PlayerWindow::updateTimeline() {
    const qint64 duration = player->duration();
    qint64 position = player->position();
    if (scrubbing) position = qRound64(timeline->value() * (double(duration) / timelineSteps));
    else {
        const QSignalBlocker blocker(timeline);
        timeline->setValue(duration > 0 ? qRound(position * (double(timelineSteps) / duration)) : 0);
    }
    timeLabel->setText(timestamp(position) + " / " + timestamp(duration));
    timeline->setAccessibleDescription("Position " + timestamp(position) + " of " + timestamp(duration));
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
