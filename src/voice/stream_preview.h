#pragma once

#include "voice/media.h"

namespace voice {
  inline constexpr int streamPreviewWidth = 512, streamPreviewHeight = 288;
  inline constexpr std::size_t maximumPreviewImageBytes = 256 * 1024;
  // Called on a worker only, after the selected source has been approved for publication.
  std::vector<std::uint8_t> encodeStreamPreview(const VideoFrame& frame, std::string& error);
} // namespace voice
