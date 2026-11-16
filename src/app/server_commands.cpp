#include "app/server_commands.h"

#include "app/client_application.h"
#include "app/server_audit.h"
#include "app/server_settings_form.h"
#include "ui/controls/input.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>

namespace app {
  namespace {
    using Json = nlohmann::json;
    using Section = api::GuildSettingsSection;
    using Verb = api::GuildSettingsVerb;
    std::string str(const Json& value, std::string_view key, std::string fallback = {}) {
      if (!value.is_object())
        return fallback;
      const auto at = value.find(key);
      return at != value.end() && at->is_string() ? at->get<std::string>() : fallback;
    }
    std::string lower(std::string text) {
      std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    }
    bool validId(const std::string& value) {
      std::uint64_t id = 0;
      const auto result = std::from_chars(value.data(), value.data() + value.size(), id);
      return !value.empty()
          && value.front() != '0'
          && id
          && result.ec == std::errc{}
      && result.ptr == value.data() + value.size();
    }
    bool commandSection(Section section) {
      return section == Section::InstalledApplications
          || section == Section::ApplicationCommands
          || section == Section::CommandPermissions;
    }
    std::string permissionCache(const std::string& application, const std::string& command) {
      return application + ":" + command;
    }
    std::string commandName(const Json& command) {
      const auto name = str(command, "name", str(command, "name_default", "Unnamed command"));
      return command.value("type", 1) == 1 ? "/" + name : name;
    }
    void mergeApplications(Json& current, const Json& incoming) {
      if (!incoming.is_array())
        throw std::runtime_error("The server returned an invalid installed application list.");
      for (const auto& item : incoming) {
        const auto& application = item.is_object() && item.contains("application") && item["application"].is_object()
            ? item["application"]
            : item;
        const auto id = str(application, "id");
        if (!validId(id))
          continue;
        const auto at = std::ranges::find_if(current, [&](const auto& value) { return str(value, "id") == id; });
        if (at == current.end())
          current.push_back(application);
        else
          at->update(application);
      }
      std::sort(current.begin(), current.end(), [](const auto& a, const auto& b) {
        return lower(str(a, "name")) < lower(str(b, "name"));
      });
    }
    Json permissionRecord(
        const Json& records, const std::string& application, const std::string& command, const std::string& guild
    ) {
      if (!records.is_array())
        throw std::runtime_error("The server returned an invalid application permission list.");
      for (const auto& record : records) {
        if (str(record, "id") != command)
          continue;
        if (str(record, "application_id", application) != application
            || str(record, "guild_id", guild) != guild
            || !record.contains("permissions")
            || !record["permissions"].is_array())
          throw std::runtime_error("The server returned permissions for a different application or server.");
        // Also validate duplicate and future unsupported fields before showing
        // an editor that could overwrite information the client cannot retain.
        return mergeCommandPermissionEdits(record["permissions"], Json::array(), Json::array());
      }
      return Json::array();
    }
  } // namespace

  void ClientApplication::resetServerCommands() { m_serverCommands = {}; }

  bool ClientApplication::loadServerCommands(bool) {
    auto& state = m_serverSettings;
    auto& view = m_serverCommands;
    if (state.page != "commands")
      return false;
    if (view.guild != state.guild) {
      resetServerCommands();
      view.guild = state.guild;
    }
    if (m_demo) {
      if (!view.indexLoaded)
        prepareServerCommandsDemo();
      return true;
    }
    const auto pending = [&](Section section) {
      return std::ranges::any_of(state.pending, [&](const auto& item) {
        return item.second.section == section && item.second.verb == Verb::Get;
      });
    };
    if (!view.applicationsLoaded && !view.applicationsFailed && !pending(Section::InstalledApplications)) {
      api::GuildSettingsRequest request;
      request.section = Section::InstalledApplications;
      if (!requestServerSettings(request))
        view.applicationsFailed = true;
    }
    if (!view.indexLoaded && !view.indexFailed && !pending(Section::ApplicationCommands)) {
      api::GuildSettingsRequest request;
      request.section = Section::ApplicationCommands;
      if (!requestServerSettings(request))
        view.indexFailed = true;
    }
    if (!view.application.empty()
        && !view.loadedApplications.contains(view.application)
        && !view.failedApplications.contains(view.application)
        && !pending(Section::CommandPermissions)) {
      api::GuildSettingsRequest request;
      request.section = Section::CommandPermissions;
      request.values = {{"application_id", view.application}};
      if (!requestServerSettings(request))
        view.failedApplications.insert(view.application);
    }
    return true;
  }

