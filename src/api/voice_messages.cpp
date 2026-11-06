#include "api/voice_messages.h"

#include <cmath>
#include <glib.h>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace api {
  bool validVoiceMessageMetadata(const VoiceMessageMetadata& value) {
    return std::isfinite(value.duration)
        && value.duration > 0
        && value.duration <= voiceMessageMaximumSeconds
        && !value.waveform.empty()
        && value.waveform.size() <= voiceMessageMaximumWaveform;
  }
  std::optional<VoiceMessageMetadata> parseVoiceMessageMetadata(const nlohmann::json& attachment) {
    const auto duration = attachment.find("duration_secs"), waveform = attachment.find("waveform");
    if (duration == attachment.end()
        || !duration->is_number()
        || waveform == attachment.end()
        || !waveform->is_string())
      return {};
    const auto& encoded = waveform->get_ref<const std::string&>();
    constexpr auto maximumEncoded = ((voiceMessageMaximumWaveform + 2) / 3) * 4;
    if (encoded.empty() || encoded.size() > maximumEncoded || encoded.size() % 4 != 0)
      return {};
    gsize length = 0;
    auto* bytes = g_base64_decode(encoded.c_str(), &length);
    auto* canonical = g_base64_encode(bytes, length);
    const bool validEncoding = encoded == canonical;
    g_free(canonical);
    VoiceMessageMetadata result{duration->get<double>(), {bytes, bytes + length}};
    g_free(bytes);
    return validEncoding && validVoiceMessageMetadata(result) ? std::optional(std::move(result)) : std::nullopt;
  }
  nlohmann::json voiceMessageMetadataBody(const VoiceMessageMetadata& value) {
    if (!validVoiceMessageMetadata(value))
      throw std::runtime_error("Invalid voice message metadata");
    auto* encoded = g_base64_encode(value.waveform.data(), value.waveform.size());
    nlohmann::json result{{"duration_secs", value.duration}, {"waveform", encoded}};
    g_free(encoded);
    return result;
  }
} // namespace api
