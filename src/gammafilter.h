#pragma once
#include <QVideoFrame>
#include <QVideoSink>
#include <array>
class QMediaPlayer;

class GammaFilter : public QObject {
public:
    explicit GammaFilter(QVideoSink *output, QObject *parent = nullptr);
    QVideoSink *input() { return &source; }
    void setGamma(int tenths);
    void apply(QMediaPlayer *player, int tenths);
private:
    void present();
    QVideoFrame adjustedFrame(const QVideoFrame &original) const;
    QVideoSink source;
    QVideoSink *destination;
    std::array<uchar, 256> lookup{};
    int gammaTenths = 10;
};
