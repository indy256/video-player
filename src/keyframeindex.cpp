#include "keyframeindex.h"
#include <QLibrary>
#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <memory>
extern "C" {
#include <libavformat/avformat.h>
}

namespace {
void loadLibrary(QLibrary &library, const char *name, int major) {
#ifdef Q_OS_WIN
    library.setFileName(QString("%1-%2").arg(name).arg(major));
#else
    library.setFileNameAndVersion(QString::fromLatin1(name), major);
#endif
    library.load();
}

QList<qint64> readKeyframes(const QUrl &source) {
    QLibrary format, codec;
    loadLibrary(format, "avformat", LIBAVFORMAT_VERSION_MAJOR);
    loadLibrary(codec, "avcodec", LIBAVCODEC_VERSION_MAJOR);
#define RESOLVE(library, name) const auto name = reinterpret_cast<decltype(&::name)>(library.resolve(#name)); if (!name) return {}
    RESOLVE(format, avformat_version);
    RESOLVE(format, avformat_alloc_context);
    RESOLVE(format, avformat_open_input);
    RESOLVE(format, avformat_find_stream_info);
    RESOLVE(format, av_find_best_stream);
    RESOLVE(format, av_read_frame);
    RESOLVE(format, avformat_close_input);
    RESOLVE(codec, av_packet_alloc);
    RESOLVE(codec, av_packet_unref);
    RESOLVE(codec, av_packet_free);
#undef RESOLVE
    if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR) return {};
    AVFormatContext *context = avformat_alloc_context();
    if (!context) return {};
    const auto closeInput = qScopeGuard([&] { avformat_close_input(&context); });
    context->interrupt_callback = {[](void *) -> int {
        return QThread::currentThread()->isInterruptionRequested();
    }, nullptr};
    const auto path = source.toLocalFile().toUtf8();
    if (avformat_open_input(&context, path.constData(), nullptr, nullptr) < 0) return {};
    if (avformat_find_stream_info(context, nullptr) < 0) return {};
    const int video = av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0) return {};
    const auto *stream = context->streams[video];
    const double milliseconds = 1000.0 * stream->time_base.num / stream->time_base.den;
    const double origin = context->start_time == AV_NOPTS_VALUE ? 0 : context->start_time / 1000.0;
    QList<qint64> result;
    AVPacket *packet = av_packet_alloc();
    if (!packet) return {};
    const auto freePacket = qScopeGuard([&] { av_packet_free(&packet); });
    int status = 0;
    while (!QThread::currentThread()->isInterruptionRequested()
           && (status = av_read_frame(context, packet)) >= 0) {
        if (packet->stream_index == video && (packet->flags & AV_PKT_FLAG_KEY)
            && packet->pts != AV_NOPTS_VALUE) {
            // Round up so Qt doesn't decode the previous GOP for a fractional timestamp.
            result.append(qMax(qint64(0), qint64(std::ceil(packet->pts * milliseconds - origin))));
        }
        av_packet_unref(packet);
    }
    if (QThread::currentThread()->isInterruptionRequested() || status != AVERROR_EOF) return {};
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
    started = false;
}

void KeyframeIndex::load() {
    if (started || !source.isLocalFile()) return;
    started = true;
    const auto request = generation;
    auto result = std::make_shared<QList<qint64>>();
    auto *job = QThread::create([url = source, result] { *result = readKeyframes(url); });
    job->setParent(this);
    connect(job, &QThread::finished, this, [this, request, result] {
        if (request != generation) return;
        positions = *result;
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
