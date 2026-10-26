#include "voice/audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <pulse/pulseaudio.h>
#include <stdexcept>

namespace {
  void require(bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  }
  template <class Predicate> void await(const std::shared_ptr<voice::Audio>& audio, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{6};
    while (!predicate(audio->state())) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      require(remaining.count() > 0, "The private native audio operation completes within its deadline");
      pollfd event{audio->eventFd(), POLLIN, 0};
      (void)::poll(&event, 1, static_cast<int>(remaining.count()));
      audio->drainEvents();
    }
  }
  class PrivateAudioControl {
  public:
    PrivateAudioControl() {
      const auto* isolated = std::getenv("CRESSIDA_VOICE_TEST_PRIVATE");
      require(
          isolated && std::string_view(isolated) == "1", "Device-removal controls require the private server fixture"
      );
      m_loop = pa_mainloop_new();
      require(m_loop, "The private audio control loop initializes");
      m_context = pa_context_new(pa_mainloop_get_api(m_loop), "Cressida private voice fixture");
      require(
          m_context && pa_context_connect(m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0,
          "The fixture connects only to its explicitly selected private audio server"
      );
      spin([&] { return pa_context_get_state(m_context) == PA_CONTEXT_READY; });
    }
    ~PrivateAudioControl() {
      if (m_context) {
        pa_context_disconnect(m_context);
        pa_context_unref(m_context);
      }
      if (m_loop)
        pa_mainloop_free(m_loop);
    }
    void removeInput(const char* name) {
      Result result;
      finish(
          pa_context_get_source_info_by_name(
              m_context, name,
              [](pa_context*, const pa_source_info* info, int end, void* data) {
                auto& result = *static_cast<Result*>(data);
                if (info) {
                  result.index = info->owner_module;
                  result.success = result.index != PA_INVALID_INDEX;
                }
                result.done = end != 0;
              },
              &result
          ),
          result
      );
      unload(result.index);
    }
    void removeOutput(const char* name) {
      Result result;
      finish(
          pa_context_get_sink_info_by_name(
              m_context, name,
              [](pa_context*, const pa_sink_info* info, int end, void* data) {
                auto& result = *static_cast<Result*>(data);
                if (info) {
                  result.index = info->owner_module;
                  result.success = result.index != PA_INVALID_INDEX;
                }
                result.done = end != 0;
              },
              &result
          ),
          result
      );
      unload(result.index);
    }
    void temporaryDefaults() {
      load("module-null-source", "source_name=cressida_default_test_input rate=48000 channels=1");
      load("module-null-sink", "sink_name=cressida_default_test_output rate=48000 channels=1");
      Result input, output;
      finish(pa_context_set_default_source(m_context, "cressida_default_test_input", succeeded, &input), input);
      finish(pa_context_set_default_sink(m_context, "cressida_default_test_output", succeeded, &output), output);
    }

  private:
    struct Result {
      bool done = false, success = false;
      std::uint32_t index = PA_INVALID_INDEX;
    };
    static void succeeded(pa_context*, int success, void* data) {
      auto& result = *static_cast<Result*>(data);
      result.success = success;
      result.done = true;
    }
    template <class Predicate> void spin(Predicate ready) {
      constexpr int pollMilliseconds = 5;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
      while (!ready()) {
        const auto state = pa_context_get_state(m_context);
        require(
            state != PA_CONTEXT_FAILED && state != PA_CONTEXT_TERMINATED,
            "The private audio control connection stays available"
        );
        require(std::chrono::steady_clock::now() < deadline, "Private device operations have a finite deadline");
        int result = 0;
        require(pa_mainloop_iterate(m_loop, 0, &result) >= 0, "Private device operation dispatch succeeds");
        if (!ready())
          (void)::poll(nullptr, 0, pollMilliseconds);
      }
    }
    void finish(pa_operation* operation, Result& result) {
      require(operation, "A private audio control operation starts");
      try {
        spin([&] { return result.done; });
      } catch (...) {
        pa_operation_cancel(operation);
        pa_operation_unref(operation);
        throw;
      }
      pa_operation_unref(operation);
      require(result.success, "The private audio server accepts the device operation");
    }
    void unload(std::uint32_t module) {
      Result result;
      finish(pa_context_unload_module(m_context, module, succeeded, &result), result);
    }
    void load(const char* name, const char* arguments) {
      Result result;
      finish(
          pa_context_load_module(
              m_context, name, arguments,
              [](pa_context*, std::uint32_t index, void* data) {
                auto& result = *static_cast<Result*>(data);
                result.index = index;
                result.success = index != PA_INVALID_INDEX;
                result.done = true;
              },
              &result
          ),
          result
      );
    }
    pa_mainloop* m_loop = nullptr;
    pa_context* m_context = nullptr;
  };
} // namespace

