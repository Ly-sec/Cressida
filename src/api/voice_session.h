#pragma once

#include "api/models.h"
#include "voice/connection.h"

#include <nlohmann/json.hpp>
#include <optional>

namespace api {
  // Private signaling credentials remain scoped to one explicit join intention.
  class VoiceSession {
  public:
    enum class Update { None, Ready, Ended, Migrating };
    void
    begin(std::uint64_t generation, std::uint64_t revision, std::string guild, std::string channel, std::string user) {
      clear();
      m_info = {
          .server = guild.empty() ? channel : guild,
          .user = std::move(user),
          .channel = std::move(channel),
          .generation = generation,
          .revision = revision
      };
      m_guild = std::move(guild);
    }
    void clear() {
      m_info = {};
      m_guild.clear();
      m_started = false;
      muted = deafened = false;
    }
    bool active() const { return !m_info.channel.empty(); }
    const std::string& guild() const { return m_guild; }
    const std::string& channel() const { return m_info.channel; }
    std::uint64_t revision() const { return m_info.revision; }
    void setRevision(std::uint64_t revision) { m_info.revision = revision; }
    bool muted = false, deafened = false;

    Update receive(const std::string& type, const nlohmann::json& data) {
      if (!active() || !data.is_object())
        return Update::None;
      const auto text = [&](const char* field, std::size_t maximum) {
        const auto at = data.find(field);
        return at != data.end() && at->is_string() && at->get_ref<const std::string&>().size() <= maximum
            ? at->get<std::string>()
            : std::string{};
      };
      const auto flag = [&](const char* field) {
        const auto at = data.find(field);
        return at != data.end() && at->is_boolean() && at->get<bool>();
      };
      constexpr std::size_t maximumIdentityBytes = 128, maximumTokenBytes = 4096, maximumEndpointBytes = 256;
      if (type == "VOICE_STATE_UPDATE") {
        if (text("user_id", maximumIdentityBytes) != m_info.user || text("guild_id", maximumIdentityBytes) != m_guild)
          return Update::None;
        const auto channel = text("channel_id", maximumIdentityBytes);
        if (channel != m_info.channel) {
          if ((m_started || !m_info.session.empty()) && data.contains("channel_id") && data["channel_id"].is_null()) {
            clear();
            return Update::Ended;
          }
          if (m_started && validSnowflake(channel)) {
            clear();
            return Update::Ended;
          }
          return Update::None;
        }
        const auto session = text("session_id", maximumIdentityBytes);
        if (session.empty())
          return Update::None;
        muted = flag("mute") || flag("suppress");
        deafened = flag("deaf");
        m_info.session = session;
      } else if (type == "VOICE_SERVER_UPDATE") {
        const auto target = text(m_guild.empty() ? "channel_id" : "guild_id", maximumIdentityBytes);
        if (target != m_info.server)
          return Update::None;
        if (data.contains("endpoint") && data["endpoint"].is_null()) {
          m_info.token.clear();
          m_info.endpoint.clear();
          return Update::Migrating;
        }
        const auto endpoint = text("endpoint", maximumEndpointBytes), token = text("token", maximumTokenBytes);
        if (endpoint.empty() || token.empty())
          return Update::None;
        m_info.endpoint = endpoint;
        m_info.token = token;
      } else
        return Update::None;
      return !m_info.session.empty() && !m_info.token.empty() && !m_info.endpoint.empty() ? Update::Ready
                                                                                          : Update::None;
    }
    voice::ConnectionInfo take() {
      auto result = m_info;
      m_info.token.clear();
      m_info.endpoint.clear();
      m_started = true;
      return result;
    }

  private:
    voice::ConnectionInfo m_info;
    std::string m_guild;
    bool m_started = false;
  };
} // namespace api
