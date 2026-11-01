#include "media/pixel_conversion.h"

extern "C" {
#include <libavutil/log.h>
}

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
  int deprecatedWarnings = 0;
  void logMessage(void* context, int level, const char* format, va_list arguments) {
    va_list copy;
    va_copy(copy, arguments);
    std::array<char, 1024> text{};
    std::vsnprintf(text.data(), text.size(), format, copy);
    va_end(copy);
    if (std::strstr(text.data(), "deprecated pixel format"))
      ++deprecatedWarnings;
    av_log_default_callback(context, level, format, arguments);
  }
  void require(bool value, const char* reason) {
    if (!value)
      throw std::runtime_error(reason);
  }
} // namespace
int main() {
  try {
    av_log_set_callback(logMessage);
    constexpr int edge = 32, channels = 4;
    for (const auto format :
         {AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUVJ411P,
          AV_PIX_FMT_YUV420P}) {
      AVFrame* frame = av_frame_alloc();
      require(frame, "Allocate full-range frame");
      frame->format = format;
      frame->width = frame->height = edge;
      require(av_frame_get_buffer(frame, 0) >= 0, "Allocate padded source planes");
      for (int plane = 0; plane < 3; ++plane)
        std::memset(
            frame->data[plane], plane == 0 ? 16 : 128,
            frame->linesize[plane]
                * edge
                / (plane > 0
                           && (format == AV_PIX_FMT_YUVJ420P
                               || format == AV_PIX_FMT_YUVJ440P
                               || format == AV_PIX_FMT_YUV420P)
                       ? 2
                       : 1)
        );
      SwsContext* scale = nullptr;
      for (const auto range : {AVCOL_RANGE_JPEG, AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG}) {
        frame->color_range = range;
        scale = sws_getCachedContext(
            scale, edge, edge, media::scalingFormat(format), edge, edge, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
            nullptr, nullptr
        );
        require(media::rgbaColorspace(scale, *frame), "Configure source range on each cached conversion");
        std::array<unsigned char, edge * edge * channels> pixels{};
        unsigned char* destination[] = {pixels.data()};
        const int strides[] = {edge * channels};
        require(
            sws_scale(scale, frame->data, frame->linesize, 0, edge, destination, strides) == edge,
            "Convert full-range planes"
        );
        const int expected = format == AV_PIX_FMT_YUV420P && range == AVCOL_RANGE_MPEG ? 0 : 16;
        for (std::size_t i = 0; i < pixels.size(); i += channels)
          require(
              std::abs(int(pixels[i]) - expected) <= 1 && pixels[i + 3] == 255,
              "Preserve full-range near-black values and range changes on a reused scaler"
          );
      }
      sws_freeContext(scale);
      av_frame_free(&frame);
    }
    require(deprecatedWarnings == 0, "Normalized formats do not produce deprecated-format warnings");
    av_log_set_callback(av_log_default_callback);
    std::cout << "Full-range pixel conversion and cached range changes passed\n";
  } catch (const std::exception& error) {
    av_log_set_callback(av_log_default_callback);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
