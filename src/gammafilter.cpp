#include "gammafilter.h"
#include <QImage>
#include <QMediaPlayer>
#include <QSignalBlocker>
#include <cmath>

GammaFilter::GammaFilter(QVideoSink *output, QObject *parent)
    : QObject(parent), destination(output) {
    connect(&source, &QVideoSink::videoFrameChanged, this, [this] { present(); });
    connect(&source, &QVideoSink::subtitleTextChanged, destination, &QVideoSink::setSubtitleText);
}

void GammaFilter::apply(QMediaPlayer *player, int tenths) {
    tenths = qBound(1, tenths, 40);
    QVideoSink *target = tenths == 10 ? destination : &source;
    if (player->videoSink() != target) {
        // Preserve the original frame and subtitles when switching, including while paused.
        QVideoSink *current = player->videoSink();
        const QVideoFrame frame = current->videoFrame();
        const QString subtitle = current->subtitleText();
        const QSignalBlocker blocker(&source);
        player->setVideoSink(target);
        target->setVideoFrame(frame);
        target->setSubtitleText(subtitle);
    }
    if (tenths == 10) {
        // Native rendering bypasses the filter and releases its cached frame.
        gammaTenths = 10;
        const QSignalBlocker blocker(&source);
        source.setVideoFrame({});
        return;
    }
    setGamma(tenths);
}

void GammaFilter::setGamma(int tenths) {
    tenths = qBound(1, tenths, 40);
    if (gammaTenths == tenths) return;
    gammaTenths = tenths;
    if (gammaTenths != 10) {
        for (int i = 0; i < 256; ++i)
            lookup[i] = uchar(qRound(255 * std::pow(i / 255.0, 10.0 / gammaTenths)));
    }
    // Always start from the original frame, including while paused.
    present();
}

void GammaFilter::present() {
    destination->setVideoFrame(adjustedFrame(source.videoFrame()));
}

QVideoFrame GammaFilter::adjustedFrame(const QVideoFrame &original) const {
    if (gammaTenths == 10 || !original.isValid()) return original;
    const QImage image = original.toImage().convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) return original;
    // Allocate a separate frame: mapped QVideoFrames share their pixel storage.
    QVideoFrame adjusted(QVideoFrameFormat(image.size(), QVideoFrameFormat::Format_RGBA8888));
    if (!adjusted.map(QVideoFrame::WriteOnly)) return original;
    for (int y = 0; y < image.height(); ++y) {
        const uchar *in = image.constScanLine(y);
        uchar *out = adjusted.bits(0) + y * adjusted.bytesPerLine(0);
        for (int x = 0; x < image.width(); ++x, in += 4, out += 4) {
            out[0] = lookup[in[0]];
            out[1] = lookup[in[1]];
            out[2] = lookup[in[2]];
            out[3] = in[3];
        }
    }
    adjusted.unmap();
    adjusted.setStartTime(original.startTime());
    adjusted.setEndTime(original.endTime());
    adjusted.setSubtitleText(original.subtitleText());
    // toImage applies the surface format, but not the frame's presentation transform.
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    adjusted.setRotation(original.rotation());
#else
    adjusted.setRotationAngle(original.rotationAngle());
#endif
    adjusted.setMirrored(original.mirrored());
    return adjusted;
}
