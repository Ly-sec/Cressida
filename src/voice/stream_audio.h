#pragma once

#include "voice/audio.h"

#include <chrono>

namespace voice {
  // Stream audio never opens a microphone and does not use the microphone's PTT gate.
  class StreamAudio : public Audio {
  public:
    virtual void sources(std::vector<std::string> applications, float volume) = 0;
    virtual void receiveAt(
        std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, std::span<const std::uint8_t> opus,
        std::chrono::steady_clock::time_point due
    ) = 0;
  };
  std::shared_ptr<StreamAudio> makeStreamAudio(bool publishing);
} // namespace voice
