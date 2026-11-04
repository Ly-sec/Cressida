#include "api/channel_features.h"
#include "app/client_application.h"
#include "app/theme.h"
#include "app/user_settings_form.h"
#include "ui/controls/button.h"
#include "ui/controls/select.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    using Json = nlohmann::json;
    using Section = api::UserSettingsSection;
    using Verb = api::UserSettingsVerb;
    bool ownedAction(const std::string& action) {
      return action == "account-reload"
          || action == "profile-open"
          || action == "profile-save"
          || action == "profile-reset"
          || action == "reveal-email"
          || action == "reveal-phone"
          || action == "email-verify"
          || action == "edit-username"
          || action == "edit-email"
          || action == "edit-password"
          || action == "password-recover"
          || action == "phone-add"
          || action == "phone-remove"
          || action == "phone-resend"
          || action == "totp-add"
          || action == "totp-remove"
          || action == "backup-view"
          || action == "backup-regenerate"
          || action == "backup-copy"
          || action == "secret-copy"
          || action == "sms-enable"
          || action == "sms-disable"
          || action == "key-register"
          || action == "connection-add"
          || action == "account-disable"
          || action == "account-delete"
          || action == "sessions-remove-all"
          || action == "dialog-submit"
          || action == "dialog-cancel"
          || action == "discard-profile"
          || action.starts_with("session-remove:")
          || action.starts_with("app-remove:")
          || action.starts_with("connection-remove:")
          || action.starts_with("connection-refresh:")
          || action.starts_with("key-rename:")
          || action.starts_with("key-remove:")
          || action.starts_with("mfa-method:")
          || action.starts_with("upload-")
          || action.starts_with("remove-")
          || action.starts_with("crop-");
    }
    std::string string(const Json& value, const char* key) {
      const auto at = value.find(key);
      return at != value.end() && at->is_string() ? at->get<std::string>() : "";
    }
    std::optional<int> color(const std::string& text) {
      if (text.size() != 7 || text.front() != '#')
        return {};
      int result = 0;
      const auto parsed = std::from_chars(text.data() + 1, text.data() + text.size(), result, 16);
      return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? std::optional(result) : std::nullopt;
    }
  } // namespace

  void ClientApplication::closeUserAccountDialog() {
    auto& state = m_userAccount;
    const bool hadDialog = !state.dialog.empty() || state.mfaRequest.has_value();
    if (hadDialog && !m_demo)
      m_client.clearUserSettingsSecrets();
    if (m_focusedInput.starts_with("user-account:secret:")) {
      m_controlInput.setFocus(nullptr);
      m_focusedInput.clear();
    }
    for (const auto& [id, input] : m_inputs)
      if (id.starts_with("user-account:secret:"))
        input->setValue("");
    state.fields.clear();
    state.backupCodes = Json::array();
    state.dialog.clear();
    state.resource.clear();
    state.service.clear();
    state.mfaRequest.reset();
    state.mfaChallenge = 0;
    state.mfaMethods.clear();
    state.mfaMethod.clear();
    state.mfaCache.clear();
    m_scrolling.stop(state.dialogScroll);
    state.dialogScroll = 0;
    if (hadDialog) {
      ++state.dialogGeneration;
      ++m_dialogGeneration;
    }
  }

  bool ClientApplication::userAccountAction(const std::string& action) {
    if (!m_settingsOpen || !ownedAction(action))
      return false;
    auto& state = m_userAccount;
    if (state.busy)
      return true;
    const auto open = [&](const std::string& kind, std::string resource = {}, std::string service = {}) {
      closeUserAccountDialog();
      closeSelectDropdown();
      m_controlInput.cancelPointerCapture();
      m_controlInput.setFocus(nullptr);
      m_focusedInput.clear();
      state.dialog = kind;
      state.resource = std::move(resource);
      state.service = std::move(service);
      state.error.clear();
      ++state.dialogGeneration;
      ++m_dialogGeneration;
      m_scrolling.stopAll();
    };
    const auto request = [&](Section section, Verb verb = Verb::Create) {
      api::UserSettingsRequest value;
      value.section = section;
      value.verb = verb;
      value.resourceId = state.resource;
      value.service = state.service;
      return value;
    };
    if (action == "dialog-cancel")
      closeUserAccountDialog();
    else if (action == "account-reload")
      loadUserAccountSettings(state.page, true);
    else if (action == "profile-open")
      selectSettingsPage("profile");
    else if (action == "reveal-email")
      state.revealEmail = !state.revealEmail;
    else if (action == "reveal-phone")
      state.revealPhone = !state.revealPhone;
    else if (action == "email-verify")
      requestUserAccount(request(Section::EmailVerification));
    else if (action == "edit-username") {
      open("username");
      state.fields["value"] = m_account.username;
    } else if (action == "edit-email")
      open("email-send-code");
    else if (action == "password-recover") {
      const auto& account = state.records[std::to_string(static_cast<int>(Section::Account)) + ':'];
      const auto login = string(account, "email");
      if (login.empty()) {
        state.error = "Reload your account details before requesting password reset instructions.";
        m_window.invalidate();
        return true;
      }
      auto value = request(Section::PasswordRecovery);
      value.values["login"] = login;
      requestUserAccount(std::move(value));
    } else if (action == "edit-password")
      open("password");
    else if (action == "phone-add")
      open("phone");
    else if (action == "phone-remove")
      open("phone-remove");
    else if (action == "phone-resend") {
      auto value = request(Section::PhoneResend);
      value.values["phone"] = state.fields["phone"];
      requestUserAccount(std::move(value));
    } else if (action == "totp-add") {
      open("totp-add");
      try {
        state.fields["secret"] = m_demo ? "JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP" : api::generateAuthenticatorSecret();
      } catch (...) {
        state.fields["secret"].clear();
      }
      if (state.fields["secret"].empty()) {
        state.error = "A secure authenticator key could not be generated. Try again.";
      }
    } else if (action == "totp-remove")
      open("totp-remove");
    else if (action == "backup-view" || action == "backup-regenerate") {
      open("backup-request");
      state.fields["regenerate"] = action == "backup-regenerate" ? "1" : "0";
    } else if (action == "backup-copy") {
      std::string codes;
      for (const auto& entry : state.backupCodes) {
        const auto value = entry.is_string() ? entry.get<std::string>() : string(entry, "code");
        if (!value.empty())
          codes += value + '\n';
      }
      if (!codes.empty()) {
        m_window.copy(std::move(codes));
        state.status = "Backup codes copied.";
      }
    } else if (action == "secret-copy") {
      if (!state.fields["secret"].empty())
        m_window.copy(state.fields["secret"]);
    } else if (action == "sms-enable")
      open("sms-enable");
    else if (action == "sms-disable")
      open("sms-disable");
    else if (action == "key-register")
      auth::openExternalUrl(api::userSettingsBrowserUrl(Section::SecurityKeys));
    else if (action.starts_with("key-rename:"))
      open("key-rename", action.substr(11));
    else if (action.starts_with("key-remove:"))
      open("key-remove", action.substr(11));
    else if (action.starts_with("session-remove:"))
      open("session-remove", action.substr(15));
    else if (action == "sessions-remove-all") {
      const auto& sessions = state.records[std::to_string(static_cast<int>(Section::Sessions)) + ':'];
      if (!sessions.contains("user_sessions") || !std::ranges::any_of(sessions["user_sessions"], [](const auto& item) {
            return item.is_object() && item.value("current_session", false);
          }))
        return true;
      open("sessions-remove-all");
    } else if (action.starts_with("app-remove:"))
      open("app-remove", action.substr(11));
    else if (action == "connection-add") {
      open("connection-add");
      state.fields["provider"] = "github";
    } else if (action.starts_with("connection-remove:") || action.starts_with("connection-refresh:")) {
      const bool refresh = action.starts_with("connection-refresh:");
      const auto suffix = action.substr(refresh ? 19 : 18);
      const auto divider = suffix.find(':');
      if (divider != std::string::npos) {
        const auto service = suffix.substr(0, divider), id = suffix.substr(divider + 1);
        if (refresh) {
          auto value = request(Section::ConnectionRefresh);
          value.resourceId = id;
          value.service = service;
          requestUserAccount(std::move(value));
        } else
          open("connection-remove", id, service);
      }
    } else if (action == "account-disable" || action == "account-delete")
      open(action);
    else if (action.starts_with("mfa-method:")) {
      const auto method = action.substr(11);
      if ((method == "totp" || method == "password")
          && std::ranges::find(state.mfaMethods, method) != state.mfaMethods.end()) {
        state.mfaMethod = method;
        state.fields.erase("code");
      }
    } else if (action == "discard-profile") {
      const auto next = state.fields["next_page"];
      state.draft.clear();
      state.profileSaves.clear();
      m_profileDirty = false;
      if (state.crop) {
        discardImage(state.crop->texture);
        state.crop.reset();
      }
      discardImage("user-profile-upload:avatar");
      discardImage("user-profile-upload:banner");
      closeUserAccountDialog();
      if (next == "close") {
        m_settingsOpen = false;
        resetUserAccountSettings();
      } else
        selectSettingsPage(next);
    } else if (action == "profile-reset") {
      state.draft.clear();
      state.profileSaves.clear();
      m_profileDirty = false;
      if (state.crop) {
        discardImage(state.crop->texture);
        state.crop.reset();
      }
      discardImage("user-profile-upload:avatar");
      discardImage("user-profile-upload:banner");
    } else if (action.starts_with("upload-"))
      beginUserProfileUpload(action.substr(7));
    else if (action == "remove-avatar" || action == "remove-banner") {
      if (!userAccountProfileReady(state))
        return true;
      state.draft[action.substr(7)] = nullptr;
      discardImage("user-profile-upload:" + action.substr(7));
      m_profileDirty = true;
    } else if (action.starts_with("crop-"))
      userProfileCropAction(action);
    else if (action == "profile-save") {
      if (!userAccountProfileReady(state)) {
        state.error = "Wait for your current profile details to finish loading before saving changes.";
        m_window.invalidate();
        return true;
      }
      Json account = Json::object(), profile = Json::object();
      for (const auto& [key, value] : state.draft.items()) {
        if (key == "theme_primary" || key == "theme_accent")
          continue;
        if (key == "accent_color") {
          const auto parsed = value.is_string() ? color(value.get<std::string>()) : std::nullopt;
          if (!parsed) {
            state.error = "Enter the banner color as #RRGGBB.";
            m_window.invalidate();
            return true;
          }
          profile[key] = *parsed;
        } else if (
            key == "global_name"
            || key == "avatar"
            || key == "nick"
            || key == "avatar_decoration_sku_id"
            || key == "nameplate_sku_id"
            || key == "collectibles"
        )
          account[key] = value;
        else
          profile[key] = value;
      }
      if (state.draft.contains("theme_primary") || state.draft.contains("theme_accent")) {
        const auto existing = state.profile.value("theme_colors", Json::array());
        Json colors = Json::array();
        for (int i = 0; i < 2; ++i) {
          const auto key = i ? "theme_accent" : "theme_primary";
          if (state.draft.contains(key)) {
            const auto value = color(state.draft[key].get<std::string>());
            if (!value) {
              state.error = "Enter both profile theme colors as #RRGGBB.";
              m_window.invalidate();
              return true;
            }
            colors.push_back(*value);
          } else if (existing.is_array() && existing.size() == 2 && existing[i].is_number_integer())
            colors.push_back(existing[i]);
          else {
            state.error = "Choose both profile theme colors before saving.";
            m_window.invalidate();
            return true;
          }
        }
        profile["theme_colors"] = std::move(colors);
      }
      state.profileSaves.clear();
      if (!state.guild.empty() && account.contains("nick") && !m_demo) {
        api::Channel synthetic;
        synthetic.overwrites.emplace();
        const auto permissions = api::channelPermissions(
            state.guild, m_account.id, m_roleDirectory.owner(state.guild), m_roleDirectory.allRoles(state.guild),
            m_roleDirectory.memberships(state.guild, m_account.id), synthetic
        );
        if (!api::permission::allows(permissions, api::permission::changeNickname)) {
          state.error = "Your current roles do not allow you to change your nickname in this server.";
          m_window.invalidate();
          return true;
        }
      }
      if (!state.guild.empty()
          && m_account.premiumType.value_or(0) != 2
          && (!profile.empty() || account.contains("avatar"))) {
        state.error = "Server profile images and custom profile details require Discord Nitro.";
        m_window.invalidate();
        return true;
      }
      for (const auto& [section, values] :
           {std::pair{state.guild.empty() ? Section::Account : Section::GuildMember, account},
            std::pair{state.guild.empty() ? Section::Profile : Section::GuildProfile, profile}}) {
        if (values.empty())
          continue;
        auto value = request(section, Verb::Update);
        value.guildId = state.guild;
        value.resourceId = m_account.id;
        value.values = values;
        state.profileSaves.push_back(std::move(value));
      }
      if (!state.profileSaves.empty()) {
        auto first = std::move(state.profileSaves.front());
        state.profileSaves.erase(state.profileSaves.begin());
        if (!requestUserAccount(std::move(first), "profile-save"))
          state.profileSaves.clear();
      }
    } else if (action == "dialog-submit") {
      auto value = request(Section::Account, Verb::Update);
      if (state.mfaRequest) {
        if (state.mfaMethod.empty() || state.fields["code"].empty()) {
          state.error = "Choose an available verification method and enter its code or password.";
          m_window.invalidate();
          return true;
        }
        value = *state.mfaRequest;
        value.mfaChallenge = state.mfaChallenge;
        value.mfaType = state.mfaMethod;
        value.mfaCode = state.fields["code"];
      } else if (state.dialog == "username") {
        value.values["username"] = state.fields["value"];
        value.password = state.fields["password"];
      } else if (state.dialog == "email-send-code")
        value = request(Section::EmailChange);
      else if (state.dialog == "email-code") {
        value = request(Section::EmailChange, Verb::Update);
        value.verificationCode = state.fields["code"];
      } else if (state.dialog == "email-new") {
        value.values["email"] = state.fields["value"];
        value.password = state.fields["password"];
      } else if (state.dialog == "password") {
        if (state.fields["new_password"] != state.fields["confirm_password"]) {
          state.error = "The new passwords do not match.";
          m_window.invalidate();
          return true;
        }
        if (api::utf8Length(state.fields["new_password"]) < 8) {
          state.error = "Choose a password with at least eight characters.";
          m_window.invalidate();
          return true;
        }
        value = request(Section::Password, Verb::Update);
        value.password = state.fields["password"];
        value.newPassword = state.fields["new_password"];
      } else if (state.dialog == "phone") {
        value = request(Section::Phone);
        value.values["phone"] = state.fields["phone"];
      } else if (state.dialog == "phone-code") {
        value = request(Section::PhoneVerification);
        value.values["phone"] = state.fields["phone"];
        value.verificationCode = state.fields["code"];
      } else if (state.dialog == "phone-attach") {
        value = request(Section::Phone, Verb::Update);
        value.values["phone"] = state.fields["phone"];
        value.password = state.fields["password"];
      } else if (state.dialog == "phone-remove") {
        value = request(Section::Phone, Verb::Delete);
        value.password = state.fields["password"];
      } else if (state.dialog == "totp-add") {
        value = request(Section::Authenticator);
        value.authenticatorSecret = state.fields["secret"];
        value.verificationCode = state.fields["code"];
      } else if (state.dialog == "totp-remove")
        value = request(Section::Authenticator, Verb::Delete);
      else if (state.dialog == "backup-request") {
        value = request(Section::BackupCodes);
        value.password = state.fields["password"];
      } else if (state.dialog == "backup-code") {
        value = request(Section::BackupCodes, Verb::Update);
        value.verificationCode = state.fields["code"];
        value.values["regenerate"] = state.fields["regenerate"] == "1";
      } else if (state.dialog == "sms-enable")
        value = request(Section::SmsAuthentication);
      else if (state.dialog == "sms-disable") {
        value = request(Section::SmsAuthentication, Verb::Delete);
        value.password = state.fields["password"];
      } else if (state.dialog == "key-rename") {
        value = request(Section::SecurityKeys, Verb::Update);
        value.values["name"] = state.fields["value"];
      } else if (state.dialog == "key-remove")
        value = request(Section::SecurityKeys, Verb::Delete);
      else if (state.dialog == "session-remove" || state.dialog == "sessions-remove-all") {
        value = request(Section::Sessions, Verb::Delete);
        value.resourceId.clear();
        value.values["session_id_hashes"] = Json::array();
        if (state.dialog == "session-remove")
          value.values["session_id_hashes"].push_back(state.resource);
        else {
          auto sessions = state.records[std::to_string(static_cast<int>(Section::Sessions)) + ':'];
          if (sessions.is_object() && sessions.contains("user_sessions"))
            sessions = sessions["user_sessions"];
          if (!sessions.is_array() || !std::ranges::any_of(sessions, [](const auto& item) {
                return item.is_object() && item.value("current_session", false);
              })) {
            state.error = "Wait for Discord to identify your current device before logging out other devices.";
            m_window.invalidate();
            return true;
          }
          if (sessions.is_array())
            for (const auto& item : sessions) {
              if (item.contains("current_session") && item["current_session"] == true)
                continue;
              const auto hash = string(item, "id_hash");
              if (!hash.empty())
                value.values["session_id_hashes"].push_back(hash);
            }
        }
      } else if (state.dialog == "app-remove")
        value = request(Section::AuthorizedApps, Verb::Delete);
      else if (state.dialog == "connection-remove")
        value = request(Section::Connections, Verb::Delete);
      else if (state.dialog == "connection-add") {
        value = request(Section::ConnectionAuthorize, Verb::Get);
        value.service = state.fields["provider"];
      } else if (state.dialog == "account-disable" || state.dialog == "account-delete") {
        const bool deleting = state.dialog == "account-delete";
        if (state.fields["confirmation"] != (deleting ? "DELETE" : "DISABLE")) {
          state.error = deleting ? "Type DELETE to confirm." : "Type DISABLE to confirm.";
          m_window.invalidate();
          return true;
        }
        value = request(deleting ? Section::DeleteAccount : Section::DisableAccount, Verb::Delete);
        value.values["confirm"] = true;
        value.password = state.fields["password"];
      } else
        return true;
      if (requestUserAccount(std::move(value), state.mfaRequest ? state.mfaCache : "dialog-action")) {
        state.fields.erase("password");
        state.fields.erase("new_password");
        state.fields.erase("confirm_password");
        state.fields.erase("code");
        for (const auto& [id, input] : m_inputs)
          if (id.starts_with("user-account:secret:")
              && !id.ends_with(":value")
              && !id.ends_with(":phone")
              && !id.ends_with(":secret"))
            input->setValue("");
      }
    }
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::userAccountDialogKey(const Key& key) {
    if (!userAccountDialogOpen())
      return false;
    if (key.symbol == XKB_KEY_Tab) {
      const auto cancel = m_widgets.find("user-account-dialog-button:dialog-cancel");
      if (cancel != m_widgets.end())
        (void)m_controlInput.cycleTabFocusInSubtree(cancel->second->parent(), key.shift);
      m_window.invalidate();
      return true;
    }
    if (key.symbol == XKB_KEY_Page_Up || key.symbol == XKB_KEY_Page_Down) {
      auto& state = m_userAccount;
      m_scrolling.stop(state.dialogScroll);
      state.dialogScroll =
          std::max(0.0F, state.dialogScroll + (key.symbol == XKB_KEY_Page_Down ? 1 : -1) * state.dialogHeight * .7F);
      m_window.invalidate();
      return true;
    }
    if (key.symbol == XKB_KEY_Escape) {
      if (!m_userAccount.busy)
        closeUserAccountDialog();
      m_window.invalidate();
      return true;
    }
    if (!key.shift
        && !key.control
        && !key.alt
        && !key.super
        && (key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter)) {
      // Retained buttons/selectors handle their own activation. Enter in a text
      // field submits the form; Enter on Cancel must only cancel.
      if (m_controlInput.focusedArea() && !m_focusedInput.starts_with("user-account:secret:"))
        return false;
      if (m_userAccount.dialog != "backup-codes" && m_userAccount.dialog != "discard-profile")
        userAccountAction("dialog-submit");
      return true;
    }
    return false;
  }
  bool ClientApplication::userAccountDialogScroll(float delta) {
    if (!userAccountDialogOpen())
      return false;
    const bool precise = m_window.preciseScroll();
    m_scrolling.scroll(
        m_userAccount.dialogScroll, ScrollMotion::distance(delta, precise),
        precise || !m_animateWheel || m_preferences.reducedMotion
    );
    m_window.invalidate();
    return true;
  }
  bool ClientApplication::userAccountDialogClick(float x, float y) {
    if (!userAccountDialogOpen())
      return false;
    auto& state = m_userAccount;
    if (x < state.dialogX
        || x >= state.dialogX + state.dialogWidth
        || y < state.dialogY
        || y >= state.dialogY + state.dialogHeight)
      return true;
    (void)m_controlInput.pointerButton(
        x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
    );
    m_window.invalidate();
    return true;
  }

  void ClientApplication::drawUserAccountDialog(Node& root, float w, float h) {
    if (!userAccountDialogOpen())
      return;
    auto& state = m_userAccount;
    const auto kind = state.mfaRequest ? std::string("mfa") : state.dialog;
    std::string title, description, submit = "Save";
    bool dangerous = false, password = false, value = false, code = false;
    if (state.mfaRequest) {
      title = "Verify your identity";
      description = "Complete Discord's verification for the action you selected. Authenticator codes and backup codes "
                    "use the same field.";
      submit = "Verify";
      code = !state.mfaMethod.empty();
    } else if (kind == "username") {
      title = "Change username";
      description = "Usernames contain 2–32 lowercase letters, numbers, underscores or periods. Consecutive periods "
                    "are not allowed. Discord applies its change limit.";
      password = value = true;
    } else if (kind == "email-send-code") {
      title = "Change email address";
      description = "First, Discord will send a verification code to your current email address.";
      submit = "Send code";
    } else if (kind == "email-code") {
      title = "Verify your current email";
      description =
          "Enter the code Discord sent to your current email address. You can choose a new address after verification.";
      code = true;
      submit = "Verify";
    } else if (kind == "email-new") {
      title = "Choose a new email address";
      description = "Enter your new email address and current password. Check the new inbox for Discord's verification "
                    "email afterward.";
      password = value = true;
    } else if (kind == "password") {
      title = "Change password";
      description = "Enter your current password and choose a new password with at least eight characters.";
      password = true;
    } else if (kind == "phone") {
      title = "Add a phone number";
      description = "Enter your number with its country code. Discord will send an SMS verification code.";
      submit = "Send SMS";
    } else if (kind == "phone-code") {
      title = "Verify your phone number";
      description = "Enter the code sent to " + state.fields["phone"] + ".";
      code = true;
      submit = "Verify";
    } else if (kind == "phone-attach") {
      title = "Confirm phone number";
      description = "Your number is verified. Enter your password to attach it to your Discord account.";
      password = true;
    } else if (kind == "phone-remove") {
      title = "Remove phone number?";
      description = "Remove your phone number from this Discord account.";
      password = dangerous = true;
      submit = "Remove";
    } else if (kind == "totp-add") {
      title = "Set up an authenticator app";
      description = "Copy this setup key into your authenticator app, then enter the six-digit code it generates. The "
                    "key stays in memory until this dialog closes.";
      code = true;
      submit = "Activate";
    } else if (kind == "totp-remove") {
      title = "Remove authenticator app?";
      description = "Stop requiring this authenticator's codes when you sign in. Discord may require another "
                    "verification method.";
      dangerous = true;
      submit = "Remove";
    } else if (kind == "backup-request") {
      title = state.fields["regenerate"] == "1" ? "Generate new backup codes" : "View backup codes";
      description = "Enter your password. Discord will then send an email verification code before showing your login "
                    "backup codes.";
      password = true;
      submit = "Continue";
    } else if (kind == "backup-code") {
      title = "Verify backup code access";
      description =
          "Enter Discord's email verification code. Generating new backup codes invalidates the previous set.";
      code = true;
      submit = state.fields["regenerate"] == "1" ? "Generate codes" : "Show codes";
    } else if (kind == "backup-codes") {
      title = "Your backup codes";
      description = "Each unused code can sign in once. Keep these codes somewhere private. Closing this dialog "
                    "removes this copy from memory.";
    } else if (kind == "sms-enable") {
      title = "Enable SMS backup?";
      description = "Allow Discord to send backup authentication codes to the verified phone number on your account.";
      submit = "Enable";
    } else if (kind == "sms-disable") {
      title = "Disable SMS backup?";
      description = "Stop using text messages as a backup authentication method.";
      password = true;
      dangerous = true;
      submit = "Disable";
    } else if (kind == "key-rename") {
      title = "Rename security key";
      description = "Choose a name that helps you recognize this passkey or security key.";
      value = true;
    } else if (kind == "key-remove") {
      title = "Remove security key?";
      description =
          "This key will no longer authenticate your Discord account. Keep access to another authentication method.";
      dangerous = true;
      submit = "Remove key";
    } else if (kind == "session-remove" || kind == "sessions-remove-all") {
      title = kind == "session-remove" ? "Log out this device?" : "Log out other devices?";
      description = "The selected sessions will need to sign in again. Discord may ask you to verify this action.";
      dangerous = true;
      submit = "Log out";
    } else if (kind == "app-remove") {
      title = "Deauthorize application?";
      description = "Remove this application's access to your account. You can authorize it again later.";
      dangerous = true;
      submit = "Deauthorize";
    } else if (kind == "connection-remove") {
      title = "Disconnect account?";
      description = "Remove this linked account from your Discord profile and stop sharing its activity.";
      dangerous = true;
      submit = "Disconnect";
    } else if (kind == "connection-add") {
      title = "Connect an account";
      description = "Choose a provider. Your browser will handle sign-in and authorization. Return here afterward and "
                    "refresh connections.";
      submit = "Open browser";
    } else if (kind == "discard-profile") {
      title = "Discard profile changes?";
      description = "Your unsaved profile changes will be lost.";
      dangerous = true;
      submit = "Discard";
    } else {
      const bool deleting = kind == "account-delete";
      title = deleting ? "Delete your account?" : "Disable your account?";
      description = deleting ? "Schedule permanent account deletion. Transfer every server you own first. Type DELETE "
                               "and enter your password to continue."
                             : "Your account can be restored by signing in again. Transfer servers you own first; "
                               "subscriptions keep renewing unless canceled. Type DISABLE and enter your password.";
      password = dangerous = true;
      submit = deleting ? "Delete account" : "Disable account";
    }
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, rgba(0, 0, 0, .68F));
    const float width = std::min(490.0F, std::max(220.0F, w - Style::spaceGroup));
    auto content = std::make_unique<Node>();
    auto& body = *content;
    float y = 0;
    y += label(
             body, 0, y, width - Style::spaceGroup, title, Style::fontSizeDialog, theme::foreground,
             FontWeight::SemiBold, 0
         )
        + 12;
    y += label(
             body, 0, y, width - Style::spaceGroup, description, Style::fontSizeDescription, theme::muted,
             FontWeight::Normal, 0
         )
        + 14;
    const auto button = [&](const std::string& caption, const std::string& action, float bw,
                            ButtonVariant variant = ButtonVariant::Default) {
      const auto id = "user-account-dialog-button:" + action;
      std::unique_ptr<Node> saved;
      if (const auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
        saved = std::move(at->second);
        m_detachedInputs.erase(at);
      }
      auto control =
          saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
      m_widgets[id] = control.get();
      control->setText(caption);
      control->setFontSize(Style::fontSizeDescription);
      control->setControlHeight(26);
      control->setRadius(Style::scaledRadius(Style::controlRadius));
      control->setFrameSize(std::min(bw, width - Style::spaceGroup), 26);
      control->setPosition(0, y);
      control->setVariant(variant);
      control->setEnabled(!state.busy);
      control->setOnClick([this, action] { userAccountAction(action); });
      body.addChild(std::move(control));
      y += (Style::space2Xl + Style::spaceXs);
    };
    const auto field = [&](const std::string& name, const std::string& caption, bool secret = false) {
      y += label(
               body, 0, y, width - Style::spaceGroup, caption, Style::fontSizeDescription, theme::foreground,
               FontWeight::Medium
           )
          + 5;
      const auto id = "user-account:secret:" + name;
      auto& input = inputControl(body, 0, y, width - 48, 32, id, state.fields[name], {}, false, secret);
      input.setEnabled(!state.busy);
      input.setPasswordMode(secret);
      if (!m_controlInput.focusedArea() && m_focusedInput.empty() && name != "secret")
        m_focusedInput = id;
      input.setOnChange([this, name](const std::string& input) {
        if (input.size() <= 1024)
          m_userAccount.fields[name] = input;
        m_window.invalidate();
      });
      input.setOnSubmit([this](const std::string&) { userAccountAction("dialog-submit"); });
      y += (Style::spaceSection + Style::spaceXs);
    };
    if (state.mfaRequest) {
      for (const auto& method : state.mfaMethods)
        if (method == "totp" || method == "password")
          button(
              method == "totp" ? "Authenticator or backup code" : "Account password", "mfa-method:" + method, 220,
              state.mfaMethod == method ? ButtonVariant::Primary : ButtonVariant::Default
          );
      if (state.mfaMethod.empty()) {
        y += label(
                 body, 0, y, width - Style::spaceGroup,
                 "Discord requires an SMS or security-key method. Complete this action in Discord's browser settings.",
                 Style::fontSizeDescription, theme::muted, FontWeight::Normal, 0
             )
            + 12;
        button("Open Discord", "key-register", 128);
      }
    }
    if (kind == "totp-add") {
      field("secret", "Authenticator setup key", true);
      button("Copy setup key", "secret-copy", 128);
    }
    if (value)
      field("value", kind == "email-new" ? "New email address" : kind == "key-rename" ? "Key name" : "Username");
    if (kind == "phone")
      field("phone", "Phone number with country code");
    if (password)
      field("password", "Current password", true);
    if (kind == "password") {
      field("new_password", "New password", true);
      field("confirm_password", "Confirm new password", true);
    }
    if (code)
      field("code", state.mfaRequest && state.mfaMethod == "password" ? "Account password" : "Verification code", true);
    if (kind == "phone-code")
      button("Resend SMS", "phone-resend", 110);
    if (kind == "account-delete" || kind == "account-disable")
      field("confirmation", kind == "account-delete" ? "Type DELETE" : "Type DISABLE");
    if (kind == "connection-add") {
      const std::vector<std::string> providers{"github", "spotify", "steam",       "twitch",    "youtube",
                                               "reddit", "xbox",    "playstation", "battlenet", "epicgames",
                                               "roblox", "twitter", "bluesky",     "tiktok",    "crunchyroll"};
      const std::vector<std::string> names{"GitHub",     "Spotify",    "Steam",      "Twitch",
                                           "YouTube",    "Reddit",     "Xbox",       "PlayStation Network",
                                           "Battle.net", "Epic Games", "Roblox",     "X",
                                           "Bluesky",    "TikTok",     "Crunchyroll"};
      const std::string id = "user-account-provider-select";
      std::unique_ptr<Node> saved;
      if (const auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
        saved = std::move(at->second);
        m_detachedInputs.erase(at);
      }
      auto control =
          saved ? std::unique_ptr<Select>(static_cast<Select*>(saved.release())) : std::make_unique<Select>();
      m_widgets[id] = control.get();
      control->setOptions(names);
      control->setFontSize(Style::fontSizeDescription);
      control->setControlHeight(30);
      control->setPosition(0, y);
      control->setFrameSize(width - Style::spaceGroup, 30);
      control->setEnabled(!state.busy);
      const auto selected = std::ranges::find(providers, state.fields["provider"]);
      control->setSelectedIndexSilently(selected == providers.end() ? 0 : selected - providers.begin());
      control->setOnSelectionChanged([this, providers](std::size_t index, std::string_view) {
        if (index < providers.size())
          m_userAccount.fields["provider"] = providers[index];
        m_window.invalidate();
      });
      body.addChild(std::move(control));
      y += (Style::spaceSection + Style::spaceXxs);
    }
    if (kind == "backup-codes") {
      for (const auto& entry : state.backupCodes) {
        const auto value = entry.is_string() ? entry.get<std::string>() : string(entry, "code");
        const auto used = entry.is_object() && entry.contains("consumed") && entry["consumed"] == true;
        y += label(
                 body, 0, y, width - Style::spaceGroup, value + (used ? " · Used" : ""), Style::fontSizeBody,
                 used ? theme::muted : theme::foreground
             )
            + 4;
      }
      y += Style::spaceSm;
      button("Copy codes", "backup-copy", 112);
      button("Generate new codes", "backup-regenerate", 160);
    }
    if (!state.error.empty())
      y += label(
               body, 0, y, width - Style::spaceGroup, state.error, Style::fontSizeDescription, theme::danger,
               FontWeight::Normal, 0
           )
          + 10;
    if (state.busy)
      y +=
          label(body, 0, y, width - Style::spaceGroup, "Waiting for Discord…", Style::fontSizeDescription, theme::muted)
          + 8;
    const bool canSubmit = kind != "backup-codes" && (!state.mfaRequest || !state.mfaMethod.empty());
    const float footerY = y + Style::spaceCompact, contentWidth = width - 48;
    y = footerY;
    button(kind == "backup-codes" ? "Done" : "Cancel", "dialog-cancel", 90);
    auto* cancel = m_widgets.at("user-account-dialog-button:dialog-cancel");
    cancel->setPosition(std::max(0.0F, contentWidth - (canSubmit && contentWidth >= 248 ? 248 : 90)), footerY);
    if (canSubmit) {
      const auto action = kind == "discard-profile" ? "discard-profile" : "dialog-submit";
      if (contentWidth >= 248)
        y = footerY;
      button(
          state.busy ? "Working…" : submit, action, 150, dangerous ? ButtonVariant::Destructive : ButtonVariant::Primary
      );
      auto* save = m_widgets.at("user-account-dialog-button:" + std::string(action));
      save->setPosition(std::max(0.0F, contentWidth - 150), save->y());
    }
    const float viewport = std::max(80.0F, std::min(y, h - 136)), height = viewport + 48;
    state.dialogWidth = width;
    state.dialogHeight = height;
    state.dialogX = (w - width) / 2;
    state.dialogY = std::max(44.0F, (h - height) / 2);
    rectangle(
        root, state.dialogX - Style::borderWidth, state.dialogY - Style::borderWidth, width + Style::spaceXxs,
        height + Style::spaceXxs, theme::raised, 11
    );
    rectangle(root, state.dialogX, state.dialogY, width, height, theme::panel, 10);
    auto clip = std::make_unique<Node>();
    clip->setPosition(state.dialogX + Style::spaceXl, state.dialogY + Style::spaceXl);
    clip->setSize(width - Style::spaceGroup, viewport);
    clip->setClipChildren(true);
    m_scrolling.limit(state.dialogScroll, std::max(0.0F, y - viewport));
    content->setPosition(0, -state.dialogScroll);
    clip->addChild(std::move(content));
    root.addChild(std::move(clip));
    if (y > viewport)
      settingsScrollbar(
          root, "user-account-dialog-scrollbar",
          {state.dialogX + width - 10, state.dialogY + Style::spaceXl, 6, viewport}, y, state.dialogScroll
      );
  }
} // namespace app
