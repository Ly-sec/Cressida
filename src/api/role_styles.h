#pragma once

#include <cstdint>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace api {
  struct Channel;
  struct RoleColors {
    std::uint32_t primary = 0;
    std::optional<std::uint32_t> secondary, tertiary;
    bool operator==(const RoleColors&) const = default;
  };
  struct ColoredRole {
    std::string id;
    int position = 0;
    RoleColors colors;
    std::string name;
    std::string icon, unicodeEmoji;
    std::optional<std::string> permissions;
  };
  // Display-name metadata from a member payload. An absent `nick` field leaves
  // the stored nickname unchanged; an explicit null or empty string clears it.
  struct MemberName {
    std::optional<std::string> nick;
    bool nickPresent = false;
    std::string user;
    bool operator==(const MemberName&) const = default;
  };
  struct GuildRoleUpdate {
    std::string guild;
    bool removed = false, replaceRoles = false;
    std::optional<bool> enhanced;
    std::vector<ColoredRole> roles;
    std::vector<std::string> removedRoles, removedMembers;
    std::map<std::string, std::vector<std::string>> members;
    std::map<std::string, MemberName> names;
    std::optional<std::string> owner;
  };
  struct RoleUpdates {
    bool reset = false;
    std::vector<GuildRoleUpdate> guilds;
  };
  RoleUpdates parseRoleUpdates(std::string_view type, const nlohmann::json& data);
  std::optional<std::vector<std::string>> parseMemberRoles(const nlohmann::json& member);

  // Application-owned, guild-scoped role/membership cache. Raw Gateway payloads
  // are projected on the worker; no rendering types cross the backend boundary.
  class RoleDirectory {
  public:
    void apply(const RoleUpdates& updates);
    std::vector<ColoredRole> allRoles(const std::string& guild) const;
    std::string roleName(const std::string& guild, const std::string& role) const;
    std::optional<std::vector<std::string>> memberships(const std::string& guild, const std::string& user) const;
    std::string owner(const std::string& guild) const;
    // The member's current server nickname, or their account name once the
    // server has reported that no nickname is set. Empty when unknown.
    std::optional<std::string> memberName(const std::string& guild, const std::string& user) const;
    std::optional<RoleColors> colors(
        const std::string& guild, const std::string& user,
        const std::optional<std::vector<std::string>>& fallbackRoles = {}
    ) const;
    std::vector<ColoredRole> roles(
        const std::string& guild, const std::string& user,
        const std::optional<std::vector<std::string>>& fallbackRoles = {}
    ) const;
    std::optional<ColoredRole> icon(
        const std::string& guild, const std::string& user,
        const std::optional<std::vector<std::string>>& fallbackRoles = {}
    ) const;

  private:
    struct GuildRoles {
      bool enhanced = true;
      std::string owner;
      std::map<std::string, ColoredRole> roles;
      std::map<std::string, std::vector<std::string>> members;
      std::map<std::string, MemberName> names;
    };
    std::map<std::string, GuildRoles> m_guilds;
    std::size_t m_memberCount = 0, m_nameCount = 0;
  };
} // namespace api
