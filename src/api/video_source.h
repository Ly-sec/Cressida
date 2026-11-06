#pragma once
#include "media/video_player.h"

#include <string_view>
namespace api {
  enum class MediaSourceKind { Video, OggAudio };
  bool mediaSourceHeader(std::span<const char> bytes, MediaSourceKind kind);
  // Fixed public media origins only; no Authorization, redirects or disk cache.
  std::shared_ptr<media::VideoInput> videoSource(std::string_view url, MediaSourceKind kind = MediaSourceKind::Video);
} // namespace api
