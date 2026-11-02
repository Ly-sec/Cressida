#pragma once
#include "api/models.h"

namespace api {
  inline constexpr std::size_t maximumInviteCodeBytes = 128;
  inline constexpr std::size_t maximumMessageInvites = 3;
  std::optional<std::string> serverInviteCode(std::string_view text);
  struct ServerInvite {
    std::string code;
    Guild guild;
    std::string banner, description, channel;
  };
  std::optional<ServerInvite> parseServerInvite(const nlohmann::json& value, std::string_view expectedCode);
  struct GuildMembershipChange {
    std::string removed;
    std::optional<Guild> guild;
  };
  std::optional<GuildMembershipChange> guildMembershipChange(std::string_view event, const nlohmann::json& data);
} // namespace api
