#include "api/read_state.h"

#include "api/models.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>

namespace api {
  namespace {
    constexpr std::size_t maxChannels = 100000, maxGuilds = 1000, maxLiveMentions = 4096;
    constexpr int maxMentions = 1000000;
    std::string string(const nlohmann::json& value, const char* key) {
      const auto at = value.find(key);
      return at != value.end() && at->is_string() ? at->get<std::string>() : "";
    }
    bool boolean(const nlohmann::json& value, const char* key) {
      const auto at = value.find(key);
      return at != value.end() && at->is_boolean() && at->get<bool>();
    }
    std::optional<int> integer(const nlohmann::json& value, const char* key) {
      const auto at = value.find(key);
      if (at == value.end() || !at->is_number_integer())
        return {};
      if (at->is_number_unsigned())
        return static_cast<int>(std::min<std::uint64_t>(at->get<std::uint64_t>(), maxMentions));
      return static_cast<int>(std::clamp<std::int64_t>(at->get<std::int64_t>(), -1, maxMentions));
    }
    bool newer(const std::string& a, const std::string& b) {
      return a.size() != b.size() ? a.size() > b.size() : a > b;
    }
  } // namespace

  void ReadState::channels(const nlohmann::json& value, const std::string& guild) {
    if (!value.is_array())
      return;
    for (const auto& channel : value) {
      const auto id = string(channel, "id");
      if (!validSnowflake(id) || (!m_channels.contains(id) && m_channels.size() >= maxChannels))
        continue;
      auto& entry = m_channels[id];
      if (!guild.empty() || entry.guild.empty())
        entry.guild = guild;
      const auto latest = string(channel, "last_message_id");
      if (validSnowflake(latest) && newer(latest, entry.latest))
        entry.latest = latest;
    }
  }

  void ReadState::context(std::string_view type, const nlohmann::json& value, const std::string& self) {
    auto member = [&](const nlohmann::json& item, const std::string& guild) {
      auto id = string(item, "user_id");
      if (item.contains("user"))
        id = string(item["user"], "id");
      if (id != self
          || self.empty()
          || !validSnowflake(guild)
          || (!m_guilds.contains(guild) && m_guilds.size() >= maxGuilds))
        return;
      const auto roles = item.find("roles");
      if (roles == item.end() || !roles->is_array())
        return;
      auto& target = m_guilds[guild].roles;
      target.clear();
      for (const auto& role : *roles)
        if (role.is_string() && validSnowflake(role.get_ref<const std::string&>()) && target.size() < 250)
          target.insert(role.get<std::string>());
    };
    auto settings = [&](const nlohmann::json& item) {
      const auto guild = string(item, "guild_id");
      if (!validSnowflake(guild) || (!m_guilds.contains(guild) && m_guilds.size() >= maxGuilds))
        return;
      auto& target = m_guilds[guild];
      if (item.contains("suppress_roles") && item["suppress_roles"].is_boolean())
        target.suppressRoles = boolean(item, "suppress_roles");
      if (item.contains("suppress_everyone") && item["suppress_everyone"].is_boolean())
        target.suppressEveryone = boolean(item, "suppress_everyone");
    };
    if (type == "READY" || type == "READY_SUPPLEMENTAL" || type == "GUILD_CREATE") {
      auto guild = [&](const nlohmann::json& item) {
        const auto id = string(item, "id");
        if (!validSnowflake(id))
          return;
        if (item.contains("channels"))
          channels(item["channels"], id);
        if (item.contains("threads"))
          channels(item["threads"], id);
        if (item.contains("members") && item["members"].is_array())
          for (const auto& entry : item["members"])
            member(entry, id);
      };
      if (type == "GUILD_CREATE")
        guild(value);
      else if (value.contains("guilds") && value["guilds"].is_array()) {
        const auto& guilds = value["guilds"];
        for (std::size_t i = 0; i < guilds.size(); ++i) {
          guild(guilds[i]);
          if (value.contains("merged_members")
              && value["merged_members"].is_array()
              && i < value["merged_members"].size()
              && value["merged_members"][i].is_array())
            for (const auto& entry : value["merged_members"][i])
              member(entry, string(guilds[i], "id"));
        }
      }
      if (value.contains("user_guild_settings")) {
        const auto& wrapper = value["user_guild_settings"];
        const auto& entries = wrapper.is_object() && wrapper.contains("entries") ? wrapper["entries"] : wrapper;
        if (entries.is_array())
          for (const auto& item : entries)
            settings(item);
      }
      if (type == "READY_SUPPLEMENTAL" && value.contains("lazy_private_channels"))
        channels(value["lazy_private_channels"]);
    } else if (type == "CHANNEL_CREATE" || type == "CHANNEL_UPDATE") {
      const auto guild = string(value, "guild_id");
      if (guild.empty() || validSnowflake(guild))
        channels(nlohmann::json::array({value}), guild);
    } else if (type == "USER_GUILD_SETTINGS_UPDATE")
      settings(value);
    else if (type == "GUILD_MEMBER_UPDATE" || type == "GUILD_MEMBER_ADD")
      member(value, string(value, "guild_id"));
    else if (type == "GUILD_DELETE" && !boolean(value, "unavailable")) {
      const auto guild = string(value, "id");
      if (!validSnowflake(guild))
        return;
      std::erase_if(m_channels, [&](const auto& entry) { return entry.second.guild == guild; });
      m_guilds.erase(guild);
      std::erase_if(m_mentions, [&](const auto& entry) { return !m_channels.contains(entry.second); });
    } else if (type == "CHANNEL_DELETE" || type == "THREAD_DELETE") {
      const auto channel = string(value, "id");
      m_channels.erase(channel);
      std::erase_if(m_mentions, [&](const auto& entry) { return entry.second == channel; });
    }
  }

