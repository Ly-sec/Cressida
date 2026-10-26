#include "api/voice_roster.h"

#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
  using Json = nlohmann::json;
  void require(bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  }
  void apply(api::VoiceRoster& roster, std::string_view type, const Json& value) {
    const auto update = api::voiceRosterUpdate(type, value);
    require(update.has_value(), "A valid voice roster event projects into a typed update");
    roster.apply(*update);
  }
  Json state(std::string guild, std::string channel, std::string user) {
    return {{"guild_id", std::move(guild)}, {"channel_id", std::move(channel)}, {"user_id", std::move(user)}};
  }
} // namespace

int main() {
  try {
    api::VoiceRoster roster;
    const auto ready = Json::parse(R"({
      "users":[
        {"id":"11","username":"maya","global_name":"Global Maya","avatar":"abcdef","email":"private@example.invalid","token":"private-profile-token"},
        {"id":"12","username":"noah","global_name":null,"avatar":null}
      ],
      "guilds":[
        {"id":"1","members":[{"user_id":"11","nick":"Server Maya"}],"voice_states":[
          {"user_id":"11","channel_id":"100","mute":true,"deaf":false,"self_mute":false,"self_deaf":true,"suppress":true,"self_video":true,"self_stream":true,"session_id":"private-session"}
        ]},
        {"properties":{"id":"2"},"voice_states":[{"user_id":"12","channel_id":"200"}]}
      ],
      "merged_members":[[],[{"user_id":"12","nick":"Second Noah"}]]
    })");
    const auto parsedReady = api::voiceRosterUpdate("READY", ready);
    require(
        parsedReady && parsedReady->reset && parsedReady->resetGuilds.size() == 2 && parsedReady->members.size() == 2,
        "READY projects every explicit server voice snapshot with account-reset scope"
    );
    roster.apply(*parsedReady);
    const auto maya = roster.members("1", "100").front();
    require(
        maya.name == "Server Maya"
            && maya.avatar == "/avatars/11/abcdef.png?size=128"
            && maya.muted
            && !maya.deafened
            && !maya.selfMuted
            && maya.selfDeafened
            && maya.suppressed
            && maya.video
            && maya.stream,
        "Deduplicated users and guild nicknames hydrate public voice identities and all status flags"
    );
    require(
        roster.members("2", "200").front().name == "Second Noah"
            && roster.members("2", "200").front().avatar.starts_with("/embed/avatars/"),
        "Aligned merged members hydrate names independently for the correct server"
    );
    require(
        roster.count("1", "100") == 1 && roster.count("2", "200") == 1 && roster.count("9", "100") == 0,
        "Voice users appear without loading the selected channel member list"
    );

    apply(roster, "VOICE_STATE_UPDATE", {{"guild_id", "1"}, {"user_id", "11"}, {"self_mute", true}});
    auto changed = roster.members("1", "100").front();
    require(
        changed.selfMuted
            && changed.muted
            && changed.selfDeafened
            && changed.video
            && changed.stream
            && changed.name == maya.name
            && changed.avatar == maya.avatar,
        "A partial mute event preserves omitted channel, public profile and independent status flags"
    );
    apply(roster, "VOICE_STATE_UPDATE", state("1", "101", "11"));
    changed = roster.members("1", "101").front();
    require(
        roster.count("1", "100") == 0
            && changed.selfMuted
            && changed.name == maya.name
            && changed.avatar == maya.avatar,
        "Live voice moves replace the user's channel while preserving omitted profile and flag fields"
    );
    apply(roster, "VOICE_STATE_UPDATE", state("2", "201", "11"));
    apply(roster, "VOICE_STATE_UPDATE", {{"guild_id", "1"}, {"user_id", "11"}, {"channel_id", nullptr}});
    require(
        roster.count("1", "101") == 0 && roster.count("2", "201") == 1,
        "A nullable departure removes only that user's state in the event's server"
    );
    apply(
        roster, "VOICE_STATE_UPDATE",
        {{"guild_id", "2"},
         {"user_id", "11"},
         {"channel_id", "201"},
         {"member",
          {{"nick", "Literal **nickname**"}, {"user", {{"id", "11"}, {"username", "maya"}, {"avatar", "123abc"}}}}}}
    );
    require(
        roster.members("2", "201").front().name == "Literal **nickname**", "Received public voice names remain literal"
    );
    apply(
        roster, "GUILD_MEMBER_UPDATE",
        {{"guild_id", "2"}, {"nick", "Renamed"}, {"user", {{"id", "11"}, {"username", "maya"}, {"avatar", nullptr}}}}
    );
    require(
        roster.members("2", "201").front().name == "Renamed"
            && roster.members("2", "201").front().avatar.starts_with("/embed/avatars/"),
        "Public member updates can refresh a present user's name and explicit cleared avatar without moving it"
    );
    apply(roster, "GUILD_MEMBER_REMOVE", {{"guild_id", "2"}, {"user", {{"id", "11"}}}});
    require(
        roster.count("2", "201") == 0 && roster.count("2", "200") == 1,
        "Guild member removal retires only the matching voice participant"
    );

    apply(roster, "VOICE_STATE_UPDATE", {{"guild_id", "2"}, {"user_id", "12"}, {"mute", true}});
    apply(roster, "GUILD_CREATE", {{"id", "2"}});
    require(roster.count("2", "200") == 1, "An omitted voice snapshot preserves the cached server roster");
    apply(
        roster, "GUILD_CREATE",
        {{"id", "2"}, {"voice_states", Json::array({{{"user_id", "12"}, {"channel_id", "202"}}})}}
    );
    require(
        roster.count("2", "200") == 0
            && roster.members("2", "202").front().name == "Second Noah"
            && roster.members("2", "202").front().muted,
        "An explicit scoped snapshot replaces membership while preserving omitted metadata for retained users"
    );
    apply(
        roster, "VOICE_STATE_UPDATE",
        {{"guild_id", "1"},
         {"user_id", "13"},
         {"channel_id", "100"},
         {"member", {{"user", {{"id", "13"}, {"username", "third"}, {"avatar", "abcdef"}}}}}}
    );
    apply(roster, "READY_SUPPLEMENTAL", {{"guilds", Json::array({{{"id", "2"}, {"voice_states", Json::array()}}})}});
    require(
        roster.count("2", "202") == 0 && roster.count("1", "100") == 1,
        "Supplemental snapshots clear only explicitly supplied server collections"
    );
    apply(roster, "CHANNEL_DELETE", {{"guild_id", "1"}, {"id", "100"}});
    require(roster.count("1", "100") == 0, "Deleted voice channels drop their participants");
    apply(roster, "VOICE_STATE_UPDATE", state("1", "100", "13"));
    apply(roster, "GUILD_DELETE", {{"id", "1"}});
    require(roster.count("1", "100") == 0, "Server removal clears its voice roster");
    apply(roster, "VOICE_STATE_UPDATE", {{"guild_id", nullptr}, {"channel_id", "300"}, {"user_id", "14"}});
    require(roster.count("", "300") == 1, "Direct voice states can be projected without inventing a server ID");
    apply(roster, "VOICE_STATE_UPDATE", {{"channel_id", nullptr}, {"user_id", "14"}});
    require(roster.count("", "300") == 0, "Direct voice departures retain their independent scope");

    for (const auto& invalid : Json::array(
             {{{"guild_id", "1"}, {"channel_id", "100"}, {"user_id", "x"}},
              {{"guild_id", "1"}, {"channel_id", "100"}, {"user_id", "18446744073709551616"}},
              {{"guild_id", "1"}, {"channel_id", "100"}, {"user_id", "0"}},
              {{"guild_id", "1"}, {"channel_id", 100}, {"user_id", "11"}},
              {{"guild_id", "1"}, {"channel_id", "100"}, {"user_id", "11"}, {"mute", "true"}},
              {{"guild_id", "1"},
               {"channel_id", "100"},
               {"user_id", "11"},
               {"member", {{"user", {{"id", "12"}, {"username", "wrong"}}}}}},
              {{"guild_id", "1"},
               {"channel_id", "100"},
               {"user_id", "11"},
               {"member", {{"guild_id", "2"}, {"nick", "wrong server"}}}},
              {{"guild_id", "1"}, {"channel_id", "100"}, {"user_id", "11"}, {"user", {{"id", "12"}}}},
              {{"guild_id", "1"},
               {"channel_id", "100"},
               {"user_id", "11"},
               {"member", {{"user", {{"id", "11"}, {"username", 5}}}}}}}
         ))
      require(
          !api::voiceRosterUpdate("VOICE_STATE_UPDATE", invalid),
          "Malformed identities, mismatched profiles and incorrectly typed flags are rejected"
      );
    require(
        !api::voiceRosterUpdate(
            "GUILD_CREATE", {{"id", "1"}, {"properties", {{"id", "2"}}}, {"voice_states", Json::array()}}
        )
            && !api::voiceRosterUpdate(
                "GUILD_CREATE", {{"id", "1"}, {"voice_states", Json::array({state("2", "100", "11")})}}
            ),
        "Outer, nested and per-state server identities must agree"
    );
    require(
        !api::voiceRosterUpdate(
            "READY",
            {{"guilds",
              Json::array(
                  {{{"id", "1"}, {"voice_states", Json::array({state("1", "100", "11"), state("1", "101", "11")})}}}
              )}}
        ),
        "An ambiguous duplicate voice user cannot enter a snapshot"
    );
    require(
        !api::voiceRosterUpdate(
            "READY_SUPPLEMENTAL", {{"guilds", Json::array({{{"id", "1"}}})}, {"merged_members", Json::array()}}
        ),
        "Merged metadata must align with its server snapshot array"
    );
    require(
        !api::voiceRosterUpdate("MESSAGE_CREATE", Json::object())
            && !api::voiceRosterUpdate("VOICE_STATE_UPDATE", Json::array()),
        "Unrelated events and malformed containers do not project voice changes"
    );

    api::VoiceRosterUpdate many;
    for (std::size_t guild = 1; guild <= 5; ++guild)
      for (std::size_t member = 1; member <= api::maximumGuildVoiceMembers + 1; ++member)
        many.members.push_back(
            {.guild = std::to_string(guild), .channel = std::to_string(guild * 100), .user = std::to_string(member)}
        );
    roster.apply(many);
    std::size_t count = 0;
    for (std::size_t guild = 1; guild <= 5; ++guild) {
      const auto size = roster.count(std::to_string(guild), std::to_string(guild * 100));
      require(size <= api::maximumGuildVoiceMembers, "Each server voice roster has a fixed member bound");
      count += size;
    }
    require(count == api::maximumVoiceMembers, "The account-wide voice roster budget bounds retained identities");
    auto oversized = Json::array();
    for (std::size_t member = 0; member <= api::maximumGuildVoiceMembers; ++member)
      oversized.push_back({{"user_id", std::to_string(member + 1)}, {"channel_id", "100"}});
    require(
        !api::voiceRosterUpdate("GUILD_CREATE", {{"id", "1"}, {"voice_states", std::move(oversized)}}),
        "Oversized snapshots are rejected before replacing an existing roster"
    );
    apply(roster, "READY", {{"guilds", Json::array()}});
    require(roster.count("1", "100") == 0, "A fresh account READY resets old voice identities");
    roster.apply(many);
    roster.clear();
    require(
        roster.members("1", "100").empty() && roster.count("5", "500") == 0,
        "Explicit account clear releases every roster and resets budgets"
    );
    std::cout << "Voice roster snapshots, public hydration, live moves, scope, validation and bounds passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
