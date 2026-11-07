#include "externalaudio.h"
#include <QAudioOutput>
#include <QDir>
#include <QFileInfo>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QScopedValueRollback>
#include <QTimer>

ExternalAudio::ExternalAudio(QMediaPlayer *player, QObject *parent)
    : QObject(parent), video(player), audio(new QMediaPlayer(this)),
      originalOutput(player->audioOutput()), externalOutput(new QAudioOutput(this)),
      handoffTimer(new QTimer(this)), muted(originalOutput->isMuted()), playbackRate(player->playbackRate()) {
    audio->setObjectName("externalAudioPlayer");
    externalOutput->setVolume(originalOutput->volume());
    externalOutput->setMuted(true);
    audio->setAudioOutput(externalOutput);
    connect(originalOutput, &QAudioOutput::volumeChanged, externalOutput, &QAudioOutput::setVolume);
    connect(originalOutput, &QAudioOutput::mutedChanged, this, [this](bool value) {
        if (updatingOutputs) return;
        muted = value;
        updateOutputs();
    });
    handoffTimer->setSingleShot(true);
    handoffTimer->setInterval(200);
    connect(handoffTimer, &QTimer::timeout, this, [this] {
        if (!ready || !audio->isPlaying() || !video->isPlaying()) return;
        if (audio->mediaStatus() != QMediaPlayer::BufferedMedia) {
            handoffTimer->start();
            return;
        }
        externalActive = true;
        updateOutputs();
        rateUpdate.restart();
    });
    connect(video, &QMediaPlayer::positionChanged, this, [this] { synchronize(); });
    connect(video, &QMediaPlayer::playbackStateChanged, this, [this] { synchronize(); });
    connect(video, &QMediaPlayer::mediaStatusChanged, this, [this] { synchronize(); });
    connect(video, &QMediaPlayer::sourceChanged, this, &ExternalAudio::updateSource);
    connect(video, &QMediaPlayer::activeTracksChanged, this, [this] {
        if (!path.isEmpty()) return;
        updateSource();
        synchronize();
    });
    connect(video, &QMediaPlayer::playbackRateChanged, this, [this](qreal rate) {
        if (updatingRate) return;
        playbackRate = rate;
        audio->setPlaybackRate(rate);
        rateUpdate.restart();
    });
    connect(audio, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        // LoadedMedia also occurs after seeking; initialize each source only once.
        if (synchronizing || audio->source().isEmpty() || ready || status != QMediaPlayer::LoadedMedia) return;
        if (audio->audioTracks().isEmpty()) {
            fail("The selected file contains no audio track.");
            return;
        }
        ready = true;
        audio->setActiveVideoTrack(-1);
        audio->setActiveSubtitleTrack(-1);
        synchronize();
    });
    connect(audio, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
        if (!audio->source().isEmpty()) fail(message);
    });
}

QStringList ExternalAudio::matchingFiles(const QUrl &video) {
    if (!video.isLocalFile()) return {};
    const QFileInfo source(video.toLocalFile());
    const QString prefix = source.completeBaseName();
    static const QStringList extensions{"aac", "ac3", "aif", "aiff", "alac", "ape", "dts", "eac3",
        "flac", "m4a", "mka", "mp2", "mp3", "oga", "ogg", "opus", "wav", "wma", "wv"};
    QStringList files;
    for (const auto &file : source.absoluteDir().entryInfoList(
            QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase)) {
        if (file.absoluteFilePath() != source.absoluteFilePath()
            && file.fileName().startsWith(prefix, Qt::CaseInsensitive)
            && extensions.contains(file.suffix().toLower()))
            files.append(file.absoluteFilePath());
    }
    return files;
}

void ExternalAudio::select(const QString &file) {
    path = file;
    updateSource();
    synchronize();
}

void ExternalAudio::clear() {
    path.clear();
    unload();
}

void ExternalAudio::unload() {
    const QScopedValueRollback guard(synchronizing, true);
    ready = false;
    handoffTimer->stop();
    setVideoRate(playbackRate);
    externalActive = false;
    audioInRange = true;
    audio->stop();
    audio->setSource({});
    updateOutputs();
}

