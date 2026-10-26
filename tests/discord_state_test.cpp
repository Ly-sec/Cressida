#include "api/member_list_identity.h"
#include "api/models.h"
#include "api/role_styles.h"
#include "api/thread_member_list.h"

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
    const json participant = {
        {"user_id", "7"},
        {"member", {{"user", {{"id", "7"}, {"username", "Maya"}}}, {"nick", "Thread nickname"}, {"roles", {"50"}}}},
        {"presence", {{"status", "idle"}}}
    };
    json threadSnapshot = {{"guild_id", "1"}, {"thread_id", "20"}, {"members", json::array({participant})}};
    auto participants = api::threadMemberList(threadSnapshot, "1", "20");
    require(
        participants
            && participants->size() == 1
            && participants->front().user.id == "7"
            && participants->front().user.name == "Thread nickname"
            && participants->front().status == "idle",
        "Thread snapshots retain participant identity, nickname and presence"
    );
    api::RoleDirectory threadRoles;
    threadRoles.apply(api::parseRoleUpdates("THREAD_MEMBER_LIST_UPDATE", threadSnapshot));
    require(
        threadRoles.memberships("1", "7") == std::vector<std::string>{"50"},
        "Thread participants retain their guild role memberships"
    );
    require(
        !api::threadMemberList(threadSnapshot, "2", "20") && !api::threadMemberList(threadSnapshot, "1", "21"),
        "Other guild/thread snapshots cannot populate the selected participant list"
    );
    threadSnapshot["members"] = json::array();
    require(
        api::threadMemberList(threadSnapshot, "1", "20")->empty(),
        "Empty participant snapshots clear a previous thread list"
    );
    auto invalidParticipant = participant;
    invalidParticipant["member"]["user"]["id"] = "8";
    threadSnapshot["members"] = json::array({invalidParticipant});
    require(
        !api::threadMemberList(threadSnapshot, "1", "20"), "Mismatched participant and account IDs reject the snapshot"
    );
    threadSnapshot["members"] = json::array({participant, participant});
    require(!api::threadMemberList(threadSnapshot, "1", "20"), "Duplicate participants reject the snapshot");
    threadSnapshot["members"] = json::array();
    for (int index = 1; index <= 125; ++index) {
      auto row = participant;
      row["user_id"] = std::to_string(index);
      row["member"]["user"]["id"] = std::to_string(index);
      threadSnapshot["members"].push_back(std::move(row));
    }
    require(
        api::threadMemberList(threadSnapshot, "1", "20")->size() == 125,
        "Thread participants are not truncated to one guild range or the approximate thread count"
    );
    // Independent fixture encoded with Python struct.pack('<Q') and base64.
    const auto layout = api::parseGuildLayout(
        json{
            {"settings",
             "ck8KJwoQLAEAAAAAAABkAAAAAAAAABICCGQaCQoHRnJpZW5kcyIECLPERAoKCgjIAAAAAAAAABIYyAAAAAAAAABkAAAAAAAAACwBAAAAA"
             "AAA8gEHaWdub3JlZA=="}
        }
    );
    require(
        layout.folders.size() == 2 && layout.folders[0].id == "100" && layout.folders[0].name == "Friends",
        "Decode folder wrappers and names"
    );
    require(
        layout.folders[0].color == 0x112233 && layout.folders[0].guildIds == std::vector<std::string>{"300", "100"},
        "Decode packed fixed64 IDs without numeric precision loss"
    );
    require(
        layout.positions == std::vector<std::string>{"200", "100", "300"}, "Decode legacy ordering alongside folders"
    );
    auto order = api::arrangeGuilds({{"100", "A"}, {"200", "B"}, {"300", "C"}, {"400", "D"}}, layout);
    require(
        order.size() == 3
            && order[0].guildIds == std::vector<std::string>{"300", "100"}
            && order[1].guildIds[0] == "200"
            && order[2].guildIds[0] == "400",
        "Follow saved folder order and append newly joined servers"
    );
    auto duplicate = layout;
    duplicate.folders.push_back({"101", "Duplicates", 0, {"100", "999"}});
    require(
        api::arrangeGuilds({{"100", "A"}, {"200", "B"}, {"300", "C"}}, duplicate).size() == 2,
        "Ignore duplicate and departed server references"
    );
    for (const auto* invalid : {"!!!!", "cgUK", "//8=", "cgEK"}) {
      bool rejected = false;
      try {
        (void)api::parseGuildLayout(json{{"proto", invalid}});
      } catch (...) {
        rejected = true;
      }
      require(rejected, "Reject malformed or truncated settings");
    }
    auto legacy = api::parseGuildLayout(
        json{
            {"guild_folders", json::array({{{"id", nullptr}, {"guild_ids", json::array({"200"})}}})},
            {"guild_positions", json::array({"300", "100"})}
        }
    );
    require(
        api::arrangeGuilds({{"100", "A"}, {"200", "B"}, {"300", "C"}}, legacy)[0].guildIds[0] == "200",
        "Accept legacy settings without inventing folders"
    );
    require(
        api::avatarPath(json{{"id", "123"}, {"avatar", "a_abc"}}) == "/avatars/123/a_abc.png?size=128",
        "Request the static frame of animated avatars"
    );
    require(
        api::avatarPath(json{{"id", "123"}, {"avatar", nullptr}}) == "/embed/avatars/0.png",
        "Use Discord default avatars for accounts without custom images"
    );
    require(api::emojiPath("1/../2").empty(), "CDN resources must use validated identifiers");
    {
      api::Channel channel{"20", "chat"};
      require(
          !api::memberListIdentity("1", "1024", channel),
          "Missing channel overwrites cannot establish shared visibility"
      );
      channel.overwrites.emplace();
      const auto publicList = api::memberListIdentity("1", "1024", channel);
      require(publicList && publicList->wireId == "everyone", "Public channels use the shared everyone wire list");
      require(
          api::memberListIdentity("1", "0", channel)->wireId == "0",
          "A guild without default VIEW_CHANNEL hashes the empty overwrite set"
      );
      channel.overwrites =
          std::vector<api::PermissionOverwrite>{{"5", "0", "1024", 0}, {"9", "18446744073709552640", "1024", 1}};
      const auto restricted = api::memberListIdentity("1", "1024", channel);
      // Independent Python struct/int MurmurHash3 fixture for "allow:9,deny:5".
      require(
          restricted && restricted->wireId == "320206352",
          "Restricted lists match the unsigned wire hash, including unknown high permission bits and allow priority"
      );
      std::ranges::reverse(*channel.overwrites);
      require(
          api::memberListIdentity("1", "1024", channel) == restricted,
          "Overwrite ordering cannot change a member-list identity"
      );
      auto other = channel;
      other.id = "21";
      require(
          api::memberListIdentity("1", "1024", other) == restricted,
          "Equal visibility shares across different ordinary channels in the same guild"
      );
      require(
          api::memberListIdentity("2", "1024", channel)->signature != restricted->signature,
          "Member-list cache identities never cross guild boundaries"
      );
      auto changed = api::memberListIdentity("1", "0", channel);
      require(
          changed->wireId == restricted->wireId && changed->signature != restricted->signature,
          "The full local signature retains base visibility omitted from restricted wire hashes"
      );
      channel.overwrites->front().type = 0;
      changed = api::memberListIdentity("1", "1024", channel);
      require(
          changed->wireId == restricted->wireId && changed->signature != restricted->signature,
          "Role and member targets cannot share cached rows merely because their wire hashes coincide"
      );
      channel.overwrites = std::vector<api::PermissionOverwrite>{{"123", "0", "1024", 0}};
      require(
          api::memberListIdentity("1", "1024", channel)->wireId == "497997582",
          "Wire hashing handles exact four-byte blocks"
      );
      channel.overwrites = std::vector<api::PermissionOverwrite>{{"123", "1024", "0", 0}};
      require(
          api::memberListIdentity("1", "0", channel)->wireId == "3352883465",
          "Wire hashing handles the remaining final byte"
      );
      require(
          api::memberListIdentity("1", "1024", channel) == publicList,
          "Grant-only overwrites preserve the public everyone list"
      );
      channel.overwrites->push_back(channel.overwrites->front());
      require(!api::memberListIdentity("1", "1024", channel), "Duplicate overwrite targets reject shared visibility");
      channel.overwrites = std::vector<api::PermissionOverwrite>{{"123", "invalid", "0", 0}};
      require(!api::memberListIdentity("1", "1024", channel), "Invalid permission data cannot select a shared list");
      channel.overwrites = std::vector<api::PermissionOverwrite>(1001, {"123", "0", "1024", 0});
      require(
          !api::memberListIdentity("1", "1024", channel), "Member-list identity work is bounded by overwrite limits"
      );
      channel.overwrites.emplace();
      for (const auto type : {1, 3, 4, 10, 11, 12}) {
        channel.type = type;
        require(
            !api::memberListIdentity("1", "1024", channel),
            "DMs, categories and threads never borrow an ordinary guild member list"
        );
      }
      channel.type = 0;
      require(!api::memberListIdentity("1", "", channel), "Unknown default role permissions cannot establish sharing");
    }
    {
      const auto sync = [](const std::string& guild, const std::string& list, int first = 0) {
        return json{
            {"guild_id", guild},
            {"id", list},
            {"ops",
             json::array(
                 {{{"op", "SYNC"},
                   {"range", {first, first + 99}},
                   {"items", json::array({{{"member", {{"user_id", "7"}}}}})}}}
             )}
        };
      };
      const auto delta = [](const std::string& list) {
        return json{{"guild_id", "1"}, {"id", list}, {"ops", json::array({{{"op", "DELETE"}, {"index", 0}}})}};
      };
      api::MemberListStream stream;
      require(!stream.select("1", "20"), "The first member-list subscription starts without borrowed rows");
      require(
          !stream.accepts(delta("first"), 0), "An unknown list cannot establish identity from incremental operations"
      );
      require(
          !stream.accepts(sync("2", "first"), 0) && !stream.accepts(sync("1", "first", 300), 0),
          "Initial synchronization must match the guild and overlap the requested viewport"
      );
      auto empty = sync("1", "first");
      empty["ops"][0]["items"] = json::array();
      require(!stream.accepts(empty, 0), "An empty synchronization cannot adopt an unrelated list identity");
      require(stream.accepts(sync("1", "first"), 0), "An initial populated SYNC establishes the service list identity");
      require(
          stream.accepts(delta("first"), 0) && !stream.accepts(sync("1", "other"), 0),
          "Subsequent updates must retain the established service identity"
      );
      require(stream.select("1", "20"), "Viewport changes within an unknown channel retain its established list");
      require(!stream.select("1", "21"), "Unrelated unknown channels cannot borrow member rows");
      require(
          !stream.accepts(sync("1", "first"), 0), "A late SYNC from a retired channel cannot populate another channel"
      );
      require(stream.accepts(sync("1", "second"), 0), "The new channel can establish its own list identity");
      require(!stream.select("1", "20"), "Returning to a different unknown channel still resets its rows");
      require(
          !stream.accepts(delta("first"), 0)
              && !stream.accepts(sync("1", "second"), 0)
              && stream.accepts(sync("1", "first"), 0),
          "A returning channel may recover its own retired identity through SYNC, while rejecting another channel's "
          "list"
      );
      const api::MemberListIdentity known{"1/full-visibility", "everyone"};
      require(!stream.select("1", "22", known), "An authoritative visibility change resets member rows");
      require(
          !stream.accepts(sync("1", "first"), 0) && stream.accepts(delta("everyone"), 0),
          "Known visibility accepts only its exact wire identity"
      );
      require(stream.select("1", "23", known), "Equal full visibility signatures retain rows across channels");
      require(
          !stream.select("1", "24", api::MemberListIdentity{"1/different-visibility", "everyone"}),
          "Coincident wire hashes alone cannot preserve rows"
      );
      stream.clear();
      for (int at = 0; at != 10; ++at) {
        stream.select("1", std::to_string(100 + at));
        require(
            stream.accepts(sync("1", std::to_string(at)), 0),
            "Independent subscriptions establish bounded retired history"
        );
      }
      stream.select("1", "200");
      require(stream.accepts(sync("1", "0"), 0), "Retired identity tracking evicts old entries after eight lists");
      stream.clear();
      require(!stream.accepts(sync("1", "0"), 0), "Account reset removes the selected identity and all retired lists");
    }
    api::MemberList members;
    const json alice = {
        {"member",
         {{"user", {{"id", "1"}, {"username", "alice"}, {"global_name", "Global Alice"}}},
          {"nick", "Alice"},
          {"presence", {{"status", "online"}}}}}
    };
    const json bob = {{"member", {{"user_id", "2"}, {"presence", {{"status", "idle"}}}}}};
    members.apply(
        json{
            {"member_count", 100000},
            {"groups", json::array({{{"id", "9"}, {"count", 2}}})},
            {"ops",
             json::array(
                 {{{"op", "SYNC"},
                   {"range", json::array({0, 99})},
                   {"items", json::array({{{"group", {{"id", "9"}}}}, alice, bob})}}}
             )}
        },
        {{"9", "Team"}}, {{"2", {"2", "Bob", ""}}}
    );
    auto rows = members.rows();
    require(
        rows.size() == 3
            && rows[0].group == "Team"
            && rows[0].count == 2
            && rows[1].user.name == "Alice"
            && rows[2].user.name == "Bob",
        "Resolve role counts from the group snapshot, nicknames and deduplicated users"
    );
    require(
        rows[1].status == "online" && rows[2].status == "idle" && members.total == 3,
        "Preserve member presence and list indices"
    );
    {
      auto partial = members;
      partial.apply(
          json{
              {"ops",
               json::array(
                   {{{"op", "UPDATE"}, {"index", 1}, {"item", {{"member", {{"presence", {{"status", "dnd"}}}}}}}}}
               )}
          },
          {}
      );
      auto updated = partial.rows();
      require(
          updated.size() == 3
              && updated[1].index == 1
              && updated[1].user.id == "1"
              && updated[1].user.name == "Alice"
              && updated[1].status == "dnd"
              && updated[2].user.id == "2",
          "A presence-only member update preserves the visible identity and neighboring remote slots"
      );
      partial.apply(
          json{
              {"ops",
               json::array({{{"op", "UPDATE"}, {"index", 1}, {"item", {{"member", {{"nick", "Renamed Alice"}}}}}}})}
          },
          {}
      );
      updated = partial.rows();
      require(
          updated[1].user.id == "1" && updated[1].user.name == "Renamed Alice" && updated[1].status == "dnd",
          "Nickname-only updates retain the member identity and previous presence"
      );
      partial.apply(
          json{{"ops", json::array({{{"op", "UPDATE"}, {"index", 1}, {"item", {{"member", {{"nick", nullptr}}}}}}})}},
          {}
      );
      updated = partial.rows();
      require(
          updated[1].user.id == "1"
              && updated[1].user.name == "Global Alice"
              && updated[1].user.username == "alice"
              && updated[1].status == "dnd",
          "An explicit null nickname restores the global display name from the retained account profile"
      );
      partial.apply(
          json{
              {"ops",
               json::array(
                   {{{"op", "UPDATE"}, {"index", 1}, {"item", {{"member", {{"presence", {{"status", "idle"}}}}}}}}}
               )}
          },
          {}
      );
      require(
          partial.rows()[1].user.name == "Global Alice",
          "Omitting a nickname after a clear preserves the restored display name"
      );
      partial.apply(
          json{
              {"ops", json::array({{{"op", "UPDATE"}, {"index", 2}, {"item", {{"member", {{"nick", "Team Bob"}}}}}}})}
          },
          {}
      );
      partial.apply(
          json{{"ops", json::array({{{"op", "UPDATE"}, {"index", 2}, {"item", {{"member", {{"nick", nullptr}}}}}}})}},
          {}, {{"2", {"2", "Bob", ""}}}
      );
      require(
          partial.rows()[2].user.name == "Bob" && partial.rows()[2].status == "idle",
          "Clearing a nickname uses the account directory when retained profile name fields are unavailable"
      );
      partial.apply(
          json{{"ops", json::array({{{"op", "UPDATE"}, {"index", 1}, {"item", {{"member", {{"user_id", "2"}}}}}}})}},
          {}, {{"2", {"2", "Bob", ""}}}
      );
      updated = partial.rows();
      require(
          updated[1].user.id == "2" && updated[1].user.name == "Bob" && updated[1].status == "offline",
          "Replacing an indexed member with a different identity does not inherit the former profile or presence"
      );
    }
    {
      auto atomic = members;
      bool rejected = false;
      try {
        atomic.apply(
            json{
                {"ops",
                 json::array(
                     {{{"op", "SYNC"},
                       {"range", json::array({0, 99})},
                       {"items",
                        json::array(
                            {{{"group", {{"id", "9"}}}}, alice, {{"member", {{"user", {{"id", "invalid"}}}}}}}
                        )}}}
                 )}
            },
            {{"9", "Team"}}
        );
      } catch (const std::exception&) {
        rejected = true;
      }
      const auto retained = atomic.rows();
      require(
          rejected
              && retained.size() == 3
              && retained[0].group == "Team"
              && retained[1].user.id == "1"
              && retained[2].user.id == "2",
          "A rejected member SYNC preserves the previous complete range instead of exposing partial blank slots"
      );
    }
    for (const auto& operation : json::array(
             {{{"op", "INSERT"}, {"index", 1}, {"item", {{"member", {{"user", {{"id", "invalid"}}}}}}}},
              {{"op", "UPDATE"}, {"index", 1}, {"item", json::object()}},
              {{"op", "UPDATE"}, {"index", 1}, {"item", {{"unknown", json::object()}}}},
              {{"op", "UPDATE"}, {"index", 1}, {"item", {{"member", {{"user", json::object()}}}}}},
              {{"op", "UPDATE"}, {"index", 1}, {"item", {{"member", nullptr}}}},
              {{"op", "UPDATE"}, {"index", 1}, {"item", {{"group", nullptr}}}}}
         )) {
      auto retainedList = members;
      bool rejected = false;
      try {
        retainedList.apply(json{{"ops", json::array({operation})}}, {});
      } catch (const std::exception&) {
        rejected = true;
      }
      const auto retained = retainedList.rows();
      require(
          rejected
              && retainedList.total == 3
              && retained.size() == 3
              && retained[0].index == 0
              && retained[0].group == "Team"
              && retained[1].index == 1
              && retained[1].user.id == "1"
              && retained[1].user.name == "Alice"
              && retained[2].index == 2
              && retained[2].user.id == "2",
          "Malformed inserts and unsupported updates preserve visible member rows and their original indices"
      );
    }
    {
      auto shrinking = members;
      shrinking.apply(
          json{
              {"ops",
               json::array(
                   {{{"op", "SYNC"},
                     {"range", json::array({100, 102})},
                     {"items", json::array({{{"group", {{"id", "offline"}}}}, alice, bob})}}}
               )}
          },
          {}
      );
      require(
          shrinking.rows().size() == 6 && shrinking.rows()[3].index == 100,
          "Unloaded member pages retain their remote index gaps"
      );
      shrinking.apply(
          json{
              {"groups", json::array({{{"id", "9"}, {"count", 2}}})},
              {"ops",
               json::array(
                   {{{"op", "SYNC"},
                     {"range", json::array({0, 99})},
                     {"items", json::array({{{"group", {{"id", "9"}}}}, alice, bob})}}}
               )}
          },
          {{"9", "Team"}}, {{"2", {"2", "Bob", ""}}}
      );
      require(
          shrinking.total == 3 && shrinking.rows().size() == 3,
          "An authoritative smaller group snapshot retires stale cached rows beyond its visible list"
      );
    }
    members.apply(
        json{
            {"ops", json::array({{{"op", "DELETE"}, {"index", 1}}, {{"op", "INSERT"}, {"index", 2}, {"item", alice}}})}
        },
        {}
    );
    rows = members.rows();
    require(rows[1].user.name == "Bob" && rows[2].user.name == "Alice", "Apply member list insert/delete index shifts");
    members.apply(json{{"ops", json::array({{{"op", "INVALIDATE"}, {"range", json::array({1, 2})}}})}}, {});
    require(members.rows().size() == 1, "Invalidate stale subscribed ranges");
    members.apply(json{{"groups", json::array({{{"id", "9"}, {"count", 17}}})}}, {});
    require(
        members.rows()[0].count == 17 && members.total == 18, "Group-only updates refresh existing counts and scrolling"
    );
    members.apply(
        json{{"ops", json::array({{{"op", "UPDATE"}, {"index", 0}, {"item", {{"group", {{"id", "9"}}}}}}})}},
        {{"9", "Team"}}
    );
    require(members.rows()[0].count == 17, "Header updates without counts retain the latest group snapshot");
    members.apply(
        json{
            {"ops",
             json::array({{{"op", "UPDATE"}, {"index", 0}, {"item", {{"group", {{"id", "9"}, {"count", 19}}}}}}})}
        },
        {{"9", "Team"}}
    );
    require(members.rows()[0].count == 19, "An inline count in a later row-only update supersedes the cached count");
    members.apply(json{{"groups", json::array({{{"id", "9"}, {"count", 0}}})}}, {});
    require(members.rows()[0].count == 0, "An explicit empty group is different from an unknown count");
    members.apply(
        json{{"ops", json::array({{{"op", "UPDATE"}, {"index", 0}, {"item", {{"group", {{"id", "8"}}}}}}})}}, {}
    );
    require(members.rows()[0].count < 0, "Missing count data must not be displayed as zero");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
