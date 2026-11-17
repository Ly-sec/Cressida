#include "api/actions.h"
#include "api/user_directory.h"

#include <iostream>
#include <stdexcept>

namespace {
  constexpr std::uint64_t pinnedConversationFlag = 1ULL << 11;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
} // namespace
int main() {
  using namespace api;
  using Json = nlohmann::json;
  using Kind = InteractionKind;
  try {
    UserDirectory gateway, ui;
    const auto ready = Json{
        {"user_guild_settings",
         {{"entries",
           Json::array(
               {{{"guild_id", "10"},
                 {"muted", true},
                 {"message_notifications", 1},
                 {"suppress_everyone", true},
                 {"suppress_roles", true},
                 {"mobile_push", false},
                 {"hide_muted_channels", true},
                 {"notify_highlights", 2},
                 {"flags", guildOptInChannelsOn | 32},
                 {"mute_config", {{"end_time", "2035-01-01T00:00:00Z"}}},
                 {"channel_overrides", Json::array({{{"channel_id", "20"}, {"message_notifications", 0}}})}},
                {{"guild_id", nullptr}, {"muted", true}}}
           )}}}
    };
    auto update = gateway.consume("READY", ready);
    ui.apply(update);
    auto initial = ui.guildPreferences("10");
    require(
        update.guildNotifications.size() == 1
            && ui.muted("10")
            && ui.notificationLevel("10") == 1
            && initial.suppressEveryone == true
            && initial.suppressRoles == true
            && initial.mobilePush == false
            && initial.hideMutedChannels == true
            && initial.notifyHighlights == 2
            && initial.flags == (guildOptInChannelsOn | 32)
            && ui.notificationLevel("20") == 0,
        "READY projects personal server preferences and channel overrides independently"
    );
    require(ui.nextMuteExpiry().has_value(), "Timed server mutes participate in the shared native expiry wake");
    const auto before = ui.guildNotificationRevision("10");
    ui.apply(gateway.consume("USER_GUILD_SETTINGS_UPDATE", {{"guild_id", "10"}, {"message_notifications", 2}}));
    require(
        ui.guildNotificationRevision("10") > before
            && ui.notificationLevel("10") == 2
            && ui.muted("10")
            && ui.guildPreferences("10").suppressEveryone == true,
        "Partial server updates preserve unrelated preferences and invalidate older REST acknowledgements"
    );
    ui.apply(gateway.consume(
        "USER_GUILD_SETTINGS_UPDATE",
        {{"guild_id", "10"},
         {"suppress_everyone", false},
         {"suppress_roles", false},
         {"mute_config", {{"end_time", "2020-01-01T00:00:00Z"}}}}
    ));
    require(
        !ui.muted("10") && !ui.nextMuteExpiry() && ui.guildPreferences("10").suppressEveryone == false,
        "Expired server mutes and explicit false preferences take effect without polling"
    );
    ui.apply(
        gateway.consume("USER_GUILD_SETTINGS_UPDATE", {{"guild_id", "10"}, {"muted", true}, {"mute_config", nullptr}})
    );
    require(ui.muted("10") && !ui.nextMuteExpiry(), "Explicit null mute configuration restores indefinite mute");
    const auto valid = ui.guildPreferences("10");
    ui.apply(gateway.consume(
        "USER_GUILD_SETTINGS_UPDATE",
        {{"guild_id", "10"},
         {"muted", "false"},
         {"message_notifications", 9},
         {"flags", -1},
         {"mute_config", {{"end_time", "nonsense"}}}}
    ));
    require(ui.guildPreferences("10") == valid, "Malformed optional fields never clear the last valid server state");
    require(
        guildNotifications({{"guild_id", "../../10"}, {"muted", true}}).empty(),
        "Malformed guild identities cannot enter preference state"
    );

    Interaction notification{.kind = Kind::GuildNotifications, .guild = "10"};
    notification.guildNotification = GuildNotification{.muted = true, .suppressEveryone = false, .level = 1};
    notification.muteDuration = 900;
    auto request = interactionRequest(notification);
    require(
        request
            && request->path == "/users/@me/guilds/10/settings"
            && request->method == "PATCH"
            && request->body["muted"] == true
            && request->body["suppress_everyone"] == false
            && request->body["mute_config"]["selected_time_window"] == 900
            && muteDeadline(request->body["mute_config"]["end_time"].get<std::string>())
            && !request->body.contains("channel_overrides"),
        "Server mutes and notification preferences use the guild-level route with a finite timed deadline"
    );
    notification.muteDuration = 13;
    require(!interactionRequest(notification), "Unsupported mute durations are rejected before enqueueing");
    notification.muteDuration = -1;
    notification.guildNotification = GuildNotification{.muted = false};
    request = interactionRequest(notification);
    require(
        request && request->body == Json{{"muted", false}},
        "Unmuting sends only the mute flag without creating an indefinite mute configuration"
    );
    const std::pair<std::string, std::string> scopes[] = {{"10", ""}, {"10", "20"}, {"", "20"}};
    for (const auto& [guild, channel] : scopes) {
      Interaction unmute{
          .kind = channel.empty() ? Kind::GuildNotifications : Kind::Unmute, .channel = channel, .guild = guild
      };
      if (channel.empty())
        unmute.guildNotification = GuildNotification{.muted = false};
      const auto unmuteRequest = interactionRequest(unmute);
      require(unmuteRequest.has_value(), "Server, channel and private conversation unmute requests are valid");
      const Json scopeId = guild.empty() ? Json(nullptr) : Json(guild);
      Json oldTarget{{"muted", true}, {"mute_config", {{"end_time", "2035-01-01T00:00:00Z"}}}};
      Json savedTarget{{"muted", false}, {"mute_config", nullptr}, {"message_notifications", 2}};
      if (!channel.empty()) {
        oldTarget["channel_id"] = channel;
        oldTarget["flags"] = pinnedConversationFlag;
        savedTarget["channel_id"] = channel;
      }
      auto settings = [&](const Json& target) {
        Json value{{"guild_id", scopeId}};
        if (channel.empty())
          value.update(target);
        else
          value["channel_overrides"] = Json::array({target, {{"channel_id", "30"}, {"muted", true}}});
        return value;
      };
      UserDirectory before;
      before.consume("READY", {{"user_guild_settings", Json::array({settings(oldTarget)})}});
      const auto target = channel.empty() ? guild : channel;
      require(before.muted(target), "Reload fixtures begin with an active timed mute");
      auto saved = settings(savedTarget);
      auto result = notificationSettingsResult(unmuteRequest->body, saved, guild, channel);
      require(result.has_value(), "The returned target explicitly confirms the unmute");
      require(
          channel.empty() ? result->guildNotifications.size() == 1
                          : result->notifications.size() == 1 && result->notifications.contains(channel),
          "A settings response projects only the edited target"
      );
      before.apply(*result);
      require(
          !before.muted(target) && before.notificationLevel(target) == 2 && !before.nextMuteExpiry(),
          "Confirmed settings use the returned level and clear the old mute deadline"
      );
      if (!channel.empty())
        require(before.pinnedDm(channel), "Unmuting preserves pin flags omitted from the response");
      UserDirectory reloaded;
      reloaded.consume("READY", {{"user_guild_settings", Json::array({saved})}});
      require(!reloaded.muted(target), "Unmute stays consistent with the saved settings after a fresh READY");

      for (const int duration : {-1, 900}) {
        auto mute = unmute;
        mute.muteDuration = duration;
        if (channel.empty())
          mute.guildNotification->muted = true;
        else
          mute.kind = Kind::Mute;
        const auto muteRequest = interactionRequest(mute);
        auto muteTarget = oldTarget;
        muteTarget["mute_config"] = {{"end_time", "invalid"}};
        auto muted = notificationSettingsResult(muteRequest->body, settings(muteTarget), guild, channel);
        require(muted.has_value(), "An explicit matching mute acknowledgement is accepted");
        UserDirectory mutedView;
        mutedView.apply(*muted);
        require(
            mutedView.muted(target) && mutedView.nextMuteExpiry().has_value() == (duration > 0),
            "Timed and indefinite mute defaults survive malformed optional response metadata"
        );
      }

      for (const Json& invalidMute : {Json(true), Json("false"), Json(nullptr)}) {
        auto rejectedTarget = savedTarget;
        rejectedTarget["muted"] = invalidMute;
        require(
            !notificationSettingsResult(unmuteRequest->body, settings(rejectedTarget), guild, channel),
            "Contradictory or malformed mute acknowledgements cannot report unmute success"
        );
      }
      auto omittedMute = savedTarget;
      omittedMute.erase("muted");
      require(
          !notificationSettingsResult(unmuteRequest->body, settings(omittedMute), guild, channel)
              && !notificationSettingsResult(unmuteRequest->body, Json::object(), guild, channel)
              && !notificationSettingsResult(unmuteRequest->body, Json::array(), guild, channel),
          "Missing mute confirmations and malformed response envelopes cannot fabricate success"
      );
      auto wrongScope = saved;
      wrongScope["guild_id"] = "99";
      require(
          !notificationSettingsResult(unmuteRequest->body, wrongScope, guild, channel),
          "Settings responses must match the requested server or private conversation scope"
      );
      if (!channel.empty()) {
        auto wrongChannel = saved;
        wrongChannel["channel_overrides"][0]["channel_id"] = "99";
        auto duplicate = saved;
        duplicate["channel_overrides"].push_back(savedTarget);
        require(
            !notificationSettingsResult(unmuteRequest->body, wrongChannel, guild, channel)
                && !notificationSettingsResult(unmuteRequest->body, duplicate, guild, channel),
            "Missing and ambiguous target channel responses are rejected"
        );
      }
    }
    notification.guildNotification = GuildNotification{.flags = guildOptInChannelsOff};
    require(!interactionRequest(notification), "Whole stale flag words cannot be written accidentally");
    notification.notificationFlagsMask = guildOptInChannelsOn | guildOptInChannelsOff;
    require(interactionRequest(notification).has_value(), "Visibility edits declare the exact bits they change");
    notification.guildNotification->flags = guildOptInChannelsOff | 32;
    require(!interactionRequest(notification), "Visibility edits cannot alter unrelated unread preference flags");
    notification.notificationFlagsMask = 32;
    notification.guildNotification->flags = 32;
    require(!interactionRequest(notification), "Only supported personal server flag edits are accepted");

    Interaction reads{.kind = Kind::MarkGuildRead, .guild = "10"};
    for (int i = 1; i <= 500; ++i)
      reads.readStates.emplace_back(std::to_string(i), std::to_string(i + 1000));
    reads.readStates.emplace_back("1", "9999");
    request = interactionRequest(reads);
    require(
        request
            && request->path == "/read-states/ack-bulk"
            && request->method == "POST"
            && request->body["read_states"].size() == 500
            && request->body["read_states"][0]["message_id"] == "9999"
            && request->body["read_states"][0]["read_state_type"] == 0,
        "One validated server read action includes over 32 channels, deduplicating their newest cursor"
    );
    reads.readStates.emplace_back("../channel", "123");
    require(!interactionRequest(reads), "A malformed bulk read target rejects the entire action");
    reads.readStates.assign(1001, {"1", "2"});
    require(!interactionRequest(reads), "Bulk read inputs have a hard bounded size");
    require(
        !interactionRequest({.kind = Kind::MarkGuildRead, .guild = "10"}),
        "An empty read action makes no network request"
    );
    request = interactionRequest({.kind = Kind::LeaveGuild, .guild = "10"});
    require(
        request
            && request->path == "/users/@me/guilds/10"
            && request->method == "DELETE"
            && request->path != "/guilds/10",
        "Leaving a server is a personal membership removal, never server deletion"
    );
    require(
        !interactionRequest({.kind = Kind::LeaveGuild, .guild = "../10"}),
        "Leaving rejects malformed server identifiers"
    );
    request = interactionRequest({.kind = Kind::GuildPrivacy, .guild = "10"});
    require(
        request && request->method == "GET" && request->path == "/users/@me/settings-proto/1",
        "Opening per-server privacy reads account settings without mutating them"
    );
    Interaction privacy{.kind = Kind::GuildPrivacy, .guild = "10"};
    privacy.guildPrivacy = GuildPrivacy{.directMessages = false};
    require(interactionRequest(privacy)->method == "PATCH", "Explicit privacy changes use a versioned update");
    require(
        !interactionRequest({.kind = Kind::MoveGuild, .text = "../folder", .guild = "10"}),
        "Folder moves validate identifiers before the worker reads account settings"
    );
    ui.apply(gateway.consume("READY", Json::object()));
    require(
        !ui.muted("10") && ui.guildPreferences("10").empty() && ui.guildNotificationRevision("10") == 0,
        "New sessions release all personal server preferences"
    );
    std::cout << "server preferences passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