void ExternalAudio::updateSource() {
    const QUrl source = !path.isEmpty() ? QUrl::fromLocalFile(path)
        : audioOffset != 0 && video->activeAudioTrack() >= 0 ? video->source() : QUrl();
    if (source == audio->source()) return;
    unload();
    audio->setSource(source);
    updateOutputs();
}

void ExternalAudio::setOffset(qint64 milliseconds) {
    if (audioOffset == milliseconds) return;
    audioOffset = milliseconds;
    updateSource();
    synchronize(true);
}

void ExternalAudio::updateOutputs() {
    const QScopedValueRollback guard(updatingOutputs, true);
    // Muting keeps the embedded decoder and its clock intact during the handoff.
    const bool shiftedEmbedded = path.isEmpty() && audioOffset != 0 && !audio->source().isEmpty();
    originalOutput->setMuted(externalActive || shiftedEmbedded || muted);
    externalOutput->setMuted(!externalActive || !audioInRange || muted);
}

void ExternalAudio::setVideoRate(qreal rate) {
    const QScopedValueRollback guard(updatingRate, true);
    video->setPlaybackRate(rate);
}

void ExternalAudio::resetRates() {
    setVideoRate(playbackRate);
    audio->setPlaybackRate(playbackRate);
    rateUpdate.restart();
}

void ExternalAudio::startAudio() {
    audio->play();
    if (!externalActive) handoffTimer->start();
}

void ExternalAudio::fail(const QString &message) {
    const QString name = QFileInfo(audio->source().toLocalFile()).fileName();
    clear();
    emit failed(name + ": " + message);
}

void ExternalAudio::seek(qint64 position) {
    {
        const QScopedValueRollback guard(synchronizing, true);
        video->setPosition(position);
    }
    synchronize(true);
}

void ExternalAudio::synchronize(bool forceSeek) {
    if (!ready || synchronizing) return;
    const QScopedValueRollback guard(synchronizing, true);
    if (path.isEmpty() && audio->activeAudioTrack() != video->activeAudioTrack())
        audio->setActiveAudioTrack(video->activeAudioTrack());
    if (video->playbackState() == QMediaPlayer::StoppedState) {
        handoffTimer->stop();
        if (audio->playbackState() != QMediaPlayer::StoppedState) audio->stop();
        return;
    }
    const bool playing = video->isPlaying() && video->mediaStatus() != QMediaPlayer::StalledMedia;
    // Positive offsets delay sound; negative offsets play it ahead of video.
    const qint64 position = video->position() - audioOffset;
    audioInRange = position >= 0 && (audio->duration() <= 0 || position < audio->duration());
    // Stay silent before delayed audio starts and after a shorter track ends.
    if (!audioInRange) {
        handoffTimer->stop();
        externalActive = true;
        updateOutputs();
        if (audio->isPlaying()) audio->pause();
        resetRates();
        return;
    }
    updateOutputs();
    if (forceSeek || !playing || !audio->isPlaying()) {
        handoffTimer->stop();
        if (!playing && audio->playbackState() != QMediaPlayer::PausedState) audio->pause();
        if (audio->position() != position) audio->setPosition(position);
        resetRates();
        if (playing) startAudio();
        return;
    }
    if (!externalActive) {
        if (!handoffTimer->isActive()) startAudio();
        return;
    }
    // Keep audible audio at a steady rate: changing it rebuilds Qt's audio
    // converter. Let video gently follow the external audio clock instead.
    if (rateUpdate.elapsed() >= 500) {
        const qint64 drift = audio->position() - position;
        qreal rate = video->playbackRate();
        if (drift > 80) rate = playbackRate * 1.02;
        else if (drift < -80) rate = playbackRate * 0.98;
        else if (qAbs(drift) < 25) rate = playbackRate;
        if (qAbs(video->playbackRate() - rate) >= 0.001) setVideoRate(rate);
        rateUpdate.restart();
    }
}