  void ReadState::ready(const nlohmann::json& value) {
    m_channels.clear();
    m_guilds.clear();
    m_mentions.clear();
    context("READY", value, value.contains("user") ? string(value["user"], "id") : "");
    if (value.contains("private_channels"))
      channels(value["private_channels"]);
    for (auto& [id, entry] : m_channels)
      entry.processed = entry.latest;
    if (!value.contains("read_state"))
      return;
    const auto& state = value["read_state"];
    const auto& entries = state.is_object() && state.contains("entries") ? state["entries"] : state;
    if (!entries.is_array())
      return;
    for (const auto& item : entries) {
      if (!item.is_object() || (item.contains("read_state_type") && integer(item, "read_state_type") != 0))
        continue;
      const auto id = string(item, "id");
      if (!validSnowflake(id) || (!m_channels.contains(id) && m_channels.size() >= maxChannels))
        continue;
      auto& entry = m_channels[id];
      const auto ack = string(item, "last_message_id");
      if (validSnowflake(ack))
        entry.acknowledged = ack;
      entry.mentions = std::max(0, integer(item, "mention_count").value_or(0));
    }
  }

  void ReadState::message(const nlohmann::json& value, const std::string& self, bool blocked, bool muted) {
    if (integer(value, "flags").value_or(0) & 64)
      return; // Private application replies never advance channel read state.
    const auto id = string(value, "channel_id"), latest = string(value, "id");
    if (!validSnowflake(id)
        || !validSnowflake(latest)
        || (!m_channels.contains(id) && m_channels.size() >= maxChannels))
      return;
    auto& entry = m_channels[id];
    if (value.contains("guild_id"))
      entry.guild = string(value, "guild_id");
    // Gateway dispatches are ordered. Replayed/duplicate or already-read messages
    // must not increase a count imported at READY or restored after RESUME.
    const bool fresh = newer(latest, entry.processed) && newer(latest, entry.acknowledged);
    if (newer(latest, entry.processed))
      entry.processed = latest;
    if (newer(latest, entry.latest))
      entry.latest = latest;
    const auto author = value.contains("author") ? string(value["author"], "id") : "";
    if (!self.empty() && author == self) {
      if (fresh && integer(value, "type").value_or(0) != 46) {
        entry.acknowledged = latest;
        entry.mentions = 0;
        std::erase_if(m_mentions, [&](const auto& item) { return item.second == id; });
      }
      return;
    }
    if (!fresh || blocked || self.empty() || !validSnowflake(author))
      return;
    bool mentioned = false;
    if (value.contains("mentions") && value["mentions"].is_array())
      for (const auto& user : value["mentions"])
        mentioned |= string(user, "id") == self;
    const auto guild = m_guilds.find(entry.guild);
    if (!entry.guild.empty()) {
      const bool suppressEveryone = guild != m_guilds.end() && guild->second.suppressEveryone;
      mentioned |= boolean(value, "mention_everyone") && !suppressEveryone;
      if (guild != m_guilds.end()
          && !guild->second.suppressRoles
          && value.contains("mention_roles")
          && value["mention_roles"].is_array())
        for (const auto& role : value["mention_roles"])
          if (role.is_string())
            mentioned |= guild->second.roles.contains(role.get_ref<const std::string&>());
    }
    const bool counted =
        entry.guild.empty() ? integer(value, "type").value_or(0) != 2 && (mentioned || !muted) : mentioned;
    if (counted) {
      entry.mentions = std::min(maxMentions, entry.mentions + 1);
      if (m_mentions.size() >= maxLiveMentions)
        m_mentions.erase(m_mentions.begin());
      m_mentions[latest] = id;
    }
  }

