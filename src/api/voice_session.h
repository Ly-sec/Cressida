#pragma once

#include "voice/state.h"

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace api {
  enum class GatewayVoiceAction { Ignore, Preserve, Reset };
  constexpr GatewayVoiceAction gatewayVoiceAction(std::string_view type) {
    if (type == "RECONNECTING" || type == "RESUMED")
      return GatewayVoiceAction::Preserve;
    if (type == "READY" || type == "CONNECTION_FAILED")
      return GatewayVoiceAction::Reset;
    return GatewayVoiceAction::Ignore;
  }

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

    Update receive(const std::string& type, const nlohmann::json& data);
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
