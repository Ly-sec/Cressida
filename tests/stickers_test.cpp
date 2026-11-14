#include "api/actions.h"
#include "api/client.h"
#include "api/models.h"
#include "api/stickers.h"
#include "app/message_history.h"
#include "app/message_layout.h"
#include "app/notification_preview.h"

#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }
} // namespace

int main() {
  try {
    using Json = nlohmann::json;
    Json list = Json::array();
    for (int format = 1; format <= 4; ++format)
      list.push_back(
          {{"id", std::to_string(100 + format)},
           {"name", "Hello"},
           {"format_type", format},
           {"guild_id", "1"},
           {"tags", "wave"}}
      );
    list.push_back({{"id", "bad"}, {"name", "Bad"}, {"format_type", 1}});
    list.push_back(list.front());
    list.push_back({{"id", "105"}, {"name", nullptr}});
    const auto stickers = api::parseStickers(list, "1");
    auto wave = stickers.back();
    wave.type = 1;
    wave.name = "Wave";
    require(api::isWaveSticker(wave), "Standard wave stickers are greeting candidates");
    wave.type = 2;
    require(!api::isWaveSticker(wave), "Server stickers cannot enter random standard greetings");
    wave.type = 1;
    wave.available = false;
    require(!api::isWaveSticker(wave), "Unavailable wave stickers cannot be chosen");
    require(
        stickers.size() == 4 && stickers.front().guild == "1",
        "Sticker catalogs validate identities and deduplicate IDs"
    );
    require(api::parseStickers(Json::object(), "1").empty(), "Malformed catalogs are ignored");
    require(
        api::stickerAsset(stickers[0]) == "https://cdn.discordapp.com/stickers/101.png"
            && api::stickerAsset(stickers[1]) == "https://cdn.discordapp.com/stickers/102.png"
            && api::stickerAsset(stickers[2]) == "https://cdn.discordapp.com/stickers/103.json"
            && api::stickerAsset(stickers[3]) == "https://media.discordapp.net/stickers/104.gif",
        "PNG, APNG, Lottie and GIF use their actual supported CDN formats"
    );
    for (const auto& sticker : stickers)
      require(!api::mediaUrl(api::stickerAsset(sticker)).empty(), "Sticker media passes the shared CDN allowlist");
    auto unavailable = stickers.front();
    unavailable.available = false;
    require(
        !api::stickerUnavailable(unavailable, "1", 2, true).empty(),
        "Boost-disabled stickers remain unavailable natively"
    );
    require(
        api::stickerUnavailable(stickers.front(), "1", 0, false).empty(),
        "Members can send their own server stickers without Nitro"
    );
    require(
        !api::stickerUnavailable(stickers.front(), "2", 0, true).empty()
            && !api::stickerUnavailable(stickers.front(), "2", 3, true).empty()
            && api::stickerUnavailable(stickers.front(), "2", 2, true).empty()
            && !api::stickerUnavailable(stickers.front(), "2", 2, false).empty(),
        "External stickers respect full Nitro entitlement and external-sticker permissions"
    );
    const auto packs = api::parseStickerPacks(
        {{"sticker_packs", Json::array({{{"id", "9"}, {"name", "Friendly"}, {"stickers", list}}})}}
    );
    require(
        packs.size() == 4
            && packs.front().type == 1
            && packs.front().pack == "9"
            && packs.front().packName == "Friendly",
        "Official sticker packs retain their source metadata"
    );
    require(api::stickerUnavailable(packs.front(), "", 0, true).empty(), "Official sticker packs are free to send");
    auto named = stickers.front();
    named.name = "Hello [friend] & café";
    const auto link = api::stickerImageLink(named);
    const auto linked = api::stickerFromImageLink(link);
    require(
        linked && linked->name == named.name && linked->id == named.id && !link.contains(' '),
        "Image-link fallback safely roundtrips names and Unicode"
    );
    require(
        !api::stickerFromImageLink("https://media.discordapp.net.evil/stickers/101.png")
            && !api::stickerFromImageLink("https://cdn.discordapp.com/stickers/../101.png")
            && !api::stickerFromImageLink("https://cdn.discordapp.com/stickers/101.json")
            && !api::stickerFromImageLink("https://cdn.discordapp.com/stickers/101.png?name=%00bad"),
        "Sticker link interpretation rejects untrusted origins, paths and malformed metadata"
    );
    const auto body = api::messageBody("<@7> sticker", api::MessageReference{"20", "10"}, {"101", "102", "103"});
    require(
        body["sticker_ids"] == Json::array({"101", "102", "103"})
            && body["message_reference"]["message_id"] == "20"
            && body["content"] == "<@7> sticker"
            && body["allowed_mentions"]["parse"] == Json::array({"users", "roles", "everyone"})
            && body["allowed_mentions"]["replied_user"] == false,
        "Sticker captions allow explicit mentions independently of the reply-author ping"
    );
    require(
        !api::validStickerIds({"101", "101"})
            && !api::validStickerIds({"../bad"})
            && !api::validStickerIds({"101", "102", "103", "104"}),
        "Sending validates the three-sticker bound and unique IDs"
    );
    api::Client disconnected;
    require(
        !disconnected.send("10", "", {}, {}, {"101"})
            && !disconnected.stickerPacks()
            && !disconnected.guildStickers("1")
            && !disconnected.sticker("101"),
        "Offline tests do not make account requests"
    );
    auto message = api::parseMessages(
                       Json::array(
                           {{{"id", "20"},
                             {"channel_id", "10"},
                             {"content", ""},
                             {"sticker_items", Json::array({list[0], list[1], list[2]})}}}
                       )
    ).front();
    require(
        message.content.empty() && message.stickers.size() == 3,
        "Sticker-only messages never become system placeholders"
    );
    api::parseMessagePatch({{"content", "Caption"}}).apply(message);
    require(message.stickers.size() == 3, "Partial Gateway updates preserve absent sticker fields");
    api::parseMessagePatch({{"sticker_items", false}}).apply(message);
    require(message.stickers.size() == 3, "Malformed sticker patches cannot clear loaded media");
    api::parseMessagePatch({{"sticker_items", Json::array()}}).apply(message);
    require(message.stickers.empty(), "An explicit empty sticker array clears existing stickers");
    api::parseMessagePatch({{"stickers", Json::array({list[0]})}}).apply(message);
    require(message.stickers.size() == 1, "Legacy full sticker records remain readable");
    const app::MessageMeasure measure = [](std::string_view text, const app::MessageStyle& style) {
      return text.size() * style.size * .5F;
    };
    std::vector<api::Message> historyRows;
    app::MessageHistory history;
    const auto query = history.begin(api::HistoryDirection::Latest, historyRows);
    require(query.has_value(), "A sticker timeline can begin a normal history load");
    history.changed(message.id, api::parseMessagePatch({{"sticker_items", Json::array({list[3]})}}));
    history.changed(message.id, api::parseMessagePatch({{"content", "New caption"}}));
    require(
        history.apply(*query, historyRows, {message}, {})
            && historyRows.size() == 1
            && historyRows.front().stickers.front().format == 4
            && historyRows.front().content == "New caption",
        "Independent sticker and caption updates win over an older in-flight REST page"
    );
    auto multiple = message;
    multiple.stickers = {stickers[0], stickers[1], stickers[2]};
    multiple.content.clear();
    const auto row = app::layoutRichMessage(multiple, 600, measure);
    require(
        row.pictures.size() == 3 && row.pictures[0].y == row.pictures[2].y && row.pictures[1].x >= 168,
        "Multiple stickers share a row when the conversation is wide enough"
    );
    const auto wrapped = app::layoutRichMessage(multiple, 200, measure);
    require(
        wrapped.pictures[1].y >= wrapped.pictures[0].y + wrapped.pictures[0].height,
        "Multiple stickers wrap without overlap in narrow conversations"
    );
    message.content.clear();
    auto layout = app::layoutRichMessage(message, 600, measure);
    require(
        layout.pictures.size() == 1
            && layout.pictures.front().sticker
            && layout.pictures.front().width == 160
            && layout.pictures.front().height == 160
            && layout.boxes.empty()
            && layout.runs.empty(),
        "Native stickers render transparently at chat size without an attachment frame"
    );
    const auto narrow = app::layoutRichMessage(message, 90, measure);
    require(narrow.pictures.front().width == 90 && narrow.height == 90, "Sticker geometry fits narrow conversations");
    message.content = "Caption";
    layout = app::layoutRichMessage(message, 600, measure);
    require(
        layout.pictures.front().y >= layout.runs.front().y + layout.runs.front().height,
        "Sticker captions and artwork have measured non-overlapping geometry"
    );
    message.content = link;
    message.stickers.clear();
    api::Embed embed;
    embed.type = "image";
    embed.url = link;
    embed.image = {link, 320, 320};
    message.embeds = {embed};
    const auto transformed = app::layoutRichMessage(message, 600, measure, 14, {.emojiImageLinks = true});
    require(
        transformed.pictures.size() == 1
            && transformed.pictures.front().sticker
            && transformed.boxes.empty()
            && transformed.plainText.contains(link),
        "The shared image-link preference renders fallback links as stickers and preserves copy text"
    );
    message.content = "A caption\n" + link;
    const auto captioned = app::layoutRichMessage(message, 600, measure, 14, {.emojiImageLinks = true});
    std::string captionText;
    for (const auto& run : captioned.runs)
      captionText += run.text;
    require(
        captioned.pictures.size() == 1 && captioned.pictures.front().sticker && captionText == "Acaption",
        "Sticker image links render below captions without a duplicate URL or embed"
    );
    message.content = link;
    const auto ordinary = app::layoutRichMessage(message, 600, measure);
    require(
        ordinary.pictures.size() == 1 && !ordinary.pictures.front().sticker,
        "Disabling image links retains ordinary link/embed presentation"
    );
    message.content = '`' + link + '`';
    require(
        !app::layoutRichMessage(message, 600, measure, 14, {.emojiImageLinks = true}).pictures.front().sticker,
        "Code containing a sticker URL is not transformed"
    );
    message.content.clear();
    message.stickers = {stickers.front()};
    require(
        app::desktopMessagePreview(message, [](char, const auto&) { return std::string("User"); })
            == "Sent a sticker: Hello",
        "Sticker notifications have a readable summary"
    );
    api::Message forward;
    forward.forwarded.emplace();
    forward.forwarded->available = true;
    forward.forwarded->stickers = {stickers.back()};
    const auto forwarded = app::layoutRichMessage(forward, 500, measure);
    require(
        forwarded.pictures.size() == 1 && forwarded.pictures.front().sticker && forwarded.forwards.size() == 1,
        "Forwarded snapshots retain sticker artwork"
    );
    std::cout << "Sticker protocol and layout tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
