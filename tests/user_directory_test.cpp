#include "api/user_directory.h"

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
    {
      api::UserDirectory gateway, view;
      const json activity{
          {"name", "Synthetic player"},     {"type", 3},
          {"details", "A private fixture"}, {"state", "Synthetic creator"},
          {"status_display_type", nullptr}, {"timestamps", {{"start", 1793534400000LL}, {"end", 1793534640000LL}}}
      };
      const auto dispatch = [&](const std::string& type, const json& data) { view.apply(gateway.consume(type, data)); };
      const auto member = [&](const std::string& id, const json& activities) {
        return json{
            {"member", {{"user", {{"id", id}}}, {"presence", {{"status", "online"}, {"activities", activities}}}}}
        };
      };
      dispatch(
          "GUILD_MEMBER_LIST_UPDATE",
          {{"guild_id", "10"},
           {"ops", json::array({{{"op", "SYNC"}, {"items", json::array({member("8", json::array({activity}))})}}})}}
      );
      require(
          view.activities("8").size() == 1 && view.activities("8").front().fields["details"] == "A private fixture",
          "Member-list snapshots hydrate non-friend rich activities through the owning-loop projection"
      );
      dispatch(
          "GUILD_MEMBER_LIST_UPDATE",
          {{"guild_id", "10"}, {"ops", json::array({{{"op", "UPDATE"}, {"item", member("8", json::array())}}})}}
      );
      require(view.activities("8").empty(), "Member-list updates clear ended activities");
      dispatch(
          "GUILD_MEMBERS_CHUNK",
          {{"guild_id", "10"},
           {"presences", json::array({{{"user_id", "8"}, {"activities", json::array({activity})}}})}}
      );
      require(view.activities("8").size() == 1, "Member chunk presences hydrate activity cards");
      dispatch("PRESENCE_UPDATE", {{"user_id", "8"}, {"activities", json::array()}});
      dispatch("PRESENCE_UPDATE", {{"user_id", "8"}, {"guild_id", "10"}, {"activities", json::array({activity})}});
      require(view.activities("8").empty(), "A global activity clear remains authoritative over guild fallbacks");
      dispatch("PRESENCE_UPDATE", {{"user_id", "8"}, {"activities", json::array({activity})}});
      dispatch("PRESENCE_UPDATE", {{"user_id", "8"}, {"status", "idle"}});
      require(view.activities("8").size() == 1, "Status-only patches retain activity fields");
      dispatch("READY", json::object());
      require(view.activities("8").empty(), "Account replacement clears all rich activity scopes");
    }
    {
      api::UserDirectory notifications;
      auto state = notifications.consume(
          "USER_GUILD_SETTINGS_UPDATE",
          json{
              {"channel_overrides",
               json::array(
                   {{{"channel_id", "10"},
                     {"message_notifications", 1},
                     {"muted", true},
                     {"mute_config", {{"end_time", "2035-01-01T00:00:00+00:00"}}}}}
               )}
          }
      );
      require(
          state.notifications.size() == 1
              && notifications.notificationLevel("10") == 1
              && notifications.muted("10")
              && notifications.nextMuteExpiry(),
          "Live notification preferences and mute expiry are retained"
      );
      const auto revision = notifications.notificationRevision("10");
      notifications.consume(
          "USER_GUILD_SETTINGS_UPDATE",
          json{{"channel_overrides", json::array({{{"channel_id", "10"}, {"message_notifications", 2}}})}}
      );
      require(
          notifications.notificationLevel("10") == 2
              && notifications.muted("10")
              && notifications.notificationRevision("10") > revision,
          "Partial notification changes retain mute state and invalidate older acknowledgements"
      );
      notifications.consume(
          "USER_GUILD_SETTINGS_UPDATE",
          json{
              {"channel_overrides",
               json::array({{{"channel_id", "10"}, {"mute_config", {{"end_time", "2020-01-01T00:00:00Z"}}}}})}
          }
      );
      require(
          !notifications.muted("10") && !notifications.nextMuteExpiry(),
          "Expired mutes stop filtering channels without a new Gateway event"
      );
      require(
          api::muteDeadline("2035-01-01T01:00:00+01:00") == api::muteDeadline("2035-01-01T00:00:00.000Z")
              && !api::muteDeadline("invalid"),
          "Mute deadlines accept UTC fractions and timezone offsets"
      );
    }
    {
      api::UserDirectory gateway, view;
      const auto receive = [&](const auto& type, const auto& data) { view.apply(gateway.consume(type, data)); };
      receive(
          "READY",
          json{
              {"merged_presences",
               {{"friends",
                 json::array(
                     {{{"user_id", "7"},
                       {"status", "online"},
                       {"activities",
                        json::array(
                            {{{"type", 4},
                              {"state", "Building native things"},
                              {"emoji", {{"name", "👀"}, {"id", nullptr}}}}}
                        )}}}
                 )}}}
          }
      );
      require(
          view.customStatus("7").text == "Building native things" && view.customStatus("7").emojiName == "👀",
          "READY custom status text and Unicode emoji reach the owning-loop view"
      );
      receive("PRESENCE_UPDATE", json{{"user", {{"id", "7"}}}, {"status", "idle"}});
      require(view.customStatus("7").text == "Building native things", "Absent activities retain the custom status");
      receive(
          "PRESENCE_UPDATE",
          json{
              {"user_id", "7"},
              {"activities",
               json::array(
                   {{{"type", 4},
                     {"state", "Updated"},
                     {"emoji", {{"name", "dance"}, {"id", "123"}, {"animated", true}}}}}
               )}
          }
      );
      require(
          view.customStatus("7").text == "Updated"
              && view.customStatus("7").emojiId == "123"
              && view.customStatus("7").animated,
          "Activity-only updates retain animated custom emoji identity"
      );
      receive(
          "PRESENCE_UPDATE",
          json{
              {"user_id", "7"},
              {"guild_id", "10"},
              {"status", "online"},
              {"activities", json::array({{{"type", 4}, {"state", "Guild fallback"}}})}
          }
      );
      require(view.customStatus("7").text == "Updated", "Global custom status wins over guild-scoped data");
      receive("PRESENCE_UPDATE", json{{"user_id", "7"}, {"activities", nullptr}});
      receive("PRESENCE_UPDATE", json{{"user_id", "7"}, {"activities", json::array({{{"type", 4}, {"state", 42}}})}});
      require(view.customStatus("7").text == "Updated", "Malformed activity lists and custom text preserve prior data");
      receive("PRESENCE_UPDATE", json{{"user_id", "7"}, {"activities", json::array()}});
      require(
          view.customStatus("7").empty(),
          "An explicit global empty list clears custom status without a guild fallback leak"
      );
      receive(
          "READY_SUPPLEMENTAL",
          json{
              {"guilds", json::array({{{"id", "10"}}})},
              {"merged_presences",
               {{"guilds",
                 json::array({json::array(
                     {{{"user_id", "8"},
                       {"status", "online"},
                       {"activities", json::array({{{"type", 4}, {"state", "Guild status"}}})}}}
                 )})}}}
          }
      );
      require(
          view.customStatus("8").text == "Guild status",
          "Guild statuses can initialize users without a global status snapshot"
      );
      receive("READY", json::object());
      require(
          view.customStatus("7").empty() && view.customStatus("8").empty(),
          "A new account/session clears custom status state"
      );
    }
    api::UserDirectory gateway, view;
    auto dispatch = [&](const auto& type, const auto& data) { view.apply(gateway.consume(type, data)); };
    dispatch(
        "READY",
        json{
            {"relationships",
             {{"entries", json::array({{{"id", "7"}, {"type", 1}, {"nickname", "My friend 🌿"}}})}, {"version", 1}}},
            {"guilds", json::array({{{"id", "10"}}})},
            {"merged_presences", {{"guilds", json::array({json::array({{{"user_id", "8"}, {"status", "online"}}})})}}}
        }
    );
    require(view.name("7", "Server name") == "My friend 🌿", "Personal aliases take priority over guild/global names");
    require(view.name("8", "Original") == "Original", "Unaliased users preserve their original name");
    require(
        view.status("8") == "online" && view.status("7") == "offline",
        "Merged guild presence and absent offline presence initialize DMs"
    );
    dispatch(
        "READY_SUPPLEMENTAL",
        json{{"merged_presences", {{"friends", json::array({{{"user_id", "7"}, {"status", "dnd"}}})}}}}
    );
    require(
        view.status("7") == "dnd" && view.name("7", "Original") == "My friend 🌿",
        "Supplemental presences merge without clearing aliases"
    );
    dispatch("PRESENCE_UPDATE", json{{"user", {{"id", "7"}}}, {"status", "idle"}});
    require(view.status("7") == "idle", "Live presence updates work with ID-only partial user objects");
    dispatch("PRESENCE_UPDATE", json{{"user_id", "7"}, {"guild_id", "10"}, {"status", "offline"}});
    require(view.status("7") == "idle", "Guild-specific presence must not override overall friend presence in DMs");
    dispatch("PRESENCE_UPDATE", json{{"user_id", "8"}, {"guild_id", "11"}, {"status", "offline"}});
    require(view.status("8") == "online", "One guild going offline must not hide another known online presence");
    dispatch("PRESENCE_UPDATE", json{{"user_id", "8"}, {"status", "offline"}});
    require(view.status("8") == "offline", "An explicit global offline update overrides guild fallbacks");
    dispatch("PRESENCE_UPDATE", json{{"user_id", "7"}, {"status", nullptr}});
    dispatch("RELATIONSHIP_UPDATE", json{{"id", "7"}, {"type", 1}});
    require(
        view.status("7") == "idle" && view.name("7", "Original") == "My friend 🌿",
        "Absent or malformed optional fields do not clear prior state"
    );
    dispatch("RELATIONSHIP_UPDATE", json{{"id", "7"}, {"nickname", nullptr}});
    require(
        view.name("7", "Server name") == "Server name", "Clearing an alias immediately restores the current fallback"
    );
    dispatch("RELATIONSHIP_ADD", json{{"id", "7"}, {"nickname", "Changed"}});
    require(view.name("7", "Original") == "Changed", "Relationship additions update existing conversations");
    dispatch("RELATIONSHIP_REMOVE", json{{"id", "7"}});
    require(view.name("7", "Original") == "Original", "Relationship removal drops aliases without losing presence");
    require(view.status("7") == "idle", "Nickname changes must preserve presence state");
    dispatch("RELATIONSHIP_UPDATE", json{{"id", "7"}, {"nickname", "<b>literal</b> **name**"}});
    require(
        view.name("7", "Original") == "<b>literal</b> **name**", "Display aliases remain raw data for the UI to escape"
    );
    const auto revision = view.relationshipRevision();
    dispatch("RELATIONSHIP_UPDATE", json{{"id", "7"}, {"user_ignored", true}});
    require(
        view.ignored("7") && view.relationshipRevision() > revision,
        "Ignored state arrives as a typed relationship update"
    );
    dispatch("RELATIONSHIP_UPDATE", json{{"id", "7"}, {"type", 1}});
    require(view.ignored("7"), "Unrelated relationship changes preserve ignored state");
    dispatch("RELATIONSHIP_UPDATE", json{{"id", "7"}, {"user_ignored", nullptr}});
    require(view.ignored("7"), "Malformed ignored state cannot clear a valid preference");
    dispatch("RELATIONSHIP_UPDATE", json{{"id", "7"}, {"user_ignored", false}});
    require(!view.ignored("7"), "Explicit false immediately clears ignored state");
    dispatch("RELATIONSHIP_ADD", json{{"id", "7"}, {"user_ignored", true}});
    dispatch("RELATIONSHIP_REMOVE", json{{"id", "7"}});
    require(
        !view.ignored("7") && view.status("7") == "idle",
        "Removing a relationship clears ignore without losing presence"
    );
    dispatch("RELATIONSHIP_UPDATE", json{{"id", "7"}, {"nickname", "Replacement"}, {"user_ignored", true}});
    api::UserUpdates replacement;
    replacement.relationshipsReset = true;
    replacement.users["8"].relationship = 1;
    replacement.users["8"].nickname = "Fresh friend";
    replacement.users["8"].ignored = true;
    view.apply(replacement);
    require(
        view.name("7", "Original") == "Original"
            && !view.ignored("7")
            && view.ignored("8")
            && view.name("8", "") == "Fresh friend"
            && view.status("7") == "idle",
        "Fresh REST relationships replace aliases and ignores without resetting unrelated presence"
    );
    dispatch("RESUMED", json::object());
    require(view.status("7") == "idle", "Resume retains state until changed by a live event");
    dispatch("READY", json{{"relationships", json::array()}});
    require(
        view.name("7", "Original") == "Original" && view.status("7") == "offline",
        "A fresh READY clears stale or previous-account state"
    );
    dispatch(
        "READY",
        json{
            {"relationships", json::array({{{"id", "9"}, {"nickname", "Legacy"}, {"user_ignored", true}}})},
            {"presences", json::array({{{"user", {{"id", "9"}}}, {"status", "online"}}})}
        }
    );
    require(
        view.name("9", "Original") == "Legacy" && view.status("9") == "online" && view.ignored("9"),
        "Support non-versioned relationships and legacy bootstrap presences"
    );
    dispatch("PRESENCE_UPDATE", json{{"user_id", "9"}, {"status", "invisible"}});
    require(view.status("9") == "offline", "Invisible remote presence is displayed as offline");
    dispatch(
        "READY",
        json{
            {"relationships", json::array({{{"id", "7"}, {"type", 2}, {"nickname", nullptr}}})},
            {"user_guild_settings",
             {{"entries",
               json::array(
                   {{{"guild_id", nullptr},
                     {"channel_overrides", json::array({{{"channel_id", "10"}, {"muted", true}}})}}}
               )}}}
        }
    );
    require(
        view.relationship("7") == 2 && view.muted("10"), "Import account block state and private conversation mutes"
    );
    dispatch(
        "USER_GUILD_SETTINGS_UPDATE",
        json{{"channel_overrides", json::array({{{"channel_id", "10"}, {"muted", false}}})}}
    );
    dispatch("RELATIONSHIP_REMOVE", json{{"id", "7"}});
    require(!view.muted("10") && view.relationship("7") == 0, "Live unblock and unmute updates clear menu state");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
