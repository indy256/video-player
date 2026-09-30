#pragma once
#include <QMainWindow>
#include <QMediaPlayer>
class QLabel;
class QStackedWidget;
class QVideoWidget;
class QAudioOutput;
class SeekSlider;
class QTimer;
class QSlider;
class QContextMenuEvent;
class QAbstractButton;

class PlayerWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit PlayerWindow(QWidget *parent = nullptr);
    void openFile(const QString &path);
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
    bool playbackReady() const;
    void toggleFullscreen();
    void updateFullscreen();
    void updateFullscreenControls();
    void chooseFile();
    void showPopupMenu(const QPoint &position);
    void togglePlayback();
    void updateControls();
    void updateTimeline();
    void seekToSlider();
    void skip(qint64 delta);
    void adjustVolume(int steps);
    QMediaPlayer *player;
    QAudioOutput *audio;
    QVideoWidget *video;
    QStackedWidget *stage;
    QLabel *statusLabel;
    QLabel *timeLabel;
    SeekSlider *timeline;
    QTimer *clickTimer;
    QSlider *volume;
    QWidget *controlsPanel;
    QAbstractButton *fullscreenClose;
    QTimer *controlsTimer;
    QByteArray windowedGeometry;
    int wheelRemainder = 0;
    bool mousePressed = false;
    bool draggingWindow = false;
    QPoint dragStartMouse;
    QPoint dragStartWindow;
    bool frameReady = false;
    bool updating = false;
    QString positionKey;
    qint64 pendingPosition = -1;
};
