#include "app/client_application.h"
#include "render/scene/input_area.h"
#include "ui/controls/input.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    using Json = nlohmann::json;
    using Section = api::UserSettingsSection;
    using Verb = api::UserSettingsVerb;
    std::string key(Section section, const std::string& guild = {}) {
      return std::to_string(static_cast<int>(section)) + ':' + guild;
    }
  } // namespace
  void ClientApplication::prepareUserAccountDemo() {
    auto& state = m_userAccount;
    if (state.loaded.contains("demo"))
      return;
    state.loaded.insert("demo");
    m_account.premiumType = 2;
    state.records[key(Section::Account)] = {
        {"id", m_account.id},
        {"username", m_account.username},
        {"global_name", m_account.name},
        {"verified", true},
        {"email", "alex@example.invalid"},
        {"phone", "+15550102030"},
        {"mfa_enabled", true},
        {"mfa_totp", true},
        {"mfa_sms", false},
        {"premium_type", 2}
    };
    state.records[key(Section::Profile)] = {
        {"id", m_account.id},       {"global_name", m_account.name},
        {"pronouns", "they/them"},  {"bio", "Building a little space for our community."},
        {"accent_color", 0x8F80D4}, {"theme_colors", {0x343040, 0xA89ACA}}
    };
    state.records[key(Section::SecurityKeys)] =
        Json::array({{{"id", "synthetic-key"}, {"name", "Laptop passkey"}, {"type", 0}}});
    state.records[key(Section::Sessions)] = {
        {"user_sessions",
         Json::array(
             {{{"id_hash", "current-demo-session"},
               {"current_session", true},
               {"approx_last_used_time", "2026-10-18T12:00:00Z"},
               {"client_info", {{"os", "Linux"}, {"platform", "Cressida"}}}},
              {{"id_hash", "other-demo-session"},
               {"current_session", false},
               {"approx_last_used_time", "2026-10-17T18:30:00Z"},
               {"client_info", {{"os", "Android"}, {"platform", "Discord"}}}}}
         )}
    };
    state.records[key(Section::AuthorizedApps)] = Json::array(
        {{{"id", "701"},
          {"application",
           {{"id", "501"}, {"name", "Community Helper"}, {"description", "Tools for your communities."}}},
          {"scopes", {"identify", "guilds", "applications.commands"}}}}
    );
    state.records[key(Section::Connections)] = Json::array(
        {{{"id", "12345"},
          {"type", "github"},
          {"name", "alex-dev"},
          {"visibility", true},
          {"metadata_visibility", false},
          {"show_activity", false},
          {"friend_sync", false},
          {"verified", true}}}
    );
    state.records[key(Section::Collectibles)] = Json::array(
        {{{"sku_id", "9000"},
          {"name", "Community cosmetics"},
          {"items",
           Json::array(
               {{{"sku_id", "9001"}, {"type", 0}, {"label", "Moon decoration"}},
                {{"sku_id", "9002"}, {"type", 1}, {"label", "Starlight effect"}},
                {{"sku_id", "9003"}, {"type", 2}, {"label", "Night nameplate"}},
                {{"sku_id", "9004"}, {"type", 3}, {"label", "Silver frame"}}}
           )}}}
    );
    for (const auto& guild : m_guilds)
      state.records[key(Section::GuildProfile, guild.id)] = {
          {"id", m_account.id},
          {"guild_id", guild.id},
          {"nick", "Alex in " + guild.name},
          {"pronouns", "they/them"},
          {"bio", "Here for the community."},
          {"accent_color", 0x708F82},
          {"theme_colors", {0x263B35, 0x789F91}}
      };
    for (const auto& guild : m_guilds)
      state.records[key(Section::GuildMember, guild.id)] = {{"nick", "Alex in " + guild.name}, {"avatar", nullptr}};
    for (const auto& [cache, _] : state.records)
      state.loaded.insert(cache);
    state.profile = state.records[key(state.guild.empty() ? Section::Profile : Section::GuildProfile, state.guild)];
  }

  void ClientApplication::demoUserAccountStep() {
    auto& state = m_userAccount;
    auto require = [](bool condition, const char* message) {
      if (!condition)
        throw std::runtime_error(message);
    };
    const auto click = [&](const std::string& id) {
      const auto at = m_widgets.find(id);
      require(at != m_widgets.end() && at->second->parent(), "The account action must use a mounted retained control");
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(at->second, left, top, right, bottom);
      m_window.motion((left + right) / 2, (top + bottom) / 2);
      m_window.click((left + right) / 2, (top + bottom) / 2, 272);
      m_window.released();
    };
    const auto enter = [&](const std::string& id, const std::string& text) {
      require(m_inputs.contains(id), "The account editor must use retained native text fields");
      m_inputs.at(id)->setValue("");
      focusControl(id);
      pasteControl(id, text);
    };
    const auto latest = [&]() {
      require(!state.pending.empty(), "Explicit native account actions must retain their request identity");
      return state.pending.rbegin()->second.request;
    };
    const auto result = [&](const api::UserSettingsRequest& request, Json data = Json::object(),
                            std::string error = {}) {
      api::Event event;
      event.operation = api::Operation::UserSettings;
      event.session = m_session;
      event.userSettingsRequest = request;
      event.userSettings = std::move(data);
      event.error = std::move(error);
      require(userAccountResult(event), "The account module must consume its matching synthetic result");
    };
    const auto step = m_demoInteractionStep++;
    std::clog << "Offline account settings frame " << step << '\n';
    if (step == 0) {
      const bool revealed = state.revealEmail;
      click("user-account-button:password-recover");
      const auto recovery = latest();
      require(
          recovery.section == Section::PasswordRecovery
              && recovery.verb == Verb::Create
              && recovery.values["login"] == state.records[key(Section::Account)]["email"]
              && state.revealEmail == revealed,
          "Password recovery requires its explicit native action and does not reveal the account email"
      );
      result(recovery, {{"requested", true}, {"method", "email"}});
      require(
          !state.busy && !state.status.empty(),
          "Password recovery provides explicit completion feedback without exposing the account email"
      );
      click("user-account-button:edit-username");
      require(state.dialog == "username" && state.pending.empty(), "Opening account editing must not issue a mutation");
    } else if (step == 1) {
      const auto* modal = m_widgets.at("user-account-dialog-button:dialog-cancel")->parent();
      for (int i = 0; i < 24; ++i) {
        onKey({.symbol = XKB_KEY_Tab, .shift = i % 2 == 0});
        bool inside = false;
        for (const Node* node = m_controlInput.focusedArea(); node; node = node->parent())
          inside |= node == modal;
        require(inside, "Tab and Shift+Tab remain inside the native account modal");
      }
      const auto typingPending = state.pending.size();
      enter("user-account:secret:value", "alex.updated");
      enter("user-account:secret:password", "synthetic-password");
      require(
          state.pending.size() == typingPending,
          "Typing account or password fields never makes a request before an explicit action"
      );
      click("user-account-dialog-button:dialog-submit");
      auto request = latest();
      require(
          request.section == Section::Account
              && request.values["username"] == "alex.updated"
              && request.password.empty(),
          "Native username editing submits only the explicit change and strips matching metadata secrets"
      );
      result(request, Json::object(), "This username is unavailable.");
      require(
          state.fields["value"] == "alex.updated"
              && state.fields["password"].empty()
              && m_inputs.at("user-account:secret:password")->value().empty(),
          "Failures keep the intended username while clearing private input and undo buffers"
      );
      const auto* cancel = m_widgets.at("user-account-dialog-button:dialog-cancel");
      bool cancelFocused = false;
      for (int i = 0; i < 16 && !cancelFocused; ++i) {
        onKey({.symbol = XKB_KEY_Tab});
        for (const Node* node = m_controlInput.focusedArea(); node; node = node->parent())
          cancelFocused |= node == cancel;
      }
      require(cancelFocused, "The modal Cancel button is keyboard focusable");
      onKey({.symbol = XKB_KEY_Return});
      require(!userAccountDialogOpen() && state.pending.empty(), "Enter on Cancel closes without submitting a change");
      userAccountAction("edit-email");
      userAccountAction("dialog-submit");
      request = latest();
      require(
          request.section == Section::EmailChange && request.verb == Verb::Create,
          "Email editing must first request current-email verification"
      );
      result(request, {{"verified", false}});
    } else if (step == 2) {
      require(state.dialog == "email-code", "Email proof delivery opens its native verification step");
      enter("user-account:secret:code", "123456");
      click("user-account-dialog-button:dialog-submit");
      const auto request = latest();
      require(
          request.section == Section::EmailChange && request.verificationCode.empty(),
          "Email verification inputs never enter public request metadata"
      );
      result(request, {{"verified", true}});
    } else if (step == 3) {
      require(state.dialog == "email-new", "A new email is collected only after the current-email code succeeds");
      enter("user-account:secret:value", "updated@example.invalid");
      enter("user-account:secret:password", "synthetic-password");
      click("user-account-dialog-button:dialog-submit");
      api::Event event;
      event.operation = api::Operation::UserSettings;
      event.session = m_session;
      event.userSettingsRequest = latest();
      event.userSettingsMfaChallenge = 901;
      event.userSettingsMfaMethods = {"totp", "password"};
      userAccountResult(event);
      require(
          state.mfaRequest && state.mfaRequest->password.empty() && state.fields["password"].empty(),
          "An MFA challenge retains only the public action identity"
      );
      m_account.avatar = "/avatars/1/synthetic-avatar.png?size=128";
      m_account.pronouns = "they/them";
      m_account.bio = "Keep this biography through partial account replies.";
    } else if (step == 4) {
      enter("user-account:secret:code", "654321");
      m_window.key({.symbol = XKB_KEY_Return});
      const auto request = latest();
      require(
          request.mfaChallenge == 901
              && request.mfaCode.empty()
              && request.values["email"] == "updated@example.invalid",
          "Enter verifies the identical explicit email action without exposing its code"
      );
      auto account = state.records[key(Section::Account)];
      account["email"] = "updated@example.invalid";
      result(request, account);
      require(
          !userAccountDialogOpen() && state.fields.empty(),
          "An acknowledged account update clears its complete verification editor"
      );
      require(
          m_account.avatar == "/avatars/1/synthetic-avatar.png?size=128"
              && m_account.pronouns == "they/them"
              && m_account.bio == "Keep this biography through partial account replies.",
          "Partial account replies must preserve cached avatar, pronouns and biography"
      );
      userAccountAction("phone-add");
    } else if (step == 5) {
      enter("user-account:secret:phone", "+15550109999");
      click("user-account-dialog-button:dialog-submit");
      auto request = latest();
      require(
          request.section == Section::Phone && request.verb == Verb::Create,
          "Phone attachment begins by sending a verification SMS"
      );
      result(request);
      require(
          state.dialog == "phone-code" && state.fields["phone"] == "+15550109999",
          "The SMS step retains the intended phone number"
      );
    } else if (step == 6) {
      enter("user-account:secret:code", "123456");
      click("user-account-dialog-button:dialog-submit");
      const auto request = latest();
      require(
          request.section == Section::PhoneVerification && !request.values.contains("phone_token"),
          "Phone verification uses no UI-managed proof token"
      );
      result(request, {{"verified", true}});
    } else if (step == 7) {
      require(
          state.dialog == "phone-attach", "Successful phone verification requires an explicit password attachment step"
      );
      enter("user-account:secret:password", "synthetic-password");
      click("user-account-dialog-button:dialog-submit");
      const auto request = latest();
      require(
          request.section == Section::Phone && request.verb == Verb::Update && request.password.empty(),
          "Verified phone attachment uses the typed worker-owned proof and a scrubbed identity"
      );
      result(request);
      userAccountAction("profile-open");
    } else if (step == 8) {
      enter("user-account:profile:global_name", "Updated Alex");
      enter("user-account:profile:bio", "An updated biography.");
      userAccountAction("profile-save");
      auto request = latest();
      require(
          request.section == Section::Account
              && request.values.contains("global_name")
              && !request.values.contains("bio"),
          "Global identity and profile metadata use their distinct primary routes"
      );
      auto account = state.records[key(Section::Account)];
      account["global_name"] = "Updated Alex";
      result(request, account);
      request = latest();
      require(
          request.section == Section::Profile && request.values == Json{{"bio", "An updated biography."}},
          "The second profile write waits for the identity acknowledgement and sends only its changed field"
      );
      result(request, Json::object(), "The profile update could not be saved.");
      require(
          !state.draft.contains("global_name") && state.draft.contains("bio") && !state.busy,
          "A split-save failure preserves only the remaining unsaved profile edits"
      );
    } else if (step == 9) {
      userAccountAction("profile-save");
      const auto request = latest();
      require(
          request.section == Section::Profile && !request.values.contains("global_name"),
          "Retry never repeats the already acknowledged identity write"
      );
      result(request, {{"bio", "An updated biography."}});
      require(state.draft.empty() && !m_profileDirty, "Acknowledged retries clear the profile's unsaved state");
      selectSettingsPage("devices");
    } else if (step == 10) {
      click("user-account-button:session-remove:other-demo-session");
      require(state.dialog == "session-remove", "Device logout requires a native confirmation before mutation");
    } else if (step == 11) {
      click("user-account-dialog-button:dialog-submit");
      auto request = latest();
      require(
          request.section == Section::Sessions
              && request.values["session_id_hashes"] == Json::array({"other-demo-session"}),
          "Device logout targets exactly the chosen opaque session"
      );
      result(request);
      selectSettingsPage("connections");
    } else if (step == 12) {
      click("user-account-toggle:user-account:connection:github:12345:visibility");
      const auto request = latest();
      require(
          request.section == Section::Connections && request.values == Json{{"visibility", false}},
          "The real retained connection toggle sends a boolean visibility change"
      );
      result(request);
      selectSettingsPage("account");
      userAccountAction("backup-view");
    } else if (step == 13) {
      enter("user-account:secret:password", "synthetic-password");
      click("user-account-dialog-button:dialog-submit");
      const auto request = latest();
      require(
          request.section == Section::BackupCodes && request.verb == Verb::Create,
          "Viewing backup codes first requests Discord's email challenge"
      );
      result(request, {{"challenge_ready", true}});
    } else if (step == 14) {
      enter("user-account:secret:code", "123456");
      click("user-account-dialog-button:dialog-submit");
      const auto request = latest();
      require(
          request.section == Section::BackupCodes
              && request.verb == Verb::Update
              && request.values["regenerate"] == false,
          "Viewing backup codes must never silently regenerate them"
      );
      result(
          request,
          {{"backup_codes",
            Json::array({{{"code", "1234-5678"}, {"consumed", false}}, {{"code", "4321-8765"}, {"consumed", true}}})}}
      );
      require(
          state.dialog == "backup-codes" && state.backupCodes.size() == 2,
          "Verified backup codes are shown only in the covered ephemeral dialog"
      );
    } else if (step == 15) {
      click("user-account-dialog-button:dialog-cancel");
      require(
          !userAccountDialogOpen() && state.backupCodes.empty() && state.fields.empty(),
          "Closing backup codes removes the private response and all verification inputs"
      );
      PickedFile stale;
      stale.request = (std::uint64_t{1} << 63) | 123;
      require(
          userAccountPickedFile(stale) && !state.crop,
          "Stale native chooser results cannot enter another account/profile editor"
      );
      userAccountAction("edit-password");
      state.fields["password"] = "synthetic-old-password";
      state.fields["new_password"] = state.fields["confirm_password"] = "synthetic-new-password";
      userAccountAction("dialog-submit");
      api::Event challenge;
      challenge.operation = api::Operation::UserSettings;
      challenge.session = m_session;
      challenge.userSettingsRequest = latest();
      require(
          challenge.userSettingsRequest->section == Section::Password
              && challenge.userSettingsRequest->newPassword.empty(),
          "A password change keeps its new password exclusively in worker-owned request state"
      );
      challenge.userSettingsMfaChallenge = 902;
      challenge.userSettingsMfaMethods = {"totp"};
      userAccountResult(challenge);
      state.fields["code"] = "123456";
      userAccountAction("dialog-submit");
      const auto resumed = latest();
      require(
          resumed.section == Section::Password && resumed.mfaChallenge == 902 && resumed.newPassword.empty(),
          "A scrubbed challenged password change must resume through worker verification without revalidating absent "
          "secrets"
      );
      result(resumed);
      api::UserSettingsRequest profileRead;
      profileRead.section = Section::Profile;
      profileRead.resourceId = m_account.id;
      requestUserAccount(profileRead);
      const auto projected = api::parseUserSettings(
          Json{
              {"user", {{"id", m_account.id}, {"bio", "Legacy account biography"}}},
              {"user_profile", {{"bio", "Current profile biography"}, {"pronouns", "they/them"}}}
          },
          profileRead
      );
      result(latest(), projected.data);
      require(
          state.profile["bio"] == "Current profile biography",
          "Nested legacy account fields cannot overwrite authoritative profile metadata"
      );
      state.loading.insert(key(Section::Profile));
      state.draft["bio"] = "A draft retained through refresh";
      const auto pendingCount = state.pending.size();
      require(
          !userAccountProfileReady(state),
          "Pending authoritative profile reads disable editing during post-save refresh"
      );
      userAccountAction("profile-save");
      require(
          state.pending.size() == pendingCount && state.draft.contains("bio"),
          "A raced Save cannot submit against a temporarily stale profile baseline"
      );
      state.loading.erase(key(Section::Profile));
      state.errors[key(Section::Profile)] = "Synthetic post-save refresh failure";
      require(
          !userAccountProfileReady(state),
          "Failed authoritative refreshes keep profile edits disabled until Retry succeeds"
      );
      userAccountAction("profile-save");
      require(
          state.pending.size() == pendingCount && state.draft.contains("bio"),
          "A failed refresh cannot allow a stale baseline Save"
      );
      state.errors.erase(key(Section::Profile));
      require(userAccountProfileReady(state), "A successful retry restores profile edit readiness");
      state.draft.clear();
      m_profileDirty = false;
      state.profile["collectibles"] =
          Json::array({{{"sku_id", "9002"}, {"type", 1}}, {{"sku_id", "9004"}, {"type", 3}}});
      api::UserSettingsRequest nameplate;
      nameplate.section = Section::GuildMember;
      nameplate.verb = Verb::Update;
      nameplate.guildId = m_guilds.front().id;
      nameplate.resourceId = m_account.id;
      nameplate.values = {{"collectibles", {{"nameplate", {{"sku_id", "9003"}}}}}};
      requestUserAccount(nameplate, "profile-save");
      result(latest());
      require(
          state.profile["collectibles"].is_array()
              && state.profile["collectibles"].size() == 2
              && state.profile["nameplate"]["sku_id"] == "9003",
          "Nameplate acknowledgements preserve the profile effect and frame baseline"
      );
      std::clog << "Offline native account settings replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
