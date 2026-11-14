#include "keyframeindex.h"
#include <QElapsedTimer>
#include <QFile>
#include <QLibrary>
#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

namespace {
constexpr qint64 maxIndexReadBytes = 8 * 1024 * 1024;

struct IndexInput {
    QFile file;
    qint64 &bytesRead;
    QElapsedTimer timer;
    bool aborted = false;

    bool cancelled() const {
        return aborted || QThread::currentThread()->isInterruptionRequested() || timer.hasExpired(2000);
    }
    static int read(void *opaque, uint8_t *buffer, int size) {
        auto &input = *static_cast<IndexInput *>(opaque);
        if (input.cancelled()) return AVERROR_EXIT;
        const qint64 remaining = maxIndexReadBytes - input.bytesRead;
        if (remaining <= 0) {
            input.aborted = true;
            return AVERROR_EXIT;
        }
        const auto count = input.file.read(reinterpret_cast<char *>(buffer), qMin(qint64(size), remaining));
        if (count < 0) return AVERROR(EIO);
        input.bytesRead += count;
        return count ? int(count) : AVERROR_EOF;
    }
    static int64_t seek(void *opaque, int64_t offset, int whence) {
        auto &input = *static_cast<IndexInput *>(opaque);
        if (input.cancelled()) return AVERROR_EXIT;
        whence &= ~AVSEEK_FORCE;
        if (whence == AVSEEK_SIZE) return input.file.size();
        qint64 base;
        switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = input.file.pos(); break;
        case SEEK_END: base = input.file.size(); break;
        default: return AVERROR(EINVAL);
        }
        if (base < 0) return AVERROR(EIO);
        if (offset < -base || offset > std::numeric_limits<qint64>::max() - base)
            return AVERROR(EINVAL);
        const qint64 target = base + offset;
        return input.file.seek(target) ? target : AVERROR(EIO);
    }
};

void setLibraryName(QLibrary &library, const char *name, int major) {
#ifdef Q_OS_WIN
    library.setFileName(QString("%1-%2").arg(name).arg(major));
#else
    library.setFileNameAndVersion(QString::fromLatin1(name), major);
#endif
}

