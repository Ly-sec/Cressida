#pragma once

#include "voice/media.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace voice {
  inline constexpr std::array streamResolutions{480, 720, 1080, 1440, 2160, 0};
  inline constexpr std::array streamFrameRates{15, 30, 60};
  enum class ContentPreference { Smoothness, Clarity };
  struct ShareSettings {
    int resolution = 720, frameRate = 30;
    ContentPreference content = ContentPreference::Smoothness;
    bool hardware = true;
    std::vector<std::string> audioSources;
    float audioVolume = 1;
    bool hidePreview = false;
    bool operator==(const ShareSettings&) const = default;
  };
  ShareSettings sanitizeShareSettings(ShareSettings settings);
  struct VideoQuality {
    int height = 720, frameRate = 30, bitrate = 3000000;
    bool operator==(const VideoQuality&) const = default;
  };
  class VideoAdaptation {
  public:
    using Clock = std::chrono::steady_clock;
    void configure(ShareSettings settings);
    void feedback(float loss, std::chrono::milliseconds delay, bool overloaded, Clock::time_point now);
    bool networkConstraints(std::uint64_t bitrate, std::uint64_t pixels, Clock::time_point now);
    VideoQuality quality(int sourceWidth, int sourceHeight) const;

  private:
    ShareSettings m_settings;
    unsigned m_pressure = 0, m_healthy = 0;
    Clock::time_point m_changed{};
    Clock::time_point m_networkChanged{};
    std::uint64_t m_networkBitrate = 0, m_networkPixels = 0;
    bool m_networkSet = false;
  };
  struct VideoCodecSupport {
    MediaCodec codec;
    bool encode = false, decode = false, hardware = false;
  };
  std::vector<VideoCodecSupport> videoCodecs();
  class VideoEncoder {
  public:
    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;
    std::vector<EncodedVideo> encode(
        const VideoFrame& frame, MediaCodec codec, VideoQuality quality, bool hardware, bool keyframe,
        ContentPreference content, std::string& error
    );
    void reset();
    bool accelerated() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };
  class VideoDecoder {
  public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    std::shared_ptr<const VideoFrame>
    decode(const EncodedVideo& packet, std::string& error, bool hardware = true, int maximumHeight = 0);
    void reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };
  struct VideoStatistics {
    VideoQuality quality;
    std::uint64_t encoded = 0, decoded = 0, dropped = 0, frameRevision = 0;
    bool hardware = false;
    std::string error;
  };
  struct VideoSnapshot {
    std::shared_ptr<const VideoFrame> frame;
    VideoStatistics statistics;
  };
  // Real-time codec work is isolated from transport, capture callbacks and drawing.
  class VideoPipeline {
  public:
    using Changed = std::function<void()>;
    using Captured = std::function<void(EncodedVideo)>;
    explicit VideoPipeline(Changed changed = {});
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;
    void configure(ShareSettings settings);
    void codec(MediaCodec codec);
    void capture(Captured callback);
    void sending(bool enabled);
    void source(std::shared_ptr<const VideoFrame> frame);
    void receive(EncodedVideo packet);
    void keyframe();
    bool needsKeyframe();
    void feedback(float loss, std::chrono::milliseconds delay);
    // A zero limit leaves the corresponding source/user limit in effect.
    bool networkConstraints(std::uint64_t bitrate, std::uint64_t pixels);
    void receiveHeight(int height);
    void clear();
    std::shared_ptr<const VideoFrame> frame() const;
    VideoStatistics statistics() const;
    VideoSnapshot snapshot() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };
} // namespace voice
