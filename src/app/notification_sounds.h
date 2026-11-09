#pragma once

#include "voice/connection.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace app {
  enum class SoundEvent {
    Message,
    Mention,
    MicMute,
    MicUnmute,
    OutputMute,
    OutputUnmute,
    VoiceJoin,
    VoiceLeave,
    ParticipantJoin,
    ParticipantLeave,
    SharingStart,
    SharingStop,
    Error,
    Warning,
    Count
  };
  inline constexpr std::array<std::string_view, 3> soundPacks{"soft", "crisp", "warm"};
  inline constexpr int defaultSoundVolume = 50, maximumSoundVolume = 100;
  struct SoundDescription {
    std::string_view id, label, description;
  };
  inline constexpr std::array<SoundDescription, static_cast<std::size_t>(SoundEvent::Count)> soundEvents{{
      {"message", "Messages", "Eligible new-message notifications and due reminders."},
      {"mention", "Mentions", "Eligible notifications mentioning you, your roles or everyone."},
      {"mic-mute", "Microphone mute", "When you mute your microphone, including through a voice command."},
      {"mic-unmute", "Microphone unmute", "When you unmute your microphone."},
      {"output-mute", "Output mute", "When you deafen yourself."},
      {"output-unmute", "Output unmute", "When you stop deafening yourself."},
      {"voice-join", "Voice join", "When your voice connection becomes ready."},
      {"voice-leave", "Voice leave", "When you leave or lose an established voice connection."},
      {"participant-join", "Participant join", "When another visible participant joins your current call."},
      {"participant-leave", "Participant leave", "When another visible participant leaves your current call."},
      {"sharing-start", "Screen sharing start", "When your stream connection becomes ready, not when capture starts."},
      {"sharing-stop", "Screen sharing stop", "When your previously live screen share ends."},
      {"error", "Serious errors", "When a voice connection or active screen-share connection fails."},
      {"warning", "Serious warnings", "When an audio device failure interrupts your call."},
  }};
  struct SoundSettings {
    std::string pack = "soft";
    int volume = defaultSoundVolume;
    std::array<bool, soundEvents.size()> enabled;
    SoundSettings() { enabled.fill(true); }
  };
  inline bool soundAllowed(
      const SoundSettings& settings, bool master, SoundEvent event, bool deafened, bool doNotDisturb,
      bool preview = false
  ) {
    const auto index = static_cast<std::size_t>(event);
    if (index >= soundEvents.size())
      return false;
    if (settings.volume == 0 || (!preview && (!master || !settings.enabled[index])))
      return false;
    if (deafened && event != SoundEvent::OutputMute && event != SoundEvent::OutputUnmute)
      return false;
    const bool unsolicited = event == SoundEvent::Message
        || event == SoundEvent::Mention
        || event == SoundEvent::ParticipantJoin
        || event == SoundEvent::ParticipantLeave;
    return preview || !unsolicited || !doNotDisturb;
  }
  inline bool validSoundPack(std::string_view pack) {
    for (const auto candidate : soundPacks)
      if (candidate == pack)
        return true;
    return false;
  }
  struct SoundLifecycle {
    std::string channel;
    bool sharing = false, failed = false;
    std::optional<SoundEvent> voice(const voice::State& state) {
      if (state.phase == voice::Phase::Failed) {
        channel.clear();
        return std::exchange(failed, true) ? std::nullopt : std::optional(SoundEvent::Error);
      }
      failed = false;
      if (state.phase == voice::Phase::Disconnected) {
        const bool joined = !channel.empty();
        channel.clear();
        return joined ? std::optional(SoundEvent::VoiceLeave) : std::nullopt;
      }
      if ((state.phase == voice::Phase::Connected || state.phase == voice::Phase::WaitingForParticipants)
          && channel != state.channel) {
        channel = state.channel;
        return SoundEvent::VoiceJoin;
      }
      return {};
    }
    std::optional<SoundEvent> stream(bool ready) {
      if (sharing == ready)
        return {};
      sharing = ready;
      return ready ? SoundEvent::SharingStart : SoundEvent::SharingStop;
    }
  };
  class NotificationSounds {
  public:
    struct Result {
      SoundEvent event;
      std::string error;
    };
    explicit NotificationSounds(bool silent = false);
    ~NotificationSounds();
    NotificationSounds(const NotificationSounds&) = delete;
    NotificationSounds& operator=(const NotificationSounds&) = delete;
    void play(SoundEvent event, std::string pack, int volume = defaultSoundVolume);
    void clear();
    int eventFd() const;
    std::optional<Result> pollResult();

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };
} // namespace app
