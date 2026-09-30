#include "playerwindow.h"
#include "seekslider.h"
#include "updater.h"
#include "gammafilter.h"
#include "externalaudio.h"
#include "embeddedsubtitles.h"
#include <QContextMenuEvent>
#include <QCursor>
#include <QMenu>
#include <QKeyEvent>
#include <QStyle>
#include <QStyleOption>
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
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMimeData>
#include <QMediaMetaData>
#include <QDir>
#include <QMessageBox>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPushButton>
#include <QPainter>
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
constexpr auto popupMenuStyle = R"(
    QMenu { background: #242b3b; color: #e4eaf8; border: 1px solid #475575;
            padding: 4px; font-family: 'Segoe UI'; font-size: 13px; }
    QMenu::item { padding: 7px 24px; }
    QMenu::item:selected { background: #33415b; }
    QMenu::item:disabled { color: #596173; }
)";

class TrackMenu final : public QMenu {
public:
    TrackMenu(QMediaPlayer *player, QWidget *parent, const QString &name, const QString &title,
              QList<QVariant> &excluded)
        : QMenu(parent), excluded(excluded) {
        setObjectName(name);
        setAccessibleName(title);
        // Another instance can open a different file while a track menu is visible.
        connect(player, &QMediaPlayer::tracksChanged, this, &QMenu::close);
        connect(player, &QMediaPlayer::sourceChanged, this, &QMenu::close);
        // Selecting a label must not change its inclusion in keyboard cycling.
        connect(this, &QMenu::triggered, this, [this](QAction *action) {
            if (action->isCheckable()) action->setChecked(!this->excluded.contains(action->data()));
        });
    }
    void addChoice(const QString &label, const QVariant &data, bool current) {
        auto *action = addAction(QString(label).replace("&", "&&"));
        action->setData(data);
        action->setCheckable(true);
        action->setChecked(!excluded.contains(data));
        if (current) {
            currentChoice = action;
            auto font = action->font();
            font.setBold(true);
            action->setFont(font);
        }
    }
    void addTracks(const QList<QMediaMetaData> &tracks, int current, bool audio) {
        for (int index = 0; index < tracks.size(); ++index)
            addChoice(trackLabel(tracks[index], index, audio), index, index == current);
    }
    void addFiles(const QStringList &files, const QString &current) {
        if (!files.isEmpty() && !isEmpty()) addSeparator();
        for (const auto &file : files)
            addChoice(QFileInfo(file).fileName(), file, file == current);
    }
    QAction *nextChoice() const {
        const auto choices = actions();
        const auto start = choices.indexOf(currentChoice);
        for (qsizetype offset = 1; offset <= choices.size(); ++offset) {
            auto *action = choices[(start + offset) % choices.size()];
            if (action->isChecked()) return action == currentChoice ? nullptr : action;
        }
        return nullptr;
    }
protected:
    void initStyleOption(QStyleOptionMenuItem *option, const QAction *action) const override {
        QMenu::initStyleOption(option, action);
        // Draw a checkbox below instead of the menu's plain check mark.
        option->checked = false;
    }
    void paintEvent(QPaintEvent *event) override {
        QMenu::paintEvent(event);
        QPainter painter(this);
        for (auto *action : actions()) {
            if (!action->isCheckable()) continue;
            QStyleOptionButton option;
            option.initFrom(this);
            option.rect = QRect(QPoint(), QSize(14, 14));
            option.rect.moveCenter(checkboxRect(action).center());
            option.state |= action->isChecked() ? QStyle::State_On : QStyle::State_Off;
            style()->drawPrimitive(QStyle::PE_IndicatorCheckBox, &option, &painter, this);
        }
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            checkboxPress = checkboxAt(event->position().toPoint());
            if (checkboxPress) {
                setActiveAction(checkboxPress);
                event->accept();
                return;
            }
        }
        QMenu::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && checkboxPress) {
            if (checkboxAt(event->position().toPoint()) == checkboxPress) toggleChoice(checkboxPress);
            checkboxPress = nullptr;
            event->accept();
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        auto *action = activeAction();
        if (event->key() == Qt::Key_Space && action && action->isCheckable()) {
            toggleChoice(action);
            event->accept();
            return;
        }
        QMenu::keyPressEvent(event);
    }
private:
    static QString trackLabel(const QMediaMetaData &metadata, int index, bool audio) {
        QStringList details{QString("Track %1").arg(index + 1)};
        for (auto key : {QMediaMetaData::Title, QMediaMetaData::Language}) {
            const QString value = metadata.stringValue(key);
            if (!value.isEmpty()) details.append(value);
        }
        if (audio) {
            const QString codec = metadata.stringValue(QMediaMetaData::AudioCodec);
            if (!codec.isEmpty()) details.append(codec);
        }
        return details.join(" - ");
    }
    QRect checkboxRect(QAction *action) const {
        const QRect row = actionGeometry(action);
        // The shared popup style reserves 24 pixels to the left of each label.
        const QRect checkbox(row.left(), row.top(), 24, row.height());
        return QStyle::visualRect(layoutDirection(), row, checkbox);
    }
    QAction *checkboxAt(const QPoint &position) const {
        auto *action = actionAt(position);
        return action && action->isCheckable() && checkboxRect(action).contains(position) ? action : nullptr;
    }
    void toggleChoice(QAction *action) {
        action->toggle();
        if (action->isChecked()) excluded.removeAll(action->data());
        else excluded.append(action->data());
    }
    QList<QVariant> &excluded;
    QAction *currentChoice = nullptr;
    QAction *checkboxPress = nullptr;
};

