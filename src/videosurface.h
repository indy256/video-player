#pragma once
#include <QWidget>
#include <QVideoFrame>
class QMediaPlayer;
class QVideoSink;
class QQuickWidget;
class GammaFilter;

// Keeps playback controls independent of the GPU surface and its CPU fallback.
class VideoSurface : public QWidget {
    Q_OBJECT
public:
    explicit VideoSurface(QWidget *parent = nullptr);
    QVideoSink *videoSink() const { return sink; }
    bool usesGpuGamma() const { return quick != nullptr; }
    void setPlayer(QMediaPlayer *player);
    void setGamma(int tenths);
    QRect contentRect() const;
    QImage grabFrame();
signals:
    void videoFrameChanged(const QVideoFrame &frame);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private slots:
    void useCpuRenderer();
private:
    void watchSurface(QWidget *surface);
    QMediaPlayer *player = nullptr;
    QVideoSink *sink = nullptr;
    QQuickWidget *quick = nullptr;
    GammaFilter *filter = nullptr;
    int gamma = 10;
};
