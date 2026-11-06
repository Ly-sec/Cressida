#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace voice {
  inline constexpr int messageSampleRate = 48000;
  inline constexpr std::size_t messageMaximumSamples = messageSampleRate * 20ULL * 60;
  inline constexpr std::size_t messageMaximumBytes = 20ULL * 1024 * 1024;
  inline constexpr std::size_t messageMaximumWaveformBytes = 256;

  struct RecordedMessage {
    std::vector<std::uint8_t> ogg;
    std::vector<std::uint8_t> waveform;
    double durationSeconds = 0;
  };

  enum class MessageRecorderPhase { Idle, Loading, Recording, Ready, Error };

  struct MessageRecorderState {
    MessageRecorderPhase phase = MessageRecorderPhase::Idle;
    std::uint64_t revision = 0;
    double durationSeconds = 0;
    std::shared_ptr<const RecordedMessage> recording;
    std::string error;
  };

  // Injection seam for account-free tests. Methods run only on the recorder worker.
  // open/read must return on wakeFd readiness and enforce timeoutMilliseconds.
  // read returns a sample count, zero on interruption/timeout, or -1 on failure.
  class MessageCapture {
  public:
    virtual ~MessageCapture() = default;
    virtual bool open(const std::string& device, int wakeFd, int timeoutMilliseconds, std::string& error) = 0;
    virtual int read(std::span<float> samples, int wakeFd, int timeoutMilliseconds, std::string& error) = 0;
  };

  class MessageRecorder {
  public:
    using CaptureFactory = std::function<std::unique_ptr<MessageCapture>()>;
    // Construction starts only an idle worker, never a connection or device.
    explicit MessageRecorder(CaptureFactory captureFactory = {});
    ~MessageRecorder();
    MessageRecorder(const MessageRecorder&) = delete;
    MessageRecorder& operator=(const MessageRecorder&) = delete;

    // Volume is a linear software gain in [0, 2]. Start replaces any earlier take.
    void start(std::string inputDevice = {}, float inputVolume = 1);
    void stop();
    void cancel();
    [[nodiscard]] MessageRecorderState state() const;
    [[nodiscard]] MessageRecorderState drain();
    [[nodiscard]] int eventFd() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
  };
} // namespace voice
