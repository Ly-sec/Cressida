#pragma once
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace api {
  struct Emoji {
    std::string id, name, unicode, guild, category;
    std::vector<std::string> aliases, keywords;
    bool animated = false, available = true;
    std::string key() const { return id.empty() ? name : id; }
    std::string token() const {
      return id.empty() ? unicode : std::string(animated ? "<a:" : "<:") + name + ":" + id + ">";
    }
    std::string reaction() const { return id.empty() ? unicode : name + ":" + id; }
  };
  struct EmojiPreferences {
    std::vector<std::string> favorites, frequent;
    std::vector<std::string> frequentChannels;
  };
  bool validEmojiKey(const std::string& key);
  bool emojiNeedsImage(const Emoji& emoji, const std::string& guild, int premiumType);
  std::string emojiImageLink(const Emoji& emoji);
  std::optional<Emoji> emojiFromToken(std::string_view token);
  std::optional<Emoji> emojiFromImageLink(std::string_view url);
  std::vector<Emoji> parseGuildEmojis(const nlohmann::json& value, const std::string& guild);
  EmojiPreferences parseEmojiPreferences(const nlohmann::json& value);
  nlohmann::json emojiFavoritePatch(const nlohmann::json& snapshot, const std::string& key, bool add);
} // namespace api
