#include "app/notification_sounds.h"
#include "core/files/resource_paths.h"
#include "media/video_player.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  app::NotificationSounds::Result wait(app::NotificationSounds& player) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (auto result = player.pollResult())
        return *result;
      pollfd fd{player.eventFd(), POLLIN, 0};
      require(::poll(&fd, 1, 100) >= 0, "Sound wake polling succeeds");
    }
    throw std::runtime_error("Sound worker did not finish within its deadline");
  }
} // namespace
int main() {
  try {
    using app::SoundEvent;
    static_assert(app::defaultSoundVolume == 50);
    app::SoundSettings settings;
    require(settings.pack == "soft" && settings.volume == app::defaultSoundVolume, "Soft starts at quiet volume");
    settings.volume = 0;
    require(
        !app::soundAllowed(settings, true, SoundEvent::Message, false, false, true),
        "Zero volume also silences explicit previews"
    );
    settings.volume = app::defaultSoundVolume;
    require(
        app::validSoundPack("warm")
            && app::validSoundPack("crisp")
            && !app::validSoundPack("../soft")
            && !app::validSoundPack("Soft"),
        "Sound set names are fixed"
    );
    for (std::size_t i = 0; i < app::soundEvents.size(); ++i) {
      const auto event = static_cast<SoundEvent>(i);
      require(app::soundAllowed(settings, true, event, false, false), "Every sound event defaults on");
      settings.enabled[i] = false;
      require(!app::soundAllowed(settings, true, event, false, false), "Every event can be disabled independently");
      require(
          app::soundAllowed(settings, false, event, false, true, true),
          "Explicit previews bypass sound switches and DND"
      );
      settings.enabled[i] = true;
      require(!app::soundAllowed(settings, false, event, false, false), "The master switch suppresses every event");
    }
    require(
        !app::soundAllowed(settings, true, SoundEvent::Message, false, true)
            && !app::soundAllowed(settings, true, SoundEvent::ParticipantJoin, false, true)
            && app::soundAllowed(settings, true, SoundEvent::MicMute, false, true),
        "DND suppresses unsolicited cues only"
    );
    require(
        !app::soundAllowed(settings, true, SoundEvent::MicMute, true, false)
            && app::soundAllowed(settings, true, SoundEvent::OutputMute, true, false)
            && app::soundAllowed(settings, true, SoundEvent::OutputUnmute, true, false),
        "Deafening suppresses other cues but retains output feedback"
    );
    app::SoundLifecycle lifecycle;
    voice::State state;
    require(!lifecycle.voice(state), "Initial disconnection is silent");
    state.channel = "123";
    state.phase = voice::Phase::Connecting;
    require(!lifecycle.voice(state), "Queued joins are silent");
    state.phase = voice::Phase::Securing;
    require(!lifecycle.voice(state), "Key setup is not a ready call");
    state.phase = voice::Phase::WaitingForParticipants;
    require(lifecycle.voice(state) == SoundEvent::VoiceJoin, "Ready empty calls sound once");
    state.phase = voice::Phase::Connected;
    require(!lifecycle.voice(state), "Another participant does not repeat the self join");
    state.phase = voice::Phase::Securing;
    require(!lifecycle.voice(state), "Rekeying preserves established call identity");
    state.phase = voice::Phase::Connected;
    require(!lifecycle.voice(state), "Rekey completion does not repeat the self join");
    state.phase = voice::Phase::Failed;
    require(lifecycle.voice(state) == SoundEvent::Error && !lifecycle.voice(state), "A call failure sounds only once");
    state = {};
    require(!lifecycle.voice(state), "Failure cleanup does not add a leave cue");
    state.channel = "456";
    state.phase = voice::Phase::Connected;
    require(lifecycle.voice(state) == SoundEvent::VoiceJoin, "Explicit rejoin can sound again");
    state = {};
    require(
        lifecycle.voice(state) == SoundEvent::VoiceLeave && !lifecycle.voice(state), "An established call leaves once"
    );
    require(
        !lifecycle.stream(false)
            && lifecycle.stream(true) == SoundEvent::SharingStart
            && !lifecycle.stream(true)
            && lifecycle.stream(false) == SoundEvent::SharingStop
            && !lifecycle.stream(false),
        "Only actual sharing readiness and stop transitions sound"
    );
    std::set<std::vector<std::uint8_t>> unique;
    for (const auto pack : app::soundPacks)
      for (std::size_t i = 0; i < app::soundEvents.size(); ++i) {
        const auto path =
            paths::assetPath("sounds/" + std::string(pack) + "/" + std::string(app::soundEvents[i].id) + ".ogg");
        std::ifstream input(path, std::ios::binary);
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
        require(
            bytes.size() > 4 && bytes.size() <= 128 * 1024 && std::string(bytes.begin(), bytes.begin() + 4) == "OggS",
            "Each packaged cue is bounded Ogg audio"
        );
        require(unique.insert(bytes).second, "All 42 cue assets are distinct");
        app::NotificationSounds player(true);
        player.play(static_cast<SoundEvent>(i), std::string(pack));
        const auto result = wait(player);
        require(
            result.error.empty() && result.event == static_cast<SoundEvent>(i),
            "Every cue decodes and plays through the null sink"
        );
      }
    app::NotificationSounds player(true);
    player.play(SoundEvent::Message, "../soft");
    require(!wait(player).error.empty(), "Invalid sound requests report errors");
    player.play(SoundEvent::Message, "soft", -1);
    require(!wait(player).error.empty(), "Negative sound volume reports an error");
    player.play(SoundEvent::Message, "soft", app::maximumSoundVolume + 1);
    require(!wait(player).error.empty(), "Excessive sound volume reports an error");
    player.play(SoundEvent::MicMute, "soft", 0);
    require(wait(player).error.empty(), "Zero-volume worker requests finish normally");
    player.play(SoundEvent::Message, "soft");
    player.clear();
    pollfd fd{player.eventFd(), POLLIN, 0};
    require(::poll(&fd, 1, 200) == 0 && !player.pollResult(), "Clearing rejects queued and in-flight completions");
    player.play(SoundEvent::Mention, "crisp");
    require(wait(player).error.empty(), "A clear does not prevent subsequent playback");
    const auto shutdown = std::chrono::steady_clock::now();
    {
      app::NotificationSounds pending(true);
      for (int i = 0; i < 50; ++i)
        pending.play(i % 2 ? SoundEvent::MicMute : SoundEvent::MicUnmute, "warm");
    }
    require(std::chrono::steady_clock::now() - shutdown < 5s, "Burst shutdown cancels promptly");
    std::cout << "Sound catalog, policies, lifecycle and null-audio playback passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
