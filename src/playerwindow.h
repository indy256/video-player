#pragma once
#include <QMainWindow>
#include <QMediaPlayer>
#include <QVariant>
#include <optional>
#include "subtitles.h"
class QLabel;
class QStackedWidget;
class VideoSurface;
class QMenu;
class QAction;
class SeekSlider;
class QTimer;
class QSlider;
class QContextMenuEvent;
class QAbstractButton;
class ExternalAudio;
class EmbeddedSubtitles;
class FolderPanel;
class KeyframeIndex;
class QVideoFrame;

class PlayerWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit PlayerWindow(QWidget *parent = nullptr);
    void openFile(const QString &path);
    void openAndActivate(const QString &path);
protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif
    void closeEvent(QCloseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    void savePosition();
    void restorePosition();
    void saveVideoSettings();
    void restoreTrackSelections();
    bool applyTrackSelection(bool audio, const QVariant &choice, bool reportErrors = true);
    bool playbackReady() const;
    void toggleFullscreen();
    void updateFullscreen();
    void updateFullscreenControls();
    void chooseFile();
    void showPopupMenu(const QPoint &position, const QStringList &paths = {});
    QAction *execPopupMenu(QMenu &menu, const QPoint &position);
    void chooseTrack(bool audio, bool cycle = false);
    void updateSubtitles();
    void updateSubtitleOverlay();
    void togglePlayback();
    void updateControls();
    void updateTimeline();
    qint64 keyframePosition(qint64 position, bool forward = false) const;
    bool atEnd() const;
    void seekTo(qint64 position);
    void cancelPendingPlayback();
    bool frameReadyForPlayback(const QVideoFrame &frame, bool allowImprecise) const;
    void seekToSlider();
    void skip(qint64 delta);
    void adjustVolume(int steps);
    void showNotification(const QString &text);
    void updateNotification();
    void updateOverlays();
    QLabel *notification;
    QLabel *subtitleOverlay;
    QTimer *notificationTimer;
    QTimer *overlayTimer;
    QMediaPlayer *player;
    ExternalAudio *externalAudio;
    EmbeddedSubtitles *embeddedSubtitles;
    VideoSurface *video;
    QStackedWidget *stage;
    FolderPanel *folderPanel;
    QLabel *statusLabel;
    QLabel *timeLabel;
    SeekSlider *timeline;
    KeyframeIndex *keyframes;
    QAction *keyframeSeeking;
    qint64 lastTimelinePosition = -1;
    QTimer *clickTimer;
    QTimer *resumeTimer;
    QSlider *volume;
    QSlider *gamma = nullptr;
    QWidget *controlsPanel;
    QAbstractButton *fullscreenClose;
    QTimer *controlsTimer;
    QMenu *commandsMenu;
    QByteArray windowedGeometry;
    int wheelRemainder = 0;
    bool mousePressed = false;
    bool draggingWindow = false;
    QPoint dragStartMouse;
    QPoint dragStartWindow;
    struct PlaybackRequest {
        bool play;
        std::optional<qint64> position;
    };
    std::optional<PlaybackRequest> pendingPlayback;
    bool updating = false;
    QString fileKey;
    bool restoringVideoSettings = false;
    // Empty after restoring; a negative position requests the midpoint.
    std::optional<qint64> pendingPosition;
    QVariant pendingAudioSelection;
    QVariant pendingSubtitleSelection;
    SubtitleTrack externalSubtitles;
    QList<QVariant> excludedAudioTracks;
    QList<QVariant> excludedSubtitles;
};
