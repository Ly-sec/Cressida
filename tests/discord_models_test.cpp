#include "api/models.h"

#include <algorithm>
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
  using nlohmann::json;
  try {
    auto appMessage = api::parseMessages(
                          json::array(
                              {{{"id", "100"},
                                {"channel_id", "10"},
                                {"content", "A bot response"},
                                {"author", {{"id", "30"}, {"username", "Orbit"}, {"bot", true}}}}}
                          )
    ).front();
    require(appMessage.authorBot, "Bot authors retain their app identity in history and live messages");
    api::parseMessagePatch({{"content", "Updated response"}}).apply(appMessage);
    require(appMessage.authorBot, "Content-only updates preserve the app badge");
    for (const auto& author : {json{{"bot", nullptr}}, json{{"bot", "true"}}, json{{"username", "Orbit"}}})
      api::parseMessagePatch({{"author", author}}).apply(appMessage);
    require(appMessage.authorBot, "Missing and malformed bot flags cannot erase a known app identity");
    api::parseMessagePatch({{"author", {{"bot", false}}}}).apply(appMessage);
    require(!appMessage.authorBot, "An explicit false bot flag clears the app identity");
    const auto specialChannels = api::parseChannels(
        json::array(
            {{{"id", "901"}, {"type", 5}, {"name", "news"}},
             {{"id", "902"}, {"type", 13}, {"name", "stage"}},
             {{"id", "903"}, {"type", 14}, {"name", "directory"}},
             {{"id", "904"}, {"type", 16}, {"name", "media"}},
             {{"id", "905"}, {"type", 11}, {"name", "thread"}}}
        )
    );
    require(
        specialChannels.size() == 4
            && specialChannels[1].kind() == api::ChannelType::GuildDirectory
            && specialChannels[3].kind() == api::ChannelType::GuildStageVoice,
        "Guild snapshots retain announcements, stages, directories and media while threads stay separate"
    );
    api::Message replyMessage;
    const json reference = {{"message_id", "123"}, {"channel_id", "456"}};
    api::parseMessagePatch(
        {{"message_reference", reference},
         {"referenced_message",
          {{"content", "Original text"},
           {"author", {{"id", "789"}, {"username", "Maya"}, {"avatar", "0123456789abcdef0123456789abcdef"}}}}}}
    ).apply(replyMessage);
    require(
        replyMessage.reply && replyMessage.reply->available && replyMessage.reply->content == "Original text",
        "Received reply snapshots retain original text and availability"
    );
    api::parseMessagePatch({{"message_reference", reference}}).apply(replyMessage);
    require(
        replyMessage.reply->content == "Original text"
            && replyMessage.reply->author == "Maya"
            && replyMessage.reply->avatar.find("0123456789abcdef0123456789abcdef") != std::string::npos,
        "Reference-only updates do not erase a received preview"
    );
    api::parseMessagePatch({{"message_reference", reference}, {"referenced_message", nullptr}}).apply(replyMessage);
    require(
        replyMessage.reply->deleted && replyMessage.reply->content.empty() && replyMessage.reply->avatar.empty(),
        "Explicit deleted originals replace the previous reply preview"
    );
    api::parseMessagePatch({{"message_reference", nullptr}}).apply(replyMessage);
    require(!replyMessage.reply, "Explicit null references still clear the reply");
    auto guild = api::parseGuilds(
                     json::array(
                         {{{"id", "12"},
                           {"name", "A community"},
                           {"features", {"COMMUNITY"}},
                           {"approximate_member_count", 6870},
                           {"approximate_presence_count", 1191}}}
                     )
    ).front();
    require(
        api::guildBadge(guild) == api::GuildBadge::Community && guild.members == 6870 && guild.online == 1191,
        "Server lists retain feature flags and approximate counts"
    );
    auto ready = api::parseGuildUpdates(
        "READY",
        {{"guilds",
          json::array(
              {{{"id", "12"},
                {"properties", {{"name", "Official"}, {"features", {"COMMUNITY", "PARTNERED", "VERIFIED"}}}},
                {"member_count", 7000}}}
          )}}
    );
    require(ready.size() == 1, "Personal-account READY projects nested server properties");
    api::GuildUpdate journal{.id = "12"};
    journal.merge(ready.front());
    for (const auto& patch :
         api::parseGuildUpdates("GUILD_MEMBER_LIST_UPDATE", {{"guild_id", "12"}, {"online_count", 0}}))
      journal.merge(patch);
    journal.apply(guild);
    require(
        guild.name == "Official"
            && api::guildBadge(guild) == api::GuildBadge::Verified
            && guild.members == 7000
            && guild.online == 0,
        "Live metadata wins over an in-flight startup list; verified badges take precedence and zero is known"
    );
    for (const auto& patch :
         api::parseGuildUpdates("GUILD_UPDATE", {{"id", "12"}, {"features", {"PARTNERED", "COMMUNITY"}}}))
      patch.apply(guild);
    require(
        api::guildBadge(guild) == api::GuildBadge::Partnered && guild.members == 7000 && guild.online == 0,
        "Partial feature edits retain counts and fall back to the partnered badge"
    );
    for (const auto& patch :
         api::parseGuildUpdates("GUILD_UPDATE", {{"id", "12"}, {"features", json::array()}, {"online_count", nullptr}}))
      patch.apply(guild);
    require(
        api::guildBadge(guild) == api::GuildBadge::None && !guild.online && guild.members == 7000,
        "Explicit empty features and null counts clear their previous values"
    );
    for (const auto& patch : api::parseGuildUpdates(
             "GUILD_UPDATE", {{"id", "12"}, {"features", false}, {"member_count", -1}, {"online_count", "bad"}}
         ))
      patch.apply(guild);
    require(
        guild.features.empty() && guild.members == 7000 && !guild.online,
        "Malformed metadata cannot replace known counts or create a zero placeholder"
    );
    require(api::parseGuildUpdates("GUILD_UPDATE", {{"id", "bad"}}).empty(), "Invalid server identities are ignored");
    auto unknownGuild = api::parseGuilds(json::array({{{"id", "13"}, {"name", "Unknown counts"}}})).front();
    require(!unknownGuild.members && !unknownGuild.online, "Missing counts remain unknown");
    journal.apply(unknownGuild);
    require(unknownGuild.name == "Unknown counts", "A metadata patch cannot affect a different server");
    auto rulesGuild =
        api::parseGuilds(json::array({{{"id", "14"}, {"name", "Rules fixture"}, {"rules_channel_id", "501"}}})).front();
    require(rulesGuild.rulesChannelId == "501", "REST server snapshots retain the configured rules channel");
    api::GuildUpdate rulesJournal{.id = "14"};
    for (const auto& patch : api::parseGuildUpdates(
             "READY", {{"guilds", json::array({{{"id", "14"}, {"properties", {{"rules_channel_id", "502"}}}}})}}
         ))
      rulesJournal.merge(patch);
    for (const auto& patch : api::parseGuildUpdates("GUILD_UPDATE", {{"id", "14"}, {"name", "Renamed"}}))
      rulesJournal.merge(patch);
    rulesJournal.apply(rulesGuild);
    require(
        rulesGuild.rulesChannelId == "502" && rulesGuild.name == "Renamed",
        "READY properties and later partial updates preserve the rules-channel setting"
    );
    for (const auto& patch : api::parseGuildUpdates("GUILD_UPDATE", {{"id", "14"}, {"rules_channel_id", false}}))
      patch.apply(rulesGuild);
    require(rulesGuild.rulesChannelId == "502", "Malformed rules settings do not replace known values");
    for (const auto& patch : api::parseGuildUpdates("GUILD_UPDATE", {{"id", "14"}, {"rules_channel_id", nullptr}}))
      rulesJournal.merge(patch);
    rulesJournal.apply(rulesGuild);
    require(
        rulesGuild.rulesChannelId && rulesGuild.rulesChannelId->empty(),
        "An explicit null clears the setting and remains authoritative over older snapshots"
    );
    for (const auto& patch : api::parseGuildUpdates(
             "GUILD_CREATE",
             {{"id", "14"}, {"properties", {{"name", "Partial properties"}}}, {"rules_channel_id", "503"}}
         ))
      patch.apply(rulesGuild);
    require(rulesGuild.rulesChannelId == "503", "Top-level rules settings survive a partial properties wrapper");
    auto account = api::parseAccount(json{{"id", "123"}, {"username", "alex"}, {"global_name", nullptr}});
    require(account.name == "alex", "A null global name must fall back to the username");
    require(
        !account.premiumType
            && api::parseAccount(json{{"id", "123"}, {"premium_type", 2}}).premiumType == 2
            && api::parseAccount(json{{"id", "123"}, {"premium_type", 0}}).premiumType == 0,
        "Distinguish omitted premium state in partial account updates from explicit non-premium"
    );
    auto channels = api::parseChannels(
        json::array({
            {{"id", "4"}, {"name", "voice"}, {"type", 2}},
            {{"id", "99"}, {"name", "unsupported"}, {"type", 99}},
            {{"id", "3"}, {"name", "news"}, {"type", 5}, {"position", 2}},
            {{"id", "2"}, {"name", "general"}, {"type", 0}, {"position", 1}},
            {{"id", "1"}, {"name", "category"}, {"type", 4}},
        })
    );
    require(
        channels.size() == 4 && channels.front().id == "2" && channels[2].id == "4" && channels.back().type == 4,
        "Retain voice channel text chats after text channels and filter unsupported channel types"
    );
    auto messages = api::parseMessages(
        json::array({
            {{"id", "100"},
             {"channel_id", "1"},
             {"author", {{"username", "maya"}, {"global_name", nullptr}}},
             {"content", "hello"},
             {"application_id", nullptr}},
            {{"id", "99"}, {"channel_id", "1"}, {"content", ""}, {"attachments", json::array({{{"id", "1"}}})}},
        })
    );
    require(messages.front().id == "99", "Sort snowflakes numerically across digit boundaries");
    require(
        messages.front().content.empty() && messages.front().attachments.size() == 1,
        "Attachment-only messages retain typed attachments instead of placeholder text"
    );
    require(messages.back().author == "maya", "Optional client fields must not prevent message decoding");
    auto edited = messages.back();
    api::parseMessagePatch({{"edited_timestamp", "2026-10-17T22:00:00Z"}}).apply(edited);
    api::parseMessagePatch({{"content", "Edited text"}}).apply(edited);
    require(edited.editedTimestamp == "2026-10-17T22:00:00Z", "Partial edits retain the server's edited timestamp");
    api::parseMessagePatch({{"edited_timestamp", nullptr}}).apply(edited);
    require(edited.editedTimestamp.empty(), "Explicit null clears an edited timestamp");
    const auto guildMessages = api::parseMessages(
        json::array(
            {{{"id", "101"}, {"author", {{"id", "7"}, {"global_name", "Maya"}}}, {"member", {{"nick", "Server Maya"}}}},
             {{"id", "102"}, {"author", {{"id", "7"}, {"global_name", "Maya"}}}, {"member", {{"nick", nullptr}}}}}
        )
    );
    require(
        guildMessages[0].author == "Server Maya" && guildMessages[1].author == "Maya",
        "Message names honor guild nicknames and null fallbacks"
    );
    const auto categorized = api::parseChannels(
        json::array(
            {{{"id", "10"}, {"type", 4}, {"name", "Chat"}, {"position", 2}},
             {{"id", "11"}, {"type", 0}, {"parent_id", "10"}, {"position", 0}},
             {{"id", "20"}, {"type", 4}, {"name", "Projects"}, {"position", 1}},
             {{"id", "21"}, {"type", 0}, {"parent_id", "20"}, {"position", 9}}}
        )
    );
    require(
        categorized.size() == 4
            && categorized[0].id == "20"
            && categorized[1].id == "21"
            && categorized[2].id == "10"
            && categorized[3].id == "11",
        "Keep channels under their ordered parent categories"
    );
    auto mixedChannelPayload = json::array({
        {{"id", "93"}, {"type", 2}, {"position", 0}, {"parent_id", "90"}},
        {{"id", "100"}, {"type", 4}, {"position", 1}},
        {{"id", "10"}, {"type", 13}, {"position", 0}},
        {{"id", "30"}, {"type", 0}, {"position", 5}},
        {{"id", "103"}, {"type", 2}, {"position", 0}, {"parent_id", "100"}},
        {{"id", "92"}, {"type", 16}, {"position", 0}, {"parent_id", "90"}},
        {{"id", "31"}, {"type", 15}, {"position", 0}},
        {{"id", "11"}, {"type", 2}, {"position", 2}},
        {{"id", "91"}, {"type", 0}, {"position", 3}, {"parent_id", "90"}},
        {{"id", "90"}, {"type", 4}, {"position", 1}},
        {{"id", "102"}, {"type", 2}, {"position", 1}, {"parent_id", "100"}},
        {{"id", "32"}, {"type", 5}, {"position", 5}},
        {{"id", "101"}, {"type", 0}, {"position", 8}, {"parent_id", "100"}},
        {{"id", "9"}, {"type", 2}, {"position", 0}},
        {{"id", "33"}, {"type", 14}, {"position", 0}},
    });
    const auto channelIds = [](const std::vector<api::Channel>& items) {
      std::vector<std::string> ids;
      for (const auto& item : items)
        ids.push_back(item.id);
      return ids;
    };
    const std::vector<std::string> expectedChannelOrder{"31", "33", "30", "32",  "9",   "10",  "11", "90",
                                                        "92", "91", "93", "100", "101", "103", "102"};
    auto mixedChannels = api::parseChannels(mixedChannelPayload);
    require(
        channelIds(mixedChannels) == expectedChannelOrder,
        "Order text-like and voice/stage groups by position and numeric ID within their own category"
    );
    std::reverse(mixedChannelPayload.begin(), mixedChannelPayload.end());
    require(
        channelIds(api::parseChannels(mixedChannelPayload)) == expectedChannelOrder,
        "Channel ordering is independent of the server response array order"
    );
    for (auto& channel : mixedChannels) {
      if (channel.id == "103")
        channel.position = 2;
      if (channel.id == "11")
        channel.parentId = "90";
    }
    const std::vector<std::string> movedChannelOrder{"31", "33", "30", "32",  "9",   "10",  "90", "92",
                                                     "91", "93", "11", "100", "101", "102", "103"};
    require(
        channelIds(api::orderChannels(std::move(mixedChannels))) == movedChannelOrder,
        "Reordering positions and moving a voice channel into a category preserve grouped server order"
    );
    const auto conversations = api::parsePrivateChannels(
        json::array(
            {{{"id", "30"},
              {"type", 1},
              {"last_message_id", "99"},
              {"recipients", json::array({{{"id", "7"}, {"username", "maya"}, {"global_name", nullptr}}})}},
             {{"id", "31"},
              {"type", 3},
              {"name", "Studio"},
              {"last_message_id", "100"},
              {"recipients", json::array({{{"id", "8"}, {"username", "alex"}}})}}}
        )
    );
    require(
        conversations.size() == 2
            && conversations[0].name == "Studio"
            && conversations[1].name == "maya"
            && !conversations[1].icon.empty(),
        "Name/avatar DMs and sort by the latest message snowflake"
    );
    const auto deduped = api::parsePrivateChannels(
        json::array({{{"id", "32"}, {"type", 1}, {"recipient_ids", json::array({"7"})}}}),
        {{"7", {"7", "Maya", "/embed/avatars/0.png"}}}
    );
    require(deduped.front().name == "Maya", "Resolve normalized Gateway DM recipient IDs");
    require(
        conversations[0].named && !conversations[1].named, "Distinguish named groups from recipient-generated titles"
    );
    require(
        api::validSnowflake("18446744073709551615") && !api::validSnowflake("1/../../users/@me"),
        "Resource IDs must not become arbitrary REST paths"
    );
    require(!api::validSnowflake("") && !api::validSnowflake("123x"), "Reject malformed IDs");
    require(api::utf8Length("hello 🌿") == 7, "Message lengths must count Unicode characters rather than bytes");
    const auto rich = api::parseMessages(
        json::array(
            {{{"id", "101"},
              {"content", ""},
              {"attachments",
               json::array(
                   {{{"id", "1"},
                     {"filename", "photo.png"},
                     {"url", "https://cdn.discordapp.com/attachments/1/2/photo.png?ex=a&hm=b"},
                     {"content_type", "image/png"},
                     {"width", 640},
                     {"height", 480}}}
               )},
              {"embeds",
               json::array(
                   {{{"title", "Preview"},
                     {"description", "**Hello**"},
                     {"color", 123},
                     {"fields", json::array({{{"name", "field"}, {"value", "value"}}})},
                     {"thumbnail",
                      {{"proxy_url", "https://media.discordapp.net/external/hash/https/example.com/image.png"}}}}}
               )}}}
        )
    );
    require(
        rich.front().attachments.front().image.width == 640
            && rich.front().embeds.front().fields.front().name == "field",
        "Decode attachments, signed CDN URLs, and rich embed fields"
    );
    require(
        !api::mediaUrl(
             "https://images-ext-1.discordapp.net/external/hash/https/i.ytimg.com/vi/test/hqdefault.jpg?width=640"
        )
             .empty(),
        "Accept Discord's external image proxy for YouTube thumbnails"
    );
    require(
        api::mediaUrl("https://images-ext-1.discordapp.net.attacker.test/external/hash").empty(),
        "External proxy origins must match exactly"
    );
    const auto profile = api::parseAccount(
        json{{"id", "123"}, {"username", "alex"}, {"global_name", "Alex"}, {"bio", "hello"}, {"pronouns", "they/them"}}
    );
    require(
        profile.username == "alex"
            && profile.displayName == "Alex"
            && profile.bio == "hello"
            && profile.pronouns == "they/them",
        "Load editable account/profile fields separately from display labels"
    );
    auto updated = rich.front();
    api::parseMessagePatch(json{{"embeds", json::array()}}).apply(updated);
    require(
        updated.embeds.empty() && updated.attachments.size() == 1 && updated.content.empty(),
        "Embed-only Gateway updates preserve unrelated fields and allow clearing previews"
    );
    api::parseMessagePatch(json{{"content", "edited"}}).apply(updated);
    require(
        updated.content == "edited" && updated.attachments.size() == 1, "Content-only updates preserve attachments"
    );
    require(
        api::mediaUrl("https://cdn.discordapp.com.attacker.test/attachments/a").empty()
            && api::mediaUrl("https://cdn.discordapp.com@attacker.test/attachments/a").empty()
            && api::mediaUrl("file:///tmp/image.png").empty(),
        "Automatic media requests stay on fixed credential-free Discord origins"
    );
    require(
        api::mediaUrl("https://cdn.discordapp.com/stickers/123.png") == "https://cdn.discordapp.com/stickers/123.png"
            && api::mediaUrl("https://media.discordapp.net/stickers/123.gif")
                == "https://media.discordapp.net/stickers/123.gif"
            && api::mediaUrl("https://cdn.discordapp.com/stickers/123.json")
                == "https://cdn.discordapp.com/stickers/123.json"
            && api::mediaUrl("https://cdn.discordapp.com/soundboard-sounds/123")
                == "https://cdn.discordapp.com/soundboard-sounds/123"
            && api::mediaUrl("https://cdn.discordapp.com/new-member-actions/123/abc.png")
                == "https://cdn.discordapp.com/new-member-actions/123/abc.png"
            && api::mediaUrl("https://cdn.discordapp.com/resource-channels/123/a_abc.gif")
                == "https://cdn.discordapp.com/resource-channels/123/a_abc.gif"
            && api::mediaUrl("https://cdn.discordapp.com.attacker.test/stickers/123.png").empty()
            && api::mediaUrl("https://media.discordapp.net@attacker.test/soundboard-sounds/123").empty(),
        "Expression previews use documented public sticker and sound routes without widening origin checks"
    );
    api::Upload image{"paste.png", "image/png", {1, 2, 3}};
    require(api::validUploads({image}), "Accept bounded pasted images");
    image.filename = "../secret";
    require(!api::validUploads({image}), "Reject path-like upload names");
    auto forwardedJson =
        json::parse(R"({"id":"900","channel_id":"10","type":0,"content":"","author":{"id":"42","username":"sender"},
      "message_reference":{"type":1,"guild_id":"1","channel_id":"20","message_id":"800"},
      "message_snapshots":[{"message":{"content":"Saved **text** <@43>","timestamp":"2026-10-18T18:50:00Z",
        "author":{"id":"999","username":"must-not-be-attributed"},
        "mentions":[{"id":"43","username":"friend"}],
        "attachments":[{"id":"700","filename":"image.png","url":"https://cdn.discordapp.com/attachments/20/700/image.png","content_type":"image/png","width":800,"height":600}],
        "message_snapshots":[{"message":{"content":"nested content must not recurse"}}]}}]})");
    auto forwarded = api::parseMessages(json::array({forwardedJson})).front();
    require(
        forwarded.forwarded
            && forwarded.forwarded->available
            && !forwarded.reply
            && forwarded.content.empty()
            && forwarded.authorId == "42"
            && forwarded.forwarded->content == "Saved **text** <@43>"
            && forwarded.forwarded->attachments.size() == 1
            && forwarded.forwarded->mentions.at("43") == "friend",
        "Forward snapshots retain independent content/media and the forwarding sender without a fake reply"
    );
    api::parseMessagePatch({{"reactions", json::array()}, {"content", "A note"}}).apply(forwarded);
    require(
        forwarded.forwarded && forwarded.forwarded->attachments.size() == 1 && forwarded.content == "A note",
        "Partial updates preserve immutable forwarded content separately from outer text"
    );
    auto missing = forwardedJson;
    missing.erase("message_snapshots");
    auto unavailable = api::parseMessages(json::array({missing})).front();
    require(
        unavailable.forwarded && !unavailable.forwarded->available && unavailable.content.empty(),
        "Missing snapshots retain explicit forward identity instead of a system-message placeholder"
    );
    api::parseMessagePatch({{"message_reference", nullptr}}).apply(forwarded);
    require(!forwarded.forwarded && !forwarded.reply, "Explicit null references clear old forward/reply metadata");
    auto ordinary = forwardedJson;
    ordinary["message_reference"]["type"] = 0;
    require(
        !api::parseMessages(json::array({ordinary})).front().forwarded,
        "Snapshot payloads without a FORWARD reference cannot spoof a forwarded card"
    );
    bool rejected = false;
    try {
      (void)api::parseAccount(json{{"id", "bad"}});
    } catch (const std::exception&) {
      rejected = true;
    }
    require(rejected, "Reject malformed account identifiers");
    rejected = false;
    try {
      (void)api::parseMessages(json::object());
    } catch (const std::exception&) {
      rejected = true;
    }
    require(rejected, "Reject malformed message responses");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
