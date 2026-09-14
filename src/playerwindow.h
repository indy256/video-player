#pragma once
#include <QMainWindow>
#include <QMediaPlayer>
class QLabel;
class QPushButton;
class QStackedWidget;
class QVideoWidget;
class QAudioOutput;
class SeekSlider;

class PlayerWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit PlayerWindow(QWidget *parent = nullptr);
    void openFile(const QString &path);
protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    void chooseFile();
    void togglePlayback();
    void updateControls();
    void updateTimeline();
    void seekToSlider();
    void skip(qint64 delta);
    QMediaPlayer *player;
    QAudioOutput *audio;
    QVideoWidget *video;
    QStackedWidget *stage;
    QLabel *fileLabel;
    QLabel *statusLabel;
    QLabel *timeLabel;
    QPushButton *playButton;
    QPushButton *backButton;
    QPushButton *forwardButton;
    SeekSlider *timeline;
    bool scrubbing = false;
    bool resumeAfterScrub = false;
};
