#include "api/role_styles.h"
#include "app/client_application.h"
#include "app/settings_color_picker.h"
#include "media/animation.h"
#include "render/core/image_decoder.h"
#include "render/core/image_encoder.h"
#include "render/core/texture_manager.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/scrollbar.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using Json = nlohmann::json;
  using Section = api::GuildSettingsSection;
  using Verb = api::GuildSettingsVerb;
  void ClientApplication::prepareServerSettingsDemo() {
    m_account.id = "42";
    m_account.name = "Lysec";
    m_account.username = "lysec";
    m_connected = true;
    m_guildId = "100";
    m_guilds = {{"100", "Noctalia", "", {"COMMUNITY", "GUILD_TAGS", "BANNER", "ROLE_ICONS", "VANITY_URL"}, 1800, 274}};
    api::Channel general{"101", "general", 0};
    general.overwrites.emplace();
    api::Channel rules{"102", "rules", 0};
    rules.overwrites.emplace();
    api::Channel moderators{"103", "moderators", 0};
    moderators.overwrites.emplace();
    api::Channel voice{"104", "Lounge", 2};
    voice.overwrites.emplace();
    m_channels = {general, rules, moderators, voice};
    m_guildChannels[m_guildId] = m_channels;
    m_channelId = "101";
    const Json roles = Json::array(
        {{{"id", "100"},
          {"name", "@everyone"},
          {"position", 0},
          {"permissions", "68608"},
          {"color", 0},
          {"hoist", false},
          {"mentionable", false}},
         {{"id", "50"},
          {"name", "Moderator"},
          {"position", 3},
          {"permissions", "1099781378062"},
          {"color", 10930279},
          {"hoist", true},
          {"mentionable", false}},
         {{"id", "51"},
          {"name", "Owlkeeper"},
          {"position", 1},
          {"permissions", "68608"},
          {"color", 5879719},
          {"hoist", true},
          {"mentionable", false}},
         {{"id", "52"},
          {"name", "Integration"},
          {"position", 2},
          {"permissions", "0"},
          {"managed", true},
          {"color", 0}}}
    );
    m_roleDirectory.apply(
        api::parseRoleUpdates(
            "GUILD_CREATE",
            {{"id", "100"},
             {"owner_id", "42"},
             {"roles", roles},
             {"members",
              Json::array(
                  {{{"user", {{"id", "42"}}}, {"roles", Json::array({"50"})}},
                   {{"user", {{"id", "43"}}}, {"roles", Json::array({"51"})}}}
              )}}
        )
    );
    openServerSettings("100");
    auto& state = m_serverSettings;
    state.pending.clear();
    state.loading = state.saving = false;
    state.overview = {
        {"id", "100"},
        {"name", "Noctalia"},
        {"owner_id", "42"},
        {"description", "A place to talk about your desktop and the things you build."},
        {"features", m_guilds.front().features},
        {"verification_level", 1},
        {"explicit_content_filter", 2},
        {"default_message_notifications", 1},
        {"system_channel_id", "101"},
        {"system_channel_flags", 0},
        {"afk_channel_id", "104"},
        {"afk_timeout", 300},
        {"rules_channel_id", "102"},
        {"public_updates_channel_id", "103"},
        {"safety_alerts_channel_id", "103"},
        {"preferred_locale", "en-US"},
        {"premium_progress_bar_enabled", true},
        {"mfa_level", 0},
        {"premium_tier", 2},
        {"premium_subscription_count", 9}
    };
    state.lists["profile"] = {
        {"id", "100"},
        {"name", "Noctalia"},
        {"description", "A place to talk about your desktop and the things you build."},
        {"brand_color_primary", "#A8C86B"},
        {"visibility", 1},
        {"tag", "NOCT"},
        {"badge", 1},
        {"badge_color_primary", "#A8C86B"},
        {"badge_color_secondary", "#668A94"},
        {"game_application_ids", Json::array({"200"})},
        {"traits",
         Json::array(
             {{{"label", "Linux"},
               {"position", 0},
               {"emoji_id", nullptr},
               {"emoji_name", "🐧"},
               {"emoji_animated", false}},
              {{"label", "Creative"},
               {"position", 1},
               {"emoji_id", nullptr},
               {"emoji_name", "✨"},
               {"emoji_animated", false}}}
         )}
    };
    state.gameNames["200"] = "Minecraft";
    state.lists["roles"] = roles;
    state.lists["members"] = Json::array(
        {{{"user", {{"id", "43"}, {"username", "barRulus"}, {"global_name", "barrulus"}}},
          {"nick", nullptr},
          {"roles", Json::array({"51"})},
          {"mute", false},
          {"deaf", false},
          {"communication_disabled_until", nullptr}},
         {{"user", {{"id", "44"}, {"username", "aditya"}, {"global_name", "Aditya"}}},
          {"roles", Json::array()},
          {"mute", false},
          {"deaf", false}}}
    );
    state.lists["invites"] = Json::array(
        {{{"code", "noctalia-demo"},
          {"uses", 12},
          {"max_uses", 0},
          {"expires_at", nullptr},
          {"temporary", false},
          {"inviter", {{"id", "42"}, {"username", "lysec"}}}}}
    );
    const Json uploader = {{"id", "42"}, {"username", "lysec"}, {"global_name", "Lysec"}};
    const Json otherUploader = {{"id", "43"}, {"username", "barrulus"}, {"global_name", "barrulus"}};
    state.lists["emoji"] = Json::array(
        {{{"id", "301"},
          {"name", "cozy"},
          {"animated", false},
          {"roles", Json::array()},
          {"user", uploader},
          {"demo_emoji", "🙂"}},
         {{"id", "311"},
          {"name", "cheer"},
          {"animated", false},
          {"roles", Json::array({"51"})},
          {"user", otherUploader},
          {"demo_emoji", "🥳"}},
         {{"id", "312"},
          {"name", "wave"},
          {"animated", true},
          {"roles", Json::array()},
          {"user", uploader},
          {"demo_emoji", "👋"}}}
    );
    state.lists["stickers"] = Json::array(
        {{{"id", "302"},
          {"name", "Wave"},
          {"description", "A friendly hello"},
          {"tags", "👋"},
          {"format_type", 1},
          {"user", uploader},
          {"demo_emoji", "👋"}},
         {{"id", "313"},
          {"name", "Happy dance"},
          {"description", "A little celebration"},
          {"tags", "🥳"},
          {"format_type", 2},
          {"user", otherUploader},
          {"demo_emoji", "🥳"}},
         {{"id", "314"},
          {"name", "Cozy"},
          {"description", "A warm welcome"},
          {"tags", "🙂"},
          {"format_type", 4},
          {"user", uploader},
          {"demo_emoji", "🙂"}}}
    );
    state.lists["soundboard"] = Json::array(
        {{{"sound_id", "303"}, {"name", "Ta-da"}, {"volume", .75}, {"emoji_name", "🎉"}, {"user", uploader}},
         {{"sound_id", "315"}, {"name", "Hello there"}, {"volume", .42}, {"emoji_name", "👋"}, {"user", otherUploader}}}
    );
    // Offline assets exercise the real retained-image and animation paths.
    for (const auto section : {Section::Emoji, Section::Stickers}) {
      const auto page = section == Section::Emoji ? "emoji" : "stickers";
      for (const auto& record : state.lists[page]) {
        auto animation = std::make_shared<media::Animation>();
        animation->width = animation->height = 64;
        const auto moving = record.value("animated", false) || record.value("format_type", 1) != 1;
        for (int frame = 0; frame < (moving ? 2 : 1); ++frame) {
          media::Frame pixels;
          pixels.duration = std::chrono::milliseconds(240);
          pixels.rgba.resize(64 * 64 * 4);
          for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
              const auto at = (y * 64 + x) * 4;
              const auto dx = x - Style::space2Xl, dy = y - Style::space2Xl;
              const bool circle = dx * dx + dy * dy < 850;
              const bool eyes = y >= 21 && y <= 26 && ((x >= 19 && x <= 24) || (x >= 40 && x <= 45));
              const bool mouth = y >= 39 + frame * 3 && y <= 44 + frame * 3 && x >= 23 && x <= 41;
              pixels.rgba[at] = eyes || mouth ? 42 : 245;
              pixels.rgba[at + 1] = eyes || mouth ? 38 : 190 + frame * 25;
              pixels.rgba[at + 2] = eyes || mouth ? 48 : 80;
              pixels.rgba[at + 3] = circle ? 255 : 0;
            }
          animation->frames.push_back(std::move(pixels));
          animation->duration += std::chrono::milliseconds(240);
        }
        auto& cache = m_images[api::guildExpressionAsset(section, record)];
        cache.texture = m_window.context().textureManager().loadFromRgba(animation->frames[0].rgba.data(), 64, 64);
        if (moving)
          cache.animation = std::move(animation);
      }
    }
    state.lists["webhooks"] = Json::array({{{"id", "304"}, {"name", "Project updates"}, {"channel_id", "101"}}});
    state.lists["integrations"] = Json::array(
        {{{"id", "305"},
          {"name", "Streaming"},
          {"type", "twitch"},
          {"account", {{"id", "500"}, {"name", "Noctalia"}}},
          {"expire_behavior", 0},
          {"expire_grace_period", 7},
          {"enable_emoticons", true}}}
    );
    state.lists["automod"] = Json::array(
        {{{"id", "306"},
          {"name", "Block invite spam"},
          {"enabled", true},
          {"event_type", 1},
          {"trigger_type", 1},
          {"trigger_metadata", {{"keyword_filter", Json::array({"discord.gg/*"})}}},
          {"actions", Json::array({{{"type", 1}}})},
          {"exempt_roles", Json::array({"50"})},
          {"exempt_channels", Json::array({"103"})}}}
    );
    state.lists["audit"] = Json::array(
        {{{"id", "307"},
          {"user_id", "42"},
          {"target_id", "101"},
          {"action_type", 11},
          {"reason", "Clarify the channel topic"},
          {"changes",
           Json::array({{{"key", "topic"}, {"old_value", "General chat"}, {"new_value", "Talk about your desktop"}}})}}}
    );
    state.lists["bans"] = Json::array(
        {{{"user", {{"id", "45"}, {"username", "spam_account"}}}, {"reason", "Repeated spam after warnings"}}}
    );
    state.lists["templates"] = Json::array(
        {{{"code", "noctalia-template"},
          {"name", "Community starter"},
          {"description", "A small set of channels to get started"},
          {"is_dirty", true},
          {"usage_count", 12},
          {"updated_at", "2026-10-17T12:00:00Z"},
          {"serialized_source_guild",
           {{"channels", Json::array({{{"name", "general"}}, {{"name", "rules"}}})}, {"roles", roles}}}}}
    );
    state.lists["events"] = Json::array(
        {{{"id", "308"},
          {"name", "Show your desktop"},
          {"description", "Share what you've been working on."},
          {"entity_type", 3},
          {"privacy_level", 2},
          {"scheduled_start_time", "2035-10-18T18:00:00Z"},
          {"scheduled_end_time", "2035-10-18T20:00:00Z"},
          {"entity_metadata", {{"location", "Community livestream"}}},
          {"status", 1}}}
    );
    state.lists["widget"] = {{"enabled", true}, {"channel_id", "101"}};
    state.lists["vanity"] = {{"code", "noctalia"}, {"uses", 193}};
    state.lists["welcome"] = {
        {"enabled", true},
        {"description", "Welcome to Noctalia! Make yourself at home."},
        {"welcome_channels",
         Json::array(
             {{{"channel_id", "102"},
               {"description", "Read our community rules"},
               {"emoji_id", nullptr},
               {"emoji_name", "📖"}}}
         )}
    };
    state.lists["onboarding"] = {
        {"enabled", true},
        {"mode", 0},
        {"default_channel_ids", Json::array({"101", "102"})},
        {"prompts",
         Json::array(
             {{{"id", "401"},
               {"type", 0},
               {"title", "What brings you here?"},
               {"single_select", false},
               {"required", false},
               {"in_onboarding", true},
               {"options",
                Json::array(
                    {{{"id", "402"},
                      {"title", "Linux desktops"},
                      {"description", "Talk about your setup"},
                      {"role_ids", Json::array({"51"})},
                      {"channel_ids", Json::array({"101"})}}}
                )}}}
         )}
    };
    state.lists["screening"] = {
        {"enabled", true},
        {"description", "Please read our rules before joining the conversation."},
        {"form_fields",
         Json::array(
             {{{"field_type", "TERMS"},
               {"label", "Server rules"},
               {"required", true},
               {"values",
                Json::array({"Be respectful to other members.", "Keep discussions in the relevant channels."})}}}
         )}
    };
    state.lists["discovery"] = {
        {"is_published", false},
        {"emoji_discoverability_enabled", false},
        {"about", "A creative Linux desktop community."},
        {"keywords", Json::array({"Linux", "desktop"})},
        {"primary_category_id", 1},
        {"social_links", Json::array()}
    };
    state.lists["discovery-categories"] =
        Json::array({{{"id", 1}, {"name", "Gaming"}}, {{"id", 2}, {"name", "Technology"}}});
    state.loaded.insert("discovery-categories");
    state.lists["discovery-requirements"] = {{"healthy", true}, {"protected", true}, {"size", false}};
    state.lists["insights"] = Json::array(
        {{{"interval_start_timestamp", "2035-10-15T00:00:00Z"}, {"active_members", 180}, {"messages", 1200}},
         {{"interval_start_timestamp", "2035-10-16T00:00:00Z"}, {"active_members", 210}, {"messages", 1480}},
         {{"interval_start_timestamp", "2035-10-17T00:00:00Z"}, {"active_members", 198}, {"messages", 1330}}}
    );
    state.loaded.insert("boosts");
    state.loaded.insert("discovery");
    state.loaded.insert("insights");
    for (const auto& id :
         {"profile",      "general",   "roles",  "members", "invites", "emoji",     "stickers",  "soundboard",
          "integrations", "webhooks",  "safety", "automod", "audit",   "bans",      "community", "onboarding",
          "welcome",      "screening", "events", "widget",  "vanity",  "templates", "ownership", "delete"})
      state.loaded.insert(id);
    prepareServerManagementDemo();
    std::string page = "profile";
    if (m_demoOverlay.starts_with("server-settings:"))
      page = m_demoOverlay.substr(16);
    if (page.starts_with("audit-"))
      page = "audit";
    if (page.starts_with("commands-"))
      page = "commands";
    if (page == "role")
      page = "roles";
    if (page == "member")
      page = "members";
    selectServerSettingsPage(page, true);
    if (m_demoOverlay.ends_with("-light"))
      m_preferences.theme = "light";
    if (m_demoOverlay == "server-settings-narrow")
      m_window.setUiScale(1.5F);
    if (m_demoOverlay == "server-settings:role" || m_demoOverlay == "server-settings-role-light") {
      selectServerSettingsPage("roles", true);
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "record:50"});
    }
    if (m_demoOverlay == "server-settings:member")
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "record:43"});
    if (m_demoOverlay == "server-settings-restricted") {
      m_account.id = "43";
      m_roleDirectory.apply(
          api::parseRoleUpdates(
              "GUILD_ROLE_UPDATE",
              {{"guild_id", "100"},
               {"role",
                {{"id", "51"},
                 {"name", "Owlkeeper"},
                 {"position", 1},
                 {"permissions", std::to_string((1ULL << 5) | (1ULL << 27))},
                 {"color", 5879719}}}}
          )
      );
      selectServerSettingsPage("general", true);
    }
    if (m_demoOverlay == "server-settings-picker") {
      state.profileColorPicker = "brand_color_primary";
      state.scroll = 400;
    }
    if (m_demoOverlay == "server-settings-role-picker") {
      selectServerSettingsPage("roles", true);
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "record:50"});
      state.colorPicker = true;
      state.scroll = 120;
    }
    if (m_demoOverlay == "server-settings-event") {
      selectServerSettingsPage("events", true);
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "record:308"});
    }
    if (m_demoOverlay == "server-settings-sticker") {
      selectServerSettingsPage("stickers", true);
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "record:313"});
    }
    if (m_demoOverlay == "server-settings-sound") {
      selectServerSettingsPage("soundboard", true);
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "record:303"});
    }
    if (m_demoOverlay == "server-settings-crop") {
      DecodedRasterImage raster;
      raster.width = 480;
      raster.height = 320;
      raster.pixels.resize(480 * 320 * 4);
      for (int y = 0; y < 320; ++y)
        for (int x = 0; x < 480; ++x) {
          const auto at = (y * 480 + x) * 4;
          raster.pixels[at] = x * 255 / 479;
          raster.pixels[at + 1] = y * 255 / 319;
          raster.pixels[at + 2] = 120;
          raster.pixels[at + 3] = 255;
        }
      beginServerImageCrop("icon", "image/png", {}, std::move(raster));
    }
    if (m_demoOverlay == "server-settings-template-null") {
      selectServerSettingsPage("templates", true);
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "record:noctalia-template"});
      state.record["is_dirty"] = nullptr;
    }
    if (m_demoOverlay == "server-settings-tags")
      state.scroll = std::numeric_limits<float>::max();
    if (m_demoOverlay == "server-settings-loading") {
      state.loaded.clear();
      state.loading = true;
      state.overview = Json::object();
    }
    if (m_demoOverlay == "server-settings-error") {
      setServerSettingsValue("name", "A revised server name");
      state.error = "Missing permissions. Your server role changed before this request was accepted.";
    }
    if (m_demoOverlay == "server-settings-mfa" || m_demoOverlay == "server-settings-mfa-error") {
      api::GuildSettingsRequest request;
      request.guildId = "100";
      request.section = Section::Profile;
      request.verb = Verb::Update;
      request.values = {{"name", "Noctalia"}};
      state.mfaRequest = request;
      state.mfaChallenge = 77;
      state.mfaMethods = {"totp", "password"};
      state.mfaType = "totp";
      if (m_demoOverlay == "server-settings-mfa-error")
        state.error = "The verification code is incorrect. Try the current code from your authenticator.";
    }
  }

  void ClientApplication::demoServerSettingsStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto click = [&](const std::string& id) {
      const auto at = m_widgets.find(id);
      require(at != m_widgets.end(), "Server settings native control must exist");
      const auto* node = at->second;
      float x = node->width() / 2, y = node->height() / 2;
      for (const auto* parent = node; parent; parent = parent->parent()) {
        x += parent->x();
        y += parent->y();
      }
      m_window.click(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272);
      m_window.released();
    };
    const auto acknowledge = [&](Json data = Json::object(), std::string error = {}) {
      auto& state = m_serverSettings;
      require(!state.pending.empty(), "Server settings request must be queued");
      const auto request = state.pending.begin()->second;
      api::Event event;
      event.operation = api::Operation::GuildSettings;
      event.guildSettingsRequest = request;
      event.guildSettings = std::move(data);
      event.error = std::move(error);
      serverSettingsResult(event);
    };
    const auto clearReads = [&] {
      auto& state = m_serverSettings;
      std::erase_if(state.pending, [](const auto& pair) { return pair.second.verb == Verb::Get; });
      state.loading = false;
      state.loaded.insert(state.page);
      if (state.page == "profile" || state.page == "widget")
        state.lists[state.page] = state.record;
      else if (state.lists.contains(state.page) && state.lists[state.page].is_array() && !state.resource.empty())
        for (auto& record : state.lists[state.page]) {
          const auto id = record.contains("user") ? record["user"].value("id", "")
                                                  : record.value("id", record.value("sound_id", ""));
          if (id == state.resource)
            record = state.record;
        }
    };
    auto& state = m_serverSettings;
    const auto step = m_demoInteractionStep++;
    std::cerr << "server-settings-cycle step " << step << "\n";
    if (step == 0) {
      require(
          state.open && m_inputs.contains("server-settings:profile:name"),
          "Server settings opens as a native settings view"
      );
      focusControl("server-settings:profile:name");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("server-settings:profile:name", "Noctalia community");
      require(
          state.draft.value("name", "") == "Noctalia community",
          "Native server name editing updates only its guild draft"
      );
    } else if (step == 1) {
      selectServerSettingsPage("roles");
      require(state.confirmDiscard && state.page == "profile", "Navigation guards unsaved server changes");
    } else if (step == 2) {
      click("server-button:keep-editing");
      require(!state.confirmDiscard && state.dirty(), "Keep editing retains the original draft");
      click("server-button:reset");
      require(!state.dirty(), "Reset discards only the server draft");
      selectServerSettingsPage("roles");
    } else if (step == 3) {
      click("server-button:record:50");
    } else if (step == 4) {
      require(
          state.resource == "50" && state.record.value("name", "") == "Moderator", "Role list opens the selected role"
      );
      click("server-toggle:roles:hoist");
    } else if (step == 5) {
      require(
          state.draft.contains("hoist") && !state.draft.contains("permissions"),
          "Role toggle patches only its changed field"
      );
      click("server-button:save");
      require(state.saving && state.pending.begin()->second.resourceId == "50", "Role save targets the edited role");
      auto changed = state.record;
      changed["hoist"] = false;
      acknowledge(changed);
      clearReads();
    } else if (step == 6) {
      require(!state.dirty() && !state.saving, "Acknowledged role changes clear the save bar");
      selectServerSettingsPage("members");
    } else if (step == 7) {
      click("server-button:record:43");
    } else if (step == 8) {
      require(state.resource == "43", "Member editor retains its target");
      setServerSettingsValue("nick", "Bar Rulus");
    } else if (step == 9) {
      click("server-button:save");
      acknowledge(Json::object(), "Nickname update failed. Please try again.");
      require(
          state.draft.value("nick", "") == "Bar Rulus" && !state.saving,
          "Failed changes retain the member draft and allow retry"
      );
    } else if (step == 10) {
      click("server-button:save");
      auto member = state.record;
      member["nick"] = "Bar Rulus";
      acknowledge(member);
      clearReads();
      selectServerSettingsPage("widget");
    } else if (step == 11) {
      click("server-toggle:widget:enabled");
    } else if (step == 12) {
      require(state.draft.value("enabled", true) == false, "Widget uses the retained native toggle");
      click("server-button:save");
      acknowledge({{"enabled", false}, {"channel_id", "101"}});
      acknowledge({{"enabled", false}, {"channel_id", "101"}});
      require(!state.dirty() && !state.record.value("enabled", true), "Widget save and refresh agree");
      selectServerSettingsPage("profile");
    } else if (step == 13) {
      setServerSettingsValue("name", "Changed server");
      closeServerSettings();
      require(state.open && state.confirmDiscard, "Escape or close protects server drafts");
    } else if (step == 14) {
      click("server-button:discard");
      require(!state.open, "Discard confirms leaving server settings");
      prepareServerSettingsDemo();
    } else if (step == 15) {
      selectServerSettingsPage("roles");
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "record:52"});
      setServerSettingsValue("name", "Cannot edit integration role");
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "save"});
      require(
          state.pending.empty() && !state.error.empty(),
          "Managed roles cannot be changed even through raced save actions"
      );
      state.draft.clear();
      selectServerSettingsPage("profile", true);
    } else if (step == 16) {
      setServerSettingsValue("name", "Protected change");
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "save"});
      require(!state.pending.empty(), "Explicit profile save queues a request");
      auto original = state.pending.begin()->second;
      api::Event challenge;
      challenge.operation = api::Operation::GuildSettings;
      challenge.guildSettingsRequest = original;
      challenge.guildSettingsMfaChallenge = 77;
      challenge.guildSettingsMfaMethods = {"totp", "password"};
      serverSettingsResult(challenge);
      require(state.mfaRequest && state.dirty(), "MFA challenge retains the exact draft and action");
    } else if (step == 17) {
      require(m_inputs.contains("server-settings:mfa-code"), "Verification uses a native masked field");
      state.mfaCode = "123456";
    } else if (step == 18) {
      click("server-button:mfa-verify");
      require(
          state.saving
              && state.pending.begin()->second.mfaChallenge == 77
              && state.pending.begin()->second.values.value("name", "") == "Protected change",
          "Verification resubmits only the explicit original action"
      );
      acknowledge({{"id", "100"}, {"name", "Protected change"}});
      clearReads();
      require(!state.dirty() && !state.mfaRequest, "Verified acknowledgement clears verification secrets and draft");
      selectServerSettingsPage("onboarding");
    } else if (step == 19) {
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "add-prompt"});
      require(state.draft["prompts"].size() == 2, "Onboarding adds a typed native question");
      state.draft.clear();
      selectServerSettingsPage("delete", true);
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "delete-server"});
    } else if (step == 20) {
      require(
          state.confirmDelete && m_inputs.contains("server-settings:confirmation"),
          "Server deletion requires an inline name confirmation"
      );
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "confirm-destructive"});
      require(state.pending.empty() && !state.error.empty(), "Unconfirmed server deletion never queues a mutation");
    } else if (step == 21) {
      state.confirmDelete = false;
      state.error.clear();
      selectServerSettingsPage("general", true);
    } else if (step == 22) {
      click("server-select:general:default_message_notifications");
      require(isSelectDropdownOpen(), "Native Select opens the settings dropdown host");
    } else if (step == 23) {
      require(
          m_selectControl && m_selectRequest && m_selectBounds.height > 0,
          "Dropdown remains retained across scene rebuilds"
      );
      m_window.key({XKB_KEY_Home, "", false, false});
      m_window.key({XKB_KEY_Return, "", false, false});
      require(
          !isSelectDropdownOpen() && state.draft.value("default_message_notifications", -1) == 0,
          "Keyboard chooses the actual dropdown option without submitting settings"
      );
      state.draft.clear();
      m_scrolling.stop(state.scroll);
      state.scroll = std::numeric_limits<float>::max();
    } else if (step == 24) {
      click("server-select:general:preferred_locale");
    } else if (step == 25) {
      require(
          isSelectDropdownOpen() && m_selectContent > m_selectBounds.height,
          "Long dropdown choices use their own clipped viewport"
      );
      m_window.key({XKB_KEY_End, "", false, false});
      require(m_selectScroll > 0, "Keyboard reveals offscreen dropdown choices");
    } else if (step == 26) {
      m_window.key({XKB_KEY_Escape, "", false, false});
      require(
          !isSelectDropdownOpen() && state.open && !state.dirty(),
          "Escape dismisses the dropdown without closing settings or saving a choice"
      );
    } else if (step == 27) {
      selectServerSettingsPage("profile", true);
    } else if (step == 28) {
      require(state.open && !state.dirty(), "Main settings replay returns to a clean profile");
      const auto at = m_widgets.find("server-button:profile-color:brand_color_primary");
      require(at != m_widgets.end(), "The profile accent picker exists in the measured form");
      float top = 0;
      for (const auto* node = at->second; node; node = node->parent())
        top += node->y();
      m_scrolling.stop(state.scroll);
      state.scroll = std::max(0.0F, state.scroll + top - 200);
    } else if (step == 29) {
      click("server-button:profile-color:brand_color_primary");
    } else if (step == 30) {
      require(
          m_widgets.contains("server-profile-color:brand_color_primary")
              && m_inputs.contains("server-settings:picker:profile:brand_color_primary:hex"),
          "Color chooser registers its retained native child fields"
      );
      m_scrolling.stop(state.scroll);
      state.scroll += 10;
    } else if (step == 31) {
      focusControl("server-settings:picker:profile:brand_color_primary:hex");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("server-settings:picker:profile:brand_color_primary:hex", "#123456");
      require(
          state.draft.value("brand_color_primary", "") == "#123456",
          "Native hex editing applies a typed server profile color"
      );
    } else if (step == 32) {
      const auto* picker = static_cast<SettingsColorPicker*>(m_widgets.at("server-profile-color:brand_color_primary"));
      require(
          m_inputs.at("server-settings:picker:profile:brand_color_primary:hex")->parent() == picker,
          "Picker children remain inside their retained parent across rebuilds"
      );
      m_window.key({XKB_KEY_z, "", true, false});
      require(picker->color() == rgbHex(0xa8c86b), "Native color input undo survives reconstruction");
      state.draft.clear();
      selectServerSettingsPage("events", true);
    } else if (step == 33) {
      click("server-button:record:308");
    } else if (step == 34) {
      require(
          m_inputs.contains("server-settings:event-date:scheduled_start_time")
              && m_inputs.contains("server-settings:event-time:scheduled_start_time"),
          "Event dates use native local date and time fields"
      );
      focusControl("server-settings:event-date:scheduled_start_time");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("server-settings:event-date:scheduled_start_time", "2035-11-20");
      focusControl("server-settings:event-time:scheduled_start_time");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("server-settings:event-time:scheduled_start_time", "20:45");
    } else if (step == 35) {
      require(
          state.lists["event-local-fields"]["scheduled_start_time"].value("date", "") == "2035-11-20"
              && state.lists["event-local-fields"]["scheduled_start_time"].value("time", "") == "20:45",
          "Local date/time edits retain the human input across rebuilds"
      );
      focusControl("server-settings:event-date:scheduled_end_time");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("server-settings:event-date:scheduled_end_time", "2035-11-20");
      focusControl("server-settings:event-time:scheduled_end_time");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("server-settings:event-time:scheduled_end_time", "22:00");
    } else if (step == 36) {
      click("server-button:save");
      require(
          !state.pending.empty() && state.pending.begin()->second.section == Section::Events,
          "Event save targets the scheduled event"
      );
      const auto request = state.pending.begin()->second;
      require(
          request.values["scheduled_start_time"].is_string()
              && request.values["scheduled_start_time"].get<std::string>().ends_with("Z")
              && request.values["scheduled_end_time"].get<std::string>().ends_with("Z"),
          "Only serialized UTC timestamps reach the API"
      );
      auto saved = state.record;
      for (const auto& [key, entry] : request.values.items())
        saved[key] = entry;
      acknowledge(saved);
      clearReads();
    } else if (step == 37) {
      require(!state.dirty() && !state.saving, "Event acknowledgement clears the explicit save draft");
      require(
          m_inputs.at("server-settings:mfa-code")->value().empty(), "Retained verification text is scrubbed after use"
      );
      selectServerSettingsPage("delete", true);
      m_scrolling.stop(state.scroll);
      state.scroll = 10000;
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "delete-server"});
      require(state.scroll == 0, "Destructive confirmations reveal themselves even after deep scrolling");
    } else if (step == 38) {
      require(
          state.confirmDelete && m_inputs.contains("server-settings:confirmation"),
          "The owner confirmation is presented inline"
      );
      focusControl("server-settings:confirmation");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("server-settings:confirmation", state.overview.value("name", ""));
    } else if (step == 39) {
      click("server-button:confirm-destructive");
      require(
          state.saving && state.pending.begin()->second.section == Section::DeleteGuild,
          "Only a confirmed owner action queues server deletion"
      );
      acknowledge(Json());
      require(
          !state.open && m_guilds.empty() && m_dmMode,
          "Confirmed server deletion removes the rail entry and opens direct messages"
      );
    } else if (step == 40) {
      require(
          !m_guildChannels.contains("100") && m_roleDirectory.owner("100").empty() && m_threads.list("100").empty(),
          "Deleted guild caches are cleared immediately"
      );
    } else if (step == 41) {
      require(
          !state.open && state.pending.empty(),
          "The complete native server replay finishes without account or state writes"
      );
    }
    if (step < 41)
      m_window.invalidate();
  }

  void ClientApplication::demoServerExpressionsStep() {
    const auto require = [](bool condition, const char* message) {
      if (!condition)
        throw std::runtime_error(message);
    };
    auto& state = m_serverSettings;
    const auto step = m_demoInteractionStep++;
    std::cerr << "server-settings-expressions-cycle step " << step << "\n";
    const auto action = [&](const std::string& id) {
      serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, id});
    };
    const auto click = [&](const std::string& id) {
      const auto at = m_widgets.find(id);
      require(at != m_widgets.end(), "Expression native control must exist");
      auto* node = at->second;
      float x = node->width() / 2, y = node->height() / 2;
      for (const auto* parent = node; parent; parent = parent->parent()) {
        x += parent->x();
        y += parent->y();
      }
      m_window.click(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272);
      m_window.released();
    };
    const auto acknowledge = [&](Json data, std::string error = {}) {
      require(!state.pending.empty(), "Expression operation must be explicitly queued");
      api::Event event;
      event.operation = api::Operation::GuildSettings;
      event.guildSettingsRequest = state.pending.begin()->second;
      event.guildSettings = std::move(data);
      event.error = std::move(error);
      serverSettingsResult(event);
      std::erase_if(state.pending, [](const auto& request) { return request.second.verb == Verb::Get; });
      state.loading = false;
    };
    if (step == 0)
      selectServerSettingsPage("emoji", true);
    else if (step == 1) {
      require(m_inputs.contains("server-settings:emoji-alias:301"), "Emoji use native inline alias editors");
      focusControl("server-settings:emoji-alias:301");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("server-settings:emoji-alias:301", "cozy_updated");
      require(
          state.expressionDrafts.contains("301") && state.dirty(), "Inline emoji alias changes remain an explicit draft"
      );
      PickedFile file;
      file.name = "party.png";
      file.mime = "image/png";
      std::vector<std::uint8_t> pixels(32 * 32 * 4, 200);
      file.bytes = encodePng(pixels.data(), 32, 32);
      file.raster = DecodedRasterImage{pixels, 32, 32};
      stageServerEmojiUploads({std::move(file)});
      require(state.emojiUploads.size() == 1, "Desktop multi-file results stage named upload drafts without sending");
    } else if (step == 2) {
      action("page:stickers");
      require(state.confirmDiscard && state.page == "emoji", "Navigation protects unsaved emoji names");
    } else if (step == 3) {
      click("server-button:keep-editing");
      require(!state.confirmDiscard && state.dirty(), "Keep editing preserves inline alias changes");
    } else if (step == 4) {
      click("server-button:save");
      require(
          state.pending.begin()->second.resourceId == "301",
          "Emoji editing targets the emoji ID rather than its uploader"
      );
      acknowledge(Json(), "Missing permission. Please try again after your role is restored.");
      require(
          state.expressionDrafts.contains("301") && !state.error.empty(), "Failed alias saves retain the native draft"
      );
    } else if (step == 5) {
      click("server-button:save");
      acknowledge({{"id", "301"}, {"name", "cozy_updated"}});
      require(
          !state.pending.empty() && state.pending.begin()->second.verb == Verb::Create,
          "Explicit Save continues with each staged emoji upload"
      );
      acknowledge({{"id", "316"}, {"name", "party"}, {"animated", false}});
      require(
          !state.dirty() && state.lists["emoji"][0]["name"] == "cozy_updated",
          "Alias acknowledgement updates the list and clears only its saved draft"
      );
    } else if (step == 6)
      selectServerSettingsPage("stickers", true);
    else if (step == 7)
      click("server-button:record:313");
    else if (step == 8) {
      require(
          state.resource == "313" && state.record.value("format_type", 0) == 2,
          "Animated stickers open their real asset record"
      );
      focusControl("server-settings:stickers:name");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("server-settings:stickers:name", "Celebration");
    } else if (step == 9) {
      click("server-button:reset");
      require(!state.dirty(), "Sticker reset restores the retained native form");
    } else if (step == 10)
      click("server-button:back");
    else if (step == 11) {
      const auto target =
          std::ranges::find_if(m_hits, [](const auto& hit) { return hit.id == "expression-delete:302"; });
      require(target != m_hits.end(), "Visible sticker trash has a clipped manual hit region");
      m_window.click(
          target->x + target->width / 2,
          target->y + target->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272
      );
      m_window.released();
      require(
          state.confirmDelete && state.resource == "302", "Sticker deletion presents a specific inline confirmation"
      );
    } else if (step == 12) {
      click("server-button:keep-editing");
      require(
          state.resource.empty() && state.pending.empty(),
          "Canceling sticker deletion returns to the grid without a request"
      );
    } else if (step == 13)
      selectServerSettingsPage("soundboard", true);
    else if (step == 14) {
      click("server-button:sound-preview:303");
      require(m_soundPreviewSource == "303", "Sound preview uses the separate audio player");
    } else if (step == 15) {
      click("server-button:sound-stop");
      require(m_soundPreviewSource.empty(), "Native Stop releases sound preview playback");
    } else if (step == 16) {
      selectServerSettingsPage("profile", true);
      DecodedRasterImage raster;
      raster.width = 480;
      raster.height = 320;
      raster.pixels.resize(480 * 320 * 4);
      for (int y = 0; y < 320; ++y)
        for (int x = 0; x < 480; ++x) {
          const auto at = (y * 480 + x) * 4;
          raster.pixels[at] = x * 255 / 479;
          raster.pixels[at + 1] = y * 255 / 319;
          raster.pixels[at + 2] = 90;
          raster.pixels[at + 3] = 255;
        }
      beginServerImageCrop("icon", "image/png", {}, std::move(raster));
    } else if (step == 17) {
      require(
          state.imageCrop && m_widgets.contains("server-crop-slider:zoom"),
          "Image cropping retains native slider controls"
      );
      click("server-crop-slider:zoom");
      require(state.imageCrop->zoom > 1, "The native crop slider changes actual crop geometry");
      m_scrolling.stop(state.scroll);
      state.scroll = 10000;
    } else if (step == 18) {
      click("server-button:crop-apply");
      require(
          !state.imageCrop && state.draft.value("icon", "").starts_with("data:image/png;base64,"),
          "Applying a crop stages an encoded PNG without sending a request"
      );
    } else if (step == 19) {
      click("server-button:reset");
      require(!state.dirty(), "Reset removes the cropped image draft");
      selectServerSettingsPage("events", true);
      action("record:308");
      m_scrolling.stop(state.scroll);
      state.scroll = 10000;
    } else if (step == 20)
      click("server-select:events:event-repeat");
    else if (step == 21) {
      m_window.key({XKB_KEY_Down, "", false, false});
      m_window.key({XKB_KEY_Return, "", false, false});
      require(
          state.draft.contains("recurrence_rule") && state.draft["recurrence_rule"].value("frequency", -1) == 3,
          "The native repeat picker stages a daily recurrence rule"
      );
    } else if (step == 22) {
      state.draft.clear();
      selectServerSettingsPage("welcome", true);
      action("add-welcome");
    } else if (step == 23) {
      const auto before = state.draft["welcome_channels"];
      action("welcome-move:up:1");
      require(
          state.draft["welcome_channels"][0] == before[1], "Welcome recommendations preserve explicit user ordering"
      );
    } else if (step == 24) {
      state.draft.clear();
      selectServerSettingsPage("emoji", true);
      m_account.id = "43";
      m_roleDirectory.apply(
          api::parseRoleUpdates(
              "GUILD_ROLE_UPDATE",
              {{"guild_id", "100"},
               {"role",
                {{"id", "51"}, {"name", "Creator"}, {"position", 1}, {"permissions", std::to_string(1ULL << 43)}}}}
          )
      );
      for (int i = 0; i < 12; ++i)
        state.lists["emoji"].push_back(
            {{"id", std::to_string(400 + i)},
             {"name", "extra_" + std::to_string(i)},
             {"animated", false},
             {"user", {{"id", "43"}, {"username", "barrulus"}}}}
        );
      m_scrolling.stop(state.scroll);
      state.scroll = 10000;
    } else if (step == 25) {
      require(
          !m_inputs.at("server-settings:emoji-alias:301")->enabled()
              && m_inputs.at("server-settings:emoji-alias:311")->enabled(),
          "Create Expressions permits editing only the account's own emoji"
      );
      bool visibleDelete = false;
      for (const auto& hit : m_hits)
        if (hit.action == Action::ServerSettingsAction && hit.id.starts_with("expression-delete:")) {
          require(
              hit.y >= 108
                  && hit.y + hit.height <= m_window.height()
                          + layout::Window::titlebarHeight
                          - m_window.titlebarHeight()
                          - Style::spaceXl,
              "Clamped manual expression hits remain inside the settings viewport"
          );
          require(
              hit.id != "expression-delete:311",
              "A scrolled-away expression trash target cannot intercept visible settings"
          );
          visibleDelete = true;
        }
      require(visibleDelete && state.scroll < 10000, "The clamp frame rebases visible manual trash hit regions");
      closeServerSettings(true);
      require(state.pending.empty() && !state.open, "Expression replay finishes without account or disk writes");
    }
    if (step < 25)
      m_window.invalidate();
  }
} // namespace app
