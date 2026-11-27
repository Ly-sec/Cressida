#include "voice/stream_audio.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <numbers>
#include <opus.h>
#include <poll.h>
#include <pulse/pulseaudio.h>
#include <stdexcept>
#include <thread>

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
  class PrivateSourceControl {
  public:
    PrivateSourceControl() {
      loop = pa_mainloop_new();
      require(loop, "The private source-control loop initializes");
      context = pa_context_new(pa_mainloop_get_api(loop), "Cressida private stream fixture");
      require(
          context && pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0,
          "Source controls connect to the fixture's private server"
      );
      spin([&] { return pa_context_get_state(context) == PA_CONTEXT_READY; });
    }
    ~PrivateSourceControl() {
      pa_context_disconnect(context);
      pa_context_unref(context);
      pa_mainloop_free(loop);
    }
    std::vector<std::uint32_t> monitors() {
      std::vector<std::uint32_t> result;
      auto* operation = pa_context_get_source_output_info_list(
          context,
          [](pa_context*, const pa_source_output_info* info, int, void* data) {
            if (!info || !info->proplist)
              return;
            const auto* name = pa_proplist_gets(info->proplist, PA_PROP_MEDIA_NAME);
            if (name && std::string_view(name) == "Shared application audio")
              static_cast<std::vector<std::uint32_t>*>(data)->push_back(info->index);
          },
          &result
      );
      finish(operation);
      return result;
    }
    void volume() {
      const auto index = music();
      pa_cvolume volume;
      pa_cvolume_set(&volume, 2, pa_sw_volume_from_linear(.99));
      bool succeeded = false;
      finish(pa_context_set_sink_input_volume(
          context, index, &volume, [](pa_context*, int success, void* data) { *static_cast<bool*>(data) = success; },
          &succeeded
      ));
      require(succeeded, "The private server accepts the unselected application's volume change");
    }
    void closeMusic() {
      bool succeeded = false;
      finish(pa_context_kill_sink_input(
          context, music(), [](pa_context*, int success, void* data) { *static_cast<bool*>(data) = success; },
          &succeeded
      ));
      require(succeeded, "The private server closes the selected music application's playback");
    }

  private:
    std::uint32_t music() {
      std::uint32_t index = PA_INVALID_INDEX;
      auto* operation = pa_context_get_sink_input_info_list(
          context,
          [](pa_context*, const pa_sink_input_info* info, int, void* data) {
            if (!info || !info->proplist)
              return;
            const auto* id = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_ID);
            if (id && std::string_view(id) == "fixture.music")
              *static_cast<std::uint32_t*>(data) = info->index;
          },
          &index
      );
      finish(operation);
      require(index != PA_INVALID_INDEX, "The private unselected application exists");
      return index;
    }
    template <class Predicate> void spin(Predicate ready) {
      constexpr int pollMilliseconds = 5;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
      while (!ready()) {
        require(
            std::chrono::steady_clock::now() < deadline && PA_CONTEXT_IS_GOOD(pa_context_get_state(context)),
            "Private source operations finish within their deadline"
        );
        int result;
        require(pa_mainloop_iterate(loop, 0, &result) >= 0, "Private source operation dispatch succeeds");
        if (!ready())
          ::poll(nullptr, 0, pollMilliseconds);
      }
    }
    void finish(pa_operation* operation) {
      require(operation, "A private source operation starts");
      try {
        spin([&] { return pa_operation_get_state(operation) != PA_OPERATION_RUNNING; });
      } catch (...) {
        pa_operation_cancel(operation);
        pa_operation_unref(operation);
        throw;
      }
      pa_operation_unref(operation);
    }
    pa_mainloop* loop = nullptr;
    pa_context* context = nullptr;
  };
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
      if (samples > 0 && frames.size() < maximumFrames) {
        frames.push_back(frame);
        timestamps.push_back(packet.timestamp);
      }
      lastFrame = frame;
      lastTimestamp = packet.timestamp;
      ++received;
      condition.notify_all();
    }
    void collect(std::function<void()> action = {}) {
      using namespace std::chrono_literals;
      std::unique_lock lock(mutex);
      frames.clear();
      timestamps.clear();
      if (received) {
        frames.push_back(lastFrame);
        timestamps.push_back(lastTimestamp);
      }
      const auto began = received;
      lock.unlock();
      if (action)
        action();
      lock.lock();
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
    void continuous(bool checkSamples = true) {
      std::lock_guard lock(mutex);
      constexpr float maximumJump = .03F;
      for (std::size_t index = 1; index < frames.size(); ++index) {
        require(
            timestamps[index] - timestamps[index - 1] == voice::frameSamples,
            "Capture retains every sample across packet boundaries and source updates"
        );
        for (unsigned channel = 0; checkSamples && channel < 2; ++channel)
          require(
              std::abs(frames[index][channel] - frames[index - 1][(voice::frameSamples - 1) * 2 + channel])
                  < maximumJump,
              "A continuous synthetic tone has no audible sample discontinuities between packets"
          );
      }
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
    std::vector<std::uint32_t> timestamps;
    std::array<float, voice::frameSamples * 2> lastFrame{};
    std::uint32_t lastTimestamp = 0;
    std::uint64_t received = 0;
  };
  void playback() {
    using namespace std::chrono_literals;
    auto audio = voice::makeStreamAudio(false);
    std::string error;
    require(audio->start([](auto) {}, error), "The private stereo playback device starts");
    int status = OPUS_OK;
    auto* encoder = opus_encoder_create(voice::sampleRate, 2, OPUS_APPLICATION_AUDIO, &status);
    require(encoder && status == OPUS_OK, "The private playback encoder initializes");
    const auto began = std::chrono::steady_clock::now();
    constexpr unsigned totalFrames = 350, pauseBegin = 150, pauseEnd = 200;
    std::uint16_t sequence = 65500;
    for (unsigned frame = 0; frame < totalFrames; ++frame) {
      std::this_thread::sleep_until(began + frame * 20ms);
      if (frame >= pauseBegin && frame < pauseEnd)
        continue;
      std::array<float, voice::frameSamples * 2> samples{};
      for (int index = 0; index < voice::frameSamples; ++index) {
        const auto position = frame * voice::frameSamples + index;
        samples[index * 2] = .1F * std::sin(2 * std::numbers::pi * 440 * position / voice::sampleRate);
        samples[index * 2 + 1] = .1F * std::sin(2 * std::numbers::pi * 880 * position / voice::sampleRate);
      }
      std::array<std::uint8_t, 1275> packet{};
      const auto bytes = opus_encode_float(encoder, samples.data(), voice::frameSamples, packet.data(), packet.size());
      require(bytes > 0, "Private stereo playback packets encode");
      audio->receiveAt(
          123, sequence++, frame * voice::frameSamples, std::span{packet}.first(bytes), began + frame * 20ms + 100ms
      );
    }
    std::this_thread::sleep_until(began + totalFrames * 20ms + 100ms);
    audio->stop();
    require(!audio->state().running, "Stopping playback retires its private device");
    opus_encoder_destroy(encoder);
  }
} // namespace
int main(int argc, char** argv) {
  try {
    const auto* isolated = std::getenv("CRESSIDA_VOICE_TEST_PRIVATE");
    require(isolated && std::string_view(isolated) == "1", "Shared-audio checks require a private audio server");
    if (argc == 2 && std::string_view(argv[1]) == "--playback") {
      playback();
      return 0;
    }
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
        samples.tone(0, 440) > .02 && samples.tone(1, 880) > .02,
        "Application audio preserves separate left and right channels"
    );
    require(
        samples.tone(0, 200) < .005 && samples.tone(1, 600) < .005,
        "Unselected applications do not enter the captured mix"
    );
    samples.continuous();
    PrivateSourceControl control;
    const auto originalMonitors = control.monitors();
    require(originalMonitors.size() == 1, "One selected application has one private capture monitor");
    samples.collect([&] { audio->refreshDevices(); });
    samples.continuous();
    require(control.monitors() == originalMonitors, "Refreshing sources retains the existing monitor and sample queue");
    samples.collect([&] { control.volume(); });
    samples.continuous();
    require(control.monitors() == originalMonitors, "Unselected application volume changes do not restart capture");
    samples.collect([&] { audio->sources({"application:fixture.browser"}, .9F); });
    samples.continuous();
    audio->sources({"application:fixture.browser"}, 1);
    audio->sources({"application:fixture.browser", "application:fixture.music"}, 1);
    samples.collect();
    const auto mixedMonitors = control.monitors();
    require(
        mixedMonitors.size() == 2 && std::ranges::find(mixedMonitors, originalMonitors.front()) != mixedMonitors.end(),
        "Adding an application preserves the existing selected monitor"
    );
    require(
        samples.tone(0, 440) > .02
            && samples.tone(0, 200) > .02
            && samples.tone(1, 880) > .02
            && samples.tone(1, 600) > .02,
        "Multiple independently selected applications mix into shared stereo audio"
    );
    audio->sources({"system"}, 1);
    samples.collect();
    require(
        samples.tone(0, 440) > .02
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
        samples.tone(0, 440) > .02, "Microphone mute and push-to-talk settings do not gate shared application audio"
    );
    samples.collect([&] { control.closeMusic(); });
    samples.continuous(false);
    require(
        audio->state().running && audio->state().error.empty(),
        "Closing one selected application retains capture for the others"
    );
    require(control.monitors() == originalMonitors, "Closing a selected application preserves the other monitor");
    require(
        samples.tone(0, 440) > .02 && samples.tone(0, 200) < .005,
        "The remaining selected application continues after another source closes"
    );
    audio->sources({}, 1);
    samples.collect();
    require(samples.peak() < .001, "The none choice opens no application capture and produces silence");
    audio->stop();
    require(!audio->state().running, "Explicit stop releases application capture");
    std::cout << "Private stereo capture continuity, refresh, gain, application selection, mixing, exclusion and stop "
                 "passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
