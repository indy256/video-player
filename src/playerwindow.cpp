#include "playerwindow.h"
#include "seekslider.h"
#include "keyframeindex.h"
#include "updater.h"
#include "gammafilter.h"
#include "externalaudio.h"
#include "embeddedsubtitles.h"
#include "fileassociations.h"
#include "folderpanel.h"
#include <QFileOpenEvent>
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
#include <QTextLayout>
#include <cmath>
#include <functional>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
void configureOverlay(QLabel *label, const QString &name) {
    // Owned, non-interactive windows stay above the native video surface.
    label->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint
        | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setAttribute(Qt::WA_ShowWithoutActivating);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setAttribute(Qt::WA_TranslucentBackground);
    label->setFocusPolicy(Qt::NoFocus);
    label->hide();
}

// Preserve Qt's caption layout, painting only its background with rounded corners.
class SubtitleOverlay final : public QLabel {
public:
    explicit SubtitleOverlay(QWidget *parent) : QLabel(parent) {
        configureOverlay(this, "subtitleOverlay");
    }

    void layoutCaption(const QSize &picture) {
        QString caption = text();
        caption.replace('\n', QChar::LineSeparator);
        if (picture == pictureSize && caption == layout.text()) return;
        pictureSize = picture;
        QFont font;
        const qreal fontSize = picture.height() * 0.045;
        font.setPointSize(qMax(1, int(fontSize)));
        layout.setText(caption);
        layout.setFont(font);
        QTextOption option;
        option.setUseDesignMetrics(true);
        option.setAlignment(Qt::AlignCenter);
        layout.setTextOption(option);
        const int leading = QFontMetrics(font).leading();
        qreal height = 0;
        qreal width = 0;
        layout.beginLayout();
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(picture.width() * 0.9);
            height += leading;
            line.setPosition(QPointF(picture.width() * 0.05, height));
            height += line.height();
            width = qMax(width, line.naturalTextWidth());
        }
        layout.endLayout();
        width += fontSize / 4;
        textOffset = QPointF(-(picture.width() - width) / 2, 0);
        setFixedSize(qMax(1, int(std::ceil(width))), qMax(1, int(std::ceil(height))));
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 128));
        painter.drawRoundedRect(QRectF(rect()), 6, 6);
        QTextLayout::FormatRange format;
        format.start = 0;
        format.length = layout.text().size();
        format.format.setForeground(Qt::white);
        layout.draw(&painter, textOffset, {format});
    }
private:
    QTextLayout layout;
    QSize pictureSize;
    QPointF textOffset;
};

// Finder and Dock opens share the same activation path as single-instance requests.
class FileOpenEvents : public QObject {
public:
    explicit FileOpenEvents(PlayerWindow *window) : QObject(window), window(window) {
        qApp->installEventFilter(this);
    }
private:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object != qApp || event->type() != QEvent::FileOpen) return false;
        const auto url = static_cast<QFileOpenEvent *>(event)->url();
        if (!url.isLocalFile()) return false;
        window->openAndActivate(url.toLocalFile());
        event->accept();
        return true;
    }
    PlayerWindow *window;
};

