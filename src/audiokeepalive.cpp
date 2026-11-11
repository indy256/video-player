#include "audiokeepalive.h"

#ifdef Q_OS_WIN
#include <QAudioDevice>
#include <QAudioOutput>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QTimer>
#include <cstring>
#include <memory>

namespace {
class Silence : public QIODevice {
public:
    Silence() { open(QIODevice::ReadOnly); }
    bool isSequential() const override { return true; }
    bool atEnd() const override { return false; }
    qint64 bytesAvailable() const override { return 65536 + QIODevice::bytesAvailable(); }
    unsigned char value = 0;
protected:
    qint64 readData(char *data, qint64 size) override {
        std::memset(data, value, size_t(size));
        return size;
    }
    qint64 writeData(const char *, qint64) override { return -1; }
};

class AudioKeepAlive : public QObject {
public:
    explicit AudioKeepAlive(QMediaPlayer *player) : QObject(player), player(player) {
        release.setSingleShot(true);
        // Brief pauses for video preparation must not close the Bluetooth stream.
        release.setInterval(2000);
        connect(&release, &QTimer::timeout, this, [this] { sink.reset(); });
        connect(player, &QMediaPlayer::playbackStateChanged, this, [this] { update(); });
        connect(player, &QMediaPlayer::hasAudioChanged, this, [this] { update(); });
        connect(player->audioOutput(), &QAudioOutput::deviceChanged, this, [this] { update(); });
        connect(&devices, &QMediaDevices::audioOutputsChanged, this, [this] { update(); });
        update();
    }
private:
    void update() {
        if (!player->hasAudio() || player->playbackState() == QMediaPlayer::StoppedState) {
            release.stop();
            sink.reset();
            return;
        }
        if (!player->isPlaying()) {
            if (sink && !release.isActive()) release.start();
            return;
        }
        release.stop();
        auto device = player->audioOutput()->device();
        if (device.isNull()) device = QMediaDevices::defaultAudioOutput();
        if (sink && device == currentDevice) return;
        sink.reset();
        currentDevice = device;
        if (device.isNull()) return;
        const auto format = device.preferredFormat();
        if (!format.isValid()) return;
        silence.value = format.sampleFormat() == QAudioFormat::UInt8 ? 128 : 0;
        sink = std::make_unique<QAudioSink>(device, format, this);
        sink->setObjectName("audioKeepAliveSink");
        // This stream contains only digital silence; media retains its own output,
        // volume, timing, and mute controls. Do not reset it when media seeks.
        sink->start(&silence);
        if (sink->error() != QAudio::NoError) sink.reset();
    }
    QMediaPlayer *player;
    QMediaDevices devices;
    QTimer release;
    QAudioDevice currentDevice;
    Silence silence;
    std::unique_ptr<QAudioSink> sink;
};
}
#endif

void installAudioKeepAlive(QMediaPlayer *player) {
#ifdef Q_OS_WIN
    new AudioKeepAlive(player);
#else
    Q_UNUSED(player);
#endif
}
