// Included inside QFFmpeg's D3D11 backend implementation. Scale before the
// decoder-pool copy and cross-device bridge, keeping NV12 frames on the GPU.
namespace {
class EarlyGpuScaler {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    AVBufferUPtr frames;
    QSize inputSize, outputSize;

    bool initialize(const AVHWFramesContext *context, const AVD3D11VADeviceContext *hw,
                    const QSize &input, const QSize &output) {
        if (device.Get() == hw->device && inputSize == input && outputSize == output)
            return bool(frames);
        frames.reset();
        processor.Reset();
        enumerator.Reset();
        videoContext.Reset();
        device = hw->device;
        inputSize = input;
        outputSize = output;
        if (FAILED(device.As(&videoDevice)) || FAILED(hw->device_context->QueryInterface(IID_PPV_ARGS(&videoContext))))
            return false;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = desc.OutputFrameRate = {60, 1};
        desc.InputWidth = inputSize.width(); desc.InputHeight = inputSize.height();
        desc.OutputWidth = output.width(); desc.OutputHeight = output.height();
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(videoDevice->CreateVideoProcessorEnumerator(&desc, &enumerator))) return false;
        UINT support = 0;
        if (FAILED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support))
            || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)
            || FAILED(videoDevice->CreateVideoProcessor(enumerator.Get(), 0, &processor))) return false;
        AVBufferUPtr pool(av_hwframe_ctx_alloc(context->device_ref));
        if (!pool) return false;
        auto *scaled = reinterpret_cast<AVHWFramesContext *>(pool->data);
        scaled->format = AV_PIX_FMT_D3D11;
        scaled->sw_format = AV_PIX_FMT_NV12;
        scaled->width = output.width(); scaled->height = output.height();
        auto *d3d = static_cast<AVD3D11VAFramesContext *>(scaled->hwctx);
        d3d->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (av_hwframe_ctx_init(pool.get()) < 0) return false;
        videoContext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        videoContext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
        RECT src{0, 0, inputSize.width(), inputSize.height()};
        RECT dst{0, 0, output.width(), output.height()};
        videoContext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &src);
        videoContext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &dst);
        videoContext->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &dst);
        frames = std::move(pool);
        qInfo("Early GPU scale: %dx%d -> %dx%d (NV12)", inputSize.width(), inputSize.height(), output.width(), output.height());
        return true;
    }
public:
    AVFrameUPtr scale(AVFrame *source, int maxWidth) {
        const auto *context = reinterpret_cast<AVHWFramesContext *>(source->hw_frames_ctx->data);
        if (context->sw_format != AV_PIX_FMT_NV12 || source->width <= maxWidth || maxWidth < 2
            || (source->flags & AV_FRAME_FLAG_INTERLACED)
            || source->color_trc == AVCOL_TRC_SMPTE2084 || source->color_trc == AVCOL_TRC_ARIB_STD_B67)
            return {};
        maxWidth &= ~1;
        const QSize output(maxWidth, qMax(2, qRound(source->height * double(maxWidth) / source->width / 2) * 2));
        const auto *hw = getHwDeviceContext(context->device_ctx);
        hw->lock(hw->lock_ctx);
        QScopeGuard unlock([&] { hw->unlock(hw->lock_ctx); });
        if (!initialize(context, hw, {source->width, source->height}, output)) return {};
        const auto failed = [this] {
            frames.reset();
            qWarning("Early GPU scaling failed; using original video frames");
            return AVFrameUPtr{};
        };
        auto result = makeAVFrame();
        if (!result || av_hwframe_get_buffer(frames.get(), result.get(), 0) < 0
            || av_frame_copy_props(result.get(), source) < 0) return failed();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in{};
        in.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        in.Texture2D.ArraySlice = getAvFramePoolIndex(source);
        ComPtr<ID3D11VideoProcessorInputView> input;
        if (FAILED(videoDevice->CreateVideoProcessorInputView(getAvFrameTexture(source).Get(), enumerator.Get(), &in, &input)))
            return failed();
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out{};
        out.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> destination;
        if (FAILED(videoDevice->CreateVideoProcessorOutputView(getAvFrameTexture(result.get()).Get(), enumerator.Get(), &out, &destination)))
            return failed();
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE color{};
        color.YCbCr_Matrix = source->colorspace == AVCOL_SPC_BT709;
        color.Nominal_Range = source->color_range == AVCOL_RANGE_JPEG ? 2 : 1;
        videoContext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &color);
        videoContext->VideoProcessorSetOutputColorSpace(processor.Get(), &color);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input.Get();
        if (FAILED(videoContext->VideoProcessorBlt(processor.Get(), destination.Get(), 0, 1, &stream))) return failed();
        // Submit the resize without a CPU wait. Qt's texture bridge synchronizes
        // GPU access when the renderer consumes the frame.
        hw->device_context->Flush();
        return result;
    }
};
}
