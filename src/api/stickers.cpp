#include "api/stickers.h"

#include "api/models.h"

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>
#include <set>

namespace api {
  namespace {
    std::string text(const nlohmann::json& value, const char* key, std::size_t limit) {
      const auto it = value.find(key);
      return it != value.end() && it->is_string() && it->get_ref<const std::string&>().size() <= limit
          ? it->get<std::string>()
          : std::string{};
    }
  } // namespace
  std::vector<Sticker> parseStickers(const nlohmann::json& value, const std::string& guild) {
    std::vector<Sticker> result;
    if (!value.is_array() || (!guild.empty() && !validSnowflake(guild)))
      return result;
    std::set<std::string> seen;
    for (const auto& item : value) {
      if (result.size() >= 1000)
        break;
      if (!item.is_object())
        continue;
      Sticker sticker;
      sticker.id = text(item, "id", 20);
      sticker.name = text(item, "name", 128);
      if (!validSnowflake(sticker.id) || sticker.name.empty() || !seen.insert(sticker.id).second)
        continue;
      if (const auto it = item.find("format_type"); it != item.end() && it->is_number_integer() && *it >= 1 && *it <= 4)
        sticker.format = it->get<int>();
      sticker.guild = guild.empty() ? text(item, "guild_id", 20) : guild;
      if (!sticker.guild.empty() && !validSnowflake(sticker.guild))
        continue;
      sticker.pack = text(item, "pack_id", 20);
      if (!sticker.pack.empty() && !validSnowflake(sticker.pack))
        continue;
      sticker.description = text(item, "description", 400);
      sticker.tags = text(item, "tags", 800);
      if (const auto it = item.find("type"); it != item.end() && it->is_number_integer() && (*it == 1 || *it == 2))
        sticker.type = it->get<int>();
      if (const auto it = item.find("available"); it != item.end() && it->is_boolean())
        sticker.available = it->get<bool>();
      result.push_back(std::move(sticker));
    }
    return result;
  }
  std::vector<Sticker> parseStickerPacks(const nlohmann::json& value) {
    std::vector<Sticker> result;
    const auto packs = value.find("sticker_packs");
    if (packs == value.end() || !packs->is_array())
      return result;
    std::set<std::string> seen;
    std::size_t count = 0;
    for (const auto& pack : *packs) {
      if (++count > 256)
        break;
      if (!pack.is_object())
        continue;
      const auto id = text(pack, "id", 20), name = text(pack, "name", 128);
      if (!validSnowflake(id) || !pack.contains("stickers"))
        continue;
      for (auto& sticker : parseStickers(pack["stickers"])) {
        if (result.size() >= 10000)
          return result;
        if (!seen.insert(sticker.id).second)
          continue;
        sticker.type = 1;
        sticker.pack = id;
        sticker.packName = name;
        result.push_back(std::move(sticker));
      }
    }
    return result;
  }
  bool isWaveSticker(const Sticker& sticker) {
    return sticker.type == 1
        && sticker.available
        && (sticker.name == "Wave" || sticker.name == "wave")
        && !stickerAsset(sticker).empty();
  }
  std::string stickerAsset(const Sticker& sticker) {
    if (!validSnowflake(sticker.id))
      return {};
    switch (sticker.format) {
    case 1:
    case 2:
      return mediaUrl("https://cdn.discordapp.com/stickers/" + sticker.id + ".png");
    case 3:
      return mediaUrl("https://cdn.discordapp.com/stickers/" + sticker.id + ".json");
    case 4:
      return mediaUrl("https://media.discordapp.net/stickers/" + sticker.id + ".gif");
    default:
      return {};
    }
  }
  std::string stickerUnavailable(const Sticker& sticker, const std::string& guild, int premium, bool external) {
    if (!sticker.available)
      return "This sticker is unavailable because its server no longer has enough boosts.";
    if (stickerAsset(sticker).empty())
      return "This sticker format is unavailable.";
    if (sticker.type == 1)
      return {};
    if (sticker.guild.empty())
      return "This sticker's server is unavailable.";
    if (sticker.guild != guild) {
      if (!external)
        return "You do not have permission to use external stickers in this channel.";
      if (premium != 2)
        return "Discord Nitro is required to use stickers from another server.";
    }
    return {};
  }
  std::string stickerImageLink(const Sticker& sticker) {
    if (!validSnowflake(sticker.id) || stickerAsset(sticker).empty())
      return {};
    std::string name;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char ch : sticker.name) {
      if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')
        name += ch;
      else {
        name += '%';
        name += hex[ch >> 4];
        name += hex[ch & 15];
      }
    }
    return "https://media.discordapp.net/stickers/"
        + sticker.id
        + (sticker.format == 4 ? ".gif" : ".png")
        + "?size=160&name="
        + name
        + "&lossless=true";
  }
  std::optional<Sticker> stickerFromImageLink(std::string_view link) {
    for (const std::string_view origin :
         {"https://cdn.discordapp.com/stickers/", "https://media.discordapp.net/stickers/"}) {
      if (!link.starts_with(origin) || mediaUrl(link).empty())
        continue;
      const auto path = link.substr(origin.size());
      const auto dot = path.find('.'), query = path.find('?');
      if (dot == path.npos || !validSnowflake(std::string(path.substr(0, dot))))
        return {};
      const auto extension = path.substr(dot, query == path.npos ? path.size() - dot : query - dot);
      if (extension != ".png" && extension != ".gif" && extension != ".webp")
        return {};
      Sticker sticker;
      sticker.id = path.substr(0, dot);
      sticker.format = extension == ".gif" ? 4 : 1;
      sticker.name = "Sticker";
      if (query != path.npos) {
        auto parameters = path.substr(query + 1);
        while (!parameters.empty()) {
          const auto end = parameters.find('&');
          const auto item = parameters.substr(0, end);
          if (item.starts_with("name=")) {
            const auto encoded = item.substr(5);
            std::string decoded;
            for (std::size_t i = 0; i < encoded.size(); ++i) {
              unsigned ch = static_cast<unsigned char>(encoded[i]);
              if (ch == '%') {
                if (i + 2 >= encoded.size())
                  return {};
                const auto [ptr, ec] = std::from_chars(encoded.data() + i + 1, encoded.data() + i + 3, ch, 16);
                if (ec != std::errc{} || ptr != encoded.data() + i + 3)
                  return {};
                i += 2;
              }
              if (ch < 32 || ch == 127 || decoded.size() >= 128)
                return {};
              decoded += static_cast<char>(ch);
            }
            try {
              (void)nlohmann::json(decoded).dump();
            } catch (const nlohmann::json::type_error&) {
              return {};
            }
            if (!decoded.empty())
              sticker.name = std::move(decoded);
          }
          if (end == parameters.npos)
            break;
          parameters.remove_prefix(end + 1);
        }
      }
      return sticker;
    }
    return {};
  }
  bool validStickerIds(const std::vector<std::string>& ids) {
    return ids.size() <= 3
        && std::ranges::all_of(ids, validSnowflake)
        && std::set<std::string>(ids.begin(), ids.end()).size() == ids.size();
  }
} // namespace api
