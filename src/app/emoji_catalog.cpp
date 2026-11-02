#include "app/emoji_catalog.h"

#include <algorithm>
#include <fstream>
#include <md4c.h>
#include <nlohmann/json.hpp>

namespace app {
  namespace {
    nlohmann::json read(const std::filesystem::path& path) {
      std::ifstream file(path, std::ios::binary);
      std::string bytes(512 * 1024 + 1, '\0');
      file.read(bytes.data(), bytes.size());
      bytes.resize(file.gcount());
      return bytes.size() <= 512 * 1024 ? nlohmann::json::parse(bytes, nullptr, false) : nlohmann::json{};
    }
    std::string normalized(std::string text) {
      for (auto at = text.find("\xEF\xB8\x8F"); at != std::string::npos; at = text.find("\xEF\xB8\x8F"))
        text.erase(at, 3);
      return text;
    }
  } // namespace
  void EmojiCatalog::load(const std::filesystem::path& catalogPath, const std::filesystem::path& aliasesPath) {
    if (!m_unicode.empty())
      return;
    const auto catalog = read(catalogPath), aliases = read(aliasesPath);
    std::map<std::string, std::vector<std::string>> names;
    if (aliases.is_object())
      for (const auto& [unicode, values] : aliases.items()) {
        if (!values.is_array())
          continue;
        m_unicodeSymbols.insert(normalized(unicode));
        for (const auto& alias : values)
          if (alias.is_string() && api::validEmojiKey(alias.get<std::string>()))
            names[normalized(unicode)].push_back(alias.get<std::string>());
      }
    for (const auto& [unicode, aliases] : names)
      if (!aliases.empty())
        m_unicodeNames.emplace(unicode, aliases.front());
    if (!catalog.is_array())
      return;
    for (const auto& item : catalog) {
      if (m_unicode.size() >= 2048)
        break;
      if (!item.is_object()
          || !item.contains("emoji")
          || !item["emoji"].is_string()
          || !item.contains("name")
          || !item["name"].is_string())
        continue;
      api::Emoji emoji;
      emoji.unicode = item["emoji"].get<std::string>();
      m_unicodeSymbols.insert(normalized(emoji.unicode));
      emoji.name = item["name"].get<std::string>();
      emoji.category = item.value("category", "symbols");
      if (emoji.unicode.empty() || emoji.unicode.size() > 160 || emoji.name.size() > 160)
        continue;
      emoji.aliases = names[normalized(emoji.unicode)];
      emoji.keywords.push_back(emoji.name);
      std::ranges::replace(emoji.name, ' ', '_');
      if (!emoji.aliases.empty())
        emoji.name = emoji.aliases.front();
      if (item.contains("keywords") && item["keywords"].is_array())
        for (const auto& word : item["keywords"])
          if (word.is_string() && word.get_ref<const std::string&>().size() < 160)
            emoji.keywords.push_back(word.get<std::string>());
      const auto key = emoji.key();
      m_aliases[emoji.unicode] = key;
      for (const auto& alias : emoji.aliases)
        m_aliases[alias] = key;
      if (!m_items.contains(key)) {
        m_unicode.push_back(key);
        m_categories[emoji.category].push_back(key);
        m_items[key] = std::move(emoji);
      }
    }
  }
  void EmojiCatalog::setGuild(const std::string& id, std::vector<api::Emoji> emojis) {
    for (const auto& key : m_guilds[id])
      m_items.erase(key);
    auto& keys = m_guilds[id];
    keys.clear();
    for (auto& emoji : emojis) {
      if (keys.size() >= 1000 || m_items.size() >= 32000)
        break;
      const auto key = emoji.key();
      if (key.empty() || m_items.contains(key))
        continue;
      keys.push_back(key);
      m_items[key] = std::move(emoji);
    }
  }
  void EmojiCatalog::clearGuilds() {
    for (const auto& [_, keys] : m_guilds)
      for (const auto& key : keys)
        m_items.erase(key);
    m_guilds.clear();
  }
  bool EmojiCatalog::isUnicode(std::string_view text) const {
    // Classify whole graphemes from the bundled Unicode data, including joined
    // sequences, flags and keycaps. Modifiers inherit the base emoji's sizing.
    if (text.find("\xEF\xB8\x8E") != std::string_view::npos)
      return false; // Explicit text presentation (VS15).
    auto key = normalized(std::string(text));
    for (std::size_t at = 0; at + 3 < key.size();) {
      if (key.compare(at, 3, "\xF0\x9F\x8F") == 0
          && static_cast<unsigned char>(key[at + 3]) >= 0xBB
          && static_cast<unsigned char>(key[at + 3]) <= 0xBF)
        key.erase(at, 4);
      else
        ++at;
    }
    return !key.empty() && m_unicodeSymbols.contains(key);
  }
  const api::Emoji* EmojiCatalog::find(const std::string& key) const {
    auto item = m_items.find(key);
    if (item == m_items.end()) {
      auto alias = m_aliases.find(key);
      if (alias != m_aliases.end())
        item = m_items.find(alias->second);
    }
    return item == m_items.end() ? nullptr : &item->second;
  }
  std::optional<api::Emoji> EmojiCatalog::describe(std::string_view text) const {
    auto custom = api::emojiFromToken(text);
    if (!custom)
      custom = api::emojiFromImageLink(text);
    if (custom) {
      if (const auto* known = find(custom->id))
        return *known;
      return custom;
    }
    if (const auto* known = find(std::string(text)); known && known->id.empty() && known->unicode == text)
      return *known;
    auto key = normalized(std::string(text));
    std::string modifiers;
    constexpr std::size_t modifierBytes = 4;
    constexpr unsigned char firstTone = 0xBB, lastTone = 0xBF;
    for (std::size_t at = 0; at + modifierBytes <= key.size();) {
      if (key.compare(at, modifierBytes - 1, "\xF0\x9F\x8F") == 0
          && static_cast<unsigned char>(key[at + modifierBytes - 1]) >= firstTone
          && static_cast<unsigned char>(key[at + modifierBytes - 1]) <= lastTone) {
        if (const auto tone = m_unicodeNames.find(key.substr(at, modifierBytes)); tone != m_unicodeNames.end())
          modifiers += "::" + tone->second;
        key.erase(at, modifierBytes);
      } else
        ++at;
    }
    if (const auto name = m_unicodeNames.find(key); name != m_unicodeNames.end() && isUnicode(text)) {
      api::Emoji emoji;
      emoji.name = name->second + modifiers;
      emoji.unicode = text;
      return emoji;
    }
    return {};
  }
  std::vector<std::string>
  EmojiCatalog::frequentKeys(const std::vector<std::string>& local, const std::vector<std::string>& remote) const {
    std::vector<std::string> result;
    std::set<std::string> seen;
    const auto append = [&](const auto& keys) {
      for (const auto& key : keys)
        if (const auto* emoji = find(key);
            emoji && emoji->available && result.size() < 64 && seen.insert(emoji->key()).second)
          result.push_back(emoji->key());
    };
    append(local);
    append(remote);
    return result;
  }
  std::string EmojiCatalog::messageContent(
      std::string_view content, const std::string& guild, int premiumType, bool fallback
  ) const {
    if (!fallback || content.empty() || content.size() > 16000)
      return std::string(content);
    // MD4C interprets static <:name:id> as an autolink. Shield complete tokens
    // with equal-length plain characters before classifying text.
    // Byte offsets stay identical, while code and actual links remain protected.
    std::string markdown(content);
    for (std::size_t i = 0; i < markdown.size(); ++i) {
      if (markdown[i] != '<')
        continue;
      const auto end = content.find('>', i);
      if (end != std::string_view::npos && api::emojiFromToken(content.substr(i, end - i + 1))) {
        std::fill(markdown.begin() + i, markdown.begin() + end + 1, 'X');
        i = end;
      }
    }
    struct Parse {
      std::string_view content;
      std::vector<bool> normal;
      unsigned links = 0;
    } parsed{markdown, std::vector<bool>(content.size())};
    MD_PARSER parser{};
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
    parser.enter_block = parser.leave_block = [](MD_BLOCKTYPE, void*, void*) { return 0; };
    parser.enter_span = [](MD_SPANTYPE type, void*, void* context) {
      if (type == MD_SPAN_A || type == MD_SPAN_IMG)
        ++static_cast<Parse*>(context)->links;
      return 0;
    };
    parser.leave_span = [](MD_SPANTYPE type, void*, void* context) {
      if (type == MD_SPAN_A || type == MD_SPAN_IMG)
        --static_cast<Parse*>(context)->links;
      return 0;
    };
    parser.text = [](MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* context) {
      auto& state = *static_cast<Parse*>(context);
      if (type == MD_TEXT_NORMAL && !state.links) {
        const auto offset = static_cast<std::size_t>(text - state.content.data());
        if (offset <= state.normal.size() && size <= state.normal.size() - offset)
          std::fill(state.normal.begin() + offset, state.normal.begin() + offset + size, true);
      }
      return 0;
    };
    if (md_parse(markdown.data(), markdown.size(), &parser, &parsed) != 0)
      return std::string(content);
    std::string result;
    for (std::size_t i = 0; i < content.size();) {
      if (content[i] == '<' && parsed.normal[i]) {
        const auto end = content.find('>', i);
        if (end != std::string_view::npos && end - i <= 100) {
          const auto tag = content.substr(i, end - i + 1);
          const auto token = api::emojiFromToken(tag);
          const auto* emoji = token ? find(token->id) : nullptr;
          std::size_t slashes = 0;
          for (auto before = i; before && content[before - 1] == '\\'; --before)
            ++slashes;
          if (token
              && slashes % 2 == 0
              && std::all_of(parsed.normal.begin() + i, parsed.normal.begin() + end + 1, [](bool v) { return v; })
              && (!emoji || api::emojiNeedsImage(*emoji, guild, premiumType))) {
            if (!result.empty() && !std::isspace(static_cast<unsigned char>(result.back())))
              result += ' ';
            result += api::emojiImageLink(*token);
            if (end + 1 < content.size() && !std::isspace(static_cast<unsigned char>(content[end + 1])))
              result += ' ';
            i = end + 1;
            continue;
          }
        }
      }
      result += content[i++];
    }
    return result;
  }
  const std::vector<std::string>& EmojiCatalog::guild(const std::string& id) const {
    static const std::vector<std::string> empty;
    const auto found = m_guilds.find(id);
    return found == m_guilds.end() ? empty : found->second;
  }
  const std::vector<std::string>& EmojiCatalog::category(const std::string& id) const {
    static const std::vector<std::string> empty;
    const auto found = m_categories.find(id);
    return found == m_categories.end() ? empty : found->second;
  }
} // namespace app
