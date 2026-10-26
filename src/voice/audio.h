#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace voice {
  inline constexpr int sampleRate = 48000;
  inline constexpr int frameSamples = 960;
  inline constexpr float maximumAudioVolume = 2.0F;
  inline constexpr float defaultActivityThreshold = 0.02F;

  struct AudioSettings {
    std::string inputDevice, outputDevice;
    float inputVolume = 1.0F, outputVolume = 1.0F;
    bool muted = false, deafened = false;
    bool pushToTalk = false;
    float threshold = defaultActivityThreshold;
    bool pttPressed = false;
    bool operator==(const AudioSettings&) const = default;
  };
  AudioSettings sanitizeAudioSettings(AudioSettings settings);
  struct AudioDevice {
    std::string id, name;
    bool operator==(const AudioDevice&) const = default;
  };
  struct AudioState {
    std::vector<AudioDevice> inputs, outputs;
    float inputLevel = 0, outputLevel = 0;
    bool running = false, testing = false, loading = false, speaking = false;
    std::string error;
    std::uint64_t revision = 0;
  };
  struct EncodedAudio {
    std::vector<std::uint8_t> opus;
    std::uint32_t timestamp = 0;
    bool speaking = false;
  };
  class Audio {
  public:
    using Capture = std::function<void(EncodedAudio)>;
    virtual ~Audio() = default;
    virtual bool start(Capture capture, std::string& error) = 0;
    virtual void stop() = 0;
    virtual void configure(AudioSettings settings) = 0;
    virtual void receive(
        std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, std::span<const std::uint8_t> opus
    ) = 0;
    virtual void remove(std::uint32_t ssrc) = 0;
    virtual AudioState state() const = 0;
    virtual void refreshDevices() = 0;
    virtual bool startTest(std::string& error) = 0;
    virtual void stopTest() = 0;
    virtual int eventFd() const noexcept = 0;
    virtual void drainEvents() = 0;
  };
  std::shared_ptr<Audio> makeAudio();
} // namespace voice
