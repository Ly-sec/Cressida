#pragma once

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace api {
  inline constexpr double voiceMessageMaximumSeconds = 20 * 60;
  inline constexpr std::size_t voiceMessageMaximumWaveform = 256;
  struct VoiceMessageMetadata {
    double duration = 0;
    std::vector<unsigned char> waveform;
    bool operator==(const VoiceMessageMetadata&) const = default;
  };
  bool validVoiceMessageMetadata(const VoiceMessageMetadata& value);
  std::optional<VoiceMessageMetadata> parseVoiceMessageMetadata(const nlohmann::json& attachment);
  nlohmann::json voiceMessageMetadataBody(const VoiceMessageMetadata& value);
} // namespace api
