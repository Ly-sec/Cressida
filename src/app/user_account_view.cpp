#include "api/channel_features.h"
#include "app/client_application.h"
#include "app/theme.h"
#include "app/user_settings_form.h"
#include "ui/controls/input.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

namespace app {
  namespace {
    using Json = nlohmann::json;
    using Section = api::UserSettingsSection;
    using Verb = api::UserSettingsVerb;
    std::string text(const Json& value, const char* key, std::string fallback = {}) {
      const auto at = value.find(key);
      return at != value.end() && at->is_string() ? at->get<std::string>() : fallback;
    }
    bool flag(const Json& value, const char* key, bool fallback = false) {
      const auto at = value.find(key);
      return at != value.end() && at->is_boolean()       ? at->get<bool>()
          : at != value.end() && at->is_number_integer() ? *at != 0
                                                         : fallback;
    }
    Json rows(const Json& value, const char* key = "") {
      if (value.is_array())
        return value;
      const auto at = value.find(key);
      return at != value.end() && at->is_array() ? *at : Json::array();
    }
    std::string cacheKey(const api::UserSettingsRequest& request) {
      return std::to_string(static_cast<int>(request.section)) + ':' + request.guildId;
    }
    Json flattenProfile(Json value) {
      if (!value.is_object())
        return Json::object();
      Json result = value;
      for (const auto* key : {"user", "user_profile", "guild_member", "guild_member_profile"})
        if (const auto at = value.find(key); at != value.end() && at->is_object())
          for (const auto& [field, content] : at->items()) {
            if (!result.contains(field)
                || (std::string_view(key) == "guild_member" && (field == "nick" || field == "avatar")))
              result[field] = content;
            if (field == "collectibles" && content.is_object() && content.contains("nameplate"))
              result["nameplate"] = content["nameplate"];
          }
      if (value.contains("collectibles")
          && value["collectibles"].is_object()
          && value["collectibles"].contains("nameplate"))
        result["nameplate"] = value["collectibles"]["nameplate"];
      return result;
    }
    std::string masked(const std::string& value, bool email) {
      if (value.empty())
        return "Not added";
      if (email) {
        const auto at = value.find('@');
        return at == std::string::npos ? "••••••••" : "••••••••" + value.substr(at);
      }
      return "••••••" + value.substr(value.size() > 4 ? value.size() - 4 : 0);
    }
    std::string connectionName(std::string type) {
      if (type == "xbox")
        return "Xbox";
      if (type == "playstation")
        return "PlayStation Network";
      if (type == "spotify")
        return "Spotify";
      if (type == "github")
        return "GitHub";
      if (type == "youtube")
        return "YouTube";
      if (!type.empty())
        type.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(type.front())));
      return type;
    }
    std::string scopeDescription(const std::string& scope) {
      const std::map<std::string, std::string> descriptions{
          {"identify", "View your username and avatar"},
          {"email", "View your email address"},
          {"connections", "View your connected accounts"},
          {"guilds", "View the servers you belong to"},
          {"guilds.join", "Add you to servers"},
          {"gdm.join", "Add you to group direct messages"},
          {"applications.commands", "Use application commands"},
          {"applications.commands.permissions.update", "Manage application command permissions"},
          {"activities.read", "View your activity"},
          {"activities.write", "Update your activity"},
          {"relationships.read", "View your friends and relationships"},
          {"role_connections.write", "Update your linked-role details"},
          {"rpc", "Access your local Discord client"},
          {"messages.read", "Read your messages"}
      };
      const auto at = descriptions.find(scope);
      return at == descriptions.end() ? "Additional permission: " + scope : at->second;
    }
  } // namespace

  bool userAccountProfileReady(const UserAccountState& state) {
    const auto profile =
        std::to_string(
            static_cast<int>(
                state.guild.empty() ? api::UserSettingsSection::Profile : api::UserSettingsSection::GuildProfile
            )
        )
        + ':'
        + state.guild;
    const auto member = std::to_string(static_cast<int>(api::UserSettingsSection::GuildMember)) + ':' + state.guild;
    return state.loaded.contains(profile)
        && !state.loading.contains(profile)
        && !state.errors.contains(profile)
        && (state.guild.empty()
            || (state.loaded.contains(member) && !state.loading.contains(member) && !state.errors.contains(member)));
  }

  void ClientApplication::resetUserAccountSettings() {
    auto& state = m_userAccount;
    closeUserAccountDialog();
    if (!m_demo)
      m_client.clearUserSettingsSecrets();
    const auto serial = state.serial;
    const auto generation = state.generation + 1;
    if (state.uploadRequest)
      m_filePicker.cancel();
    if (state.crop)
      discardImage(state.crop->texture);
    discardImagesWithPrefix("user-profile-upload:");
    state = {};
    state.serial = serial;
    state.generation = generation;
    m_profileDirty = false;
    // Retained Inputs own undo buffers as well as their visible text.
    for (const auto& [id, input] : m_inputs)
      if (id.starts_with("user-account:"))
        input->setValue("");
  }

  bool ClientApplication::requestUserAccount(api::UserSettingsRequest request, std::string cache) {
    auto& state = m_userAccount;
    if (!m_settingsOpen || (!m_demo && !m_connected))
      return false;
    request.request = (std::uint64_t{1} << 61) | ++state.serial;
    if (cache.empty())
      cache = cacheKey(request);
    const bool valid = request.mfaChallenge ? (request.mfaType == "totp" || request.mfaType == "password")
            && !request.mfaCode.empty()
            && request.mfaCode.size() <= 1024
                                            : api::userSettingsHttpRequest(request).has_value();
    if (!valid || (!m_demo && !m_client.userSettings(request))) {
      state.error = "This action could not be started. Try again.";
      state.errors[cache] = state.error;
      m_window.invalidate();
      return false;
    }
    state.pending.emplace(
        request.request, UserAccountPending{api::publicUserSettingsRequest(request), state.generation, cache}
    );
    if (request.verb == Verb::Get && cache != "dialog-action" && cache != "mfa-action")
      state.loading.insert(cache);
    else {
      state.busy = true;
      state.saveRequest = request.request;
    }
    state.error.clear();
    state.errors.erase(cache);
    state.status.clear();
    m_window.invalidate();
    return true;
  }

  void ClientApplication::loadUserAccountSettings(const std::string& page, bool force) {
    auto& state = m_userAccount;
    const bool owned =
        page == "account" || page == "profile" || page == "devices" || page == "authorized" || page == "connections";
    if (state.page != page) {
      if (state.busy) {
        m_settingsPage = state.page;
        state.error = "Wait for the current account change to finish before changing pages.";
        m_window.invalidate();
        return;
      }
      if ((!state.draft.empty() || state.crop) && state.page == "profile") {
        state.fields["next_page"] = page;
        m_settingsPage = state.page;
        state.dialog = "discard-profile";
        ++m_dialogGeneration;
        m_window.invalidate();
        return;
      }
      closeUserAccountDialog();
      state.page = page;
      state.status.clear();
      state.error.clear();
      force = true;
    }
    if (!owned)
      return;
    if (m_demo) {
      if (!state.loaded.contains("demo"))
        prepareUserAccountDemo();
      return;
    }
    const auto load = [&](Section section) {
      api::UserSettingsRequest request;
      request.section = section;
      if (section == Section::Profile || section == Section::GuildProfile || section == Section::GuildMember)
        request.resourceId = m_account.id;
      if (section == Section::GuildProfile || section == Section::GuildMember)
        request.guildId = state.guild;
      const auto key = cacheKey(request);
      if ((section == Section::Profile || section == Section::GuildProfile) && state.records.contains(key))
        state.profile = flattenProfile(state.records[key]);
      if (!state.loading.contains(key) && (force || !state.loaded.contains(key)))
        requestUserAccount(std::move(request), key);
    };
    if (page == "account") {
      load(Section::Account);
      load(Section::SecurityKeys);
    } else if (page == "profile") {
      load(state.guild.empty() ? Section::Profile : Section::GuildProfile);
      if (!state.guild.empty())
        load(Section::GuildMember);
      load(Section::Collectibles);
    } else if (page == "devices")
      load(Section::Sessions);
    else if (page == "authorized")
      load(Section::AuthorizedApps);
    else
      load(Section::Connections);
  }

  bool ClientApplication::userAccountResult(const api::Event& event) {
    if (!event.userSettingsRequest)
      return false;
    auto& state = m_userAccount;
    const auto& request = *event.userSettingsRequest;
    const auto pending = state.pending.find(request.request);
    if (pending == state.pending.end())
      return false;
    const auto identity = pending->second;
    const auto& expected = identity.request;
    if (request.section != expected.section
        || request.verb != expected.verb
        || request.resourceId != expected.resourceId
        || request.guildId != expected.guildId
        || request.service != expected.service)
      return true;
    state.pending.erase(pending);
    state.loading.erase(identity.cache);
    if (identity.generation != state.generation)
      return true;
    if (state.saveRequest == request.request) {
      state.busy = false;
      state.saveRequest = 0;
    }
    if (event.userSettingsMfaChallenge) {
      state.mfaRequest = expected;
      state.mfaCache = identity.cache;
      state.mfaChallenge = event.userSettingsMfaChallenge;
      state.mfaMethods = event.userSettingsMfaMethods;
      state.mfaMethod = std::ranges::find(state.mfaMethods, "totp") != state.mfaMethods.end() ? "totp"
          : std::ranges::find(state.mfaMethods, "password") != state.mfaMethods.end()         ? "password"
                                                                                              : "";
      std::erase_if(state.fields, [](const auto& field) {
        return field.first != "phone"
            && field.first != "regenerate"
            && field.first != "value"
            && field.first != "next_page";
      });
      for (const auto& [id, input] : m_inputs)
        if (id.starts_with("user-account:secret:"))
          input->setValue("");
      state.error.clear();
      ++m_dialogGeneration;
      m_window.invalidate();
      return true;
    }
    if (!event.error.empty()) {
      state.error = event.error;
      state.errors[identity.cache] = event.error;
      state.profileSaves.clear();
      state.fields.erase("password");
      state.fields.erase("new_password");
      state.fields.erase("confirm_password");
      for (const auto& [id, input] : m_inputs)
        if (id.starts_with("user-account:secret:"))
          input->setValue("");
      m_window.invalidate();
      return true;
    }
    if (request.section == Section::ConnectionAuthorize) {
      const auto url = text(event.userSettings, "url");
      if (!url.empty())
        auth::openExternalUrl(url);
      closeUserAccountDialog();
      state.status = "Complete authorization in your browser, then refresh connections.";
    } else if (identity.cache == "profile-save") {
      for (const auto& [field, value] : expected.values.items()) {
        if (field == "collectibles" && value.is_object()) {
          if (value.contains("nameplate"))
            state.profile["nameplate"] = value["nameplate"];
        } else if (field == "nameplate_sku_id")
          state.profile["nameplate"] = value.is_null() ? Json(nullptr) : Json{{"sku_id", value}};
        else if (field == "avatar_decoration_sku_id")
          state.profile["avatar_decoration_data"] = value.is_null() ? Json(nullptr) : Json{{"sku_id", value}};
        else
          state.profile[field] = value;
        state.draft.erase(field);
        if (field == "theme_colors") {
          state.draft.erase("theme_primary");
          state.draft.erase("theme_accent");
        }
      }
      state.mfaRequest.reset();
      state.mfaChallenge = 0;
      state.mfaMethods.clear();
      state.mfaMethod.clear();
      if (!state.profileSaves.empty()) {
        auto next = std::move(state.profileSaves.front());
        state.profileSaves.erase(state.profileSaves.begin());
        if (!requestUserAccount(std::move(next), "profile-save"))
          state.profileSaves.clear();
      } else {
        closeUserAccountDialog();
        m_profileDirty = !state.draft.empty();
        state.status = "Profile saved.";
        loadUserAccountSettings("profile", true);
      }
    } else if (request.verb == Verb::Get) {
      state.records[identity.cache] = event.userSettings;
      state.loaded.insert(identity.cache);
      state.errors.erase(identity.cache);
      if (request.section == Section::Profile || request.section == Section::GuildProfile) {
        state.profile = flattenProfile(event.userSettings);
        if (!state.guild.empty()) {
          const auto memberKey = std::to_string(static_cast<int>(Section::GuildMember)) + ':' + state.guild;
          if (state.records.contains(memberKey)) {
            const auto member = flattenProfile(state.records[memberKey]);
            for (const auto* field : {"nick", "avatar", "avatar_decoration_data", "nameplate"})
              if (member.contains(field))
                state.profile[field] = member[field];
          }
        }
        // Reads update the baseline without replacing text the user has edited.
      }
      if (request.section == Section::GuildMember && request.guildId == state.guild) {
        const auto member = flattenProfile(event.userSettings);
        for (const auto* field : {"nick", "avatar", "avatar_decoration_data", "nameplate"})
          if (member.contains(field))
            state.profile[field] = member[field];
      }
    } else if (request.section == Section::BackupCodes && request.verb == Verb::Create) {
      if (flag(event.userSettings, "challenge_ready")) {
        state.dialog = "backup-code";
        state.error.clear();
        ++m_dialogGeneration;
      } else
        state.error = "Discord did not start the backup-code verification. Try again.";
    } else if (request.section == Section::EmailChange) {
      state.dialog = request.verb == Verb::Create ? "email-code" : "email-new";
      state.error.clear();
      ++m_dialogGeneration;
    } else if (request.section == Section::PhoneVerification) {
      state.dialog = "phone-attach";
      state.error.clear();
      ++m_dialogGeneration;
    } else if (request.section == Section::PhoneResend) {
      state.status = "A new SMS verification code was sent.";
    } else if (request.section == Section::BackupCodes || request.section == Section::Authenticator) {
      const auto codes = rows(event.userSettings, "backup_codes");
      closeUserAccountDialog();
      if (!codes.empty()) {
        state.backupCodes = codes;
        state.dialog = "backup-codes";
        ++m_dialogGeneration;
      }
      state.status =
          request.section == Section::Authenticator ? "Authenticator settings updated." : "Backup codes loaded.";
      state.loaded.erase(std::to_string(static_cast<int>(Section::Account)) + ':');
      loadUserAccountSettings("account", true);
    } else if (request.section == Section::Phone && request.verb == Verb::Create) {
      state.dialog = "phone-code";
      state.error.clear();
      ++m_dialogGeneration;
    } else if (request.section == Section::PasswordRecovery) {
      state.status = "Password reset instructions have been sent to your account email.";
    } else if (request.section == Section::EmailVerification) {
      state.status = "Verification instructions have been sent to your email.";
      closeUserAccountDialog();
    } else if (request.section == Section::DisableAccount || request.section == Section::DeleteAccount) {
      closeUserAccountDialog();
      if (!m_demo)
        activate({0, 0, 0, 0, Action::SignOut, {}});
      return true;
    } else {
      closeUserAccountDialog();
      state.status = "Changes saved.";
      if (request.section == Section::Profile || request.section == Section::GuildProfile) {
        state.draft.clear();
        m_profileDirty = false;
      }
      // Refresh only the affected page after an acknowledged explicit mutation.
      loadUserAccountSettings(state.page, true);
    }
    if (request.section == Section::Account
        || request.section == Section::Profile
        || request.section == Section::Password) {
      const auto account = flattenProfile(event.userSettings);
      const auto incomingId = text(account, "id");
      const bool self = incomingId.empty() ? request.resourceId.empty() || request.resourceId == m_account.id
                                           : incomingId == m_account.id;
      if (self && !account.empty()) {
        Json merged{
            {"id", m_account.id},
            {"username", m_account.username},
            {"global_name", m_account.displayName},
            {"bio", m_account.bio},
            {"pronouns", m_account.pronouns}
        };
        if (m_account.premiumType)
          merged["premium_type"] = *m_account.premiumType;
        merged.update(account);
        auto parsed = api::parseAccount(merged);
        if (!account.contains("avatar"))
          parsed.avatar = m_account.avatar;
        if (!account.contains("primary_guild"))
          parsed.guildTag = m_account.guildTag;
        m_account = std::move(parsed);
        auto& cached = state.records[std::to_string(static_cast<int>(Section::Account)) + ':'];
        if (!cached.is_object())
          cached = Json::object();
        cached.update(account);
        if (state.draft.empty())
          resetProfile();
      }
    }
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::drawUserAccountSettings(UserSettingsForm& form) {
    auto& state = m_userAccount;
    const auto page = m_settingsPage;
    if (page != "account" && page != "profile" && page != "devices" && page != "authorized" && page != "connections")
      return false;
    const auto section = page == "account" ? Section::Account
        : page == "profile"                ? (state.guild.empty() ? Section::Profile : Section::GuildProfile)
        : page == "devices"                ? Section::Sessions
        : page == "authorized"             ? Section::AuthorizedApps
                                           : Section::Connections;
    api::UserSettingsRequest selected;
    selected.section = section;
    if (section == Section::GuildProfile)
      selected.guildId = state.guild;
    const auto key = cacheKey(selected);
    const auto& data = state.records[key];
    if (state.loading.contains(key))
      form.row("Loading", "Your current account settings are being loaded.");
    if (const auto error = state.errors.find(key); error != state.errors.end()) {
      form.rowButton(form.actionRow("Could not load account settings", error->second), "Retry", "account-reload");
    }
    if (!state.status.empty())
      form.row("Saved", state.status);
    if (!state.error.empty() && !userAccountDialogOpen())
      form.row("Could not save", state.error);
    const auto action = [&](const std::string& title, const std::string& description, const std::string& caption,
                            const std::string& id, ButtonVariant variant = ButtonVariant::Default,
                            bool enabled = true) {
      form.rowButton(form.actionRow(title, description), caption, id, variant, 128, enabled && !state.busy);
    };
    if (page == "account") {
      form.section("Account identity");
      action("Display name", m_account.name, "Edit profile", "profile-open");
      action("Username", '@' + m_account.username, "Edit", "edit-username");
      const auto email = text(data, "email"), phone = text(data, "phone");
      action(
          "Email",
          (state.revealEmail ? email : masked(email, true))
              + (flag(data, "verified") ? " · Verified" : " · Not verified"),
          "Edit", "edit-email"
      );
      form.button(state.revealEmail ? "Hide email" : "Show email", "reveal-email", ButtonVariant::Ghost, 110);
      if (!email.empty() && !flag(data, "verified"))
        form.button("Resend verification", "email-verify", ButtonVariant::Default, 154);
      action(
          "Phone number", state.revealPhone ? phone : masked(phone, false), phone.empty() ? "Add" : "Remove",
          phone.empty() ? "phone-add" : "phone-remove"
      );
      if (!phone.empty())
        form.button(state.revealPhone ? "Hide number" : "Show number", "reveal-phone", ButtonVariant::Ghost, 110);
      form.row("Account ID", m_account.id);
      form.section("Password & authentication");
      action("Password", "Change the password used to sign in to Discord.", "Change", "edit-password");
      action(
          "Password recovery", "Send Discord's password reset instructions to your account email.", "Send reset email",
          "password-recover", ButtonVariant::Default, !email.empty()
      );
      const bool mfaEnabled = flag(data, "mfa_enabled"), enabled = flag(data, "mfa_totp");
      const bool known = data.contains("mfa_totp") || !mfaEnabled;
      action(
          "Authenticator app",
          !known        ? "Reload after Discord has delivered your current authentication methods."
              : enabled ? "An authenticator app is enabled for this account."
                        : "Protect your account with codes generated by your authenticator app.",
          !known        ? "Reload"
              : enabled ? "Remove"
                        : "Enable",
          !known        ? "account-reload"
              : enabled ? "totp-remove"
                        : "totp-add"
      );
      if (mfaEnabled) {
        action(
            "Backup codes", "View your one-time login codes or generate a new set. Keep them private.", "View codes",
            "backup-view"
        );
        action(
            "SMS backup",
            phone.empty() ? "Add a verified phone number before enabling SMS backup."
                : !enabled && !flag(data, "mfa_sms")
                ? "Enable an authenticator app before adding SMS as a backup."
                : "Receive backup authentication codes on your verified phone number.",
            flag(data, "mfa_sms") ? "Disable" : "Enable", flag(data, "mfa_sms") ? "sms-disable" : "sms-enable",
            ButtonVariant::Default, !phone.empty() && (enabled || flag(data, "mfa_sms"))
        );
      }
      form.section("Security keys");
      const auto keys =
          rows(state.records[std::to_string(static_cast<int>(Section::SecurityKeys)) + ':'], "credentials");
      for (const auto& item : keys) {
        const auto id = text(item, "id");
        action(
            text(item, "name", "Security key"), "Passkey or physical security key registered with Discord.", "Rename",
            "key-rename:" + id
        );
        form.button("Remove key", "key-remove:" + id, ButtonVariant::Destructive, 116, !state.busy);
      }
      action(
          "Register a security key",
          "Your browser and operating system will guide you through registering a passkey or security key.",
          "Open Discord", "key-register"
      );
      form.section("Danger zone");
      action(
          "Disable account",
          "Pause your account until you sign in again. Transfer servers you own first. Existing subscriptions continue "
          "until canceled.",
          "Disable", "account-disable", ButtonVariant::Destructive
      );
      action(
          "Delete account",
          "Schedule your Discord account for permanent deletion. Transfer servers you own before continuing.", "Delete",
          "account-delete", ButtonVariant::Destructive
      );
    } else if (page == "profile") {
      form.section("Profile identity");
      const bool ready = userAccountProfileReady(state);
      if (!ready && !state.guild.empty() && !state.loading.contains(key) && !state.errors.contains(key)) {
        const auto member = std::to_string(static_cast<int>(Section::GuildMember)) + ':' + state.guild;
        if (state.loading.contains(member))
          form.row(
              "Loading server identity", "Your server avatar, nickname and cosmetics are being loaded before editing."
          );
        else {
          const auto error = state.errors.find(member);
          form.rowButton(
              form.actionRow(
                  "Server profile unavailable",
                  error == state.errors.end() ? "Load your current server identity before making changes."
                                              : error->second
              ),
              "Retry", "account-reload"
          );
        }
      }
      const bool nitro = m_account.premiumType.value_or(0) == 2;
      api::Channel synthetic;
      synthetic.overwrites.emplace();
      const auto permissions = api::channelPermissions(
          state.guild, m_account.id, m_roleDirectory.owner(state.guild), m_roleDirectory.allRoles(state.guild),
          m_roleDirectory.memberships(state.guild, m_account.id), synthetic
      );
      const bool nickname = m_demo || state.guild.empty() || (permissions.value_or(0) & ((1ULL << 3) | (1ULL << 26)));
      std::vector<std::string> choices{"User profile"}, guilds{""};
      for (const auto& guild : m_guilds) {
        choices.push_back(guild.name);
        guilds.push_back(guild.id);
      }
      auto at = std::ranges::find(guilds, state.guild);
      form.choose(
          "user-account:profile-scope", "Profile",
          "Customize your global profile or your identity in a server. Server nicknames are available to everyone.",
          choices, at == guilds.end() ? 0 : at - guilds.begin(),
          [this, guilds](std::size_t index) {
            if (index >= guilds.size())
              return;
            if (!m_userAccount.draft.empty() || m_userAccount.crop) {
              m_userAccount.error = "Save or reset your profile changes before choosing another server.";
              m_window.invalidate();
              return;
            }
            ++m_userAccount.generation;
            m_userAccount.pending.clear();
            m_userAccount.loading.clear();
            m_userAccount.profile = Json::object();
            ++m_dialogGeneration;
            m_userAccount.guild = guilds[index];
            loadUserAccountSettings("profile", true);
          },
          !state.busy
      );
      const auto current = [&](const char* name, std::string fallback = {}) {
        const auto draft = state.draft.find(name);
        if (draft != state.draft.end() && draft->is_string())
          return draft->get<std::string>();
        if (std::string_view(name) == "accent_color"
            && state.profile.contains(name)
            && state.profile[name].is_number_integer()) {
          char buffer[8]{};
          std::snprintf(buffer, sizeof(buffer), "#%06X", state.profile[name].get<int>() & 0xFFFFFF);
          return std::string(buffer);
        }
        if ((std::string_view(name) == "theme_primary" || std::string_view(name) == "theme_accent")
            && state.profile.contains("theme_colors")
            && state.profile["theme_colors"].is_array()
            && state.profile["theme_colors"].size() == 2) {
          const auto& value = state.profile["theme_colors"][std::string_view(name) == "theme_primary" ? 0 : 1];
          if (value.is_number_integer()) {
            char buffer[8]{};
            std::snprintf(buffer, sizeof(buffer), "#%06X", value.get<int>() & 0xFFFFFF);
            return std::string(buffer);
          }
        }
        return text(state.profile, name, std::move(fallback));
      };
      const auto field = [&](const char* name, const std::string& title, const std::string& description,
                             std::size_t limit, float height = 32, bool entitled = true) {
        form.field(
            "user-account:profile:" + std::string(name), title, description, current(name),
            [this, key = std::string(name), limit](const std::string& value) {
              if (userAccountProfileReady(m_userAccount) && api::utf8Length(value) <= limit) {
                m_userAccount.draft[key] = value;
                m_profileDirty = true;
                m_userAccount.error.clear();
              }
              m_window.invalidate();
            },
            height, false, !state.busy && ready && entitled
        );
      };
      if (state.guild.empty())
        field(
            "global_name", "Display name",
            "The name shown across Discord. Leave empty to use your username; up to 32 characters.", 32
        );
      else
        field(
            "nick", "Server nickname",
            nickname
                ? "The name shown in this server. Leave empty to use your global display name; up to 32 characters."
                : "Your current roles do not allow you to change your nickname in this server.",
            32, 32, nickname
        );
      field(
          "pronouns", "Pronouns",
          state.guild.empty() ? "Shown on your profile; up to 40 characters."
                              : "Override your pronouns in this server; up to 40 characters. Requires Nitro.",
          40, 32, state.guild.empty() || nitro
      );
      field(
          "bio", "About me",
          state.guild.empty() ? "Introduce yourself with text, links and Markdown; up to 190 characters."
                              : "Introduce yourself in this server; up to 190 characters. Requires Nitro.",
          190, 94, state.guild.empty() || nitro
      );
      form.section("Profile appearance");
      action(
          "Avatar",
          state.guild.empty() ? "Choose an image for your avatar. Animated avatars require Nitro."
                              : "Choose a different avatar for this server. Requires Nitro.",
          "Change", "upload-avatar", ButtonVariant::Default, ready && (state.guild.empty() || nitro)
      );
      form.button(
          "Remove avatar", "remove-avatar", ButtonVariant::Default, 128,
          !state.busy && ready && (state.guild.empty() || nitro)
      );
      action(
          "Banner", "Choose an image or animated GIF under 10 MiB for your profile banner. Requires Nitro.", "Change",
          "upload-banner", ButtonVariant::Default, ready && nitro
      );
      form.button("Remove banner", "remove-banner", ButtonVariant::Default, 128, !state.busy && ready && nitro);
      field(
          "accent_color", "Banner color",
          state.guild.empty() ? "Enter a six-digit hex color, such as #A6C973. Used when no banner image is set."
                              : "Choose a server profile banner color as #RRGGBB. Requires Nitro.",
          7, 32, state.guild.empty() || nitro
      );
      if (nitro) {
        field("theme_primary", "Profile primary color", "The primary color of your profile card, as #RRGGBB.", 7);
        field("theme_accent", "Profile accent color", "The accent color of your profile card, as #RRGGBB.", 7);
      }
      form.section("Owned cosmetics");
      struct Cosmetic {
        std::string sku, name;
        int type;
      };
      std::vector<Cosmetic> cosmetics;
      const auto collect = [&](auto&& self, const Json& value, std::string name, bool item, unsigned depth) -> void {
        if (depth > 8 || cosmetics.size() >= 2000)
          return;
        if (value.is_array()) {
          for (const auto& child : value)
            self(self, child, name, item, depth + 1);
          return;
        }
        if (!value.is_object())
          return;
        name = text(value, "name", name);
        if (item && value.contains("type") && value["type"].is_number_integer()) {
          const int type = value["type"].get<int>();
          const auto sku = text(value, "sku_id");
          if (type >= 0
              && type <= 3
              && api::validSnowflake(sku)
              && std::ranges::none_of(cosmetics, [&](const auto& old) { return old.sku == sku; }))
            cosmetics.push_back({sku, text(value, "label", name.empty() ? "Owned cosmetic" : name), type});
        }
        for (const auto* key : {"items", "bundled_products", "variants"})
          if (value.contains(key))
            self(self, value[key], name, std::string_view(key) == "items", depth + 1);
      };
      collect(collect, state.records[std::to_string(static_cast<int>(Section::Collectibles)) + ':'], {}, false, 0);
      for (int type = 0; type < 4; ++type) {
        const char* title = type == 0 ? "Avatar decoration"
            : type == 1               ? "Profile effect"
            : type == 2               ? "Nameplate"
                                      : "Profile frame";
        std::vector<std::string> labels{"None"}, skus{""};
        for (const auto& cosmetic : cosmetics)
          if (cosmetic.type == type) {
            labels.push_back(cosmetic.name);
            skus.push_back(cosmetic.sku);
          }
        std::string currentSku;
        if (type == 0)
          currentSku = state.draft.contains("avatar_decoration_sku_id")
              ? text(state.draft, "avatar_decoration_sku_id")
              : text(state.profile.value("avatar_decoration_data", Json::object()), "sku_id");
        else if (type == 2) {
          if (state.guild.empty() && state.draft.contains("nameplate_sku_id"))
            currentSku = text(state.draft, "nameplate_sku_id");
          else if (!state.guild.empty() && state.draft.contains("collectibles"))
            currentSku = text(state.draft["collectibles"].value("nameplate", Json::object()), "sku_id");
          else
            currentSku = text(state.profile.value("nameplate", Json::object()), "sku_id");
        } else {
          for (const auto& item : rows(state.profile.value("collectibles", Json::array())))
            if (item.is_object() && item.value("type", -1) == type)
              currentSku = text(item, "sku_id");
          if (state.draft.contains("collectibles_sku_ids")) {
            currentSku.clear();
            for (const auto& sku : state.draft["collectibles_sku_ids"])
              if (sku.is_string())
                for (const auto& cosmetic : cosmetics)
                  if (cosmetic.type == type && cosmetic.sku == sku.get<std::string>())
                    currentSku = cosmetic.sku;
          }
        }
        auto selected = std::ranges::find(skus, currentSku);
        if (!currentSku.empty() && selected == skus.end()) {
          labels.push_back("Currently equipped");
          skus.push_back(currentSku);
          selected = std::prev(skus.end());
        }
        form.choose(
            "user-account:cosmetic:" + std::to_string(type), title,
            "Choose from cosmetics you already own. Purchase new cosmetics in Discord's shop.", labels,
            selected == skus.end() ? 0 : selected - skus.begin(),
            [this, type, skus, cosmetics](std::size_t index) {
              if (index >= skus.size())
                return;
              auto& state = m_userAccount;
              if (!userAccountProfileReady(state))
                return;
              const auto& sku = skus[index];
              const Json value = sku.empty() ? Json(nullptr) : Json(sku);
              if (type == 0)
                state.draft["avatar_decoration_sku_id"] = value;
              else if (type == 2) {
                if (state.guild.empty())
                  state.draft["nameplate_sku_id"] = value;
                else
                  state.draft["collectibles"] = {{"nameplate", sku.empty() ? Json(nullptr) : Json{{"sku_id", sku}}}};
              } else {
                std::set<std::string> remove;
                for (const auto& item : rows(state.profile.value("collectibles", Json::array())))
                  if (item.is_object() && item.value("type", -1) == type)
                    remove.insert(text(item, "sku_id"));
                for (const auto& item : cosmetics)
                  if (item.type == type)
                    remove.insert(item.sku);
                Json retained = Json::array(), original = state.draft.value("collectibles_sku_ids", Json::array());
                if (!state.draft.contains("collectibles_sku_ids"))
                  for (const auto& item : rows(state.profile.value("collectibles", Json::array())))
                    if (item.is_object() && !text(item, "sku_id").empty())
                      original.push_back(text(item, "sku_id"));
                for (const auto& old : original)
                  if (old.is_string() && !remove.contains(old.get<std::string>()))
                    retained.push_back(old);
                if (!sku.empty())
                  retained.push_back(sku);
                state.draft["collectibles_sku_ids"] = std::move(retained);
              }
              m_profileDirty = true;
              m_window.invalidate();
            },
            !state.busy
                && ready
                && state.loaded.contains(std::to_string(static_cast<int>(Section::Collectibles)) + ':')
                && !state.loading.contains(std::to_string(static_cast<int>(Section::Collectibles)) + ':')
                && (state.guild.empty() || nitro)
        );
      }
      if (state.crop)
        drawUserProfileCrop(form);
      form.section("Preview");
      const float cardWidth = std::min(form.width, 360.0F);
      const auto hash = text(state.profile, "avatar");
      const auto avatarUrl = !state.guild.empty() && api::validAssetHash(hash)
          ? "/guilds/" + state.guild + "/users/" + m_account.id + "/avatars/" + hash + ".png?size=128"
          : text(state.profile, "avatar_url", m_account.avatar);
      const auto preview = state.draft.contains("avatar") && !state.draft["avatar"].is_null()
          ? "user-profile-upload:avatar"
          : state.draft.contains("avatar") ? std::string{}
                                           : avatarUrl;
      const auto banner = state.draft.contains("banner") && !state.draft["banner"].is_null()
          ? "user-profile-upload:banner"
          : state.draft.contains("banner") ? std::string{}
                                           : text(state.profile, "banner_url");
      int color = state.profile.contains("accent_color") && state.profile["accent_color"].is_number_integer()
          ? state.profile["accent_color"].get<int>()
          : 0;
      if (state.draft.contains("accent_color") && state.draft["accent_color"].is_string()) {
        const auto value = state.draft["accent_color"].get<std::string>();
        if (value.size() == 7 && value.front() == '#') {
          unsigned parsed = 0;
          if (std::sscanf(value.c_str() + 1, "%06x", &parsed) == 1)
            color = static_cast<int>(parsed);
        }
      }
      rectangle(form.node, 0, form.y, cardWidth, 152, theme::panel, Style::hoverRadius);
      rectangle(form.node, 0, form.y, cardWidth, 76, color ? rgbHex(color) : theme::raised, Style::hoverRadius);
      if (!banner.empty())
        picture(form.node, 0, form.y, cardWidth, 76, banner, Style::radiusMd, true, false, ImageFitMode::Cover);
      rectangle(form.node, 10, form.y + Style::spaceGroup, 56, 56, theme::panel, 28);
      avatar(form.node, 14, form.y + 52, layout::Avatar::rail, preview, current("global_name", m_account.name));
      auto name = current(state.guild.empty() ? "global_name" : "nick", m_account.name);
      if (name.empty())
        name = state.guild.empty() ? m_account.username : m_account.name;
      label(
          form.node, 14, form.y + 111, cardWidth - Style::spaceWide, name, Style::fontSizeTitle, theme::foreground,
          FontWeight::SemiBold
      );
      label(
          form.node, 14, form.y + 133, cardWidth - Style::spaceWide, current("pronouns", m_account.pronouns),
          Style::fontSizeDescription, theme::muted
      );
      form.y += 164;
      if (!current("bio").empty())
        form.row("About me", current("bio"));
      form.button(
          "Save Changes", "profile-save", ButtonVariant::Primary, 132, !state.busy && ready && !state.draft.empty()
      );
      form.button("Reset", "profile-reset", ButtonVariant::Default, 80, !state.busy && !state.draft.empty());
    } else if (page == "devices") {
      form.section("Active sessions");
      const auto sessions = rows(data, "user_sessions");
      if (sessions.empty() && !state.loading.contains(key))
        form.row("No other sessions", "No active devices were returned for this account.");
      for (const auto& item : sessions) {
        const auto info =
            item.contains("client_info") && item["client_info"].is_object() ? item["client_info"] : Json::object();
        const auto name = text(info, "os", "Unknown device") + " · " + text(info, "platform", "Discord");
        const auto used = text(item, "approx_last_used_time", "Unknown");
        action(
            name, std::string(flag(item, "current_session") ? "This device. " : "") + "Last active: " + used, "Log out",
            "session-remove:" + text(item, "id_hash"), ButtonVariant::Destructive, !flag(item, "current_session")
        );
      }
      const bool currentKnown =
          std::ranges::any_of(sessions, [](const auto& item) { return flag(item, "current_session"); });
      action(
          "Log out other devices",
          currentKnown ? "End the other sessions shown above. Discord may ask you to verify this action."
                       : "Available after Discord identifies your current device. Reconnect if it has not arrived.",
          "Log out others", "sessions-remove-all", ButtonVariant::Destructive, currentKnown && sessions.size() > 1
      );
    } else if (page == "authorized") {
      form.section("Authorized applications");
      const auto apps = rows(data, "authorizations");
      if (apps.empty() && !state.loading.contains(key))
        form.row("No authorized apps", "Applications you authorize will appear here.");
      for (const auto& item : apps) {
        const auto application = item.value("application", Json::object());
        std::string permissions;
        for (const auto& scope : rows(item.value("scopes", Json::array())))
          if (scope.is_string()) {
            if (!permissions.empty())
              permissions += ", ";
            permissions += scopeDescription(scope.get<std::string>());
          }
        const auto id = text(item, "id", text(application, "id"));
        action(
            text(application, "name", "Application"),
            text(application, "description") + (permissions.empty() ? "" : "\nAccess: " + permissions), "Deauthorize",
            "app-remove:" + id, ButtonVariant::Destructive
        );
      }
    } else {
      form.section("Connected accounts");
      action(
          "Add an account", "Link an account in your browser, then refresh this list after authorization.", "Connect",
          "connection-add"
      );
      const auto connections = rows(data, "connections");
      if (connections.empty() && !state.loading.contains(key))
        form.row("No connected accounts", "Linked accounts will appear here after authorization.");
      for (const auto& item : connections) {
        const auto id = text(item, "id"), service = text(item, "type"), suffix = service + ':' + id;
        form.section(connectionName(service) + " · " + text(item, "name", "Connected account"));
        for (const auto* field : {"visibility", "metadata_visibility", "show_activity", "friend_sync"}) {
          if (!item.contains(field) && std::string_view(field) != "visibility")
            continue;
          const auto title = std::string_view(field) == "visibility" ? "Display on profile"
              : std::string_view(field) == "metadata_visibility"     ? "Display details on profile"
              : std::string_view(field) == "show_activity"           ? "Display activity as your status"
                                                                     : "Sync friends";
          const auto description = std::string_view(field) == "visibility"
              ? "Show this connected account on your Discord profile."
              : std::string_view(field) == "metadata_visibility" ? "Show verified account details on your profile."
              : std::string_view(field) == "show_activity" ? "Share activity from this account as your Discord status."
                                                           : "Use this connection to discover friends on Discord.";
          form.toggle(
              "user-account:connection:" + suffix + ':' + field, title, description, flag(item, field),
              [this, service, id, field = std::string(field)](bool value) {
                api::UserSettingsRequest request;
                request.section = Section::Connections;
                request.verb = Verb::Update;
                request.service = service;
                request.resourceId = id;
                request.values[field] = value;
                requestUserAccount(std::move(request));
              },
              !state.busy
          );
        }
        form.button("Refresh details", "connection-refresh:" + suffix, ButtonVariant::Default, 132, !state.busy);
        form.button("Disconnect", "connection-remove:" + suffix, ButtonVariant::Destructive, 116, !state.busy);
      }
      form.button("Refresh connections", "account-reload", ButtonVariant::Default, 154, !state.busy);
    }
    return true;
  }
} // namespace app
