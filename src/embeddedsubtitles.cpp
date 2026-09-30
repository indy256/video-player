#include "embeddedsubtitles.h"
#include <QMediaPlayer>

EmbeddedSubtitles::EmbeddedSubtitles(QMediaPlayer *video, QObject *parent)
    : QObject(parent), video(video), reader(new QMediaPlayer(this)) {
    reader->setObjectName("subtitlePlayer");
    reader->setVideoSink(&sink);
    connect(&sink, &QVideoSink::subtitleTextChanged, this, &EmbeddedSubtitles::textChanged);
    connect(video, &QMediaPlayer::sourceChanged, this, &EmbeddedSubtitles::clear);
    connect(video, &QMediaPlayer::positionChanged, this, &EmbeddedSubtitles::synchronize);
    connect(video, &QMediaPlayer::playbackStateChanged, this, &EmbeddedSubtitles::synchronize);
    connect(video, &QMediaPlayer::playbackRateChanged, this, &EmbeddedSubtitles::synchronize);
    connect(reader, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) finishedAt = this->video->position();
        // LoadedMedia is also emitted after seeking.
        if (ready || status != QMediaPlayer::LoadedMedia) return;
        ready = true;
        reader->setActiveAudioTrack(-1);
        reader->setActiveVideoTrack(-1);
        synchronize();
    });
    connect(reader, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
        clear();
        emit failed(message);
    });
}

void EmbeddedSubtitles::select(int track) {
    if (track == selected) return;
    selected = track;
    sink.setSubtitleText({});
    if (track >= 0 && reader->source().isEmpty()) reader->setSource(video->source());
    synchronize();
}

void EmbeddedSubtitles::clear() {
    selected = -1;
    ready = false;
    finishedAt = -1;
    reader->stop();
    reader->setSource({});
    sink.setSubtitleText({});
}

QString EmbeddedSubtitles::text() const {
    return selected < 0 ? QString() : sink.subtitleText();
}

void EmbeddedSubtitles::synchronize() {
    if (!ready) return;
    if (selected < 0) {
        reader->pause();
        return;
    }
    const bool trackChanged = reader->activeSubtitleTrack() != selected;
    if (trackChanged) reader->setActiveSubtitleTrack(selected);
    if (video->playbackState() == QMediaPlayer::StoppedState) {
        reader->stop();
        finishedAt = -1;
        return;
    }
    const qint64 position = video->position();
    // Subtitles often end before the movie. Resume reading only after seeking back or selecting a track.
    if (finishedAt >= 0 && !trackChanged && position >= finishedAt) return;
    finishedAt = -1;
    reader->setPlaybackRate(video->playbackRate());
    const bool playing = video->isPlaying();
    if (!playing) reader->pause();
    if (trackChanged || (playing && !reader->isPlaying()) || (!playing && reader->position() != position)
        || qAbs(reader->position() - position) > 250)
        reader->setPosition(position);
    if (playing) reader->play();
}
