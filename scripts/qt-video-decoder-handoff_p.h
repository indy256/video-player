#ifndef QFFMPEGVIDEODECODERHANDOFF_P_H
#define QFFMPEGVIDEODECODERHANDOFF_P_H

#include <QtFFmpegMediaPluginImpl/private/qffmpegcodeccontext_p.h>
#include <QtFFmpegMediaPluginImpl/private/qffmpegpacket_p.h>
#include <QtCore/qthreadpool.h>
#include <QtCore/qvariant.h>
#include <QtMultimedia/qvideosink.h>

#include "qffmpegcodecstorage_p.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>

QT_BEGIN_NAMESPACE
namespace QFFmpeg {

class VideoDecoderHandoff
{
public:
    static constexpr char softwareDecodingProperty[] = "videoPlayerSoftwareDecoding";

    static bool softwareDecodingRequested(const QVideoSink *sink)
    {
        return sink && sink->property(softwareDecodingProperty).toBool();
    }

    static bool prefersSoftwareVideoDecoder()
    {
        return !qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES");
    }

    static std::optional<Codec> findSoftwareDecoder(AVCodecID id)
    {
        if (id != AV_CODEC_ID_AV1)
            return findAVDecoder(id);

        // FFmpeg 7.1's native AV1 decoder requires a hardware context.
        const auto *decoder = avcodec_find_decoder_by_name("libdav1d");
        if (!decoder)
            decoder = avcodec_find_decoder_by_name("libaom-av1");
        return decoder ? std::optional<Codec>{ Codec(decoder) } : std::nullopt;
    }

    // Qt 6.11 passes playback options, while Qt 6.8 opens the decoder without them.
    template <typename... Options>
    static std::unique_ptr<VideoDecoderHandoff> create(const CodecContext &codec,
                                                      const Options &...options)
    {
        if (codec.context()->codec_type != AVMEDIA_TYPE_VIDEO || codec.hwAccel() || codec.d->softwareOnly
            || !prefersSoftwareVideoDecoder())
            return {};

        auto openHardware = [=](const AVCodecParameters *parameters)
                -> std::optional<CodecContext::OpenedDecoder> {
            auto decoder = CodecContext::openDecoder(parameters, options..., CodecContext::Hw);
            if (!decoder)
                return {};
            return std::move(*decoder);
        };
        return std::make_unique<VideoDecoderHandoff>(codec.stream()->codecpar,
                                                     std::move(openHardware));
    }

    using OpenHardware = std::function<std::optional<CodecContext::OpenedDecoder>(
            const AVCodecParameters *)>;
    VideoDecoderHandoff(const AVCodecParameters *parameters, OpenHardware openHardware)
        : m_parameters(parameters), m_openHardware(std::move(openHardware))
    {
    }

    ~VideoDecoderHandoff();
    void reset();
    void softwareFrameQueued(const AVFrame &frame);
    void enqueue(const Packet &packet);
    std::optional<qint64> switchIfReady(const Packet &nextPacket, const LoopOffset &offset,
                                       CodecContext &codec, std::deque<AVFrameUPtr> &queuedFrames);

private:
    void cancel();
    static qint64 timestamp(const AVFrame &frame)
    {
        return frame.pts == AV_NOPTS_VALUE ? frame.best_effort_timestamp : frame.pts;
    }

    struct State {
        enum class Status { Running, Idle, Cancelled };
        void process();
        void cancel();
        bool expired() const { return std::chrono::steady_clock::now() > deadline; }

        std::mutex mutex;
        CodecContext::OpenedDecoder decoder;
        std::deque<Packet> packets;
        std::deque<AVFrameUPtr> frames;
        Status status = Status::Running;
        bool startedAtKeyframe = false;
        const std::chrono::steady_clock::time_point deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(8);
    };

