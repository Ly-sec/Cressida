#include "api/guild_settings.h"

#include "api/channel_features.h"
#include "api/history.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace api {
  namespace {
    using Json = nlohmann::json;
    using Section = GuildSettingsSection;
    using Verb = GuildSettingsVerb;
    namespace p = permission;
    constexpr std::uint64_t invite = p::createInstantInvite, kick = p::kickMembers, ban = p::banMembers,
                            admin = p::administrator, manageChannels = p::manageChannels, manageGuild = p::manageGuild,
                            audit = p::viewAuditLog, muteMembers = p::muteMembers, deafenMembers = p::deafenMembers,
                            moveMembers = p::moveMembers, nicknames = p::manageNicknames, manageRoles = p::manageRoles,
                            webhooks = p::manageWebhooks, expressions = p::manageGuildExpressions,
                            manageEvents = p::manageEvents, moderate = p::moderateMembers,
                            createExpressions = p::createGuildExpressions, createEvents = p::createEvents;

    bool text(const Json& value, std::size_t min, std::size_t max, bool multiline = false) {
      if (!value.is_string())
        return false;
      const auto& string = value.get_ref<const std::string&>();
      return string.size() <= max * 4
          && utf8Length(string) >= min
          && utf8Length(string) <= max
          && std::ranges::none_of(string, [multiline](unsigned char ch) {
               return ch == 0 || ch == 127 || (ch < 32 && !(multiline && (ch == '\n' || ch == '\t' || ch == '\r')));
             });
    }
    bool decimal(const Json& value) {
      return value.is_string()
          && !value.get_ref<const std::string&>().empty()
          && value.get_ref<const std::string&>().size() <= 128
          && std::ranges::all_of(value.get_ref<const std::string&>(), [](unsigned char c) {
               return c >= '0' && c <= '9';
             });
    }
    bool number(const Json& value, std::int64_t low, std::int64_t high) {
      if (value.is_number_unsigned())
        return value.get<std::uint64_t>() >= static_cast<std::uint64_t>(low)
            && value.get<std::uint64_t>() <= static_cast<std::uint64_t>(high);
      return value.is_number_integer() && value.get<std::int64_t>() >= low && value.get<std::int64_t>() <= high;
    }
    bool id(const Json& value, bool nullable = false) {
      return (nullable && value.is_null())
          || (value.is_string() && validSnowflake(value.get_ref<const std::string&>()));
    }
    bool ids(const Json& value, std::size_t max = 250) {
      if (!value.is_array() || value.size() > max)
        return false;
      std::set<std::string> distinct;
      for (const auto& entry : value)
        if (!id(entry) || !distinct.insert(entry.get<std::string>()).second)
          return false;
      return true;
    }
    bool code(std::string_view value) {
      return !value.empty()
          && value.size() <= 128
          && std::ranges::all_of(value, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
    }
    std::string encode(std::string_view value) {
      std::string result;
      constexpr char hex[] = "0123456789ABCDEF";
      for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '-'
            || c == '_'
            || c == '.')
          result += static_cast<char>(c);
        else {
          result += '%';
          result += hex[c >> 4];
          result += hex[c & 15];
        }
      }
      return result;
    }
    bool data(const Json& value, std::string_view category, std::size_t max) {
      if (value.is_null())
        return true;
      if (!value.is_string())
        return false;
      const auto& string = value.get_ref<const std::string&>();
      if (string.size() > max || !string.starts_with("data:" + std::string(category) + "/"))
        return false;
      const auto comma = string.find(";base64,");
      if (comma == std::string::npos || comma > 40 || comma + 8 >= string.size())
        return false;
      const auto format = std::string_view(string).substr(5 + category.size() + 1, comma - 5 - category.size() - 1);
      if (category == "image" && format != "png" && format != "jpeg" && format != "gif" && format != "webp")
        return false;
      if (category == "audio" && format != "mpeg" && format != "ogg")
        return false;
      const auto bytes = std::string_view(string).substr(comma + 8);
      return bytes.size() % 4 == 0 && std::ranges::all_of(bytes, [](unsigned char c) {
               return (c >= 'a' && c <= 'z')
                   || (c >= 'A' && c <= 'Z')
                   || (c >= '0' && c <= '9')
                   || c == '+'
                   || c == '/'
                   || c == '=';
             });
    }
    bool timestamp(const Json& value, bool nullable = false) {
      if (nullable && value.is_null())
        return true;
      if (!value.is_string())
        return false;
      const auto& string = value.get_ref<const std::string&>();
      return string.size() >= 20
          && string.size() <= 40
          && string[4] == '-'
          && string[7] == '-'
          && string[10] == 'T'
          && std::ranges::all_of(string, [](unsigned char c) {
               return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == ':' || c == '.' || c == 'T' || c == 'Z';
             });
    }
    std::optional<std::int64_t> epochMilliseconds(const Json& value) {
      if (!timestamp(value))
        return {};
      const auto& string = value.get_ref<const std::string&>();
      std::tm time{};
      std::istringstream input(string.substr(0, 19));
      input >> std::get_time(&time, "%Y-%m-%dT%H:%M:%S");
      if (input.fail())
        return {};
      const auto seconds = timegm(&time);
      if (seconds < 0)
        return {};
      std::int64_t fraction = 0;
      std::size_t offset = 19;
      if (offset < string.size() && string[offset] == '.') {
        ++offset;
        unsigned count = 0;
        while (offset < string.size() && string[offset] >= '0' && string[offset] <= '9') {
          if (count++ < 3)
            fraction = fraction * 10 + string[offset] - '0';
          ++offset;
        }
        while (count++ < 3)
          fraction *= 10;
      }
      if (offset >= string.size())
        return {};
      std::int64_t zoneSeconds = 0;
      if (string[offset] == '+' || string[offset] == '-') {
        if (offset + 6 != string.size() || string[offset + 3] != ':')
          return {};
        const int hours = (string[offset + 1] - '0') * 10 + string[offset + 2] - '0';
        const int minutes = (string[offset + 4] - '0') * 10 + string[offset + 5] - '0';
        if (hours > 23 || minutes > 59)
          return {};
        zoneSeconds = (hours * 60 + minutes) * 60 * (string[offset] == '+' ? 1 : -1);
      } else if (string[offset] != 'Z' || offset + 1 != string.size())
        return {};
      return (static_cast<std::int64_t>(seconds) - zoneSeconds) * 1000 + fraction;
    }
    std::string utcTimestamp(std::chrono::system_clock::time_point value) {
      const auto raw = std::chrono::system_clock::to_time_t(value);
      std::tm utc{};
      if (!gmtime_r(&raw, &utc))
        return {};
      std::ostringstream result;
      result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S.000Z");
      return result.str();
    }
    bool keys(const Json& value, std::initializer_list<std::string_view> allowed) {
      if (!value.is_object())
        return false;
      for (const auto& [name, entry] : value.items())
        if (std::ranges::find(allowed, name) == allowed.end())
          return false;
      return true;
    }
    bool required(const Json& value, std::initializer_list<std::string_view> names) {
      return std::ranges::all_of(names, [&](const auto name) { return value.contains(name); });
    }
    bool colors(const Json& value) {
      if (!keys(value, {"primary_color", "secondary_color", "tertiary_color"}) || !value.contains("primary_color"))
        return false;
      for (const auto& [name, entry] : value.items())
        if (!(name != "primary_color" && entry.is_null()) && !number(entry, 0, 0xffffff))
          return false;
      if (value.contains("tertiary_color") && !value["tertiary_color"].is_null())
        return value["primary_color"] == 11127295
            && value.contains("secondary_color")
            && value["secondary_color"] == 16759788
            && value["tertiary_color"] == 16761760;
      return true;
    }
    bool overview(const Json& value) {
      if (!keys(
              value,
              {"name",
               "description",
               "icon",
               "banner",
               "splash",
               "discovery_splash",
               "verification_level",
               "default_message_notifications",
               "explicit_content_filter",
               "afk_channel_id",
               "afk_timeout",
               "system_channel_id",
               "system_channel_flags",
               "rules_channel_id",
               "public_updates_channel_id",
               "safety_alerts_channel_id",
               "preferred_locale",
               "features",
               "premium_progress_bar_enabled",
               "owner_id",
               "code",
               "home_header",
               "owner_configured_content_level",
               "moderator_reporting_enabled",
               "official_message_color",
               "verification_role_id",
               "profile"}
          ))
        return false;
      for (const auto& [field, entry] : value.items()) {
        if (field == "name") {
          if (!text(entry, 2, 100)
              || entry.get_ref<const std::string&>().front() == ' '
              || entry.get_ref<const std::string&>().back() == ' ')
            return false;
        } else if (field == "description") {
          if (!entry.is_null() && !text(entry, 0, 300, true))
            return false;
        } else if (
            field == "icon"
            || field == "banner"
            || field == "splash"
            || field == "discovery_splash"
            || field == "home_header"
        ) {
          if (!data(entry, "image", 14 * 1024 * 1024))
            return false;
        } else if (field.ends_with("_channel_id") || field == "owner_id" || field == "verification_role_id") {
          if (!id(entry, field != "owner_id"))
            return false;
        } else if (field == "verification_level") {
          if (!number(entry, 0, 4))
            return false;
        } else if (field == "default_message_notifications") {
          if (!number(entry, 0, 1))
            return false;
        } else if (field == "explicit_content_filter") {
          if (!number(entry, 0, 2))
            return false;
        } else if (field == "afk_timeout") {
          if (!number(entry, 60, 3600)
              || (entry != 60 && entry != 300 && entry != 900 && entry != 1800 && entry != 3600))
            return false;
        } else if (field == "system_channel_flags") {
          if (!number(entry, 0, 0xffffffffLL))
            return false;
        } else if (field == "premium_progress_bar_enabled" || field == "moderator_reporting_enabled") {
          if (!entry.is_boolean())
            return false;
        } else if (field == "code") {
          if (!value.contains("owner_id") || (!entry.is_null() && !text(entry, 6, 12)))
            return false;
        } else if (field == "owner_configured_content_level") {
          if (!number(entry, 0, 3))
            return false;
        } else if (field == "official_message_color") {
          if (!entry.is_null() && !number(entry, 0, 0xffffff))
            return false;
        } else if (field == "profile") {
          if (!entry.is_null()
              && (!keys(entry, {"tag"})
                  || !entry.contains("tag")
                  || (!entry["tag"].is_null() && !text(entry["tag"], 2, 4))))
            return false;
        } else if (field == "preferred_locale") {
          if (!text(entry, 2, 16))
            return false;
        } else if (field == "features") {
          if (!entry.is_array() || entry.size() > 128)
            return false;
          for (const auto& feature : entry)
            if (!text(feature, 1, 100)
                || !std::ranges::all_of(feature.get_ref<const std::string&>(), [](unsigned char c) {
                     return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
                   }))
              return false;
        }
      }
      return !value.empty();
    }
    bool profile(const Json& value) {
      if (!keys(
              value,
              {"name", "description", "icon", "custom_banner", "brand_color_primary", "game_application_ids", "tag",
               "badge", "badge_color_primary", "badge_color_secondary", "traits", "visibility"}
          )
          || value.empty())
        return false;
      for (const auto& [field, entry] : value.items()) {
        if (field == "name" && !text(entry, 2, 100))
          return false;
        if (field == "description" && !entry.is_null() && !text(entry, 0, 300, true))
          return false;
        if ((field == "icon" || field == "custom_banner") && !data(entry, "image", 14 * 1024 * 1024))
          return false;
        if (field == "game_application_ids" && !ids(entry, 20))
          return false;
        if (field == "tag" && !entry.is_null() && !text(entry, 2, 4))
          return false;
        if (field == "badge" && !number(entry, 0, 100))
          return false;
        if (field == "visibility" && !number(entry, 0, 3))
          return false;
        if (field == "brand_color_primary" || field == "badge_color_primary" || field == "badge_color_secondary") {
          if (entry.is_null())
            continue;
          if (!entry.is_string())
            return false;
          const auto& color = entry.get_ref<const std::string&>();
          const auto digits = std::string_view(color).substr(color.starts_with('#') ? 1 : 0);
          if (digits.size() != 6 || !std::ranges::all_of(digits, [](unsigned char c) { return std::isxdigit(c); }))
            return false;
        }
        if (field == "traits") {
          if (!entry.is_array() || entry.size() > 5)
            return false;
          std::set<unsigned> positions;
          for (const auto& trait : entry) {
            if (!keys(trait, {"label", "position", "emoji_id", "emoji_name", "emoji_animated"})
                || !required(trait, {"label", "position"})
                || !text(trait["label"], 1, 32)
                || !number(trait["position"], 0, 4)
                || !positions.insert(trait["position"].get<unsigned>()).second)
              return false;
            if (trait.contains("emoji_id") && !id(trait["emoji_id"], true))
              return false;
            if (trait.contains("emoji_name") && !trait["emoji_name"].is_null() && !text(trait["emoji_name"], 1, 32))
              return false;
            if (trait.contains("emoji_animated") && !trait["emoji_animated"].is_boolean())
              return false;
          }
        }
      }
      return true;
    }
    bool discovery(const Json& value) {
      if (!keys(
              value,
              {"primary_category_id", "keywords", "emoji_discoverability_enabled", "is_published", "reasons_to_join",
               "social_links", "about"}
          )
          || value.empty())
        return false;
      for (const auto& [field, entry] : value.items()) {
        if (entry.is_null())
          continue;
        if (field == "primary_category_id" && !number(entry, 0, 10000))
          return false;
        if ((field == "emoji_discoverability_enabled" || field == "is_published") && !entry.is_boolean())
          return false;
        if (field == "about" && !text(entry, 0, 2400, true))
          return false;
        if (field == "keywords" || field == "social_links") {
          if (!entry.is_array() || entry.size() > (field == "keywords" ? 10u : 9u))
            return false;
          for (const auto& item : entry)
            if (!text(item, 1, field == "keywords" ? 30 : 256))
              return false;
        }
        if (field == "reasons_to_join") {
          if (!entry.is_array() || entry.size() > 4)
            return false;
          for (const auto& reason : entry) {
            if (!keys(reason, {"reason", "emoji_id", "emoji_name"})
                || !reason.contains("reason")
                || !text(reason["reason"], 1, 300))
              return false;
            if (reason.contains("emoji_id") && !id(reason["emoji_id"], true))
              return false;
            if (reason.contains("emoji_name") && !reason["emoji_name"].is_null() && !text(reason["emoji_name"], 1, 32))
              return false;
          }
        }
      }
      return true;
    }
    bool role(const Json& value) {
      if (!keys(value, {"name", "permissions", "color", "colors", "hoist", "icon", "unicode_emoji", "mentionable"}))
        return false;
      for (const auto& [field, entry] : value.items()) {
        if (field == "name" && !text(entry, 1, 100))
          return false;
        if (field == "permissions" && !decimal(entry))
          return false;
        if (field == "color" && !number(entry, 0, 0xffffff))
          return false;
        if (field == "colors" && !colors(entry))
          return false;
        if ((field == "hoist" || field == "mentionable") && !entry.is_boolean())
          return false;
        if (field == "icon" && !data(entry, "image", 512 * 1024))
          return false;
        if (field == "unicode_emoji" && !entry.is_null() && !text(entry, 1, 32))
          return false;
      }
      return true;
    }
    bool member(const Json& value) {
      if (!keys(value, {"nick", "roles", "mute", "deaf", "channel_id", "communication_disabled_until", "flags"}))
        return false;
      for (const auto& [field, entry] : value.items()) {
        if (field == "nick" && !entry.is_null() && !text(entry, 0, 32))
          return false;
        if (field == "roles" && !ids(entry))
          return false;
        if ((field == "mute" || field == "deaf") && !entry.is_boolean())
          return false;
        if (field == "channel_id" && !id(entry, true))
          return false;
        if (field == "communication_disabled_until" && !timestamp(entry, true))
          return false;
        if (field == "flags" && !number(entry, 0, 0xffffffffLL))
          return false;
      }
      return !value.empty();
    }
    bool expression(const Json& value, Section section, bool create) {
      if (section == Section::Emoji) {
        if (!keys(value, {"name", "image", "roles"}) || (create && !required(value, {"name", "image"})))
          return false;
        for (const auto& [field, entry] : value.items()) {
          if (field == "name"
              && (!text(entry, 2, 32) || !std::ranges::all_of(entry.get_ref<const std::string&>(), [](unsigned char c) {
                   return std::isalnum(c) || c == '_';
                 })))
            return false;
          if (field == "image" && (!create || entry.is_null() || !data(entry, "image", 512 * 1024)))
            return false;
          if (field == "roles" && !ids(entry))
            return false;
        }
      } else if (section == Section::Stickers) {
        if (!keys(
                value,
                create ? std::initializer_list<
                             std::string_view>{"name", "description", "tags", "file", "filename", "content_type"}
                       : std::initializer_list<std::string_view>{"name", "description", "tags"}
            )
            || (create && !required(value, {"name", "tags", "file", "filename", "content_type"})))
          return false;
        for (const auto& [field, entry] : value.items()) {
          if (field == "name" && !text(entry, 2, 30))
            return false;
          if (field == "description" && !entry.is_null() && !text(entry, 0, 100))
            return false;
          if (field == "tags" && !text(entry, 1, 200))
            return false;
          if (field == "filename"
              && (!text(entry, 1, 128)
                  || entry.get_ref<const std::string&>().find_first_of("/\\") != std::string::npos))
            return false;
          if (field == "content_type"
              && entry != "image/png"
              && entry != "image/apng"
              && entry != "image/gif"
              && entry != "application/json")
            return false;
          if (field == "file"
              && (!entry.is_binary() || entry.get_binary().empty() || entry.get_binary().size() > 512 * 1024))
            return false;
        }
      } else {
        if (!keys(value, {"name", "sound", "volume", "emoji_id", "emoji_name"})
            || (create && !required(value, {"name", "sound"})))
          return false;
        for (const auto& [field, entry] : value.items()) {
          if (field == "name" && !text(entry, 2, 32))
            return false;
          if (field == "sound" && (!create || entry.is_null() || !data(entry, "audio", 1024 * 1024)))
            return false;
          if (field == "volume"
              && (!entry.is_number()
                  || !std::isfinite(entry.get<double>())
                  || entry.get<double>() < 0
                  || entry.get<double>() > 1))
            return false;
          if (field == "emoji_id" && !id(entry, true))
            return false;
          if (field == "emoji_name" && !entry.is_null() && !text(entry, 1, 32))
            return false;
        }
      }
      return !value.empty();
    }
    bool welcome(const Json& value) {
      if (!keys(value, {"enabled", "description", "welcome_channels"}))
        return false;
      for (const auto& [field, entry] : value.items()) {
        if (field == "enabled" && !entry.is_boolean())
          return false;
        if (field == "description" && !entry.is_null() && !text(entry, 0, 140, true))
          return false;
        if (field == "welcome_channels") {
          if (!entry.is_array() || entry.size() > 5)
            return false;
          for (const auto& channel : entry) {
            if (!keys(channel, {"channel_id", "description", "emoji_id", "emoji_name"})
                || !required(channel, {"channel_id", "description"})
                || !id(channel["channel_id"])
                || !text(channel["description"], 1, 50))
              return false;
            if (channel.contains("emoji_id") && !id(channel["emoji_id"], true))
              return false;
            if (channel.contains("emoji_name")
                && !channel["emoji_name"].is_null()
                && !text(channel["emoji_name"], 1, 32))
              return false;
          }
        }
      }
      return !value.empty();
    }
    bool onboarding(const Json& value) {
      if (!keys(value, {"enabled", "mode", "default_channel_ids", "prompts"}))
        return false;
      for (const auto& [field, entry] : value.items()) {
        if (field == "enabled" && !entry.is_boolean())
          return false;
        if (field == "mode" && !number(entry, 0, 1))
          return false;
        if (field == "default_channel_ids" && !ids(entry, 100))
          return false;
        if (field != "prompts")
          continue;
        if (!entry.is_array() || entry.size() > 20)
          return false;
        for (const auto& prompt : entry) {
          if (!keys(prompt, {"id", "type", "options", "title", "single_select", "required", "in_onboarding"})
              || !required(prompt, {"title", "options"})
              || !text(prompt["title"], 1, 100))
            return false;
          if (prompt.contains("id") && !id(prompt["id"]))
            return false;
          if (prompt.contains("type") && !number(prompt["type"], 0, 1))
            return false;
          for (const auto flag : {"single_select", "required", "in_onboarding"})
            if (prompt.contains(flag) && !prompt[flag].is_boolean())
              return false;
          if (!prompt["options"].is_array() || prompt["options"].empty() || prompt["options"].size() > 50)
            return false;
          for (const auto& option : prompt["options"]) {
            if (!keys(
                    option,
                    {"id", "channel_ids", "role_ids", "emoji_id", "emoji_name", "emoji_animated", "title",
                     "description"}
                )
                || !required(option, {"title", "channel_ids", "role_ids"})
                || !text(option["title"], 1, 50)
                || !ids(option["channel_ids"], 50)
                || !ids(option["role_ids"], 50))
              return false;
            if (option.contains("id") && !id(option["id"]))
              return false;
            if (option.contains("description") && !text(option["description"], 0, 100))
              return false;
            if (option.contains("emoji_id") && !id(option["emoji_id"], true))
              return false;
            if (option.contains("emoji_name") && !option["emoji_name"].is_null() && !text(option["emoji_name"], 1, 32))
              return false;
            if (option.contains("emoji_animated") && !option["emoji_animated"].is_boolean())
              return false;
          }
        }
      }
      return !value.empty();
    }
    bool automod(const Json& value, bool create) {
      if (!keys(
              value,
              {"name", "event_type", "trigger_type", "trigger_metadata", "actions", "enabled", "exempt_roles",
               "exempt_channels"}
          )
          || (create && !required(value, {"name", "event_type", "trigger_type", "actions"})))
        return false;
      for (const auto& [field, entry] : value.items()) {
        if (field == "name" && !text(entry, 1, 100))
          return false;
        if (field == "event_type" && !number(entry, 1, 2))
          return false;
        if (field == "trigger_type" && (!create || !number(entry, 1, 6) || entry == 2))
          return false;
        if (field == "enabled" && !entry.is_boolean())
          return false;
        if (field == "exempt_roles" && !ids(entry, 20))
          return false;
        if (field == "exempt_channels" && !ids(entry, 50))
          return false;
        if (field == "trigger_metadata") {
          if (!keys(
                  entry,
                  {"keyword_filter", "regex_patterns", "presets", "allow_list", "mention_total_limit",
                   "mention_raid_protection_enabled"}
              ))
            return false;
          for (const auto& [name, item] : entry.items()) {
            if (name == "mention_total_limit") {
              if (!number(item, 1, 50))
                return false;
            } else if (name == "mention_raid_protection_enabled") {
              if (!item.is_boolean())
                return false;
            } else {
              if (!item.is_array() || item.size() > (name == "regex_patterns" ? 10u : name == "presets" ? 3u : 1000u))
                return false;
              for (const auto& word : item)
                if (name == "presets" ? !number(word, 1, 3) : !text(word, 1, name == "regex_patterns" ? 260 : 60))
                  return false;
            }
          }
        }
        if (field == "actions") {
          if (!entry.is_array() || entry.empty() || entry.size() > 4)
            return false;
          for (const auto& action : entry) {
            if (!keys(action, {"type", "metadata"}) || !action.contains("type") || !number(action["type"], 1, 4))
              return false;
            if (!action.contains("metadata")) {
              if (action["type"] == 2 || action["type"] == 3)
                return false;
              continue;
            }
            if (!keys(action["metadata"], {"channel_id", "duration_seconds", "custom_message"}))
              return false;
            for (const auto& [key, item] : action["metadata"].items()) {
              if (key == "channel_id" && !id(item))
                return false;
              if (key == "duration_seconds" && !number(item, 1, 2419200))
                return false;
              if (key == "custom_message" && !text(item, 0, 150))
                return false;
            }
            if ((action["type"] == 2 && !action["metadata"].contains("channel_id"))
                || (action["type"] == 3 && !action["metadata"].contains("duration_seconds")))
              return false;
          }
        }
      }
      return !value.empty();
    }
    bool recurrence(const Json& value) {
      if (value.is_null())
        return true;
      if (!keys(value, {"start", "frequency", "interval", "by_weekday", "by_n_weekday", "by_month", "by_month_day"})
          || !required(value, {"start", "frequency", "interval"})
          || !timestamp(value["start"])
          || !number(value["frequency"], 0, 3)
          || !number(value["interval"], 1, 2))
        return false;
      const int frequency = value["frequency"].get<int>(), interval = value["interval"].get<int>();
      if (interval == 2 && frequency != 2)
        return false;
      auto present = [&](const char* field) { return value.contains(field) && !value[field].is_null(); };
      const bool weekdays = present("by_weekday"), nthWeekdays = present("by_n_weekday"), months = present("by_month"),
                 monthDays = present("by_month_day");
      if (static_cast<int>(weekdays) + static_cast<int>(nthWeekdays) + static_cast<int>(months || monthDays) > 1)
        return false;
      if (weekdays) {
        const auto& days = value["by_weekday"];
        if (!days.is_array() || days.empty() || days.size() > 7 || (frequency != 2 && frequency != 3))
          return false;
        std::set<int> unique;
        for (const auto& day : days)
          if (!number(day, 0, 6) || !unique.insert(day.get<int>()).second)
            return false;
        if (frequency == 2 && days.size() != 1)
          return false;
        if (frequency == 3) {
          const std::array<std::set<int>, 6> allowed{
              {{0, 1, 2, 3, 4}, {1, 2, 3, 4, 5}, {0, 1, 2, 3, 6}, {4, 5}, {5, 6}, {0, 6}}
          };
          if (std::ranges::find(allowed, unique) == allowed.end())
            return false;
        }
      }
      if (nthWeekdays) {
        const auto& nth = value["by_n_weekday"];
        if (frequency != 1
            || !nth.is_array()
            || nth.size() != 1
            || !keys(nth[0], {"n", "day"})
            || !required(nth[0], {"n", "day"})
            || !number(nth[0]["n"], 1, 5)
            || !number(nth[0]["day"], 0, 6))
          return false;
      }
      if (months || monthDays) {
        if (frequency != 0
            || !months
            || !monthDays
            || !value["by_month"].is_array()
            || value["by_month"].size() != 1
            || !value["by_month_day"].is_array()
            || value["by_month_day"].size() != 1
            || !number(value["by_month"][0], 1, 12)
            || !number(value["by_month_day"][0], 1, 31))
          return false;
        constexpr std::array monthLengths{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (value["by_month_day"][0].get<int>() > monthLengths[value["by_month"][0].get<int>() - 1])
          return false;
      }
      return true;
    }
    bool event(const Json& value, bool create) {
      if (!keys(
              value,
              {"name", "description", "channel_id", "privacy_level", "scheduled_start_time", "scheduled_end_time",
               "entity_type", "entity_metadata", "status", "image", "recurrence_rule"}
          )
          || (create && !required(value, {"name", "privacy_level", "scheduled_start_time", "entity_type"})))
        return false;
      for (const auto& [field, entry] : value.items()) {
        if (field == "name" && !text(entry, 1, 100))
          return false;
        if (field == "description" && !entry.is_null() && !text(entry, 0, 1000, true))
          return false;
        if (field == "channel_id" && !id(entry, true))
          return false;
        if (field == "privacy_level" && entry != 2)
          return false;
        if ((field == "scheduled_start_time" || field == "scheduled_end_time")
            && !timestamp(entry, field == "scheduled_end_time"))
          return false;
        if (field == "entity_type" && !number(entry, 1, 3))
          return false;
        if (field == "status" && !number(entry, 1, 4))
          return false;
        if (field == "image" && !data(entry, "image", 14 * 1024 * 1024))
          return false;
        if (field == "recurrence_rule" && !recurrence(entry))
          return false;
        if (field == "entity_metadata"
            && !entry.is_null()
            && (!keys(entry, {"location"}) || !entry.contains("location") || !text(entry["location"], 1, 100)))
          return false;
      }
      if (create && value["entity_type"] == 3)
        return value.contains("entity_metadata")
            && !value["entity_metadata"].is_null()
            && value.contains("scheduled_end_time")
            && !value["scheduled_end_time"].is_null()
            && (!value.contains("channel_id") || value["channel_id"].is_null());
      if (create && (!value.contains("channel_id") || value["channel_id"].is_null()))
        return false;
      return !value.empty();
    }
    bool guide(const Json& value) {
      if (!keys(value, {"enabled", "welcome_message", "new_member_actions", "resource_channels"}) || value.empty())
        return false;
      if (value.contains("enabled") && !value["enabled"].is_boolean())
        return false;
      if (value.contains("welcome_message")) {
        const auto& welcome = value["welcome_message"];
        if (!keys(welcome, {"author_ids", "message"})
            || !required(welcome, {"author_ids", "message"})
            || !ids(welcome["author_ids"], 7)
            || !text(welcome["message"], 0, 300, true))
          return false;
      }
      for (const auto* field : {"new_member_actions", "resource_channels"}) {
        if (!value.contains(field))
          continue;
        const auto& entries = value[field];
        const bool actions = std::string_view(field) == "new_member_actions";
        if (!entries.is_array() || entries.size() > (actions ? 5u : 7u))
          return false;
        std::set<std::string> channels;
        for (const auto& entry : entries) {
          if (!keys(entry, {"channel_id", "action_type", "title", "description", "emoji", "icon"})
              || !required(entry, {"channel_id", "title"})
              || !id(entry["channel_id"])
              || !channels.insert(entry["channel_id"].get<std::string>()).second
              || !text(entry["title"], 1, actions ? 60 : 30))
            return false;
          if (actions && (!entry.contains("action_type") || !number(entry["action_type"], 0, 1)))
            return false;
          if (!actions && entry.contains("action_type"))
            return false;
          if (entry.contains("description") && !text(entry["description"], 0, 200, true))
            return false;
          if (entry.contains("icon")
              && !entry["icon"].is_null()
              && !(entry["icon"].is_string() && validAssetHash(entry["icon"].get_ref<const std::string&>())))
            return false;
          if (entry.contains("emoji") && !entry["emoji"].is_null()) {
            const auto& emoji = entry["emoji"];
            if (!keys(emoji, {"id", "name", "animated"})
                || (emoji.contains("id") && !id(emoji["id"], true))
                || (emoji.contains("name") && !emoji["name"].is_null() && !text(emoji["name"], 1, 32))
                || (emoji.contains("animated") && !emoji["animated"].is_boolean()))
              return false;
          }
        }
      }
      return true;
    }

    std::optional<Json> memberSearch(const GuildSettingsRequest& request) {
      const auto& values = request.values;
      if (!keys(
              values,
              {"role_ids", "sort", "joined_after", "joined_before", "account_after", "account_before",
               "source_invite_code", "join_source_type", "require_unusual_dm_activity", "require_timeout",
               "require_unusual_account_activity", "require_quarantined_username"}
          ))
        return {};
      Json result{
          {"limit", std::min(250u, request.limit)},
          {"sort", 1},
          {"and_query", Json::object()},
          {"or_query", Json::object()}
      };
      if (values.contains("sort")) {
        if (!number(values["sort"], 1, 4))
          return {};
        result["sort"] = values["sort"];
      }
      if (!request.query.empty()) {
        Json names = Json::array(), users = Json::array();
        std::size_t offset = 0;
        while (offset < request.query.size()) {
          const auto end = request.query.find(',', offset);
          auto term = std::string_view(request.query)
                          .substr(offset, end == std::string::npos ? std::string::npos : end - offset);
          while (!term.empty() && term.front() == ' ')
            term.remove_prefix(1);
          while (!term.empty() && term.back() == ' ')
            term.remove_suffix(1);
          if (!term.empty()) {
            if (names.size() + users.size() >= 25)
              return {};
            (validSnowflake(std::string(term)) ? users : names).push_back(term);
          }
          if (end == std::string::npos)
            break;
          offset = end + 1;
        }
        if (!names.empty())
          result["and_query"]["usernames"] = {{"or_query", names}};
        if (!users.empty())
          result["and_query"]["user_id"] = {{"or_query", users}};
      }
      if (values.contains("role_ids")) {
        if (!ids(values["role_ids"], 100))
          return {};
        if (!values["role_ids"].empty())
          result["and_query"]["role_ids"] = {{"and_query", values["role_ids"]}};
      }
      for (const auto prefix : {"joined", "account"}) {
        const auto afterKey = std::string(prefix) + "_after", beforeKey = std::string(prefix) + "_before";
        std::optional<std::int64_t> after, before;
        if (values.contains(afterKey))
          after = epochMilliseconds(values[afterKey]);
        if (values.contains(beforeKey))
          before = epochMilliseconds(values[beforeKey]);
        if ((values.contains(afterKey) && !after)
            || (values.contains(beforeKey) && !before)
            || (after && before && *after > *before))
          return {};
        if (std::string_view(prefix) == "account") {
          constexpr auto lastSnowflakeTime =
              1420070400000LL + static_cast<std::int64_t>(std::numeric_limits<std::uint64_t>::max() >> 22);
          if ((after && *after > lastSnowflakeTime) || (before && *before > lastSnowflakeTime))
            return {};
        }
        const auto target = std::string_view(prefix) == "joined" ? "guild_joined_at" : "user_id";
        const auto asValue = [&](std::int64_t milliseconds) -> Json {
          if (std::string_view(prefix) == "joined")
            return milliseconds;
          if (milliseconds < 1420070400000LL)
            return "0";
          return std::to_string(static_cast<std::uint64_t>(milliseconds - 1420070400000LL) << 22);
        };
        if (after)
          result["and_query"][target]["range"]["gte"] = asValue(*after);
        if (before)
          result["and_query"][target]["range"]["lte"] = asValue(*before);
      }
      if (values.contains("source_invite_code")) {
        if (!values["source_invite_code"].is_string()
            || !code(values["source_invite_code"].get_ref<const std::string&>()))
          return {};
        result["and_query"]["source_invite_code"] = {{"or_query", Json::array({values["source_invite_code"]})}};
      }
      if (values.contains("join_source_type")) {
        if (!number(values["join_source_type"], 0, 8))
          return {};
        result["and_query"]["join_source_type"] = {{"or_query", Json::array({values["join_source_type"]})}};
      }
      const auto now =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();
      for (const auto flag :
           {"require_unusual_dm_activity", "require_timeout", "require_unusual_account_activity",
            "require_quarantined_username"}) {
        if (!values.contains(flag))
          continue;
        if (!values[flag].is_boolean())
          return {};
        if (!values[flag].get<bool>())
          continue;
        auto& signals = result["or_query"]["safety_signals"];
        if (std::string_view(flag) == "require_unusual_dm_activity")
          signals["unusual_dm_activity_until"] = {{"range", {{"gte", now - 2LL * 86400000}}}};
        if (std::string_view(flag) == "require_timeout")
          signals["communication_disabled_until"] = {{"range", {{"gte", now}}}};
        if (std::string_view(flag) == "require_unusual_account_activity")
          signals["unusual_account_activity"] = true;
        if (std::string_view(flag) == "require_quarantined_username")
          signals["automod_quarantined_username"] = true;
      }
      if (!request.cursor.empty()) {
        auto cursor = Json::parse(request.cursor, nullptr, false);
        if (!keys(cursor, {"guild_joined_at", "user_id"})
            || !required(cursor, {"guild_joined_at", "user_id"})
            || !id(cursor["user_id"])
            || !number(cursor["guild_joined_at"], 0, 253402300799999LL))
          return {};
        result["after"] = std::move(cursor);
      }
      return result;
    }
    void bounded(const Json& value, unsigned depth, std::size_t& nodes) {
      if (++nodes > 200000 || depth > 16)
        throw std::runtime_error("Server settings response exceeds limits");
      if (value.is_string() && value.get_ref<const std::string&>().size() > 65536)
        throw std::runtime_error("Server settings text exceeds limits");
      if ((value.is_array() || value.is_object()) && value.size() > 10000)
        throw std::runtime_error("Server settings collection exceeds limits");
      if (value.is_array() || value.is_object())
        for (const auto& item : value)
          bounded(item, depth + 1, nodes);
    }
  } // namespace

  std::optional<GuildSettingsHttpRequest> guildSettingsHttpRequest(const GuildSettingsRequest& request) {
    if (!validSnowflake(request.guildId)
        || request.limit < 1
        || request.limit > 1000
        || request.query.size() > 400
        || !text(Json(request.query), 0, 100)
        || !text(Json(request.reason), 0, 512)
        || (!request.cursor.empty() && request.section != Section::Members && !validSnowflake(request.cursor))
        || request.cursor.size() > 256
        || request.mfaCode.size() > 1024
        || (request.mfaChallenge
            && request.mfaType != "totp"
            && request.mfaType != "sms"
            && request.mfaType != "password"))
      return {};
    const auto verb = request.verb;
    const bool get = verb == Verb::Get, create = verb == Verb::Create, update = verb == Verb::Update,
               remove = verb == Verb::Delete;
    if (!get && !remove && request.values.dump().size() > 16 * 1024 * 1024)
      return {};
    const auto base = "/guilds/" + request.guildId;
    const auto& resource = request.resourceId;
    const auto& values = request.values;
    GuildSettingsHttpRequest result;
    auto standard = [&](std::string path, bool allowCreate = true, bool allowUpdate = true, bool allowDelete = true) {
      if (get && resource.empty()) {
        result.path = std::move(path);
        return true;
      }
      if (create && resource.empty() && allowCreate) {
        result.path = std::move(path);
        result.method = "POST";
        result.body = values;
        return true;
      }
      if (!validSnowflake(resource))
        return false;
      if (get || (update && allowUpdate) || (remove && allowDelete)) {
        result.path = path + "/" + resource;
        result.method = get ? "GET" : remove ? "DELETE" : "PATCH";
        if (update)
          result.body = values;
        return true;
      }
      return false;
    };
    auto singleton = [&](std::string suffix, bool put = false) {
      if (!resource.empty() || (!get && !update))
        return false;
      result.path = base + suffix;
      result.method = get ? "GET" : put ? "PUT" : "PATCH";
      if (update)
        result.body = values;
      return true;
    };
    switch (request.section) {
    case Section::Overview:
      if (update && values.is_object() && values.contains("mfa_level")) {
        if (!resource.empty()
            || !keys(values, {"mfa_level", "code"})
            || !number(values["mfa_level"], 0, 1)
            || (values.contains("code") && !text(values["code"], 6, 12)))
          return {};
        result.path = base + "/mfa";
        result.method = "POST";
        result.body = {{"level", values["mfa_level"]}};
        if (values.contains("code"))
          result.body["code"] = values["code"];
        break;
      }
      if (!singleton("") || (update && !overview(values)))
        return {};
      if (get)
        result.path += "?with_counts=true";
      break;
    case Section::Profile:
      if (!singleton("/profile") || (update && !profile(values)))
        return {};
      if (update && values.size() == 1 && values.contains("visibility"))
        result.path += "/visibility";
      break;
    case Section::OwnershipCode:
      if (!create || !resource.empty() || !values.is_object() || !values.empty())
        return {};
      result.path = base + "/pincode";
      result.method = "PUT";
      break;
    case Section::Applications:
      if (!get || !resource.empty() || request.query.empty() || !request.cursor.empty())
        return {};
      result.path = "/games/autocomplete?q=" + encode(request.query) + "&filter_group=guild_profile_games";
      break;
    case Section::Discovery:
      if (get && resource == "categories")
        result.path = "/discovery/categories";
      else if (get && resource == "requirements")
        result.path = base + "/discovery-requirements";
      else if ((create || remove) && validSnowflake(resource) && values.is_object() && values.empty()) {
        result.path = base + "/discovery-categories/" + resource;
        result.method = create ? "PUT" : "DELETE";
      } else if (!singleton("/discovery-metadata") || (update && !discovery(values)))
        return {};
      break;
    case Section::Insights: {
      if (!get || !request.cursor.empty() || !keys(values, {"start", "end", "interval", "days"}))
        return {};
      if (!resource.empty() && resource != "engagement" && resource != "growth" && resource != "retention")
        return {};
      if (values.contains("interval") && !number(values["interval"], 1, 3))
        return {};
      std::string start, end;
      if (values.contains("start") || values.contains("end")) {
        if (values.contains("days") || !required(values, {"start", "end"}))
          return {};
        const auto startTime = epochMilliseconds(values["start"]), endTime = epochMilliseconds(values["end"]);
        if (!startTime || !endTime || *startTime >= *endTime || *endTime - *startTime > 366LL * 86400000)
          return {};
        start = values["start"].get<std::string>();
        end = values["end"].get<std::string>();
      } else {
        const auto days = values.value("days", Json(28));
        if (!number(days, 1, 90) || (days != 7 && days != 28 && days != 90))
          return {};
        const auto now = std::chrono::system_clock::now();
        start = utcTimestamp(now - std::chrono::hours(24 * days.get<int>()));
        end = utcTimestamp(now);
      }
      const auto suffix = resource == "growth" ? "/analytics/growth-activation/overview"
          : resource == "retention"            ? "/analytics/growth-activation/retention"
                                               : "/analytics/engagement/overview";
      result.path = base
          + suffix
          + "?start="
          + encode(start)
          + "&end="
          + encode(end)
          + "&interval="
          + std::to_string(values.value("interval", 1));
      break;
    }
    case Section::Channels:
      if (!get || !resource.empty())
        return {};
      result.path = base + "/channels";
      break;
    case Section::Roles:
      if (verb == Verb::Reorder) {
        if ((!resource.empty() && !validSnowflake(resource))
            || !values.is_array()
            || values.empty()
            || values.size() > 250)
          return {};
        std::set<std::string> positions;
        for (const auto& entry : values)
          if (!keys(entry, {"id", "position"})
              || !required(entry, {"id", "position"})
              || !id(entry["id"])
              || entry["id"] == request.guildId
              || !number(entry["position"], 1, 250)
              || !positions.insert(entry["id"].get<std::string>()).second)
            return {};
        if (!resource.empty() && !positions.contains(resource))
          return {};
        result.path = base + "/roles";
        result.method = "PATCH";
        result.body = values;
      } else if (
          !standard(base + "/roles") || ((create || update) && !role(values)) || (remove && resource == request.guildId)
      )
        return {};
      break;
    case Section::Members:
      if (resource == "@me") {
        if (get)
          result.path = "/users/@me/guilds/" + request.guildId + "/member";
        else if (update && keys(values, {"nick"}) && values.contains("nick") && member(values)) {
          result.path = base + "/members/@me";
          result.method = "PATCH";
          result.body = values;
        } else
          return {};
      } else if (get && resource.empty()) {
        // The personal-account management screen uses the indexed member search,
        // rather than the privileged bot member-list endpoint.
        result.path = base + "/members-search";
        result.method = "POST";
        const auto search = memberSearch(request);
        if (!search)
          return {};
        result.body = *search;
      } else if (!standard(base + "/members", false) || (update && !member(values)))
        return {};
      break;
    case Section::Bans:
      if (!standard(base + "/bans", false, false)) {
        if (!create
            || !validSnowflake(resource)
            || !keys(values, {"delete_message_seconds"})
            || (values.contains("delete_message_seconds") && !number(values["delete_message_seconds"], 0, 604800)))
          return {};
        result.path = base + "/bans/" + resource;
        result.method = "PUT";
        result.body = values;
      }
      if (get && resource.empty()) {
        result.path += "?limit=" + std::to_string(request.limit);
        if (!request.cursor.empty())
          result.path += "&after=" + request.cursor;
      }
      break;
    case Section::Invites:
      if (get && resource.empty())
        result.path = base + "/invites";
      else if (remove && code(resource)) {
        result.path = "/invites/" + resource;
        result.method = "DELETE";
      } else if (create && validSnowflake(resource) && keys(values, {"max_age", "max_uses", "temporary", "unique"})) {
        for (const auto& [field, entry] : values.items())
          if ((field == "max_age" && !number(entry, 0, 604800))
              || (field == "max_uses" && !number(entry, 0, 100))
              || ((field == "temporary" || field == "unique") && !entry.is_boolean()))
            return {};
        result.path = "/channels/" + resource + "/invites";
        result.method = "POST";
        result.body = values;
      } else
        return {};
      break;
    case Section::AuditLog:
      if (!get || !resource.empty() || !keys(values, {"user_id", "action_type"}))
        return {};
      result.path = base + "/audit-logs?limit=" + std::to_string(std::min(request.limit, 100u));
      if (!request.cursor.empty())
        result.path += "&before=" + request.cursor;
      if (values.contains("user_id")) {
        if (!id(values["user_id"]))
          return {};
        result.path += "&user_id=" + values["user_id"].get<std::string>();
      }
      if (values.contains("action_type")) {
        if (!number(values["action_type"], 1, 1000))
          return {};
        result.path += "&action_type=" + std::to_string(values["action_type"].get<int>());
      }
      break;
    case Section::Emoji:
    case Section::Stickers:
    case Section::Soundboard: {
      const auto suffix = request.section == Section::Emoji ? "/emojis"
          : request.section == Section::Stickers            ? "/stickers"
                                                            : "/soundboard-sounds";
      if (!standard(base + suffix) || ((create || update) && !expression(values, request.section, create)))
        return {};
      result.stickerUpload = request.section == Section::Stickers && create;
      break;
    }
    case Section::Integrations:
      if (verb == Verb::Sync && validSnowflake(resource) && values.is_object() && values.empty()) {
        result.path = base + "/integrations/" + resource + "/sync";
        result.method = "POST";
      } else {
        if ((get && !resource.empty()) || !standard(base + "/integrations", false))
          return {};
        if (get)
          result.path += "?include_applications=true&include_role_connections_metadata=true";
        if (update) {
          if (!keys(values, {"expire_behavior", "expire_grace_period", "enable_emoticons"}) || values.empty())
            return {};
          if (values.contains("expire_behavior") && !number(values["expire_behavior"], 0, 1))
            return {};
          if (values.contains("expire_grace_period")
              && values["expire_grace_period"] != 1
              && values["expire_grace_period"] != 3
              && values["expire_grace_period"] != 7
              && values["expire_grace_period"] != 14
              && values["expire_grace_period"] != 30)
            return {};
          if (values.contains("enable_emoticons") && !values["enable_emoticons"].is_boolean())
            return {};
        }
      }
      break;
    case Section::Webhooks:
      if (get && resource.empty())
        result.path = base + "/webhooks";
      else if (get && validSnowflake(resource))
        result.path = "/webhooks/" + resource;
      else if (create && validSnowflake(resource)) {
        result.path = "/channels/" + resource + "/webhooks";
        result.method = "POST";
        result.body = values;
      } else if ((update || remove) && validSnowflake(resource)) {
        result.path = "/webhooks/" + resource;
        result.method = remove ? "DELETE" : "PATCH";
        if (update)
          result.body = values;
      } else
        return {};
      if (create || update) {
        if (!keys(values, {"name", "avatar", "channel_id"}) || (create && !values.contains("name")) || values.empty())
          return {};
        for (const auto& [field, entry] : values.items())
          if ((field == "name" && !text(entry, 1, 80))
              || (field == "avatar" && !data(entry, "image", 14 * 1024 * 1024))
              || (field == "channel_id" && (create || !id(entry))))
            return {};
      }
      break;
    case Section::Widget:
      if (!singleton("/widget") || (update && (!keys(values, {"enabled", "channel_id"}) || values.empty())))
        return {};
      if (values.contains("enabled") && !values["enabled"].is_boolean())
        return {};
      if (values.contains("channel_id") && !id(values["channel_id"], true))
        return {};
      break;
    case Section::Welcome:
      if (!singleton("/welcome-screen") || (update && !welcome(values)))
        return {};
      break;
    case Section::Onboarding:
      if (!singleton("/onboarding", true) || (update && !onboarding(values)))
        return {};
      break;
    case Section::Vanity:
      if (!singleton("/vanity-url")
          || (update
              && (!keys(values, {"code"})
                  || !required(values, {"code"})
                  || (!values["code"].is_null()
                      && (!values["code"].is_string() || !code(values["code"].get<std::string>()))))))
        return {};
      break;
    case Section::Templates:
      if (get && resource.empty())
        result.path = base + "/templates";
      else if (create && resource.empty()) {
        result.path = base + "/templates";
        result.method = "POST";
        result.body = values;
      } else if ((update || remove || verb == Verb::Sync) && code(resource)) {
        result.path = base + "/templates/" + resource;
        result.method = remove ? "DELETE" : verb == Verb::Sync ? "PUT" : "PATCH";
        if (update)
          result.body = values;
      } else
        return {};
      if (create || update) {
        if (!keys(values, {"name", "description"}) || (create && !values.contains("name")) || values.empty())
          return {};
        if (values.contains("name") && !text(values["name"], 1, 100))
          return {};
        if (values.contains("description") && !values["description"].is_null() && !text(values["description"], 0, 120))
          return {};
      }
      break;
    case Section::Events:
      if (!standard(base + "/scheduled-events") || ((create || update) && !event(values, create)))
        return {};
      if (get)
        result.path += "?with_user_count=true";
      break;
    case Section::AutoModeration:
      if (!standard(base + "/auto-moderation/rules") || ((create || update) && !automod(values, create)))
        return {};
      break;
    case Section::Prune:
      if ((!get && !create) || !resource.empty() || !keys(values, {"days", "include_roles", "compute_prune_count"}))
        return {};
      if (values.contains("days") && !number(values["days"], 1, 30))
        return {};
      if (values.contains("include_roles") && !ids(values["include_roles"]))
        return {};
      if (values.contains("compute_prune_count") && !values["compute_prune_count"].is_boolean())
        return {};
      result.path = base + "/prune";
      if (get) {
        result.path += "?days=" + std::to_string(values.value("days", 7));
        if (values.contains("include_roles") && !values["include_roles"].empty()) {
          std::string list;
          for (const auto& roleId : values["include_roles"])
            list += (list.empty() ? "" : ",") + roleId.get<std::string>();
          result.path += "&include_roles=" + encode(list);
        }
      } else {
        result.method = "POST";
        result.body = values;
      }
      break;
    case Section::DeleteGuild:
      if (!remove
          || !resource.empty()
          || !keys(values, {"code"})
          || (values.contains("code") && !text(values["code"], 6, 12)))
        return {};
      // Personal accounts use the same explicit deletion route as Discord's client.
      result.path = base + "/delete";
      result.method = "POST";
      if (!values.empty())
        result.body = values;
      break;
    case Section::ServerGuide:
      if (!singleton("/new-member-welcome", true) || (update && !guide(values)))
        return {};
      break;
    case Section::GuideAction:
    case Section::GuideResource:
      if (!update
          || !validSnowflake(resource)
          || !keys(values, {"icon", "emoji"})
          || values.empty()
          || (request.section == Section::GuideResource && values.contains("emoji")))
        return {};
      if (values.contains("icon") && (!data(values["icon"], "image", 14 * 1024 * 1024) || values.contains("emoji")))
        return {};
      if (values.contains("emoji")) {
        const auto& emoji = values["emoji"];
        if (!keys(emoji, {"id", "name", "animated"})
            || (emoji.contains("id") && !id(emoji["id"], true))
            || (emoji.contains("name") && !emoji["name"].is_null() && !text(emoji["name"], 1, 32))
            || (emoji.contains("animated") && !emoji["animated"].is_boolean())
            || (!emoji.contains("id") && !emoji.contains("name")))
          return {};
      }
      result.path =
          base + (request.section == Section::GuideAction ? "/new-member-actions/" : "/resource-channels/") + resource;
      result.method = request.section == Section::GuideAction ? "PATCH" : "PUT";
      result.body = values;
      break;
    case Section::DiscoveryCategory:
      if ((!create && !remove)
          || !validSnowflake(resource)
          || resource.size() > 5
          || std::stoul(resource) > 10000
          || !values.is_object()
          || !values.empty())
        return {};
      result.path = base + "/discovery-categories/" + resource;
      result.method = create ? "PUT" : "DELETE";
      break;
    case Section::IncidentActions:
      if (get && resource.empty()) {
        result.path = base;
        break;
      }
      if (!singleton("/incident-actions", true)
          || !keys(values, {"invites_disabled_until", "dms_disabled_until"})
          || values.empty())
        return {};
      for (const auto& [field, value] : values.items()) {
        if (value.is_null())
          continue;
        const auto until = epochMilliseconds(value);
        const auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (!until || *until < now || *until - now > 86400000)
          return {};
      }
      break;
    case Section::BulkBans:
      if (!create
          || !resource.empty()
          || !keys(values, {"user_ids", "delete_message_seconds"})
          || !values.contains("user_ids")
          || !ids(values["user_ids"], 200)
          || values["user_ids"].empty()
          || (values.contains("delete_message_seconds") && !number(values["delete_message_seconds"], 0, 604800)))
        return {};
      result.path = base + "/bulk-ban/v2";
      result.method = "POST";
      result.body = values;
      break;
    case Section::JoinRequests:
      if (get && resource.empty()) {
        if (!keys(values, {"status", "after"}) || (values.contains("status") && !values["status"].is_string()))
          return {};
        const auto status = values.value("status", std::string("SUBMITTED"));
        if (status != "SUBMITTED" && status != "APPROVED" && status != "REJECTED")
          return {};
        result.path = base + "/requests?status=" + status + "&limit=" + std::to_string(std::min(request.limit, 100u));
        if (!request.cursor.empty())
          result.path += "&before=" + request.cursor;
        if (values.contains("after")) {
          if (!request.cursor.empty() || !id(values["after"]))
            return {};
          result.path += "&after=" + values["after"].get<std::string>();
        }
      } else if (update && validSnowflake(resource)) {
        if (!keys(values, {"action", "rejection_reason"})
            || !values.contains("action")
            || (values["action"] != "APPROVED" && values["action"] != "REJECTED")
            || (values.contains("rejection_reason")
                && !values["rejection_reason"].is_null()
                && !text(values["rejection_reason"], 0, 1000, true)))
          return {};
        result.path = base + "/requests/" + resource;
        result.method = "PATCH";
        result.body = values;
      } else if (get && validSnowflake(resource))
        result.path = "/join-requests/" + resource;
      else
        return {};
      break;
    case Section::JoinInterview:
      if (!create || !validSnowflake(resource) || !values.is_object() || !values.empty())
        return {};
      result.path = "/join-requests/" + resource + "/interview";
      result.method = "POST";
      break;
    case Section::InstalledApplications:
    case Section::ApplicationCommands:
      if (!get || !resource.empty() || !values.is_object() || !values.empty())
        return {};
      result.path =
          base + (request.section == Section::InstalledApplications ? "/applications" : "/application-command-index");
      break;
    case Section::CommandPermissions: {
      if ((!get && !update)
          || !keys(values, {"application_id", "permissions"})
          || !values.contains("application_id")
          || !id(values["application_id"]))
        return {};
      result.path =
          "/applications/" + values["application_id"].get<std::string>() + "/guilds/" + request.guildId + "/commands";
      if (!resource.empty()) {
        if (!validSnowflake(resource))
          return {};
        result.path += "/" + resource;
      } else if (update)
        return {};
      result.path += "/permissions";
      if (get && values.contains("permissions"))
        return {};
      if (update) {
        if (!values.contains("permissions") || !values["permissions"].is_array() || values["permissions"].size() > 100)
          return {};
        std::set<std::pair<int, std::string>> unique;
        for (const auto& permission : values["permissions"]) {
          if (!keys(permission, {"id", "type", "permission"})
              || !required(permission, {"id", "type", "permission"})
              || !id(permission["id"])
              || !number(permission["type"], 1, 3)
              || !permission["permission"].is_boolean()
              || !unique.emplace(permission["type"].get<int>(), permission["id"].get<std::string>()).second)
            return {};
        }
        result.method = "PUT";
        result.body = {{"permissions", values["permissions"]}};
      }
      break;
    }
    case Section::BoostSlots:
      if (!get || !resource.empty() || !values.is_object() || !values.empty())
        return {};
      result.path = "/users/@me/guilds/premium/subscription-slots";
      break;
    case Section::Boosts:
      if (get && resource.empty()) {
        if (!keys(values, {"include_ended"})
            || (values.contains("include_ended") && !values["include_ended"].is_boolean()))
          return {};
        result.path = base + "/premium/subscriptions";
        if (values.value("include_ended", false))
          result.path += "?include_ended=true";
      } else if (create && resource.empty()) {
        if (!keys(values, {"user_premium_guild_subscription_slot_ids", "disable_powerup_auto_apply"})
            || !values.contains("user_premium_guild_subscription_slot_ids")
            || !ids(values["user_premium_guild_subscription_slot_ids"], 100)
            || values["user_premium_guild_subscription_slot_ids"].empty()
            || (values.contains("disable_powerup_auto_apply") && !values["disable_powerup_auto_apply"].is_boolean()))
          return {};
        result.path = base + "/premium/subscriptions";
        result.method = "PUT";
        result.body = values;
      } else if (remove && validSnowflake(resource) && values.is_object() && values.empty()) {
        result.path = base + "/premium/subscriptions/" + resource;
        result.method = "DELETE";
      } else
        return {};
      break;
    case Section::MonetizationRequirements:
    case Section::MonetizationRestrictions:
      if (!get || !resource.empty() || !values.is_object() || !values.empty())
        return {};
      result.path = base
          + (request.section == Section::MonetizationRequirements ? "/creator-monetization/requirements"
                                                                  : "/creator-monetization/restrictions");
      break;
    case Section::Access:
      if (!get && !update)
        return {};
      if (!resource.empty()
          || !keys(values, {"mode", "form_fields", "enabled", "description", "bulk_action"})
          || (get && !values.empty()))
        return {};
      result.path = base + "/member-verification";
      if (update) {
        if (!values.contains("mode") || !values["mode"].is_string())
          return {};
        const auto mode = values["mode"].get<std::string>();
        if (mode != "invite" && mode != "apply" && mode != "discoverable")
          return {};
        auto screening = request;
        screening.section = Section::MembershipScreening;
        screening.values.erase("mode");
        if (!screening.values.empty() && !guildSettingsHttpRequest(screening))
          return {};
        result.method = "PATCH";
        result.body = std::move(screening.values);
      }
      break;
    case Section::MembershipScreening:
      if (!singleton("/member-verification"))
        return {};
      if (update) {
        if (!keys(values, {"enabled", "description", "form_fields", "bulk_action"}) || values.empty())
          return {};
        if (values.contains("enabled") && !values["enabled"].is_boolean())
          return {};
        if (values.contains("description") && !text(values["description"], 0, 300, true))
          return {};
        if (values.contains("bulk_action")
            && !values["bulk_action"].is_null()
            && values["bulk_action"] != "APPROVED"
            && values["bulk_action"] != "REJECTED")
          return {};
        if (values.contains("form_fields")) {
          if (!values["form_fields"].is_array() || values["form_fields"].size() > 5)
            return {};
          for (const auto& field : values["form_fields"]) {
            if (!keys(field, {"field_type", "label", "values", "required", "choices", "description", "placeholder"})
                || !required(field, {"field_type", "label", "required"})
                || !text(field["label"], 1, 300)
                || !field["required"].is_boolean())
              return {};
            const auto type = field["field_type"].is_string() ? field["field_type"].get<std::string>() : std::string{};
            if (type != "TERMS"
                && type != "TEXT_INPUT"
                && type != "PARAGRAPH"
                && type != "MULTIPLE_CHOICE"
                && type != "VERIFICATION")
              return {};
            if (type == "TERMS"
                && (!field.contains("values")
                    || !field["values"].is_array()
                    || field["values"].empty()
                    || field["values"].size() > 16))
              return {};
            if (field.contains("values")) {
              if (!field["values"].is_null() && !field["values"].is_array())
                return {};
              for (const auto& rule : field["values"])
                if (!text(rule, 1, 300))
                  return {};
            }
            if (type == "MULTIPLE_CHOICE"
                && (!field.contains("choices")
                    || !field["choices"].is_array()
                    || field["choices"].empty()
                    || field["choices"].size() > 8))
              return {};
            if (field.contains("choices")) {
              if (!field["choices"].is_array() || field["choices"].size() > 8)
                return {};
              for (const auto& choice : field["choices"])
                if (!text(choice, 1, 150))
                  return {};
            }
            if (field.contains("description")
                && !field["description"].is_null()
                && !text(field["description"], 0, 300, true))
              return {};
            if (field.contains("placeholder") && !field["placeholder"].is_null() && !text(field["placeholder"], 0, 150))
              return {};
          }
        }
      }
      break;
    }
    if (result.path.empty())
      return {};
    return result;
  }

  GuildSettingsResult parseGuildSettings(const Json& value, const GuildSettingsRequest& request) {
    std::size_t nodes = 0;
    bounded(value, 0, nodes);
    if (value.dump().size() > 8 * 1024 * 1024)
      throw std::runtime_error("Server settings response exceeds limits");
    GuildSettingsResult result{value};
    if (value.is_null())
      return result;
    if (request.section == Section::IncidentActions && request.verb == Verb::Get) {
      if (!value.is_object() || !value.contains("id") || value["id"] != request.guildId)
        throw std::runtime_error("Mismatched server incident response");
      result.data = value.value("incidents_data", Json{});
      if (result.data.is_null())
        result.data = Json::object();
      return result;
    }
    if (request.section == Section::JoinRequests) {
      if (value.is_object() && value.contains("guild_id") && value["guild_id"] != request.guildId)
        throw std::runtime_error("Mismatched server application response");
      if (request.verb == Verb::Get && request.resourceId.empty()) {
        if (!value.is_object() || !value.contains("guild_join_requests") || !value["guild_join_requests"].is_array())
          throw std::runtime_error("Invalid server applications");
        const auto& applications = value["guild_join_requests"];
        for (const auto& application : applications)
          if (!application.is_object()
              || !application.contains("guild_id")
              || application["guild_id"] != request.guildId)
            throw std::runtime_error("Mismatched server application response");
        result.more = applications.size() >= std::min(request.limit, 100u);
        if (!applications.empty()) {
          const auto& last = applications.back();
          if (last.is_object() && last.contains("join_request_id") && id(last["join_request_id"]))
            result.cursor = last["join_request_id"].get<std::string>();
        }
        if (result.cursor.empty() || result.cursor == request.cursor)
          result.more = false;
      }
      return result;
    }
    if (request.section == Section::Webhooks && !request.resourceId.empty() && value.is_object()) {
      if (!value.contains("id")
          || value["id"] != request.resourceId
          || !value.contains("guild_id")
          || value["guild_id"] != request.guildId)
        throw std::runtime_error("Mismatched server webhook response");
    }
    if (request.section == Section::Access) {
      if (!value.is_object()
          || !value.contains("guild")
          || !value["guild"].is_object()
          || value["guild"].value("id", std::string{}) != request.guildId
          || !value.contains("screening")
          || (!value["screening"].is_null() && !value["screening"].is_object()))
        throw std::runtime_error("Mismatched server access response");
      return result;
    }
    if (request.section == Section::CommandPermissions) {
      const auto validRecord = [&](const Json& entry) {
        return entry.is_object()
            && entry.contains("guild_id")
            && entry["guild_id"] == request.guildId
            && entry.contains("application_id")
            && entry["application_id"] == request.values["application_id"]
            && (request.resourceId.empty() || (entry.contains("id") && entry["id"] == request.resourceId));
      };
      if (value.is_array()) {
        for (const auto& entry : value)
          if (!validRecord(entry))
            throw std::runtime_error("Mismatched application command permissions");
      } else if (!validRecord(value))
        throw std::runtime_error("Mismatched application command permissions");
      return result;
    }
    if (request.section == Section::ApplicationCommands) {
      if (!value.is_object()
          || !value.contains("applications")
          || !value["applications"].is_array()
          || !value.contains("application_commands")
          || !value["application_commands"].is_array())
        throw std::runtime_error("The server command index is not ready. Please try again.");
      return result;
    }
    if (request.section == Section::Applications) {
      if (!value.is_array())
        throw std::runtime_error("Invalid game suggestions");
      result.data = Json::array();
      for (const auto& game : value) {
        if (result.data.size() >= 50)
          break;
        if (!game.is_object() || !game.contains("id") || !game.contains("name") || !text(game["name"], 1, 200))
          continue;
        Json projected{{"name", game["name"]}};
        if (id(game["id"]))
          projected["id"] = game["id"];
        else if (
            game["id"].is_number_unsigned() || (game["id"].is_number_integer() && game["id"].get<std::int64_t>() > 0)
        )
          projected["id"] = std::to_string(game["id"].get<std::uint64_t>());
        else
          continue;
        if (game.contains("icon")
            && game["icon"].is_string()
            && validAssetHash(game["icon"].get_ref<const std::string&>()))
          projected["icon"] = game["icon"];
        result.data.push_back(std::move(projected));
      }
      return result;
    }
    if (request.section == Section::Insights && value.is_array()) {
      for (auto& interval : result.data)
        if (interval.is_object() && !interval.contains("interval_start_timestamp") && interval.contains("day_pt"))
          interval["interval_start_timestamp"] = interval["day_pt"];
    }
    if (request.section == Section::Overview && value.is_object()) {
      if (!request.values.contains("mfa_level") && (!value.contains("id") || value["id"] != request.guildId))
        throw std::runtime_error("Mismatched server settings response");
    }
    const Json* entries = nullptr;
    if (request.verb == Verb::Get && request.resourceId.empty()) {
      if (value.is_array())
        entries = &value;
      else if (
          request.section == Section::AuditLog
          && value.is_object()
          && value.contains("audit_log_entries")
          && value["audit_log_entries"].is_array()
      )
        entries = &value["audit_log_entries"];
      else if (
          request.section == Section::Soundboard
          && value.is_object()
          && value.contains("items")
          && value["items"].is_array()
      )
        result.data = value["items"];
      else if (request.section == Section::Members && value.is_object()) {
        if (!value.contains("guild_id")
            || value["guild_id"] != request.guildId
            || !value.contains("members")
            || !value["members"].is_array())
          throw std::runtime_error("Invalid indexed member response");
        result.data = Json::array();
        for (const auto& record : value["members"]) {
          if (!record.is_object()
              || !record.contains("member")
              || !record["member"].is_object()
              || !record["member"].contains("user")
              || !record["member"]["user"].is_object()
              || !record["member"]["user"].contains("id")
              || !id(record["member"]["user"]["id"]))
            continue;
          auto member = record["member"];
          for (const auto field :
               {"source_invite_code", "join_source_type", "join_source_application_id", "join_source_channel_id",
                "inviter_id"})
            if (record.contains(field))
              member[field] = record[field];
          result.data.push_back(std::move(member));
        }
        if (!result.data.empty()) {
          const auto& last = result.data.back();
          const auto joined = last.contains("joined_at") ? epochMilliseconds(last["joined_at"]) : std::nullopt;
          if (joined)
            result.cursor = Json{{"guild_joined_at", *joined}, {"user_id", last["user"]["id"]}}.dump();
        }
        result.more = value["members"].size() >= std::min(250u, request.limit)
            && !result.cursor.empty()
            && result.cursor != request.cursor;
        return result;
      }
    }
    if (entries
        && (request.section == Section::Members
            || request.section == Section::Bans
            || request.section == Section::AuditLog)) {
      const auto limit = request.section == Section::AuditLog ? std::min(100u, request.limit) : request.limit;
      result.more = entries->size() >= limit && (request.section != Section::Members || request.query.empty());
      if (!entries->empty()) {
        const auto& last = entries->back();
        const auto& cursor =
            (request.section == Section::Members || request.section == Section::Bans) && last.contains("user")
            ? last["user"]
            : last;
        if (cursor.is_object() && cursor.contains("id") && id(cursor["id"]))
          result.cursor = cursor["id"].get<std::string>();
      }
      if (result.cursor.empty() || result.cursor == request.cursor)
        result.more = false;
    }
    return result;
  }

  Json guildDiscoveryPatch(const Json& current, const Json& changes) {
    if (!current.is_object() || !discovery(changes))
      throw std::runtime_error("Invalid discovery settings");
    Json result = Json::object();
    for (const auto* field :
         {"primary_category_id", "keywords", "emoji_discoverability_enabled", "is_published", "reasons_to_join",
          "social_links", "about"})
      if (current.contains(field))
        result[field] = current[field];
    result.update(changes);
    if (!discovery(result))
      throw std::runtime_error("Invalid discovery snapshot");
    return result;
  }

  Json guildGuidePatch(const Json& current, const Json& changes) {
    if ((!current.is_null() && !current.is_object()) || !guide(changes))
      throw std::runtime_error("Invalid Server Guide settings");
    Json result = Json::object();
    for (const auto* field : {"enabled", "welcome_message", "new_member_actions", "resource_channels"})
      if (current.contains(field))
        result[field] = current[field];
    result.update(changes);
    if (!guide(result))
      throw std::runtime_error("Invalid Server Guide snapshot");
    if (result.value("enabled", false)
        && (!result.contains("welcome_message")
            || result["welcome_message"]["author_ids"].empty()
            || !text(result["welcome_message"]["message"], 7, 300, true)
            || !result.contains("new_member_actions")
            || result["new_member_actions"].size() < 3))
      throw std::runtime_error("Server Guide needs a welcome message, an author and at least three actions");
    return result;
  }

  std::string guildAccessMode(const Json& guild) {
    std::set<std::string> features;
    if (guild.is_object() && guild.contains("features") && guild["features"].is_array())
      for (const auto& feature : guild["features"])
        if (feature.is_string())
          features.insert(feature.get<std::string>());
    if (features.contains("COMMUNITY") && features.contains("DISCOVERABLE"))
      return "discoverable";
    if (features.contains("MEMBER_VERIFICATION_GATE_ENABLED")
        && features.contains("MEMBER_VERIFICATION_MANUAL_APPROVAL"))
      return "apply";
    return "invite";
  }

  GuildAccessPlan guildAccessPlan(const Json& guild, const Json& screening, const GuildSettingsRequest& request) {
    if (request.section != Section::Access
        || request.verb != Verb::Update
        || !guildSettingsHttpRequest(request)
        || !guild.is_object()
        || guild.value("id", std::string{}) != request.guildId
        || !guild.contains("features")
        || !guild["features"].is_array()
        || guild["features"].size() > 256
        || !screening.is_object())
      throw std::runtime_error("Could not verify the current server access settings.");
    GuildAccessPlan result{Json::object(), Json::object(), request.values["mode"].get<std::string>()};
    for (const auto field : {"form_fields", "enabled", "description"})
      if (screening.contains(field) && !(std::string_view(field) == "description" && screening[field].is_null()))
        result.verificationPatch[field] = screening[field];
    for (const auto& [field, value] : request.values.items())
      if (field != "mode")
        result.verificationPatch[field] = value;
    if (!result.verificationPatch.contains("form_fields"))
      result.verificationPatch["form_fields"] = Json::array();
    if (!result.verificationPatch.contains("enabled"))
      result.verificationPatch["enabled"] = false;
    auto verification = request;
    verification.section = Section::MembershipScreening;
    verification.values = result.verificationPatch;
    if (!guildSettingsHttpRequest(verification))
      throw std::runtime_error("The membership form contains unsupported or invalid fields.");
    bool questions = false;
    for (const auto& field : result.verificationPatch["form_fields"])
      questions |= field["field_type"] != "TERMS";
    if (result.mode == "apply" && (!questions || result.verificationPatch["enabled"] != true))
      throw std::runtime_error("Applications require an enabled form with at least one application question.");
    if (result.mode != "apply" && questions)
      throw std::runtime_error("Remove application questions before changing to invite-only or discoverable access.");
    Json features = Json::array();
    bool community = false, changed = false, hadDiscovery = false;
    std::set<std::string> seen;
    for (const auto& feature : guild["features"]) {
      if (!text(feature, 1, 100))
        throw std::runtime_error("Invalid server feature snapshot.");
      const auto name = feature.get<std::string>();
      if (!seen.insert(name).second)
        continue;
      community |= name == "COMMUNITY";
      hadDiscovery |= name == "DISCOVERABLE";
      if ((name == "DISCOVERABLE" && result.mode != "discoverable")
          || (name == "MEMBER_VERIFICATION_MANUAL_APPROVAL" && result.mode == "discoverable"))
        changed = true;
      else
        features.push_back(name);
    }
    if (result.mode == "discoverable") {
      if (!community)
        throw std::runtime_error("Discovery requires a Community server.");
      if (!hadDiscovery) {
        features.push_back("DISCOVERABLE");
        changed = true;
      }
    }
    if (changed)
      result.guildPatch["features"] = std::move(features);
    return result;
  }

  std::uint64_t guildSettingsPermission(const GuildSettingsRequest& request) {
    const bool get = request.verb == Verb::Get;
    switch (request.section) {
    case Section::Overview:
      return get ? 0 : manageGuild;
    case Section::Channels:
      return get ? 0 : manageChannels;
    case Section::Applications:
      return 0;
    case Section::Roles:
      return get ? 0 : manageRoles;
    case Section::Members:
      if (get)
        return 0;
      if (request.verb == Verb::Delete)
        return kick;
      {
        std::uint64_t permissions = 0;
        for (const auto& [field, value] : request.values.items()) {
          if (field == "nick")
            permissions |= nicknames;
          if (field == "roles")
            permissions |= manageRoles;
          if (field == "mute")
            permissions |= muteMembers;
          if (field == "deaf")
            permissions |= deafenMembers;
          if (field == "channel_id")
            permissions |= moveMembers;
          if (field == "communication_disabled_until")
            permissions |= moderate;
          if (field == "flags")
            permissions |= manageRoles;
        }
        return permissions;
      }
    case Section::Bans:
      return ban;
    case Section::AuditLog:
      return audit;
    case Section::Insights:
      return 1ULL << 19;
    case Section::Invites:
      return request.verb == Verb::Create ? invite : manageGuild;
    case Section::Emoji:
    case Section::Stickers:
    case Section::Soundboard:
      return get ? 0 : request.verb == Verb::Create ? createExpressions : expressions;
    case Section::Webhooks:
      return webhooks;
    case Section::Events:
      return get ? 0 : request.verb == Verb::Create ? createEvents : manageEvents;
    case Section::Onboarding:
      return get ? manageGuild : manageGuild | manageRoles;
    case Section::AutoModeration:
      if (!get
          && request.values.is_object()
          && request.values.contains("actions")
          && request.values["actions"].is_array())
        for (const auto& action : request.values["actions"])
          if (action.is_object() && action.contains("type") && action["type"] == 3)
            return manageGuild | moderate;
      return manageGuild;
    case Section::Prune:
      return manageGuild | kick;
    case Section::Access:
      return request.verb != Verb::Get
              && request.values.contains("bulk_action")
              && !request.values["bulk_action"].is_null()
          ? manageGuild | kick
          : manageGuild;
    case Section::CommandPermissions:
      return manageGuild | manageRoles;
    case Section::BoostSlots:
    case Section::Boosts:
      return 0;
    case Section::BulkBans:
      return manageGuild | ban;
    case Section::JoinRequests:
    case Section::JoinInterview:
      return kick;
    case Section::DeleteGuild:
      return 0;
    default:
      return manageGuild;
    }
  }

  bool canManageGuildSettings(const GuildSettingsRequest& request, const GuildSettingsAccess& access) {
    if (access.self.empty() || access.owner.empty())
      return false;
    const bool owner = access.self == access.owner;
    // Existing boost slots belong to the signed-in account, independently of
    // guild management permissions. Only their owner may withdraw an applied slot.
    if (request.section == Section::BoostSlots)
      return request.verb == Verb::Get;
    if (request.section == Section::Boosts)
      return request.verb != Verb::Delete || access.targetCreator == access.self;
    if (request.section == Section::Members
        && request.verb == Verb::Update
        && (request.resourceId == access.self || request.resourceId == "@me")
        && keys(request.values, {"nick"})
        && request.values.contains("nick"))
      return owner || p::allows(access.permissions, p::changeNickname);
    if (request.section == Section::DeleteGuild
        || request.section == Section::OwnershipCode
        || (request.section == Section::Overview && request.values.contains("owner_id")))
      return owner;
    if (request.section == Section::Overview && request.values.contains("mfa_level"))
      return owner;
    if (request.section == Section::Bans && request.verb != Verb::Get && request.resourceId == access.owner)
      return false;
    if (request.section == Section::BulkBans && request.values.is_object() && request.values.contains("user_ids"))
      for (const auto& target : request.values["user_ids"])
        if (target == access.self || target == access.owner)
          return false;
    const bool mutation = request.verb != Verb::Get;
    if (mutation
        && access.targetManaged
        && (request.section == Section::Emoji
            || request.section == Section::Stickers
            || request.section == Section::Soundboard))
      return false;
    if (request.section == Section::Roles
        && mutation
        && (!request.resourceId.empty() || request.verb == Verb::Reorder)) {
      if (access.targetManaged || (request.verb == Verb::Delete && request.resourceId == request.guildId))
        return false;
      if (!owner
          && (access.selfPosition < 0 || access.targetPosition < 0 || access.selfPosition <= access.targetPosition))
        return false;
    }
    if ((request.section == Section::Members || (request.section == Section::Bans && request.verb == Verb::Create))
        && mutation) {
      if (request.resourceId == access.owner || request.resourceId == access.self)
        return false;
      if (!owner
          && (access.selfPosition < 0 || access.targetPosition < 0 || access.selfPosition <= access.targetPosition))
        return false;
    }
    if (owner)
      return true;
    if (!access.permissions)
      return false;
    if (*access.permissions & admin)
      return true;
    if (request.verb != Verb::Get && request.verb != Verb::Create && access.targetCreator == access.self) {
      if ((request.section == Section::Emoji
           || request.section == Section::Stickers
           || request.section == Section::Soundboard)
          && (*access.permissions & createExpressions))
        return true;
      if (request.section == Section::Events && (*access.permissions & createEvents))
        return true;
    }
    auto required = guildSettingsPermission(request);
    // Discord's earlier Manage Expressions/Events grants also include creation.
    auto granted = *access.permissions;
    if (granted & expressions)
      granted |= createExpressions;
    if (granted & manageEvents)
      granted |= createEvents;
    if (request.section == Section::Bans && request.verb == Verb::Get && (granted & audit))
      return true;
    return (granted & required) == required;
  }

  std::string guildRolePermission(std::string decimalValue, unsigned bit, bool enabled) {
    if (!decimal(Json(decimalValue)) || bit > 511)
      throw std::invalid_argument("Invalid role permission");
    std::vector<unsigned char> bits;
    while (decimalValue != "0") {
      std::string quotient;
      unsigned carry = 0;
      for (unsigned char digit : decimalValue) {
        const auto number = carry * 10 + digit - '0';
        if (!quotient.empty() || number / 2)
          quotient += static_cast<char>('0' + number / 2);
        carry = number % 2;
      }
      bits.push_back(static_cast<unsigned char>(carry));
      decimalValue = quotient.empty() ? "0" : std::move(quotient);
    }
    bits.resize(std::max(bits.size(), static_cast<std::size_t>(bit) + 1));
    bits[bit] = enabled;
    std::string result = "0";
    for (auto digit = bits.rbegin(); digit != bits.rend(); ++digit) {
      unsigned carry = *digit;
      for (auto position = result.rbegin(); position != result.rend(); ++position) {
        const unsigned value = static_cast<unsigned>(*position - '0') * 2 + carry;
        *position = static_cast<char>('0' + value % 10);
        carry = value / 10;
      }
      if (carry)
        result.insert(result.begin(), static_cast<char>('0' + carry));
    }
    return result;
  }

  std::span<const GuildAuditAction> guildAuditActions() {
    // Discord's public client constants include newer actions before their
    // developer-resource prose is updated. Keep unknown numeric actions valid.
    static constexpr GuildAuditAction actions[]{
        {1, "Update server", "Server"},
        {10, "Create channel", "Channels"},
        {11, "Update channel", "Channels"},
        {12, "Delete channel", "Channels"},
        {13, "Add channel permissions", "Channels"},
        {14, "Update channel permissions", "Channels"},
        {15, "Remove channel permissions", "Channels"},
        {16, "Reorder channels", "Channels"},
        {20, "Kick member", "Members"},
        {21, "Prune members", "Members"},
        {22, "Ban member", "Members"},
        {23, "Unban member", "Members"},
        {24, "Update member", "Members"},
        {25, "Update member roles", "Members"},
        {26, "Move voice member", "Members"},
        {27, "Disconnect voice member", "Members"},
        {28, "Add bot", "Apps"},
        {30, "Create role", "Roles"},
        {31, "Update role", "Roles"},
        {32, "Delete role", "Roles"},
        {33, "Reorder roles", "Roles"},
        {40, "Create invite", "Invites"},
        {41, "Update invite", "Invites"},
        {42, "Delete invite", "Invites"},
        {50, "Create webhook", "Apps"},
        {51, "Update webhook", "Apps"},
        {52, "Delete webhook", "Apps"},
        {60, "Upload emoji", "Expressions"},
        {61, "Update emoji", "Expressions"},
        {62, "Delete emoji", "Expressions"},
        {72, "Delete message", "Messages"},
        {73, "Bulk delete messages", "Messages"},
        {74, "Pin message", "Messages"},
        {75, "Unpin message", "Messages"},
        {80, "Add integration", "Apps"},
        {81, "Update integration", "Apps"},
        {82, "Remove integration", "Apps"},
        {83, "Create stage", "Voice"},
        {84, "Update stage", "Voice"},
        {85, "Delete stage", "Voice"},
        {90, "Upload sticker", "Expressions"},
        {91, "Update sticker", "Expressions"},
        {92, "Delete sticker", "Expressions"},
        {100, "Create event", "Events"},
        {101, "Update event", "Events"},
        {102, "Delete event", "Events"},
        {110, "Create thread", "Channels"},
        {111, "Update thread", "Channels"},
        {112, "Delete thread", "Channels"},
        {121, "Update command permissions", "Apps"},
        {130, "Upload sound", "Expressions"},
        {131, "Update sound", "Expressions"},
        {132, "Delete sound", "Expressions"},
        {140, "Create AutoMod rule", "Safety"},
        {141, "Update AutoMod rule", "Safety"},
        {142, "Delete AutoMod rule", "Safety"},
        {143, "AutoMod blocked message", "Safety"},
        {144, "AutoMod flagged message", "Safety"},
        {145, "AutoMod timed out member", "Safety"},
        {146, "AutoMod quarantined member", "Safety"},
        {150, "Request creator monetization", "Monetization"},
        {151, "Accept creator monetization terms", "Monetization"},
        {163, "Create onboarding question", "Community"},
        {164, "Update onboarding question", "Community"},
        {165, "Delete onboarding question", "Community"},
        {166, "Create onboarding", "Community"},
        {167, "Update onboarding", "Community"},
        {171, "Feature home item", "Community"},
        {172, "Remove home item", "Community"},
        {180, "Block harmful link", "Safety"},
        {190, "Create Server Guide", "Community"},
        {191, "Update Server Guide", "Community"},
        {192, "Set voice channel status", "Voice"},
        {193, "Remove voice channel status", "Voice"},
        {200, "Create event exception", "Events"},
        {201, "Update event exception", "Events"},
        {202, "Delete event exception", "Events"},
        {210, "Update member applications", "Community"},
        {211, "Update server profile", "Server"},
        {212, "Migrate pin permissions", "Roles"},
        {213, "Migrate slowmode permissions", "Roles"}
    };
    return actions;
  }

  std::string_view guildAuditActionLabel(int type) {
    for (const auto& action : guildAuditActions())
      if (action.type == type)
        return action.name;
    return "Unknown action";
  }

  std::string guildExpressionAsset(GuildSettingsSection section, const Json& value) {
    if (!value.is_object())
      return {};
    const auto idField = section == Section::Soundboard ? "sound_id" : "id";
    if (!value.contains(idField) || !id(value[idField]))
      return {};
    const auto identity = value[idField].get<std::string>();
    if (section == Section::Emoji)
      return mediaUrl(
          "https://cdn.discordapp.com/emojis/"
          + identity
          + (value.contains("animated") && value["animated"] == true ? ".gif?size=128" : ".png?size=128")
      );
    if (section == Section::Soundboard)
      return mediaUrl("https://cdn.discordapp.com/soundboard-sounds/" + identity);
    if (section == Section::Stickers) {
      const auto format = value.contains("format_type") && value["format_type"].is_number_integer()
          ? value["format_type"].get<int>()
          : 0;
      if (format == 1 || format == 2)
        return mediaUrl("https://cdn.discordapp.com/stickers/" + identity + ".png");
      if (format == 3)
        return mediaUrl("https://cdn.discordapp.com/stickers/" + identity + ".json");
      if (format == 4)
        return mediaUrl("https://media.discordapp.net/stickers/" + identity + ".gif");
    }
    return {};
  }
} // namespace api
