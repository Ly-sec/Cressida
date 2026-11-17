#include "api/server_preferences.h"

#include "api/models.h"
#include "api/user_directory.h"

#include <algorithm>
#include <ctime>
#include <nlohmann/json.hpp>

namespace api {
  bool GuildNotification::empty() const {
    return !muted
        && !suppressEveryone
        && !suppressRoles
        && !mobilePush
        && !hideMutedChannels
        && !muteScheduledEvents
        && !muteUntil
        && !level
        && !notifyHighlights
        && !flags;
  }
  std::map<std::string, GuildNotification> guildNotifications(const nlohmann::json& settings) {
    std::map<std::string, GuildNotification> result;
    if (!settings.is_object())
      return result;
    const auto id = settings.find("guild_id");
    if (id == settings.end() || !id->is_string() || !validSnowflake(id->get_ref<const std::string&>()))
      return result;
    GuildNotification patch;
    const auto boolean = [&](const char* key, std::optional<bool>& target) {
      if (const auto at = settings.find(key); at != settings.end() && at->is_boolean())
        target = at->get<bool>();
    };
    boolean("muted", patch.muted);
    boolean("suppress_everyone", patch.suppressEveryone);
    boolean("suppress_roles", patch.suppressRoles);
    boolean("mobile_push", patch.mobilePush);
    boolean("hide_muted_channels", patch.hideMutedChannels);
    boolean("mute_scheduled_events", patch.muteScheduledEvents);
    const auto integer = [&](const char* key, int maximum, std::optional<int>& target) {
      if (const auto at = settings.find(key);
          at != settings.end() && at->is_number_integer() && *at >= 0 && *at <= maximum)
        target = at->get<int>();
    };
    integer("message_notifications", 3, patch.level);
    integer("notify_highlights", 2, patch.notifyHighlights);
    if (const auto at = settings.find("flags"); at != settings.end() && at->is_number_integer() && *at >= 0)
      patch.flags = at->get<std::uint64_t>();
    if (const auto at = settings.find("mute_config"); at != settings.end()) {
      if (at->is_null())
        patch.muteUntil = "";
      else if (at->is_object()) {
        if (const auto end = at->find("end_time"); end != at->end()) {
          if (end->is_null())
            patch.muteUntil = "";
          else if (end->is_string() && muteDeadline(end->get_ref<const std::string&>()))
            patch.muteUntil = end->get<std::string>();
        }
      }
    }
    if (!patch.empty())
      result.emplace(id->get<std::string>(), std::move(patch));
    return result;
  }
  std::optional<nlohmann::json> guildNotificationPatch(const GuildNotification& patch, int muteDuration) {
    if (patch.empty()
        || patch.muteUntil
        || (patch.level && (*patch.level < 0 || *patch.level > 2))
        || (patch.notifyHighlights && (*patch.notifyHighlights < 0 || *patch.notifyHighlights > 2))
        || (muteDuration != -1
            && muteDuration != 900
            && muteDuration != 3600
            && muteDuration != 10800
            && muteDuration != 28800
            && muteDuration != 86400)
        || (muteDuration != -1 && (!patch.muted || !*patch.muted)))
      return {};
    auto body = nlohmann::json::object();
    const auto boolean = [&](const char* key, const std::optional<bool>& value) {
      if (value)
        body[key] = *value;
    };
    boolean("muted", patch.muted);
    boolean("suppress_everyone", patch.suppressEveryone);
    boolean("suppress_roles", patch.suppressRoles);
    boolean("mobile_push", patch.mobilePush);
    boolean("hide_muted_channels", patch.hideMutedChannels);
    boolean("mute_scheduled_events", patch.muteScheduledEvents);
    if (patch.level)
      body["message_notifications"] = *patch.level;
    if (patch.notifyHighlights)
      body["notify_highlights"] = *patch.notifyHighlights;
    if (patch.flags)
      body["flags"] = *patch.flags;
    if (patch.muted.value_or(false)) {
      body["mute_config"] = {{"end_time", nullptr}, {"selected_time_window", -1}};
      if (muteDuration != -1) {
        const auto end = std::time(nullptr) + muteDuration;
        std::tm utc{};
        gmtime_r(&end, &utc);
        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
        body["mute_config"] = {{"end_time", timestamp}, {"selected_time_window", muteDuration}};
      }
    }
    return body;
  }
  std::optional<nlohmann::json> guildReadBody(const std::vector<std::pair<std::string, std::string>>& states) {
    if (states.empty() || states.size() > 1000)
      return {};
    std::map<std::string, std::string> channels;
    for (const auto& [channel, message] : states) {
      if (!validSnowflake(channel) || !validSnowflake(message))
        return {};
      auto& latest = channels[channel];
      if (latest.size() < message.size() || (latest.size() == message.size() && latest < message))
        latest = message;
    }
    auto reads = nlohmann::json::array();
    for (const auto& [channel, message] : channels)
      reads.push_back({{"channel_id", channel}, {"message_id", message}, {"read_state_type", 0}});
    return nlohmann::json{{"read_states", std::move(reads)}};
  }
} // namespace api