class FullscreenCloseButton final : public QAbstractButton {
public:
    explicit FullscreenCloseButton(QWidget *parent) : QAbstractButton(parent) {
        // Like the seek panel, this must sit above the native video window.
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFixedSize(43, 43);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setToolTip(tr("Close"));
        setAccessibleName(tr("Close"));
        hide();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), isDown() ? QColor(170, 35, 35) : QColor(40, 40, 40, 220));
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(160, 160, 160), 1, Qt::SolidLine, Qt::RoundCap));
        const QPointF center(width() / 2.0, height() / 2.0);
        painter.drawLine(center + QPointF(-7, -7), center + QPointF(7, 7));
        painter.drawLine(center + QPointF(7, -7), center + QPointF(-7, 7));
    }
    void leaveEvent(QEvent *event) override {
        hide();
        QAbstractButton::leaveEvent(event);
    }
};

constexpr int timelineSteps = 1000000;
QString timestamp(qint64 milliseconds) {
    const qint64 seconds = qMax(qint64(0), milliseconds) / 1000;
    return QString("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QStringList subtitleFiles(const QUrl &source) {
    QStringList files;
    if (!source.isLocalFile()) return files;
    const QDir directory = QFileInfo(source.toLocalFile()).absoluteDir();
    for (const auto &file : directory.entryInfoList(QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase))
        if (file.suffix().compare("srt", Qt::CaseInsensitive) == 0) files.append(file.absoluteFilePath());
    return files;
}

void setSubtitleText(QVideoSink *sink, const QString &text, bool paused) {
    if (sink->subtitleText() == text && (!paused || sink->videoFrame().subtitleText() == text)) return;
    sink->setSubtitleText(text);
    if (paused) {
        // Qt applies subtitle text when a new frame arrives. Refresh the existing
        // frame as well so selecting, disabling, and seeking work while paused.
        const QVideoFrame frame = sink->videoFrame();
        if (frame.isValid()) {
            sink->setVideoFrame({});
            sink->setVideoFrame(frame);
        }
    }
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
    auto *audio = new QAudioOutput(this);
    const int savedVolume = qBound(0, QSettings().value("audio/volume", 70).toInt(), 100);
    audio->setVolume(savedVolume / 100.f);
    player->setAudioOutput(audio);
    externalAudio = new ExternalAudio(player, this);
    connect(externalAudio, &ExternalAudio::failed, this, [this](const QString &message) {
        QMessageBox::warning(this, "Cannot play external audio", message);
    }, Qt::QueuedConnection);
    auto *page = new QWidget(this);
    setCentralWidget(page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 12);
    layout->setSpacing(16);
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
    auto *gammaFilter = new GammaFilter(video->videoSink(), this);
    player->setVideoOutput(video);
    embeddedSubtitles = new EmbeddedSubtitles(player, this);
    connect(embeddedSubtitles, &EmbeddedSubtitles::textChanged, this, &PlayerWindow::updateSubtitles);
    connect(embeddedSubtitles, &EmbeddedSubtitles::failed, this, [this](const QString &message) {
        QMessageBox::warning(this, "Cannot play subtitles", message);
    }, Qt::QueuedConnection);
    connect(video->videoSink(), &QVideoSink::subtitleTextChanged,
        this, &PlayerWindow::updateSubtitles, Qt::QueuedConnection);
    layout->addWidget(stage, 1);
    timeline = new SeekSlider;
    timeline->setObjectName("timeline");
    timeline->setAccessibleName("Video position");
    timeline->setRange(0, timelineSteps);
    timeline->setToolTip("Click or drag to seek");
    timeline->installEventFilter(this);
    controlsPanel = new QWidget(page);
    controlsPanel->setObjectName("controlsPanel");
    controlsPanel->setAttribute(Qt::WA_ShowWithoutActivating);
    auto *controls = new QGridLayout(controlsPanel);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setHorizontalSpacing(16);
    controls->setVerticalSpacing(4);
    controls->setColumnStretch(0, 1);
    controls->addWidget(timeline, 0, 0, Qt::AlignVCenter);
    timeLabel = new QLabel;
    timeLabel->setObjectName("timeReadout");
    timeLabel->setAccessibleName("Current and total time, video codec and resolution");
    controls->addWidget(timeLabel, 1, 0);
    const auto addControlSlider = [this, controls](const QString &name, const QString &title, int column) {
        auto *slider = new SeekSlider;
        slider->setObjectName(name);
        slider->setAccessibleName(title);
        slider->setFixedWidth(100);
        slider->installEventFilter(this);
        controls->addWidget(slider, 0, column, Qt::AlignVCenter);
        return slider;
    };
    volume = addControlSlider("volume", "Volume", 1);
    volume->setSingleStep(5);
    volume->setRange(0, 100);
    volume->setValue(savedVolume);
    auto *volumeLabel = new QLabel(QString("Volume %1").arg(savedVolume));
    volumeLabel->setObjectName("volumeReadout");
    controls->addWidget(volumeLabel, 1, 1);
    gamma = addControlSlider("gamma", "Gamma", 2);
    gamma->setRange(1, 40);
    gamma->setValue(10);
    gamma->setPageStep(5);
    auto *gammaLabel = new QLabel("Gamma 1.0");
    gammaLabel->setObjectName("gammaReadout");
    gamma->setToolTip("Gamma 1.0 (double-click to reset)");
    controls->addWidget(gammaLabel, 1, 2);
    connect(gamma, &QSlider::valueChanged, this, [gammaFilter, gammaLabel, this](int value) {
        const QString text = QString("Gamma %1").arg(value / 10.0, 0, 'f', 1);
        gammaLabel->setText(text);
        gamma->setToolTip(text + " (double-click to reset)");
        saveVideoSettings();
        gammaFilter->apply(player, value);
        updateSubtitles();
    });
    layout->addWidget(controlsPanel);
    fullscreenClose = new FullscreenCloseButton(this);
    fullscreenClose->setObjectName("fullscreenClose");
    connect(fullscreenClose, &QAbstractButton::clicked, this, &PlayerWindow::close);
    // Poll only in fullscreen: native video surfaces do not always forward mouse moves.
    controlsTimer = new QTimer(this);
    controlsTimer->setInterval(100);
    connect(controlsTimer, &QTimer::timeout, this, &PlayerWindow::updateFullscreenControls);
    setStyleSheet(R"(
        QMainWindow, QWidget { background: #000000; color: #939eb4; font-family: 'Segoe UI'; font-size: 13px; }
        QWidget#emptyStage { background: #000000; border: 1px solid #252b3a; border-radius: 12px; }
        QWidget#emptyStage QLabel { background: transparent; border: none; }
        QPushButton { background: #242b3b; color: #e4eaf8; border: 1px solid #333e53; border-radius: 7px; padding: 9px 16px; }
        QPushButton:hover { background: #33415b; }
        QPushButton:focus { border-color: #94b4ff; }
        QPushButton:disabled { color: #596173; background: #1b202c; border-color: #252c39; }
        QSlider::groove:horizontal { background: #2b3243; height: 5px; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: #759eff; border-radius: 2px; }
        QSlider::handle:horizontal { background: #e5edff; width: 14px; margin: -5px 0; border-radius: 7px; }
        QSlider:disabled::sub-page:horizontal { background: #2b3243; }
        QToolTip { background: #242b3b; color: #e4eaf8; border: 1px solid #475575; }
    )");
    connect(emptyOpen, &QPushButton::clicked, this, &PlayerWindow::chooseFile);
    connect(volume, &QSlider::valueChanged, this, [this, audio, volumeLabel](int value) {
        volumeLabel->setText(QString("Volume %1").arg(value));
        audio->setVolume(value / 100.f);
        saveVideoSettings(true);
    });
    connect(timeline, &QSlider::valueChanged, this, &PlayerWindow::seekToSlider);
    connect(timeline, &QSlider::sliderReleased, this, &PlayerWindow::updateTimeline);
    connect(player, &QMediaPlayer::positionChanged, this, &PlayerWindow::updateTimeline);
    connect(player, &QMediaPlayer::positionChanged, this, &PlayerWindow::updateSubtitles);
    connect(player, &QMediaPlayer::playbackStateChanged, this, &PlayerWindow::updateSubtitles);
    connect(player, &QMediaPlayer::metaDataChanged, this, &PlayerWindow::updateTimeline);
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
    shortcut(QKeySequence(Qt::Key_A), [this] { chooseAudioTrack(true); });
    shortcut(QKeySequence(Qt::Key_S), [this] { chooseSubtitles(true); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_A), [this] { chooseAudioTrack(); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_S), [this] { chooseSubtitles(); });
    shortcut(QKeySequence(Qt::Key_Space), &PlayerWindow::togglePlayback);
    shortcut(QKeySequence(Qt::Key_Left), [this] { skip(-3000); });
    shortcut(QKeySequence(Qt::Key_Right), [this] { skip(3000); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_Left), [this] { skip(-30000); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_Right), [this] { skip(30000); });
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

QAction *PlayerWindow::execPopupMenu(QMenu &menu, const QPoint &position) {
    clickTimer->stop();
    menu.setStyleSheet(popupMenuStyle);
    return menu.exec(position);
}

void PlayerWindow::showPopupMenu(const QPoint &position) {
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
    execPopupMenu(menu, position);
}

void PlayerWindow::chooseAudioTrack(bool cycle) {
    TrackMenu menu(player, this, "audioTrackMenu", "Audio tracks", excludedAudioTracks);
    menu.addTracks(player->audioTracks(), externalAudio->filePath().isEmpty() ? player->activeAudioTrack() : -1, true);
    menu.addFiles(ExternalAudio::matchingFiles(player->source()), externalAudio->filePath());
    if (menu.isEmpty()) menu.addAction("No audio tracks available")->setEnabled(false);
    const auto *selected = cycle ? menu.nextChoice() : execPopupMenu(menu, mapToGlobal(rect().center()));
    if (!selected || !selected->data().isValid()) return;
    if (selected->data().metaType().id() == QMetaType::QString)
        externalAudio->select(selected->data().toString());
    else {
        externalAudio->clear();
        player->setActiveAudioTrack(selected->data().toInt());
    }
}

void PlayerWindow::chooseSubtitles(bool cycle) {
    TrackMenu menu(player, this, "subtitleMenu", "Subtitles", excludedSubtitles);
    const bool embedded = externalSubtitles.filePath().isEmpty();
    menu.addChoice("Off", -1, embedded && embeddedSubtitles->activeTrack() < 0);
    menu.addTracks(player->subtitleTracks(), embedded ? embeddedSubtitles->activeTrack() : -1, false);
    menu.addFiles(subtitleFiles(player->source()), externalSubtitles.filePath());
    const auto *selected = cycle ? menu.nextChoice() : execPopupMenu(menu, mapToGlobal(rect().center()));
    if (!selected) return;
    if (selected->data().metaType().id() == QMetaType::QString) {
        QString error;
        if (!externalSubtitles.load(selected->data().toString(), error)) {
            QMessageBox::warning(this, "Cannot load subtitles", error);
            return;
        }
        embeddedSubtitles->select(-1);
    } else {
        externalSubtitles.clear();
        embeddedSubtitles->select(selected->data().toInt());
    }
    updateSubtitles();
}

void PlayerWindow::updateSubtitles() {
    QString text;
    if (player->playbackState() != QMediaPlayer::StoppedState)
        text = externalSubtitles.filePath().isEmpty()
            ? embeddedSubtitles->text() : externalSubtitles.textAt(player->position());
    setSubtitleText(player->videoSink(), text, !player->isPlaying());
}

void PlayerWindow::openFile(const QString &path) {
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable()) {
        statusLabel->setText("Cannot open file: " + path);
        return;
    }
    savePosition();
    externalSubtitles.clear();
    externalAudio->clear();
    excludedAudioTracks.clear();
    excludedSubtitles.clear();
    pendingPosition = -1;
    clickTimer->stop();
    timeline->setSliderDown(false);
    player->stop();
    QString identity = file.canonicalFilePath();
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    fileKey = QString::fromLatin1(QCryptographicHash::hash(
        identity.toUtf8(), QCryptographicHash::Sha256).toHex());
    QSettings settings;
    pendingPosition = settings.value("positions/" + fileKey, 0).toLongLong();
    const int defaultVolume = settings.value("audio/volume", 70).toInt();
    settings.beginGroup("videos/" + fileKey);
    restoringVideoSettings = true;
    // QSlider clamps restored values to its configured range.
    volume->setValue(settings.value("volume", defaultVolume).toInt());
    gamma->setValue(settings.value("gamma", 10).toInt());
    restoringVideoSettings = false;
    saveVideoSettings();
    frameReady = false;
    stage->setCurrentIndex(2);
    setWindowTitle(file.fileName() + QStringLiteral(" - Video Player v" VIDEO_PLAYER_VERSION));
    player->setSource(QUrl::fromLocalFile(file.absoluteFilePath()));
    restorePosition();
    player->play();
}

void PlayerWindow::saveVideoSettings(bool rememberVolume) {
    if (restoringVideoSettings) return;
    QSettings settings;
    if (rememberVolume) settings.setValue("audio/volume", volume->value());
    if (!fileKey.isEmpty()) {
        settings.beginGroup("videos/" + fileKey);
        settings.setValue("volume", volume->value());
        settings.setValue("gamma", gamma->value());
    }
    settings.sync();
}

void PlayerWindow::savePosition() {
    if (fileKey.isEmpty() || pendingPosition >= 0 || !player->isSeekable()
        || player->duration() <= 0 || player->error() != QMediaPlayer::NoError) return;
    QSettings settings;
    settings.setValue("positions/" + fileKey, player->mediaStatus() == QMediaPlayer::EndOfMedia
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
    externalAudio->seek(position);
}

void PlayerWindow::closeEvent(QCloseEvent *event) {
    clickTimer->stop();
    controlsTimer->stop();
    fullscreenClose->hide();
    savePosition();
    externalAudio->clear();
    embeddedSubtitles->clear();
    player->stop();
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
        if (player->mediaStatus() == QMediaPlayer::EndOfMedia) externalAudio->seek(0);
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
    if (watched == gamma && event->type() == QEvent::MouseButtonDblClick
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        gamma->setValue(10);
        return true;
    }
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
    if (isFullScreen()) {
        showNormal();
        // Restore after the controls have returned to the windowed layout.
        centralWidget()->layout()->activate();
        if (!windowedGeometry.isEmpty()) restoreGeometry(windowedGeometry);
    } else {
        windowedGeometry = saveGeometry();
        showFullScreen();
    }
}

void PlayerWindow::updateFullscreen() {
    const bool fullscreen = isFullScreen();
    auto *layout = centralWidget()->layout();
    if (fullscreen) {
        layout->removeWidget(controlsPanel);
        if (!controlsPanel->isWindow()) {
            // A separate owned window stays above the native video surface on Windows.
            controlsPanel->setParent(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
            controlsPanel->hide();
        }
        controlsTimer->start();
    } else {
        controlsTimer->stop();
        fullscreenClose->hide();
        if (controlsPanel->isWindow()) controlsPanel->setParent(centralWidget(), Qt::Widget);
        if (layout->indexOf(controlsPanel) < 0) layout->addWidget(controlsPanel);
        controlsPanel->show();
    }
    controlsPanel->layout()->setContentsMargins(fullscreen ? QMargins(12, 12, 12, 12) : QMargins());
    layout->setContentsMargins(fullscreen ? QMargins() : QMargins(0, 0, 0, 12));
    layout->setSpacing(fullscreen ? 0 : 16);
    const bool showMedia = fullscreen || (!player->source().isEmpty() && player->error() == QMediaPlayer::NoError);
    stage->setCurrentIndex(showMedia ? (frameReady ? 1 : 2) : 0);
}

void PlayerWindow::updateFullscreenControls() {
    if (!isFullScreen()) return;
    if (!isVisible() || isMinimized()) {
        controlsPanel->hide();
        fullscreenClose->hide();
        return;
    }
    const QPoint globalCursor = QCursor::pos();
    fullscreenClose->move(centralWidget()->mapToGlobal(
        QPoint(centralWidget()->width() - fullscreenClose->width(), 0)));
    const bool overClose = fullscreenClose->geometry().contains(globalCursor);
    fullscreenClose->setVisible(overClose && !QApplication::activePopupWidget()
        && !QApplication::activeModalWidget()
        && (QApplication::mouseButtons() == Qt::NoButton || fullscreenClose->isDown()));
    if (fullscreenClose->isVisible()) fullscreenClose->raise();
    const int panelHeight = controlsPanel->sizeHint().height();
    const QPoint panelPosition = centralWidget()->mapToGlobal(QPoint(0, centralWidget()->height() - panelHeight));
    controlsPanel->setGeometry(QRect(panelPosition, QSize(centralWidget()->width(), panelHeight)));
    const QPoint cursor = centralWidget()->mapFromGlobal(globalCursor);
    const bool inside = centralWidget()->rect().contains(cursor);
    const bool atBottom = inside && cursor.y() >= centralWidget()->height() - 6;
    const bool overControls = inside && controlsPanel->isVisible()
        && controlsPanel->geometry().contains(globalCursor);
    controlsPanel->setVisible(atBottom || overControls || timeline->isSliderDown()
        || volume->isSliderDown() || gamma->isSliderDown());
    if (controlsPanel->isVisible()) controlsPanel->raise();
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
    const QString timeText = positionText + " / " + durationText;
    QString readout = " " + timeText;
    const auto metadata = player->metaData();
    const QString codec = metadata.stringValue(QMediaMetaData::VideoCodec);
    if (!codec.isEmpty()) readout += "    " + codec;
    const QSize resolution = metadata.value(QMediaMetaData::Resolution).toSize();
    if (!resolution.isEmpty())
        readout += QString("    %1\u00d7%2").arg(resolution.width()).arg(resolution.height());
    timeLabel->setText(readout);
    timeline->setToolTip(timeText);
    timeline->setAccessibleDescription("Position " + positionText + " of " + durationText);
    if (duration > 0) {
        timeline->setSingleStep(qMax(1, qRound(5000.0 * timelineSteps / duration)));
        timeline->setPageStep(qMax(1, qRound(30000.0 * timelineSteps / duration)));
    }
}

void PlayerWindow::seekToSlider() {
    if (player->isSeekable()) externalAudio->seek(qRound64(timeline->value() * (double(player->duration()) / timelineSteps)));
    updateTimeline();
}

void PlayerWindow::skip(qint64 delta) {
    if (timeline->isEnabled()) externalAudio->seek(qBound(qint64(0), player->position() + delta, player->duration()));
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
