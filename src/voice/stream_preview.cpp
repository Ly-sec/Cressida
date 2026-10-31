#include "voice/stream_preview.h"

#include <algorithm>
#include <memory>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace voice {
  std::vector<std::uint8_t> encodeStreamPreview(const VideoFrame& source, std::string& error) {
    error.clear();
    if (!source.valid() || source.width < 2 || source.height < 2) {
      error = "The stream preview source is invalid.";
      return {};
    }
    const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    auto* context = codec ? avcodec_alloc_context3(codec) : nullptr;
    auto* frame = av_frame_alloc();
    auto* packet = av_packet_alloc();
    struct Cleanup {
      AVCodecContext*& context;
      AVFrame*& frame;
      AVPacket*& packet;
      ~Cleanup() {
        avcodec_free_context(&context);
        av_frame_free(&frame);
        av_packet_free(&packet);
      }
    } cleanup{context, frame, packet};
    const auto failure = [&] {
      error = "The stream preview could not be encoded.";
      return std::vector<std::uint8_t>{};
    };
    if (!context || !frame || !packet)
      return failure();
    const double scale = std::min(
        {1.0, static_cast<double>(streamPreviewWidth) / source.width,
         static_cast<double>(streamPreviewHeight) / source.height}
    );
    constexpr int evenDimensionMask = ~1, previewQuantizer = 8, frameAlignment = 32;
    context->width = std::max(2, static_cast<int>(source.width * scale) & evenDimensionMask);
    context->height = std::max(2, static_cast<int>(source.height * scale) & evenDimensionMask);
    context->pix_fmt = AV_PIX_FMT_YUVJ420P;
    context->color_range = AVCOL_RANGE_JPEG;
    context->time_base = {1, 1};
    context->flags |= AV_CODEC_FLAG_QSCALE;
    context->global_quality = previewQuantizer * FF_QP2LAMBDA;
    context->thread_count = 1;
    if (avcodec_open2(context, codec, nullptr) < 0)
      return failure();
    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;
    frame->color_range = context->color_range;
    frame->quality = context->global_quality;
    if (av_frame_get_buffer(frame, frameAlignment) < 0)
      return failure();
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler(
        sws_getContext(
            source.width, source.height, AV_PIX_FMT_RGBA, context->width, context->height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        ),
        sws_freeContext
    );
    if (!scaler)
      return failure();
    const auto* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
    constexpr int unitColorScale = 1 << 16;
    if (sws_setColorspaceDetails(scaler.get(), coefficients, 1, coefficients, 1, 0, unitColorScale, unitColorScale) < 0)
      return failure();
    const std::uint8_t* planes[]{source.rgba.data()};
    const int strides[]{source.stride};
    if (sws_scale(scaler.get(), planes, strides, 0, source.height, frame->data, frame->linesize) != context->height
        || avcodec_send_frame(context, frame) < 0
        || avcodec_receive_packet(context, packet) < 0
        || packet->size <= 0
        || static_cast<std::size_t>(packet->size) > maximumPreviewImageBytes)
      return failure();
    return {packet->data, packet->data + packet->size};
  }
} // namespace voice
