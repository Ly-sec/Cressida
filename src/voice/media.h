#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace voice {
  enum class MediaCodec { Opus, VP8, H264, VP9 };
  inline constexpr std::size_t maximumVideoFrameBytes = 4 * 1024 * 1024;
  inline constexpr std::size_t maximumCapturedFrameBytes = 256 * 1024 * 1024;
  inline constexpr std::size_t maximumVideoPixels = maximumCapturedFrameBytes / 4;
  inline constexpr int videoClockRate = 90000;
  inline constexpr int maximumVideoDimension = 16384;
  inline constexpr int maximumVideoBitrate = 32 * 1000 * 1000;
  inline constexpr int minimumVideoBitrate = 128 * 1000;
  inline constexpr std::size_t maximumDecodedVideoBytes = 256 * 1024 * 1024;
  inline constexpr std::size_t maximumVideoReceivePackets = 4096;
  inline constexpr auto videoAssemblyDeadline = std::chrono::milliseconds{250};
  inline std::string_view codecName(MediaCodec codec) {
    switch (codec) {
    case MediaCodec::Opus:
      return "opus";
    case MediaCodec::VP8:
      return "VP8";
    case MediaCodec::H264:
      return "H264";
    case MediaCodec::VP9:
      return "VP9";
    }
    return {};
  }
  struct VideoFrame {
    int width = 0, height = 0, stride = 0;
    std::int64_t timestamp = 0;
    std::vector<std::uint8_t> rgba;
    bool valid() const {
      return width > 0
          && height > 0
          && width <= maximumVideoDimension
          && height <= maximumVideoDimension
          && stride >= width * 4
          && static_cast<std::size_t>(stride) <= maximumCapturedFrameBytes / static_cast<std::size_t>(height)
          && static_cast<std::size_t>(stride) * height <= rgba.size()
          && rgba.size() <= maximumCapturedFrameBytes;
    }
  };
  struct EncodedVideo {
    MediaCodec codec = MediaCodec::VP8;
    std::vector<std::uint8_t> bytes;
    std::uint32_t timestamp = 0;
    int width = 0, height = 0;
    bool keyframe = false;
    std::uint64_t revision = 0;
    std::int64_t capturedAtUs=0;
    std::chrono::steady_clock::time_point presentAt{};
  };
} // namespace voice
