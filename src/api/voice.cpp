#include "api/client.h"

#include <algorithm>

namespace api {
  bool Client::joinVoice(const std::string& guild, const std::string& channel, voice::AudioSettings settings) {
    if ((!guild.empty() && !validSnowflake(guild)) || !validSnowflake(channel))
      return false;
    const auto session = m_session.load();
    std::string user;
    {
      std::lock_guard lock(m_mutex);
      if (!session || session != m_authenticatedSession)
        return false;
      user = m_authenticatedUser;
    }
    if (!validSnowflake(user))
      return false;
    std::lock_guard lock(m_voiceMutex);
    if (session != m_session)
      return false;
    const auto revision = ++m_voiceRevision;
    m_streams->reset();
    m_voice->disconnect();
    m_voiceAudio->stopTest();
    m_voiceSettings = voice::sanitizeAudioSettings(std::move(settings));
    m_voiceSettings.pttPressed = false;
    m_voice->configure(m_voiceSettings);
    m_voiceSession.begin(session, revision, guild, channel, user);
    if (!m_gateway->setVoice(session, guild, channel, m_voiceSettings.muted, m_voiceSettings.deafened)) {
      m_voiceSession.clear();
      return false;
    }
    publish(
        Event{
            .operation = Operation::Voice,
            .session = session,
            .voice = voice::State{
                .phase = voice::Phase::Connecting,
                .generation = session,
                .revision = revision,
                .channel = channel,
                .muted = m_voiceSettings.muted,
                .deafened = m_voiceSettings.deafened
            }
        }
    );
    return true;
  }

  voice::State Client::resetVoice(bool notifyGateway, std::uint64_t expectedSession) {
    std::lock_guard lock(m_voiceMutex);
    const auto generation = m_session.load();
    if (expectedSession && expectedSession != generation)
      return {};
    const auto guild = m_voiceSession.guild();
    const bool active = m_voiceSession.active();
    const auto revision = ++m_voiceRevision;
    if (m_streams)
      m_streams->reset(!notifyGateway);
    m_voiceSession.clear();
    if (!notifyGateway)
      m_voiceRoster.clear();
    m_voiceSettings.pttPressed = false;
    if (m_voice)
      m_voice->disconnect();
    if (m_voiceAudio) {
      m_voiceAudio->stopTest();
      m_voiceAudio->stop();
    }
    if (active && notifyGateway)
      m_gateway->setVoice(generation, guild, {}, false, false);
    return {.generation = generation, .revision = revision};
  }
  void Client::leaveVoice(std::uint64_t expectedSession) {
    auto state = resetVoice(true, expectedSession);
    publish(Event{.operation = Operation::Voice, .session = state.generation, .voice = std::move(state)});
  }
  void Client::configureVoice(voice::AudioSettings settings) {
    std::lock_guard lock(m_voiceMutex);
    settings = voice::sanitizeAudioSettings(std::move(settings));
    const bool flagsChanged = settings.muted != m_voiceSettings.muted || settings.deafened != m_voiceSettings.deafened;
    m_voiceSettings = settings;
    settings.muted |= m_voiceSession.muted;
    settings.deafened |= m_voiceSession.deafened;
    m_voiceAudio->configure(settings);
    m_voice->configure(settings);
    m_streams->playback(settings);
    if (flagsChanged && m_voiceSession.active())
      m_gateway->setVoice(
          m_session, m_voiceSession.guild(), m_voiceSession.channel(), m_voiceSettings.muted, m_voiceSettings.deafened
      );
  }
  void Client::voiceEvent(std::uint64_t session, const std::string& type, const nlohmann::json& data) {
    std::lock_guard lock(m_voiceMutex);
    if (session != m_session || session != m_authenticatedSession)
      return;
    const auto channel = m_voiceSession.channel();
    const auto guild = m_voiceSession.guild();
    const auto result = m_voiceSession.receive(type, data);
    if (result == VoiceSession::Update::Ended) {
      m_streams->reset();
      ++m_voiceRevision;
      m_voice->disconnect();
      m_gateway->setVoice(session, guild, {}, false, false);
      publish(
          Event{
              .operation = Operation::Voice,
              .session = session,
              .voice = voice::State{
                  .phase = voice::Phase::Failed,
                  .generation = session,
                  .revision = m_voiceRevision,
                  .channel = channel,
                  .error = "The voice session ended. Join again to reconnect."
              }
          }
      );
      return;
    }
    if (result == VoiceSession::Update::Migrating) {
      m_streams->reset();
      const auto migrationRevision = ++m_voiceRevision;
      m_voiceSession.setRevision(migrationRevision);
      m_voice->disconnect();
      publish(
          Event{
              .operation = Operation::Voice,
              .session = session,
              .voice = voice::State{
                  .phase = voice::Phase::Connecting,
                  .generation = session,
                  .revision = migrationRevision,
                  .channel = channel
              }
          }
      );
      return;
    }
    auto effective = m_voiceSettings;
    effective.muted |= m_voiceSession.muted;
    effective.deafened |= m_voiceSession.deafened;
    m_voiceAudio->configure(effective);
    m_voice->configure(effective);
    m_streams->playback(effective);
    if (result != VoiceSession::Update::Ready)
      return;
    const auto transportRevision = ++m_voiceRevision;
    m_voiceSession.setRevision(transportRevision);
    auto info = m_voiceSession.take();
    m_streams->parent({session, guild, channel, info.user, info.session});
    for (const auto& member : m_voiceRoster.members(guild, channel))
      info.participants.push_back(member.user);
    if (!m_voice->connect(std::move(info)))
      publish(
          Event{
              .operation = Operation::Voice,
              .session = session,
              .voice = voice::State{
                  .phase = voice::Phase::Failed,
                  .generation = session,
                  .revision = transportRevision,
                  .channel = channel,
                  .error = "The voice server supplied invalid connection details."
              }
          }
      );
  }
  void Client::voiceRosterEvent(std::uint64_t session, const std::string& type, const nlohmann::json& data) {
    if (session != m_session || session != m_authenticatedSession)
      return;
    auto update = voiceRosterUpdate(type, data);
    if (!update)
      return;
    bool removedActive = false;
    {
      std::lock_guard lock(m_voiceMutex);
      if (session != m_session)
        return;
      m_voiceRoster.apply(*update);
      m_streams->roster(session,*update);
      removedActive = m_voiceSession.active()
          && (std::ranges::find(update->removedChannels, m_voiceSession.channel()) != update->removedChannels.end()
              || (type == "GUILD_DELETE"
                  && std::ranges::find(update->resetGuilds, m_voiceSession.guild()) != update->resetGuilds.end()));
      if (m_voiceSession.active() && !removedActive) {
        std::vector<std::string> users;
        for (const auto& member : m_voiceRoster.members(m_voiceSession.guild(), m_voiceSession.channel()))
          users.push_back(member.user);
        m_voice->participants(std::move(users));
      }
    }
    if (removedActive)
      leaveVoice(session);
    publish(Event{.operation = Operation::VoiceRoster, .session = session, .voiceRoster = std::move(update)});
  }
} // namespace api
