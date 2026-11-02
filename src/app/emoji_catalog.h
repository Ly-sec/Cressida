#pragma once
#include "api/emoji.h"

#include <filesystem>
#include <map>
#include <set>

namespace app {
  class EmojiCatalog {
  public:
    void load(const std::filesystem::path& catalog, const std::filesystem::path& aliases);
    void setGuild(const std::string& guild, std::vector<api::Emoji> emojis);
    void clearGuilds();
    const api::Emoji* find(const std::string& key) const;
    std::optional<api::Emoji> describe(std::string_view text) const;
    bool isUnicode(std::string_view text) const;
    std::vector<std::string>
    frequentKeys(const std::vector<std::string>& local, const std::vector<std::string>& remote) const;
    std::string
    messageContent(std::string_view content, const std::string& guild, int premiumType, bool fallback) const;
    const std::vector<std::string>& unicode() const { return m_unicode; }
    const std::vector<std::string>& guild(const std::string& id) const;
    const std::vector<std::string>& category(const std::string& id) const;

  private:
    std::map<std::string, api::Emoji> m_items;
    std::map<std::string, std::string> m_aliases;
    std::map<std::string, std::string> m_unicodeNames;
    std::map<std::string, std::vector<std::string>> m_guilds;
    std::map<std::string, std::vector<std::string>> m_categories;
    std::vector<std::string> m_unicode;
    std::set<std::string, std::less<>> m_unicodeSymbols;
  };
} // namespace app
