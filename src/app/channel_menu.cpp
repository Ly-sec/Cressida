#include "app/client_application.h"
#include "app/theme.h"
#include "render/scene/rect_node.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;
  namespace {
    constexpr std::uint64_t invitePermission = 1;
    bool submenu(ChannelMenuKind kind) {
      return kind == ChannelMenuKind::MuteMenu
          || kind == ChannelMenuKind::NotifyMenu
          || kind == ChannelMenuKind::MoveMenu;
    }
  } // namespace
  std::optional<std::uint64_t> ClientApplication::channelPermissionsFor(const std::string& id) const {
    const auto at = std::ranges::find(m_channels, id, &api::Channel::id);
    if (m_dmMode || at == m_channels.end())
      return {};
    return api::channelPermissions(
        m_guildId, m_account.id, m_roleDirectory.owner(m_guildId), m_roleDirectory.allRoles(m_guildId),
        m_roleDirectory.memberships(m_guildId, m_account.id), *at
    );
  }
  std::optional<std::uint64_t> ClientApplication::guildPermissions() const {
    api::Channel base;
    base.overwrites.emplace();
    return api::channelPermissions(
        m_guildId, m_account.id, m_roleDirectory.owner(m_guildId), m_roleDirectory.allRoles(m_guildId),
        m_roleDirectory.memberships(m_guildId, m_account.id), base
    );
  }
  void ClientApplication::scheduleMuteExpiry() {
    m_channelMuteTimer.stop();
    if (const auto end = m_userDirectory.nextMuteExpiry())
      m_channelMuteTimer.start(
          std::max(
              std::chrono::milliseconds(1),
              std::chrono::duration_cast<std::chrono::milliseconds>(*end - std::chrono::system_clock::now())
                  + std::chrono::milliseconds(1)
          ),
          [this] {
            m_window.invalidate();
            scheduleMuteExpiry();
          }
      );
  }
  bool ClientApplication::channelMuted(const std::string& id) const {
    if (m_userDirectory.muted(id))
      return true;
    const std::vector<api::Channel>* channels = &m_channels;
    auto channel = std::ranges::find(*channels, id, &api::Channel::id);
    if (channel == channels->end()) {
      const auto location = m_unreadGuilds.find(id);
      if (location == m_unreadGuilds.end())
        return false;
      const auto guild = m_guildChannels.find(location->second);
      if (guild == m_guildChannels.end())
        return false;
      channels = &guild->second;
      channel = std::ranges::find(*channels, id, &api::Channel::id);
    }
    for (int depth = 0; depth < 4 && channel != channels->end() && !channel->parentId.empty(); ++depth) {
      const auto parent = channel->parentId;
      if (m_userDirectory.muted(parent))
        return true;
      channel = std::ranges::find(*channels, parent, &api::Channel::id);
    }
    return false;
  }
  bool ClientApplication::openChannelContext(const Hit& target, float x, float y) {
    if (m_dmMode || m_guildId.empty())
      return false;
    const auto found = std::ranges::find(m_channels, target.id, &api::Channel::id);
    const api::Channel* channel = target.action == Action::ChannelMenu && found != m_channels.end() ? &*found : nullptr;
    if (target.action == Action::ChannelMenu && !channel)
      return false;
    m_channelSubmenu = m_reactionSubmenu = m_reactionSubmenuFocus = false;
    m_scrolling.stop(m_contextScroll);
    m_contextScroll = 0;
    m_channelSubmenuEntries.clear();
    m_reactionChoices.clear();
    m_contextEntries.clear();
    m_channelMenuCommands.clear();
    m_contextChannel = channel ? channel->id : "";
    m_contextMessage.clear();
    m_contextUser.clear();
    const auto permissions = channel ? channelPermissionsFor(channel->id).value_or(0) : guildPermissions().value_or(0);
    const auto guild = guildPermissions().value_or(0);
    const bool category = channel && channel->type == 4;
    const auto add = [&](ChannelMenuKind kind, const std::string& label, const char* icon = "",
                         bool danger = false) -> ContextMenuControlEntry& {
      const int id = channelMenuBase + static_cast<int>(m_channelMenuCommands.size());
      m_channelMenuCommands.push_back({kind, m_guildId, channel ? channel->id : ""});
      m_contextEntries.push_back(
          {.id = id, .label = label, .hasSubmenu = submenu(kind), .icon = icon, .danger = danger}
      );
      return m_contextEntries.back();
    };
    const auto separate = [&] {
      if (!m_contextEntries.empty() && !m_contextEntries.back().separator)
        m_contextEntries.push_back({.separator = true});
    };
    if (!channel) {
      auto& hide = add(ChannelMenuKind::HideMuted, "Hide Muted Channels");
      hide.checkmark = true;
      hide.toggleState = m_channelListPreferences.hideMuted ? 1 : 0;
      separate();
      if (guild & api::manageChannel) {
        add(ChannelMenuKind::CreateText, "Create Channel", "hash");
        add(ChannelMenuKind::CreateCategory, "Create Category", "folder-plus");
      }
      if (std::ranges::any_of(m_channels, [&](const auto& value) {
            return value.type != 4
                && !value.isThread()
                && (channelPermissionsFor(value.id).value_or(0) & invitePermission);
          }))
        add(ChannelMenuKind::Invite, "Invite to Server", "user-plus");
    } else {
      if (!category) {
        add(ChannelMenuKind::Read, "Mark as Read", "checks");
        if (m_preferences.logDeletedMessages || m_preferences.logEditHistory)
          add(ChannelMenuKind::ClearLog, "Clear Message Log", "eraser", true);
        separate();
        if (permissions & invitePermission)
          add(ChannelMenuKind::Invite, "Invite to Channel", "user-plus");
        const bool pinned =
            std::ranges::find(m_channelListPreferences.pinned, channel->id) != m_channelListPreferences.pinned.end();
        add(ChannelMenuKind::Pin, pinned ? "Unpin Channel" : "Pin Channel to Top", "pin");
        add(ChannelMenuKind::CopyLink, "Copy Link", "link");
        separate();
      } else {
        add(ChannelMenuKind::Read, "Mark as Read", "checks");
        separate();
      }
      add(m_userDirectory.muted(channel->id) ? ChannelMenuKind::Mute : ChannelMenuKind::MuteMenu,
          m_userDirectory.muted(channel->id) ? (category ? "Unmute Category" : "Unmute Channel")
              : category                     ? "Mute Category"
                                             : "Mute Channel",
          "bell-off");
      add(ChannelMenuKind::NotifyMenu, "Notification Settings", "bell");
      if (!channel->isThread() && !category && (permissions & api::manageChannel)) {
        separate();
        add(ChannelMenuKind::MoveMenu, "Move to…", "folder");
      }
      if (!channel->isThread() && (permissions & (api::manageChannel | api::managePermissions))) {
        separate();
        add(ChannelMenuKind::Edit, category ? "Edit Category" : "Edit Channel", "pencil");
        if (!category && (permissions & api::manageChannel) && (guild & api::manageChannel))
          add(ChannelMenuKind::Duplicate, "Duplicate Channel", "copy");
      }
      const auto createPermissions = category ? permissions
          : channel->parentId.empty()         ? guild
                                              : channelPermissionsFor(channel->parentId).value_or(0);
      if (createPermissions & api::manageChannel)
        add(ChannelMenuKind::CreateText, "Create Text Channel", "hash");
      if (!channel->isThread() && (permissions & api::manageChannel))
        add(ChannelMenuKind::Delete, category ? "Delete Category" : "Delete Channel", "trash", true);
      separate();
      if (m_preferences.developerMode || m_demo)
        add(ChannelMenuKind::CopyId, "Copy Channel ID", "id");
      if (!channel->overwrites
          && !m_demo
          && !m_channelDetailRequests.contains(channel->id)
          && m_client.channelDetails(channel->id))
        m_channelDetailRequests.insert(channel->id);
    }
    while (!m_contextEntries.empty() && m_contextEntries.back().separator)
      m_contextEntries.pop_back();
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_emojiMenu = m_presenceMenu = false;
    m_contextQuickIndex = -1;
    m_contextX = x;
    m_contextY = y;
    m_contextOpen = m_contextDirty = true;
    m_window.invalidate();
    return true;
  }
  void ClientApplication::openChannelSubmenu(int id, float center) {
    const auto index = id - channelMenuBase;
    if (index < 0 || static_cast<std::size_t>(index) >= m_channelMenuCommands.size())
      return;
    const auto command = m_channelMenuCommands[index];
    if (!submenu(command.kind))
      return;
    std::size_t roots = 0;
    for (const auto& entry : m_contextEntries)
      if (entry.id >= channelMenuBase)
        roots = std::max(roots, static_cast<std::size_t>(entry.id - channelMenuBase + 1));
    m_channelMenuCommands.resize(roots);
    m_channelSubmenuEntries.clear();
    m_reactionChoices.clear();
    const auto add = [&](ChannelMenuKind kind, const std::string& label, int value, std::string argument = {},
                         bool checked = false) {
      const int next = channelMenuBase + static_cast<int>(m_channelMenuCommands.size());
      m_channelMenuCommands.push_back({kind, command.guild, command.channel, std::move(argument), value});
      m_channelSubmenuEntries.push_back(
          {.id = next,
           .label = label,
           .checkmark = kind == ChannelMenuKind::Move,
           .radio = kind == ChannelMenuKind::Notify,
           .toggleState = checked ? 1 : 0}
      );
      m_reactionChoices.emplace_back("", label);
    };
    if (command.kind == ChannelMenuKind::MuteMenu) {
      if (m_userDirectory.muted(command.channel))
        add(ChannelMenuKind::Mute, "Unmute Channel", 0);
      else {
        add(ChannelMenuKind::Mute, "For 15 Minutes", 900);
        add(ChannelMenuKind::Mute, "For 1 Hour", 3600);
        add(ChannelMenuKind::Mute, "For 3 Hours", 10800);
        add(ChannelMenuKind::Mute, "For 8 Hours", 28800);
        add(ChannelMenuKind::Mute, "For 24 Hours", 86400);
        add(ChannelMenuKind::Mute, "Until I Turn It Back On", -1);
      }
    } else if (command.kind == ChannelMenuKind::NotifyMenu) {
      for (const auto& [level, label] : std::vector<std::pair<int, std::string>>{
               {3, "Use Server Default"}, {0, "All Messages"}, {1, "Only @mentions"}, {2, "Nothing"}
           })
        add(ChannelMenuKind::Notify, label, level, {}, m_userDirectory.notificationLevel(command.channel) == level);
    } else {
      const auto source = std::ranges::find(m_channels, command.channel, &api::Channel::id);
      if (guildPermissions().value_or(0) & api::manageChannel)
        add(ChannelMenuKind::Move, "No Category", 0, "", source != m_channels.end() && source->parentId.empty());
      for (const auto& category : m_channels)
        if (category.type == 4 && (channelPermissionsFor(category.id).value_or(0) & api::manageChannel))
          add(ChannelMenuKind::Move, category.name, 0, category.id,
              source != m_channels.end() && source->parentId == category.id);
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
  bool ClientApplication::channelMenuEntry(int id) {
    if (id < channelMenuBase)
      return false;
    const auto index = static_cast<std::size_t>(id - channelMenuBase);
    if (index >= m_channelMenuCommands.size())
      return true;
    const auto command = m_channelMenuCommands[index];
    if (command.guild != m_guildId || m_dmMode) {
      closeContext();
      return true;
    }
    if (submenu(command.kind)) {
      float center = 16;
      if (m_contextControl)
        center = (m_contextControl->rowTop(m_contextControl->highlightedIndex())
                  + m_contextControl->rowBottom(m_contextControl->highlightedIndex()))
            / 2;
      openChannelSubmenu(id, center);
      return true;
    }
    closeContext();
    const auto found = std::ranges::find(m_channels, command.channel, &api::Channel::id);
    const api::Channel* channel = found == m_channels.end() ? nullptr : &*found;
    if (!command.channel.empty() && (!channel || m_deletedChannels.contains(command.channel)))
      return true;
    const auto permissions = channelPermissionsFor(command.channel).value_or(0);
    const auto guild = guildPermissions().value_or(0);
    const auto fail = [&](const std::string& error) {
      closeDialog();
      m_dialogKind = "sidebar-error";
      m_dialogTitle = "Channel action";
      m_dialogError = error;
      m_window.invalidate();
    };
    using Kind = ChannelMenuKind;
    if ((command.kind == Kind::Edit && !(permissions & (api::manageChannel | api::managePermissions)))
        || ((command.kind == Kind::Delete || command.kind == Kind::Move || command.kind == Kind::Duplicate)
            && !(permissions & api::manageChannel))) {
      fail("You no longer have permission to manage this channel.");
      return true;
    }
    if (command.kind == Kind::CopyId)
      m_window.copy(command.channel);
    else if (command.kind == Kind::CopyLink)
      m_window.copy("https://discord.com/channels/" + command.guild + "/" + command.channel);
    else if (command.kind == Kind::HideMuted || command.kind == Kind::Pin) {
      if (command.kind == Kind::HideMuted && !m_demo) {
        api::Interaction action{.kind = api::InteractionKind::GuildNotifications, .guild = command.guild};
        action.guildNotification = api::GuildNotification{.hideMutedChannels = !m_channelListPreferences.hideMuted};
        startInteraction(action);
        return true;
      }
      if (command.kind == Kind::HideMuted)
        m_channelListPreferences.hideMuted = !m_channelListPreferences.hideMuted;
      else if (
          std::ranges::find(m_channelListPreferences.pinned, command.channel) != m_channelListPreferences.pinned.end()
      )
        std::erase(m_channelListPreferences.pinned, command.channel);
      else if (m_channelListPreferences.pinned.size() < 200)
        m_channelListPreferences.pinned.insert(m_channelListPreferences.pinned.begin(), command.channel);
      else {
        fail("You can pin up to 200 channels in this server.");
        return true;
      }
      if (!m_demo)
        m_sessionStore.saveChannelList(m_account.id, command.guild, m_channelListPreferences);
      m_scrolling.stop(m_channelScroll);
    } else if (command.kind == Kind::Read) {
      std::vector<std::string> targets{command.channel};
      if (channel && channel->type == 4) {
        targets.clear();
        for (const auto& child : m_channels)
          if (child.parentId == command.channel)
            targets.push_back(child.id);
      }
      for (const auto& target : targets) {
        std::string latest;
        if (const auto cursor = m_unreadTimeline.cursor(target))
          latest = cursor->latest;
        if (latest.empty())
          if (const auto at = std::ranges::find(m_channels, target, &api::Channel::id); at != m_channels.end())
            latest = at->lastMessageId;
        if (latest.empty() && target == m_channelId) {
          markChannelRead(true);
          continue;
        }
        if (latest.empty())
          continue;
        acknowledgeChannelRead(target, latest);
      }
    } else if (command.kind == Kind::ClearLog) {
      m_messageLog.clearChannelContent(command.channel);
      if (const auto at = m_channelMessages.find(command.channel); at != m_channelMessages.end())
        m_messageLog.filter(command.channel, at->second);
      if (command.channel == m_channelId) {
        m_messageLog.filter(command.channel, m_messages);
        m_messageLayouts.clear();
        m_selectedMessage.clear();
        m_selectedText.clear();
        m_messageText.clear();
        m_chatEdit = {};
      }
    } else if (command.kind == Kind::Mute || command.kind == Kind::Notify || command.kind == Kind::Move) {
      api::Interaction action{
          .kind = command.kind == Kind::Notify ? api::InteractionKind::ChannelNotifications
              : command.kind == Kind::Move     ? api::InteractionKind::MoveChannel
              : command.value == 0             ? api::InteractionKind::Unmute
                                               : api::InteractionKind::Mute,
          .channel = command.channel,
          .guild = command.guild,
          .revision = ++m_channelFeatureSerial
      };
      if (command.kind == Kind::Move) {
        if (channel && channel->parentId == command.argument)
          return true;
        const auto destination = command.argument.empty() ? guild : channelPermissionsFor(command.argument).value_or(0);
        if (!(destination & api::manageChannel)) {
          fail("You cannot move this channel into that category.");
          return true;
        }
        action.parentId = command.argument;
      }
      action.notificationLevel = command.value;
      action.muteDuration = command.value;
      closeDialog();
      m_dialogTitle = command.kind == Kind::Move ? "Move channel" : "Channel notifications";
      m_dialogAction = action;
      if (command.kind == Kind::Move && !command.argument.empty() && channel && channel->parentId != command.argument) {
        const auto parent = std::ranges::find(m_channels, command.argument, &api::Channel::id);
        if (parent != m_channels.end()
            && parent->overwrites
            && channel->overwrites
            && !std::ranges::equal(
                *parent->overwrites, *channel->overwrites,
                [](const auto& a, const auto& b) {
                  return a.id == b.id && a.type == b.type && a.allow == b.allow && a.deny == b.deny;
                }
            )
            && (permissions & api::managePermissions)) {
          m_dialogKind = "sidebar-move";
          m_dialogTitle = "Sync permissions?";
          m_window.invalidate();
          return true;
        }
      }
      if (startInteraction(action))
        m_dialogPending = false;
      else {
        m_dialogKind = "sidebar-error";
        m_dialogError = m_status;
      }
    } else if (command.kind == Kind::Edit)
      openChannelDialog("channel-edit", command.channel);
    else {
      api::Interaction action{.channel = command.channel, .guild = command.guild, .revision = ++m_channelFeatureSerial};
      if (command.kind == Kind::CreateText || command.kind == Kind::CreateCategory) {
        const auto parent = command.kind == Kind::CreateText && channel
            ? (channel->type == 4 ? channel->id : channel->parentId)
            : std::string{};
        if (!((parent.empty() ? guild : channelPermissionsFor(parent).value_or(0)) & api::manageChannel)) {
          fail("You cannot create channels here.");
          return true;
        }
        action.kind = api::InteractionKind::CreateChannel;
        action.channelType = command.kind == Kind::CreateCategory ? 4 : 0;
        if (!parent.empty())
          action.parentId = parent;
      } else if (command.kind == Kind::Duplicate) {
        if (!(guild & api::manageChannel)) {
          fail("You cannot create channels in this server.");
          return true;
        }
        action.kind = api::InteractionKind::DuplicateChannel;
      } else if (command.kind == Kind::Delete)
        action.kind = api::InteractionKind::DeleteChannel;
      else if (command.kind == Kind::Invite) {
        if (!channel) {
          const auto at = std::ranges::find_if(m_channels, [&](const auto& candidate) {
            return candidate.type != 4
                && !candidate.isThread()
                && (channelPermissionsFor(candidate.id).value_or(0) & invitePermission);
          });
          if (at == m_channels.end()) {
            fail("No channel is available for creating an invite.");
            return true;
          }
          action.channel = at->id;
        } else if (!(permissions & invitePermission)) {
          fail("You cannot create invites for this channel.");
          return true;
        }
        action.kind = api::InteractionKind::CreateInvite;
      } else
        return true;
      closeDialog();
      if (action.kind == api::InteractionKind::CreateInvite) {
        openChannelInvite(action);
        m_window.invalidate();
        return true;
      }
      if (action.kind == api::InteractionKind::CreateChannel) {
        openChannelCreate(std::move(action));
        return true;
      }
      m_sidebarInviteUrl.clear();
      m_dialogAction = action;
      m_dialogKind = action.kind == api::InteractionKind::DeleteChannel ? "sidebar-delete"
          : action.kind == api::InteractionKind::CreateInvite           ? "sidebar-invite"
                                                                        : "sidebar-create";
      m_dialogTitle = action.kind == api::InteractionKind::DeleteChannel
          ? (channel && channel->type == 4 ? "Delete category?" : "Delete channel?")
          : action.kind == api::InteractionKind::DuplicateChannel ? "Duplicate channel"
          : action.kind == api::InteractionKind::CreateInvite     ? "Invite to server"
          : action.channelType == 4                               ? "Create category"
                                                                  : "Create Channel";
      m_dialogText = action.kind == api::InteractionKind::DuplicateChannel && channel ? channel->name + "-copy" : "";
      if (m_dialogKind == "sidebar-create")
        m_focusedInput = "dialog";
    }
    m_window.invalidate();
    return true;
  }
  void ClientApplication::submitSidebarDialog() {
    if (!m_dialogAction || m_dialogPending || !m_dialogKind.starts_with("sidebar-"))
      return;
    auto action = *m_dialogAction;
    if (std::ranges::find(m_guilds, action.guild, &api::Guild::id) == m_guilds.end()) {
      m_dialogError = "This server is no longer available.";
      m_window.invalidate();
      return;
    }
    const auto permissions = serverChannelPermissions(action.guild, action.channel).value_or(0);
    const auto guild = serverGuildPermissions(action.guild).value_or(0);
    bool allowed = false;
    using Kind = api::InteractionKind;
    if (action.kind == Kind::CreateChannel)
      allowed = (action.parentId && !action.parentId->empty()
                     ? serverChannelPermissions(action.guild, *action.parentId).value_or(0)
                     : guild)
          & api::manageChannel;
    else if (action.kind == Kind::DuplicateChannel)
      allowed = (permissions & api::manageChannel) && (guild & api::manageChannel);
    else if (action.kind == Kind::DeleteChannel)
      allowed = permissions & api::manageChannel;
    else if (action.kind == Kind::CreateInvite)
      allowed = permissions & invitePermission;
    if (!allowed) {
      m_dialogError = "Your permissions changed. Reopen the menu to try again.";
      m_window.invalidate();
      return;
    }
    action.text = m_dialogText;
    if (action.kind == Kind::CreateChannel) {
      if (action.channelType == 15) {
        const auto server = std::ranges::find(m_guilds, action.guild, &api::Guild::id);
        if (server == m_guilds.end() || std::ranges::find(server->features, "COMMUNITY") == server->features.end()) {
          m_dialogError = "Forum channels require a Community server.";
          m_window.invalidate();
          return;
        }
      }
      action.user = m_account.id;
      if (action.privateChannel) {
        const auto accessPermissions = action.parentId && !action.parentId->empty()
            ? serverChannelPermissions(action.guild, *action.parentId).value_or(0)
            : guild;
        if (!(accessPermissions & api::managePermissions)) {
          m_dialogError = "You need Manage Permissions to create a private channel.";
          return;
        }
        if (!m_channelCreateAccessStep && api::interactionRequest(action)) {
          m_channelCreateAccessStep = true;
          m_scrolling.stop(m_channelCreateScroll);
          m_channelCreateScroll = 0;
          m_focusedInput = "channel-create-access";
          m_window.invalidate();
          return;
        }
      }
    }
    if (action.kind == Kind::CreateInvite) {
      if (m_channelInvite.editing || !m_dialogError.empty())
        generateChannelInvite();
      return;
    }
    if (!api::interactionRequest(action)) {
      m_dialogError = "Enter a channel name containing 1–100 characters.";
      m_window.invalidate();
      return;
    }
    if (startInteraction(action)) {
      m_dialogAction = action;
      m_dialogPending = true;
      m_dialogError.clear();
    } else
      m_dialogError = m_status;
    m_window.invalidate();
  }
  bool ClientApplication::channelMenuAction(const Hit& hit) {
    if (channelInviteAction(hit) || channelCreateAction(hit))
      return true;
    if (hit.action == Action::ChannelMoveSync && m_dialogKind == "sidebar-move" && m_dialogAction) {
      if (m_dialogPending)
        return true;
      auto action = *m_dialogAction;
      if (!(channelPermissionsFor(action.channel).value_or(0) & api::manageChannel)
          || !action.parentId
          || !(channelPermissionsFor(*action.parentId).value_or(0) & api::manageChannel)) {
        m_dialogError = "Your permissions changed. Reopen the menu to try again.";
        m_window.invalidate();
        return true;
      }
      action.syncPermissions = hit.id == "sync";
      if (action.syncPermissions && !(channelPermissionsFor(action.channel).value_or(0) & api::managePermissions))
        return true;
      m_dialogAction = action;
      if (startInteraction(action))
        m_dialogPending = true;
      else
        m_dialogError = m_status;
      m_window.invalidate();
      return true;
    }
    if (hit.action == Action::SidebarSubmit) {
      submitSidebarDialog();
      return true;
    }
    if (hit.action == Action::SidebarCopyInvite) {
      if (!m_sidebarInviteUrl.empty())
        m_window.copy(m_sidebarInviteUrl);
      return true;
    }
    if (hit.action == Action::SubmitDialog && m_dialogKind.starts_with("sidebar-")) {
      submitSidebarDialog();
      return true;
    }
    return false;
  }
  void ClientApplication::drawSidebarDialog(Node& root, float w, float h) {
    if (m_dialogKind == "sidebar-invite") {
      drawChannelInvite(root, w, h);
      return;
    }
    if (m_dialogKind == "sidebar-create"
        && m_dialogAction
        && m_dialogAction->kind == api::InteractionKind::CreateChannel) {
      drawChannelCreate(root, w, h);
      return;
    }
    const bool duplicate = m_dialogKind == "sidebar-create";
    const bool deleting = m_dialogKind == "sidebar-delete";
    const bool moving = m_dialogKind == "sidebar-move";
    const float width = std::min(448.0F, w - Style::space2Xl);
    const float height = (duplicate ? 238 : moving ? 242 : deleting ? 216 : 194) + (m_dialogError.empty() ? 0 : 30);
    const float x = (w - width) / 2, y = std::max(42.0F, (h - height + 36) / 2);
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, Color{0, 0, 0, .65F});
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::None, {}});
    auto surface = std::make_unique<RectNode>();
    surface->setPosition(x, y);
    surface->setSize(width, height);
    surface->setStyle({.fill = background, .border = raised, .radius = Style::scaledRadiusXl(), .borderWidth = 1});
    root.addChild(std::move(surface));
    m_dialogBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    label(
        root, x + Style::spaceXl, y + ((Style::spaceXl - Style::spaceXxs) + Style::borderWidth), width - 80,
        m_dialogTitle, Style::fontSizeHeading, foreground, FontWeight::SemiBold
    );
    iconButton(
        root,
        {x + width - 42, y + Style::cardPadding, 28, 28, m_dialogPending ? Action::None : Action::CloseDialog, {}}, "x",
        "Close"
    );
    if (duplicate) {
      label(
          root, x + Style::spaceXl, y + 67, width - Style::spaceGroup, "Channel Name", Style::fontSizeCaption,
          foreground, FontWeight::Medium
      );
      rectangle(root, x + Style::spaceXl, y + 92, width - Style::spaceGroup, 36, sidebar, Style::hoverRadius);
      glyph(
          root, x + ((Style::space2Xl + Style::spaceXxs) + Style::borderWidth), y + 101, Style::glyphSizeMedium, "hash",
          muted
      );
      auto& name = inputControl(root, x + 57, y + 92, width - 81, 36, "dialog", m_dialogText, "new-channel");
      name.setFrameVisible(false);
      name.setHorizontalPadding(3);
      name.setEnabled(!m_dialogPending);
      label(
          root, x + Style::spaceXl, y + 139, width - Style::spaceGroup,
          "A new channel with the same settings and permissions.", Style::fontSizeDescription, muted
      );
    } else {
      std::string description;
      const auto target =
          m_dialogAction ? std::ranges::find(m_channels, m_dialogAction->channel, &api::Channel::id) : m_channels.end();
      if (deleting) {
        description = target != m_channels.end() && target->type == 4
            ? "Are you sure you want to delete " + target->name + "? Its channels will remain in the server."
            : "Are you sure you want to delete #"
                + (target == m_channels.end() ? "this channel" : target->name)
                + "? This cannot be undone.";
      } else if (moving) {
        const auto parent = m_dialogAction && m_dialogAction->parentId
            ? std::ranges::find(m_channels, *m_dialogAction->parentId, &api::Channel::id)
            : m_channels.end();
        description = "Do you want to sync this channel's permissions with "
            + (parent == m_channels.end() ? std::string("the category") : parent->name)
            + "? This replaces its permission overrides with the category's. You can also keep its current "
              "permissions.";
      }
      label(
          root, x + Style::spaceXl, y + 69, width - Style::spaceGroup, description, Style::fontSizeCaption, foreground,
          FontWeight::Normal, 4
      );
    }
    if (!m_dialogError.empty())
      label(
          root, x + Style::spaceXl, y + height - 90, width - Style::spaceGroup, m_dialogError, Style::fontSizeMini,
          danger, FontWeight::Normal, 2
      );
    const float buttonWidth = (width - 56) / 2;
    const float buttonY = y + height - 58;
    button(
        root,
        {x + Style::spaceXl, buttonY, buttonWidth, 36,
         m_dialogPending ? Action::None
             : moving    ? Action::ChannelMoveSync
                         : Action::CloseDialog,
         moving ? "keep" : ""},
        moving ? "Keep Current" : "Cancel", false, false, true, true
    );
    if (duplicate || deleting || moving) {
      const Hit hit{
          x + Style::space2Xl + buttonWidth,
          buttonY,
          buttonWidth,
          36,
          m_dialogPending ? Action::None
              : moving    ? Action::ChannelMoveSync
                          : Action::SidebarSubmit,
          moving ? "sync" : ""
      };
      if (deleting) {
        const bool over = !m_dialogPending && hit.contains(m_pointerX, m_pointerY);
        rectangle(
            root, hit.x, hit.y, hit.width, hit.height,
            m_dialogPending ? lerpColor(background, destructive, .4F)
                : over      ? lerpColor(destructive, foreground, .12F)
                            : destructive,
            Style::surfaceRadius
        );
        const bool category = m_dialogAction && std::ranges::any_of(m_channels, [&](const auto& channel) {
                                return channel.id == m_dialogAction->channel && channel.type == 4;
                              });
        const std::string caption = m_dialogPending ? "Deleting…" : category ? "Delete Category" : "Delete Channel";
        const auto metrics = m_window.target().renderer().measureText(caption, Style::fontSizeBody, FontWeight::Medium);
        label(
            root, hit.x + (hit.width - metrics.width) / 2, hit.y + (hit.height - (metrics.bottom - metrics.top)) / 2,
            metrics.width + Style::spaceXxs, caption, Style::fontSizeBody, m_dialogPending ? muted : rgbHex(0xFFFFFF),
            FontWeight::Medium
        );
        m_hits.push_back(hit);
      } else {
        button(
            root, hit,
            m_dialogPending ? "Saving…"
                : moving    ? "Sync Permissions"
                            : "Create Channel",
            true, false, true, true
        );
      }
    }

    m_tooltip.clear();
  }
  bool ClientApplication::sidebarInteraction(const api::Event& event, bool dialog) {
    const auto& action = *event.interaction;
    using Kind = api::InteractionKind;
    if (action.kind == Kind::Mute || action.kind == Kind::Unmute || action.kind == Kind::ChannelNotifications) {
      if (m_userDirectory.notificationRevision(action.channel) == action.stateRevision) {
        auto changes = event.users;
        if (changes.notifications.empty()) {
          if (action.kind == Kind::ChannelNotifications)
            changes.notifications[action.channel].level = action.notificationLevel;
          else
            changes.mutedChannels[action.channel] = action.kind == Kind::Mute;
        }
        m_userDirectory.apply(changes);
      }
      scheduleMuteExpiry();
      if (dialog)
        closeDialog();
      return true;
    }
    if (action.kind != Kind::CreateChannel
        && action.kind != Kind::DuplicateChannel
        && action.kind != Kind::DeleteChannel
        && action.kind != Kind::MoveChannel
        && action.kind != Kind::CreateInvite)
      return false;
    if (action.kind == Kind::CreateInvite) {
      if (dialog) {
        m_sidebarInviteUrl = event.inviteUrl;
        m_dialogPending = false;
      }
      return true;
    }
    if (action.kind == Kind::DeleteChannel) {
      api::Event removed{.operation = api::Operation::InvalidateChannels, .resource = action.guild};
      removed.removed = {action.channel};
      removeGuildChannels(removed);
    } else {
      auto& base = m_guildChannels[action.guild];
      for (const auto& fresh : event.channels) {
        if (m_deletedChannels.contains(fresh.id))
          continue;
        const auto at = std::ranges::find(base, fresh.id, &api::Channel::id);
        if (at == base.end())
          base.push_back(fresh);
        else
          *at = fresh;
      }
      base = api::orderChannels(std::move(base));
      if (action.guild == m_guildId && !m_dmMode)
        rebuildThreadChannels();
    }
    if (dialog)
      closeDialog();
    if (dialog
        && (action.kind == Kind::CreateChannel || action.kind == Kind::DuplicateChannel)
        && !event.channels.empty()
        && action.guild == m_guildId
        && event.channels.front().type != 4) {
      if (m_loadingChannels)
        m_restoreChannel = event.channels.front().id;
      selectChannel(event.channels.front().id);
    }
    return true;
  }
  void ClientApplication::prepareChannelMenuDemo(const std::string& view) {
    prepareChannelDemo(view);
    auto source = m_channels.front();
    source.parentId = "200";
    auto other = source;
    other.id = "103";
    other.name = "other-channel";
    other.overwrites.emplace();
    other.overwrites->clear();
    other.lastMessageId = "9002";
    auto denied = other;
    denied.id = "102";
    denied.name = "read-only";
    denied.overwrites = std::vector<api::PermissionOverwrite>{{"51", "0", "268435472", 0}};
    api::Channel category{"200", "INFO", 4};
    category.overwrites.emplace();
    auto destination = category;
    destination.id = "201";
    destination.name = "CHATS";
    destination.overwrites = std::vector<api::PermissionOverwrite>{{"52", "0", "2048", 0}};
    m_channels = {category, source, denied, other, destination};
    m_guildChannels[m_guildId] = m_channels;
    auto roles = m_roleDirectory.allRoles(m_guildId);
    for (auto& role : roles)
      if (role.id == m_guildId)
        role.permissions = "68609";
    api::GuildRoleUpdate update{.guild = m_guildId};
    update.roles = roles;
    m_roleDirectory.apply({false, {update}});
    m_preferences.logDeletedMessages = m_preferences.logEditHistory = true;
    m_unreadTimeline.snapshot({{"103", {"9000", "9002", 0}}});
    m_unreadTimeline.positioned();
    m_unreadChannels.insert("103");
    m_unreadGuilds["103"] = m_guildId;
    auto deleted = m_messages.front();
    deleted.id = "8800";
    deleted.channelId = "103";
    m_messageLog.capture("103", deleted);
    m_messageLog.removed("103", std::vector<std::string>{"8800"});
    m_channelMessages["103"] = {deleted};
    m_messageLog.filter("103", m_channelMessages["103"]);
    if (view.ends_with("-light"))
      m_preferences.theme = "light";
    if (view == "channel-menu-scroll-cycle")
      m_window.setUiScale(1.8F);
  }
  void ClientApplication::demoChannelMenuStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto command = [&](ChannelMenuKind kind) {
      for (const auto& entry : m_contextEntries) {
        const int index = entry.id - channelMenuBase;
        if (index >= 0
            && static_cast<std::size_t>(index) < m_channelMenuCommands.size()
            && m_channelMenuCommands[index].kind == kind)
          return entry.id;
      }
      return -1;
    };
    const auto click = [&](ChannelMenuKind kind) {
      const int id = command(kind);
      require(id >= 0, "Expected channel-menu command must be present");
      const auto at = std::ranges::find(m_contextEntries, id, &ContextMenuControlEntry::id);
      const auto index = static_cast<std::size_t>(at - m_contextEntries.begin());
      const float y = m_contextBounds.y + (m_contextControl->rowTop(index) + m_contextControl->rowBottom(index)) / 2;
      m_window.click(
          m_contextBounds.x + m_contextBounds.width / 2, y + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          272
      );
      m_window.released();
    };
    const auto open = [&](const std::string& id) {
      closeDialog();
      const auto at = std::ranges::find_if(m_contextTargets, [&](const auto& target) {
        return id.empty() ? target.action == Action::ChannelListMenu
                          : target.action == Action::ChannelMenu && target.id == id;
      });
      require(at != m_contextTargets.end(), "Right-click target must exist in the drawn sidebar");
      m_window.contextClick(
          at->x + Style::spaceDense,
          (id.empty() ? 590 : at->y + Style::spaceDense) + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
    };
    const auto child = [&](const std::string& label) {
      const auto at = std::ranges::find(m_channelSubmenuEntries, label, &ContextMenuControlEntry::label);
      require(
          at != m_channelSubmenuEntries.end() && m_reactionSubmenu, "Expected native submenu choice must be visible"
      );
      const auto index = static_cast<std::size_t>(at - m_channelSubmenuEntries.begin());
      m_window.click(
          m_reactionSubmenuBounds.x + 70,
          m_reactionSubmenuBounds.y
              + (m_reactionControl->rowTop(index) + m_reactionControl->rowBottom(index)) / 2
              + m_window.titlebarHeight()
              - (Style::space2Xl + Style::spaceXs),
          272
      );
      m_window.released();
    };
    const auto ack = [&](std::vector<api::Channel> channels = {}, std::string invite = {}) {
      require(m_dialogAction.has_value(), "The dialog must retain its explicit action");
      api::Event result;
      result.operation = api::Operation::Interaction;
      result.interaction = m_dialogAction;
      result.interaction->stateRevision = m_userDirectory.notificationRevision(result.interaction->channel);
      result.channels = std::move(channels);
      result.inviteUrl = std::move(invite);
      if (result.interaction->kind == api::InteractionKind::Mute)
        result.users.notifications[result.interaction->channel] = {{}, true, "2035-01-01T00:00:00Z"};
      if (result.interaction->kind == api::InteractionKind::ChannelNotifications)
        result.users.notifications[result.interaction->channel].level = result.interaction->notificationLevel;
      interactionResult(result);
    };
    using Kind = ChannelMenuKind;
    const auto step = m_demoInteractionStep++;
    if (m_demoOverlay == "channel-menu-scroll-cycle") {
      if (step == 0) {
        require(m_contextFullHeight > m_contextBounds.height, "A short window reserves a bounded menu viewport");
        for (std::size_t i = 0; i < m_contextEntries.size(); ++i) {
          if (m_contextEntries[m_contextControl->highlightedIndex()].id == command(Kind::CopyId))
            break;
          m_window.key({.symbol = XKB_KEY_Down});
        }
      } else if (step == 1) {
        require(
            m_contextScroll > 0
                && m_contextBounds.y + m_contextBounds.height
                    <= m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight() - Style::spaceSm,
            "Keyboard navigation reveals the last action while keeping the popup inside the window"
        );
        m_window.key({.symbol = XKB_KEY_Return});
      } else if (step == 2)
        require(!m_contextOpen, "The last menu action remains functional after scrolling");
      std::clog << "Offline compact channel menu step " << step + 1 << " passed\n";
      m_window.invalidate();
      return;
    }
    if (step == 0) {
      require(
          m_contextOpen && m_contextChannel == "103" && m_channelId == "101",
          "Channel context must target the right-clicked row without navigating"
      );
      require(
          command(Kind::Edit) >= 0 && command(Kind::Delete) >= 0 && command(Kind::Invite) >= 0,
          "Authorized channel actions are shown"
      );
      click(Kind::Pin);
    } else if (step == 1) {
      require(
          m_channelListPreferences.pinned == std::vector<std::string>{"103"},
          "Pin-to-top updates the local server preference"
      );
      open("103");
      channelMenuEntry(command(Kind::Read));
      require(
          !m_unreadChannels.contains("103") && m_localRead["103"] == "9002" && m_channelId == "101",
          "Mark-as-read uses the target's unread cursor without selecting it"
      );
      open("103");
      channelMenuEntry(command(Kind::ClearLog));
      require(
          m_messageLog.size() == 0 && m_channelMessages["103"].empty() && m_messageLog.known("103", "8800"),
          "Clear log forgets channel content while retaining deletion protection"
      );
      open("102");
    } else if (step == 2) {
      require(
          command(Kind::Edit) < 0
              && command(Kind::Delete) < 0
              && command(Kind::MoveMenu) < 0
              && command(Kind::Pin) >= 0,
          "A channel overwrite hides management commands independently of the selected channel"
      );
      open("103");
    } else if (step == 3)
      click(Kind::Edit);
    else if (step == 4) {
      require(
          m_dialogKind == "channel-edit" && m_channelEditOriginal.id == "103" && m_channelId == "101",
          "Edit opens the right-clicked channel's form"
      );
      auto fresh = *std::ranges::find(m_channels, "103", &api::Channel::id);
      fresh.name = "renamed";
      m_dialogAction = api::Interaction{
          .kind = api::InteractionKind::EditChannel,
          .channel = "103",
          .guild = m_guildId,
          .revision = ++m_channelFeatureSerial,
          .channelName = "renamed"
      };
      ack({fresh});
      require(m_channelId == "101", "Editing a sibling must not change the conversation");
      open("103");
    } else if (step == 5)
      click(Kind::MuteMenu);
    else if (step == 6) {
      require(m_channelSubmenuEntries.size() == 6, "Mute durations use a real submenu");
      child("For 15 Minutes");
    } else if (step == 7) {
      require(
          m_dialogAction && m_dialogAction->channel == "103" && m_dialogAction->muteDuration == 900,
          "Mute targets the chosen row and duration"
      );
      ack();
      require(
          m_userDirectory.muted("103") && m_channelMuteTimer.active(),
          "Acknowledged timed mutes schedule a native expiry wake"
      );
      api::UserUpdates categoryMute;
      categoryMute.notifications["200"].muted = true;
      m_userDirectory.apply(categoryMute);
      require(
          channelMuted("101") && !m_userDirectory.muted("101"),
          "Category mutes apply to children without fabricating a child override"
      );
      categoryMute.notifications["200"].muted = false;
      m_userDirectory.apply(categoryMute);
      open("103");
    } else if (step == 8) {
      require(
          command(Kind::Mute) >= 0 && command(Kind::MuteMenu) < 0,
          "Unmute is an immediate command rather than an extra submenu"
      );
      click(Kind::NotifyMenu);
    } else if (step == 9) {
      child("Only @mentions");
      ack();
      require(
          m_userDirectory.notificationLevel("103") == 1 && m_userDirectory.muted("103"),
          "Notification changes preserve mute preferences"
      );
      open("");
    } else if (step == 10)
      click(Kind::HideMuted);
    else if (step == 11) {
      require(
          m_channelListPreferences.hideMuted
              && std::ranges::none_of(
                  m_hits, [](const auto& hit) { return hit.action == Action::Channel && hit.id == "103"; }
              ),
          "Hide muted removes the row from both layout and hit targets"
      );
      open("");
    } else if (step == 12)
      click(Kind::HideMuted);
    else if (step == 13) {
      require(!m_channelListPreferences.hideMuted, "Hide-muted toggles back off");
      open("103");
    } else if (step == 14)
      click(Kind::MoveMenu);
    else if (step == 15) {
      child("CHATS");
      require(m_dialogAction && m_dialogAction->parentId == "201", "Move submenu retains the destination category");
      require(m_dialogKind == "sidebar-move", "Moving to different category permissions asks whether to sync");
      activate({0, 0, 0, 0, Action::ChannelMoveSync, "keep"});
      require(!m_dialogAction->syncPermissions, "Keeping permissions explicitly preserves the channel overwrites");
      auto moved = *std::ranges::find(m_channels, "103", &api::Channel::id);
      moved.parentId = "201";
      ack({moved});
      open("103");
    } else if (step == 16)
      click(Kind::Duplicate);
    else if (step == 17) {
      require(
          m_dialogKind == "sidebar-create" && m_dialogText == "renamed-copy", "Duplicate keeps an editable name draft"
      );
      auto copy = *std::ranges::find(m_channels, "103", &api::Channel::id);
      copy.id = "301";
      copy.name = m_dialogText;
      m_loadingChannels = true;
      m_restoreChannel = "101";
      ack({copy});
      require(
          m_channelId == "301" && m_restoreChannel == "301",
          "A pending channel reload preserves the newly created selection"
      );
      m_loadingChannels = false;
    } else if (step == 18)
      open("");
    else if (step == 19)
      click(Kind::CreateCategory);
    else if (step == 20) {
      require(
          m_dialogAction && m_dialogAction->channelType == 4,
          "Create category retains its type until explicit submission"
      );
      api::Channel category{"302", "NEW CATEGORY", 4};
      category.overwrites.emplace();
      ack({category});
      api::Event deletedCategory{.operation = api::Operation::InvalidateChannels, .resource = m_guildId};
      deletedCategory.removed = {"200"};
      removeGuildChannels(deletedCategory);
      const auto remaining = std::ranges::find(m_channels, "101", &api::Channel::id);
      require(
          remaining != m_channels.end() && remaining->parentId.empty() && !m_deletedChannels.contains("101"),
          "Deleting a category immediately unparents its channels without deleting them"
      );
      open("103");
    } else if (step == 21)
      click(Kind::Delete);
    else if (step == 22) {
      require(
          m_dialogKind == "sidebar-delete" && !m_deletedChannels.contains("103"),
          "Delete requires an explicit confirmation"
      );
      closeDialog();
      open("103");
    } else if (step == 23)
      click(Kind::Delete);
    else if (step == 24) {
      ack();
      require(
          m_deletedChannels.contains("103")
              && std::ranges::find(m_channels, "103", &api::Channel::id) == m_channels.end(),
          "Acknowledged deletion removes the row immediately"
      );
      open("");
    } else if (step == 25)
      click(Kind::Invite);
    else if (step == 26) {
      require(m_dialogKind == "sidebar-invite", "Invite generation requires an explicit native form action");
      ack({}, "https://discord.gg/offline-demo");
    } else if (step == 27) {
      require(
          m_sidebarInviteUrl == "https://discord.gg/offline-demo" && !m_dialogPending,
          "Only an acknowledged invite becomes copyable"
      );
      activate({0, 0, 0, 0, Action::SidebarCopyInvite, {}});
      open("301");
    } else if (step == 28) {
      const int remove = command(Kind::Delete);
      require(remove >= 0, "A management command exists before permission revocation");
      auto roles = m_roleDirectory.allRoles(m_guildId);
      for (auto& role : roles)
        if (role.id == "51")
          role.permissions = "0";
      api::GuildRoleUpdate update{.guild = m_guildId};
      update.roles = roles;
      m_roleDirectory.apply({false, {update}});
      channelMenuEntry(remove);
    } else if (step == 29) {
      require(m_dialogKind == "sidebar-error" && !m_dialogPending, "A stale menu cannot bypass revoked permissions");
      open("");
    } else if (step == 30) {
      require(
          command(Kind::CreateText) < 0 && command(Kind::CreateCategory) < 0 && command(Kind::HideMuted) >= 0,
          "Empty-space management options also follow current permissions"
      );
      const auto selectedGuild = m_guildId, selectedChannel = m_channelId;
      const std::string background = "109";
      m_guilds.push_back({.id = background, .name = "Background server"});
      api::GuildRoleUpdate roles{.guild = background};
      roles.owner = m_account.id;
      roles.roles = {{.id = "1091", .name = "Background role", .permissions = "0"}};
      m_roleDirectory.apply({false, {roles}});
      api::Channel backgroundChannel{"1092", "background-chat", 0};
      backgroundChannel.overwrites.emplace();
      m_guildChannels[background] = {backgroundChannel};
      openChannelInvite(
          {.kind = api::InteractionKind::CreateInvite, .channel = backgroundChannel.id, .guild = background}
      );
      require(
          m_dialogPending
              && m_dialogError.empty()
              && m_dialogAction->guild == background
              && m_dialogAction->channel == backgroundChannel.id
              && m_guildId == selectedGuild
              && m_channelId == selectedChannel,
          "Inviting to a background server uses its own channel and permissions without changing navigation"
      );
      openChannelCreate({.kind = api::InteractionKind::CreateChannel, .guild = background});
      channelCreateAction({0, 0, 0, 0, Action::ChannelCreateType, "15"});
      require(
          m_dialogAction->channelType == 0, "Background channel types follow the target server's Community feature"
      );
      channelCreateAction({0, 0, 0, 0, Action::ChannelCreatePrivate, {}});
      m_dialogText = "background-room";
      submitSidebarDialog();
      require(
          m_channelCreateAccessStep
              && m_dialogError.empty()
              && !m_dialogPending
              && m_dialogAction->guild == background
              && m_guildId == selectedGuild
              && m_channelId == selectedChannel,
          "Creating a private channel in a background server uses its permissions without selecting it"
      );
      channelCreateAction({0, 0, 0, 0, Action::ChannelCreateAccess, "0:50"});
      require(m_dialogAction->channelAccess.empty(), "A previous server's role cannot enter the creation draft");
      channelCreateAction({0, 0, 0, 0, Action::ChannelCreateAccess, "0:1091"});
      require(
          m_dialogAction->channelAccess.size() == 1 && m_dialogAction->channelAccess.front().id == "1091",
          "Private channel role selection follows the target server"
      );
      openChannelCreate({.kind = api::InteractionKind::CreateChannel, .guild = background, .channelType = 4});
      require(
          m_dialogAction->channelType == 4 && !m_channelCreateAccessStep && m_dialogText.empty(),
          "Background category creation starts a fresh draft"
      );
      closeDialog();
      m_guildChannels.erase(background);
      openChannelInvite(
          {.kind = api::InteractionKind::CreateInvite, .channel = backgroundChannel.id, .guild = background}
      );
      require(
          !m_dialogPending && !m_dialogError.empty(),
          "Missing background channel data produces an explicit invite error"
      );
      closeDialog();
    }
    std::clog << "Offline channel menu replay step " << step + 1 << " passed\n";
    m_window.invalidate();
  }
} // namespace app
