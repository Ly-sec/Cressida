#include "api/emoji.h"
#include "api/gifs.h"
#include "api/models.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
  using Json = nlohmann::json;
  void require(bool value, const char* text) {
    if (!value)
      throw std::runtime_error(text);
  }
  template <class F> void rejects(F fn, const char* text) {
    bool rejected = false;
    try {
      fn();
    } catch (...) {
      rejected = true;
    }
    require(rejected, text);
  }
  std::string varint(std::uint64_t value) {
    std::string out;
    do {
      out += char((value & 127) | (value > 127 ? 128 : 0));
      value >>= 7;
    } while (value);
    return out;
  }
  std::string field(unsigned id, const std::string& value) { return varint(id * 8 + 2) + varint(value.size()) + value; }
  std::string scalar(unsigned id, std::uint64_t value) { return varint(id * 8) + varint(value); }
  std::string encode(const std::string& value) {
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    unsigned bits = 0, count = 0;
    for (unsigned char byte : value) {
      bits = (bits << 8) | byte;
      count += 8;
      while (count >= 6) {
        count -= 6;
        result += alphabet[(bits >> count) & 63];
      }
    }
    if (count)
      result += alphabet[(bits << (6 - count)) & 63];
    while (result.size() % 4)
      result += '=';
    return result;
  }
} // namespace
int main() {
  try {
    const Json response = Json::array(
        {{{"id", "clip"},
          {"title", "A GIF"},
          {"url", "https://klipy.com/gifs/clip"},
          {"src", "https://static.klipy.com/ii/clip.gif"},
          {"gif_src", "https://static.klipy.com/ii/clip.webp"},
          {"preview", "https://static.klipy.com/ii/clip.png"},
          {"width", 320},
          {"height", 180}}}
    );
    auto found = api::parseGifs(response);
    require(
        found.size() == 1 && found[0].src.ends_with(".gif") && found[0].width == 320,
        "Use the requested format rather than assuming gif_src is a GIF"
    );
    require(
        api::parseGifs({{"gifs", response}}).size() == 1 && api::parseGifs(Json::array()).empty(),
        "Read public search/trending envelopes"
    );
    auto invalid = response;
    invalid[0]["src"] = "https://static.klipy.com.attacker.test/clip.gif";
    invalid[0]["gif_src"] = "file:///tmp/private.gif";
    require(api::parseGifs(invalid).empty(), "Never fetch unapproved provider origins");
    for (auto url :
         {"https://static.klipy.com@127.0.0.1/clip.gif", "http://media.tenor.com/clip.gif",
          "https://media.giphy.com.attacker.test/a"})
      require(api::mediaUrl(url).empty(), "Reject confusing hosts and insecure schemes");
    require(api::mediaUrl(std::string("https://static.klipy.com/a\0.gif", 31)).empty(), "Reject embedded NULs in URLs");
    require(
        api::gifSearchPath("a &é").ends_with("&q=a%20%26%C3%A9")
            && api::gifSearchPath("").starts_with("/gifs/trending-gifs?"),
        "Encode entire search input and use trending for an empty query"
    );
    rejects([] { api::gifSearchPath(std::string(257, 'a')); }, "Bound search length");
    const auto gif = found[0];
    const auto version = field(1, scalar(3, 17));
    const auto unrelated = field(6, field(1, "unrelated emoji history"));
    const auto unknown = field(42, "future favorite-group field") + scalar(2, 1);
    const Json empty{{"settings", encode(version + field(2, unknown) + unrelated)}};
    const auto added = api::gifFavoritePatch(empty, gif, true);
    require(
        added.at("required_data_version") == 17 && added.at("settings") != empty.at("settings"),
        "Require the freshly fetched snapshot version for mutations"
    );
    auto favorites = api::parseGifFavorites(added);
    require(favorites.size() == 1 && favorites[0].url == gif.url && favorites[0].order == 1, "Favorite GIF round trip");
    const auto favoriteValue = scalar(1, 1) + field(2, gif.src) + scalar(3, 320) + scalar(4, 180) + scalar(5, 1);
    const auto expected = field(2, unknown + field(1, field(1, gif.url) + field(2, favoriteValue)));
    require(
        added.at("settings") == encode(expected),
        "Preserve every unknown favorite-group field and omit unrelated top-level settings"
    );
    const Json current{{"settings", encode(version + expected + unrelated)}};
    const auto removed = api::gifFavoritePatch(current, gif, false);
    require(
        removed.at("settings") == encode(field(2, unknown)) && api::parseGifFavorites(removed).empty(),
        "Remove the final GIF while retaining tooltip and unknown group settings"
    );
    auto other = gif;
    other.url += "-second";
    const auto withSecond = api::gifFavoritePatch(current, other, true);
    const auto both = api::parseGifFavorites(withSecond);
    require(
        both.size() == 2 && both.front().url == other.url && both.front().order == 2,
        "New favorites precede existing favorites without losing them"
    );
    require(
        api::gifFavoritePatch(current, gif, true).at("settings") == encode(expected),
        "Favoriting an existing GIF preserves its raw record and ordering"
    );
    rejects([&] { api::gifFavoritePatch({{"settings", ""}}, gif, true); }, "Refuse unversioned account writes");
    rejects([] { api::parseGifFavorites({{"settings", "not base64!"}}); }, "Reject malformed protobuf encodings");
    rejects([] { api::parseGifFavorites({{"settings", "EgU="}}); }, "Reject truncated protobuf fields");
    const auto unknownEmojiField = field(19, "future emoji option");
    const auto emojiGroup = field(1, "heart") + field(1, "123456789012345678") + unknownEmojiField;
    const auto emojiSnapshot = Json{{"settings", encode(version + field(5, emojiGroup) + expected + unrelated)}};
    auto emojiPreferences = api::parseEmojiPreferences(emojiSnapshot);
    require(
        emojiPreferences.favorites == std::vector<std::string>{"heart", "123456789012345678"},
        "Read Unicode aliases and custom emoji IDs from account favorites"
    );
    const auto emojiAdded = api::emojiFavoritePatch(emojiSnapshot, "fire", true);
    require(
        emojiAdded.at("required_data_version") == 17
            && emojiAdded.at("settings") == encode(field(5, emojiGroup + field(1, "fire"))),
        "Emoji stars preserve unknown group fields and omit GIF and frecency data"
    );
    const auto emojiRemoved = api::emojiFavoritePatch(emojiSnapshot, "heart", false);
    require(
        emojiRemoved.at("settings") == encode(field(5, field(1, "123456789012345678") + unknownEmojiField)),
        "Removing a favorite preserves all other emoji and unknown settings"
    );
    const auto usage = field(
        6, field(1, field(1, "heart") + field(2, scalar(1, 2))) + field(1, field(1, "fire") + field(2, scalar(1, 7)))
    );
    require(
        api::parseEmojiPreferences({{"settings", encode(version + usage)}}).frequent
            == std::vector<std::string>{"fire", "heart"},
        "Import frequent account emoji by total use count"
    );
    const auto scoredUsage = field(
        6,
        field(1, field(1, "heart") + field(2, scalar(1, 1000) + scalar(3, 2) + scalar(4, 1)))
            + field(1, field(1, "fire") + field(2, scalar(1, 2) + scalar(3, 1) + scalar(4, 20)))
    );
    require(
        api::parseEmojiPreferences({{"settings", encode(version + scoredUsage)}}).frequent
            == std::vector<std::string>{"fire", "heart"},
        "Discord's provided recency/frequency score takes precedence over lifetime usage"
    );
    const auto channelEntry = [&](std::uint64_t id, std::uint64_t score, std::uint64_t recent) {
      std::string key(1, char(9));
      for (unsigned byte = 0; byte < sizeof(id); ++byte)
        key += static_cast<char>((id >> (byte * 8)) & 255);
      return field(1, key + field(2, scalar(1, 100) + field(2, varint(recent)) + scalar(4, score)));
    };
    const auto channelUsage = field(
        12, channelEntry(100, 1, 30) + channelEntry(200, 10, 20) + channelEntry(300, 10, 40) + channelEntry(300, 10, 40)
    );
    const auto importedUsage = api::parseEmojiPreferences({{"settings", encode(version + scoredUsage + channelUsage)}});
    require(
        importedUsage.frequentChannels == std::vector<std::string>{"300", "200", "100"}
            && importedUsage.frequent == std::vector<std::string>{"fire", "heart"},
        "Account channel usage ranks by provided score and recent timestamps independently of emoji preferences"
    );
    require(
        api::parseEmojiPreferences(
            {{"settings", encode(field(12, field(1, scalar(1, 100) + field(2, scalar(4, 50)))))}}
        ).frequentChannels.empty(),
        "Channel usage requires validated fixed-width source identities"
    );
    const auto truncatedChannels = field(12, field(1, std::string(1, char(9))));
    rejects(
        [&] { api::parseEmojiPreferences({{"settings", encode(truncatedChannels)}}); },
        "Truncated channel identities fail without inventing a ranking"
    );
    std::string tooManyChannels;
    for (std::uint64_t id = 1; id <= 4097; ++id)
      tooManyChannels += channelEntry(id, 1, id);
    rejects(
        [&] { api::parseEmojiPreferences({{"settings", encode(field(12, tooManyChannels))}}); },
        "Account channel usage has a bounded result count"
    );
    const auto decayedUsage = field(
        6,
        field(1, field(1, "heart") + field(2, scalar(1, 1000) + scalar(3, 1)))
            + field(1, field(1, "fire") + field(2, scalar(1, 2) + scalar(3, 10)))
    );
    require(
        api::parseEmojiPreferences({{"settings", encode(version + decayedUsage)}}).frequent
            == std::vector<std::string>{"fire", "heart"},
        "Frecency works when no precomputed score is present"
    );
    rejects(
        [&] { api::emojiFavoritePatch({{"settings", ""}}, "heart", true); },
        "Do not mutate emoji favorites without the current data version"
    );
    auto video = gif;
    video.format = 2;
    video.src = "https://media.tenor.com/a/clip.mp4";
    require(api::gifFromJson(api::gifJson(video)).format == 2, "Keep video favorites in video format");
    api::Message message =
        api::parseMessages(
            Json::array(
                {{{"id", "1"},
                  {"channel_id", "2"},
                  {"embeds",
                   Json::array(
                       {{{"type", "gifv"}, {"video", {{"url", video.src}, {"width", 320}, {"height", 180}}}}}
                   )}}}
            )
        ).front();
    require(message.embeds.front().video.url == video.src, "Preserve GIF video media during message parsing");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