QList<qint64> readKeyframes(const QUrl &source, qint64 &bytesRead) {
    QLibrary format, codec, util;
    setLibraryName(format, "avformat", LIBAVFORMAT_VERSION_MAJOR);
    setLibraryName(codec, "avcodec", LIBAVCODEC_VERSION_MAJOR);
    setLibraryName(util, "avutil", LIBAVUTIL_VERSION_MAJOR);
#define RESOLVE(library, name) \
    const auto name = reinterpret_cast<decltype(&::name)>(library.resolve(#name)); \
    if (!name) return {}
    RESOLVE(format, avformat_version);
    RESOLVE(format, avformat_alloc_context);
    RESOLVE(format, avformat_open_input);
    RESOLVE(format, avformat_find_stream_info);
    RESOLVE(format, av_find_best_stream);
    RESOLVE(format, av_seek_frame);
    RESOLVE(format, avformat_index_get_entries_count);
    RESOLVE(format, avformat_index_get_entry);
    RESOLVE(format, av_read_frame);
    RESOLVE(format, avformat_close_input);
    RESOLVE(format, avio_alloc_context);
    RESOLVE(format, avio_context_free);
    RESOLVE(codec, av_packet_alloc);
    RESOLVE(codec, av_packet_unref);
    RESOLVE(codec, av_packet_free);
    RESOLVE(util, av_malloc);
    RESOLVE(util, av_free);
    RESOLVE(util, av_strdup);
#undef RESOLVE
    if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR) return {};
    IndexInput input{QFile(source.toLocalFile()), bytesRead};
    input.timer.start();
    if (!input.file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) return {};
    constexpr int bufferSize = 32768;
    auto *buffer = static_cast<unsigned char *>(av_malloc(bufferSize));
    if (!buffer) return {};
    AVIOContext *io = avio_alloc_context(buffer, bufferSize, 0, &input, &IndexInput::read,
                                       nullptr, &IndexInput::seek);
    if (!io) { av_free(buffer); return {}; }
    const auto closeIo = qScopeGuard([&] {
        av_free(io->buffer); // FFmpeg may replace the original buffer.
        avio_context_free(&io);
    });
    AVFormatContext *context = avformat_alloc_context();
    if (!context) return {};
    const auto closeInput = qScopeGuard([&] { avformat_close_input(&context); });
    context->interrupt_callback = {[](void *opaque) -> int {
        return static_cast<IndexInput *>(opaque)->cancelled();
    }, &input};
    context->pb = io;
    context->flags |= AVFMT_FLAG_CUSTOM_IO;
    // Other formats often build only partial indexes while reading packets.
    // Restrict this optional feature to containers with persistent seek tables.
    context->format_whitelist = av_strdup("matroska,webm,mov,mp4,m4a,3gp,3g2,mj2,avi");
    if (!context->format_whitelist) return {};
    const auto path = source.toLocalFile().toUtf8();
    if (avformat_open_input(&context, path.constData(), nullptr, nullptr) < 0) return {};
    const int video = av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0) return {};
    auto *stream = context->streams[video];
    if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) return {};
    if (stream->time_base.num <= 0 || stream->time_base.den <= 0) return {};
    const QByteArray container(context->iformat->name);
    const bool matroska = container.contains("matroska");
    const bool avi = container == "avi";
    // Load deferred Matroska Cues before probing packets can build a partial
    // index. A timestamp preceding the stream lets native seeking clamp to its
    // first cue, but makes generic seeking fail when there is no stored index.
    if (matroska || avi) {
        const int status = av_seek_frame(context, video, std::numeric_limits<int64_t>::min() + 1,
                                        AVSEEK_FLAG_BACKWARD);
        if (matroska && status < 0) return {};
        // AVI also loads its table on demand. Its native seek does not clamp
        // before the first entry, so inspect the loaded table even on failure.
        if (avi && avformat_index_get_entries_count(stream) < 2) return {};
    }
    if (input.cancelled()) return {};
    QList<AVIndexEntry> entries;
    const int count = avformat_index_get_entries_count(stream);
    for (int i = 0; i < count; ++i) {
        if (input.cancelled()) return {};
        const auto *entry = avformat_index_get_entry(stream, i);
        if (entry && (entry->flags & AVINDEX_KEYFRAME) && entry->timestamp != AV_NOPTS_VALUE)
            entries.append(*entry);
    }
    if (entries.isEmpty()) return {};
    // Establish the same presentation-time origin used by normal playback.
    if (avformat_find_stream_info(context, nullptr) < 0 || input.cancelled()) return {};
    const double milliseconds = 1000.0 * stream->time_base.num / stream->time_base.den;
    const double origin = context->start_time == AV_NOPTS_VALUE ? 0 : context->start_time / 1000.0;
    QList<qint64> result;
    AVPacket *packet = av_packet_alloc();
    if (!packet) return {};
    const auto freePacket = qScopeGuard([&] { av_packet_free(&packet); });
    for (const auto &entry : entries) {
        if (input.cancelled()) return {};
        int64_t timestamp = entry.timestamp;
        if (!matroska) {
            // MOV/AVI indexes can contain decode timestamps. Read only the
            // indexed key packet to preserve presentation timestamps with B-frames.
            if (av_seek_frame(context, video, entry.timestamp, AVSEEK_FLAG_BACKWARD) < 0) return {};
            // AVI entries point at the chunk header, packets at its payload.
            if (avi && (entry.pos < 0 || entry.pos > input.file.size() - 8)) return {};
            const int64_t packetPosition = entry.pos + (avi ? 8 : 0);
            for (;;) {
                av_packet_unref(packet);
                if (input.cancelled() || av_read_frame(context, packet) < 0) return {};
                if (packet->stream_index != video) continue;
                if (packet->pos > packetPosition) return {};
                if (packet->pos == packetPosition && (packet->flags & AV_PKT_FLAG_KEY)
                    && packet->pts != AV_NOPTS_VALUE) break;
            }
            timestamp = packet->pts;
        }
        // Round up so Qt doesn't decode the previous GOP for a fractional timestamp.
        const double position = qMax(0.0, std::ceil(timestamp * milliseconds - origin));
        if (!std::isfinite(position) || position >= double(std::numeric_limits<qint64>::max())) return {};
        result.append(qint64(position));
    }
    if (input.cancelled()) return {};
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
}

KeyframeIndex::~KeyframeIndex() {
    const auto jobs = findChildren<QThread *>();
    for (auto *job : jobs) job->requestInterruption();
    for (auto *job : jobs) job->wait();
}

void KeyframeIndex::setSource(const QUrl &value) {
    ++generation;
    for (auto *job : findChildren<QThread *>()) job->requestInterruption();
    source = value;
    positions.clear();
    readBytes = 0;
    started = false;
}

void KeyframeIndex::load() {
    if (started || !source.isLocalFile()) return;
    started = true;
    const auto request = generation;
    struct Result { QList<qint64> positions; qint64 bytesRead = 0; };
    auto result = std::make_shared<Result>();
    auto *job = QThread::create([url = source, result] {
        result->positions = readKeyframes(url, result->bytesRead);
    });
    job->setParent(this);
    connect(job, &QThread::finished, this, [this, request, result] {
        if (request != generation) return;
        positions = result->positions;
        readBytes = result->bytesRead;
        emit finished(isReady());
    });
    connect(job, &QThread::finished, job, &QObject::deleteLater);
    job->start(QThread::LowPriority);
}

qint64 KeyframeIndex::seekPosition(qint64 requested, bool forward) const {
    if (positions.isEmpty()) return requested;
    if (forward) {
        const auto next = std::lower_bound(positions.cbegin(), positions.cend(), requested);
        return next == positions.cend() ? requested : *next;
    }
    const auto next = std::upper_bound(positions.cbegin(), positions.cend(), requested);
    return next == positions.cbegin() ? positions.first() : *std::prev(next);
}
