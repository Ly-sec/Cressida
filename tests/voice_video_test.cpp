#include "voice/video_codec.h"
#include "voice/video_transport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }
  std::shared_ptr<voice::VideoFrame> pattern() {
    auto frame = std::make_shared<voice::VideoFrame>();
    frame->width = 320;
    frame->height = 180;
    frame->stride = frame->width * 4;
    frame->timestamp = 1000000;
    frame->rgba.resize(frame->stride * frame->height);
    for (int y = 0; y < frame->height; ++y)
      for (int x = 0; x < frame->width; ++x) {
        auto* pixel = frame->rgba.data() + y * frame->stride + x * 4;
        pixel[0] = x * 255 / frame->width;
        pixel[1] = y * 255 / frame->height;
        pixel[2] = 64;
        pixel[3] = 255;
      }
    return frame;
  }
} // namespace
int main() {
  using namespace voice;
  using namespace std::chrono_literals;
  try {
    ShareSettings settings;
    settings.resolution = 999;
    settings.frameRate = 100;
    settings.audioVolume = NAN;
    settings.audioSources = {"app:7", "app:7", "", "app:8"};
    settings = sanitizeShareSettings(settings);
    require(
        settings.resolution == 720
            && settings.frameRate == 30
            && settings.audioVolume == 1
            && settings.audioSources.size() == 2,
        "Saved quality and audio choices are bounded"
    );
    VideoAdaptation motion, clarity;
    settings.resolution = 2160;
    settings.frameRate = 60;
    motion.configure(settings);
    settings.content = ContentPreference::Clarity;
    clarity.configure(settings);
    const auto now = VideoAdaptation::Clock::time_point{} + 10s;
    motion.feedback(.1F, {}, false, now);
    clarity.feedback(.1F, {}, false, now);
    require(
        motion.quality(3840, 2160).height < 2160 && motion.quality(3840, 2160).frameRate == 60,
        "Smoothness protects frame rate under measured loss"
    );
    require(
        clarity.quality(3840, 2160).height == 2160 && clarity.quality(3840, 2160).frameRate < 60,
        "Clarity protects resolution under measured loss"
    );
    for (int i = 0; i < 5; ++i) {
      motion.feedback(0, {}, false, now + 10s + i * 3s);
      clarity.feedback(0, {}, false, now + 10s + i * 3s);
    }
    require(
        motion.quality(3840, 2160).height == 2160 && clarity.quality(3840, 2160).frameRate == 60,
        "Sustained healthy feedback restores requested quality"
    );
    VideoAdaptation constrained;
    constrained.configure(settings);
    require(
        constrained.networkConstraints(1000000, 1280 * 720, now), "The first receiver constraint is applied immediately"
    );
    auto limited = constrained.quality(3840, 2160);
    require(
        limited.height == 720 && limited.frameRate == 60 && limited.bitrate <= 1000000,
        "Receiver pixels preserve source aspect and selected frame rate while bandwidth caps encoder bitrate"
    );
    require(
        !constrained.networkConstraints(1000000, 640 * 360, now + 1s),
        "Rapid receiver resizing does not continuously reopen the encoder"
    );
    require(
        constrained.networkConstraints(1000000, 640 * 360, now + 2s) && constrained.quality(3840, 2160).height == 360,
        "The latest receiver size applies after the bounded adaptation interval"
    );
    require(
        constrained.quality(1080, 1920).height == 640,
        "Portrait sharing uses the actual source aspect rather than a landscape assumption"
    );
    require(
        constrained.networkConstraints(0, 0, now + 2100ms) && constrained.quality(3840, 2160).height == 2160,
        "A zero pixel budget removes the limit immediately when a receiver resumes without a size constraint"
    );
    require(constrained.quality(640, 360).height == 360, "Removing receiver limits never upscales a smaller source");
    require(
        constrained.networkConstraints(1000000, 1280 * 720, now + 2200ms), "A new size can constrain a resumed receiver"
    );
    require(
        !constrained.networkConstraints(980000, 1280 * 720 * 103 / 100, now + 5s),
        "Small bandwidth and pixel fluctuations stay inside hysteresis after the cooldown has elapsed"
    );
    VideoAdaptation minimumSize;
    auto sourceSettings = settings;
    sourceSettings.resolution = 0;
    minimumSize.configure(sourceSettings);
    minimumSize.networkConstraints(0, 4, now);
    require(
        minimumSize.quality(10000, 2).height == 2 && minimumSize.quality(2, 10000).height == 10000,
        "Impossible tiny requests retain the minimum feasible dimensions for an extreme source aspect"
    );
    const auto source = pattern();
    for (const auto codec : {MediaCodec::VP8, MediaCodec::H264, MediaCodec::VP9}) {
      VideoEncoder encoder;
      VideoDecoder decoder;
      std::string error;
      const auto encoded =
          encoder.encode(*source, codec, {180, 30, 1000000}, false, true, ContentPreference::Clarity, error);
      require(
          error.empty() && encoded.size() == 1 && encoded.front().keyframe, "A real software codec produces a keyframe"
      );
      auto output = decoder.decode(encoded.front(), error, false);
      require(
          error.empty()
              && output
              && output->valid()
              && output->width == source->width
              && output->height == source->height,
          "A real decoder recovers native dimensions"
      );
      double squared = 0;
      for (std::size_t i = 0; i < source->rgba.size(); ++i) {
        const auto delta = static_cast<int>(source->rgba[i]) - output->rgba[i];
        squared += delta * delta;
      }
      require(squared / source->rgba.size() < 100, "Decoded gradient preserves color and content");
    }
    std::mutex mutex;
    std::condition_variable condition;
    unsigned captured = 0;
    std::int64_t capturedAt = 0;
    VideoPipeline pipeline;
    settings = {};
    settings.hardware = false;
    pipeline.configure(settings);
    pipeline.capture([&](EncodedVideo frame) {
      require(!frame.bytes.empty(), "Pipeline output contains encoded media");
      std::lock_guard lock(mutex);
      ++captured;
      capturedAt = frame.capturedAtUs;
      condition.notify_all();
    });
    pipeline.source(source);
    require(pipeline.frame() == source && captured == 0, "Preview is available before encrypted transmission is ready");
    pipeline.sending(true);
    {
      std::unique_lock lock(mutex);
      require(
          condition.wait_for(lock, 5s, [&] { return captured == 1; }),
          "Encrypted readiness encodes the retained consented source without another damage callback"
      );
      require(capturedAt > source->timestamp, "A retained static frame receives a current presentation timestamp");
      pipeline.keyframe();
      require(
          condition.wait_for(lock, 5s, [&] { return captured == 2; }),
          "A keyframe request resends static content without another capture callback"
      );
    }
    pipeline.sending(false);
    pipeline.sending(true);
    {
      std::unique_lock lock(mutex);
      require(
          condition.wait_for(lock, 5s, [&] { return captured == 3; }),
          "Resuming a static source schedules a fresh keyframe"
      );
    }
    require(pipeline.networkConstraints(1000000, 160 * 90), "Receiver constraints reach the real codec worker");
    {
      std::unique_lock lock(mutex);
      require(
          condition.wait_for(lock, 5s, [&] { return captured == 4; }) && pipeline.statistics().quality.height == 90,
          "A static source is re-encoded at the receiver size without another capture callback"
      );
    }
    require(pipeline.networkConstraints(0, 0), "A native publisher can clear its receiver limits");
    {
      std::unique_lock lock(mutex);
      require(
          condition.wait_for(lock, 5s, [&] { return captured == 5; }) && pipeline.statistics().quality.height == 180,
          "Clearing receiver limits restores the original selected source quality"
      );
    }
    pipeline.clear();
    pipeline.sending(true);
    {
      std::unique_lock lock(mutex);
      require(
          !condition.wait_for(lock, std::chrono::milliseconds{50}, [&] { return captured > 5; }) && !pipeline.frame(),
          "Cleared capture cannot be revived by readiness or a keyframe request"
      );
    }
    std::cout << "Video codecs and adaptation passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