constexpr auto popupMenuStyle = R"(
    QMenu { background: #242b3b; color: #e4eaf8; border: 1px solid #475575;
            padding: 4px; font-family: 'Segoe UI'; font-size: 13px; }
    QMenu::item { padding: 7px 24px; }
    QMenu::item:selected { background: #33415b; }
    QMenu::item:disabled { color: #596173; }
)";

void setCommandLabel(QAction *action, const QString &label) {
    const QString shortcuts = action->data().toString();
    action->setText(shortcuts.isEmpty() ? label : label + '\t' + shortcuts);
}

class TrackMenu final : public QMenu {
public:
    TrackMenu(QMediaPlayer *player, QWidget *parent, const QString &name, const QString &title,
              QList<QVariant> &excluded, std::function<void()> saveExclusions)
        : QMenu(parent), excluded(excluded), saveExclusions(std::move(saveExclusions)) {
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
        saveExclusions();
    }
    QList<QVariant> &excluded;
    std::function<void()> saveExclusions;
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
    stage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    notification = new QLabel(this);
    configureOverlay(notification, "actionNotification");
    notification->setStyleSheet("QLabel { color: #759eff; background: transparent; "
        "font-size: 14px; border: none; }");
    subtitleOverlay = new SubtitleOverlay(this);
    notificationTimer = new QTimer(this);
    notificationTimer->setSingleShot(true);
    notificationTimer->setInterval(3000);
    overlayTimer = new QTimer(this);
    overlayTimer->setInterval(50);
    connect(overlayTimer, &QTimer::timeout, this, &PlayerWindow::updateOverlays);
    connect(notificationTimer, &QTimer::timeout, this, &PlayerWindow::updateOverlays);
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
    auto *screen = new QWidget;
    auto *screenLayout = new QHBoxLayout(screen);
    screenLayout->setContentsMargins(0, 0, 0, 0);
    screenLayout->setSpacing(0);
    screenLayout->addWidget(stage, 1);
    folderPanel = new FolderPanel;
    folderPanel->hide();
    screenLayout->addWidget(folderPanel);
    connect(folderPanel, &FolderPanel::fileSelected, this, &PlayerWindow::openFile);
    layout->addWidget(screen, 1);
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
    controls->setContentsMargins(12, 0, 12, 0);
    controls->setHorizontalSpacing(16);
    controls->setVerticalSpacing(4);
    controls->setColumnStretch(0, 1);
    controls->addWidget(timeline, 0, 0, Qt::AlignVCenter);
    const auto addReadout = [controls](const QString &name, const QString &text, int column,
                                     Qt::Alignment alignment = Qt::AlignCenter) {
        auto *label = new QLabel(text);
        label->setObjectName(name + "Readout");
        label->setAlignment(alignment);
        controls->addWidget(label, 1, column);
        return label;
    };
    timeLabel = addReadout("time", {}, 0, Qt::AlignLeft | Qt::AlignVCenter);
    timeLabel->setAccessibleName("Current and total time, video codec and resolution");
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
    auto *volumeLabel = addReadout("volume", QString("Volume %1").arg(savedVolume), 1);
    gamma = addControlSlider("gamma", "Gamma", 2);
    gamma->setRange(1, 40);
    gamma->setValue(10);
    gamma->setPageStep(5);
    auto *gammaLabel = addReadout("gamma", "Gamma 1.0", 2);
    gamma->setToolTip("Gamma 1.0 (double-click to reset)");
    auto *filesButton = new QPushButton("List");
    filesButton->setObjectName("folderFilesButton");
    filesButton->setCheckable(true);
    filesButton->setFocusPolicy(Qt::NoFocus);
    filesButton->setToolTip("Show/hide folder files (L)");
    filesButton->setAccessibleName("Show/hide folder files");
    controls->addWidget(filesButton, 0, 3, Qt::AlignVCenter);
    connect(filesButton, &QPushButton::toggled, folderPanel, &QWidget::setVisible);
    connect(gamma, &QSlider::valueChanged, this, [gammaFilter, gammaLabel, this](int value) {
        const QString text = QString("Gamma %1").arg(value / 10.0, 0, 'f', 1);
        gammaLabel->setText(text);
        gamma->setToolTip(text + " (double-click to reset)");
        saveVideoSettings();
        gammaFilter->apply(player, value);
        updateSubtitles();
        if (!restoringVideoSettings) showNotification(text);
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
        QPushButton#folderFilesButton { padding: 0 12px; }
        QPushButton:hover { background: #33415b; }
        QPushButton:checked { background: #33415b; border-color: #759eff; }
        QPushButton:focus { border-color: #94b4ff; }
        QPushButton:disabled { color: #596173; background: #1b202c; border-color: #252c39; }
        QSlider { background: #171c27; border: none; border-radius: 7px; padding: 0 7px; }
        QSlider::groove:horizontal { background: #2b3243; height: 5px; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: #759eff; border-radius: 2px; }
        QSlider::handle:horizontal { background: #e5edff; width: 14px; margin: -5px 0; border-radius: 7px; }
        QSlider:disabled::sub-page:horizontal { background: #2b3243; }
        QToolTip { background: #242b3b; color: #e4eaf8; border: 1px solid #475575; }
    )");
    volume->ensurePolished();
    filesButton->setFixedHeight(qRound(volume->sizeHint().height() * 1.5));
    connect(emptyOpen, &QPushButton::clicked, this, &PlayerWindow::chooseFile);
    connect(volume, &QSlider::valueChanged, this, [audio, volumeLabel, this](int value) {
        volumeLabel->setText(QString("Volume %1").arg(value));
        audio->setVolume(value / 100.f);
        QSettings().setValue("audio/volume", value);
        showNotification(QString("Volume %1%").arg(value));
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
    connect(player, &QMediaPlayer::mediaStatusChanged, this, &PlayerWindow::restoreTrackSelections);
    connect(player, &QMediaPlayer::tracksChanged, this, &PlayerWindow::restoreTrackSelections);
    connect(player, &QMediaPlayer::seekableChanged, this, &PlayerWindow::restorePosition);
    connect(player, &QMediaPlayer::durationChanged, this, &PlayerWindow::restorePosition);
    connect(player, &QMediaPlayer::hasVideoChanged, this, &PlayerWindow::updateFullscreen);
    connect(player, &QMediaPlayer::errorOccurred, this, &PlayerWindow::updateControls);
    new FileOpenEvents(this);
    commandsMenu = new QMenu(this);
    commandsMenu->setObjectName("playerMenu");
    auto command = [this](const QString &name, const QString &label,
                          const QList<QKeySequence> &keys, auto callback) {
        auto *action = commandsMenu->addAction(label, this, callback);
        action->setObjectName(name);
        QStringList shortcuts;
        for (const auto &key : keys) {
            shortcuts.append(key.toString(QKeySequence::NativeText));
            auto *shortcut = new QShortcut(key, this);
            connect(shortcut, &QShortcut::activated, this, callback);
        }
        // Display the keys without registering a second shortcut on the menu action.
        action->setData(shortcuts.join(" / "));
        setCommandLabel(action, label);
        return action;
    };
    command("openVideo", "Open video", {QKeySequence::Open}, &PlayerWindow::chooseFile);
    command("folderFiles", "Show/hide folder files", {QKeySequence(Qt::Key_L)}, [filesButton] {
        filesButton->click();
    });
    auto *play = command("playPause", "Play", {QKeySequence(Qt::Key_Space)}, &PlayerWindow::togglePlayback);
    auto *fullscreen = command("fullscreen", "Fullscreen",
        {QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter),
         QKeySequence(Qt::Key_F), QKeySequence(Qt::Key_F11)}, &PlayerWindow::toggleFullscreen);
    commandsMenu->addSeparator();
    const QList<QAction *> seeking{
        command("back3", "Back 3 seconds", {QKeySequence(Qt::Key_Left)}, [this] { skip(-3000); }),
        command("forward3", "Forward 3 seconds", {QKeySequence(Qt::Key_Right)}, [this] { skip(3000); }),
        command("back30", "Back 30 seconds", {QKeySequence(Qt::SHIFT | Qt::Key_Left)}, [this] { skip(-30000); }),
        command("forward30", "Forward 30 seconds", {QKeySequence(Qt::SHIFT | Qt::Key_Right)}, [this] { skip(30000); })
    };
    keyframes = new KeyframeIndex(this);
    keyframeSeeking = commandsMenu->addAction("Keyframe seeking");
    keyframeSeeking->setObjectName("keyframeSeeking");
    keyframeSeeking->setCheckable(true);
    keyframeSeeking->setChecked(QSettings().value("seeking/keyframes", false).toBool());
    connect(keyframeSeeking, &QAction::toggled, this, [this](bool enabled) {
        QSettings().setValue("seeking/keyframes", enabled);
        lastTimelinePosition = -1;
        if (enabled) keyframes->load();
        showNotification(enabled ? "Keyframe seeking on" : "Precise seeking on");
    });
    connect(keyframes, &KeyframeIndex::finished, this, [this](bool available) {
        if (keyframeSeeking->isChecked())
            showNotification(available ? "Keyframe seeking ready"
                                      : "Keyframes unavailable; using precise seeking");
    });
    connect(timeline, &QSlider::sliderPressed, this, [this] { lastTimelinePosition = -1; });
    connect(timeline, &QSlider::sliderReleased, this, [this] {
        lastTimelinePosition = -1;
        updateTimeline();
    });
    commandsMenu->addSeparator();
    const QList<QAction *> tracks{
        command("nextAudio", "Next enabled audio track", {QKeySequence(Qt::Key_A)}, [this] { chooseTrack(true, true); }),
        command("audioTracks", "Audio tracks...", {QKeySequence(Qt::SHIFT | Qt::Key_A)}, [this] { chooseTrack(true); }),
        command("nextSubtitles", "Next enabled subtitle track", {QKeySequence(Qt::Key_S)}, [this] { chooseTrack(false, true); }),
        command("subtitles", "Subtitles...", {QKeySequence(Qt::SHIFT | Qt::Key_S)}, [this] { chooseTrack(false); })
    };
    commandsMenu->addSeparator();
    command("volumeUp", "Volume up", {QKeySequence(Qt::Key_Up)}, [this] { adjustVolume(1); });
    command("volumeDown", "Volume down", {QKeySequence(Qt::Key_Down)}, [this] { adjustVolume(-1); });
    command("gammaDown", "Decrease gamma by 0.1", {QKeySequence(Qt::Key_W)}, [this] { gamma->setValue(gamma->value() - 1); });
    command("gammaUp", "Increase gamma by 0.1", {QKeySequence(Qt::Key_E)}, [this] { gamma->setValue(gamma->value() + 1); });
    command("resetGamma", "Reset gamma to 1.0", {}, [this] { gamma->setValue(10); });
    commandsMenu->addSeparator();
    command("registerFileTypes", "Register file types", {}, [this] {
        const QString error = FileAssociations::registerFileTypes();
        if (!error.isEmpty()) {
            QMessageBox::warning(this, "Register file types", error);
            return;
        }
        QString message = "Video Player is registered for " + FileAssociations::extensions().join(", ").toUpper() + ".\n\n";
#ifdef Q_OS_WIN
        message += "Choose Video Player in Open with, or in Settings > Apps > Default apps to make it your default player.";
#elif defined(Q_OS_MACOS)
        message += "To make it the default, select a video in Finder, open Get Info, choose VideoPlayer under Open with, and click Change All.";
#else
        message += "Choose Video Player in your file manager's Open With menu to make it the default player.";
#endif
        QMessageBox::information(this, "Register file types", message);
    });
    auto *update = command("update", "Update to latest version", {}, [this] {
        if (updating) return;
        updating = true;
        const bool installed = AppUpdate::installLatest(this, player->source().toLocalFile());
        updating = false;
        if (installed) {
            savePosition();
            QApplication::quit();
        }
    });
    commandsMenu->addSeparator();
    command("exit", "Exit", {QKeySequence(Qt::Key_Escape)}, &PlayerWindow::close);
    connect(commandsMenu, &QMenu::aboutToShow, this, [this, play, fullscreen, seeking, tracks, update] {
        setCommandLabel(play, player->isPlaying() ? "Pause" : "Play");
        setCommandLabel(fullscreen, isFullScreen() ? "Leave fullscreen" : "Fullscreen");
        play->setEnabled(playbackReady());
        for (auto *action : seeking) action->setEnabled(timeline->isEnabled());
        for (auto *action : tracks) action->setEnabled(playbackReady());
        update->setEnabled(!updating);
    });
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

void PlayerWindow::openAndActivate(const QString &path) {
    if (!path.isEmpty()) openFile(path);
    if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
}

void PlayerWindow::chooseFile() {
    const QString path = QFileDialog::getOpenFileName(this, "Open video", {},
        QString("Video files (*.%1);;All files (*)").arg(FileAssociations::extensions().join(" *.")));
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
    execPopupMenu(*commandsMenu, position);
}

void PlayerWindow::chooseTrack(bool audio, bool cycle) {
    auto &excluded = audio ? excludedAudioTracks : excludedSubtitles;
    TrackMenu menu(player, this, audio ? "audioTrackMenu" : "subtitleMenu",
                   audio ? "Audio tracks" : "Subtitles", excluded, [&] {
        if (!fileKey.isEmpty())
            QSettings().setValue("videos/" + fileKey
                + (audio ? "/excludedAudioTracks" : "/excludedSubtitles"), excluded);
    });
    const QString external = audio ? externalAudio->filePath() : externalSubtitles.filePath();
    const int active = audio ? player->activeAudioTrack() : embeddedSubtitles->activeTrack();
    if (!audio) menu.addChoice("Off", -1, external.isEmpty() && active < 0);
    menu.addTracks(audio ? player->audioTracks() : player->subtitleTracks(),
                   external.isEmpty() ? active : -1, audio);
    menu.addFiles(audio ? ExternalAudio::matchingFiles(player->source()) : subtitleFiles(player->source()), external);
    if (menu.isEmpty()) menu.addAction("No audio tracks available")->setEnabled(false);
    const auto *selected = cycle ? menu.nextChoice() : execPopupMenu(menu, mapToGlobal(rect().center()));
    if (!selected || !applyTrackSelection(audio, selected->data())) return;
    (audio ? pendingAudioSelection : pendingSubtitleSelection).clear();
    if (!fileKey.isEmpty())
        QSettings().setValue("videos/" + fileKey + (audio ? "/audioTrack" : "/subtitleTrack"), selected->data());
    showNotification((audio ? "Audio: " : "Subtitles: ") + QString(selected->text()).replace("&&", "&"));
}

bool PlayerWindow::applyTrackSelection(bool audio, const QVariant &choice, bool reportErrors) {
    if (!choice.isValid()) return false;
    // INI settings may reload numeric values as strings; external paths are absolute.
    bool isIndex = false;
    const int index = choice.toInt(&isIndex);
    if (!isIndex) {
        const QFileInfo file(choice.toString());
        if (!file.isFile() || !file.isReadable()) return false;
        if (audio) {
            externalAudio->select(file.absoluteFilePath());
            return true;
        }
        QString error;
        if (!externalSubtitles.load(file.absoluteFilePath(), error)) {
            if (reportErrors) QMessageBox::warning(this, "Cannot load subtitles", error);
            return false;
        }
        embeddedSubtitles->select(-1);
    } else {
        const int count = audio ? player->audioTracks().size() : player->subtitleTracks().size();
        if (index < (audio ? 0 : -1) || index >= count) return false;
        if (audio) {
            externalAudio->clear();
            player->setActiveAudioTrack(index);
        } else {
            externalSubtitles.clear();
            embeddedSubtitles->select(index);
        }
    }
    updateSubtitles();
    return true;
}

void PlayerWindow::restoreTrackSelections() {
    const auto status = player->mediaStatus();
    if (status != QMediaPlayer::LoadedMedia && status != QMediaPlayer::BufferedMedia
        && status != QMediaPlayer::StalledMedia && status != QMediaPlayer::EndOfMedia) return;
    // Clear pending choices before applying them: selection can emit more media signals.
    const QVariant audio = pendingAudioSelection;
    const QVariant subtitles = pendingSubtitleSelection;
    pendingAudioSelection.clear();
    pendingSubtitleSelection.clear();
    applyTrackSelection(true, audio, false);
    applyTrackSelection(false, subtitles, false);
}

void PlayerWindow::updateSubtitles() {
    QString text;
    if (player->playbackState() != QMediaPlayer::StoppedState)
        text = externalSubtitles.filePath().isEmpty()
            ? embeddedSubtitles->text() : externalSubtitles.textAt(player->position());
    subtitleOverlay->setText(text);
    updateOverlays();
}

void PlayerWindow::updateSubtitleOverlay() {
    if (subtitleOverlay->text().isEmpty() || !isVisible() || isMinimized()
        || stage->currentWidget() != video) {
        subtitleOverlay->hide();
        return;
    }
    QSize pictureSize = video->videoSink()->videoSize();
    if (pictureSize.isEmpty()) pictureSize = video->size();
    pictureSize.scale(video->size(), Qt::KeepAspectRatio);
    QRect picture(QPoint(), pictureSize);
    picture.moveCenter(video->rect().center());
    static_cast<SubtitleOverlay *>(subtitleOverlay)->layoutCaption(pictureSize);
    subtitleOverlay->move(video->mapToGlobal(QPoint(
        picture.x() + (picture.width() - subtitleOverlay->width()) / 2,
        picture.bottom() + 1 - picture.height() / 20 - subtitleOverlay->height())));
    subtitleOverlay->show();
    subtitleOverlay->raise();
}

void PlayerWindow::openFile(const QString &path) {
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable()) {
        statusLabel->setText("Cannot open file: " + path);
        return;
    }
    savePosition();
    pendingAudioSelection.clear();
    pendingSubtitleSelection.clear();
    notificationTimer->stop();
    notification->hide();
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
    settings.beginGroup("videos/" + fileKey);
    excludedAudioTracks = settings.value("excludedAudioTracks").toList();
    excludedSubtitles = settings.value("excludedSubtitles").toList();
    restoringVideoSettings = true;
    // QSlider clamps restored values to its configured range.
    gamma->setValue(settings.value("gamma", 10).toInt());
    restoringVideoSettings = false;
    saveVideoSettings();
    frameReady = false;
    stage->setCurrentIndex(2);
    setWindowTitle(file.fileName() + QStringLiteral(" - Video Player v" VIDEO_PLAYER_VERSION));
    const auto source = QUrl::fromLocalFile(file.absoluteFilePath());
    lastTimelinePosition = -1;
    keyframes->setSource(source);
    if (keyframeSeeking->isChecked()) keyframes->load();
    player->setSource(source);
    pendingAudioSelection = settings.value("audioTrack");
    pendingSubtitleSelection = settings.value("subtitleTrack");
    restoreTrackSelections();
    folderPanel->setFile(file.absoluteFilePath());
    restorePosition();
    player->play();
}

void PlayerWindow::saveVideoSettings() {
    if (restoringVideoSettings) return;
    QSettings settings;
    if (!fileKey.isEmpty()) {
        settings.beginGroup("videos/" + fileKey);
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
    notificationTimer->stop();
    overlayTimer->stop();
    notification->hide();
    fullscreenClose->hide();
    subtitleOverlay->hide();
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
    if (steps == 0) return;
    int value = volume->value();
    while (steps > 0 && value < volume->maximum()) {
        value = qMin(volume->maximum(), value + (value < 5 ? 1 : 5));
        --steps;
    }
    while (steps < 0 && value > volume->minimum()) {
        value = value <= 5 ? value - 1 : qMax(5, value - 5);
        ++steps;
    }
    if (value == volume->value()) showNotification(QString("Volume %1%").arg(value));
    else volume->setValue(value);
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
    controlsPanel->layout()->setContentsMargins(12, fullscreen ? 12 : 0, 12, fullscreen ? 12 : 0);
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
    const bool overControls = controlsPanel->geometry().contains(globalCursor);
    controlsPanel->setVisible(overControls || timeline->isSliderDown()
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

qint64 PlayerWindow::keyframePosition(qint64 position, bool forward) const {
    if (keyframeSeeking->isChecked()) position = keyframes->seekPosition(position, forward);
    return qBound(qint64(0), position, player->duration());
}

void PlayerWindow::seekToSlider() {
    if (player->isSeekable()) {
        const qint64 position = keyframePosition(
            qRound64(timeline->value() * (double(player->duration()) / timelineSteps)));
        if (!timeline->isSliderDown() || position != lastTimelinePosition) {
            lastTimelinePosition = position;
            externalAudio->seek(position);
        }
    }
    updateTimeline();
}

void PlayerWindow::skip(qint64 delta) {
    if (!timeline->isEnabled()) return;
    qint64 position = qBound(qint64(0), player->position() + delta, player->duration());
    if (position > 0 && position < player->duration()) position = keyframePosition(position, delta > 0);
    externalAudio->seek(position);
    showNotification(QString("%1 %2 seconds — %3")
        .arg(delta < 0 ? "Backward" : "Forward").arg(qAbs(delta) / 1000).arg(timestamp(position)));
}

void PlayerWindow::showNotification(const QString &text) {
    notification->setText(text.simplified());
    notificationTimer->start();
    updateOverlays();
}

void PlayerWindow::updateOverlays() {
    if (notificationTimer->isActive() || !subtitleOverlay->text().isEmpty()) {
        if (!overlayTimer->isActive()) overlayTimer->start();
    } else {
        overlayTimer->stop();
    }
    updateNotification();
    updateSubtitleOverlay();
}

void PlayerWindow::updateNotification() {
    if (!notificationTimer->isActive() || !isVisible() || isMinimized()) {
        notification->hide();
        return;
    }
    notification->setMaximumWidth(qMax(1, stage->width() - 24));
    notification->adjustSize();
    notification->move(stage->mapToGlobal(QPoint(12, 12)));
    notification->show();
    notification->raise();
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
