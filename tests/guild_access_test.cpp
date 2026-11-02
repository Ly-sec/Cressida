#include "api/actions.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

int main() {
  const auto require = [](bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  };
  using nlohmann::json;
  try {
    for (const auto input : {"abc-DEF_123", "https://discord.gg/abc-DEF_123", "discord.com/invite/abc-DEF_123?event=1"})
      require(api::serverInviteCode(input) == "abc-DEF_123", "Invite inputs normalize to a route-safe code");
    for (const auto input :
         {"", "https://evil.invalid/invite/code", "https://discord.gg@evil.invalid/code", "../code",
          "https://discord.gg/code/extra", "code?x=1", "https://discord.gg/%2f", "https://discord.gg/"})
      require(!api::serverInviteCode(input), "Untrusted URLs and path injection never become invite routes");
    json value{
        {"code", "abc"},
        {"type", 0},
        {"guild", {{"id", "42"}, {"name", "Moon Cafe"}, {"icon", nullptr}}},
        {"approximate_member_count", 21},
        {"approximate_presence_count", 13},
        {"channel", {{"id", "43"}}}
    };
    const auto invite = api::parseServerInvite(value, "abc");
    require(
        invite
            && invite->guild.id == "42"
            && invite->guild.members == 21
            && invite->guild.online == 13
            && invite->channel == "43",
        "Server invite metadata retains counts and its scoped destination"
    );
    require(!api::parseServerInvite(value, "other"), "An unrelated response cannot replace a selected invite");
    value["type"] = 1;
    require(!api::parseServerInvite(value, "abc"), "Group invites are not mistaken for server membership");
    require(
        !api::guildMembershipChange("GUILD_CREATE", {{"id", "42"}, {"lurker", true}})
            && !api::guildMembershipChange("GUILD_DELETE", {{"id", "42"}, {"unavailable", true}}),
        "Preview-only and temporary outage events do not change membership"
    );
    const auto added =
        api::guildMembershipChange("GUILD_CREATE", {{"id", "42"}, {"properties", {{"name", "Moon Cafe"}}}});
    require(
        added && added->guild && added->guild->name == "Moon Cafe", "A joined server can use nested live properties"
    );
    const auto removed = api::guildMembershipChange("GUILD_DELETE", {{"id", "42"}});
    require(removed && removed->removed == "42", "An actual departure removes its server");
    const auto preview = api::interactionRequest({.kind = api::InteractionKind::InvitePreview, .text = "abc"});
    require(
        preview && preview->method == "GET" && preview->path == "/invites/abc?with_counts=true&with_expiration=true",
        "Invite previews are explicit fixed-route reads"
    );
    const auto join =
        api::interactionRequest({.kind = api::InteractionKind::AcceptInvite, .text = "abc", .guild = "42"});
    require(
        join && join->method == "POST" && join->path == "/invites/abc", "An explicit join accepts the selected invite"
    );
    require(
        !api::interactionRequest({.kind = api::InteractionKind::AcceptInvite, .text = "abc"}),
        "Joining requires a validated preview destination"
    );
    const auto create = api::interactionRequest({.kind = api::InteractionKind::CreateGuild, .text = "Moon Cafe"});
    require(
        create && create->method == "POST" && create->body["name"] == "Moon Cafe",
        "Creation has a validated native request"
    );
    require(
        !api::interactionRequest({.kind = api::InteractionKind::CreateGuild, .text = " "}),
        "Blank names never send a creation request"
    );
    std::cout << "Server invite, membership and explicit creation checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
