#include "api/profile.h"
#include "api/role_styles.h"
#include "api/user_directory.h"
#include "app/discord_timestamp.h"

#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

int main() {
  using nlohmann::json;
  const auto require = [](bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  try {
    const std::string hash = "1234567890abcdef1234567890abcdef";
    auto userTag = json{
        {"primary_guild", {{"identity_enabled", true}, {"identity_guild_id", "99"}, {"tag", "MOON"}, {"badge", hash}}}
    };
    const auto tag = api::parseGuildTag(userTag);
    require(
        tag && tag->guild == "99" && tag->tag == "MOON" && !tag->badge.empty(),
        "Guild tags retain the server ID, text and CDN badge"
    );
    for (const auto& invalid :
         {json(nullptr), json{{"identity_enabled", false}, {"identity_guild_id", "99"}, {"tag", "MOON"}},
          json{{"identity_enabled", true}, {"identity_guild_id", "99/../1"}, {"tag", "MOON"}}})
      require(!api::parseGuildTag(json{{"primary_guild", invalid}}), "Hidden/null and malformed tags remain hidden");
    require(
        !api::parseGuildTag(json{{"primary_guild", nullptr}, {"clan", userTag["primary_guild"]}}),
        "An explicit modern null does not resurrect a legacy tag"
    );
    require(
        api::parseGuildTag(json{{"clan", userTag["primary_guild"]}}) == tag,
        "Legacy clan metadata retains the same badge"
    );
    auto taggedMessage =
        api::parseMessages(
            json::array(
                {{{"id", "100"},
                  {"channel_id", "10"},
                  {"author", {{"id", "11"}, {"username", "maya"}, {"primary_guild", userTag["primary_guild"]}}},
                  {"content", "hello"}}}
            )
        ).front();
    require(taggedMessage.authorTag == tag, "REST and live message authors retain their guild tags");
    api::parseMessagePatch(json{{"author", {{"id", "11"}}}, {"content", "edited"}}).apply(taggedMessage);
    require(taggedMessage.authorTag == tag, "Partial author/message updates preserve omitted tag metadata");
    api::parseMessagePatch(json{{"author", {{"id", "11"}, {"primary_guild", nullptr}}}}).apply(taggedMessage);
    require(!taggedMessage.authorTag, "Explicit null tag updates clear an existing message badge");
    api::UserDirectory tags;
    auto liveUser = userTag;
    liveUser["id"] = "11";
    tags.consume("READY", json{{"users", json::array({liveUser})}});
    require(tags.guildTag("11") == tag, "READY seeds current user tag metadata");
    tags.consume("PRESENCE_UPDATE", json{{"user", {{"id", "11"}, {"primary_guild", 123}}}});
    require(tags.guildTag("11") == tag, "Malformed partial tag metadata does not erase the prior badge");
    tags.consume("PRESENCE_UPDATE", json{{"user", {{"id", "11"}, {"primary_guild", nullptr}}}});
    require(!tags.guildTag("11", tag), "A live clear overrides stale message-author tags");
    tags.consume("RESUMED", json::object());
    require(!tags.guildTag("11", tag), "Gateway resume preserves known tag clears");
    tags.consume("READY", json::object());
    require(tags.guildTag("11", tag) == tag, "A new session clears the previous tag directory");
    json response = {
        {"user",
         {{"id", "11"},
          {"username", "maya"},
          {"global_name", "Maya"},
          {"bio", "old"},
          {"banner", hash},
          {"accent_color", 0xBD78B1},
          {"avatar_decoration_data", {{"asset", hash}, {"sku_id", "123"}}},
          {"primary_guild",
           {{"identity_enabled", true}, {"identity_guild_id", "1"}, {"tag", "MOON"}, {"badge", hash}}}}},
        {"user_profile",
         {{"bio", "**Hello** <t:1754172000:t>"}, {"pronouns", "she/her"}, {"theme_colors", {0x884499, 0x123456}}}},
        {"guild_member_profile",
         {{"guild_id", "1"}, {"bio", "Server bio"}, {"banner", "a_" + hash}, {"accent_color", 0xFFFFFF}}},
        {"guild_member",
         {{"roles", {"50", "60", "60"}},
          {"nick", "Server Maya"},
          {"avatar", hash},
          {"joined_at", "2024-01-01T00:00:00Z"}}},
        {"badges", json::array({{{"description", "Early Supporter"}, {"icon", hash}}})},
        {"guild_badges", json::array({{{"description", "Server booster"}, {"icon", hash}}})},
        {"mutual_friends_count", 2},
        {"mutual_friends", json::array({{{"id", "12"}, {"username", "noah"}}})},
        {"mutual_guilds", json::array({{{"id", "1"}}, {{"id", "2"}}, {{"id", "1"}}, {{"id", "../invalid"}}})},
        {"token", "synthetic-private-response-field"},
        {"analytics_token", "synthetic-only"}
    };
    require(
        api::accountCreationDate("4194304000") == "01 Jan 2015" && api::accountCreationDate("not-an-id").empty(),
        "Account creation dates use a validated timestamp and stable UTC date"
    );
    auto global = api::parseUserProfile(response);
    require(
        global.user.id == "11"
            && global.user.name == "Maya"
            && global.user.bio == "**Hello** <t:1754172000:t>"
            && global.user.pronouns == "she/her",
        "Global profile metadata overrides account fields"
    );
    require(
        global.banner == "/banners/11/" + hash + ".png?size=512"
            && global.accent == 0xBD78B1
            && global.theme == std::array<std::uint32_t, 2>{0x884499, 0x123456},
        "Banner, accent and theme are retained independently"
    );
    require(
        global.guild.empty() && !global.roles && global.badges.size() == 1,
        "Global reads never apply unrelated guild fields"
    );
    require(
        global.decoration == api::collectibleAsset("123")
            && global.guildTag == "MOON"
            && global.guildBadge == "/guild-tag-badges/1/" + hash + ".png?size=32",
        "Decorations and guild tags produce fixed CDN paths"
    );
    require(
        global.mutualFriendCount == 2 && global.mutualFriends.size() == 1 && global.mutualGuildCount == 2,
        "Mutual counts are real server values and guild IDs are deduplicated"
    );
    auto server = api::parseUserProfile(response, "1");
    require(
        server.guild == "1"
            && server.user.name == "Server Maya"
            && server.user.bio == "Server bio"
            && server.user.pronouns == "she/her"
            && server.roles
            && server.badges.size() == 2,
        "Requested guild profile overrides only supplied fields"
    );
    require(
        server.banner == "/guilds/1/users/11/banners/a_" + hash + ".gif?size=512"
            && server.user.avatar == "/guilds/1/users/11/avatars/" + hash + ".png?size=128"
            && server.accent == global.accent,
        "Guild avatar/banner use guild routes; guild accent is not applied"
    );
    require(
        api::parseUserProfile(response, "2").guild.empty(), "Mismatched guild metadata cannot leak into another server"
    );
    response["guild_member_profile"]["bio"] = "";
    response["guild_member_profile"]["pronouns"] = nullptr;
    require(
        api::parseUserProfile(response, "1").user.bio.empty()
            && api::parseUserProfile(response, "1").user.pronouns == "she/her",
        "Explicit empty text clears; null metadata retains the global fallback"
    );
    response["private"] = true;
    auto hidden = api::parseUserProfile(response, "1");
    require(
        hidden.privateProfile && hidden.user.bio.empty() && hidden.user.pronouns.empty() && hidden.badges.empty(),
        "Private profiles do not expose cached public biography metadata"
    );
    auto minimal =
        api::parseUserProfile(json{{"user", {{"id", "11"}, {"username", "maya"}}}, {"user_profile", nullptr}});
    require(
        !minimal.mutualFriendCount && !minimal.mutualGuildCount && minimal.banner.empty() && !minimal.roles,
        "Omitted profile fields stay unknown instead of fabricating zero counts"
    );
    auto invalid = json{
        {"user",
         {{"id", "11"},
          {"banner", "../../x"},
          {"bio", std::string(4097, 'x')},
          {"avatar_decoration_data", {{"asset", "https://invalid.example/"}}},
          {"accent_color", 0x1000000}}},
        {"user_profile", {{"theme_colors", {-1, 10}}, {"pronouns", std::string(161, 'x')}}},
        {"badges", json::array({{{"icon", "../evil"}}})}
    };
    auto sanitized = api::parseUserProfile(invalid);
    require(
        sanitized.banner.empty()
            && sanitized.decoration.empty()
            && sanitized.badges.empty()
            && !sanitized.accent
            && !sanitized.theme
            && sanitized.user.bio.empty()
            && sanitized.user.pronouns.empty(),
        "Malformed paths, colors and oversized metadata are discarded"
    );
    response.erase("private");
    response["badges"] = json::array();
    response["mutual_friends"] = json::array();
    for (int i = 0; i < 100; ++i) {
      response["badges"].push_back({{"icon", hash}, {"description", "Badge"}});
      response["mutual_friends"].push_back({{"id", std::to_string(100 + i)}});
    }
    auto bounded = api::parseUserProfile(response, "1");
    require(
        bounded.badges.size() == 16 && bounded.mutualFriends.size() == 3, "Profile thumbnails and badges stay bounded"
    );

    api::RoleDirectory directory;
    directory.apply(
        api::parseRoleUpdates(
            "GUILD_CREATE",
            json{
                {"id", "1"},
                {"roles",
                 json::array(
                     {{{"id", "1"}, {"name", "@everyone"}},
                      {{"id", "50"}, {"name", "Member"}, {"position", 1}},
                      {{"id", "60"}, {"name", "Artist"}, {"position", 2}, {"color", 0x123456}},
                      {{"id", "70"}, {"name", "Supporter"}, {"position", 2}}}
                 )}
            }
        )
    );
    auto chips = directory.roles("1", "11", std::vector<std::string>{"1", "50", "60", "60", "70", "999"});
    require(
        chips.size() == 3 && chips[0].id == "60" && chips[1].id == "70" && chips[2].name == "Member",
        "Role chips include colorless roles in hierarchy order; omit everyone, missing roles and duplicates"
    );
    directory.apply(
        api::parseRoleUpdates(
            "GUILD_MEMBER_UPDATE", json{{"guild_id", "1"}, {"user_id", "11"}, {"roles", json::array()}}
        )
    );
    require(
        directory.roles("1", "11", std::vector<std::string>{"50"}).empty(),
        "Live membership wins over an older profile reply"
    );
    require(directory.roles("2", "11", std::vector<std::string>{"50"}).empty(), "Profile roles cannot cross guilds");

    directory.apply(
        api::parseRoleUpdates(
            "GUILD_ROLE_CREATE",
            json{
                {"guild_id", "1"},
                {"role", {{"id", "80"}, {"name", "Icon role"}, {"position", 10}, {"color", 0}, {"icon", hash}}}
            }
        )
    );
    auto icon = directory.icon("1", "20", std::vector<std::string>{"60", "80"});
    require(
        icon && icon->id == "80" && icon->icon == "/role-icons/80/" + hash + ".png?size=32",
        "Highest icon role is independent of the highest colored role"
    );
    require(
        directory.colors("1", "20", std::vector<std::string>{"60", "80"})->primary == 0x123456,
        "A colorless icon role does not replace the name color"
    );
    directory.apply(
        api::parseRoleUpdates(
            "GUILD_ROLE_UPDATE",
            json{
                {"guild_id", "1"},
                {"role",
                 {{"id", "80"}, {"name", "Moon role"}, {"position", 10}, {"icon", nullptr}, {"unicode_emoji", "🌙"}}}
            }
        )
    );
    icon = directory.icon("1", "20", std::vector<std::string>{"80"});
    require(
        icon && icon->icon.empty() && icon->unicodeEmoji == "🌙",
        "Live role edits can replace a custom icon with a Unicode emoji"
    );
    directory.apply(api::parseRoleUpdates("GUILD_ROLE_DELETE", json{{"guild_id", "1"}, {"role_id", "80"}}));
    require(!directory.icon("1", "20", std::vector<std::string>{"80"}), "Deleted role icons disappear");

    auto guildPreview = api::parseGuildProfile(
        json{
            {"id", "99"},
            {"name", "Moon Cafe"},
            {"icon_hash", hash},
            {"custom_banner_hash", hash},
            {"banner_hash", "a_" + hash},
            {"description", "Hello <markup>"},
            {"member_count", 75205},
            {"online_count", 23918},
            {"tag", "MOON"},
            {"badge_hash", hash.substr(1)},
            {"brand_color_primary", "#abcdef"},
            {"visibility", 1},
            {"features", {"VERIFIED"}},
            {"traits", json::array({{{"label", "art"}, {"emoji_name", "🎨"}}})}
        }
    );
    require(
        guildPreview.id == "99"
            && guildPreview.joinable
            && guildPreview.verified
            && guildPreview.members == 75205
            && guildPreview.online == 23918
            && guildPreview.accent == 0xABCDEF
            && guildPreview.traits.size() == 1
            && guildPreview.traits.front().emoji == "🎨",
        "Server previews project real counts, traits, public visibility and brand metadata"
    );
    require(
        guildPreview.banner == "/discovery-splashes/99/" + hash + ".png?size=512"
            && guildPreview.tag
            && !guildPreview.tag->badge.empty(),
        "Discovery banners take priority and bounded hexadecimal badge hashes need not be padded"
    );
    auto restricted =
        api::parseGuildProfile(json{{"id", "99"}, {"visibility", 2}, {"member_count", nullptr}, {"online_count", -1}});
    require(
        !restricted.joinable && !restricted.members && !restricted.online,
        "Private servers require invitations and missing counts remain unknown"
    );
    require(
        api::parseGuildProfile(json{{"id", "99"}, {"visibility", 3}}).recruitment,
        "Recruitment servers are distinguished from immediate public joining"
    );
    for (const auto& path :
         {global.banner, global.decoration, global.guildBadge, global.badges.front().image, guildPreview.banner,
          guildPreview.tag->badge, std::string("/role-icons/80/") + hash + ".png?size=32",
          std::string("/guilds/1/users/11/banners/") + hash + ".png?size=512"}) {
      require(!api::mediaUrl(path).empty(), "Every projected profile/tag/role CDN path must pass the media allowlist");
      require(
          api::mediaUrl("https://cdn.discordapp.com.evil.test" + path).empty(),
          "New asset routes preserve exact origin checks"
      );
    }

    setenv("TZ", "UTC", 1);
    tzset();
    require(app::discordTimestamp("t:1754172000:t") == "22:00", "Profile timestamps use local clock formatting");
    require(
        app::discordTimestamp("t:0:T") == "00:00:00" && app::discordTimestamp("t:0:d") == "01/01/1970",
        "Timestamp styles retain time and date semantics"
    );
    require(
        app::discordTimestamp("t:7200:R", 0) == "in 2 hours" && app::discordTimestamp("t:0:R", 60) == "1 minute ago",
        "Relative timestamps can be formatted deterministically"
    );
    for (const std::string suffix : {"", ":t", ":T", ":d", ":D", ":f", ":F", ":R"}) {
      const auto timestamp = app::parseTimestampMention("t:1793041212" + suffix);
      require(
          timestamp
              && timestamp->seconds == 1793041212
              && app::timestampMentionTooltip(timestamp->seconds) == "Monday, 26 October 2026 at 19:00"
              && app::timestampMentionTooltip(timestamp->seconds, true) == "Monday, 26 October 2026 at 7:00 PM",
          "Every display style shares one exact instant and full-date hover caption"
      );
    }
    require(
        app::timestampMentionTooltip(0) == "Thursday, 1 January 1970 at 00:00"
            && app::timestampMentionTooltip(0, true) == "Thursday, 1 January 1970 at 12:00 AM"
            && app::timestampMentionTooltip(-1) == "Wednesday, 31 December 1969 at 23:59"
            && app::timestampMentionTooltip(-1, true) == "Wednesday, 31 December 1969 at 11:59 PM",
        "Timestamp hover captions preserve epoch zero, negative instants and midnight clock conventions"
    );
    require(
        app::timestampMentionTooltip(app::TimestampMention::minimumSeconds - 1).empty()
            && app::timestampMentionTooltip(app::TimestampMention::maximumSeconds + 1).empty(),
        "Out-of-range instants never produce misleading timestamp hover captions"
    );
    for (const auto token :
         {"t:", "t:123:x", "t:123:tt", "t:123x:t", "t:999999999999999999999", "t:253402300800", "t:-62135596801"})
      require(!app::discordTimestamp(token), "Malformed or out-of-range timestamps remain literal");
    require(setenv("TZ", "IST-5:30", 1) == 0, "The half-hour test timezone can be set");
    tzset();
    require(
        app::timestampMentionTooltip(1793041212) == "Tuesday, 27 October 2026 at 00:30"
            && app::timestampMentionTooltip(1793041212, true) == "Tuesday, 27 October 2026 at 12:30 AM",
        "Timestamp hover captions convert the full date and both clock formats through local midnight"
    );
    require(setenv("TZ", "PST8PDT,M3.2.0/2,M11.1.0/2", 1) == 0, "The daylight-saving test timezone can be set");
    tzset();
    require(
        app::timestampMentionTooltip(1754172000) == "Saturday, 2 August 2025 at 15:00"
            && app::timestampMentionTooltip(0, true) == "Wednesday, 31 December 1969 at 4:00 PM",
        "Timestamp hover captions follow local daylight-saving rules and calendar-year boundaries"
    );
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
