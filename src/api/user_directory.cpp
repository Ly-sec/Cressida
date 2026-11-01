#include "api/user_directory.h"

#include <charconv>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace api {
  namespace {
    constexpr std::size_t maxUsers = 100000;
    constexpr std::size_t maximumActivities = 64, maximumStatusBytes = 512, maximumStatusEmojiBytes = 128;
    constexpr std::size_t maximumPresenceGuilds = 1000;
    constexpr int customActivityType = 4;
    std::string text(const nlohmann::json& value, const char* key) {
      const auto field = value.find(key);
      return field != value.end() && field->is_string() ? field->get<std::string>() : std::string{};
    }
  } // namespace
  std::optional<std::chrono::system_clock::time_point> muteDeadline(std::string_view text) {
    if (text.size() < 20 || text.size() > 64)
      return {};
    std::tm utc{};
    std::istringstream input(std::string(text.substr(0, 19)));
    input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%S");
    if (input.fail())
      return {};
    std::size_t zone = 19;
    if (text[zone] == '.') {
      ++zone;
      while (zone < text.size() && text[zone] >= '0' && text[zone] <= '9')
        ++zone;
    }
    int offset = 0;
    if (text.substr(zone) != "Z") {
      if (text.size() - zone != 6 || (text[zone] != '+' && text[zone] != '-') || text[zone + 3] != ':')
        return {};
      int hours = 0, minutes = 0;
      const auto a = std::from_chars(text.data() + zone + 1, text.data() + zone + 3, hours);
      const auto b = std::from_chars(text.data() + zone + 4, text.data() + zone + 6, minutes);
      if (a.ec != std::errc{}
          || b.ec != std::errc{}
          || a.ptr != text.data() + zone + 3
          || b.ptr != text.data() + zone + 6
          || hours > 23
          || minutes > 59)
        return {};
      offset = (hours * 3600 + minutes * 60) * (text[zone] == '-' ? -1 : 1);
    }
    return std::chrono::system_clock::from_time_t(timegm(&utc) - offset);
  }
  std::map<std::string, ChannelNotification> channelNotifications(const nlohmann::json& settings) {
    std::map<std::string, ChannelNotification> result;
    const auto list = settings.find("channel_overrides");
    if (list == settings.end() || !list->is_array())
      return result;
    for (const auto& row : *list) {
      const auto id = text(row, "channel_id");
      if (!validSnowflake(id) || result.size() >= maxUsers)
        continue;
      ChannelNotification update;
      if (const auto at = row.find("flags"); at != row.end() && at->is_number_unsigned())
        update.flags = at->get<std::uint64_t>();
      else if (at != row.end() && at->is_number_integer() && *at >= 0)
        update.flags = at->get<std::uint64_t>();
      if (const auto at = row.find("message_notifications");
          at != row.end() && at->is_number_integer() && *at >= 0 && *at <= 3)
        update.level = at->get<int>();
      if (const auto at = row.find("muted"); at != row.end() && at->is_boolean())
        update.muted = at->get<bool>();
      if (const auto config = row.find("mute_config"); config != row.end()) {
        if (config->is_null())
          update.muteUntil = "";
        else if (config->is_object() && config->contains("end_time")) {
          const auto& end = (*config)["end_time"];
          if (end.is_null())
            update.muteUntil = "";
          else if (end.is_string() && muteDeadline(end.get<std::string>()))
            update.muteUntil = end.get<std::string>();
        }
      }
      result.emplace(id, std::move(update));
    }
    return result;
  }

  void UserDirectory::apply(const UserUpdates& update) {
    if (update.reset || update.relationshipsReset) {
      m_names.clear();
      m_relationships.clear();
      m_ignored.clear();
      ++m_relationshipRevision;
    }
    if (update.reset) {
      m_accounts.clear();
      m_cosmetics.clear();
      m_guildCosmetics.clear();
      m_statuses.clear();
      m_customStatuses.clear();
      m_activities.clear();
      m_relationships.clear();
      m_mutedChannels.clear();
      m_notifications.clear();
      m_notificationRevisions.clear();
      m_guildNotifications.clear();
      m_guildNotificationRevisions.clear();
      m_tags.clear();
    }
    for (const auto& [id, user] : update.users) {
      if (!validSnowflake(id))
        continue;
      if ((user.cosmetics.decoration || user.cosmetics.nameplate)
          && (m_cosmetics.contains(id) || m_cosmetics.size() < maxUsers))
        mergeCosmetics(m_cosmetics[id], user.cosmetics);
      if (user.account && (m_accounts.contains(id) || m_accounts.size() < maxUsers))
        m_accounts[id] = *user.account;
      if (user.nickname || user.relationship || user.ignored)
        ++m_relationshipRevision;
      if (user.ignored && (m_ignored.contains(id) || m_ignored.size() < maxUsers)) {
        if (*user.ignored)
          m_ignored[id] = true;
        else
          m_ignored.erase(id);
      }
      if (user.nickname) {
        if (user.nickname->empty())
          m_names.erase(id);
        else if (m_names.contains(id) || m_names.size() < maxUsers)
          m_names[id] = *user.nickname;
      }
      if (user.status && (m_statuses.contains(id) || m_statuses.size() < maxUsers))
        m_statuses[id] = *user.status;
      if (user.customStatus && (m_customStatuses.contains(id) || m_customStatuses.size() < maxUsers))
        m_customStatuses[id] = *user.customStatus;
      if (user.activities && (m_activities.contains(id) || m_activities.size() < maxUsers))
        m_activities[id] = *user.activities;
      if (user.relationship && (m_relationships.contains(id) || m_relationships.size() < maxUsers))
        m_relationships[id] = *user.relationship;
      if (user.guildTag && (m_tags.contains(id) || m_tags.size() < maxUsers))
        m_tags[id] = *user.guildTag;
    }
    for (const auto& [guild, users] : update.guildCosmetics) {
      if (!validSnowflake(guild)
          || (!m_guildCosmetics.contains(guild) && m_guildCosmetics.size() >= maximumPresenceGuilds))
        continue;
      auto& target = m_guildCosmetics[guild];
      for (const auto& [id, patch] : users)
        if (validSnowflake(id) && (target.contains(id) || target.size() < maxUsers))
          mergeCosmetics(target[id], patch);
    }
    for (const auto& [id, muted] : update.mutedChannels)
      if (validSnowflake(id) && (m_mutedChannels.contains(id) || m_mutedChannels.size() < maxUsers))
        m_mutedChannels[id] = muted;
    for (const auto& [id, patch] : update.notifications) {
      if (!validSnowflake(id) || (!m_notifications.contains(id) && m_notifications.size() >= maxUsers))
        continue;
      auto& target = m_notifications[id];
      if (patch.flags)
        target.flags = patch.flags;
      if (patch.level)
        target.level = patch.level;
      if (patch.muted) {
        target.muted = patch.muted;
        m_mutedChannels[id] = *patch.muted;
      }
      if (patch.muteUntil)
        target.muteUntil = patch.muteUntil;
      m_notificationRevisions[id] = ++m_notificationSerial;
    }
    for (const auto& [id, patch] : update.guildNotifications) {
      if (!validSnowflake(id) || (!m_guildNotifications.contains(id) && m_guildNotifications.size() >= 1000))
        continue;
      auto& target = m_guildNotifications[id];
      const auto merge = [](auto& value, const auto& change) {
        if (change)
          value = change;
      };
      merge(target.muted, patch.muted);
      merge(target.suppressEveryone, patch.suppressEveryone);
      merge(target.suppressRoles, patch.suppressRoles);
      merge(target.mobilePush, patch.mobilePush);
      merge(target.hideMutedChannels, patch.hideMutedChannels);
      merge(target.muteScheduledEvents, patch.muteScheduledEvents);
      merge(target.muteUntil, patch.muteUntil);
      merge(target.level, patch.level);
      merge(target.notifyHighlights, patch.notifyHighlights);
      merge(target.flags, patch.flags);
      m_guildNotificationRevisions[id] = ++m_notificationSerial;
    }
  }

  std::string UserDirectory::name(const std::string& user, const std::string& fallback) const {
    const auto found = m_names.find(user);
    return found == m_names.end() ? fallback : found->second;
  }
  std::string UserDirectory::status(const std::string& user, const std::string& fallback) const {
    const auto found = m_statuses.find(user);
    return found == m_statuses.end() ? fallback : found->second;
  }
  UserCustomStatus UserDirectory::customStatus(const std::string& user) const {
    const auto at = m_customStatuses.find(user);
    return at == m_customStatuses.end() ? UserCustomStatus{} : at->second;
  }
  std::vector<rpc::Activity> UserDirectory::activities(const std::string& user) const {
    const auto at = m_activities.find(user);
    return at == m_activities.end() ? std::vector<rpc::Activity>{} : at->second;
  }
  int UserDirectory::relationship(const std::string& user) const {
    const auto found = m_relationships.find(user);
    return found == m_relationships.end() ? 0 : found->second;
  }
  bool UserDirectory::ignored(const std::string& user) const {
    const auto found = m_ignored.find(user);
    return found != m_ignored.end() && found->second;
  }
  bool UserDirectory::muted(const std::string& channel) const {
    if (const auto guild = m_guildNotifications.find(channel); guild != m_guildNotifications.end()) {
      if (!guild->second.muted.value_or(false))
        return false;
      if (guild->second.muteUntil)
        if (const auto end = muteDeadline(*guild->second.muteUntil); end && *end <= std::chrono::system_clock::now())
          return false;
      return true;
    }
    const auto found = m_mutedChannels.find(channel);
    if (found == m_mutedChannels.end() || !found->second)
      return false;
    const auto at = m_notifications.find(channel);
    if (at != m_notifications.end() && at->second.muteUntil)
      if (const auto end = muteDeadline(*at->second.muteUntil); end && *end <= std::chrono::system_clock::now())
        return false;
    return true;
  }
  std::uint64_t UserDirectory::notificationFlags(const std::string& channel) const {
    const auto at = m_notifications.find(channel);
    return at == m_notifications.end() ? 0 : at->second.flags.value_or(0);
  }
  int UserDirectory::notificationLevel(const std::string& channel) const {
    if (const auto guild = m_guildNotifications.find(channel); guild != m_guildNotifications.end())
      return guild->second.level.value_or(3);
    const auto at = m_notifications.find(channel);
    return at == m_notifications.end() ? 3 : at->second.level.value_or(3);
  }
  std::uint64_t UserDirectory::notificationRevision(const std::string& channel) const {
    const auto at = m_notificationRevisions.find(channel);
    return at == m_notificationRevisions.end() ? 0 : at->second;
  }
  GuildNotification UserDirectory::guildPreferences(const std::string& guild) const {
    const auto at = m_guildNotifications.find(guild);
    return at == m_guildNotifications.end() ? GuildNotification{} : at->second;
  }
  std::uint64_t UserDirectory::guildNotificationRevision(const std::string& guild) const {
    const auto at = m_guildNotificationRevisions.find(guild);
    return at == m_guildNotificationRevisions.end() ? 0 : at->second;
  }
  std::optional<std::chrono::system_clock::time_point> UserDirectory::nextMuteExpiry() const {
    std::optional<std::chrono::system_clock::time_point> result;
    const auto now = std::chrono::system_clock::now();
    for (const auto& [id, value] : m_notifications)
      if (muted(id) && value.muteUntil)
        if (const auto end = muteDeadline(*value.muteUntil); end && *end > now && (!result || *end < *result))
          result = end;
    for (const auto& [id, value] : m_guildNotifications)
      if (muted(id) && value.muteUntil)
        if (const auto end = muteDeadline(*value.muteUntil); end && *end > now && (!result || *end < *result))
          result = end;
    return result;
  }
  std::optional<GuildTag>
  UserDirectory::guildTag(const std::string& user, const std::optional<GuildTag>& fallback) const {
    const auto at = m_tags.find(user);
    return at == m_tags.end() ? fallback : at->second;
  }

  UserUpdates UserDirectory::consume(std::string_view type, const nlohmann::json& data) {
    UserUpdates changes;
    if (!data.is_object())
      return changes;
    if (type == "READY") {
      changes.reset = true;
      m_globalStatuses.clear();
      m_guildStatuses.clear();
      m_globalCustomStatuses.clear();
      m_guildCustomStatuses.clear();
      m_globalActivities.clear();
      m_guildActivities.clear();
    }
    auto recordTag = [&](const nlohmann::json& user) {
      const auto id = text(user, "id");
      if (validSnowflake(id) && (changes.users.contains(id) || changes.users.size() < maxUsers))
        mergeCosmetics(changes.users[id].cosmetics, parseUserCosmetics(user));
      if (validSnowflake(id)
          && user.contains("username")
          && (changes.users.size() < maxUsers || changes.users.contains(id)))
        changes.users[id].account = parseAccount(user);
      const auto at = user.find(user.contains("primary_guild") ? "primary_guild" : "clan");
      if (!validSnowflake(id) || at == user.end() || (changes.users.size() >= maxUsers && !changes.users.contains(id)))
        return;
      if (at->is_null() || (at->is_object() && at->contains("identity_enabled") && (*at)["identity_enabled"] == false))
        changes.users[id].guildTag.emplace(std::nullopt);
      else if (const auto tag = parseGuildTag(user))
        changes.users[id].guildTag.emplace(tag);
    };
    if (type == "USER_UPDATE")
      recordTag(data);
    if (data.contains("user"))
      recordTag(data["user"]);
    if (data.contains("author"))
      recordTag(data["author"]);
    if ((type == "READY" || type == "READY_SUPPLEMENTAL") && data.contains("users") && data["users"].is_array())
      for (const auto& user : data["users"])
        recordTag(user);
    auto relationship = [&](const nlohmann::json& item, bool removed = false) {
      if (!item.is_object())
        return;
      if (item.contains("user"))
        recordTag(item["user"]);
      auto id = text(item, "id");
      if (!validSnowflake(id) || changes.users.size() >= maxUsers)
        return;
      if (removed)
        changes.users[id].relationship = 0;
      else if (item.contains("type") && item["type"].is_number_integer())
        changes.users[id].relationship = item["type"].get<int>();
      const auto ignored = item.find("user_ignored");
      if (removed)
        changes.users[id].ignored = false;
      else if (ignored != item.end() && ignored->is_boolean())
        changes.users[id].ignored = ignored->get<bool>();
      const auto nickname = item.find("nickname");
      if (removed || (nickname != item.end() && nickname->is_null()))
        changes.users[id].nickname = "";
      else if (nickname != item.end() && nickname->is_string() && nickname->get_ref<const std::string&>().size() <= 512)
        changes.users[id].nickname = nickname->get<std::string>();
    };
    auto presence = [&](const nlohmann::json& item, const std::string& guild = std::string{}) {
      if (!item.is_object())
        return;
      if (item.contains("user"))
        recordTag(item["user"]);
      auto id = text(item, "user_id");
      if (const auto user = item.find("user"); user != item.end() && user->is_object())
        id = text(*user, "id");
      if (!validSnowflake(id) || (changes.users.size() >= maxUsers && !changes.users.contains(id)))
        return;
      const auto activityScope = text(item, "guild_id");
      const auto& activityGuild = activityScope.empty() ? guild : activityScope;
      if (!activityGuild.empty() && !validSnowflake(activityGuild))
        return;
      if (const auto list = item.find("activities");
          list != item.end() && list->is_array() && list->size() <= maximumActivities) {
        const auto activities = rpc::receivedActivities(*list);
        if (activityGuild.empty()) {
          if (m_globalActivities.contains(id) || m_globalActivities.size() < maxUsers)
            m_globalActivities[id] = activities;
        } else if (m_guildActivities.contains(id) || m_guildActivities.size() < maxUsers) {
          auto& scopes = m_guildActivities[id];
          if (scopes.contains(activityGuild) || scopes.size() < maximumPresenceGuilds)
            scopes[activityGuild] = activities;
        }
        auto resolvedActivities = activities;
        if (const auto global = m_globalActivities.find(id); global != m_globalActivities.end())
          resolvedActivities = global->second;
        else if (const auto scopes = m_guildActivities.find(id); scopes != m_guildActivities.end())
          for (const auto& [scope, value] : scopes->second)
            if (!value.empty()) {
              resolvedActivities = value;
              break;
            }
        changes.users[id].activities = std::move(resolvedActivities);
        std::optional<UserCustomStatus> custom = UserCustomStatus{};
        for (const auto& activity : *list) {
          if (!activity.is_object()
              || !activity.contains("type")
              || !activity["type"].is_number_integer()
              || activity["type"] != customActivityType)
            continue;
          if (const auto state = activity.find("state"); state != activity.end() && !state->is_null()) {
            if (!state->is_string() || state->get_ref<const std::string&>().size() > maximumStatusBytes) {
              custom.reset();
              break;
            }
            custom->text = state->get<std::string>();
          }
          if (const auto emoji = activity.find("emoji"); emoji != activity.end() && emoji->is_object()) {
            const auto name = text(*emoji, "name");
            const auto emojiId = text(*emoji, "id");
            if (name.size() <= maximumStatusEmojiBytes && (emojiId.empty() || validSnowflake(emojiId))) {
              custom->emojiName = name;
              custom->emojiId = emojiId;
              custom->animated = emoji->contains("animated") && (*emoji)["animated"] == true;
            }
          }
          break;
        }
        if (custom) {
          if (activityGuild.empty()) {
            if (m_globalCustomStatuses.contains(id) || m_globalCustomStatuses.size() < maxUsers)
              m_globalCustomStatuses[id] = *custom;
          } else if (m_guildCustomStatuses.contains(id) || m_guildCustomStatuses.size() < maxUsers) {
            auto& scopes = m_guildCustomStatuses[id];
            if (scopes.contains(activityGuild) || scopes.size() < maximumPresenceGuilds)
              scopes[activityGuild] = *custom;
          }
          UserCustomStatus resolved;
          if (const auto global = m_globalCustomStatuses.find(id); global != m_globalCustomStatuses.end())
            resolved = global->second;
          else if (const auto scopes = m_guildCustomStatuses.find(id); scopes != m_guildCustomStatuses.end())
            for (const auto& [scope, value] : scopes->second)
              if (!value.empty()) {
                resolved = value;
                break;
              }
          changes.users[id].customStatus = std::move(resolved);
        }
      }
      auto status = text(item, "status");
      if (!validSnowflake(id)
          || (status != "online"
              && status != "idle"
              && status != "dnd"
              && status != "offline"
              && status != "invisible"))
        return;
      if (status == "invisible")
        status = "offline";
      const auto scope = text(item, "guild_id");
      const auto& guildId = scope.empty() ? guild : scope;
      if (guildId.empty()) {
        if (!m_globalStatuses.contains(id) && m_globalStatuses.size() >= maxUsers)
          return;
        m_globalStatuses[id] = status;
      } else {
        if (!validSnowflake(guildId) || (!m_guildStatuses.contains(id) && m_guildStatuses.size() >= maxUsers))
          return;
        auto& guilds = m_guildStatuses[id];
        if (!guilds.contains(guildId) && guilds.size() >= maximumPresenceGuilds)
          return;
        guilds[guildId] = status;
        // A global friend presence is authoritative for DMs. Guild-scoped data
        // is useful for non-friends, but must not override that global status.
        if (m_globalStatuses.contains(id))
          status = m_globalStatuses.at(id);
        else {
          status = "offline";
          for (const auto& [key, value] : guilds)
            if (value != "offline") {
              status = value;
              if (value == "online")
                break;
            }
        }
      }
      if (changes.users.size() < maxUsers || changes.users.contains(id))
        changes.users[id].status = status;
    };
    auto notificationSettings = [&](const nlohmann::json& settings) {
      for (const auto& [id, patch] : guildNotifications(settings))
        if (changes.guildNotifications.contains(id) || changes.guildNotifications.size() < 1000)
          changes.guildNotifications[id] = patch;
      for (const auto& [id, patch] : channelNotifications(settings)) {
        changes.notifications[id] = patch;
        if (patch.muted)
          changes.mutedChannels[id] = *patch.muted;
      }
    };
    if (type == "READY" || type == "READY_SUPPLEMENTAL") {
      if (const auto settings = data.find("user_guild_settings"); settings != data.end()) {
        const auto* entries = &*settings;
        if (entries->is_object() && entries->contains("entries"))
          entries = &(*entries)["entries"];
        if (entries->is_array())
          for (const auto& item : *entries)
            notificationSettings(item);
      }
      if (const auto relationships = data.find("relationships"); relationships != data.end()) {
        const auto* entries = &*relationships;
        if (entries->is_object() && entries->contains("entries"))
          entries = &(*entries)["entries"];
        if (entries->is_array())
          for (const auto& item : *entries)
            relationship(item);
      }
      if (const auto guilds = data.find("guilds"); guilds != data.end() && guilds->is_array())
        for (const auto& guild : *guilds)
          if (const auto list = guild.find("presences"); list != guild.end() && list->is_array())
            for (const auto& item : *list)
              presence(item, text(guild, "id"));
      if (const auto merged = data.find("merged_presences"); merged != data.end() && merged->is_object()) {
        if (const auto guilds = merged->find("guilds"); guilds != merged->end() && guilds->is_array())
          for (std::size_t i = 0; i < guilds->size(); ++i) {
            std::string guildId;
            if (data.contains("guilds") && data["guilds"].is_array() && i < data["guilds"].size())
              guildId = text(data["guilds"][i], "id");
            if ((*guilds)[i].is_array())
              for (const auto& item : (*guilds)[i])
                presence(item, guildId);
          }
        if (const auto friends = merged->find("friends"); friends != merged->end() && friends->is_array())
          for (const auto& item : *friends)
            presence(item);
      }
      if (const auto list = data.find("presences"); list != data.end() && list->is_array())
        for (const auto& item : *list)
          presence(item);
    } else if (type == "RELATIONSHIP_ADD" || type == "RELATIONSHIP_UPDATE" || type == "RELATIONSHIP_REMOVE")
      relationship(data, type == "RELATIONSHIP_REMOVE");
    else if (type == "PRESENCE_UPDATE")
      presence(data);
    else if (type == "GUILD_CREATE" || type == "GUILD_MEMBERS_CHUNK") {
      const auto guild = text(data, type == "GUILD_CREATE" ? "id" : "guild_id");
      if (const auto list = data.find("presences"); list != data.end() && list->is_array() && list->size() <= maxUsers)
        for (const auto& item : *list)
          presence(item, guild);
    } else if (type == "GUILD_MEMBER_LIST_UPDATE") {
      const auto guild = text(data, "guild_id");
      constexpr std::size_t maximumPresenceOperations = 128, maximumPresenceItems = 1000;
      const auto memberPresence = [&](const nlohmann::json& item) {
        if (!item.is_object() || !item.contains("member") || !item["member"].is_object())
          return;
        const auto& member = item["member"];
        if (!member.contains("presence")
            || !member["presence"].is_object()
            || !member.contains("user")
            || !member["user"].is_object())
          return;
        auto value = member["presence"];
        const auto id = text(member["user"], "id");
        if (!validSnowflake(id) || (value.contains("user") && text(value["user"], "id") != id))
          return;
        value["user_id"] = id;
        presence(value, guild);
      };
      if (const auto operations = data.find("ops");
          operations != data.end() && operations->is_array() && operations->size() <= maximumPresenceOperations)
        for (const auto& operation : *operations) {
          if (const auto items = operation.find("items");
              items != operation.end() && items->is_array() && items->size() <= maximumPresenceItems)
            for (const auto& item : *items)
              memberPresence(item);
          if (const auto item = operation.find("item"); item != operation.end())
            memberPresence(*item);
        }
    } else if (type == "USER_GUILD_SETTINGS_UPDATE")
      notificationSettings(data);
    const auto recordMember = [&](const nlohmann::json& member, const std::string& guild) {
      if (!member.is_object() || !validSnowflake(guild))
        return;
      const auto user = member.find("user");
      if (user == member.end() || !user->is_object())
        return;
      const auto id = text(*user, "id");
      if (!validSnowflake(id))
        return;
      recordTag(*user);
      auto patch = parseUserCosmetics(member);
      if (type == "GUILD_MEMBER_REMOVE")
        patch = {.decoration = "", .nameplate = ""};
      if ((patch.decoration || patch.nameplate)
          && changes.guildCosmetics.size() < maximumPresenceGuilds
          && changes.guildCosmetics[guild].size() < maxUsers)
        mergeCosmetics(changes.guildCosmetics[guild][id], patch);
    };
    const auto members = [&](const nlohmann::json& payload) {
      const auto guild = text(payload, "guild_id").empty() ? text(payload, "id") : text(payload, "guild_id");
      recordMember(payload, guild);
      if (const auto list = payload.find("members"); list != payload.end() && list->is_array())
        for (const auto& member : *list)
          recordMember(member, guild);
      if (const auto ops = payload.find("ops"); ops != payload.end() && ops->is_array())
        for (const auto& op : *ops) {
          if (const auto list = op.find("items"); list != op.end() && list->is_array())
            for (const auto& item : *list)
              if (item.contains("member"))
                recordMember(item["member"], guild);
          if (op.contains("item") && op["item"].contains("member"))
            recordMember(op["item"]["member"], guild);
        }
    };
    members(data);
    if (const auto guilds = data.find("guilds"); guilds != data.end() && guilds->is_array())
      for (const auto& guild : *guilds)
        if (guild.is_object())
          members(guild);
    const auto recipients = [&](const nlohmann::json& channel) {
      if (const auto list = channel.find("recipients"); list != channel.end() && list->is_array())
        for (const auto& user : *list)
          recordTag(user);
    };
    recipients(data);
    if (const auto list = data.find("private_channels"); list != data.end() && list->is_array())
      for (const auto& channel : *list)
        if (channel.is_object())
          recipients(channel);
    apply(changes);
    return changes;
  }
  UserCosmetics UserDirectory::cosmetics(const std::string& user, const std::string& guild) const {
    UserCosmetics result;
    if (const auto at = m_cosmetics.find(user); at != m_cosmetics.end())
      result = at->second;
    if (const auto at = m_guildCosmetics.find(guild); at != m_guildCosmetics.end())
      if (const auto member = at->second.find(user); member != at->second.end()) {
        if (member->second.decoration && !member->second.decoration->empty())
          result.decoration = member->second.decoration;
        if (member->second.nameplate && !member->second.nameplate->empty()) {
          result.nameplate = member->second.nameplate;
          result.nameplatePalette = member->second.nameplatePalette;
        }
      }
    return result;
  }
  std::vector<Account> UserDirectory::relatedUsers() const {
    std::vector<Account> result;
    for (const auto& [id, type] : m_relationships)
      if (type != 0) {
        const auto user = m_accounts.find(id);
        result.push_back(user == m_accounts.end() ? Account{id, "Unknown user"} : user->second);
      }
    return result;
  }
} // namespace api
