#pragma once
#include <QObject>
#include <QVideoSink>

class QMediaPlayer;

// Decode embedded captions independently so changing them cannot flush playback buffers.
class EmbeddedSubtitles : public QObject {
    Q_OBJECT
public:
    explicit EmbeddedSubtitles(QMediaPlayer *video, QObject *parent = nullptr);
    void select(int track);
    void clear();
    int activeTrack() const { return selected; }
    QString text() const;
signals:
    void textChanged();
    void failed(const QString &message);
private:
    void synchronize();
    QMediaPlayer *video;
    QMediaPlayer *reader;
    QVideoSink sink;
    int selected = -1;
    bool ready = false;
    qint64 finishedAt = -1;
};