  bool ClientApplication::serverCommandsResult(const api::Event& event) {
    if (!event.guildSettingsRequest || !commandSection(event.guildSettingsRequest->section))
      return false;
    auto& state = m_serverSettings;
    auto& view = m_serverCommands;
    const auto& request = *event.guildSettingsRequest;
    const auto pending = state.pending.find(request.request);
    if (pending == state.pending.end() || pending->second != request || request.guildId != state.guild)
      return true;
    if (event.guildSettingsMfaChallenge)
      return false;
    state.pending.erase(pending);
    state.loading = std::ranges::any_of(state.pending, [](const auto& item) { return item.second.verb == Verb::Get; });
    if (view.guild != request.guildId)
      return true;
    const bool preparing = view.preparing && *view.preparing == request;
    if (preparing)
      view.preparing.reset();
    state.saving = view.preparing.has_value()
        || std::ranges::any_of(state.pending, [](const auto& item) { return item.second.verb != Verb::Get; });
    if (!event.error.empty()) {
      if (request.section == Section::InstalledApplications)
        view.applicationsFailed = true;
      if (request.section == Section::ApplicationCommands)
        view.indexFailed = true;
      if (request.section == Section::CommandPermissions && request.verb == Verb::Get && !preparing)
        view.failedApplications.insert(str(request.values, "application_id"));
      const bool oldApplication = request.section == Section::CommandPermissions
          && !preparing
          && str(request.values, "application_id") != view.application;
      if (!oldApplication)
        state.error = event.error;
      else
        loadServerCommands();
      m_window.invalidate();
      return true;
    }
    try {
      if (request.section == Section::InstalledApplications) {
        mergeApplications(view.applications, event.guildSettings);
        view.applicationsLoaded = true;
      } else if (request.section == Section::ApplicationCommands) {
        const auto& result = event.guildSettings;
        if (!result.is_object()
            || !result.contains("application_commands")
            || !result["application_commands"].is_array()
            || !result.contains("applications")
            || !result["applications"].is_array())
          throw std::runtime_error("The application command index is not ready. Try loading it again.");
        mergeApplications(view.applications, result["applications"]);
        view.commands = result["application_commands"];
        view.indexLoaded = true;
      } else {
        const auto application = str(request.values, "application_id");
        if (request.verb == Verb::Get) {
          const auto& result = event.guildSettings;
          if (!result.is_array())
            throw std::runtime_error("The server returned an invalid application permission list.");
          // An absent record has no overrides. Clear old cached records first
          // so Reset to application defaults remains authoritative after reload.
          std::erase_if(view.permissions, [&](const auto& item) { return item.first.starts_with(application + ":"); });
          for (const auto& record : result) {
            const auto command = str(record, "id");
            if (!validId(command))
              continue;
            view.permissions[permissionCache(application, command)] =
                permissionRecord(result, application, command, state.guild);
          }
          view.loadedApplications.insert(application);
          if (preparing) {
            if (state.page != "commands" || view.application != application || !state.draft.contains("permissions"))
              return true;
            auto current = permissionRecord(result, application, view.command, state.guild);
            const auto merged =
                mergeCommandPermissionEdits(current, view.baseline, state.draft["permissions"], view.restoringDefaults);
            api::GuildSettingsRequest write;
            write.section = Section::CommandPermissions;
            write.verb = Verb::Update;
            write.resourceId = view.command;
            write.values = {{"application_id", application}, {"permissions", merged}};
            requestServerSettings(write);
          } else if (view.application == application && !state.dirty()) {
            view.baseline = permissionRecord(result, application, view.command, state.guild);
            state.record = {{"permissions", view.baseline}};
          }
        } else {
          auto updated = request.values.at("permissions");
          if (event.guildSettings.is_object() && event.guildSettings.contains("permissions"))
            updated = event.guildSettings["permissions"];
          updated = mergeCommandPermissionEdits(updated, Json::array(), Json::array());
          view.permissions[permissionCache(application, request.resourceId)] = updated;
          if (view.application == application && view.command == request.resourceId) {
            view.baseline = updated;
            state.record = {{"permissions", updated}};
            state.draft = Json::object();
            view.restoringDefaults = false;
            state.status = "Command permissions saved.";
            state.mfaRequest.reset();
            state.mfaChallenge = 0;
            state.mfaCode.clear();
          }
          // Refetch both the permission records and command index once after a
          // successful explicit save. This is not an automatic retry loop.
          view.loadedApplications.erase(application);
          view.indexLoaded = false;
          loadServerCommands();
          state.status = "Command permissions saved.";
        }
      }
      if (view.application.empty() && !view.applications.empty() && view.indexLoaded) {
        auto requested = state.lists.find("commands:open");
        auto app = requested != state.lists.end() && requested->second.is_string()
            ? requested->second.get<std::string>()
            : str(view.applications.front(), "id");
        if (std::ranges::none_of(view.applications, [&](const auto& value) { return str(value, "id") == app; }))
          app = str(view.applications.front(), "id");
        state.lists.erase("commands:open");
        serverCommandsAction("commands:application:" + app);
      }
      if (view.applicationsLoaded && view.indexLoaded)
        state.loaded.insert("commands");
      if (request.section == Section::CommandPermissions && str(request.values, "application_id") != view.application)
        loadServerCommands();
    } catch (const std::exception& error) {
      state.error = error.what();
      state.saving = false;
      view.preparing.reset();
      if (request.section == Section::InstalledApplications)
        view.applicationsFailed = true;
      if (request.section == Section::ApplicationCommands)
        view.indexFailed = true;
      if (request.section == Section::CommandPermissions && request.verb == Verb::Get && !preparing)
        view.failedApplications.insert(str(request.values, "application_id"));
    }
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::serverCommandsAction(const std::string& action) {
    auto& state = m_serverSettings;
    auto& view = m_serverCommands;
    if (action.starts_with("commands:open:")) {
      state.lists["commands:open"] = action.substr(14);
      selectServerSettingsPage("commands");
      if (state.page == "commands" && view.indexLoaded)
        serverCommandsAction("commands:application:" + action.substr(14));
      return true;
    }
    if (state.page != "commands")
      return false;
    if (action == "retry") {
      state.error.clear();
      view.applicationsFailed = view.indexFailed = false;
      view.failedApplications.erase(view.application);
      loadServerCommands();
      return true;
    }
    if (action == "reset") {
      if (state.saving || view.preparing || state.mfaRequest)
        return true;
      state.draft = Json::object();
      view.restoringDefaults = false;
      state.error.clear();
      state.status.clear();
      m_window.invalidate();
      return true;
    }
    if (action == "save") {
      if (state.saving || view.preparing || state.mfaRequest || !state.draft.contains("permissions"))
        return true;
      const auto access = serverSettingsAccess();
      api::GuildSettingsRequest check;
      check.guildId = state.guild;
      check.section = Section::CommandPermissions;
      check.verb = Verb::Update;
      if (!api::canManageGuildSettings(check, access)) {
        state.error = "You need both Manage Server and Manage Roles to change application command permissions.";
        return true;
      }
      if (m_demo) {
        view.baseline = state.draft["permissions"];
        view.permissions[permissionCache(view.application, view.command)] = view.baseline;
        state.record = {{"permissions", view.baseline}};
        state.draft = Json::object();
        view.restoringDefaults = false;
        state.status = "Command permissions saved.";
        m_window.invalidate();
        return true;
      }
      api::GuildSettingsRequest read;
      read.section = Section::CommandPermissions;
      read.values = {{"application_id", view.application}};
      if (requestServerSettings(read)) {
        view.preparing = state.pending.at(state.serial);
        // Protect the explicit save's fresh-read/write sequence from navigation
        // and another edit until that preparation has finished.
        state.saving = true;
      }
      m_window.invalidate();
      return true;
    }
    if (!action.starts_with("commands:"))
      return false;
    if (state.saving || view.preparing || state.mfaRequest)
      return true;
    const auto stage = [&](Json value) {
      if (value == view.baseline && !view.restoringDefaults)
        state.draft.erase("permissions");
      else
        state.draft["permissions"] = std::move(value);
      state.error.clear();
      state.status.clear();
    };
    const auto select = [&](const std::string& application, const std::string& command, bool discard) {
      if (state.dirty() && !discard) {
        view.pendingApplication = application;
        view.pendingCommand = command;
        view.confirming = true;
        m_scrolling.stop(state.scroll);
        state.scroll = 0;
        return;
      }
      closeSelectDropdown();
      ++state.generation;
      m_controlInput.setFocus(nullptr);
      m_focusedInput.clear();
      state.draft = Json::object();
      view.application = application;
      view.command = command;
      state.resource = permissionCache(application, command);
      view.target.clear();
      view.targetSearch.clear();
      view.confirming = false;
      view.restoringDefaults = false;
      const auto saved = view.permissions.find(permissionCache(application, command));
      view.baseline = saved == view.permissions.end() ? Json::array() : saved->second;
      state.record = {{"permissions", view.baseline}};
      state.error.clear();
      state.status.clear();
      m_scrolling.stop(state.scroll);
      state.scroll = 0;
      loadServerCommands();
    };
    try {
      if (action.starts_with("commands:application:")) {
        const auto application = action.substr(21);
        if (validId(application))
          select(application, application, false);
      } else if (action.starts_with("commands:command:")) {
        const auto command = action.substr(17);
        if (validId(command))
          select(view.application, command, false);
      } else if (action == "commands:discard-switch")
        select(view.pendingApplication, view.pendingCommand, true);
      else if (action == "commands:keep-editing")
        view.confirming = false;
      else if (action == "commands:inherit") {
        view.restoringDefaults = true;
        stage(Json::array());
      } else if (action == "commands:refresh") {
        if (state.dirty()) {
          state.error = "Save or reset these command permission changes before refreshing.";
        } else {
          view.applicationsLoaded = view.indexLoaded = false;
          view.applicationsFailed = view.indexFailed = false;
          view.failedApplications.clear();
          view.loadedApplications.erase(view.application);
          state.lists["commands:open"] = view.application;
          view.applications = Json::array();
          view.application.clear();
          view.command.clear();
          state.resource.clear();
          state.record = Json::object();
          loadServerCommands();
        }
      } else if (action.starts_with("commands:add:")) {
        const auto target = !view.target.empty() ? view.target : view.targetSearch;
        auto draft = state.draft.value("permissions", view.baseline);
        if (view.command != view.application
            && (view.restoringDefaults || (!state.draft.contains("permissions") && view.baseline.empty()))) {
          const auto defaults = view.permissions.find(permissionCache(view.application, view.application));
          draft = defaults == view.permissions.end() ? Json::array() : defaults->second;
        }
        view.restoringDefaults = false;
        stage(editCommandPermission(draft, target, view.targetType, action.ends_with("allow")));
      } else if (action.starts_with("commands:override:")) {
        const auto value = action.substr(18);
        const auto first = value.find(':'), last = value.rfind(':');
        if (first == std::string::npos || first == last)
          return true;
        const int type = value.front() - '0';
        const auto target = value.substr(first + 1, last - first - 1), choice = value.substr(last + 1);
        const auto draft = state.draft.value("permissions", view.baseline);
        stage(editCommandPermission(
            draft, target, type, choice == "remove" ? std::nullopt : std::optional(choice == "allow")
        ));
      }
    } catch (const std::exception& error) {
      state.error = error.what();
    }
    m_window.invalidate();
    return true;
  }

  void ClientApplication::prepareServerCommandsDemo() {
    auto& state = m_serverSettings;
    auto& view = m_serverCommands;
    view.guild = state.guild;
    view.applications = Json::array(
        {{{"id", "500"}, {"name", "Community Helper"}, {"description", "Moderation and useful community commands"}},
         {{"id", "501"}, {"name", "Music"}, {"description", "Shared music controls"}}}
    );
    view.commands = Json::array(
        {{{"id", "600"},
          {"application_id", "500"},
          {"name", "announce"},
          {"description", "Post an announcement in this channel"},
          {"type", 1},
          {"default_member_permissions", "32"}},
         {{"id", "601"},
          {"application_id", "500"},
          {"name", "profile"},
          {"description", "View your community profile"},
          {"type", 1},
          {"default_member_permissions", nullptr}},
         {{"id", "602"},
          {"application_id", "500"},
          {"name", "User information"},
          {"type", 2},
          {"default_member_permissions", nullptr}},
         {{"id", "603"},
          {"application_id", "500"},
          {"name", "Report message"},
          {"type", 3},
          {"default_member_permissions", nullptr}},
         {{"id", "604"}, {"application_id", "501"}, {"name", "play"}, {"description", "Play a track"}, {"type", 1}}}
    );
    view.permissions["500:500"] = Json::array(
        {{{"id", state.guild}, {"type", 1}, {"permission", true}},
         {{"id", allCommandChannelsId(state.guild)}, {"type", 3}, {"permission", true}}}
    );
    view.permissions["500:600"] = Json::array(
        {{{"id", state.guild}, {"type", 1}, {"permission", false}},
         {{"id", "50"}, {"type", 1}, {"permission", true}},
         {{"id", "101"}, {"type", 3}, {"permission", true}}}
    );
    view.loadedApplications = {"500", "501"};
    view.applicationsLoaded = view.indexLoaded = true;
    view.application = "500";
    view.command = m_demoOverlay.find("commands-custom") != std::string::npos ? "600" : "500";
    view.baseline = view.permissions[permissionCache(view.application, view.command)];
    state.resource = permissionCache(view.application, view.command);
    state.record = {{"permissions", view.baseline}};
  }

  bool ClientApplication::drawServerCommands(ServerSettingsForm& form) {
    auto& state = m_serverSettings;
    auto& view = m_serverCommands;
    if (state.page != "commands")
      return false;
    if (view.guild != state.guild) {
      resetServerCommands();
      view.guild = state.guild;
    }
    if (m_demo && !view.indexLoaded)
      prepareServerCommandsDemo();
    const auto access = serverSettingsAccess();
    api::GuildSettingsRequest check;
    check.guildId = state.guild;
    check.section = Section::CommandPermissions;
    check.verb = Verb::Update;
    const bool canEdit = api::canManageGuildSettings(check, access);
    if (view.confirming) {
      form.section("Unsaved command permissions");
      form.detail(
          "Keep these changes?",
          "Save your changes before switching, or discard them to open the other application or command."
      );
      form.button("Keep editing", "commands:keep-editing");
      form.button("Discard changes", "commands:discard-switch", ButtonVariant::Destructive, 144);
      return true;
    }
    const auto search = [&](const std::string& id, std::string& current, const std::string& placeholder) {
      auto& input =
          inputControl(form.node, 0, form.y, form.width, 30, "server-settings:commands:" + id, current, placeholder);
      input.setFontSize(Style::fontSizeDescription);
      input.setOnChange([this, id](const std::string& value) {
        if (id == "application-search")
          m_serverCommands.applicationSearch = value;
        else if (id == "command-search")
          m_serverCommands.commandSearch = value;
        else {
          m_serverCommands.targetSearch = value;
          m_serverCommands.target.clear();
        }
        m_window.invalidate();
      });
      input.setOnSubmit([](const std::string&) {});
      form.y += Style::spaceSection;
    };
    form.section("Installed application");
    form.detail(
        "Application", "Choose an installed app to control who can use its commands and where they can be used."
    );
    if (!view.applicationsLoaded && !view.indexLoaded) {
      form.row("Loading applications", "Installed apps and their commands are loading.");
      return true;
    }
    search("application-search", view.applicationSearch, "Find an application");
    std::vector<std::string> applications, applicationNames;
    std::size_t selectedApplication = 0;
    for (const auto& application : view.applications) {
      const auto id = str(application, "id"), name = str(application, "name", id);
      if (id != view.application
          && !view.applicationSearch.empty()
          && !lower(name).contains(lower(view.applicationSearch)))
        continue;
      if (id == view.application)
        selectedApplication = applications.size();
      applications.push_back(id);
      applicationNames.push_back(name);
    }
    if (applications.empty()) {
      form.row(
          view.applicationSearch.empty() ? "No applications installed" : "No matching applications",
          view.applicationSearch.empty() ? "Apps installed to this server appear here when they expose commands."
                                         : "Try another application name."
      );
      return true;
    }
    form.chooseCallback(
        "application", "Select application", "", applicationNames, selectedApplication,
        [this, applications](std::size_t at) {
          if (at < applications.size())
            serverCommandsAction("commands:application:" + applications[at]);
        }
    );
    form.button("Refresh applications", "commands:refresh", ButtonVariant::Default, 160, !state.loading);
    if (view.application.empty())
      return true;
    form.section("Command permissions");
    if (!canEdit)
      form.detail(
          "Read-only permissions",
          "You need both Manage Server and Manage Roles to change command permissions. Administrators can always use "
          "all commands."
      );
    search("command-search", view.commandSearch, "Find a command by name or description");
    std::vector<std::string> commands{view.application}, names{"All commands · Application defaults"};
    std::size_t selectedCommand = 0;
    const Json* selected = nullptr;
    for (const auto& command : view.commands) {
      if (str(command, "application_id") != view.application)
        continue;
      const auto id = str(command, "id"), name = commandName(command), description = str(command, "description");
      if (id == view.command)
        selected = &command;
      if (id != view.command
          && !view.commandSearch.empty()
          && !lower(name + " " + description).contains(lower(view.commandSearch)))
        continue;
      if (id == view.command)
        selectedCommand = commands.size();
      commands.push_back(id);
      names.push_back(
          name
          + (command.value("type", 1) == 2       ? " · User command"
                 : command.value("type", 1) == 3 ? " · Message command"
                                                 : "")
      );
    }
    form.chooseCallback(
        "command", "Command", "Application defaults apply to commands without individual overrides.", names,
        selectedCommand, [this, commands](std::size_t at) {
          if (at < commands.size())
            serverCommandsAction("commands:command:" + commands[at]);
        }
    );
    if (!view.loadedApplications.contains(view.application)) {
      form.row("Loading permissions", "The current permission overrides are loading.");
      return true;
    }
    const auto permissions = state.draft.value("permissions", view.baseline);
    const bool appDefaults = view.command == view.application, inherited = !appDefaults && permissions.empty();
    if (selected) {
      auto description = str(*selected, "description", str(*selected, "description_default"));
      if (const auto defaults = selected->find("default_member_permissions");
          defaults != selected->end() && defaults->is_string()) {
        const auto value = defaults->get<std::string>();
        const auto required = value == "0" ? "Restricted to administrators unless an override grants access"
                                           : auditValue("permissions", *defaults, {});
        description += (description.empty() ? "" : "\n") + std::string("Developer defaults: ") + required + ".";
      }
      form.row(commandName(*selected), description);
    }
    form.row(
        appDefaults     ? "Application defaults"
            : inherited ? "Synced with application defaults"
                        : "Not synced with application defaults",
        appDefaults
            ? "These rules apply to commands that have no individual overrides. Administrators can always use commands."
            : inherited
            ? "This command follows the application's role, member and channel rules. Add an override to customize it."
            : "This command has its own role, member or channel rules. Use application defaults to remove its "
              "individual overrides."
    );
    if (!appDefaults && !inherited)
      form.button("Use app defaults", "commands:inherit", ButtonVariant::Default, 144, canEdit);
    const auto roleName = [&](const std::string& id) {
      if (id == state.guild)
        return std::string("@everyone");
      for (const auto& role : m_roleDirectory.allRoles(state.guild))
        if (role.id == id)
          return role.name;
      return id;
    };
    const auto channelName = [&](const std::string& id) {
      if (id == allCommandChannelsId(state.guild))
        return std::string("All channels");
      const auto channels = m_guildChannels.find(state.guild);
      if (channels != m_guildChannels.end())
        for (const auto& channel : channels->second)
          if (channel.id == id)
            return "#" + channel.name;
      if (state.guild == m_guildId)
        for (const auto& channel : m_channels)
          if (channel.id == id)
            return "#" + channel.name;
      return id;
    };
    const auto memberName = [&](const std::string& id) {
      if (id == m_account.id)
        return m_account.name;
      if (state.guild == m_guildId)
        for (const auto& member : m_members)
          if (member.user.id == id)
            return member.user.name;
      const auto members = state.lists.find("members");
      if (members != state.lists.end() && members->second.is_array())
        for (const auto& member : members->second)
          if (member.contains("user") && str(member["user"], "id") == id)
            return str(member, "nick", str(member["user"], "global_name", str(member["user"], "username", id)));
      return userName(id, id);
    };
    for (const auto* group : {"Roles and members", "Channels"}) {
      const bool channels = std::string_view(group) == "Channels";
      bool started = false;
      for (const auto& permission : permissions) {
        const auto type = permission.value("type", 0);
        if ((type == 3) != channels)
          continue;
        if (!started) {
          form.section(group);
          started = true;
        }
        const auto id = str(permission, "id"), key = commandPermissionKey(permission);
        const auto name = type == 1 ? roleName(id) : type == 2 ? memberName(id) : channelName(id);
        form.chooseCallback(
            "override:" + key, name,
            type == 1       ? "Role override"
                : type == 2 ? "Member override"
                            : "Channel override",
            {"Allow", "Deny", "Remove override"}, permission.value("permission", false) ? 0 : 1,
            [this, key](std::size_t choice) {
              serverCommandsAction(
                  "commands:override:"
                  + key
                  + ":"
                  + (choice == 0       ? "allow"
                         : choice == 1 ? "deny"
                                       : "remove")
              );
            },
            canEdit
        );
      }
    }
    if (canEdit) {
      form.section("Add permission override");
      form.chooseCallback(
          "target-type", "Target", "Allow or deny a role, member or channel. A command supports up to 100 overrides.",
          {"Role", "Member", "Channel"}, view.targetType - 1, [this](std::size_t index) {
            m_serverCommands.targetType = static_cast<int>(index) + 1;
            m_serverCommands.target.clear();
            m_serverCommands.targetSearch.clear();
            m_window.invalidate();
          }
      );
      search("target-search", view.targetSearch, "Find a target or enter its ID");
      std::map<std::string, std::string> targets;
      if (view.targetType == 1) {
        targets[state.guild] = "@everyone";
        for (const auto& role : m_roleDirectory.allRoles(state.guild))
          if (access.owner == access.self || (role.position < access.selfPosition && !role.id.empty()))
            targets[role.id] = role.name;
      } else if (view.targetType == 2) {
        targets[m_account.id] = m_account.name;
        if (state.guild == m_guildId)
          for (const auto& member : m_members)
            if (!member.user.id.empty())
              targets[member.user.id] = member.user.name;
        const auto members = state.lists.find("members");
        if (members != state.lists.end() && members->second.is_array())
          for (const auto& member : members->second)
            if (member.contains("user")) {
              const auto id = str(member["user"], "id");
              if (!id.empty())
                targets[id] = memberName(id);
            }
      } else {
        targets[allCommandChannelsId(state.guild)] = "All channels";
        const auto channels = m_guildChannels.find(state.guild);
        const auto add = [&](const auto& list) {
          for (const auto& channel : list)
            if (!channel.isThread())
              targets[channel.id] = "#" + channel.name;
        };
        if (channels != m_guildChannels.end())
          add(channels->second);
        if (state.guild == m_guildId)
          add(m_channels);
      }
      if (validId(view.targetSearch))
        targets.try_emplace(view.targetSearch, view.targetSearch);
      std::vector<std::string> ids, labels;
      std::size_t index = 0;
      for (const auto& [id, name] : targets) {
        if (!view.targetSearch.empty() && !lower(name + " " + id).contains(lower(view.targetSearch)))
          continue;
        if (id == view.target)
          index = ids.size();
        ids.push_back(id);
        labels.push_back(name);
      }
      if (!ids.empty()) {
        if (std::ranges::find(ids, view.target) == ids.end())
          view.target = validId(view.targetSearch) && std::ranges::find(ids, view.targetSearch) != ids.end()
              ? view.targetSearch
              : ids.front();
        index = std::ranges::find(ids, view.target) - ids.begin();
        form.chooseCallback("target", "Select target", "", labels, index, [this, ids](std::size_t at) {
          if (at < ids.size())
            m_serverCommands.target = ids[at];
        });
        form.button("Allow", "commands:add:allow", ButtonVariant::Default, 92, permissions.size() < 100);
        form.button("Deny", "commands:add:deny", ButtonVariant::Default, 92, permissions.size() < 100);
      } else
        form.row("No matching targets", "Try another name or enter a role, member or channel ID.");
    }
    return true;
  }
} // namespace app
