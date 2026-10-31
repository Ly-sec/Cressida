#include "voice/stream_audio.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <numbers>
#include <opus.h>
#include <poll.h>
#include <stdexcept>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  template <class Predicate> void await(voice::Audio& audio, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!predicate(audio.state())) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      require(remaining.count() > 0, "Private shared-audio source discovery finishes within its deadline");
      pollfd event{audio.eventFd(), POLLIN, 0};
      ::poll(&event, 1, remaining.count());
      audio.drainEvents();
    }
  }
  class Samples {
  public:
    Samples() {
      int error = 0;
      decoder = opus_decoder_create(voice::sampleRate, 2, &error);
      require(decoder && error == OPUS_OK, "The private stereo decoder initializes");
    }
    ~Samples() { opus_decoder_destroy(decoder); }
    void receive(voice::EncodedAudio packet) {
      std::lock_guard lock(mutex);
      std::array<float, voice::frameSamples * 2> frame{};
      const auto samples =
          opus_decode_float(decoder, packet.opus.data(), packet.opus.size(), frame.data(), voice::frameSamples, 0);
      if (samples > 0 && frames.size() < maximumFrames)
        frames.push_back(frame);
      ++received;
      condition.notify_all();
    }
    void collect() {
      using namespace std::chrono_literals;
      std::unique_lock lock(mutex);
      frames.clear();
      const auto began = received;
      require(
          condition.wait_for(lock, 3s, [&] { return received >= began + maximumFrames; }),
          "Private shared-audio capture supplies real stereo Opus frames"
      );
    }
    double tone(unsigned channel, double frequency) {
      std::lock_guard lock(mutex);
      double real = 0, imaginary = 0;
      std::size_t at = 0;
      constexpr std::size_t settleFrames = 5;
      for (std::size_t index = settleFrames; index < frames.size(); ++index)
        for (int sample = 0; sample < voice::frameSamples; ++sample, ++at) {
          const auto angle = 2 * std::numbers::pi * frequency * at / voice::sampleRate;
          real += frames[index][sample * 2 + channel] * std::cos(angle);
          imaginary += frames[index][sample * 2 + channel] * std::sin(angle);
        }
      return at ? std::hypot(real, imaginary) / at : 0;
    }
    double peak() {
      std::lock_guard lock(mutex);
      double peak = 0;
      for (std::size_t index = 5; index < frames.size(); ++index)
        for (auto value : frames[index])
          peak = std::max(peak, std::abs(static_cast<double>(value)));
      return peak;
    }

  private:
    static constexpr std::size_t maximumFrames = 30;
    OpusDecoder* decoder = nullptr;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::array<float, voice::frameSamples * 2>> frames;
    std::uint64_t received = 0;
  };
} // namespace
int main() {
  try {
    const auto* isolated = std::getenv("CRESSIDA_VOICE_TEST_PRIVATE");
    require(isolated && std::string_view(isolated) == "1", "Shared-audio checks require a private audio server");
    auto audio = voice::makeStreamAudio(true);
    audio->refreshDevices();
    await(*audio, [](const auto& state) { return state.inputs.size() >= 2 || !state.error.empty(); });
    const auto devices = audio->state();
    require(devices.error.empty() && !devices.running, "Source enumeration never starts capture");
    require(
        std::ranges::any_of(
            devices.inputs, [](const auto& source) { return source.id == "application:fixture.browser"; }
        )
            && std::ranges::any_of(
                devices.inputs, [](const auto& source) { return source.id == "application:fixture.music"; }
            ),
        "Applications have independently selectable stable source identities"
    );
    require(
        std::ranges::none_of(
            devices.inputs, [](const auto& source) { return source.name == "Excluded self playback"; }
        ),
        "Cressida playback is excluded during source discovery"
    );
    Samples samples;
    audio->sources({"application:fixture.browser"}, 1);
    std::string error;
    require(
        audio->start([&](auto packet) { samples.receive(std::move(packet)); }, error),
        error.empty() ? "Private application capture starts" : error.c_str()
    );
    samples.collect();
    require(
        samples.tone(0, 400) > .02 && samples.tone(1, 800) > .02,
        "Application audio preserves separate left and right channels"
    );
    require(
        samples.tone(0, 200) < .005 && samples.tone(1, 600) < .005,
        "Unselected applications do not enter the captured mix"
    );
    audio->sources({"application:fixture.browser", "application:fixture.music"}, 1);
    samples.collect();
    require(
        samples.tone(0, 400) > .02
            && samples.tone(0, 200) > .02
            && samples.tone(1, 800) > .02
            && samples.tone(1, 600) > .02,
        "Multiple independently selected applications mix into shared stereo audio"
    );
    audio->sources({"system"}, 1);
    samples.collect();
    require(
        samples.tone(0, 400) > .02
            && samples.tone(0, 200) > .02
            && samples.tone(0, 1000) < .005
            && samples.tone(1, 1000) < .005,
        "System application sharing excludes Cressida playback from the actual audio samples"
    );
    voice::AudioSettings microphoneFlags;
    microphoneFlags.muted = true;
    microphoneFlags.pushToTalk = true;
    audio->configure(microphoneFlags);
    samples.collect();
    require(
        samples.tone(0, 400) > .02, "Microphone mute and push-to-talk settings do not gate shared application audio"
    );
    audio->sources({}, 1);
    samples.collect();
    require(samples.peak() < .001, "The none choice opens no application capture and produces silence");
    audio->stop();
    require(!audio->state().running, "Explicit stop releases application capture");
    std::cout << "Private stereo application selection, mixing, exclusion and stop passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
