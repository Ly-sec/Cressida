#include "voice/audio.h"
#include "voice/audio_dsp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <poll.h>
#include <stdexcept>

namespace {
  void require(bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  }
  voice::AudioFrame tone(int frame) {
    constexpr float amplitude = 0.2F;
    constexpr float frequency = 440;
    voice::AudioFrame result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
      const auto sample = static_cast<double>(frame) * voice::frameSamples + i;
      result[i] =
          amplitude * static_cast<float>(std::sin(sample * frequency * 2 * std::numbers::pi / voice::sampleRate));
    }
    return result;
  }
  float peak(const voice::AudioFrame& frame) {
    float result = 0;
    for (float sample : frame) {
      require(std::isfinite(sample) && std::abs(sample) <= 1, "Mixed voice samples remain finite and bounded");
      result = std::max(result, std::abs(sample));
    }
    return result;
  }
  template <class Predicate> void await(const std::shared_ptr<voice::Audio>& audio, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{6};
    while (!predicate(audio->state())) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      require(remaining.count() > 0, "Native audio completion arrives within its bounded deadline");
      pollfd descriptor{audio->eventFd(), POLLIN, 0};
      (void)::poll(&descriptor, 1, static_cast<int>(remaining.count()));
      audio->drainEvents();
    }
  }
} // namespace

