#include "api/models.h"
#include "api/read_state.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
int main() {
  using nlohmann::json;
  auto require = [](bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  };
  try {
    api::ReadState state;
    state.ready(
        json{
            {"guilds",
             json::array(
                 {{{"id", "1"},
                   {"channels",
                    json::array(
                        {{{"id", "10"}, {"last_message_id", "100"}}, {{"id", "11"}, {"last_message_id", "99"}}}
                    )}}}
             )},
            {"private_channels", json::array({{{"id", "20"}, {"last_message_id", "900"}}})},
            {"read_state",
             {{"entries",
               json::array(
                   {{{"id", "10"}, {"last_message_id", "99"}},
                    {{"id", "11"}, {"last_message_id", "99"}},
                    {{"id", "20"}, {"last_message_id", "899"}},
                    {{"id", "77"}, {"read_state_type", 2}, {"mention_count", 1}}}
               )}}}
        }
    );
    require(
        state.unread() == std::map<std::string, std::string>{{"10", "1"}, {"20", ""}},
        "Initial unread channels compare numeric snowflakes; ignore non-channel states"
    );
    require(
        state.unreadCursors()
            == std::map<std::string, api::UnreadCursor>{{"10", {"99", "100"}}, {"20", {"899", "900"}}},
        "Unread snapshots expose exact read cursors, without unrelated channel states"
    );
    state.acknowledge(json{{"channel_id", "10"}, {"message_id", "100"}});
    state.message(json{{"channel_id", "10"}, {"id", "102"}, {"flags", 64}, {"author", {{"id", "3"}}}}, "2");
    require(!state.unreadCursors().contains("10"), "Ephemeral application replies never advance public unread state");
    require(!state.unreadCursors().contains("10"), "Read cursor snapshots omit acknowledged channels");
    require(!state.unread().contains("10"), "Another client's read acknowledgement clears channel state");
    state.channels(json::array({{{"id", "10"}, {"last_message_id", "101"}}}), "1");
    require(state.unread().contains("10"), "Passive channel updates mark new messages unread");
    state.message(json{{"channel_id", "20"}, {"id", "901"}, {"author", {{"id", "2"}}}}, "2");
    require(!state.unread().contains("20"), "Your own DM message acknowledges prior unread messages");
    state.acknowledge(json{{"channel_id", "20"}, {"message_id", "899"}});
    require(!state.unread().contains("20"), "Late acknowledgements cannot rewind normal read state");
    state.acknowledge(json{{"channel_id", "20"}, {"message_id", "899"}, {"manual", true}});
    require(state.unread().contains("20"), "Explicit mark-unread can rewind read state");
    state.ready(json::object());
    require(state.unread().empty(), "Fresh login replaces previous account read state");
    state.ready(
        json{
            {"read_state",
             {{"entries",
               json::array(
                   {{{"id", "10"}, {"last_message_id", "99"}, {"mention_count", 2}},
                    {{"id", "20"}, {"last_message_id", "899"}}}
               )}}}
        }
    );
    state.context(
        "GUILD_CREATE",
        json{
            {"id", "1"},
            {"channels",
             json::array({{{"id", "10"}, {"last_message_id", "100"}}, {{"id", "11"}, {"last_message_id", "101"}}})}
        },
        "2"
    );
    require(
        state.unread().at("10") == "1"
            && state.unread().contains("11")
            && state.unreadCursors().at("10").latest == "100"
            && state.unreadCursors().at("10").mentions == 2,
        "Delayed guild hydration restores unread styling and scopes imported mention badges to the server"
    );
    state.context(
        "READY_SUPPLEMENTAL",
        json{{"lazy_private_channels", json::array({{{"id", "20"}, {"last_message_id", "900"}}})}}, "2"
    );
    require(
        state.unreadCursors().at("20").after == "899" && state.unreadCursors().at("20").latest == "900",
        "Lazy private channels hydrate latest messages without dropping their initial read cursor"
    );
    state.acknowledge(json{{"channel_id", "10"}, {"message_id", "100"}});
    state.context("CHANNEL_UPDATE", json{{"guild_id", "1"}, {"id", "10"}, {"last_message_id", "102"}}, "2");
    require(
        state.unread().at("10") == "1" && state.unreadCursors().at("10").after == "100",
        "Later channel metadata restores new unread state without replacing acknowledgements"
    );
    state.context("CHANNEL_UPDATE", json{{"guild_id", "1"}, {"id", "10"}, {"last_message_id", "99"}}, "2");
    require(state.unreadCursors().at("10").latest == "102", "Stale metadata cannot rewind the newest message");
    state.context(
        "READY_SUPPLEMENTAL",
        json{
            {"guilds",
             json::array({{{"id", "1"}, {"threads", json::array({{{"id", "12"}, {"last_message_id", "103"}}})}}})}
        },
        "2"
    );
    require(state.unread().at("12") == "1", "Supplemental threads retain their server association");
    state.context("CHANNEL_CREATE", json{{"guild_id", "1"}, {"id", "13"}, {"last_message_id", "104"}}, "2");
    require(state.unread().at("13") == "1", "Newly discovered channels can expose existing unread history");
    state.context(
        "GUILD_CREATE",
        json{
            {"id", "1"},
            {"channels",
             json::array({{{"id", "10"}, {"last_message_id", "102"}}, {{"id", "invalid"}, {"last_message_id", "200"}}})}
        },
        "2"
    );
    require(
        state.unreadCursors().at("10").after == "100" && !state.unread().contains("invalid"),
        "Repeated hydration preserves reads and ignores malformed channel identities"
    );
    state.ready(json::object());
    auto count = [&](const std::string& channel) {
      const auto cursors = state.unreadCursors();
      const auto found = cursors.find(channel);
      return found == cursors.end() ? 0 : found->second.mentions;
    };
    auto message = [](const char* id, bool ping = false) {
      json value{{"id", id}, {"channel_id", "10"}, {"guild_id", "1"}, {"author", {{"id", "3"}}}};
      if (ping)
        value["mentions"] = json::array({{{"id", "2"}}});
      return value;
    };
    state.ready(
        json{
            {"user", {{"id", "2"}}},
            {"guilds",
             json::array({{{"id", "1"}, {"channels", json::array({{{"id", "10"}, {"last_message_id", "100"}}})}}})},
            {"merged_members", json::array({json::array({{{"user_id", "2"}, {"roles", json::array({"50"})}}})})},
            {"read_state",
             json::array(
                 {{{"id", "10"}, {"last_message_id", "99"}, {"mention_count", 12}},
                  {{"id", "20"}, {"mention_count", 3}}}
             )}
        }
    );
    require(
        count("10") == 12 && count("20") == 3,
        "Import server mention and DM unread counts, even without latest metadata"
    );
    state.message(message("100", true), "2");
    require(count("10") == 12, "READY already includes historical mentions; do not count replayed messages");
    state.message(message("101", false), "2");
    require(count("10") == 12, "Ordinary guild messages are unread, but do not create mention badges");
    state.message(message("102", true), "2");
    state.message(message("102", true), "2");
    require(count("10") == 13, "Count a direct mention once across duplicate dispatches");
    state.message(message("103", true), "2", true);
    require(count("10") == 13, "Blocked authors do not increment mention badges");
    auto role = message("104");
    role["mention_roles"] = json::array({"50", "51"});
    state.message(role, "2");
    require(count("10") == 14, "Own READY merged-member roles resolve role mentions");
    state.context(
        "USER_GUILD_SETTINGS_UPDATE", json{{"guild_id", "1"}, {"suppress_roles", true}, {"suppress_everyone", true}},
        "2"
    );
    role["id"] = "105";
    role["mention_everyone"] = true;
    state.message(role, "2");
    require(count("10") == 14, "Suppressed roles and everyone do not count");
    role["id"] = "106";
    role["mentions"] = json::array({{{"id", "2"}}});
    state.message(role, "2", false, true);
    require(count("10") == 15, "Explicit user pings survive suppression and channel mute");
    state.context(
        "USER_GUILD_SETTINGS_UPDATE", json{{"guild_id", "1"}, {"suppress_roles", false}, {"suppress_everyone", false}},
        "2"
    );
    state.context(
        "GUILD_MEMBER_UPDATE", json{{"guild_id", "1"}, {"user", {{"id", "2"}}}, {"roles", json::array({"51"})}}, "2"
    );
    role = message("107");
    role["mention_roles"] = json::array({"50"});
    state.message(role, "2");
    require(count("10") == 15, "Cleared membership stops matching a former role");
    role["id"] = "108";
    role["mention_roles"] = json::array({"51"});
    state.message(role, "2");
    auto everyone = message("109");
    everyone["mention_everyone"] = true;
    state.message(everyone, "2");
    require(count("10") == 17, "Live membership and unsuppressed everyone pings update badges");
    state.channels(json::array({{{"id", "10"}, {"last_message_id", "110"}}}), "1");
    state.message(message("110", true), "2");
    require(count("10") == 18, "Passive channel updates arriving before a create must not hide its mention");
    state.removed(json{{"channel_id", "10"}, {"ids", json::array({"108", "109", "109"})}});
    require(count("10") == 16, "Deleted known mentions decrement once; duplicate deletes do not underflow");
    state.acknowledge(json{{"channel_id", "10"}, {"message_id", "106"}, {"mention_count", 1}});
    require(count("10") == 1, "Other-device partial ACK supplies authoritative remaining mentions");
    require(
        state.unreadCursors().at("10").mentionMessages == std::vector<std::string>{"110"},
        "ACK removes old live mention identities"
    );
    state.acknowledge(json{{"channel_id", "10"}, {"message_id", "105"}, {"mention_count", 80}});
    require(count("10") == 1, "Delayed ACK must not resurrect older counts");
    state.acknowledge(json{{"channel_id", "10"}, {"message_id", "110"}, {"mention_count", 0}});
    require(count("10") == 0 && !state.unread().contains("10"), "Reading clears both counts and normal unreads");
    state.acknowledge(json{{"channel_id", "10"}, {"message_id", "99"}, {"manual", true}, {"mention_count", 9}});
    require(count("10") == 9, "Manual mark-unread restores supplied count");
    auto own = message("111");
    own["author"]["id"] = "2";
    own["type"] = 46;
    state.message(own, "2");
    require(count("10") == 9, "Own poll results must not auto-read a conversation");
    own["id"] = "112";
    own["type"] = 0;
    state.message(own, "2");
    require(count("10") == 0, "Own ordinary messages clear mentions");
    auto dm = json{{"id", "1000"}, {"channel_id", "20"}, {"author", {{"id", "3"}}}};
    state.message(dm, "2");
    require(count("20") == 4, "Every incoming unmuted DM adds a badge");
    dm["id"] = "1001";
    state.message(dm, "2", false, true);
    require(count("20") == 4, "Muted ordinary DMs do not add a badge");
    dm["id"] = "1002";
    dm["mentions"] = json::array({{{"id", "2"}}});
    state.message(dm, "2", false, true);
    require(count("20") == 5, "A directly mentioned muted DM still adds a badge");
    dm["id"] = "1003";
    dm["type"] = 2;
    state.message(dm, "2");
    require(count("20") == 5, "Recipient removal does not badge DMs");
    state.context("CHANNEL_DELETE", json{{"id", "20"}}, "2");
    require(!state.unread().contains("20"), "Deleted conversations lose their counts");
    state.ready(
        json{
            {"read_state",
             {{"entries",
               json::array(
                   {{{"id", "10"}, {"mention_count", -2}},
                    {{"id", "20"}, {"mention_count", "bad"}},
                    {{"id", "30"}, {"mention_count", 18446744073709551615ULL}},
                    {{"id", "40"}, {"read_state_type", "bad"}, {"mention_count", 5}}}
               )}}}
        }
    );
    require(
        count("10") == 0 && count("20") == 0 && count("30") == 1000000 && count("40") == 0,
        "Optional malformed/negative counts cannot break READY or overflow aggregates"
    );
    state.ready(json::object());
    require(state.unreadCursors().empty(), "Account reset clears badge data and live mention journal");
    auto entries = api::parseSettingsEntries(
        "devices",
        json{
            {"user_sessions",
             json::array(
                 {{{"id_hash", "secret"},
                   {"client_info", {{"os", "Linux"}, {"platform", "Discord Client"}, {"location", "private"}}},
                   {"approx_last_used_time", "2026-10-17"}}}
             )}
        }
    );
    require(
        entries.size() == 1
            && entries[0].name == "Linux · Discord Client"
            && entries[0].id.empty()
            && entries[0].detail == "2026-10-17",
        "Device list projects display fields without session credentials or location"
    );
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