int main() {
  try {
    auto audio = voice::makeAudio();
    audio->refreshDevices();
    await(audio, [](const auto& state) {
      return (!state.inputs.empty() && !state.outputs.empty()) || !state.error.empty();
    });
    require(audio->state().error.empty(), "The private audio server lists its null devices");
    const auto initial = audio->state();
    require(
        std::ranges::any_of(initial.inputs, [](const auto& device) { return device.id == "cressida_test_input"; })
            && std::ranges::any_of(
                initial.outputs, [](const auto& device) { return device.id == "cressida_test_output"; }
            ),
        "Input and output enumeration returns the private server's actual device identities"
    );
    voice::AudioSettings settings;
    settings.inputDevice = "cressida_test_input";
    settings.outputDevice = "cressida_test_output";
    settings.pushToTalk = settings.pttPressed = true;
    audio->configure(settings);
    std::atomic<unsigned> captured = 0, transmitted = 0;
    std::atomic<bool> valid = true;
    const auto destination = [&](voice::EncodedAudio packet) {
      const auto frame = captured.fetch_add(1);
      if (packet.timestamp != frame * voice::frameSamples)
        valid = false;
      if (packet.speaking && !packet.opus.empty())
        ++transmitted;
    };
    std::string error;
    require(audio->start(destination, error), "Native capture opens the selected null input and output");
    await(audio, [&](const auto& state) { return transmitted >= 4 && state.running; });
    require(
        valid && audio->state().error.empty(), "The native worker delivers bounded Opus frames on its sample timeline"
    );
    require(!audio->startTest(error), "A microphone test cannot replace a joined voice conversation");
    settings.muted = true;
    audio->configure(settings);
    await(audio, [](const auto& state) { return !state.speaking; });
    const auto mutedCount = transmitted.load(), initialFrames = captured.load();
    await(audio, [&](const auto&) { return captured >= initialFrames + 4; });
    require(transmitted == mutedCount, "Native mute keeps ticking without forwarding microphone packets");
    audio->stop();
    const auto stoppedCount = captured.load();
    require(!audio->state().running, "Stop completes after retiring streams and pending capture callbacks");
    settings.muted = false;
    audio->configure(settings);
    require(audio->startTest(error), "The native microphone test starts explicitly after leaving voice");
    await(audio, [](const auto& state) { return state.testing || !state.error.empty(); });
    require(
        audio->state().testing && audio->state().error.empty() && captured == stoppedCount,
        "Microphone testing plays locally without invoking a voice destination"
    );
    audio->stopTest();
    await(audio, [](const auto& state) { return !state.testing && !state.loading; });
    settings.inputDevice = "cressida_missing_test_input";
    audio->configure(settings);
    require(
        !audio->start([](auto) {}, error) && !error.empty(), "A removed or unavailable selected device fails explicitly"
    );
    settings.inputDevice = "cressida_test_input";
    audio->configure(settings);
    require(
        audio->start([](auto) {}, error), "A fresh explicit connection recovers after selecting an available device"
    );
    PrivateAudioControl control;
    control.removeInput("cressida_test_input");
    await(audio, [](const auto& state) { return !state.running && !state.error.empty(); });
    require(
        !audio->start([](auto) {}, error),
        "An unplugged saved microphone cannot silently use the available fallback input"
    );
    settings.inputDevice = "cressida_fallback_input";
    audio->configure(settings);
    require(audio->start([](auto) {}, error), "Choosing an available microphone permits an explicit new connection");
    control.removeOutput("cressida_test_output");
    await(audio, [](const auto& state) { return !state.running && !state.error.empty(); });
    require(
        !audio->start([](auto) {}, error),
        "An unplugged saved output cannot silently use the available fallback speakers"
    );
    settings.outputDevice = "cressida_fallback_output";
    audio->configure(settings);
    require(audio->start([](auto) {}, error), "Choosing an available output recovers after selected-device loss");
    audio->stop();
    control.temporaryDefaults();
    settings.inputDevice.clear();
    settings.outputDevice.clear();
    audio->configure(settings);
    std::atomic<unsigned> defaultFrames = 0;
    require(
        audio->start([&](auto) { ++defaultFrames; }, error),
        "System-default devices open without pinning a physical input or output"
    );
    await(audio, [&](const auto& state) { return state.running && defaultFrames >= 3; });
    control.removeInput("cressida_default_test_input");
    control.removeOutput("cressida_default_test_output");
    const auto movedAt = defaultFrames.load();
    await(audio, [&](const auto& state) { return !state.error.empty() || defaultFrames >= movedAt + 5; });
    require(
        audio->state().running && audio->state().error.empty(),
        "System-default streams keep working when the audio server moves them to available replacements"
    );
    audio->stop();
    audio.reset();
    std::cout
        << "Private native voice devices, capture, mute, microphone test, unplug and default-device recovery passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
