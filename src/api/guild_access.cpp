#include "api/guild_access.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace api {
  namespace {
    constexpr std::size_t maximumInviteInputBytes = 2048;
    constexpr int maximumInviteCount = 1000000000;
    std::string text(const nlohmann::json& value, const char* key, std::size_t maximum) {
      const auto at = value.find(key);
      return at != value.end() && at->is_string() ? at->get<std::string>().substr(0, maximum) : "";
    }
  } // namespace
  std::optional<std::string> serverInviteCode(std::string_view value) {
    if (value.size() > maximumInviteInputBytes)
      return {};
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
      value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
      value.remove_suffix(1);
    const bool scheme = value.starts_with("https://") || value.starts_with("http://");
    if (scheme)
      value.remove_prefix(value.starts_with("https://") ? 8 : 7);
    bool url = false;
    for (const auto prefix :
         {"discord.gg/", "www.discord.gg/", "discord.com/invite/", "www.discord.com/invite/", "discordapp.com/invite/",
          "ptb.discord.com/invite/", "canary.discord.com/invite/"})
      if (value.starts_with(prefix)) {
        value.remove_prefix(std::string_view(prefix).size());
        url = true;
        break;
      }
    if (scheme && !url)
      return {};
    if (url) {
      value = value.substr(0, value.find_first_of("?#"));
      if (value.ends_with('/'))
        value.remove_suffix(1);
    }
    if (value.empty() || value.size() > maximumInviteCodeBytes || !std::ranges::all_of(value, [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }))
      return {};
    return std::string(value);
  }
  std::optional<ServerInvite> parseServerInvite(const nlohmann::json& value, std::string_view expectedCode) {
    if (!value.is_object()
        || !serverInviteCode(expectedCode)
        || text(value, "code", maximumInviteCodeBytes + 1) != expectedCode)
      return {};
    const auto type = value.find("type");
    if (type != value.end() && (!type->is_number_integer() || *type != 0))
      return {};
    const auto at = value.find("guild");
    if (at == value.end() || !at->is_object() || !validSnowflake(text(*at, "id", 32)))
      return {};
    auto guilds = parseGuilds(nlohmann::json::array({*at}));
    if (guilds.size() != 1 || guilds.front().name.empty() || guilds.front().name == "Unnamed server")
      return {};
    ServerInvite result{std::string(expectedCode), std::move(guilds.front())};
    const auto count = [&](const char* key, std::optional<int>& target) {
      const auto item = value.find(key);
      if (item != value.end() && item->is_number_integer() && *item >= 0 && *item <= maximumInviteCount)
        target = item->get<int>();
    };
    count("approximate_member_count", result.guild.members);
    count("approximate_presence_count", result.guild.online);
    const auto banner = text(*at, "banner", 128);
    if (validAssetHash(banner))
      result.banner = "/banners/" + result.guild.id + "/" + banner + ".png?size=512";
    result.description = text(*at, "description", maximumInviteInputBytes);
    const auto channel = value.find("channel");
    if (channel != value.end() && channel->is_object()) {
      result.channel = text(*channel, "id", 32);
      if (!validSnowflake(result.channel))
        result.channel.clear();
    }
    return result;
  }
  std::optional<GuildMembershipChange> guildMembershipChange(std::string_view event, const nlohmann::json& data) {
    if ((event != "GUILD_CREATE" && event != "GUILD_DELETE") || !data.is_object())
      return {};
    const auto id = text(data, "id", 32);
    if (!validSnowflake(id))
      return {};
    for (const auto flag : {"unavailable", "lurker"})
      if (const auto at = data.find(flag); at != data.end() && (!at->is_boolean() || at->get<bool>()))
        return {};
    if (event == "GUILD_DELETE")
      return GuildMembershipChange{id, {}};
    auto guilds = parseGuilds(nlohmann::json::array({data}));
    return GuildMembershipChange{"", std::move(guilds.front())};
  }
} // namespace api
