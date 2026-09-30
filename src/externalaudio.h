#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>

class QAudioOutput;
class QMediaPlayer;
class QTimer;
class QUrl;

class ExternalAudio : public QObject {
    Q_OBJECT
public:
    explicit ExternalAudio(QMediaPlayer *player, QObject *parent = nullptr);
    static QStringList matchingFiles(const QUrl &video);
    QString filePath() const { return path; }
    void select(const QString &file);
    void clear();
    void seek(qint64 position);
signals:
    void failed(const QString &message);
private:
    void synchronize();
    void startAudio();
    void resetRates();
    void updateOutputs();
    void setVideoRate(qreal rate);
    void fail(const QString &message);
    QMediaPlayer *video;
    QMediaPlayer *audio;
    QAudioOutput *originalOutput;
    QAudioOutput *externalOutput;
    QTimer *handoffTimer;
    QString path;
    bool ready = false;
    bool synchronizing = false;
    bool externalActive = false;
    bool muted = false;
    bool updatingOutputs = false;
    bool updatingRate = false;
    qreal playbackRate = 1;
    QElapsedTimer rateUpdate;
};
