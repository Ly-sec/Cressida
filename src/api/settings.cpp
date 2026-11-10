#include "api/emoji.h"
#include "api/gifs.h"
#include "api/models.h"
#include "api/server_organization.h"
#include "api/server_privacy.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string_view>

namespace api {
  namespace {
    // Read bounded wire fields without a generated schema. Explicit edits retain
    // unknown fields in each complete group sent back to Discord.
    class Wire {
    public:
      explicit Wire(std::string_view input) : bytes(input) {}
      std::string_view bytes;
      unsigned field = 0, type = 0;
      std::uint64_t number = 0;
      std::string_view data;
      std::uint64_t varint() {
        std::uint64_t result = 0;
        for (int shift = 0; shift < 70; shift += 7) {
          if (bytes.empty())
            throw std::runtime_error("Truncated settings");
          const auto byte = static_cast<unsigned char>(bytes.front());
          bytes.remove_prefix(1);
          if (shift == 63 && byte > 1)
            throw std::runtime_error("Settings integer overflow");
          result |= std::uint64_t(byte & 127) << shift;
          if (!(byte & 128))
            return result;
        }
        throw std::runtime_error("Invalid settings integer");
      }
      std::string_view take(std::uint64_t count) {
        if (count > bytes.size())
          throw std::runtime_error("Truncated settings field");
        const auto result = bytes.substr(0, count);
        bytes.remove_prefix(count);
        return result;
      }
      bool next() {
        if (bytes.empty())
          return false;
        const auto tag = varint();
        field = tag >> 3;
        type = tag & 7;
        number = 0;
        data = {};
        if (!field)
          throw std::runtime_error("Invalid settings field");
        if (type == 0)
          number = varint();
        else if (type == 1 || type == 5) {
          data = take(type == 1 ? 8 : 4);
          for (std::size_t i = 0; i < data.size(); ++i)
            number |= std::uint64_t(static_cast<unsigned char>(data[i])) << (i * 8);
        } else if (type == 2)
          data = take(varint());
        else
          throw std::runtime_error("Unsupported settings wire type");
        return true;
      }
    };

    std::string base64(std::string_view input) {
      if (input.size() > 16 * 1024 * 1024)
        throw std::runtime_error("Settings too large");
      constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string output;
      std::uint32_t bits = 0;
      int count = 0;
      bool padding = false;
      for (const auto ch : input) {
        if (ch == '=') {
          padding = true;
          continue;
        }
        const auto value = alphabet.find(ch);
        if (padding || value == std::string_view::npos)
          throw std::runtime_error("Invalid settings encoding");
        bits = (bits << 6) | static_cast<unsigned int>(value);
        count += 6;
        if (count >= 8) {
          count -= 8;
          output += static_cast<char>((bits >> count) & 255);
        }
      }
      return output;
    }

    void ids(const Wire& field, std::vector<std::string>& output) {
      if (field.type == 1)
        output.push_back(std::to_string(field.number));
      else if (field.type == 2) {
        Wire packed(field.data);
        while (!packed.bytes.empty()) {
          const auto bytes = packed.take(8);
          std::uint64_t number = 0;
          for (int i = 0; i < 8; ++i)
            number |= std::uint64_t(static_cast<unsigned char>(bytes[i])) << (i * 8);
          output.push_back(std::to_string(number));
        }
      }
    }
  } // namespace

