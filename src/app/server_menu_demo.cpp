#include "app/client_application.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::demoFolderMenuStep() {
    using Kind = ServerMenuKind;
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto open = [&] {
      const auto hit = std::ranges::find_if(m_contextTargets, [](const auto& target) {
        return target.action == Action::Folder && target.id == "77";
      });
      require(hit != m_contextTargets.end(), "Folder rail headers expose their own context target");
      m_window.contextClick(
          hit->x + hit->width / 2, hit->y + hit->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      require(m_contextOpen && m_contextGuild.empty(), "Folder context menus never target the selected server");
    };
    const auto click = [&](Kind kind) {
      const auto at = std::ranges::find_if(m_contextEntries, [&](const auto& entry) {
        const auto index = entry.id - serverMenuBase;
        return !entry.separator
            && index >= 0
            && static_cast<std::size_t>(index) < m_serverMenuCommands.size()
            && m_serverMenuCommands[index].kind == kind;
      });
      require(at != m_contextEntries.end() && m_contextControl, "Folder menu mounts native actionable entries");
      const auto index = static_cast<std::size_t>(at - m_contextEntries.begin());
      const auto center = (m_contextControl->rowTop(index) + m_contextControl->rowBottom(index)) / 2;
      m_window.click(
          m_contextBounds.x + m_contextBounds.width / 2,
          m_contextBounds.y + center - m_contextScroll + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272
      );
      m_window.released();
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      if (std::ranges::find(m_guilds, "2", &api::Guild::id) == m_guilds.end())
        m_guilds.push_back({"2", "Background"});
      m_guildLayout.folders = {{"77", "Communities", 0x123456, {m_guildId, "2"}}};
      m_guildLayout.positions = {m_guildId, "2"};
      m_foldersRestored = true;
      m_openFolders = {"77"};
      arrangeGuildFolders();
      api::Channel background{"201", "background-chat", 0};
      background.lastMessageId = "9100";
      m_guildChannels["2"] = {background};
      m_roleDirectory.apply(
          {false, {{.guild = m_guildId, .owner = m_account.id}, {.guild = "2", .owner = m_account.id}}}
      );
      m_channels.front().lastMessageId = "9000";
    } else if (step == 1)
      open();
    else if (step == 2) {
      require(m_contextEntries.size() == 6, "Folder menu provides all four actions and both separators");
      click(Kind::FolderMoveMenu);
    } else if (step == 3) {
      require(
          m_channelSubmenuEntries.size() == 4 && m_reactionControl,
          "Folder move submenu provides Top, Up, Down and Bottom"
      );
      const auto index = m_channelSubmenuEntries.size() - 1;
      const auto id = m_channelSubmenuEntries[index].id;
      require(m_serverMenuCommands[id - serverMenuBase].guild == "77", "Folder submenus retain the folder identifier");
      const auto center = (m_reactionControl->rowTop(index) + m_reactionControl->rowBottom(index)) / 2;
      m_window.click(
          m_reactionSubmenuBounds.x + m_reactionSubmenuBounds.width / 2,
          m_reactionSubmenuBounds.y
              + center
              - m_reactionSubmenuScroll
              + m_window.titlebarHeight()
              - layout::Window::titlebarHeight,
          272
      );
      m_window.released();
      require(!m_contextOpen, "Choosing a folder position dispatches through the native submenu");
      open();
    } else if (step == 4)
      click(Kind::FolderSettings);
    else if (step == 5) {
      require(
          m_dialogKind == "server-folder-settings"
              && m_dialogText == "Communities"
              && m_serverFolderColor == "#123456"
              && m_inputs.contains("server-menu:folder-color"),
          "Folder settings opens with the actual name, color and retained native inputs"
      );
      focusControl("dialog");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("dialog", "Orbit");
      focusControl("server-menu:folder-color");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("server-menu:folder-color", "invalid");
      submitServerMenuDialog();
      require(
          !m_dialogPending && !m_dialogError.empty() && m_dialogText == "Orbit",
          "Invalid folder colors retain the editable name and report an error"
      );
    } else if (step == 6) {
      focusControl("server-menu:folder-color");
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl("server-menu:folder-color", "#ABCDEF");
      submitServerMenuDialog();
      require(
          m_dialogPending
              && m_dialogAction
              && m_dialogAction->folderChange
              && m_dialogAction->folderChange->name == "Orbit"
              && m_dialogAction->folderChange->color == 0xABCDEF,
          "Folder save submits typed name/color together"
      );
      api::Event failed;
      failed.interaction = *m_dialogAction;
      failed.error = "Settings changed on another device.";
      serverMenuInteractionResult(failed);
      require(
          !m_dialogPending && !m_dialogError.empty() && m_folders.front().name == "Communities",
          "Failed saves retain the draft and never change the rendered organization"
      );
    } else if (step == 7) {
      submitServerMenuDialog();
      api::Event saved;
      saved.interaction = *m_dialogAction;
      saved.layout = m_guildLayout;
      saved.layout.folders.front().name = "Orbit";
      saved.layout.folders.front().color = 0xABCDEF;
      serverMenuInteractionResult(saved);
      require(
          m_dialogKind.empty()
              && m_folders.front().name == "Orbit"
              && m_folders.front().color == 0xABCDEF
              && m_openFolders.contains("77"),
          "Successful folder saves project the layout without collapsing the folder"
      );
      open();
    } else if (step == 8) {
      click(Kind::FolderCloseAll);
      require(m_openFolders.empty() && m_foldersRestored, "Close All Folders records an explicit empty expansion set");
      arrangeGuildFolders();
      require(m_openFolders.empty(), "Layout refresh does not force the selected server folder back open");
      open();
    } else if (step == 9) {
      const auto action = guildReadAction(m_folders.front().guildIds);
      const auto request = api::interactionRequest(action);
      require(
          request
              && std::ranges::find(action.readStates, std::pair<std::string, std::string>{"201", "9100"})
                  != action.readStates.end()
              && action.readStates.size() >= 2,
          "Folder reads include channel cursors from both selected and background servers"
      );
      click(Kind::FolderRead);
      api::Event ack;
      ack.interaction = action;
      serverMenuInteractionResult(ack);
      require(m_localRead["201"] == "9100", "Folder read acknowledgements update every included server");
    } else if (step == 10) {
      api::Event stale;
      stale.interaction = api::Interaction{
          .kind = api::InteractionKind::EditGuildFolder,
          .guild = m_guildId,
          .stateRevision = m_guildLayoutRevision - 1,
          .folderChange = api::GuildFolderChange{.folder = "77", .name = "Stale"}
      };
      stale.layout.folders = {{"77", "Stale", 0, {m_guildId}}};
      serverMenuInteractionResult(stale);
      require(m_folders.front().name == "Orbit", "Older folder acknowledgements cannot overwrite a newer layout");
      open();
    } else if (step == 11) {
      const auto command = m_contextEntries.front().id;
      m_guildLayout.folders.clear();
      arrangeGuildFolders();
      serverMenuEntry(command);
      require(!m_contextOpen && m_dialogKind.empty(), "Deleted folder targets dismiss raced menu actions");
    } else if (step == 12)
      require(m_guildId == "1" && m_channelId == "101", "Folder actions preserve the selected conversation");
  }
  void ClientApplication::demoServerMenuStep() {
    using Kind = ServerMenuKind;
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const std::string background = "2";
    const auto rootCommand = [&](Kind kind) {
      for (const auto& entry : m_contextEntries) {
        const auto index = entry.id - serverMenuBase;
        if (!entry.separator
            && index >= 0
            && static_cast<std::size_t>(index) < m_serverMenuCommands.size()
            && m_serverMenuCommands[index].kind == kind)
          return entry.id;
      }
      return -1;
    };
    const auto open = [&](const std::string& guild) {
      // Construct the ordinary account menu without the demo's developer-action
      // preview exception. Opening a menu itself makes no account requests.
      const auto demo = m_demo;
      m_demo = false;
      const bool opened = openServerContext({0, 0, 0, 0, Action::GuildMenu, guild}, 84, 62);
      m_demo = demo;
      require(opened, "A cached target server has an independent native menu");
    };
    const auto click = [&](Kind kind) {
      const auto id = rootCommand(kind);
      require(id >= 0 && m_contextControl, "Expected native server-menu action is present");
      const auto at = std::ranges::find(m_contextEntries, id, &ContextMenuControlEntry::id);
      const auto index = static_cast<std::size_t>(at - m_contextEntries.begin());
      const auto center = (m_contextControl->rowTop(index) + m_contextControl->rowBottom(index)) / 2;
      const auto y = m_contextBounds.y + center - m_contextScroll;
      require(
          y > m_contextBounds.y && y < m_contextBounds.y + m_contextBounds.height,
          "Native server-menu replay clicks are inside the clipped popup"
      );
      m_window.click(m_contextBounds.x + 72, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272);
      m_window.released();
    };
    const auto child = [&](Kind kind, int value = 0, const std::string& argument = "") {
      const auto at = std::ranges::find_if(m_channelSubmenuEntries, [&](const auto& entry) {
        const auto index = entry.id - serverMenuBase;
        if (index < 0 || static_cast<std::size_t>(index) >= m_serverMenuCommands.size())
          return false;
        const auto& command = m_serverMenuCommands[index];
        return command.kind == kind && command.value == value && command.argument == argument;
      });
      require(
          at != m_channelSubmenuEntries.end() && m_reactionSubmenu && m_reactionControl,
          "Server submenus expose actionable typed choices"
      );
      const auto index = static_cast<std::size_t>(at - m_channelSubmenuEntries.begin());
      const auto command = m_serverMenuCommands[at->id - serverMenuBase];
      require(command.guild == m_contextGuild, "Submenu commands retain the right-clicked server identity");
      m_window.click(
          m_reactionSubmenuBounds.x + 72,
          m_reactionSubmenuBounds.y
              + (m_reactionControl->rowTop(index) + m_reactionControl->rowBottom(index)) / 2
              + m_window.titlebarHeight()
              - (Style::space2Xl + Style::spaceXs),
          272
      );
      m_window.released();
    };
    const auto notification = [&](const std::string& guild, api::GuildNotification patch, const std::string& error = "",
                                  std::optional<std::uint64_t> revision = {}) {
      api::Event result;
      result.operation = api::Operation::Interaction;
      result.interaction = api::Interaction{
          .kind = api::InteractionKind::GuildNotifications,
          .guild = guild,
          .stateRevision = revision.value_or(m_userDirectory.guildNotificationRevision(guild)),
          .guildNotification = patch
      };
      result.error = error;
      result.users.guildNotifications[guild] = std::move(patch);
      require(serverMenuInteractionResult(result), "Server notification acknowledgements use their own result path");
    };

    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(m_guildId == "1" && m_channelId == "101", "Server-menu replay begins in the selected chat fixture");
      auto selected = std::ranges::find(m_guilds, m_guildId, &api::Guild::id);
      auto other = std::ranges::find(m_guilds, background, &api::Guild::id);
      require(selected != m_guilds.end() && other != m_guilds.end(), "Both replay servers are cached");
      selected->features = other->features = {"COMMUNITY"};
      api::GuildRoleUpdate selectedRoles{.guild = m_guildId, .replaceRoles = true};
      selectedRoles.owner = "11";
      selectedRoles.roles = {
          {.id = m_guildId,
           .name = "@everyone",
           .permissions = std::to_string(1ULL | api::manageChannel | (1ULL << 5) | api::viewChannel | (1ULL << 44))},
          {.id = "50", .name = "Channel access", .permissions = "0"}
      };
      // The inherited channel fixture denies everyone and grants this role access.
      selectedRoles.members[m_account.id] = {"50"};
      api::GuildRoleUpdate backgroundRoles{.guild = background, .replaceRoles = true};
      backgroundRoles.owner = "11";
      backgroundRoles.roles = {{.id = background, .name = "@everyone", .permissions = "3072"}};
      backgroundRoles.members[m_account.id] = {};
      m_roleDirectory.apply({false, {selectedRoles, backgroundRoles}});
      require(
          (serverChannelPermissions(m_guildId, m_channelId).value_or(0) & api::viewChannel)
              && (serverChannelPermissions(m_guildId, "103").value_or(0) & api::viewChannel),
          "Server-menu filtering targets must be accessible before follow preferences are applied"
      );
      api::Channel backgroundChannel{"201", "background-chat", 0};
      backgroundChannel.overwrites.emplace();
      backgroundChannel.lastMessageId = "9100";
      m_guildChannels[background] = {backgroundChannel};
      m_guildLayout.folders = {{"77", "Communities", 0x123456, {m_guildId}}, {"", "", 0, {background}}};
      m_guildLayout.positions = {m_guildId, background};
      arrangeGuildFolders();
      m_preferences.developerMode = false;
      m_channelListPreferences.hideMuted = false;
      m_channelListPreferences.pinned.clear();
      api::UserUpdates initial;
      initial.guildNotifications[background].flags = 1ULL << 30;
      m_userDirectory.apply(initial);
      open(background);
    } else if (step == 1) {
      require(
          m_contextGuild == background && m_guildId == "1" && m_channelId == "101",
          "Right-clicking another server leaves the selected conversation unchanged"
      );
      for (const auto kind :
           {Kind::Read, Kind::MuteMenu, Kind::NotifyMenu, Kind::HideMuted, Kind::ShowAll, Kind::Privacy, Kind::Profile,
            Kind::MoveMenu, Kind::Leave})
        require(rootCommand(kind) >= 0, "Ordinary members retain all personal server actions");
      for (const auto kind :
           {Kind::Invite, Kind::SettingsMenu, Kind::CreateChannel, Kind::CreateCategory, Kind::CreateEvent,
            Kind::Security, Kind::CopyId})
        require(rootCommand(kind) < 0, "The target's permissions and developer preference hide unauthorized actions");
      require(
          !(serverGuildPermissions(background).value_or(0) & api::manageChannel)
              && (serverGuildPermissions(m_guildId).value_or(0) & api::manageChannel),
          "Background menus never inherit the selected server's management permissions"
      );
      click(Kind::MuteMenu);
    } else if (step == 2) {
      require(m_channelSubmenuEntries.size() == 6, "Mute Server offers all six Discord duration choices");
      child(Kind::Mute, 900);
      require(!m_contextOpen, "Choosing a native mute submenu entry dispatches and closes the menu");
    } else if (step == 3) {
      notification(background, {.muted = true, .muteUntil = "2035-01-01T00:00:00Z"});
      require(
          m_userDirectory.muted(background) && !m_userDirectory.muted(m_guildId) && m_channelMuteTimer.active(),
          "A timed server mute updates only its target and schedules one expiry wake"
      );
      notification(background, {.muted = false}, "Synthetic preference failure");
      require(m_userDirectory.muted(background), "Failed preference writes cannot change the acknowledged state");
      const auto stale = m_userDirectory.guildNotificationRevision(background);
      api::UserUpdates live;
      live.guildNotifications[background].level = 2;
      m_userDirectory.apply(live);
      notification(background, {.muted = false}, "", stale);
      require(m_userDirectory.muted(background), "A stale acknowledgement cannot overwrite a newer Gateway setting");
      open(background);
    } else if (step == 4) {
      require(rootCommand(Kind::Mute) >= 0 && rootCommand(Kind::MuteMenu) < 0, "Unmute Server is an immediate command");
      click(Kind::NotifyMenu);
    } else if (step == 5) {
      require(m_channelSubmenuEntries.size() == 4, "Notification submenu includes three levels and detailed settings");
      child(Kind::Notify, 1);
    } else if (step == 6) {
      notification(background, {.level = 1});
      require(
          m_userDirectory.notificationLevel(background) == 1 && m_userDirectory.muted(background),
          "Choosing Only mentions preserves the server's independent mute state"
      );
      open(background);
    } else if (step == 7)
      click(Kind::HideMuted);
    else if (step == 8) {
      require(!m_contextOpen, "The native Hide Muted checkbox dispatches an action");
      notification(background, {.hideMutedChannels = true});
      require(
          m_userDirectory.guildPreferences(background).hideMutedChannels == true && !m_channelListPreferences.hideMuted,
          "A background server's Hide Muted preference cannot change the selected server's list"
      );
      open(background);
    } else if (step == 9)
      click(Kind::ShowAll);
    else if (step == 10) {
      require(!m_contextOpen, "The native Show All checkbox dispatches an action");
      notification(background, {.flags = api::guildOptInChannelsOn | (1ULL << 30)});
      require(
          m_userDirectory.guildPreferences(background).flags == (api::guildOptInChannelsOn | (1ULL << 30)),
          "Show All acknowledgements preserve unrelated guild flag bits"
      );
      api::UserUpdates followed;
      followed.guildNotifications[m_guildId].flags = api::guildOptInChannelsOn;
      m_userDirectory.apply(followed);
      require(
          serverChannelVisible(*serverChannel(m_guildId, "101"))
              && !serverChannelVisible(*serverChannel(m_guildId, "103")),
          "Follow-only filtering preserves the current channel while hiding unfollowed siblings"
      );
      followed.notifications["103"].flags = api::channelOptInEnabled;
      m_userDirectory.apply(followed);
      require(serverChannelVisible(*serverChannel(m_guildId, "103")), "Followed channels reappear without a reload");
      followed.guildNotifications[m_guildId].flags = api::guildOptInChannelsOff;
      m_userDirectory.apply(followed);
      m_preferences.developerMode = true;
      open(m_guildId);
    } else if (step == 11) {
      for (const auto kind :
           {Kind::Invite, Kind::SettingsMenu, Kind::CreateChannel, Kind::CreateCategory, Kind::CreateEvent,
            Kind::Security, Kind::CopyId})
        require(rootCommand(kind) >= 0, "Authorized server management and developer actions are available");
      click(Kind::SettingsMenu);
    } else if (step == 12)
      child(Kind::Settings, 0, "general");
    else if (step == 13) {
      require(
          m_serverSettings.open && m_serverSettings.guild == m_guildId && m_serverSettings.page == "general",
          "A native settings submenu opens the requested management page"
      );
      closeServerSettings(true);
      api::GuildRoleUpdate owner{.guild = background};
      owner.owner = m_account.id;
      m_roleDirectory.apply({false, {owner}});
      m_preferences.developerMode = false;
      open(background);
    } else if (step == 14) {
      require(
          rootCommand(Kind::Leave) < 0 && rootCommand(Kind::CreateCategory) >= 0 && rootCommand(Kind::Security) >= 0,
          "Server owners retain management actions and cannot leave their own server"
      );
      serverMenuEntry(rootCommand(Kind::CreateCategory));
      require(
          m_dialogAction
              && m_dialogAction->guild == background
              && m_dialogAction->channelType == 4
              && m_guildId == "1"
              && m_channelId == "101",
          "Create Category targets the background server without selecting it"
      );
      closeDialog();
      open(background);
      const auto create = rootCommand(Kind::CreateCategory);
      api::GuildRoleUpdate revoked{.guild = background};
      revoked.owner = "11";
      m_roleDirectory.apply({false, {revoked}});
      serverMenuEntry(create);
      require(m_dialogKind.empty(), "Revoking management while a menu is open prevents raced create actions");
      open(background);
    } else if (step == 15)
      click(Kind::MoveMenu);
    else if (step == 16) {
      require(m_channelSubmenuEntries.size() == 3, "Move offers Ungrouped, existing folders and Create New Folder");
      child(Kind::NewFolder);
    } else if (step == 17) {
      require(
          m_dialogKind == "server-folder"
              && m_serverDialogGuild == background
              && m_guildId == "1"
              && m_channelId == "101",
          "Create New Folder opens the target server's editable draft without changing chat"
      );
      closeDialog();
      open(m_guildId);
      serverMenuEntry(rootCommand(Kind::Read));
      api::Event read;
      read.operation = api::Operation::Interaction;
      read.interaction = api::Interaction{
          .kind = api::InteractionKind::MarkGuildRead, .guild = m_guildId, .readStates = {{"103", "9002"}}
      };
      require(serverMenuInteractionResult(read), "Mark Server as Read has an explicit result path");
      require(
          m_localRead["103"] == "9002" && !m_unreadChannels.contains("103") && m_channelId == "101",
          "Mark Server as Read acknowledges cached unread channels without navigating"
      );
      openServerMenuDialog("server-privacy", background);
    } else if (step == 18) {
      require(
          m_dialogPending
              && m_dialogAction
              && m_dialogAction->kind == api::InteractionKind::GuildPrivacy
              && !m_dialogAction->guildPrivacy,
          "Privacy Settings loads the target server's actual account preferences before enabling controls"
      );
      api::Event loaded;
      loaded.operation = api::Operation::Interaction;
      loaded.interaction = m_dialogAction;
      loaded.guildPrivacy = api::GuildPrivacy{true, true, true, true};
      require(serverMenuDialogResult(loaded), "The matching privacy read initializes the retained native form");
      require(m_serverPrivacyLoaded && !m_dialogPending, "Loaded privacy settings become editable");
    } else if (step == 19) {
      require(m_dialogAction.has_value(), "The completed privacy read retains its identity until a save starts");
      const auto get = *m_dialogAction;
      const auto control = m_widgets.find("toggle:server-menu:direct-messages");
      require(
          control != m_widgets.end() && control->second && control->second->parent(),
          "The privacy dialog mounts the shared retained native toggle"
      );
      float x = 0, y = 0;
      Node::mapToScene(control->second, control->second->width() / 2, control->second->height() / 2, x, y);
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.released();
      require(m_serverPrivacyDraft.directMessages == false, "The native privacy toggle updates the target draft");
      submitServerMenuDialog();
      require(
          m_dialogPending
              && m_dialogAction
              && m_dialogAction->guildPrivacy
              && m_dialogAction->guildPrivacy->directMessages == false
              && m_dialogAction->revision != get.revision,
          "Saving privacy uses a fresh request identity and the explicit draft"
      );
      const auto save = *m_dialogAction;
      api::Event staleGet;
      staleGet.operation = api::Operation::Interaction;
      staleGet.interaction = get;
      staleGet.guildPrivacy = api::GuildPrivacy{true, true, true, true};
      require(
          !serverMenuDialogResult(staleGet) && m_dialogPending && m_serverPrivacyDraft.directMessages == false,
          "A delayed privacy read cannot replace the edited draft or dismiss a pending save"
      );
      api::Event failed;
      failed.operation = api::Operation::Interaction;
      failed.interaction = save;
      failed.error = "Synthetic privacy save failure";
      require(
          serverMenuDialogResult(failed)
              && !m_dialogPending
              && m_serverPrivacyDraft.directMessages == false
              && m_dialogError == failed.error,
          "A failed privacy save retains the user's edits and explicit retry feedback"
      );
      submitServerMenuDialog();
      require(
          m_dialogPending && m_dialogAction && m_dialogAction->revision != save.revision,
          "Retrying privacy uses a fresh generation"
      );
      const auto retry = *m_dialogAction;
      failed.error.clear();
      require(
          !serverMenuDialogResult(failed) && m_dialogPending,
          "The earlier save's delayed acknowledgement cannot close a retried form"
      );
      api::Event saved;
      saved.operation = api::Operation::Interaction;
      saved.interaction = retry;
      require(
          serverMenuDialogResult(saved) && m_dialogKind.empty() && m_guildId == "1" && m_channelId == "101",
          "Only the current privacy save acknowledgement closes the dialog without changing chat"
      );
    }
    std::clog << "Offline server menu step " << step + 1 << " passed\n";
    m_window.invalidate();
  }
} // namespace app
