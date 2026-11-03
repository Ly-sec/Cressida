#include "api/actions.h"
#include "api/channel_features.h"
#include "api/history.h"
#include "app/message_history.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

using namespace api;
using nlohmann::json;
namespace {
  void require(bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  }
  json msg(const char* id, const char* channel) {
    return {
        {"id", id},
        {"channel_id", channel},
        {"author", {{"id", "9"}, {"username", "Support"}, {"bot", true}}},
        {"content", "Help"}
    };
  }
} // namespace
int main() {
  try {
    {
      Channel channel{.id = "10", .name = "read-only", .type = 0};
      const auto allowed = viewChannel | sendMessages;
      require(
          messageWriteAccess(channel, allowed) == MessageWriteAccess::Allowed,
          "Ordinary channels require known view and send permissions"
      );
      require(
          messageWriteAccess(channel, viewChannel) == MessageWriteAccess::Denied,
          "Read-only channels reject message writes"
      );
      require(
          messageWriteAccess(channel, sendMessages) == MessageWriteAccess::Hidden,
          "Send permission cannot grant access to hidden channels"
      );
      require(
          messageWriteAccess(channel, {}) == MessageWriteAccess::Unknown,
          "Missing permission metadata cannot grant write access"
      );
      channel.type = static_cast<int>(ChannelType::GuildVoice);
      require(
          messageWriteAccess(channel, viewChannel) == MessageWriteAccess::Denied,
          "Voice-channel conversations enforce ordinary message permission"
      );
      channel.type = static_cast<int>(ChannelType::PublicThread);
      channel.archived = false;
      require(
          messageWriteAccess(channel, allowed) == MessageWriteAccess::Denied,
          "Parent Send Messages does not grant thread sending"
      );
      require(
          messageWriteAccess(channel, viewChannel | sendMessagesInThreads) == MessageWriteAccess::Allowed,
          "Thread sends do not require parent Send Messages"
      );
      channel.locked = true;
      require(
          messageWriteAccess(channel, viewChannel | sendMessagesInThreads) == MessageWriteAccess::Allowed,
          "Locking an active thread only restricts reopening"
      );
      channel.archived = true;
      require(
          messageWriteAccess(channel, viewChannel | sendMessagesInThreads) == MessageWriteAccess::Locked,
          "Locked archived threads cannot be reopened by ordinary members"
      );
      require(
          messageWriteAccess(channel, viewChannel | sendMessagesInThreads | manageThreads)
              == MessageWriteAccess::Archived,
          "Managers must explicitly reopen an archived thread before sending"
      );
      channel.type = static_cast<int>(ChannelType::PrivateThread);
      channel.archived = false;
      channel.joined = false;
      require(
          messageWriteAccess(channel, viewChannel | sendMessagesInThreads) == MessageWriteAccess::NotJoined,
          "Private thread sending requires membership"
      );
      channel.joined.reset();
      require(
          messageWriteAccess(channel, viewChannel | sendMessagesInThreads) == MessageWriteAccess::Unknown,
          "Unknown private membership cannot grant write access"
      );
      require(
          messageWriteAccess(channel, viewChannel | sendMessagesInThreads | manageThreads)
              == MessageWriteAccess::Allowed,
          "Thread managers can access private threads without joining"
      );
      channel.joined = true;
      require(
          messageWriteAccess(channel, viewChannel | sendMessagesInThreads) == MessageWriteAccess::Allowed,
          "Joined private threads accept the thread-specific send permission"
      );
      channel.type = static_cast<int>(ChannelType::GuildForum);
      require(
          messageWriteAccess(channel, ~std::uint64_t{0}) == MessageWriteAccess::Unsupported,
          "Forum containers cannot receive ordinary chat messages"
      );
      for (const auto kind : {ChannelType::DirectMessage, ChannelType::GroupDirectMessage}) {
        channel.type = static_cast<int>(kind);
        require(
            messageWriteAccess(channel, {}) == MessageWriteAccess::Allowed,
            "Private conversations do not depend on guild permission metadata"
        );
      }
    }
    {
      Interaction create{.kind = InteractionKind::CreateChannel, .text = "new-channel", .guild = "1", .parentId = "20"};
      auto request = interactionRequest(create);
      require(
          request
              && request->path == "/guilds/1/channels"
              && request->method == "POST"
              && request->body == json{{"name", "new-channel"}, {"type", 0}, {"parent_id", "20"}},
          "Channel creation uses an explicit validated parent and never mutates drafts"
      );
      create.channelType = 4;
      create.parentId.reset();
      require(interactionRequest(create)->body["type"] == 4, "Categories use the channel creation route");
      create.text = "";
      require(!interactionRequest(create), "Empty channel names are rejected");
      auto move =
          interactionRequest({.kind = InteractionKind::MoveChannel, .channel = "10", .guild = "1", .parentId = ""});
      require(
          move && move->method == "PATCH" && move->body == json{{"parent_id", nullptr}},
          "Moving out of a category preserves all permission overwrites"
      );
      require(
          !interactionRequest(
              {.kind = InteractionKind::MoveChannel, .channel = "10", .guild = "1", .parentId = "../bad"}
          ),
          "Move targets cannot escape fixed routes"
      );
      auto invite = interactionRequest({.kind = InteractionKind::CreateInvite, .channel = "10", .guild = "1"});
      require(
          invite
              && invite->path == "/channels/10/invites"
              && invite->body == json{{"max_age", 604800}, {"max_uses", 0}, {"temporary", false}, {"unique", false}},
          "The default invite lasts seven days with unlimited uses"
      );
      Interaction inviteOptions{.kind = InteractionKind::CreateInvite, .channel = "10", .guild = "1"};
      inviteOptions.inviteMaxAge = 0;
      inviteOptions.inviteMaxUses = 5;
      inviteOptions.inviteTemporary = true;
      require(
          interactionRequest(inviteOptions)->body
              == json{{"max_age", 0}, {"max_uses", 5}, {"temporary", true}, {"unique", false}},
          "Invite settings support permanent links, limited uses and temporary membership"
      );
      inviteOptions.inviteUnique = true;
      require(
          interactionRequest(inviteOptions)->body
              == json{{"max_age", 0}, {"max_uses", 5}, {"temporary", true}, {"unique", true}},
          "Explicitly generating a new link requests a fresh invite instead of reusing an existing match"
      );
      for (const auto age : {-1, 604801}) {
        inviteOptions.inviteMaxAge = age;
        require(!interactionRequest(inviteOptions), "Invite expiry must stay within Discord's bounds");
      }
      inviteOptions.inviteMaxAge = 604800;
      for (const auto uses : {-1, 101}) {
        inviteOptions.inviteMaxUses = uses;
        require(!interactionRequest(inviteOptions), "Invite uses must stay within Discord's bounds");
      }
      const auto share = interactionRequest(
          {.kind = InteractionKind::SendInvite, .channel = "10", .text = "https://discord.gg/test_code-1"}
      );
      require(
          share
              && share->path == "/channels/10/messages"
              && share->method == "POST"
              && share->body == messageBody("https://discord.gg/test_code-1"),
          "Sharing an invite sends the chosen URL through the normal safe message body"
      );
      const auto friendShare =
          interactionRequest({.kind = InteractionKind::SendInvite, .user = "9", .text = "https://discord.gg/test"});
      require(
          friendShare && friendShare->path == "/users/@me/channels" && friendShare->body == json{{"recipient_id", "9"}},
          "Sharing with a friend first prepares a DM for that exact recipient"
      );
      for (const auto* url :
           {"https://example.com/invite", "https://discord.gg/", "https://discord.gg/test?x=1",
            "https://discord.gg/test\n"})
        require(
            !interactionRequest({.kind = InteractionKind::SendInvite, .channel = "10", .text = url}),
            "Invite sharing rejects arbitrary URLs and malformed invite codes"
        );
      auto remove = interactionRequest({.kind = InteractionKind::DeleteChannel, .channel = "10", .guild = "1"});
      require(
          remove && remove->method == "DELETE" && remove->path == "/channels/10",
          "Deletion targets exactly the chosen channel"
      );
      for (int level = 0; level < 4; ++level) {
        auto notify = interactionRequest(
            {.kind = InteractionKind::ChannelNotifications, .channel = "10", .guild = "1", .notificationLevel = level}
        );
        require(
            notify && notify->body == json{{"channel_overrides", {{"10", {{"message_notifications", level}}}}}},
            "Notification updates touch only the chosen field and channel"
        );
      }
      require(
          !interactionRequest(
              {.kind = InteractionKind::ChannelNotifications, .channel = "10", .guild = "1", .notificationLevel = 4}
          ),
          "Unknown notification levels are rejected"
      );
      const auto mute =
          interactionRequest({.kind = InteractionKind::Mute, .channel = "10", .guild = "1", .muteDuration = 900});
      require(
          mute
              && mute->body["channel_overrides"]["10"]["mute_config"]["selected_time_window"] == 900
              && mute->body["channel_overrides"]["10"]["mute_config"]["end_time"].is_string(),
          "Timed mutes include their expiry and selected duration"
      );
      require(
          !interactionRequest({.kind = InteractionKind::Mute, .channel = "10", .guild = "1", .muteDuration = 42}),
          "Unknown mute durations are rejected"
      );
      const auto clone = duplicateChannelBody(
          json{
              {"id", "10"},
              {"guild_id", "1"},
              {"type", 15},
              {"name", "old"},
              {"permission_overwrites",
               json::array({{{"id", "1"}, {"type", 0}, {"allow", "18446744073709552640"}, {"deny", "0"}}})},
              {"available_tags", json::array({{{"id", "99"}, {"name", "Help"}, {"moderated", true}}})},
              {"topic", "Keep me"},
              {"last_message_id", "999"}
          },
          {.kind = InteractionKind::DuplicateChannel, .channel = "10", .text = "copy", .guild = "1"}
      );
      require(
          clone["name"] == "copy"
              && clone["topic"] == "Keep me"
              && !clone.contains("id")
              && !clone.contains("last_message_id")
              && !clone["available_tags"][0].contains("id")
              && clone["permission_overwrites"][0]["allow"] == "18446744073709552640",
          "Duplication preserves fresh channel settings and unknown permission bits, without copying identities or "
          "messages"
      );
      for (const auto type : {0, 2, 4, 15}) {
        create.text = "new-channel";
        create.channelType = type;
        require(
            interactionRequest(create) && createChannelBody(create)["type"] == type,
            "Text, voice, category and forum creation all use their actual API type"
        );
      }
      create.channelType = 7;
      require(!interactionRequest(create), "Unimplemented creation types are rejected");
      create.channelType = 4;
      create.parentId = "20";
      require(!interactionRequest(create), "Categories cannot be nested inside another category");
      create.channelType = 0;
      create.privateChannel = true;
      require(!interactionRequest(create), "Private creation requires the current account identity");
      create.user = "9";
      create.channelAccess = {{.id = "50", .type = 0}, {.id = "8", .type = 1}};
      const json parent{
          {"id", "20"},
          {"guild_id", "1"},
          {"type", 4},
          {"permission_overwrites",
           json::array(
               {{{"id", "1"}, {"type", 0}, {"allow", "18446744073709552640"}, {"deny", "8192"}},
                {{"id", "50"}, {"type", 0}, {"allow", "2048"}, {"deny", "1024"}},
                {{"id", "51"}, {"type", 0}, {"allow", "3072"}, {"deny", "0"}},
                {{"id", "9"}, {"type", 1}, {"allow", "32768"}, {"deny", "18446744073709552640"}},
                {{"id", "8"}, {"type", 1}, {"allow", "0"}, {"deny", "1024"}}}
           )}
      };
      const auto privateBody = createChannelBody(create, parent);
      require(
          privateBody["parent_id"] == "20"
              && privateBody["permission_overwrites"]
                  == json::array(
                      {{{"id", "1"}, {"type", 0}, {"allow", "18446744073709551616"}, {"deny", "9216"}},
                       {{"id", "50"}, {"type", 0}, {"allow", "3072"}, {"deny", "0"}},
                       {{"id", "51"}, {"type", 0}, {"allow", "2048"}, {"deny", "0"}},
                       {{"id", "9"}, {"type", 1}, {"allow", "33792"}, {"deny", "18446744073709551616"}},
                       {{"id", "8"}, {"type", 1}, {"allow", "1024"}, {"deny", "0"}}}
                  ),
          "Private creation preserves inherited high and unrelated permission bits while restricting View Channel "
          "to the creator and explicitly selected roles/members"
      );
      bool missingParentRejected = false;
      try {
        (void)createChannelBody(create);
      } catch (const std::exception&) {
        missingParentRejected = true;
      }
      require(missingParentRejected, "Private category children cannot be created from stale or missing permissions");
      auto wrongParent = parent;
      wrongParent["guild_id"] = "2";
      bool wrongGuildRejected = false;
      try {
        (void)createChannelBody(create, wrongParent);
      } catch (const std::exception&) {
        wrongGuildRejected = true;
      }
      require(wrongGuildRejected, "The fresh parent snapshot must belong to the chosen server");
      create.channelAccess.push_back({.id = "1", .type = 0});
      require(!interactionRequest(create), "Private access selection cannot grant View Channel to everyone");
      create.channelAccess.clear();
      create.parentId.reset();
      require(
          createChannelBody(create)["permission_overwrites"]
              == json::array(
                  {{{"id", "1"}, {"type", 0}, {"allow", "0"}, {"deny", "1024"}},
                   {{"id", "9"}, {"type", 1}, {"allow", "1024"}, {"deny", "0"}}}
              ),
          "Private root channels get explicit everyone deny and creator access"
      );
      Interaction syncMove{.kind = InteractionKind::MoveChannel, .channel = "10", .guild = "1", .parentId = "20"};
      syncMove.syncPermissions = true;
      require(
          moveChannelBody(parent, syncMove)
              == json{{"parent_id", "20"}, {"permission_overwrites", parent["permission_overwrites"]}},
          "An explicitly synchronized move copies the fresh destination category permissions only"
      );
      syncMove.parentId = "";
      require(!interactionRequest(syncMove), "Moving out of categories cannot request permission synchronization");
      auto voiceSource = json{
          {"id", "10"},
          {"guild_id", "1"},
          {"type", 2},
          {"permission_overwrites", json::array()},
          {"bitrate", 128000},
          {"user_limit", 5},
          {"rtc_region", nullptr},
          {"video_quality_mode", 2},
          {"topic", "not a voice field"},
          {"available_tags", json::array()},
          {"flags", (1 << 21) | (1 << 17) | 2}
      };
      auto voiceCopy = duplicateChannelBody(
          voiceSource, {.kind = InteractionKind::DuplicateChannel, .channel = "10", .text = "copy", .guild = "1"}
      );
      require(
          voiceCopy["type"] == 2
              && voiceCopy["bitrate"] == 128000
              && voiceCopy["user_limit"] == 5
              && voiceCopy["rtc_region"].is_null()
              && voiceCopy["video_quality_mode"] == 2
              && voiceCopy["flags"] == (1 << 21)
              && !voiceCopy.contains("topic")
              && !voiceCopy.contains("available_tags"),
          "Voice duplication retains voice settings and strips unsupported fields and read-only flags"
      );
      voiceSource["type"] = 15;
      voiceSource["flags"] = (1 << 4) | (1 << 15) | (1 << 17) | 2;
      auto forumCopy = duplicateChannelBody(
          voiceSource, {.kind = InteractionKind::DuplicateChannel, .channel = "10", .text = "copy", .guild = "1"}
      );
      require(
          forumCopy["flags"] == (1 << 4) && !forumCopy.contains("bitrate") && !forumCopy.contains("user_limit"),
          "Forum duplication retains only writable forum flags and fields"
      );
      Interaction editPermission{.kind = InteractionKind::EditChannelPermission, .channel = "20", .user = "9"};
      editPermission.overwriteType = 1;
      editPermission.permissionBit = 1024;
      for (const auto value : {-1, 0, 1}) {
        editPermission.permissionValue = value;
        const auto edited = channelPermissionBody(parent, editPermission);
        require(
            edited["allow"] == (value == 1 ? "33792" : "32768")
                && edited["deny"] == (value == -1 ? "18446744073709552640" : "18446744073709551616"),
            "Tri-state permission changes preserve every unrelated current overwrite bit"
        );
      }
      for (unsigned value = 0; value < 4096; ++value) {
        auto current = parent;
        current["permission_overwrites"][3]["allow"] = std::to_string(value);
        current["permission_overwrites"][3]["deny"] = std::to_string(value);
        editPermission.permissionBit = 16;
        for (const auto setting : {-1, 0, 1}) {
          editPermission.permissionValue = setting;
          const auto edited = channelPermissionBody(current, editPermission);
          require(
              edited["allow"] == std::to_string(setting == 1 ? value | 16U : value & ~16U)
                  && edited["deny"] == std::to_string(setting == -1 ? value | 16U : value & ~16U),
              "Permission editing handles decimal carries and borrows for each tri-state setting"
          );
        }
      }
      auto current = parent;
      current["permission_overwrites"][3]["allow"] = "18446744073709551616";
      current["permission_overwrites"][3]["deny"] = "27670116110564327424";
      editPermission.permissionBit = 1ULL << 63;
      editPermission.permissionValue = 1;
      require(
          channelPermissionBody(current, editPermission)
              == json{{"type", 1}, {"allow", "27670116110564327424"}, {"deny", "18446744073709551616"}},
          "Editing the highest known permission bit retains arbitrary unknown higher bits"
      );
      editPermission.permissionBit = 3;
      require(!interactionRequest(editPermission), "Permission edits cannot modify more than one bit at once");
      Interaction channelSettings{.kind = InteractionKind::EditChannel, .channel = "10"};
      channelSettings.channelNsfw = false;
      channelSettings.channelRateLimit = 21600;
      require(
          interactionRequest(channelSettings)->body == json{{"nsfw", false}, {"rate_limit_per_user", 21600}},
          "Channel settings support explicit NSFW clearing and slowmode independently from name/topic"
      );
      channelSettings.channelRateLimit = 21601;
      require(!interactionRequest(channelSettings), "Slowmode rejects values above six hours");
      Channel parsedSettings;
      parseChannelDetails(parsedSettings, {{"nsfw", true}, {"rate_limit_per_user", 21600}});
      require(
          parsedSettings.nsfw == true && parsedSettings.rateLimitPerUser == 21600,
          "Channel details retain age restriction and the current slowmode setting"
      );
      parseChannelDetails(parsedSettings, {{"topic", "unrelated update"}});
      require(
          parsedSettings.nsfw == true && parsedSettings.rateLimitPerUser == 21600,
          "Partial channel updates preserve absent age restriction and slowmode fields"
      );
      parseChannelDetails(parsedSettings, {{"nsfw", false}, {"rate_limit_per_user", 0}});
      require(
          parsedSettings.nsfw == false && parsedSettings.rateLimitPerUser == 0,
          "Explicit false and zero clear age restriction and slowmode"
      );
      parseChannelDetails(parsedSettings, {{"nsfw", "invalid"}, {"rate_limit_per_user", -1}});
      parseChannelDetails(parsedSettings, {{"rate_limit_per_user", 21601}});
      require(
          parsedSettings.nsfw == false && parsedSettings.rateLimitPerUser == 0,
          "Malformed or out-of-range channel settings do not replace validated state"
      );
    }
    require(
        permissionBits("18446744073709551616") == 0
            && permissionBits("18446744073709552640") == viewChannel
            && !permissionBits("1024x"),
        "Large permission integers retain their known low bits without truncating raw snapshots"
    );
    require(
        withViewPermission("18446744073709551616", true) == "18446744073709552640"
            && withViewPermission("18446744073709552640", false) == "18446744073709551616",
        "Editing View Channel preserves unknown high permission bits"
    );
    for (unsigned value = 0; value < 4096; ++value) {
      const auto raw = std::to_string(value);
      require(
          withViewPermission(raw, true) == std::to_string(value | 1024)
              && withViewPermission(raw, false) == std::to_string(value & ~1024U),
          "Decimal permission editing must handle carries and borrows"
      );
    }
    Channel channel;
    channel.id = "10";
    parseChannelDetails(
        channel,
        {{"topic", "**Need help?**"},
         {"permission_overwrites",
          json::array(
              {{{"id", "1"}, {"type", 0}, {"allow", "0"}, {"deny", "1024"}},
               {{"id", "2"}, {"type", 0}, {"allow", "1024"}, {"deny", "0"}},
               {{"id", "3"}, {"type", 0}, {"allow", "0"}, {"deny", "1024"}},
               {{"id", "7"}, {"type", 1}, {"allow", "0"}, {"deny", "1024"}}}
          )}}
    );
    require(
        channel.topic == "**Need help?**" && channel.overwrites->size() == 4,
        "Channel topics and permission overrides are projected"
    );
    parseChannelDetails(channel, {{"name", "partial"}});
    require(
        channel.topic == "**Need help?**" && channel.overwrites->size() == 4,
        "Partial channel fields retain prior metadata"
    );
    std::vector<ColoredRole> roles{
        {"1", 0, {}, "@everyone", {}, {}, "1024"},
        {"2", 1, {}, "Members", {}, {}, "0"},
        {"3", 2, {}, "Restricted", {}, {}, "0"},
        {"4", 3, {}, "Admin", {}, {}, "8"}
    };
    require(
        (channelPermissions("1", "8", "", roles, std::vector<std::string>{"2", "3"}, channel).value_or(0) & viewChannel)
            != 0,
        "Role overwrite allows aggregate after role denies"
    );
    require(
        (channelPermissions("1", "7", "", roles, std::vector<std::string>{"2"}, channel).value_or(0) & viewChannel)
            == 0,
        "Member overrides apply after role overrides"
    );
    require(
        channelPermissions("1", "7", "", roles, std::vector<std::string>{"4"}, channel) == ~std::uint64_t{0}
            && channelPermissions("1", "7", "7", {}, {}, channel) == ~std::uint64_t{0},
        "Administrator and owner permissions bypass channel overrides"
    );
    require(
        !channelPermissions("1", "7", "", roles, {}, channel),
        "Unknown member roles cannot invent management permissions"
    );
    const auto access = channelAccessRoles("1", roles, channel);
    require(
        access.size() == 2 && access[0].id == "4" && access[1].id == "2",
        "Private-channel access roles include allowed roles and administrators"
    );
    Channel malformed;
    parseChannelDetails(
        malformed,
        {{"permission_overwrites", json::array({{{"id", "2"}, {"type", 0}, {"allow", "bad"}, {"deny", "0"}}})}}
    );
    require(!malformed.overwrites, "Malformed permissions are unknown, never an empty public overwrite list");
    parseChannelDetails(channel, {{"topic", nullptr}, {"permission_overwrites", json::array()}});
    require(
        channel.topic == "" && channel.overwrites->empty(), "Explicit null and empty channel metadata can clear fields"
    );
    RoleDirectory directory;
    directory.apply(parseRoleUpdates(
        "READY",
        {{"user", {{"id", "7"}}},
         {"guilds",
          json::array(
              {{{"id", "1"},
                {"properties", {{"owner_id", "7"}}},
                {"roles", json::array({{{"id", "1"}, {"permissions", "1024"}}, {{"id", "2"}, {"permissions", "16"}}})},
                {"member", {{"roles", json::array({"2"})}}}}}
          )}}
    ));
    require(
        directory.owner("1") == "7"
            && directory.memberships("1", "7")->at(0) == "2"
            && directory.allRoles("1").size() == 2,
        "READY supplies owner, own memberships and full role permissions"
    );
    auto raw = msg("100", "10");
    raw["components"] = json::array(
        {{{"type", 1},
          {"components",
           json::array(
               {{{"type", 2},
                 {"style", 2},
                 {"label", "Get help"},
                 {"custom_id", "support:open"},
                 {"emoji", {{"name", "🌙"}}}},
                {{"type", 2}, {"style", 5}, {"label", "Docs"}, {"url", "https://example.com"}},
                {{"type", 2}, {"style", 4}, {"label", "Disabled"}, {"custom_id", "disabled"}, {"disabled", true}},
                {{"type", 2}, {"style", 5}, {"label", "Invalid"}, {"url", "javascript:alert(1)"}}}
           )}}}
    );
    raw["flags"] = 64;
    auto message = parseMessages(json::array({raw})).front();
    require(
        message.buttons.size() == 1
            && message.buttons[0].size() == 3
            && message.buttons[0][0].emojiName == "🌙"
            && message.buttons[0][2].disabled
            && message.applicationId == "9"
            && message.flags == 64,
        "Messages retain real component actions, URLs, disabled state, bot identity and ephemeral flags"
    );
    parseMessagePatch({{"content", "Edited"}}).apply(message);
    require(
        message.buttons[0][0].customId == "support:open" && message.applicationId == "9",
        "Partial edits retain buttons and application identity"
    );
    parseMessagePatch({{"components", json::array()}}).apply(message);
    require(message.buttons.empty(), "Explicit empty components clear message buttons");
    raw["content"] = "";
    raw.erase("embeds");
    require(
        parseMessages(json::array({raw})).front().content.empty(),
        "Button-only messages must not gain fake system-message text"
    );
    Interaction button{
        .kind = InteractionKind::MessageButton,
        .channel = "10",
        .message = "100",
        .guild = "1",
        .application = "9",
        .customId = "support:open",
        .nonce = "123",
        .messageFlags = 64
    };
    auto request = interactionRequest(button);
    require(
        request
            && request->path == "/interactions"
            && request->method == "POST"
            && request->body["type"] == 3
            && request->body["data"] == json{{"component_type", 2}, {"custom_id", "support:open"}}
            && !request->body.contains("session_id"),
        "Button requests contain validated public fields; private session identity stays on the REST worker"
    );
    button.application = "../bad";
    require(!interactionRequest(button), "Component actions validate application identity");
    const auto formJson = json::parse(R"JSON({
      "id":"600","channel_id":"10","application":{"id":"9"},"custom_id":"support:details","title":"Support details",
      "components":[
        {"type":1,"id":1,"components":[{"type":4,"id":2,"custom_id":"system","label":"System","style":1,"min_length":2,"max_length":20}]},
        {"type":18,"id":3,"label":"Details","description":"What happened?","component":{"type":4,"id":4,"custom_id":"details","style":2,"required":false}}
      ]})JSON");
    const auto modal = parseApplicationModal(formJson);
    require(
        modal.fields.size() == 2 && modal.fields[1].multiline && !modal.fields[1].required,
        "Native forms project both legacy action rows and current labels without exposing response credentials"
    );
    const std::vector<std::string> values{"Linux", "Line one\nLine two 🌙"};
    const auto submitted = modalSubmission(modal, values);
    require(
        submitted["id"] == "600"
            && submitted["custom_id"] == "support:details"
            && submitted["components"][0]["components"][0]
                == json{{"type", 4}, {"id", 2}, {"custom_id", "system"}, {"value", "Linux"}}
            && submitted["components"][1]["component"]["value"] == values[1],
        "Form submission preserves server IDs, wrapper types, custom identifiers and multiline Unicode values"
    );
    bool rejected = false;
    try {
      (void)modalSubmission(modal, std::vector<std::string>{"", ""});
    } catch (const std::exception&) {
      rejected = true;
    }
    require(rejected, "Required fields validate before sending");
    auto invalidForm = formJson;
    invalidForm["components"][1]["component"]["type"] = 19;
    rejected = false;
    try {
      (void)parseApplicationModal(invalidForm);
    } catch (const std::exception&) {
      rejected = true;
    }
    require(rejected, "Unsupported form controls cannot be silently dropped from a submission");
    auto modalRequest = interactionRequest(
        {.kind = InteractionKind::SubmitModal,
         .channel = "10",
         .guild = "1",
         .application = "9",
         .nonce = "777",
         .modal = modal,
         .values = values}
    );
    require(
        modalRequest
            && modalRequest->path == "/interactions"
            && modalRequest->body["type"] == 5
            && modalRequest->body["data"] == submitted
            && !modalRequest->body.contains("session_id"),
        "Explicit form submissions use validated type-5 interactions; session identity stays backend-owned"
    );
    auto edit =
        interactionRequest({.kind = InteractionKind::EditChannel, .channel = "10", .channelTopic = "New topic"});
    require(
        edit && edit->body == json{{"topic", "New topic"}} && edit->method == "PATCH",
        "Channel edits send only explicitly changed fields"
    );
    require(
        !interactionRequest({.kind = InteractionKind::EditChannel, .channel = "10", .channelName = ""})
            && !interactionRequest({.kind = InteractionKind::GrantChannelAccess, .channel = "10", .user = "../bad"}),
        "Channel mutation routes and names are validated"
    );
    auto grant = interactionRequest({.kind = InteractionKind::GrantChannelAccess, .channel = "10", .user = "2"});
    require(
        grant && grant->path == "/channels/10/permissions/2" && grant->body == json{{"type", 0}},
        "Granting access uses a fixed route and defers merging fresh overwrite bits to the worker"
    );
    ChannelPageQuery search{ChannelPage::Search, "1", "10", "help & 🌙"};
    require(
        channelPagePath(search) == "/guilds/1/messages/search?content=help%20%26%20%F0%9F%8C%99&offset=0",
        "Server search safely encodes content"
    );
    require(
        channelPagePath({ChannelPage::Search, {}, "10", "help"})
            == "/channels/10/messages/search?content=help&offset=0",
        "DM search uses the channel route"
    );
    ChannelPageQuery pins{ChannelPage::Pins, "1", "10", {}, "2026-10-17T09:00:00+00:00"};
    require(
        channelPagePath(pins) == "/channels/10/messages/pins?limit=50&before=2026-10-17T09%3A00%3A00%2B00%3A00",
        "Pinned messages use Discord's current paginated endpoint"
    );
    auto hit = msg("100", "10");
    hit["hit"] = true;
    auto context = msg("99", "10");
    context["hit"] = false;
    auto page =
        parseChannelPage({{"messages", json::array({json::array({context, hit})})}, {"total_results", 80}}, search);
    require(
        page.messages.size() == 1 && page.messages[0].id == "100" && page.more && page.total == 80,
        "Search results project hits instead of duplicate context messages"
    );
    auto pinPage = parseChannelPage(
        {{"items", json::array({{{"pinned_at", "2026-10-16T09:00:00Z"}, {"message", msg("100", "10")}}})},
         {"has_more", true}},
        pins
    );
    require(
        pinPage.messages.size() == 1 && pinPage.more && pinPage.before == "2026-10-16T09:00:00Z",
        "Pin pages project messages and advancing timestamps"
    );
    require(
        messageHistoryPath("10", {HistoryDirection::Around, "100", 1}).ends_with("&around=100"),
        "Message navigation loads native history around the selected result"
    );
    app::MessageHistory history;
    const auto failedAround = history.beginAround("100");
    history.fail(*failedAround, "Temporary failure");
    const auto retryAround = history.begin(HistoryDirection::Around, {}, true);
    require(
        retryAround && retryAround->cursor == "100" && retryAround->request != failedAround->request,
        "Retrying a result jump preserves its exact message cursor and uses a fresh request identity"
    );
    const auto around = history.beginAround("100");
    std::vector<Message> messages{{"1000", "10", "Old cache", "old"}};
    history.apply(
        *around, messages, parseMessages(json::array({msg("99", "10"), msg("100", "10"), msg("101", "10")})), {}
    );
    require(
        messages.size() == 3 && messages[1].id == "100" && !history.atLatest() && !history.olderEnd(),
        "Around-history replaces disconnected cached tails and defers read acknowledgement until reaching the present"
    );
    std::cout << "Channel metadata, permissions, components, routes, search, pins and message navigation passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