  void ReadState::removed(const nlohmann::json& value) {
    const auto channel = string(value, "channel_id");
    auto remove = [&](const std::string& id) {
      const auto at = m_mentions.find(id);
      if (at == m_mentions.end() || at->second != channel)
        return;
      if (auto entry = m_channels.find(channel); entry != m_channels.end())
        entry->second.mentions = std::max(0, entry->second.mentions - 1);
      m_mentions.erase(at);
    };
    remove(string(value, "id"));
    if (value.contains("ids") && value["ids"].is_array())
      for (const auto& id : value["ids"])
        if (id.is_string())
          remove(id.get_ref<const std::string&>());
  }

  void ReadState::acknowledge(const nlohmann::json& value) {
    if (integer(value, "ack_type").value_or(0) != 0)
      return;
    const auto id = string(value, "channel_id"), message = string(value, "message_id");
    if (!validSnowflake(id)
        || !validSnowflake(message)
        || (!m_channels.contains(id) && m_channels.size() >= maxChannels))
      return;
    auto& entry = m_channels[id];
    const bool manual = boolean(value, "manual");
    if (manual || !newer(entry.acknowledged, message)) {
      entry.acknowledged = message;
      int removed = 0;
      std::erase_if(m_mentions, [&](const auto& item) {
        if (item.second != id || (!manual && newer(item.first, message)))
          return false;
        ++removed;
        return true;
      });
      if (const auto count = integer(value, "mention_count"); count && *count >= 0)
        entry.mentions = *count;
      else if (!entry.latest.empty() && !newer(entry.latest, message))
        entry.mentions = 0;
      else
        entry.mentions = std::max(0, entry.mentions - removed);
    }
  }

  std::map<std::string, std::string> ReadState::unread() const {
    std::map<std::string, std::string> result;
    for (const auto& [id, entry] : m_channels)
      if (entry.mentions > 0 || (!entry.latest.empty() && newer(entry.latest, entry.acknowledged)))
        result[id] = entry.guild;
    return result;
  }
  std::map<std::string, UnreadCursor> ReadState::unreadCursors() const {
    std::map<std::string, UnreadCursor> result;
    for (const auto& [id, entry] : m_channels)
      if (entry.mentions > 0 || (!entry.latest.empty() && newer(entry.latest, entry.acknowledged)))
        result[id] = {
            validSnowflake(entry.acknowledged) ? entry.acknowledged : "",
            validSnowflake(entry.latest) ? entry.latest : "",
            entry.mentions,
            {}
        };
    for (const auto& [message, channel] : m_mentions)
      if (auto at = result.find(channel); at != result.end())
        at->second.mentionMessages.push_back(message);
    return result;
  }
} // namespace api
