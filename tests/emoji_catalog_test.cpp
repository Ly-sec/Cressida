#include "api/models.h"
#include "app/emoji_catalog.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

int main(int argc, char** argv) {
  auto require = [](bool value, const char* text) {
    if (!value)
      throw std::runtime_error(text);
  };
  try {
    require(argc == 2, "Emoji assets directory required");
    app::EmojiCatalog catalog;
    catalog.load(std::filesystem::path(argv[1]) / "emoji.json", std::filesystem::path(argv[1]) / "emoji-aliases.json");
    require(catalog.unicode().size() > 1800, "Load the complete bundled Unicode catalog");
    require(
        catalog.isUnicode("👍🏽")
            && catalog.isUnicode("👩🏽‍💻")
            && catalog.isUnicode("🇩🇪")
            && catalog.isUnicode("1️⃣")
            && !catalog.isUnicode("1")
            && !catalog.isUnicode("hello")
            && !catalog.isUnicode("❤︎"),
        "Recognize complete Unicode graphemes and preserve explicit text presentation"
    );
    require(
        catalog.find("heart") && catalog.find("❤️") && catalog.find("heart")->key() == catalog.find("❤️")->key(),
        "Resolve account short names and Unicode variants to the same emoji"
    );
    require(
        catalog.find("thumbsup") && catalog.find("thumbsup")->unicode == "👍",
        "Resolve common aliases for favorites and search"
    );
    const auto emojis = api::parseGuildEmojis(
        nlohmann::json::array(
            {{{"id", "123456789012345678"}, {"name", "umbriel"}, {"animated", true}},
             {{"id", "123456789012345679"}, {"name", "unavailable"}, {"available", false}},
             {{"id", "123"}, {"name", "bad:tag"}}}
        ),
        "456"
    );
    require(
        emojis.size() == 2 && emojis[0].animated && !emojis[1].available,
        "Parse custom emoji availability/animation and reject invalid tags"
    );
    catalog.setGuild("456", emojis);
    const auto known = catalog.describe(emojis.front().token());
    require(
        known && known->name == "umbriel" && known->guild == "456" && known->animated,
        "Chat emoji details retain a known source server and animation"
    );
    const auto unknown = catalog.describe("<:elsewhere:987654321012345678>");
    require(
        unknown && unknown->name == "elsewhere" && unknown->guild.empty(),
        "Unknown custom emoji retain their wire name without inventing a source server"
    );
    require(
        catalog.describe("👍🏽")
            && catalog.describe("👩🏽‍💻")
            && catalog.describe("🇩🇪")
            && catalog.describe("1️⃣")
            && catalog.describe("❤️")
            && !catalog.describe("hello")
            && !catalog.describe("❤︎")
            && !catalog.describe("<:bad:tag>"),
        "Name lookup preserves complete Unicode emoji and rejects ordinary text and malformed tokens"
    );
    auto duplicate = emojis.front();
    duplicate.id = "123456789012345681";
    duplicate.guild = "789";
    catalog.setGuild("789", {duplicate});
    require(
        catalog.frequentKeys(
            {"thumbsup", "+1", "❤️", "heart", duplicate.id},
            {"heart", emojis.front().id, emojis[1].id, duplicate.id, "unknown-emoji"}
        )
            == std::vector<
                std::
                    string>{catalog.find("thumbsup")->key(), catalog.find("heart")->key(), duplicate.id, emojis.front().id},
        "Usage ranking canonicalizes Unicode aliases, preserves distinct same-name server IDs and skips unavailable "
        "emoji"
    );
    require(
        catalog.frequentKeys(std::vector<std::string>(80, "heart"), {duplicate.id}).size() == 2,
        "Duplicate usage keys never exhaust the frequent-result bound before distinct emoji"
    );
    const auto tag = emojis[0].token(), image = api::emojiImageLink(emojis[0]);
    require(catalog.messageContent(tag, "789", 0, false) == tag, "Emoji image links are opt-in");
    require(
        catalog.messageContent(tag, "789", 0, true) == image,
        "Unsupported animated external emoji becomes a named CDN image link"
    );
    require(
        catalog.messageContent(tag, "456", 2, true) == tag, "A premium account retains supported native emoji tags"
    );
    auto still = emojis[0];
    still.animated = false;
    still.id = "123456789012345680";
    catalog.setGuild("456", {emojis[0], emojis[1], still});
    require(
        catalog.messageContent(still.token(), "789", 0, true) == api::emojiImageLink(still),
        "Static external emoji uses the same send-time fallback as animated emoji"
    );
    require(
        catalog.messageContent(still.token(), "", 0, true) == api::emojiImageLink(still),
        "Static server emoji falls back in direct messages"
    );
    require(
        catalog.messageContent(still.token(), "456", 0, true) == still.token(),
        "Supported static emoji from this server stays native"
    );
    require(
        catalog.messageContent(still.token() + tag, "789", 0, true) == api::emojiImageLink(still) + " " + image,
        "Mixed static and animated emoji both convert, including adjacent tags"
    );
    catalog.setGuild("456", emojis);
    require(
        !api::emojiNeedsImage(still, "456", 0) && api::emojiNeedsImage(still, "789", 0),
        "Non-premium accounts can use static emoji from the current server"
    );
    require(api::emojiNeedsImage(emojis[1], "456", 2), "Unavailable emoji need image fallback even with premium");
    require(
        catalog.messageContent("hello" + tag + "!", "789", 0, true) == "hello " + image + " !",
        "Fallback links have word boundaries without removing surrounding text"
    );
    for (const auto& literal :
         {"`" + tag + "`", "```cpp\n" + tag + "\n```", "~~~\n" + tag + "\n~~~", "    " + tag, "\\" + tag,
          "[" + tag + "](https://example.com)"})
      require(
          catalog.messageContent(literal, "789", 0, true) == literal,
          "Never rewrite code, escaped tags or existing Markdown links"
      );
    require(
        catalog.messageContent("❤️ <:unknown:999>", "789", 0, true)
            == "❤️ [unknown](https://cdn.discordapp.com/emojis/999.png?size=48&name=unknown&lossless=true)",
        "Copied valid custom tags fall back before their server catalog loads; Unicode stays intact"
    );
    require(
        catalog.messageContent("<:bad/name:999> <@123>", "789", 0, true) == "<:bad/name:999> <@123>",
        "Malformed custom tags and mentions are never treated as emoji"
    );
    for (const auto& literal :
         {"`" + still.token() + "`", "~~~\n" + still.token() + "\n~~~", "\\" + still.token(),
          "[" + still.token() + "](https://example.com)"})
      require(
          catalog.messageContent(literal, "789", 0, true) == literal,
          "Static-token shielding preserves code, escapes and link labels"
      );
    require(
        catalog.messageContent("<:__wave__:999>", "789", 0, true)
            == "[__wave__](https://cdn.discordapp.com/emojis/999.png?size=48&name=__wave__&lossless=true)",
        "Markdown-like underscores inside valid emoji names do not prevent fallback"
    );
    const auto decoded =
        api::emojiFromImageLink(image.substr(image.find("https://"), image.size() - image.find("https://") - 1));
    require(
        decoded && decoded->id == emojis[0].id && decoded->animated && decoded->name == emojis[0].name,
        "Read emoji image-link metadata without an account or embed response"
    );
    require(
        !api::emojiFromImageLink("https://cdn.discordapp.com.evil/emojis/123.gif")
            && !api::emojiFromImageLink("https://cdn.discordapp.com/emojis/123.gif/other"),
        "Only canonical emoji CDN paths are transformed"
    );
    require(
        catalog.guild("456").size() == 2
            && catalog.find("123456789012345678")->token() == "<a:umbriel:123456789012345678>"
            && catalog.find("123456789012345678")->reaction() == "umbriel:123456789012345678",
        "Compose custom tags and reaction keys, never bare user-visible IDs"
    );
    catalog.setGuild("456", {});
    require(
        !catalog.find("123456789012345678") && catalog.guild("456").empty(), "Live updates remove deleted guild emoji"
    );
    catalog.setGuild("456", emojis);
    catalog.clearGuilds();
    require(
        !catalog.find("123456789012345678") && catalog.find("heart"),
        "Account changes clear server data while retaining the Unicode catalog"
    );
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
