#pragma once

#include "voice/audio.h"

#include <chrono>
#include <memory>

namespace voice {
  inline constexpr int streamAudioChannels = 2;
  inline constexpr std::size_t maximumStreamAudioPacketBytes = 1275;
  inline constexpr std::size_t maximumStreamAudioPackets = 16;

  // The worker asks for samples only when the output has space, then acknowledges
  // exactly the samples written. Future audio never becomes artificial silence.
  class StreamAudioPlayout {
  public:
    using Clock = std::chrono::steady_clock;
    StreamAudioPlayout();
    ~StreamAudioPlayout();
    void receive(
        std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, std::span<const std::uint8_t> opus,
        Clock::time_point due, Clock::time_point now
    );
    std::span<const float> prepare(Clock::time_point now, std::chrono::microseconds lead);
    void consume(std::size_t samples);
    Clock::time_point deadline() const;
    void remove(std::uint32_t ssrc);
    void reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };
} // namespace voice
