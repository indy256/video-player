// Included by the Qt FFmpeg demuxer in the bundled backend.
#pragma once

#include <QFileInfo>

static int seekWithAudioPreroll(AVFormatContext *ctx, int64_t target, bool hasAudio, QObject &demuxer)
{
    // The first seek also loads deferred Matroska cues.
    auto seekNormally = [&] { return av_seek_frame(ctx, -1, target, AVSEEK_FLAG_BACKWARD); };
    demuxer.setProperty("audioPrerollKey", {});
    const int result = seekNormally();
    const QByteArrayView format(ctx->iformat->name);
    const bool avi = format == "avi";
    if (result < 0 || !hasAudio || (!avi && !format.startsWith("matroska")))
        return result;
    const int track = av_find_default_stream_index(ctx);
    if (track < 0 || ctx->streams[track]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        return result;
    AVStream *stream = ctx->streams[track];
    const auto pts = av_rescale_q(target, AV_TIME_BASE_Q, stream->time_base);
    const int index = av_index_search_timestamp(stream, pts, AVSEEK_FLAG_BACKWARD);
    if (index <= 0)
        return result;
    const auto *cue = avformat_index_get_entry(stream, index);
    // Matroska laces can span a cue. AVI chunk boundaries can split a
    // compressed audio frame. An AVI index also includes non-key video chunks.
    const int64_t warmupUs = avi ? 250000 : 500000;
    if (!cue || av_compare_ts(pts - cue->timestamp, stream->time_base, warmupUs, AV_TIME_BASE_Q) > 0
        || !QFileInfo(QString::fromUtf8(ctx->url)).isFile())
        return result;
    int previousKey = index - 1;
    if (avi) {
        const int64_t warmupTicks = qMax<int64_t>(1, av_rescale_q(warmupUs, AV_TIME_BASE_Q, stream->time_base));
        previousKey = qMax(0, av_index_search_timestamp(stream, pts - warmupTicks, AVSEEK_FLAG_BACKWARD));
    }
    if (previousKey >= index)
        return result;
    const auto *previous = avformat_index_get_entry(stream, previousKey);
    if (!previous || previous->pos < 0 || cue->pos <= previous->pos
        || cue->pos - previous->pos > 16 * 1024 * 1024)
        return result;
    const auto key = cue->timestamp;
    if (av_seek_frame(ctx, track, previous->timestamp, AVSEEK_FLAG_BACKWARD) < 0)
        return seekNormally();
    demuxer.setProperty("audioPrerollKey", qint64(key));
    return 0;
}

// Keep the original Qt class layout for the backend built against the binary kit.
// Only inspect packets in the extra cluster. Preserve codec warm-up samples.
static int readWithAudioPreroll(AVFormatContext *ctx, AVPacket *packet, int64_t target,
                                QObject &demuxer)
{
    bool preroll;
    const auto key = demuxer.property("audioPrerollKey").toLongLong(&preroll);
    const int video = preroll ? av_find_default_stream_index(ctx) : -1;
    for (;;) {
        const int result = av_read_frame(ctx, packet);
        if (result < 0 || !preroll)
            return result;
        const auto *stream = ctx->streams[packet->stream_index];
        const auto *codec = stream->codecpar;
        if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            const auto warmup = codec->sample_rate > 0
                ? av_rescale(codec->seek_preroll, AV_TIME_BASE, codec->sample_rate) : 0;
            const auto cutoff = target - qMax<int64_t>(250000, warmup);
            if (packet->pts == AV_NOPTS_VALUE || packet->duration <= 0
                || av_compare_ts(packet->pts + packet->duration, stream->time_base,
                                 cutoff, AV_TIME_BASE_Q) >= 0)
                return result;
        } else if (packet->stream_index == video) {
            // Resume ordinary demuxing even with a missing or mislabelled cue.
            const auto timestamp = QByteArrayView(ctx->iformat->name) == "avi" ? packet->dts : packet->pts;
            const bool reachedCue = timestamp != AV_NOPTS_VALUE && timestamp >= key;
            if (timestamp == AV_NOPTS_VALUE || (reachedCue && ((packet->flags & AV_PKT_FLAG_KEY)
                || av_compare_ts(timestamp - key, stream->time_base, 500000, AV_TIME_BASE_Q) > 0))) {
                demuxer.setProperty("audioPrerollKey", {});
                return result;
            }
        } else {
            return result;
        }
        av_packet_unref(packet);
    }
}
