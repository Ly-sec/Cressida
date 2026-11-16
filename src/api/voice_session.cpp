#include "api/voice_session.h"

#include "api/models.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace api {
  VoiceSession::Update VoiceSession::receive(const std::string& type, const nlohmann::json& data) {
    if (!active() || !data.is_object())
      return Update::None;
    const auto text = [&](const char* field, std::size_t maximum) {
      const auto at = data.find(field);
      if (at == data.end() || !at->is_string())
        return std::string{};
      const auto& value = at->get_ref<const std::string&>();
      return value.size() <= maximum
              && std::ranges::none_of(value, [](unsigned char ch) { return ch < ' ' || ch == 127; })
          ? value
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
      if (m_started && session != m_info.session) {
        clear();
        return Update::Ended;
      }
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
    return !m_info.session.empty() && !m_info.token.empty() && !m_info.endpoint.empty() ? Update::Ready : Update::None;
  }
} // namespace api
