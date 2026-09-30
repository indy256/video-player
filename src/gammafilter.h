#pragma once
#include <QVideoFrame>
#include <QVideoSink>
#include <array>

class GammaFilter : public QObject {
public:
    explicit GammaFilter(QVideoSink *output, QObject *parent = nullptr);
    QVideoSink *input() { return &source; }
    void setGamma(int tenths);
private:
    void present();
    QVideoSink source;
    QVideoSink *destination;
    std::array<uchar, 256> lookup{};
    int gammaTenths = 10;
};
