#pragma once

#include "voice/audio.h"

#include <array>
#include <memory>

namespace voice {
  using AudioFrame = std::array<float, frameSamples>;
  class AudioProcessor {
  public:
    AudioProcessor();
    ~AudioProcessor();
    AudioProcessor(const AudioProcessor&) = delete;
    AudioProcessor& operator=(const AudioProcessor&) = delete;
    void configure(AudioSettings settings);
    EncodedAudio capture(std::span<const float> samples);
    void
    receive(std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, std::span<const std::uint8_t> opus);
    AudioFrame playback();
    void remove(std::uint32_t ssrc);
    void reset();
    float inputLevel() const;
    float outputLevel() const;
    std::size_t speakers() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };
} // namespace voice