  bool validStatus(std::string_view status) {
    return status == "online" || status == "idle" || status == "dnd" || status == "invisible";
  }
  namespace {
    std::string encodedSettings(const nlohmann::json& value) {
      return value.value("settings", value.value("proto", std::string{}));
    }
    void integer(std::string& output, std::uint64_t value) {
      while (value >= 128) {
        output += static_cast<char>((value & 127) | 128);
        value >>= 7;
      }
      output += static_cast<char>(value);
    }
    std::string field(unsigned id, std::string_view value) {
      std::string output;
      integer(output, (id << 3) | 2);
      integer(output, value.size());
      output += value;
      return output;
    }
    std::string encode64(std::string_view bytes) {
      constexpr std::string_view chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string result;
      for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        result += chars[a >> 2];
        result += chars[((a & 3) << 4) | (b >> 4)];
        result += i + 1 < bytes.size() ? chars[((b & 15) << 2) | (c >> 6)] : '=';
        result += i + 2 < bytes.size() ? chars[c & 63] : '=';
      }
      return result;
    }
  } // namespace
  EmojiPreferences parseEmojiPreferences(const nlohmann::json& value) {
    if (!value.is_object() || (!value.contains("settings") && !value.contains("proto")))
      throw std::runtime_error("Unreadable emoji settings");
    const auto bytes = base64(encodedSettings(value));
    Wire root(bytes);
    EmojiPreferences result;
    std::vector<std::pair<std::uint64_t, std::string>> usage;
    struct ChannelUsage {
      std::string id;
      std::uint64_t score = 0, recent = 0;
    };
    std::vector<ChannelUsage> channels;
    constexpr unsigned channelFrecencyField = 12;
    constexpr std::size_t maximumChannelFrecencyEntries = 4096;
    while (root.next()) {
      if (root.type == 2 && root.field == channelFrecencyField) {
        Wire items(root.data);
        while (items.next()) {
          if (items.field != 1 || items.type != 2)
            continue;
          if (channels.size() >= maximumChannelFrecencyEntries)
            throw std::runtime_error("Too many channel usage entries");
          Wire entry(items.data);
          ChannelUsage channel;
          std::uint64_t uses = 0, frequency = 0, score = 0;
          while (entry.next()) {
            if (entry.field == 1 && entry.type == 1)
              channel.id = std::to_string(entry.number);
            else if (entry.field == 2 && entry.type == 2) {
              Wire counts(entry.data);
              while (counts.next()) {
                if (counts.field == 1 && counts.type == 0)
                  uses = std::min<std::uint64_t>(counts.number, std::numeric_limits<std::uint32_t>::max());
                else if (
                    counts.field == 3 && counts.type == 0 && counts.number <= std::numeric_limits<std::int32_t>::max()
                )
                  frequency = counts.number;
                else if (
                    counts.field == 4 && counts.type == 0 && counts.number <= std::numeric_limits<std::int32_t>::max()
                )
                  score = counts.number;
                else if (counts.field == 2 && counts.type == 0)
                  channel.recent = std::max(channel.recent, counts.number);
                else if (counts.field == 2 && counts.type == 2) {
                  Wire packed(counts.data);
                  while (!packed.bytes.empty())
                    channel.recent = std::max(channel.recent, packed.varint());
                }
              }
            }
          }
          channel.score = score ? score : frequency ? frequency : uses;
          if (validSnowflake(channel.id) && (channel.score || channel.recent))
            channels.push_back(std::move(channel));
        }
        continue;
      }
      if (root.type != 2 || (root.field != 5 && root.field != 6))
        continue;
      const auto group = root.field;
      Wire fields(root.data);
      while (fields.next()) {
        if (fields.field != 1 || fields.type != 2)
          continue;
        if (group == 5) {
          std::string key(fields.data);
          if (validEmojiKey(key))
            result.favorites.push_back(std::move(key));
          if (result.favorites.size() > 4096)
            throw std::runtime_error("Too many favorite emoji");
        } else {
          Wire entry(fields.data);
          std::string key;
          std::uint64_t uses = 0, frecency = 0, score = 0;
          while (entry.next()) {
            if (entry.field == 1 && entry.type == 2)
              key = entry.data;
            if (entry.field == 2 && entry.type == 2) {
              Wire counts(entry.data);
              while (counts.next()) {
                if (counts.field == 1 && counts.type == 0)
                  uses = counts.number;
                else if (counts.field == 3 && counts.type == 0 && counts.number <= 2147483647)
                  frecency = counts.number;
                else if (counts.field == 4 && counts.type == 0 && counts.number <= 2147483647)
                  score = counts.number;
              }
            }
          }
          const auto rank = score ? score : frecency ? frecency : uses;
          if (rank && validEmojiKey(key) && usage.size() < 4096)
            usage.emplace_back(rank, std::move(key));
        }
      }
    }
    std::ranges::sort(usage, [](const auto& a, const auto& b) {
      return a.first > b.first || (a.first == b.first && a.second < b.second);
    });
    for (const auto& [_, key] : usage)
      if (result.frequent.size() < 64)
        result.frequent.push_back(key);
    std::ranges::sort(channels, [](const auto& a, const auto& b) {
      return a.score != b.score ? a.score > b.score : a.recent != b.recent ? a.recent > b.recent : a.id < b.id;
    });
    std::set<std::string> seenChannels;
    for (const auto& channel : channels)
      if (seenChannels.insert(channel.id).second)
        result.frequentChannels.push_back(channel.id);
    return result;
  }
  nlohmann::json emojiFavoritePatch(const nlohmann::json& snapshot, const std::string& key, bool add) {
    if (!validEmojiKey(key))
      throw std::runtime_error("Invalid favorite emoji");
    const auto bytes = base64(encodedSettings(snapshot));
    Wire root(bytes);
    std::optional<std::uint64_t> version;
    std::string block;
    bool exists = false;
    while (root.next()) {
      if (root.field == 1 && root.type == 2) {
        Wire versions(root.data);
        while (versions.next())
          if (versions.field == 3 && versions.type == 0)
            version = versions.number;
      }
      if (root.field != 5 || root.type != 2)
        continue;
      Wire favorites(root.data);
      while (!favorites.bytes.empty()) {
        const auto before = favorites.bytes;
        favorites.next();
        const bool match = favorites.field == 1 && favorites.type == 2 && favorites.data == key;
        exists |= match;
        if (!match || add)
          block += before.substr(0, before.size() - favorites.bytes.size());
      }
    }
    if (!version)
      throw std::runtime_error("Emoji settings need a data version");
    if (add && !exists)
      block += field(1, key);
    return {{"settings", encode64(field(5, block))}, {"required_data_version", *version}};
  }
  std::vector<Gif> parseGifFavorites(const nlohmann::json& value) {
    if (!value.is_object() || (!value.contains("settings") && !value.contains("proto")))
      throw std::runtime_error("Unreadable favorite GIF settings");
    const auto bytes = base64(encodedSettings(value));
    Wire root(bytes);
    std::map<std::string, Gif> byUrl;
    while (root.next()) {
      if (root.field != 2 || root.type != 2)
        continue;
      Wire favorites(root.data);
      while (favorites.next()) {
        if (favorites.field != 1 || favorites.type != 2)
          continue;
        Wire entry(favorites.data);
        Gif gif;
        while (entry.next()) {
          if (entry.field == 1 && entry.type == 2)
            gif.url = entry.data;
          if (entry.field == 2 && entry.type == 2) {
            Wire fields(entry.data);
            while (fields.next()) {
              if (fields.field == 1 && fields.type == 0)
                gif.format = fields.number;
              if (fields.field == 2 && fields.type == 2)
                gif.src = mediaUrl(fields.data);
              if (fields.field == 3 && fields.type == 0 && fields.number <= 16384)
                gif.width = fields.number;
              if (fields.field == 4 && fields.type == 0 && fields.number <= 16384)
                gif.height = fields.number;
              if (fields.field == 5 && fields.type == 0 && fields.number <= UINT32_MAX)
                gif.order = fields.number;
            }
          }
        }
        if (validGif(gif))
          byUrl[gif.url] = std::move(gif);
        if (byUrl.size() > 1000)
          throw std::runtime_error("Too many favorite GIFs");
      }
    }
    std::vector<Gif> result;
    for (auto& [_, gif] : byUrl)
      result.push_back(std::move(gif));
    std::ranges::sort(result, [](const auto& a, const auto& b) {
      return a.order > b.order || (a.order == b.order && a.url < b.url);
    });
    return result;
  }
  nlohmann::json gifFavoritePatch(const nlohmann::json& snapshot, const Gif& gif, bool add) {
    if (!validGif(gif))
      throw std::runtime_error("Invalid favorite GIF");
    const auto bytes = base64(encodedSettings(snapshot));
    Wire root(bytes);
    std::optional<std::uint64_t> version;
    std::string block;
    std::uint64_t lastOrder = 0;
    bool exists = false;
    while (root.next()) {
      if (root.field == 1 && root.type == 2) {
        Wire versions(root.data);
        while (versions.next())
          if (versions.field == 3 && versions.type == 0)
            version = versions.number;
      }
      if (root.field != 2 || root.type != 2)
        continue;
      Wire favorites(root.data);
      while (!favorites.bytes.empty()) {
        const auto before = favorites.bytes;
        favorites.next();
        bool retain = true;
        if (favorites.field == 1 && favorites.type == 2) {
          Wire entry(favorites.data);
          std::string key;
          while (entry.next()) {
            if (entry.field == 1 && entry.type == 2)
              key = entry.data;
            if (entry.field == 2 && entry.type == 2) {
              Wire fields(entry.data);
              while (fields.next())
                if (fields.field == 5 && fields.type == 0)
                  lastOrder = std::max(lastOrder, fields.number);
            }
          }
          if (key == gif.url) {
            exists = true;
            retain = add;
          }
        }
        if (retain)
          block += before.substr(0, before.size() - favorites.bytes.size());
      }
    }
    if (!version)
      throw std::runtime_error("Favorite GIF settings need a data version");
    if (add && !exists) {
      if (lastOrder >= UINT32_MAX)
        throw std::runtime_error("Favorite GIF order is out of range");
      std::string value;
      auto scalar = [&](unsigned id, std::uint64_t number) {
        integer(value, id << 3);
        integer(value, number);
      };
      scalar(1, gif.format);
      value += field(2, gif.src);
      scalar(3, gif.width);
      scalar(4, gif.height);
      scalar(5, lastOrder + 1);
      block += field(1, field(1, gif.url) + field(2, value));
    }
    return {{"settings", encode64(field(2, block))}, {"required_data_version", *version}};
  }
  std::optional<Presence> parsePresenceSettings(const nlohmann::json& value) {
    auto bytes = base64(encodedSettings(value));
    Wire root(bytes);
    while (root.next())
      if (root.field == 11 && root.type == 2) {
        Presence result;
        Wire status(root.data);
        while (status.next()) {
          if (status.field == 1 && status.type == 2) {
            Wire wrapped(status.data);
            while (wrapped.next())
              if (wrapped.field == 1 && wrapped.type == 2)
                result.status = wrapped.data;
          }
          if (status.field == 2 && status.type == 2) {
            Wire custom(status.data);
            while (custom.next()) {
              if (custom.field == 1 && custom.type == 2)
                result.text = custom.data;
              if (custom.field == 2 && custom.type == 1 && custom.number)
                result.emojiId = std::to_string(custom.number);
              if (custom.field == 3 && custom.type == 2)
                result.emojiName = custom.data;
            }
          }
        }
        return result;
      }
    return {};
  }
  nlohmann::json presenceSettingsPatch(
      const nlohmann::json& current, const std::string& status, const std::optional<std::string>& customText
  ) {
    if (!validStatus(status) || (customText && utf8Length(*customText) > 128))
      throw std::runtime_error("Invalid presence");
    const auto bytes = base64(encodedSettings(current));
    Wire root(bytes);
    std::string block;
    std::optional<std::uint64_t> version;
    while (root.next()) {
      if (root.field == 1 && root.type == 2) {
        Wire versions(root.data);
        while (versions.next())
          if (versions.field == 3 && versions.type == 0)
            version = versions.number;
      }
      if (root.field != 11 || root.type != 2)
        continue;
      Wire settings(root.data);
      while (!settings.bytes.empty()) {
        const auto before = settings.bytes;
        settings.next();
        if (settings.field != 1 && !(customText && settings.field == 2))
          block += before.substr(0, before.size() - settings.bytes.size());
      }
    }
    block += field(1, field(1, status));
    if (customText)
      block += field(2, customText->empty() ? std::string{} : field(1, *customText));
    nlohmann::json result{{"settings", encode64(field(11, block))}};
    if (version)
      result["required_data_version"] = *version;
    return result;
  }

  namespace {
    struct SettingField {
      const char* key;
      unsigned group, id;
      bool wrapped;
      int fallback, maximum;
      unsigned parent = 0;
      bool locale = false;
    };
    constexpr SettingField fields[] = {
        {"restrict_new_guild_dms", 8, 27, true, 0, 1},
        {"restrict_message_requests", 8, 17, true, 0, 1},
        {"friend_sources", 8, 11, true, 14, 14},
        {"contact_sync", 8, 10, true, 0, 1},
        {"hide_legacy_username", 8, 22, true, 0, 1},
        {"conversation_warnings", 8, 23, true, 1, 1},
        {"recent_games", 8, 24, true, 1, 1},
        {"profile_visibility", 8, 32, false, 0, 3},
        {"join_friends", 8, 1, true, 1, 1},
        {"join_voice", 8, 2, true, 1, 1},
        {"show_activity", 11, 3, true, 1, 1},
        {"in_app_notifications", 7, 1, true, 1, 1},
        {"notify_friends_stream", 7, 2, true, 0, 1},
        {"friend_online_notifications", 7, 12, true, 1, 1},
        {"profile_notifications", 7, 16, true, 1, 1},
        {"server_event_notifications", 7, 22, true, 1, 1},
        {"notify_friends_profile", 7, 24, true, 1, 1},
        {"notify_friends_online", 7, 25, true, 1, 1},
        {"reaction_notifications", 7, 7, false, 0, 2},
        {"dm_spam_filter", 6, 27, false, 0, 3},
        {"detect_platform_accounts", 8, 8, true, 1, 1, 0, false},
        {"drops_opted_out", 8, 18, true, 0, 1, 0, false},
        {"non_spam_retraining", 8, 19, true, 0, 1, 0, false},
        {"game_friend_dms", 8, 26, true, 1, 1, 0, false},
        {"quests_data_opted_out", 8, 30, true, 0, 1, 0, false},
        {"show_local_time", 8, 31, true, 0, 1, 0, false},
        {"hide_friend_request_notes", 8, 33, true, 0, 1, 0, false},
        {"message_promotions_opted_out", 8, 35, true, 0, 1, 0, false},
        {"leaderboard_opt_out", 8, 25, false, 0, 1, 0, false},
        {"activity_restriction", 8, 29, false, 0, 3, 0, false},
        {"burst_reaction_notifications", 7, 4, true, 1, 1, 0, false},
        {"quiet_mode", 7, 5, true, 0, 1, 0, false},
        {"voice_activity_notifications", 7, 11, true, 1, 1, 0, false},
        {"returning_friend_notifications", 7, 13, true, 1, 1, 0, false},
        {"friend_anniversary_notifications", 7, 14, true, 1, 1, 0, false},
        {"game_update_notifications", 7, 15, true, 1, 1, 0, false},
        {"trending_server_notifications", 7, 17, true, 1, 1, 0, false},
        {"dm_reply_reminders", 7, 18, true, 1, 1, 0, false},
        {"summary_reminders", 7, 19, true, 1, 1, 0, false},
        {"group_reaction_notifications", 7, 20, true, 1, 1, 0, false},
        {"friend_game_notifications", 7, 21, true, 1, 1, 0, false},
        {"downtime_notifications", 7, 23, true, 1, 1, 0, false},
        {"notify_server_stream", 7, 26, true, 1, 1, 0, false},
        {"exclude_steam_notifications", 7, 10, true, 0, 1, 0, false},
        {"game_activity_notifications", 7, 8, false, 0, 3, 0, false},
        {"custom_status_notifications", 7, 9, false, 0, 2, 0, false},
        {"always_preview_video", 5, 5, true, 1, 1, 0, false},
        {"stream_notifications", 5, 7, true, 1, 1, 0, false},
        {"disable_stream_previews", 5, 10, true, 0, 1, 0, false},
        {"afk_timeout", 5, 6, true, 600, 3600, 0, false},
        {"clips_voice_recording", 19, 1, true, 1, 1, 0, false},
        {"explicit_guild_media", 6, 1, false, 0, 3, 29, false},
        {"explicit_friend_media", 6, 2, false, 0, 3, 29, false},
        {"explicit_other_media", 6, 3, false, 0, 3, 29, false},
        {"gore_guild_media", 6, 1, false, 0, 3, 32, false},
        {"gore_friend_media", 6, 2, false, 0, 3, 32, false},
        {"gore_other_media", 6, 3, false, 0, 3, 32, false},
        {"filter_profanity", 6, 1, true, 0, 1, 30, false},
        {"filter_sexual", 6, 2, true, 0, 1, 30, false},
        {"filter_slurs", 6, 3, true, 0, 1, 30, false},
        {"safety_preset", 21, 1, false, 0, 4, 0, false},
        {"locale", 12, 1, true, 0, 31, 0, true},
    };
    constexpr std::string_view locales[] = {"en-US", "en-GB", "bg",     "zh-CN", "zh-TW", "hr", "cs",    "da",
                                            "nl",    "fi",    "fr",     "de",    "el",    "hi", "hu",    "id",
                                            "it",    "ja",    "ko",     "lt",    "no",    "pl", "pt-BR", "ro",
                                            "ru",    "es-ES", "es-419", "sv-SE", "th",    "tr", "uk",    "vi"};
    const SettingField* settingField(std::string_view key) {
      for (const auto& f : fields)
        if (f.key == key)
          return &f;
      return nullptr;
    }
    std::string numericField(unsigned id, std::uint64_t value) {
      std::string output;
      integer(output, id << 3);
      integer(output, value);
      return output;
    }
  } // namespace
  bool validSettingChanges(const SettingValues& changes) {
    if (changes.empty() || changes.size() > std::size(fields))
      return false;
    for (const auto& [key, value] : changes) {
      const auto* f = settingField(key);
      if (!f || value < 0 || value > f->maximum)
        return false;
      if (key == "friend_sources" && (value & ~14 || ((value & 8) && (value & 6) != 6)))
        return false;
    }
    return true;
  }
  namespace {
    std::optional<std::string> nestedField(std::string_view bytes, unsigned id) {
      Wire input(bytes);
      std::optional<std::string> result;
      while (input.next()) {
        if (input.field == id && input.type == 2) {
          if (!result)
            result.emplace();
          result->append(input.data);
        }
      }
      return result;
    }
    struct SettingsEdit {
      std::optional<std::string> replacement;
      std::map<unsigned, SettingsEdit> children;
    };
    std::string mergeSettings(std::string_view bytes, const std::map<unsigned, SettingsEdit>& edits) {
      Wire input(bytes);
      std::string result;
      std::map<unsigned, std::string> originals;
      while (!input.bytes.empty()) {
        const auto before = input.bytes;
        input.next();
        if (!edits.contains(input.field))
          result.append(before.substr(0, before.size() - input.bytes.size()));
        else if (input.type == 2)
          originals[input.field].append(input.data);
      }
      for (const auto& [id, edit] : edits)
        result += edit.replacement ? *edit.replacement : field(id, mergeSettings(originals[id], edit.children));
      return result;
    }
  } // namespace
  SettingValues parseAccountSettings(const nlohmann::json& value) {
    SettingValues result;
    const auto bytes = base64(encodedSettings(value));
    for (const auto& f : fields) {
      result[f.key] = f.fallback;
      auto container = nestedField(bytes, f.group);
      if (container && f.parent)
        container = nestedField(*container, f.parent);
      if (!container)
        continue;
      std::uint64_t number = f.fallback;
      if (f.wrapped) {
        if (const auto data = nestedField(*container, f.id)) {
          number = 0;
          Wire wrapper(*data);
          while (wrapper.next()) {
            if (wrapper.field != 1)
              continue;
            if (f.locale && wrapper.type == 2) {
              const auto language = std::ranges::find(locales, wrapper.data);
              if (language != std::end(locales))
                number = language - std::begin(locales);
            } else if (!f.locale && wrapper.type == 0)
              number = wrapper.number;
          }
        }
      } else {
        Wire input(*container);
        while (input.next())
          if (input.field == f.id && input.type == 0)
            number = input.number;
      }
      if (number <= static_cast<std::uint64_t>(f.maximum))
        result[f.key] = number;
    }
    return result;
  }
  nlohmann::json accountSettingsPatch(const nlohmann::json& snapshot, const SettingValues& changes) {
    if (!validSettingChanges(changes))
      throw std::runtime_error("Invalid settings edits");
    const auto bytes = base64(encodedSettings(snapshot));
    std::optional<std::uint64_t> version;
    if (const auto versions = nestedField(bytes, 1)) {
      Wire input(*versions);
      while (input.next())
        if (input.field == 3 && input.type == 0)
          version = input.number;
    }
    if (!version)
      throw std::runtime_error("Cannot save settings without an account data version");
    std::map<unsigned, SettingsEdit> edits;
    for (const auto& [key, value] : changes) {
      const auto& f = *settingField(key);
      auto* edit = &edits[f.group];
      if (f.parent)
        edit = &edit->children[f.parent];
      if (f.wrapped) {
        auto& wrapper = edit->children[f.id].children[1];
        wrapper.replacement = f.locale ? field(1, locales[value]) : numericField(1, value);
      } else
        edit->children[f.id].replacement = numericField(f.id, value);
    }
    // PATCH contains complete changed groups; untouched groups are omitted. Preserve
    // unknown fields recursively, including inside wrappers and content filters.
    std::string edited;
    for (const auto& [id, edit] : edits) {
      std::string original;
      Wire input(bytes);
      while (input.next())
        if (input.field == id && input.type == 2)
          original.append(input.data);
      edited += field(id, mergeSettings(original, edit.children));
    }
    return {{"settings", encode64(edited)}, {"required_data_version", *version}};
  }

  namespace {
    constexpr std::size_t maximumFolderNameCharacters = 100;
    constexpr std::size_t maximumFolderNameBytes = maximumFolderNameCharacters * 4;
    std::uint64_t settingsId(const std::string& text, bool allowZero = false) {
      std::uint64_t value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (text.empty()
          || parsed.ec != std::errc{}
          || parsed.ptr != text.data() + text.size()
          || (!allowZero && !value)
          || std::to_string(value) != text)
        throw std::runtime_error("Invalid server organization identifier");
      return value;
    }
    std::string organizationSnapshot(const nlohmann::json& snapshot) {
      if (!snapshot.is_object())
        throw std::runtime_error("Unreadable account organization settings");
      const auto at = snapshot.find(snapshot.contains("settings") ? "settings" : "proto");
      if (at == snapshot.end() || !at->is_string())
        throw std::runtime_error("Unreadable account organization settings");
      return base64(at->get_ref<const std::string&>());
    }
    std::uint64_t organizationVersion(std::string_view bytes) {
      std::optional<std::uint64_t> version;
      if (const auto versions = nestedField(bytes, 1)) {
        Wire input(*versions);
        while (input.next())
          if (input.field == 3) {
            if (input.type != 0 || input.number > UINT32_MAX)
              throw std::runtime_error("Invalid account settings data version");
            version = input.number;
          }
      }
      if (!version)
        throw std::runtime_error("Cannot save organization settings without an account data version");
      return *version;
    }
    std::string fixedId(unsigned id, std::uint64_t value) {
      std::string output;
      integer(output, (id << 3) | 1);
      for (int byte = 0; byte < 8; ++byte)
        output += static_cast<char>((value >> (byte * 8)) & 255);
      return output;
    }
    std::vector<std::string> organizationIds(std::string_view bytes, unsigned id, std::size_t maximum = 10000) {
      Wire input(bytes);
      std::vector<std::string> result;
      while (input.next()) {
        if (input.field != id)
          continue;
        if ((input.type != 1 && input.type != 2) || (input.type == 2 && input.data.size() % 8))
          throw std::runtime_error("Invalid server list in account settings");
        const auto count = input.type == 1 ? 1 : input.data.size() / 8;
        if (count > maximum - result.size())
          throw std::runtime_error("Too many servers in account settings");
        ids(input, result);
      }
      for (const auto& value : result)
        settingsId(value);
      return result;
    }
    std::string organizationSnapshot(const nlohmann::json& snapshot, std::span<const std::string> joinedGuilds) {
      auto bytes = organizationSnapshot(snapshot);
      if (joinedGuilds.empty())
        return bytes;
      constexpr std::size_t maximumJoinedGuilds = 10000;
      if (joinedGuilds.size() > maximumJoinedGuilds)
        throw std::runtime_error("Too many joined servers in account organization");
      const auto group = nestedField(bytes, 14).value_or("");
      const auto positions = organizationIds(group, 2);
      std::set<std::string> known(positions.begin(), positions.end());
      Wire folders(group);
      while (folders.next())
        if (folders.field == 1 && folders.type == 2)
          for (const auto& id : organizationIds(folders.data, 1, 100))
            known.insert(id);
      std::string added;
      for (const auto& id : joinedGuilds) {
        const auto number = settingsId(id);
        if (known.insert(id).second)
          added += field(1, fixedId(1, number));
      }
      if (!added.empty())
        bytes += field(14, added);
      return bytes;
    }
    // Keep untouched wire fields byte for byte. Filtering a packed list changes
    // only its length and selected IDs, including for future/unknown guilds.
    std::string removeOrganizationId(std::string_view bytes, unsigned id, std::uint64_t guild) {
      Wire input(bytes);
      std::string output;
      while (!input.bytes.empty()) {
        const auto before = input.bytes;
        input.next();
        const auto raw = before.substr(0, before.size() - input.bytes.size());
        if (input.field != id || (input.type == 1 && input.number != guild)) {
          output.append(raw);
          continue;
        }
        if (input.type == 1)
          continue;
        if (input.type != 2 || input.data.size() % 8)
          throw std::runtime_error("Invalid server list in account settings");
        Wire packed(input.data);
        std::string retained;
        bool removed = false;
        while (!packed.bytes.empty()) {
          const auto value = packed.take(8);
          std::uint64_t number = 0;
          for (int byte = 0; byte < 8; ++byte)
            number |= std::uint64_t(static_cast<unsigned char>(value[byte])) << (byte * 8);
          if (number == guild)
            removed = true;
          else
            retained.append(value);
        }
        if (!removed)
          output.append(raw);
        else if (!retained.empty())
          output += field(id, retained);
      }
      return output;
    }
  } // namespace

  GuildPrivacy parseGuildPrivacy(const nlohmann::json& snapshot, const std::string& guild) {
    settingsId(guild);
    const auto bytes = organizationSnapshot(snapshot);
    const auto privacy = nestedField(bytes, 8).value_or("");
    const auto allowed = [&](unsigned id) {
      const auto restricted = organizationIds(privacy, id);
      return std::ranges::find(restricted, guild) == restricted.end();
    };
    return {allowed(3), allowed(13), allowed(15), allowed(16)};
  }
  nlohmann::json
  guildPrivacyPatch(const nlohmann::json& snapshot, const std::string& guild, const GuildPrivacy& changes) {
    const auto number = settingsId(guild);
    if (changes.empty())
      throw std::runtime_error("No server privacy settings were changed");
    const auto bytes = organizationSnapshot(snapshot);
    const auto version = organizationVersion(bytes);
    auto privacy = nestedField(bytes, 8).value_or("");
    for (const auto& [id, allow] : std::vector<std::pair<unsigned, std::optional<bool>>>{
             {3, changes.directMessages},
             {13, changes.activity},
             {15, changes.activityJoining},
             {16, changes.messageRequests}
         }) {
      if (!allow)
        continue;
      const auto restricted = organizationIds(privacy, id);
      const bool contains = std::ranges::find(restricted, guild) != restricted.end();
      if (*allow && contains)
        privacy = removeOrganizationId(privacy, id, number);
      else if (!*allow && !contains) {
        if (restricted.size() >= 10000)
          throw std::runtime_error("Too many server privacy restrictions");
        privacy += fixedId(id, number);
      }
    }
    return {{"settings", encode64(field(8, privacy))}, {"required_data_version", version}};
  }

  nlohmann::json guildMovePatch(
      const nlohmann::json& snapshot, const std::string& guild, const std::string& folderId,
      const std::optional<std::string>& newFolderName, std::span<const std::string> joinedGuilds
  ) {
    const auto guildNumber = settingsId(guild);
    if (!folderId.empty() && settingsId(folderId, true) > std::numeric_limits<std::int64_t>::max())
      throw std::runtime_error("Invalid destination folder identifier");
    if (newFolderName) {
      if (!folderId.empty()
          || newFolderName->empty()
          || newFolderName->size() > maximumFolderNameBytes
          || utf8Length(*newFolderName) > maximumFolderNameCharacters
          || std::ranges::all_of(*newFolderName, [](char c) { return c == ' '; })
          || std::ranges::any_of(*newFolderName, [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::runtime_error("A new server folder needs a name of at most 100 characters");
      // nlohmann validates UTF-8 before any encoded request can be produced.
      (void)nlohmann::json(*newFolderName).dump();
    }
    const auto bytes = organizationSnapshot(snapshot, joinedGuilds);
    const auto version = organizationVersion(bytes);
    const auto group = nestedField(bytes, 14).value_or("");
    struct Folder {
      std::string bytes, id;
      std::vector<std::string> members;
    };
    struct Part {
      std::string raw;
      std::optional<Folder> folder;
    };
    std::vector<Part> parts;
    std::set<std::string> folderIds, membership;
    std::optional<std::size_t> source, target;
    std::size_t folderCount = 0;
    Wire input(group);
    while (!input.bytes.empty()) {
      const auto before = input.bytes;
      input.next();
      if (parts.size() >= 50000)
        throw std::runtime_error("Too many organization settings fields");
      Part part{.raw = std::string(before.substr(0, before.size() - input.bytes.size())), .folder = {}};
      if (input.field == 1) {
        if (input.type != 2 || ++folderCount > 1000)
          throw std::runtime_error("Invalid server folder list");
        Folder folder{.bytes = std::string(input.data), .id = {}, .members = {}};
        folder.members = organizationIds(folder.bytes, 1, 100);
        Wire folderFields(folder.bytes);
        while (folderFields.next())
          if (folderFields.field == 2 && folderFields.type != 2)
            throw std::runtime_error("Invalid existing server folder identifier");
        if (const auto wrappedId = nestedField(folder.bytes, 2)) {
          std::uint64_t id = 0;
          Wire wrapper(*wrappedId);
          while (wrapper.next())
            if (wrapper.field == 1) {
              if (wrapper.type != 0 || wrapper.number > std::numeric_limits<std::int64_t>::max())
                throw std::runtime_error("Invalid existing server folder identifier");
              id = wrapper.number;
            }
          folder.id = std::to_string(id);
          if (!folderIds.insert(folder.id).second)
            throw std::runtime_error("Duplicate server folder identifier");
        }
        if (!folderId.empty() && folder.id == folderId)
          target = parts.size();
        for (const auto& member : folder.members) {
          if (!membership.insert(member).second || membership.size() > 10000)
            throw std::runtime_error("A server occurs in more than one folder");
          if (member == guild)
            source = parts.size();
        }
        part.folder = std::move(folder);
      }
      parts.push_back(std::move(part));
    }
    const auto positions = organizationIds(group, 2);
    if (!source && std::ranges::find(positions, guild) == positions.end())
      throw std::runtime_error("The server is absent from the current account organization");
    if (!folderId.empty() && !target)
      throw std::runtime_error("The destination server folder no longer exists");
    if (source && !newFolderName && parts[*source].folder->id == folderId)
      return {{"settings", encode64(field(14, group))}, {"required_data_version", version}};
    if (target && parts[*target].folder->members.size() >= 100)
      throw std::runtime_error("The destination folder already holds 100 servers");

    std::string newFolder;
    if (!target) {
      newFolder = fixedId(1, guildNumber);
      if (newFolderName) {
        if (folderCount >= 1000)
          throw std::runtime_error("Too many server folders");
        std::uint64_t next = 1;
        while (folderIds.contains(std::to_string(next)))
          ++next;
        newFolder += field(2, numericField(1, next)) + field(3, field(1, *newFolderName));
      }
    }
    std::string updated;
    for (std::size_t index = 0; index < parts.size(); ++index) {
      auto& part = parts[index];
      if (source == index) {
        const auto remaining = removeOrganizationId(part.folder->bytes, 1, guildNumber);
        if (part.folder->members.size() > 1)
          updated += field(1, remaining);
        if (!target)
          updated += field(1, newFolder);
      } else if (target == index)
        updated += field(1, part.folder->bytes + fixedId(1, guildNumber));
      else
        updated += part.raw;
    }
    if (!source && !target)
      updated += field(1, newFolder);
    // guild_positions remains byte-identical: the folder sequence controls the
    // rail while positions retain the ordering of ungrouped/unloaded servers.
    return {{"settings", encode64(field(14, updated))}, {"required_data_version", version}};
  }

  bool validRailMove(const GuildRailMove& move) {
    try {
      const auto source = settingsId(move.source, move.sourceFolder);
      const auto target = settingsId(move.target, move.targetFolder);
      return (!move.sourceFolder || source <= std::numeric_limits<std::int64_t>::max())
          && (!move.targetFolder || target <= std::numeric_limits<std::int64_t>::max())
          && !(move.source == move.target && move.sourceFolder == move.targetFolder)
          && (move.placement == RailPlacement::Before
              || move.placement == RailPlacement::After
              || (move.placement == RailPlacement::Into && !move.sourceFolder));
    } catch (...) {
      return false;
    }
  }
  nlohmann::json
  guildRailPatch(const nlohmann::json& snapshot, const GuildRailMove& move, std::span<const std::string> joinedGuilds) {
    if (!validRailMove(move))
      throw std::runtime_error("Invalid server drag target");
    const auto bytes = organizationSnapshot(snapshot, joinedGuilds);
    const nlohmann::json current{{"settings", encode64(bytes)}};
    auto layout = parseGuildLayout(current);
    const auto find = [&](const std::string& id, bool folder) {
      return std::ranges::find_if(layout.folders, [&](const auto& entry) {
        return folder ? entry.id == id : std::ranges::find(entry.guildIds, id) != entry.guildIds.end();
      });
    };
    // Validate the entire raw snapshot through the existing bounded organization
    // reader before editing. This also rejects duplicate IDs and malformed fields.
    const auto source = find(move.source, move.sourceFolder);
    std::string validationGuild = move.source;
    if (move.sourceFolder) {
      if (source == layout.folders.end() || source->guildIds.empty())
        throw std::runtime_error("The dragged folder no longer exists");
      validationGuild = source->guildIds.front();
    }
    const auto validation = guildMovePatch(current, validationGuild, source == layout.folders.end() ? "" : source->id);
    (void)validation;
    const auto version = organizationVersion(bytes);
    const auto group = nestedField(bytes, 14).value_or("");
    struct Record {
      GuildFolder folder;
      std::string bytes;
    };
    struct Part {
      std::string raw;
      bool folder;
    };
    std::vector<Record> records;
    std::vector<Part> parts;
    Wire input(group);
    while (!input.bytes.empty()) {
      const auto before = input.bytes;
      input.next();
      parts.push_back({std::string(before.substr(0, before.size() - input.bytes.size())), input.field == 1});
      if (input.field == 1)
        records.push_back({layout.folders.at(records.size()), std::string(input.data)});
    }
    // Old snapshots may keep servers solely in guild_positions. Materialize
    // only an involved anchor; leave unrelated position-only records untouched.
    for (const auto& id : layout.positions)
      if (((!move.sourceFolder && id == move.source) || (!move.targetFolder && id == move.target))
          && std::ranges::none_of(records, [&](const auto& entry) {
               return std::ranges::find(entry.folder.guildIds, id) != entry.folder.guildIds.end();
             }))
        records.push_back({{.guildIds = {id}}, fixedId(1, settingsId(id))});
    const auto locate = [&](const std::string& id, bool folder) {
      return std::ranges::find_if(records, [&](const auto& entry) {
        return folder ? entry.folder.id == id
                      : std::ranges::find(entry.folder.guildIds, id) != entry.folder.guildIds.end();
      });
    };
    auto from = locate(move.source, move.sourceFolder), to = locate(move.target, move.targetFolder);
    if (from == records.end() || to == records.end())
      throw std::runtime_error("The dragged server or destination no longer exists");
    if (move.sourceFolder && from == to)
      throw std::runtime_error("A folder cannot be moved into itself");
    if (from == to && move.targetFolder && move.placement == RailPlacement::Into && from->folder.guildIds.size() == 1)
      return {{"settings", encode64(field(14, group))}, {"required_data_version", version}};
    const auto setMembers = [&](Record& entry, std::vector<std::string> members) {
      std::string retained;
      Wire fields(entry.bytes);
      while (!fields.bytes.empty()) {
        const auto before = fields.bytes;
        fields.next();
        if (fields.field != 1)
          retained += before.substr(0, before.size() - fields.bytes.size());
      }
      for (const auto& id : members)
        retained += fixedId(1, settingsId(id));
      entry.bytes = std::move(retained);
      entry.folder.guildIds = std::move(members);
    };
    Record moving;
    if (move.sourceFolder) {
      moving = std::move(*from);
      records.erase(from);
    } else {
      moving = {{.guildIds = {move.source}}, fixedId(1, settingsId(move.source))};
      if (from->folder.id.empty() && from->folder.guildIds.size() == 1) {
        moving = std::move(*from); // Retain opaque fields on a standalone tile.
        records.erase(from);
      } else {
        auto remaining = from->folder.guildIds;
        std::erase(remaining, move.source);
        setMembers(*from, std::move(remaining));
        if (from->folder.guildIds.empty())
          records.erase(from);
      }
    }
    to = locate(move.target, move.targetFolder);
    if (to == records.end())
      throw std::runtime_error("The drop target disappeared");
    if (move.placement == RailPlacement::Into && !move.targetFolder && to->folder.id.empty()) {
      // Dropping a server onto a standalone server creates a folder. Allocate a
      // fresh ID from the current snapshot, never from a stale rendered layout.
      constexpr std::size_t maximumFolders = 1000;
      if (records.size() >= maximumFolders)
        throw std::runtime_error("Too many server folders");
      std::uint64_t id = 1;
      while (std::ranges::find(layout.folders, std::to_string(id), &GuildFolder::id) != layout.folders.end())
        ++id;
      to->folder.id = std::to_string(id);
      to->bytes += field(2, numericField(1, id));
    }
    if (!move.sourceFolder
        && (move.placement == RailPlacement::Into || (!move.targetFolder && !to->folder.id.empty()))) {
      constexpr std::size_t maximumFolderServers = 100;
      if (to->folder.guildIds.size() >= maximumFolderServers)
        throw std::runtime_error("The destination folder already holds 100 servers");
      auto members = to->folder.guildIds;
      auto at = std::ranges::find(members, move.target);
      if (move.placement == RailPlacement::Into)
        at = members.end();
      else if (move.placement == RailPlacement::After)
        ++at;
      members.insert(at, move.source);
      setMembers(*to, std::move(members));
    } else if (!move.targetFolder && to->folder.id.empty() && to->folder.guildIds.size() > 1) {
      // Be tolerant of legacy anonymous groups containing several servers. Split
      // only the target group, preserving its metadata on the first remainder.
      const auto index = static_cast<std::size_t>(to - records.begin());
      auto members = to->folder.guildIds;
      auto at = std::ranges::find(members, move.target);
      const auto split = static_cast<std::size_t>(at - members.begin()) + (move.placement == RailPlacement::After);
      std::vector<Record> replacement;
      if (split) {
        auto first = *to;
        setMembers(first, {members.begin(), members.begin() + split});
        replacement.push_back(std::move(first));
      }
      replacement.push_back(std::move(moving));
      if (split < members.size()) {
        Record tail = split ? Record{} : *to;
        setMembers(tail, {members.begin() + split, members.end()});
        replacement.push_back(std::move(tail));
      }
      records.erase(records.begin() + index);
      records.insert(records.begin() + index, replacement.begin(), replacement.end());
    } else {
      if (move.placement == RailPlacement::After)
        ++to;
      records.insert(to, std::move(moving));
    }
    // Reuse existing folder slots while leaving all non-folder wire fields in
    // place. Extra records append; deprecated guild_positions stays byte-identical.
    std::string updated;
    std::size_t index = 0;
    for (const auto& part : parts) {
      if (!part.folder)
        updated += part.raw;
      else if (index < records.size())
        updated += field(1, records[index++].bytes);
    }
    while (index < records.size())
      updated += field(1, records[index++].bytes);
    return {{"settings", encode64(field(14, updated))}, {"required_data_version", version}};
  }

  bool validFolderChange(const GuildFolderChange& change) {
    try {
      if (settingsId(change.folder, true) > std::numeric_limits<std::int64_t>::max()
          || (!change.name && !change.color && !change.position)
          || (change.color && *change.color > 0xFFFFFF))
        return false;
      if (change.position
          && *change.position != FolderPosition::Top
          && *change.position != FolderPosition::Up
          && *change.position != FolderPosition::Down
          && *change.position != FolderPosition::Bottom)
        return false;
      if (change.name) {
        if (change.name->size() > maximumFolderNameBytes
            || utf8Length(*change.name) > maximumFolderNameCharacters
            || (!change.name->empty() && change.name->find_first_not_of(' ') == std::string::npos)
            || std::ranges::any_of(*change.name, [](unsigned char c) { return c < 32 || c == 127; }))
          return false;
        (void)nlohmann::json(*change.name).dump();
      }
      return true;
    } catch (...) {
      return false;
    }
  }
  nlohmann::json guildFolderPatch(const nlohmann::json& snapshot, const GuildFolderChange& change) {
    if (!validFolderChange(change))
      throw std::runtime_error("Invalid folder name, color or position");
    const auto layout = parseGuildLayout(snapshot);
    const auto target = std::ranges::find(layout.folders, change.folder, &GuildFolder::id);
    if (target == layout.folders.end() || target->guildIds.empty())
      throw std::runtime_error("The server folder no longer exists");
    // The shared organization writer validates the complete snapshot, including
    // duplicate membership, malformed IDs and the required data version. A move
    // to the existing folder leaves its opaque protobuf fields byte-identical.
    auto patch = guildMovePatch(snapshot, target->guildIds.front(), change.folder);
    const auto group = nestedField(organizationSnapshot(patch), 14).value_or("");
    struct Part {
      std::string raw;
      bool folder;
    };
    std::vector<Part> parts;
    std::vector<std::string> folders;
    Wire input(group);
    while (!input.bytes.empty()) {
      const auto before = input.bytes;
      input.next();
      parts.push_back({std::string(before.substr(0, before.size() - input.bytes.size())), input.field == 1});
      if (input.field == 1)
        folders.emplace_back(input.data);
    }
    const auto index = static_cast<std::size_t>(target - layout.folders.begin());
    std::map<unsigned, SettingsEdit> edits;
    if (change.name)
      edits[3].children[1].replacement = field(1, *change.name);
    if (change.color)
      edits[4].children[1].replacement = numericField(1, *change.color);
    if (!edits.empty())
      folders[index] = mergeSettings(folders[index], edits);
    if (change.position) {
      const auto destination = *change.position == FolderPosition::Top ? std::size_t{0}
          : *change.position == FolderPosition::Bottom                 ? folders.size() - 1
          : *change.position == FolderPosition::Up                     ? (index ? index - 1 : index)
                                                                       : std::min(index + 1, folders.size() - 1);
      auto folder = std::move(folders[index]);
      folders.erase(folders.begin() + index);
      folders.insert(folders.begin() + destination, std::move(folder));
    }
    std::string updated;
    std::size_t indexOut = 0;
    for (const auto& part : parts)
      updated += part.folder ? field(1, folders[indexOut++]) : part.raw;
    patch["settings"] = encode64(field(14, updated));
    return patch;
  }

  GuildLayout parseGuildLayout(const nlohmann::json& value) {
    GuildLayout result;
    if (value.contains("guild_folders") && value["guild_folders"].is_array()) {
      for (const auto& item : value["guild_folders"]) {
        GuildFolder folder;
        if (item.contains("id") && !item["id"].is_null())
          folder.id = item["id"].is_string() ? item["id"].get<std::string>() : item["id"].dump();
        if (item.contains("name") && item["name"].is_string())
          folder.name = item["name"];
        if (item.contains("color") && item["color"].is_number_unsigned())
          folder.color = item["color"];
        if (item.contains("guild_ids"))
          for (const auto& id : item["guild_ids"])
            if (id.is_string() && validSnowflake(id.get<std::string>()))
              folder.guildIds.push_back(id);
        result.folders.push_back(std::move(folder));
      }
      if (value.contains("guild_positions"))
        for (const auto& id : value["guild_positions"])
          if (id.is_string() && validSnowflake(id.get<std::string>()))
            result.positions.push_back(id);
      return result;
    }
    const auto encoded = value.value("settings", value.value("proto", std::string{}));
    const auto decoded = base64(encoded);
    Wire root(decoded);
    while (root.next()) {
      if (root.field != 14 || root.type != 2)
        continue;
      Wire folders(root.data);
      while (folders.next()) {
        if (folders.field == 2)
          ids(folders, result.positions);
        if (folders.field != 1 || folders.type != 2)
          continue;
        GuildFolder folder;
        Wire entry(folders.data);
        while (entry.next()) {
          if (entry.field == 1)
            ids(entry, folder.guildIds);
          if (entry.type == 2 && entry.field >= 2 && entry.field <= 4) {
            Wire wrapper(entry.data);
            // An empty wrapper still means folder ID 0 is present.
            if (entry.field == 2)
              folder.id = "0";
            while (wrapper.next())
              if (wrapper.field == 1) {
                if (entry.field == 2 && wrapper.type == 0)
                  folder.id = std::to_string(wrapper.number);
                if (entry.field == 3 && wrapper.type == 2)
                  folder.name = wrapper.data;
                if (entry.field == 4 && wrapper.type == 0)
                  folder.color = wrapper.number & 0xFFFFFF;
              }
          }
        }
        result.folders.push_back(std::move(folder));
      }
    }
    return result;
  }
} // namespace api
