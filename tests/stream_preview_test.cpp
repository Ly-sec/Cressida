#include "voice/stream_preview.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
} // namespace
int main() {
  try {
    for (const auto& [width, height] :
         {std::pair{1920, 1080}, std::pair{1080, 1920}, std::pair{320, 180}, std::pair{2, 2}}) {
      voice::VideoFrame image{.width = width, .height = height, .stride = width * 4};
      image.rgba.resize(image.stride * height);
      for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = 200;
        image.rgba[i + 1] = 60;
        image.rgba[i + 2] = 20;
        image.rgba[i + 3] = 255;
      }
      std::string error;
      const auto jpeg = voice::encodeStreamPreview(image, error);
      require(
          !jpeg.empty() && error.empty() && jpeg.size() <= voice::maximumPreviewImageBytes,
          "A bounded JPEG is encoded from valid captured pixels"
      );
      auto* context = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_MJPEG));
      auto* frame = av_frame_alloc();
      auto* packet = av_packet_alloc();
      require(
          context && frame && packet && avcodec_open2(context, context->codec, nullptr) >= 0,
          "The actual JPEG decoder opens"
      );
      require(av_new_packet(packet, jpeg.size()) == 0, "A real encoded thumbnail fits the decoder packet");
      std::copy(jpeg.begin(), jpeg.end(), packet->data);
      require(
          avcodec_send_packet(context, packet) >= 0 && avcodec_receive_frame(context, frame) >= 0,
          "The thumbnail is a decodable JPEG"
      );
      require(
          frame->width <= voice::streamPreviewWidth
              && frame->height <= voice::streamPreviewHeight
              && frame->width <= width
              && frame->height <= height,
          "Thumbnails retain source bounds without upscaling"
      );
      require(
          std::abs(static_cast<double>(frame->width) / frame->height - static_cast<double>(width) / height) < .02,
          "Preview scaling preserves aspect ratio"
      );
      avcodec_free_context(&context);
      av_frame_free(&frame);
      av_packet_free(&packet);
    }
    std::string error;
    require(
        voice::encodeStreamPreview({}, error).empty() && !error.empty(), "Invalid capture data never produces an upload"
    );
    std::cout << "Bounded native JPEG stream previews passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