    const AVCodecParameters *const m_parameters;
    const OpenHardware m_openHardware;
    std::shared_ptr<State> m_state;
    qint64 m_softwarePts = AV_NOPTS_VALUE;
    bool m_attempted = false;
    // Declared last so teardown joins the worker before destroying the handoff state.
    std::unique_ptr<QThreadPool> m_pool;
};

inline void VideoDecoderHandoff::softwareFrameQueued(const AVFrame &frame)
{
    m_softwarePts = timestamp(frame);
    if (m_attempted)
        return;
    m_attempted = true;

    // The worker can outlive a seek or file change. Give it its own codec parameters.
    const auto freeParameters = [](AVCodecParameters *parameters) {
        avcodec_parameters_free(&parameters);
    };
    std::unique_ptr<AVCodecParameters, decltype(freeParameters)> snapshot(
            avcodec_parameters_alloc(), freeParameters);
    if (!snapshot || avcodec_parameters_copy(snapshot.get(), m_parameters) < 0)
        return;

    if (!m_pool) {
        m_pool = std::make_unique<QThreadPool>();
        m_pool->setMaxThreadCount(1);
    }
    m_state = std::make_shared<State>();

    m_pool->start([state = m_state, snapshot = std::move(snapshot), openHardware = m_openHardware] {
        {
            std::lock_guard lock(state->mutex);
            if (state->status == State::Status::Cancelled)
                return;
        }
        auto result = openHardware(snapshot.get());
        if (!result) {
            state->cancel();
            return;
        }
        // Only the worker accesses the decoder until the state becomes idle.
        state->decoder = std::move(*result);
        state->process();
    });
}

inline VideoDecoderHandoff::~VideoDecoderHandoff()
{
    cancel();
}

inline void VideoDecoderHandoff::reset()
{
    cancel();
    m_attempted = false;
}

inline void VideoDecoderHandoff::cancel()
{
    if (m_state) {
        m_state->cancel();
        m_state.reset();
    }
}

inline void VideoDecoderHandoff::State::cancel()
{
    std::lock_guard lock(mutex);
    status = Status::Cancelled;
    packets.clear();
    frames.clear();
}

inline void VideoDecoderHandoff::enqueue(const Packet &packet)
{
    if (!m_state)
        return;
    std::unique_lock lock(m_state->mutex);
    if (!packet.isValid() || m_state->status == State::Status::Cancelled || m_state->expired()
        || m_state->packets.size() >= 96) {
        lock.unlock();
        cancel();
        return;
    }
    if (!m_state->startedAtKeyframe) {
        if (!(packet.avPacket()->flags & AV_PKT_FLAG_KEY))
            return;
        m_state->startedAtKeyframe = true;
    }
    m_state->packets.push_back(packet);
    if (m_state->status == State::Status::Idle) {
        m_state->status = State::Status::Running;
        m_pool->start([state = m_state] { state->process(); });
    }
}

inline void VideoDecoderHandoff::State::process()
{
    // Only one worker owns the decoder until the queue drains and the stream takes it.
    auto *context = decoder.context.get();
    for (;;) {
        Packet packet;
        {
            std::lock_guard lock(mutex);
            if (status == Status::Cancelled)
                return;
            if (packets.empty()) {
                status = Status::Idle;
                return;
            }
            packet = std::move(packets.front());
            packets.pop_front();
        }
        if (avcodec_send_packet(context, packet.avPacket()) < 0) {
            cancel();
            return;
        }
        for (;;) {
            auto frame = makeAVFrame();
            const int result = avcodec_receive_frame(context, frame.get());
            if (result == AVERROR(EAGAIN))
                break;
            if (result < 0 || !isHwPixelFormat(AVPixelFormat(frame->format))) {
                cancel();
                return;
            }
            frame = copyFromHwPool(std::move(frame));
            // Keep the warmup queue near 96 MiB even for high-resolution video.
            const auto pixels = std::max<qint64>(1, qint64(frame->width) * frame->height);
            const auto frameLimit = size_t(std::clamp<qint64>(24 * 1024 * 1024 / pixels, 1, 24));
            std::lock_guard lock(mutex);
            if (status == Status::Cancelled)
                return;
            while (frames.size() >= frameLimit)
                frames.pop_front();
            frames.push_back(std::move(frame));
        }
    }
}

inline std::optional<qint64> VideoDecoderHandoff::switchIfReady(
        const Packet &nextPacket, const LoopOffset &offset, CodecContext &codec,
        std::deque<AVFrameUPtr> &queuedFrames)
{
    if (!m_state || m_softwarePts == AV_NOPTS_VALUE || !nextPacket.isValid()
        || nextPacket.loopOffset().loopIndex != offset.loopIndex)
        return {};
    std::lock_guard lock(m_state->mutex);
    if (m_state->status != State::Status::Idle || m_state->expired())
        return {};
    auto &frames = m_state->frames;
    while (!frames.empty() && timestamp(*frames.front()) < m_softwarePts)
        frames.pop_front();
    if (frames.empty() || timestamp(*frames.front()) != m_softwarePts)
        return {};
    // The matching software frame is already queued for display.
    frames.pop_front();
    auto &decoder = m_state->decoder;
    avcodec_flush_buffers(codec.context());
    codec = CodecContext(new CodecContext::Data(std::move(decoder.context), codec.stream(),
                                                codec.d->formatContext, std::move(decoder.hwAccel)));
    queuedFrames = std::move(frames);
    return m_softwarePts;
}

} // namespace QFFmpeg
QT_END_NAMESPACE

#endif // QFFMPEGVIDEODECODERHANDOFF_P_H
