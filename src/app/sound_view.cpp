#include "app/client_application.h"

#include <algorithm>

namespace app {
  void ClientApplication::playSound(SoundEvent event, bool preview) {
    const auto index = static_cast<std::size_t>(event);
    if (m_demo || m_account.id.empty() || index >= soundEvents.size())
      return;
    if (!soundAllowed(
            m_preferences.sounds, m_preferences.notificationSounds, event, m_voiceDeafened, m_presence.status == "dnd",
            preview
        ))
      return;
    m_notificationSounds.play(event, m_preferences.sounds.pack, m_preferences.sounds.volume);
  }
  bool ClientApplication::soundPreference(const std::string& action) {
    if (action.starts_with("sound-pack:")) {
      const auto pack = action.substr(std::string_view("sound-pack:").size());
      if (!validSoundPack(pack)) {
        m_settingsStatus = "That sound set is unavailable.";
        return true;
      }
      m_notificationSounds.clear();
      m_preferences.sounds.pack = pack;
      savePreferences();
      return true;
    }
    const bool preview = action.starts_with("sound-preview:");
    const bool toggle = action.starts_with("sound-toggle:");
    if (!preview && !toggle)
      return false;
    const auto name =
        action.substr(preview ? std::string_view("sound-preview:").size() : std::string_view("sound-toggle:").size());
    for (std::size_t i = 0; i < soundEvents.size(); ++i)
      if (name == soundEvents[i].id) {
        if (preview) {
          m_notificationSounds.clear();
          playSound(static_cast<SoundEvent>(i), true);
          m_settingsStatus = m_voiceDeafened     ? "Unmute your output to preview sounds."
              : m_preferences.sounds.volume == 0 ? "Increase sound volume to preview sounds."
                                                 : "Playing sound preview.";
        } else {
          m_notificationSounds.clear();
          m_preferences.sounds.enabled[i] = !m_preferences.sounds.enabled[i];
          savePreferences();
        }
        return true;
      }
    m_settingsStatus = "That sound event is unavailable.";
    return true;
  }
  void ClientApplication::voiceSoundState(const voice::State& state) {
    if (const auto event = m_soundLifecycle.voice(state))
      playSound(*event);
  }
  void ClientApplication::voiceRosterSounds(const api::VoiceRosterUpdate& update) {
    const auto before = m_voiceRoster.members(m_voiceGuild, m_voiceState.channel);
    m_voiceRoster.apply(update);
    if (update.reset
        || !update.resetGuilds.empty()
        || !update.removedChannels.empty()
        || m_soundLifecycle.channel.empty()
        || m_soundLifecycle.channel != m_voiceState.channel)
      return;
    const auto after = m_voiceRoster.members(m_voiceGuild, m_voiceState.channel);
    const auto eligible = [&](const api::VoiceMember& member) {
      return member.user != m_account.id
          && m_userDirectory.relationship(member.user) != 2
          && !m_userDirectory.ignored(member.user);
    };
    if (std::ranges::any_of(after, [&](const auto& member) {
          return eligible(member) && std::ranges::find(before, member.user, &api::VoiceMember::user) == before.end();
        }))
      playSound(SoundEvent::ParticipantJoin);
    if (std::ranges::any_of(before, [&](const auto& member) {
          return eligible(member) && std::ranges::find(after, member.user, &api::VoiceMember::user) == after.end();
        }))
      playSound(SoundEvent::ParticipantLeave);
  }
  void ClientApplication::sharingSounds(const api::StreamSnapshot& snapshot) {
    const bool ready = std::ranges::any_of(snapshot.active, [&](const auto& stream) {
      if (!stream.publishing || stream.connection.phase == voice::Phase::Failed)
        return false;
      const bool continuing =
          m_soundLifecycle.sharing && std::ranges::any_of(m_streamSnapshot.active, [&](const auto& old) {
            return old.publishing && old.stream.key == stream.stream.key;
          });
      return stream.connection.phase == voice::Phase::Connected || continuing;
    });
    const bool failed = std::ranges::any_of(snapshot.active, [&](const auto& stream) {
      if (!stream.publishing || stream.connection.phase != voice::Phase::Failed)
        return false;
      return std::ranges::none_of(m_streamSnapshot.active, [&](const auto& old) {
        return old.stream.key == stream.stream.key && old.connection.phase == voice::Phase::Failed;
      });
    });
    if (const auto event = m_soundLifecycle.stream(ready))
      playSound(*event);
    if (failed)
      playSound(SoundEvent::Error);
  }
} // namespace app
