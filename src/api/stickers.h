#pragma once

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace api {
  struct Sticker {
    std::string id, name, description, tags, guild, pack, packName;
    int format = 0, type = 2;
    bool available = true;
    bool operator==(const Sticker&) const = default;
  };
  std::vector<Sticker> parseStickers(const nlohmann::json& value, const std::string& guild = {});
  std::vector<Sticker> parseStickerPacks(const nlohmann::json& value);
  bool isWaveSticker(const Sticker& sticker);
  std::string stickerAsset(const Sticker& sticker);
  std::string stickerImageLink(const Sticker& sticker);
  std::optional<Sticker> stickerFromImageLink(std::string_view link);
  std::string stickerUnavailable(const Sticker& sticker, const std::string& guild, int premium, bool external);
  bool validStickerIds(const std::vector<std::string>& ids);
} // namespace api
