#include "app/client_application.h"

#include <algorithm>

namespace app {
  namespace {
    bool submenu(ServerMenuKind kind) {
      return kind == ServerMenuKind::MuteMenu
          || kind == ServerMenuKind::NotifyMenu
          || kind == ServerMenuKind::SettingsMenu
          || kind == ServerMenuKind::MoveMenu
          || kind == ServerMenuKind::FolderMoveMenu;
    }
    bool hasFeature(const api::Guild& guild, const char* feature) {
      return std::ranges::find(guild.features, feature) != guild.features.end();
    }
  } // namespace
  std::optional<std::uint64_t> ClientApplication::serverGuildPermissions(const std::string& guild) const {
    if (std::ranges::find(m_guilds, guild, &api::Guild::id) == m_guilds.end())
      return {};
    api::Channel base;
    base.overwrites.emplace();
    return api::channelPermissions(
        guild, m_account.id, m_roleDirectory.owner(guild), m_roleDirectory.allRoles(guild),
        m_roleDirectory.memberships(guild, m_account.id), base
    );
  }
  const std::vector<api::Channel>& ClientApplication::serverChannels(const std::string& guild) const {
    if (!m_dmMode && guild == m_guildId)
      return m_channels;
    const auto at = m_guildChannels.find(guild);
    static const std::vector<api::Channel> empty;
    return at == m_guildChannels.end() ? empty : at->second;
  }
  const api::Channel* ClientApplication::serverChannel(const std::string& guild, const std::string& channel) const {
    const auto& channels = serverChannels(guild);
    const auto at = std::ranges::find(channels, channel, &api::Channel::id);
    return at == channels.end() ? nullptr : &*at;
  }
  std::optional<std::uint64_t>
  ClientApplication::serverChannelPermissions(const std::string& guild, const std::string& channel) const {
    const auto* target = serverChannel(guild, channel);
    if (!target)
      return {};
    return api::channelPermissions(
        guild, m_account.id, m_roleDirectory.owner(guild), m_roleDirectory.allRoles(guild),
        m_roleDirectory.memberships(guild, m_account.id), *target
    );
  }
  bool ClientApplication::serverChannelVisible(const api::Channel& channel) const {
    const auto guild = std::ranges::find(m_guilds, m_guildId, &api::Guild::id);
    const auto visible = [&](const api::Channel& item) {
      auto permission = serverChannelPermissions(m_guildId, item.isThread() ? item.parentId : item.id);
      if (!permission)
        permission = guildPermissions();
      return !permission || (*permission & api::viewChannel) || (item.isThread() && item.joined.value_or(false));
    };
    if (channel.type != 4) {
      if (!visible(channel))
        return false;
    } else if (!std::ranges::any_of(m_channels, [&](const auto& item) {
                 return item.parentId == channel.id && visible(item);
               })) {
      return false;
    }
    if (guild == m_guilds.end() || !hasFeature(*guild, "COMMUNITY"))
      return true;
    const auto flags = m_userDirectory.guildPreferences(m_guildId).flags.value_or(0);
    if (!(flags & api::guildOptInChannelsOn) || (flags & api::guildOptInChannelsOff))
      return true;
    const auto followed = [&](const api::Channel& item) {
      return item.id == m_channelId
          || m_unreadTimeline.mentions(item.id) > 0
          || (m_userDirectory.notificationFlags(item.id) & api::channelOptInEnabled)
          || (m_userDirectory.notificationFlags(item.parentId) & api::channelOptInEnabled)
          || std::ranges::find(m_channelListPreferences.pinned, item.id) != m_channelListPreferences.pinned.end();
    };
    if (channel.type != 4)
      return followed(channel);
    return followed(channel) || std::ranges::any_of(m_channels, [&](const auto& item) {
             return item.parentId == channel.id && followed(item);
           });
  }
  api::Interaction ClientApplication::guildReadAction(const std::vector<std::string>& guilds) const {
    api::Interaction action{.kind = api::InteractionKind::MarkGuildRead, .guild = guilds.empty() ? "" : guilds.front()};
    std::map<std::string, std::string> states;
    for (const auto& guild : guilds) {
      for (const auto& channel : serverChannels(guild))
        if (channel.type != 4 && !channel.lastMessageId.empty())
          states[channel.id] = channel.lastMessageId;
      for (const auto& [channel, source] : m_unreadGuilds)
        if (source == guild)
          if (const auto cursor = m_unreadTimeline.cursor(channel))
            states[channel] = cursor->latest;
    }
    for (const auto& [channel, latest] : states)
      if (!latest.empty())
        action.readStates.emplace_back(channel, latest);
    return action;
  }
  bool ClientApplication::openFolderContext(const Hit& target, float x, float y) {
    const auto folder = std::ranges::find(m_folders, target.id, &api::GuildFolder::id);
    if (target.id.empty() || folder == m_folders.end() || m_serverSettings.open)
      return false;
    const auto folderId = target.id;
    closeContext();
    m_contextEntries.clear();
    m_serverMenuCommands.clear();
    m_channelMenuCommands.clear();
    m_contextMessage.clear();
    m_contextChannel.clear();
    m_contextUser.clear();
    m_contextGuild.clear();
    const auto add = [&](ServerMenuKind kind, const std::string& label) {
      const auto id = serverMenuBase + static_cast<int>(m_serverMenuCommands.size());
      m_serverMenuCommands.push_back({kind, folderId});
      m_contextEntries.push_back({.id = id, .label = label, .hasSubmenu = submenu(kind)});
    };
    add(ServerMenuKind::FolderRead, "Mark Folder as Read");
    m_contextEntries.push_back({.separator = true});
    add(ServerMenuKind::FolderMoveMenu, "Move to…");
    m_contextEntries.push_back({.separator = true});
    add(ServerMenuKind::FolderSettings, "Folder Settings");
    add(ServerMenuKind::FolderCloseAll, "Close All Folders");
    m_contextQuickIndex = -1;
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_contextX = x;
    m_contextY = y;
    m_contextScroll = 0;
    m_contextOpen = m_contextDirty = true;
    m_window.invalidate();
    return true;
  }
  bool ClientApplication::folderMenuEntry(int id) {
    const auto command = m_serverMenuCommands[id - serverMenuBase];
    using Kind = ServerMenuKind;
    if (command.kind != Kind::FolderRead
        && command.kind != Kind::FolderMoveMenu
        && command.kind != Kind::FolderMove
        && command.kind != Kind::FolderSettings
        && command.kind != Kind::FolderCloseAll)
      return false;
    const auto at = std::ranges::find(m_folders, command.guild, &api::GuildFolder::id);
    if (at == m_folders.end() || at->id.empty()) {
      closeContext();
      return true;
    }
    if (command.kind == Kind::FolderMoveMenu) {
      const float center = m_contextControl ? (m_contextControl->rowTop(m_contextControl->highlightedIndex())
                                               + m_contextControl->rowBottom(m_contextControl->highlightedIndex()))
              / 2
                                            : Style::spaceLg;
      openServerSubmenu(id, center);
      return true;
    }
    const auto folder = *at;
    closeContext();
    if (command.kind == Kind::FolderCloseAll) {
      m_openFolders.clear();
      saveExpandedFolders();
    } else if (!folder.guildIds.empty()) {
      api::Interaction action{.guild = folder.guildIds.front()};
      if (command.kind == Kind::FolderSettings) {
        openServerMenuDialog("server-folder-settings", action.guild);
        m_serverDialogFolder = folder.id;
        m_dialogText = folder.name;
        constexpr char digits[] = "0123456789ABCDEF";
        m_serverFolderColor = "#";
        constexpr int colorNibbles = 6;
        for (int nibble = colorNibbles - 1; nibble >= 0; --nibble)
          m_serverFolderColor += digits[(folder.color >> (nibble * 4)) & 0xF];
      } else if (command.kind == Kind::FolderMove) {
        action.kind = api::InteractionKind::EditGuildFolder;
        action.folderChange =
            api::GuildFolderChange{.folder = folder.id, .position = static_cast<api::FolderPosition>(command.value)};
        startInteraction(action);
      } else {
        action = guildReadAction(folder.guildIds);
        if (!action.readStates.empty())
          startInteraction(action);
      }
    }
    m_window.invalidate();
    return true;
  }
  bool ClientApplication::openServerContext(const Hit& target, float x, float y) {
    const auto guild = std::ranges::find(m_guilds, target.id, &api::Guild::id);
    if (guild == m_guilds.end() || m_serverSettings.open)
      return false;
    closeContext();
    m_contextEntries.clear();
    m_serverMenuCommands.clear();
    m_channelMenuCommands.clear();
    m_contextMessage.clear();
    m_contextChannel.clear();
    m_contextUser.clear();
    m_contextGuild = guild->id;
    const auto permissions = serverGuildPermissions(guild->id).value_or(0);
    const auto add = [&](ServerMenuKind kind, const std::string& label, const char* icon = "",
                         bool danger = false) -> ContextMenuControlEntry& {
      const auto id = serverMenuBase + static_cast<int>(m_serverMenuCommands.size());
      m_serverMenuCommands.push_back({kind, guild->id});
      m_contextEntries.push_back(
          {.id = id, .label = label, .hasSubmenu = submenu(kind), .icon = icon, .danger = danger}
      );
      return m_contextEntries.back();
    };
    const auto separate = [&] {
      if (!m_contextEntries.empty() && !m_contextEntries.back().separator)
        m_contextEntries.push_back({.separator = true});
    };
    add(ServerMenuKind::Read, "Mark as Read", "checks");
    separate();
    const auto& channels = serverChannels(guild->id);
    const bool invite = std::ranges::any_of(channels, [&](const auto& channel) {
      return channel.type != 4
          && !channel.isThread()
          && (serverChannelPermissions(guild->id, channel.id).value_or(0) & 1);
    });
    if (invite || (permissions & 1))
      add(ServerMenuKind::Invite, "Invite to Server", "user-plus");
    separate();
    add(m_userDirectory.muted(guild->id) ? ServerMenuKind::Mute : ServerMenuKind::MuteMenu,
        m_userDirectory.muted(guild->id) ? "Unmute Server" : "Mute Server", "bell-off");
    add(ServerMenuKind::NotifyMenu, "Notification Settings", "bell");
    auto& hide = add(ServerMenuKind::HideMuted, "Hide Muted Channels");
    hide.checkmark = true;
    const auto settings = m_userDirectory.guildPreferences(guild->id);
    const auto local = guild->id == m_guildId && !m_dmMode ? m_channelListPreferences
                                                           : m_sessionStore.channelList(m_account.id, guild->id);
    hide.toggleState = settings.hideMutedChannels.value_or(local.hideMuted) ? 1 : 0;
    if (hasFeature(*guild, "COMMUNITY")) {
      auto& all = add(ServerMenuKind::ShowAll, "Show All Channels");
      all.checkmark = true;
      const auto flags = settings.flags.value_or(0);
      all.toggleState = !(flags & api::guildOptInChannelsOn) || (flags & api::guildOptInChannelsOff) ? 1 : 0;
    }
    separate();
    if (!serverSettingsPages(guild->id).empty())
      add(ServerMenuKind::SettingsMenu, "Server Settings", "settings");
    add(ServerMenuKind::Privacy, "Privacy Settings", "shield");
    add(ServerMenuKind::Profile, "Edit Per-server Profile", "user");
    if (permissions & (api::manageChannel | (1ULL << 33) | (1ULL << 44))) {
      separate();
      if (permissions & api::manageChannel) {
        add(ServerMenuKind::CreateChannel, "Create Channel", "hash");
        add(ServerMenuKind::CreateCategory, "Create Category", "folder-plus");
      }
      if (permissions & ((1ULL << 33) | (1ULL << 44)))
        add(ServerMenuKind::CreateEvent, "Create Event", "calendar");
    }
    separate();
    add(ServerMenuKind::MoveMenu, "Move to…", "folder");
    separate();
    if (permissions & (1ULL << 5))
      add(ServerMenuKind::Security, "Security Actions", "shield", true);
    if (m_roleDirectory.owner(guild->id) != m_account.id)
      add(ServerMenuKind::Leave, "Leave Server", "logout", true);
    if (m_preferences.developerMode || m_demo) {
      separate();
      add(ServerMenuKind::CopyId, "Copy Server ID", "id");
    }
    m_contextQuickIndex = -1;
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_contextX = x;
    m_contextY = y;
    m_contextScroll = 0;
    m_contextOpen = m_contextDirty = true;
    m_window.invalidate();
    return true;
  }
  void ClientApplication::openServerSubmenu(int id, float center) {
    const auto index = id - serverMenuBase;
    if (index < 0 || static_cast<std::size_t>(index) >= m_serverMenuCommands.size())
      return;
    const auto command = m_serverMenuCommands[index];
    if (!submenu(command.kind))
      return;
    std::size_t roots = 0;
    for (const auto& entry : m_contextEntries)
      if (entry.id >= serverMenuBase)
        roots = std::max(roots, static_cast<std::size_t>(entry.id - serverMenuBase + 1));
    m_serverMenuCommands.resize(roots);
    m_channelSubmenuEntries.clear();
    m_reactionChoices.clear();
    const auto add = [&](ServerMenuKind kind, const std::string& label, int value = 0, std::string argument = {},
                         bool checked = false) {
      const auto next = serverMenuBase + static_cast<int>(m_serverMenuCommands.size());
      m_serverMenuCommands.push_back({kind, command.guild, std::move(argument), value});
      m_channelSubmenuEntries.push_back(
          {.id = next,
           .label = label,
           .checkmark = kind == ServerMenuKind::Move,
           .radio = kind == ServerMenuKind::Notify,
           .toggleState = checked ? 1 : 0}
      );
      m_reactionChoices.emplace_back("", label);
    };
    using Kind = ServerMenuKind;
    if (command.kind == Kind::FolderMoveMenu) {
      using Position = api::FolderPosition;
      add(Kind::FolderMove, "Top", static_cast<int>(Position::Top));
      add(Kind::FolderMove, "Up", static_cast<int>(Position::Up));
      add(Kind::FolderMove, "Down", static_cast<int>(Position::Down));
      add(Kind::FolderMove, "Bottom", static_cast<int>(Position::Bottom));
    } else if (command.kind == Kind::MuteMenu) {
      for (const auto& [duration, label] : std::vector<std::pair<int, std::string>>{
               {900, "For 15 Minutes"},
               {3600, "For 1 Hour"},
               {10800, "For 3 Hours"},
               {28800, "For 8 Hours"},
               {86400, "For 24 Hours"},
               {-1, "Until I Turn It Back On"}
           })
        add(Kind::Mute, label, duration);
    } else if (command.kind == Kind::NotifyMenu) {
      const int current = m_userDirectory.notificationLevel(command.guild);
      for (const auto& [level, label] :
           std::vector<std::pair<int, std::string>>{{0, "All Messages"}, {1, "Only @mentions"}, {2, "Nothing"}})
        add(Kind::Notify, label, level, {}, current == level);
      add(Kind::Notifications, "More Notification Settings…");
    } else if (command.kind == Kind::SettingsMenu) {
      for (const auto& [page, label] : serverSettingsPages(command.guild))
        add(Kind::Settings, label, 0, page);
    } else {
      std::string current;
      for (const auto& folder : m_guildLayout.folders)
        if (std::ranges::find(folder.guildIds, command.guild) != folder.guildIds.end())
          current = folder.id;
      add(Kind::Move, "No Folder", 0, "", current.empty());
      for (const auto& folder : m_guildLayout.folders)
        if (!folder.id.empty())
          add(Kind::Move, folder.name.empty() ? "Unnamed Folder" : folder.name, 0, folder.id, current == folder.id);
      add(Kind::NewFolder, "Create New Folder…");
    }
    if (m_reactionChoices.empty())
      return;
    m_channelSubmenu = true;
    m_reactionSubmenu = m_reactionSubmenuDirty = true;
    m_reactionSubmenuFocus = false;
    m_reactionSubmenuScroll = 0;
    m_reactionSubmenuAnchor = center;
    m_window.invalidate();
  }
  bool ClientApplication::serverMenuEntry(int id) {
    if (id < serverMenuBase)
      return false;
    const auto index = static_cast<std::size_t>(id - serverMenuBase);
    if (index >= m_serverMenuCommands.size())
      return true;
    const auto command = m_serverMenuCommands[index];
    if (folderMenuEntry(id))
      return true;
    if (std::ranges::find(m_guilds, command.guild, &api::Guild::id) == m_guilds.end()) {
      closeContext();
      return true;
    }
    if (submenu(command.kind)) {
      float center = 16;
      if (m_contextControl)
        center = (m_contextControl->rowTop(m_contextControl->highlightedIndex())
                  + m_contextControl->rowBottom(m_contextControl->highlightedIndex()))
            / 2;
      openServerSubmenu(id, center);
      return true;
    }
    closeContext();
    const auto permissions = serverGuildPermissions(command.guild).value_or(0);
    using Kind = ServerMenuKind;
    api::Interaction action;
    action.guild = command.guild;
    if (command.kind == Kind::CopyId) {
      if (m_preferences.developerMode || m_demo)
        m_window.copy(command.guild);
    } else if (command.kind == Kind::Privacy)
      openServerMenuDialog("server-privacy", command.guild);
    else if (command.kind == Kind::Profile)
      openServerProfile(command.guild);
    else if (command.kind == Kind::Notifications)
      openServerMenuDialog("server-notifications", command.guild);
    else if (command.kind == Kind::Leave) {
      if (m_roleDirectory.owner(command.guild) != m_account.id)
        openServerMenuDialog("server-leave", command.guild);
    } else if (command.kind == Kind::NewFolder)
      openServerMenuDialog("server-folder", command.guild);
    else if (command.kind == Kind::Settings || command.kind == Kind::Security || command.kind == Kind::CreateEvent) {
      const auto page = command.kind == Kind::Security ? "security"
          : command.kind == Kind::CreateEvent          ? "events"
                                                       : command.argument;
      const auto pages = serverSettingsPages(command.guild);
      if (std::ranges::none_of(pages, [&](const auto& value) { return value.first == page; }))
        return true;
      openServerSettings(command.guild);
      selectServerSettingsPage(page);
      if (command.kind == Kind::CreateEvent)
        serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "new"});
    } else if (command.kind == Kind::CreateChannel || command.kind == Kind::CreateCategory) {
      if (!(permissions & api::manageChannel))
        return true;
      action.kind = api::InteractionKind::CreateChannel;
      action.channelType = command.kind == Kind::CreateCategory ? 4 : 0;
      openChannelCreate(action);
    } else if (command.kind == Kind::Invite) {
      bool opened = false;
      for (const auto& channel : serverChannels(command.guild))
        if (channel.type != 4
            && !channel.isThread()
            && (serverChannelPermissions(command.guild, channel.id).value_or(0) & 1)) {
          action.kind = api::InteractionKind::CreateInvite;
          action.channel = channel.id;
          openChannelInvite(action);
          opened = true;
          break;
        }
      if (!opened && (permissions & 1)) {
        action.kind = api::InteractionKind::CreateInvite;
        openChannelInvite(action);
        if (!m_demo && m_client.channels(command.guild)) {
          m_serverInvitePending = command.guild;
          m_dialogError = "Loading this server’s invite channels…";
        }
      }
    } else if (command.kind == Kind::Move) {
      action.kind = api::InteractionKind::MoveGuild;
      action.text = command.argument;
      startInteraction(action);
    } else if (command.kind == Kind::Read) {
      action = guildReadAction({command.guild});
      if (!action.readStates.empty())
        startInteraction(action);
    } else {
      action.kind = api::InteractionKind::GuildNotifications;
      api::GuildNotification patch;
      if (command.kind == Kind::Mute) {
        patch.muted = command.value != 0;
        action.muteDuration = command.value == 0 ? -1 : command.value;
      } else if (command.kind == Kind::Notify)
        patch.level = command.value;
      else if (command.kind == Kind::HideMuted) {
        const auto current = m_userDirectory.guildPreferences(command.guild);
        const auto local = command.guild == m_guildId && !m_dmMode
            ? m_channelListPreferences
            : m_sessionStore.channelList(m_account.id, command.guild);
        patch.hideMutedChannels = !current.hideMutedChannels.value_or(local.hideMuted);
      } else if (command.kind == Kind::ShowAll) {
        const auto current = m_userDirectory.guildPreferences(command.guild).flags.value_or(0);
        const bool all = !(current & api::guildOptInChannelsOn) || (current & api::guildOptInChannelsOff);
        action.notificationFlagsMask = api::guildOptInChannelsOn | api::guildOptInChannelsOff;
        patch.flags = all ? api::guildOptInChannelsOn : api::guildOptInChannelsOff;
      } else
        return true;
      action.guildNotification = patch;
      startInteraction(action);
    }
    m_window.invalidate();
    return true;
  }
  bool ClientApplication::serverMenuInteractionResult(const api::Event& event) {
    if (!event.interaction)
      return false;
    const auto& action = *event.interaction;
    using Kind = api::InteractionKind;
    if (action.kind != Kind::GuildNotifications
        && action.kind != Kind::MarkGuildRead
        && action.kind != Kind::MoveGuild
        && action.kind != Kind::EditGuildFolder
        && action.kind != Kind::ReorderGuilds
        && action.kind != Kind::LeaveGuild
        && action.kind != Kind::GuildPrivacy)
      return false;
    if (action.kind == Kind::ReorderGuilds)
      m_railSaving = false;
    const bool dialog = serverMenuDialogResult(event);
    if (!event.error.empty()) {
      if (!dialog)
        m_status = event.error;
      m_window.invalidate();
      return true;
    }
    if (std::ranges::find(m_guilds, action.guild, &api::Guild::id) == m_guilds.end())
      return true;
    if (action.kind == Kind::GuildNotifications
        && action.stateRevision == m_userDirectory.guildNotificationRevision(action.guild)) {
      m_userDirectory.apply(event.users);
      scheduleMuteExpiry();
      const auto prefs = m_userDirectory.guildPreferences(action.guild);
      if (prefs.hideMutedChannels) {
        auto local = action.guild == m_guildId && !m_dmMode ? m_channelListPreferences
                                                            : m_sessionStore.channelList(m_account.id, action.guild);
        local.hideMuted = *prefs.hideMutedChannels;
        if (!m_dmMode && action.guild == m_guildId)
          m_channelListPreferences = local;
        if (!m_demo)
          m_sessionStore.saveChannelList(m_account.id, action.guild, local);
      }
      m_scrolling.stop(m_channelScroll);
    } else if (action.kind == Kind::MarkGuildRead) {
      for (const auto& [channel, latest] : action.readStates) {
        cancelAutomaticRead(channel);
        m_localRead[channel] = latest;
        m_unreadTimeline.acknowledged(channel, latest);
        if (!m_unreadTimeline.cursor(channel))
          m_unreadChannels.erase(channel);
      }
    } else if (
        (action.kind == Kind::MoveGuild || action.kind == Kind::EditGuildFolder || action.kind == Kind::ReorderGuilds)
        && action.stateRevision == m_guildLayoutRevision
    ) {
      ++m_guildLayoutRevision;
      m_guildLayout = event.layout;
      arrangeGuildFolders();
    } else if (action.kind == Kind::LeaveGuild) {
      std::set<std::string> removed;
      for (const auto& channel : serverChannels(action.guild))
        removed.insert(channel.id);
      for (const auto& channel : m_threads.list(action.guild))
        removed.insert(channel.id);
      for (const auto& [channel, guild] : m_unreadGuilds)
        if (guild == action.guild)
          removed.insert(channel);
      if (!m_dmMode && m_guildId == action.guild)
        showDirectMessages();
      for (const auto& channel : removed) {
        m_channelMessages.erase(channel);
        m_channelHistoryState.erase(channel);
        m_channelMembers.erase(channel);
        m_messageLog.clearChannel(channel);
        m_unreadChannels.erase(channel);
        m_unreadGuilds.erase(channel);
        cancelAutomaticRead(channel);
        m_localRead.erase(channel);
        m_channelDetailRequests.erase(channel);
        m_channelDetailsLoaded.erase(channel);
        m_channelDetailErrors.erase(channel);
        m_forumFirstMessages.erase(channel);
        m_drafts.erase(channel);
        m_replies.erase(channel);
        std::erase(m_channelCacheOrder, channel);
        if (m_chatPreloadChannel == channel || m_chatPreloadHover == channel)
          clearChatPreload();
      }
      m_lastChannels.erase(action.guild);
      m_roleDirectory.apply({false, {{.guild = action.guild, .removed = true}}});
      std::erase_if(m_guilds, [&](const auto& guild) { return guild.id == action.guild; });
      m_guildChannels.erase(action.guild);
      m_guildUpdates.erase(action.guild);
      arrangeGuildFolders();
      if (!m_dmMode && m_guildId == action.guild)
        showDirectMessages();
    }
    m_window.invalidate();
    return true;
  }

  void ClientApplication::resumeServerInvite(const api::Event& event) {
    if (m_serverInvitePending.empty() || event.resource != m_serverInvitePending)
      return;
    m_serverInvitePending.clear();
    if (m_dialogKind != "sidebar-invite"
        || !m_dialogAction
        || m_dialogAction->guild != event.resource
        || !m_dialogAction->channel.empty())
      return;
    if (!event.error.empty()) {
      m_dialogError = event.error;
      m_window.invalidate();
      return;
    }
    for (const auto& channel : serverChannels(event.resource))
      if (channel.type != 4
          && !channel.isThread()
          && (serverChannelPermissions(event.resource, channel.id).value_or(0) & 1)) {
        m_dialogAction->channel = channel.id;
        generateChannelInvite();
        return;
      }
    m_dialogError = "You do not have permission to invite people to an available channel in this server.";
    m_window.invalidate();
  }
} // namespace app
