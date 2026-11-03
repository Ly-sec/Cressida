#include "api/models.h"
#include "api/role_styles.h"
#include "app/role_motion.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

int main() {
  using nlohmann::json;
  auto require = [](bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  try {
    api::RoleDirectory directory;
    auto solid = json{{"id", "50"}, {"name", "Nightwatch"}, {"position", 1}, {"color", 0xFF8844}};
    auto gradient = json{
        {"id", "60"},
        {"position", 2},
        {"color", 0xFF0000},
        {"colors", {{"primary_color", 0xF05090}, {"secondary_color", 0x50CCFF}, {"tertiary_color", nullptr}}}
    };
    auto holo = json{
        {"id", "70"},
        {"position", 3},
        {"colors", {{"primary_color", 11127295}, {"secondary_color", 16759788}, {"tertiary_color", 16761760}}}
    };
    auto noColor = json{{"id", "80"}, {"position", 4}, {"colors", {{"primary_color", 0}}}};
    auto ready = json{
        {"guilds",
         json::array(
             {{{"id", "1"},
               {"features", json::array({"ENHANCED_ROLE_COLORS"})},
               {"roles", json::array({solid, gradient, holo, noColor})}},
              {{"id", "2"}, {"roles", json::array({{{"id", "90"}, {"position", 1}, {"color", 0x00FF00}}})}}}
         )},
        {"merged_members",
         json::array(
             {json::array(
                  {{{"user_id", "11"}, {"roles", json::array({"50", "60", "80"})}},
                   {{"user", {{"id", "12"}}}, {"roles", json::array({"70"})}}}
              ),
              json::array({{{"user_id", "11"}, {"roles", json::array({"90"})}}})}
         )}
    };
    directory.apply(api::parseRoleUpdates("READY", ready));
    require(directory.roleName("1", "50") == "Nightwatch", "Mention names resolve directly from READY roles");
    require(
        directory.roleName("2", "50").empty()
            && directory.roleName("", "50").empty()
            && directory.roleName("1", "999").empty(),
        "Unknown roles and other server contexts never borrow a role name"
    );
    require(
        directory.colors("1", "11") == api::RoleColors{0xF05090, 0x50CCFF, {}},
        "Highest colored role wins over higher colorless roles; new colors override legacy color"
    );
    require(
        directory.colors("2", "11")->primary == 0x00FF00 && !directory.colors("", "11"),
        "Role colors are guild scoped and cannot leak into DMs"
    );
    require(directory.colors("1", "12")->tertiary == 16761760, "Holographic palette retains all three server colors");
    directory.apply(
        api::parseRoleUpdates(
            "GUILD_MEMBER_UPDATE", json{{"guild_id", "1"}, {"user", {{"id", "11"}}}, {"roles", json::array({"50"})}}
        )
    );
    require(
        directory.colors("1", "11")->primary == 0xFF8844,
        "Live membership changes recolor existing messages without fetching history"
    );
    directory.apply(
        api::parseRoleUpdates(
            "GUILD_MEMBER_UPDATE", json{{"guild_id", "1"}, {"user", {{"id", "11"}}}, {"nick", "Changed"}}
        )
    );
    require(directory.colors("1", "11")->primary == 0xFF8844, "Partial member updates preserve roles");
    directory.apply(
        api::parseRoleUpdates(
            "GUILD_ROLE_UPDATE",
            json{
                {"guild_id", "1"}, {"role", {{"id", "50"}, {"name", "Moonwatch"}, {"position", 1}, {"color", 0x2255AA}}}
            }
        )
    );
    require(directory.colors("1", "11")->primary == 0x2255AA, "Live role edits replace colors");
    require(directory.roleName("1", "50") == "Moonwatch", "Live role edits refresh mention names");
    directory.apply(
        api::parseRoleUpdates(
            "GUILD_MEMBER_UPDATE",
            json{{"guild_id", "1"}, {"user_id", "13"}, {"roles", json::array({"50", "60", "999"})}}
        )
    );
    directory.apply(api::parseRoleUpdates("GUILD_ROLE_DELETE", json{{"guild_id", "1"}, {"role_id", "50"}}));
    require(!directory.colors("1", "11"), "Deleting the only colored role restores default color");
    require(directory.roleName("1", "50").empty(), "Deleted roles cannot retain stale mention names");
    const auto emptyMembership = directory.memberships("1", "11");
    require(
        emptyMembership && emptyMembership->empty(), "Deleting a member's last role retains known empty membership"
    );
    require(
        directory.memberships("1", "13") == std::vector<std::string>{"60", "999"}
            && directory.memberships("2", "11") == std::vector<std::string>{"90"},
        "Role deletion removes only the explicit ID in its own server and preserves unrelated unknown roles"
    );
    directory.apply(api::parseRoleUpdates("GUILD_ROLE_DELETE", json{{"guild_id", "1"}, {"role_id", "999"}}));
    require(
        directory.memberships("1", "13") == std::vector<std::string>{"60"},
        "An explicit deletion also clears an unknown role from cached memberships"
    );
    directory.apply(
        api::parseRoleUpdates(
            "GUILD_MEMBER_UPDATE", json{{"guild_id", "1"}, {"user_id", "12"}, {"roles", json::array()}}
        )
    );
    require(
        !directory.colors("1", "12", std::vector<std::string>{"70"}),
        "Live empty roles override stale message membership"
    );
    directory.apply(
        api::parseRoleUpdates(
            "MESSAGE_CREATE",
            json{{"guild_id", "1"}, {"author", {{"id", "12"}}}, {"member", {{"roles", json::array({"70"})}}}}
        )
    );
    require(directory.colors("1", "12")->tertiary.has_value(), "Live message membership refreshes the directory");
    directory.apply(api::parseRoleUpdates("GUILD_UPDATE", json{{"id", "1"}, {"features", json::array()}}));
    require(
        !directory.colors("1", "12")->secondary && directory.colors("1", "12")->primary == 11127295,
        "Losing enhanced styles falls back to primary color"
    );
    directory.apply(
        api::parseRoleUpdates("GUILD_UPDATE", json{{"id", "1"}, {"features", json::array({"ENHANCED_ROLE_COLORS"})}})
    );
    require(directory.colors("1", "12")->tertiary.has_value(), "Restored perk enables the saved palette");
    directory.apply(
        api::parseRoleUpdates(
            "GUILD_MEMBER_LIST_UPDATE",
            json{
                {"guild_id", "1"},
                {"ops",
                 json::array(
                     {{{"op", "SYNC"},
                       {"items", json::array({{{"member", {{"user_id", "13"}, {"roles", json::array({"60"})}}}}})}}}
                 )}
            }
        )
    );
    require(directory.colors("1", "13")->secondary == 0x50CCFF, "Lazy member-list data resolves colors");
    const auto messages = api::parseMessages(
        json::array(
            {{{"id", "100"},
              {"channel_id", "10"},
              {"author", {{"id", "14"}, {"username", "member"}}},
              {"member", {{"roles", json::array({"60"})}}},
              {"content", "hello"}}}
        )
    );
    require(
        directory.colors("1", "14", messages.front().memberRoles)->primary == 0xF05090,
        "REST message roles work without loading the member list first"
    );
    auto message = messages.front();
    api::parseMessagePatch(json{{"content", "updated"}}).apply(message);
    require(message.memberRoles == messages.front().memberRoles, "Missing roles preserve message membership on edits");
    api::parseMessagePatch(json{{"member", {{"roles", json::array()}}}}).apply(message);
    require(message.memberRoles && message.memberRoles->empty(), "Explicit empty role lists clear message membership");
    require(!api::parseMemberRoles(json{{"roles", json::array({123})}}), "Malformed role IDs are not coerced");
    directory.apply(
        api::parseRoleUpdates(
            "GUILD_ROLE_CREATE",
            json{
                {"guild_id", "1"},
                {"role",
                 {{"id", "61"}, {"position", 2}, {"colors", {{"primary_color", 0x112233}, {"secondary_color", 0}}}}}
            }
        )
    );
    require(
        directory.colors("1", "15", std::vector<std::string>{"61"})->secondary == 0, "Black is a valid secondary color"
    );
    require(
        directory.colors("1", "15", std::vector<std::string>{"60", "61"})->primary == 0xF05090,
        "Smaller role ID wins equal-position ties"
    );
    const auto history = api::parseMessages(
        json::array(
            {{{"id", "101"},
              {"channel_id", "10"},
              {"author", {{"id", "16"}, {"username", "uncached author"}}},
              {"content", "Earlier message"}}}
        )
    );
    require(
        !history.front().memberRoles && !directory.colors("1", "16"), "History without member data starts unresolved"
    );
    directory.apply(
        api::parseRoleUpdates(
            "GUILD_MEMBERS_CHUNK",
            json{
                {"guild_id", "1"},
                {"members", json::array({{{"user", {{"id", "16"}}}, {"roles", json::array({"60"})}}})}
            }
        )
    );
    require(
        directory.colors("1", "16", history.front().memberRoles)->secondary == 0x50CCFF,
        "Targeted member replies recolor existing history without a member-list page or profile"
    );
    directory.apply(api::parseRoleUpdates("GUILD_DELETE", json{{"id", "1"}, {"unavailable", true}}));
    require(directory.colors("1", "13").has_value(), "Temporary guild unavailability preserves colors");
    directory.apply(api::parseRoleUpdates("GUILD_DELETE", json{{"id", "1"}}));
    require(!directory.colors("1", "13"), "Departed guild clears role state");
    directory.apply(api::parseRoleUpdates("RESUMED", json::object()));
    require(directory.colors("2", "11").has_value(), "RESUME retains account state");
    directory.apply(api::parseRoleUpdates("READY", json::object()));
    require(!directory.colors("2", "11"), "Fresh READY clears the previous account");

    app::RoleMotion motion;
    const auto start = app::RoleMotion::Clock::time_point{};
    motion.begin();
    require(
        motion.phase("name", false, start) == 0 && !motion.active(), "Idle gradient is static and schedules no frames"
    );
    require(motion.phase("name", true, start) == 0, "Hover starts at the static gradient");
    motion.end();
    motion.begin();
    require(
        std::abs(motion.phase("name", true, start + std::chrono::seconds(1)) - 0.25F) < .001F,
        "Phase survives scene rebuilds"
    );
    require(motion.phase("other", false, start) == 0, "Unhovered names remain static independently");
    motion.end();
    motion.begin();
    require(motion.phase("name", true, start + std::chrono::seconds(4)) == 0, "Gradient loop is continuous");
    motion.end();
    motion.begin();
    motion.end();
    require(!motion.active(), "Offscreen names stop requesting frames");
    motion.begin();
    motion.phase("name", true, start);
    require(
        motion.phase("name", false, start + std::chrono::seconds(1)) == 0 && !motion.active(),
        "Hover leave, disabled motion and overlays restore static rendering"
    );
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