int main() {
  try {
    voice::AudioSettings settings;
    settings.inputVolume = std::numeric_limits<float>::infinity();
    settings.outputVolume = -1;
    settings.threshold = std::numeric_limits<float>::quiet_NaN();
    settings.inputDevice = std::string("bad\0device", 10);
    settings.outputDevice = std::string(1024, 'x');
    const auto normalized = voice::sanitizeAudioSettings(settings);
    require(
        normalized.inputVolume == 1
            && normalized.outputVolume == 0
            && normalized.threshold == voice::defaultActivityThreshold
            && normalized.inputDevice.empty()
            && normalized.outputDevice.empty(),
        "Malformed audio settings cannot reach the device or codec APIs"
    );

    voice::AudioProcessor sender;
    voice::AudioFrame silence{};
    require(sender.capture(silence).opus.empty(), "Silence below the activity threshold is not transmitted");
    auto packet = sender.capture(tone(0));
    require(
        packet.speaking
            && !packet.opus.empty()
            && packet.opus.size() <= 1275
            && packet.timestamp == voice::frameSamples,
        "Capture produces a bounded 20ms Opus frame on a monotonic sample timeline"
    );
    require(sender.inputLevel() > 0 && sender.inputLevel() < 1, "The input meter reflects captured samples");
    require(sender.capture(silence).speaking, "A short activity hangover avoids clipping word endings");
    for (int frame = 0; frame < 20; ++frame)
      packet = sender.capture(silence);
    require(!packet.speaking && packet.opus.empty(), "The activity hangover settles without continued speech");
    settings = {};
    settings.muted = true;
    sender.configure(settings);
    require(sender.capture(tone(1)).opus.empty(), "Mute closes the capture gate immediately");
    settings.muted = false;
    sender.configure(settings);
    require(sender.capture(silence).opus.empty(), "Muted speech cannot leave a queued activity hangover");
    settings.pushToTalk = true;
    sender.configure(settings);
    require(sender.capture(tone(2)).opus.empty(), "Push to talk never transmits without a held key");
    settings.pttPressed = true;
    sender.configure(settings);
    require(!sender.capture(tone(3)).opus.empty(), "Holding push to talk opens the microphone gate");
    settings.pttPressed = false;
    sender.configure(settings);
    require(sender.capture(tone(4)).opus.empty(), "Releasing push to talk closes the microphone gate");
    bool invalidFrame = false;
    try {
      (void)sender.capture(std::span<const float>(silence).first(1));
    } catch (const std::invalid_argument&) {
      invalidFrame = true;
    }
    require(invalidFrame, "The codec rejects partial microphone frames");

    settings = {};
    settings.pushToTalk = settings.pttPressed = true;
    sender.configure(settings);
    sender.reset();
    std::vector<voice::EncodedAudio> packets;
    for (int frame = 0; frame < 10; ++frame)
      packets.push_back(sender.capture(tone(frame)));
    voice::AudioProcessor ordered, reordered, quieter;
    voice::AudioSettings quiet;
    quiet.outputVolume = 0.5F;
    quieter.configure(quiet);
    for (std::size_t frame = 0; frame < packets.size(); ++frame) {
      ordered.receive(1, static_cast<std::uint16_t>(frame), packets[frame].timestamp, packets[frame].opus);
      quieter.receive(1, static_cast<std::uint16_t>(frame), packets[frame].timestamp, packets[frame].opus);
    }
    for (const std::size_t frame : {0, 2, 1, 4, 3, 5, 7, 6, 9, 8})
      reordered.receive(1, static_cast<std::uint16_t>(frame), packets[frame].timestamp, packets[frame].opus);
    reordered.receive(1, 1, packets[1].timestamp, packets[1].opus);
    float audible = 0;
    for (std::size_t frame = 0; frame < packets.size() + 2; ++frame) {
      const auto original = ordered.playback(), actual = reordered.playback(), reduced = quieter.playback();
      require(original == actual, "Jitter reordering and duplicates preserve the exact decode order");
      for (std::size_t i = 0; i < original.size(); ++i)
        require(reduced[i] == original[i] * quiet.outputVolume, "Output volume scales decoded voice samples");
      if (frame < 2)
        require(peak(original) == 0, "The initial jitter delay buffers two audio frames");
      audible = std::max(audible, peak(original));
    }
    require(audible > 0.05F, "Encoded synthetic speech decodes to audible playback");

    voice::AudioProcessor wrapped;
    const auto wrappedBase = std::numeric_limits<std::uint32_t>::max() - voice::frameSamples;
    for (std::size_t frame = 0; frame < 3; ++frame)
      wrapped.receive(
          2, static_cast<std::uint16_t>(65534 + frame),
          wrappedBase + static_cast<std::uint32_t>(frame * voice::frameSamples), packets[frame].opus
      );
    for (int frame = 0; frame < 5; ++frame)
      (void)peak(wrapped.playback());
    require(wrapped.outputLevel() > 0, "RTP sequence and timestamp wrap preserve playout");

    voice::AudioProcessor paused, resumed;
    paused.receive(1, 0, packets[0].timestamp, packets[0].opus);
    for (int frame = 0; frame < 5; ++frame)
      (void)paused.playback();
    const auto resumeTimestamp = static_cast<std::uint32_t>(voice::frameSamples * 20);
    paused.receive(1, 1, resumeTimestamp, packets[1].opus);
    resumed.receive(1, 1, resumeTimestamp, packets[1].opus);
    for (int frame = 0; frame < 3; ++frame)
      require(
          paused.playback() == resumed.playback(),
          "A fresh talk spurt resumes after silence without losing its first packet to concealment"
      );

    voice::AudioProcessor lossy;
    lossy.receive(1, 0, packets[0].timestamp, packets[0].opus);
    lossy.receive(1, 2, packets[2].timestamp, packets[2].opus);
    float concealed = 0;
    for (int frame = 0; frame < 5; ++frame)
      concealed = std::max(concealed, peak(lossy.playback()));
    require(concealed > 0, "A missing packet uses bounded decoder loss concealment");
    settings = {};
    settings.deafened = true;
    lossy.configure(settings);
    lossy.receive(2, 0, packets[0].timestamp, packets[0].opus);
    require(
        lossy.speakers() == 0 && peak(lossy.playback()) == 0 && lossy.capture(tone(0)).opus.empty(),
        "Deafen drops receive buffers, silences playback and closes the microphone gate"
    );
    settings.deafened = false;
    lossy.configure(settings);
    lossy.receive(1, 3, packets[3].timestamp, packets[3].opus);
    require(lossy.speakers() == 1, "Undeafen accepts fresh audio without replaying old buffers");
    lossy.remove(1);
    require(lossy.speakers() == 0, "A departing speaker releases its decoder");

    voice::AudioProcessor bounded;
    bounded.receive(1, 0, 0, {});
    const std::vector<std::uint8_t> oversized(4096, 0);
    bounded.receive(1, 0, 0, oversized);
    require(bounded.speakers() == 0, "Malformed or oversized packets allocate no decoder");
    for (std::uint32_t speaker = 1; speaker <= 64; ++speaker)
      bounded.receive(speaker, 0, 0, packets[0].opus);
    require(bounded.speakers() == 32, "Remote audio retains a bounded number of independent decoders");
    for (int frame = 0; frame < 260; ++frame)
      (void)peak(bounded.playback());
    require(bounded.speakers() == 0, "Idle speakers release their buffers and codec state");
    sender.reset();
    require(
        sender.speakers() == 0
            && sender.inputLevel() == 0
            && sender.outputLevel() == 0
            && sender.capture(tone(0)).timestamp == 0,
        "Reset clears sample timelines, levels and codec buffers"
    );

    require(
        setenv("PULSE_SERVER", "unix:/cressida-voice-test-no-audio-server", 1) == 0,
        "The native failure fixture is isolated from desktop audio"
    );
    auto audio = voice::makeAudio();
    require(
        audio->eventFd() >= 0 && !audio->state().running && !audio->state().testing,
        "Construction creates a pollable worker without opening the microphone"
    );
    audio->refreshDevices();
    await(audio, [](const auto& state) { return !state.error.empty(); });
    require(!audio->state().running && !audio->state().testing, "Missing device enumeration does not start capture");
    std::string error;
    require(audio->startTest(error), "A microphone test is an explicit asynchronous local action");
    await(audio, [](const auto& state) { return !state.loading && !state.error.empty(); });
    audio->stopTest();
    bool captured = false;
    require(
        !audio->start([&](auto) { captured = true; }, error) && !error.empty(),
        "An unavailable audio server reports a bounded connection error"
    );
    audio->stop();
    require(!captured && !audio->state().running, "Failed capture never invokes a network destination");
    audio.reset();
    std::cout << "Voice capture gates, Opus, jitter, mixing, bounds and isolated native lifecycle passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
