#include "app/automod_message.h"
#include "app/client_application.h"
#include "app/system_message.h"
#include "app/theme.h"
#include "core/files/resource_paths.h"
#include "render/scene/rect_node.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;
  namespace {
    std::string interactionKey(const api::Interaction& action) {
      auto key = std::to_string(static_cast<int>(action.kind))
          + ":"
          + action.channel
          + ":"
          + action.message
          + ":"
          + action.user
          + ":"
          + action.emoji
          + (action.burst ? ":burst" : ":normal");
      if (action.kind == api::InteractionKind::UserProfile
          || action.kind == api::InteractionKind::GuildProfile
          || action.kind == api::InteractionKind::JoinGuild
          || action.kind == api::InteractionKind::CreateThread
          || action.kind == api::InteractionKind::CreateForumPost)
        key += ":" + action.guild + ":" + std::to_string(action.revision);
      if (action.kind == api::InteractionKind::MessageButton || action.kind == api::InteractionKind::SubmitModal)
        key += ":" + action.customId + ":" + action.nonce;
      if (static_cast<int>(action.kind) >= static_cast<int>(api::InteractionKind::CreateChannel))
        key += ":" + action.guild + ":" + std::to_string(action.revision);
      if (action.kind == api::InteractionKind::EditChannel || action.kind == api::InteractionKind::GrantChannelAccess)
        key += ":" + std::to_string(action.revision);
      return key;
    }
  } // namespace
  api::Account ClientApplication::lookupUser(const std::string& id) const {
    if (m_account.id == id)
      return m_account;
    for (const auto& member : m_members)
      if (member.user.id == id)
        return member.user;
    for (const auto& channel : m_privateChannels)
      for (const auto& user : channel.recipients)
        if (user.id == id)
          return user;
    for (const auto& message : m_messages)
      if (message.authorId == id)
        return {id, message.author, message.avatar};
    for (const auto& user : m_userDirectory.relatedUsers())
      if (user.id == id)
        return user;
    return {id, "Unknown user"};
  }
  bool ClientApplication::blockedMessage(const api::Message& message) const {
    return (m_userDirectory.relationship(message.authorId) == 2 || m_userDirectory.ignored(message.authorId))
        && !m_revealedBlocked.contains(message.id);
  }
  void ClientApplication::closeContext() {
    m_contextOpen = false;
    m_imageContext.clear();
    m_scrolling.stop(m_contextScroll);
    if (m_channelSubmenu)
      m_reactionChoices.clear();
    m_channelSubmenu = false;
    m_channelSubmenuEntries.clear();
    m_reactionSubmenu = m_reactionSubmenuFocus = false;
    m_window.invalidate();
  }
  void ClientApplication::openContext(float x, float y) {
    if (m_completionQuery && m_completionBounds.contains(x, y))
      return;
    m_scrolling.stopAll();
    closeGifPicker();
    if (!m_viewImage.empty()) {
      openImageContext(x, y);
      return;
    }
    if (!m_connected || settingsVisible() || m_serverSettings.open || !m_viewImage.empty() || !m_dialogKind.empty())
      return;
    const auto target = std::find_if(m_contextTargets.rbegin(), m_contextTargets.rend(), [&](const auto& hit) {
      return hit.contains(x, y);
    });
    if (target == m_contextTargets.rend()) {
      closeContext();
      return;
    }
    m_reactionSubmenu = m_reactionSubmenuFocus = false;
    m_reactionChoices.clear();
    m_channelSubmenu = false;
    m_channelSubmenuEntries.clear();
    m_contextQuickIndex = -1;
    m_contextMessage.clear();
    m_contextUser.clear();
    m_contextChannel = m_channelId;
    m_contextGuild.clear();
    m_contextEntries.clear();
    if (target->action == Action::Folder) {
      openFolderContext(*target, x, y);
      return;
    }
    if (target->action == Action::GuildMenu) {
      openServerContext(*target, x, y);
      return;
    }
    if (target->action == Action::ChannelMenu || target->action == Action::ChannelListMenu) {
      openChannelContext(*target, x, y);
      return;
    }
    m_channelMenuCommands.clear();
    auto add = [&](Action action, std::string title) {
      ContextMenuControlEntry entry{static_cast<int>(action), std::move(title)};
      switch (action) {
      case Action::AddReaction:
        entry.icon = "mood-smile";
        entry.hasSubmenu = true;
        break;
      case Action::CreateThread:
        entry.icon = "messages";
        break;
      case Action::Reply:
        entry.icon = "corner-up-left";
        break;
      case Action::EditMessage:
      case Action::SetNickname:
        entry.icon = "pencil";
        break;
      case Action::DeleteMessage:
      case Action::RemoveEmbeds:
        entry.icon = "trash";
        entry.danger = true;
        break;
      case Action::RemoveFromLog:
      case Action::ClearEditHistory:
        entry.icon = "eraser";
        entry.danger = action == Action::RemoveFromLog;
        break;
      case Action::CopyMessage:
        entry.icon = "copy";
        break;
      case Action::CopyLink:
        entry.icon = "link";
        break;
      case Action::CopyId:
        entry.icon = "id";
        break;
      case Action::UserProfile:
        entry.icon = "user";
        break;
      case Action::OpenDm:
        entry.icon = "message-circle";
        break;
      case Action::MentionUser:
        entry.icon = "at";
        break;
      case Action::AddFriend:
        entry.icon = "user-plus";
        break;
      case Action::RemoveFriend:
        entry.icon = "user-minus";
        entry.danger = true;
        break;
      case Action::BlockUser:
        entry.icon = "ban";
        entry.danger = true;
        break;
      case Action::UnblockUser:
        entry.icon = "user-check";
        break;
      case Action::PinConversation:
      case Action::UnpinConversation:
        entry.icon = action == Action::PinConversation ? "pin" : "pinned-off";
        break;

      case Action::BookmarkMessage:
        entry.icon = "bookmark";
        break;
      case Action::RemindMessage:
        entry.icon = "bell";
        break;
      case Action::MuteConversation:
        entry.icon = "bell-off";
        break;
      case Action::UnmuteConversation:
        entry.icon = "bell";
        break;
      default:
        break;
      }
      m_contextEntries.push_back(std::move(entry));
    };
    auto separator = [&] {
      ContextMenuControlEntry entry;
      entry.separator = true;
      m_contextEntries.push_back(entry);
    };
    if (target->action == Action::MessageMenu) {
      const auto message = std::ranges::find(m_messages, target->id, &api::Message::id);
      if (message == m_messages.end())
        return;
      m_contextMessage = message->id;
      m_contextUser = message->authorId;
      if (!message->deleted) {
        add(Action::AddReaction, "Add Reaction");
        separator();
      }
      if (!message->deleted
          && message->authorId == m_account.id
          && message->webhookId.empty()
          && !message->forwarded
          && (message->stickers.empty() || !message->content.empty())
          && (message->type == 0 || message->type == 19)
          && !(message->flags & 64))
        add(Action::EditMessage, "Edit Message");
      if (api::canRemoveMessageEmbeds(*message, m_account.id, channelPermissionsFor(m_contextChannel)))
        add(Action::RemoveEmbeds, "Remove Embeds");
      if (!message->deleted && message->type != 7 && message->type != autoModerationActionType)
        add(Action::Reply, "Reply");
      const auto selected = std::ranges::find(m_channels, m_channelId, &api::Channel::id);
      if (!message->deleted
          && message->type != 7
          && message->type != autoModerationActionType
          && !m_dmMode
          && selected != m_channels.end()
          && (selected->type == 0 || selected->type == 5))
        add(Action::CreateThread,
            message->hasThread || message->thread || m_threads.find(m_guildId, message->id) ? "Open Thread"
                                                                                            : "Create Thread");

      if (!message->deleted && !(message->flags & 64)) {
        add(Action::BookmarkMessage, "Bookmark Message");
        add(Action::RemindMessage, "Remind Me");
      }
      add(Action::UserProfile, "View Author Profile");
      separator();
      add(Action::CopyMessage, "Copy Text");
      add(Action::CopyLink, "Copy Message Link");
      const bool retainedMessage = m_messageLog.retained(m_contextChannel, message->id);
      const bool rememberedEdits = !m_messageLog.edits(m_contextChannel, message->id).empty();
      if (retainedMessage || rememberedEdits) {
        separator();
        if (retainedMessage)
          add(Action::RemoveFromLog, "Remove from Log");
        if (rememberedEdits)
          add(Action::ClearEditHistory, "Clear Edit History");
      }
      if (!message->deleted && message->authorId == m_account.id) {
        separator();
        add(Action::DeleteMessage, "Delete Message");
      }
      if (m_preferences.developerMode || m_demo) {
        separator();
        add(Action::CopyId, "Copy Message ID");
      }
    } else {
      if (target->action == Action::ConversationMenu) {
        m_contextChannel = target->id;
        const auto channel = std::ranges::find(m_privateChannels, target->id, &api::Channel::id);
        if (channel == m_privateChannels.end())
          return;
        if (channel->type == 1 && !channel->recipients.empty())
          m_contextUser = channel->recipients.front().id;
      } else
        m_contextUser = target->id;
      if (!m_contextUser.empty()) {
        add(Action::UserProfile, "View profile");
        if (m_contextUser != m_account.id) {
          add(Action::OpenDm, "Message");
          add(Action::MentionUser, "Mention");
          const auto relationship = m_userDirectory.relationship(m_contextUser);
          if (relationship == 1) {
            add(Action::SetNickname, "Change friend nickname");
            add(Action::RemoveFriend, "Remove friend");
          } else if (relationship != 2)
            add(Action::AddFriend, relationship == 3 ? "Accept friend request" : "Add friend");
          separator();
          add(relationship == 2 ? Action::UnblockUser : Action::BlockUser, relationship == 2 ? "Unblock" : "Block");
        }
      }
      if (target->action == Action::ConversationMenu) {
        const auto channel = std::ranges::find(m_privateChannels, m_contextChannel, &api::Channel::id);
        if (channel != m_privateChannels.end() && !channel->isMessageRequest()) {
          separator();
          const bool pinned = m_userDirectory.pinnedDm(m_contextChannel);
          add(pinned ? Action::UnpinConversation : Action::PinConversation,
              pinned ? "Unpin conversation" : "Pin to top");
        }
      }
      // User menus can mute an existing one-to-one DM, even from a guild member.
      if (target->action != Action::ConversationMenu) {
        m_contextChannel.clear();
        for (const auto& channel : m_privateChannels)
          if (channel.type == 1
              && std::ranges::any_of(channel.recipients, [&](const auto& user) { return user.id == m_contextUser; })) {
            m_contextChannel = channel.id;
            break;
          }
      }
      if (!m_contextChannel.empty()) {
        separator();
        add(m_userDirectory.muted(m_contextChannel) ? Action::UnmuteConversation : Action::MuteConversation,
            m_userDirectory.muted(m_contextChannel) ? "Unmute conversation" : "Mute conversation");
      }
      if (!m_contextUser.empty() && (m_preferences.developerMode || m_demo)) {
        separator();
        add(Action::CopyId, "Copy user ID");
      }
    }
    if (m_contextEntries.empty())
      return;
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_emojiMenu = m_presenceMenu = false;
    m_contextX = x;
    m_contextY = y;
    m_contextScroll = 0;
    m_contextOpen = m_contextDirty = true;
    m_window.invalidate();
  }
  void ClientApplication::drawContext(Node& root, float w, float h) {
    if (!m_contextOpen)
      return;
    std::unique_ptr<Node> node;
    if (const auto stored = m_detachedInputs.find("popup:context"); stored != m_detachedInputs.end()) {
      node = std::move(stored->second);
      m_detachedInputs.erase(stored);
    } else {
      auto control = std::make_unique<ContextMenuControl>();
      control->setMaxVisible(64);
      control->setRedrawCallback([this] { m_window.invalidate(); });
      control->setOnActivate([this](const ContextMenuControlEntry& entry) {
        if (!serverMenuEntry(entry.id) && !channelMenuEntry(entry.id))
          contextAction(static_cast<Action>(entry.id));
      });
      control->setOnSubmenuOpen([this](const ContextMenuControlEntry& entry, float center) {
        if (entry.id >= serverMenuBase)
          openServerSubmenu(entry.id, center);
        else if (entry.id >= channelMenuBase)
          openChannelSubmenu(entry.id, center);
        else if (static_cast<Action>(entry.id) == Action::AddReaction)
          openReactionSubmenu(center);
      });
      m_contextControl = control.get();
      m_widgets["popup:context"] = control.get();
      node = std::move(control);
      m_contextDirty = true;
    }
    if (m_contextDirty) {
      m_contextControl->setEntries(m_contextEntries);
      m_contextDirty = false;
    }
    const auto width = std::clamp(
        ContextMenuControl::preferredWidth(m_window.target().renderer(), m_contextEntries),
        m_contextMessage.empty() ? 210.0F : 230.0F, std::max(230.0F, w - Style::spaceLg)
    );
    const float header = m_contextMessage.empty() || m_messageLog.known(m_contextChannel, m_contextMessage) ? 0 : 58;
    const auto fullHeight = m_contextControl->preferredHeight();
    const float height = std::min(fullHeight + header, h - Style::spaceGroup);
    m_contextFullHeight = fullHeight + header;
    m_scrolling.limit(m_contextScroll, std::max(0.0F, fullHeight - (height - header)));
    if (m_contextControl->width() != width)
      m_contextControl->setMenuWidth(width);
    const auto x = std::clamp(m_contextX, 8.0F, std::max(8.0F, w - width - 8));
    const auto y = std::clamp(m_contextY, 40.0F, std::max(40.0F, h - height - 8));
    m_contextBounds = {x, y, width, height, Action::None, {}};
    rectangle(
        root, x - Style::borderWidth, y - Style::borderWidth, width + Style::spaceXxs, height + Style::spaceXxs, raised,
        Style::radiusLg
    );
    rectangle(root, x, y, width, height, panel, Style::surfaceRadius);
    if (header) {
      const float gap = 5, cell = (width - 24 - gap * 3) / 4;
      for (std::size_t i = 0; i < 4 && i < m_frequentReactions.size(); ++i) {
        const float bx = x + Style::spaceMd + i * (cell + gap), by = y + Style::spaceDense;
        const auto& emoji = m_frequentReactions[i];
        rectangle(
            root, bx, by, cell, 42,
            hovered(bx, by, cell, 42) || m_contextQuickIndex == static_cast<int>(i) ? accent : raised,
            Style::hoverRadius
        );
        const auto colon = emoji.find(':');
        if (colon == std::string::npos)
          label(
              root, bx + (cell - 30) / 2, by + (Style::spaceCompact + Style::borderWidth), 32, emoji,
              Style::fontSizeHero, foreground
          );
        else
          emojiImage(
              root, bx + (cell - Style::spaceXl) / 2, by + (Style::spaceSm + Style::borderWidth), 24,
              api::emojiPath(emoji.substr(colon + 1))
          );
        m_hits.push_back({bx, by, cell, 42, Action::QuickReaction, emoji});
      }
    }
    auto viewport = std::make_unique<Node>();
    viewport->setPosition(x, y + header);
    viewport->setSize(width, std::max(0.0F, height - header));
    viewport->setClipChildren(true);
    node->setPosition(0, -m_contextScroll);
    viewport->addChild(std::move(node));
    root.addChild(std::move(viewport));
    if (fullHeight > height - header)
      settingsScrollbar(
          root, "popup:context-scrollbar", {x + width - 7, y + header, 5, height - header, Action::None, {}},
          fullHeight, m_contextScroll
      );
    if (m_reactionSubmenu) {
      const std::size_t visible = std::min<std::size_t>(11, std::max(1, static_cast<int>((h - 64) / 32)));
      const auto maxOffset = m_reactionChoices.size() > visible ? m_reactionChoices.size() - visible : 0;
      m_reactionSubmenuScroll = std::clamp(m_reactionSubmenuScroll, 0.0F, static_cast<float>(maxOffset));
      const auto offset = static_cast<std::size_t>(m_reactionSubmenuScroll);
      std::unique_ptr<Node> child;
      if (const auto found = m_detachedInputs.find("popup:reactions"); found != m_detachedInputs.end()) {
        child = std::move(found->second);
        m_detachedInputs.erase(found);
      } else {
        auto control = std::make_unique<ContextMenuControl>();
        m_reactionControl = control.get();
        m_widgets["popup:reactions"] = control.get();
        control->setMaxVisible(11);
        control->setRedrawCallback([this] { m_window.invalidate(); });
        control->setOnActivate([this](const ContextMenuControlEntry& entry) {
          const auto index = static_cast<std::size_t>(entry.id);
          if (m_channelSubmenu && index < m_channelSubmenuEntries.size()) {
            const auto id = m_channelSubmenuEntries[index].id;
            if (!serverMenuEntry(id))
              channelMenuEntry(id);
          } else if (index < m_reactionChoices.size())
            chooseReaction(m_reactionChoices[index].first);
        });
        child = std::move(control);
        m_reactionSubmenuDirty = true;
      }
      if (m_reactionSubmenuDirty) {
        const auto highlighted = m_reactionControl->highlightedIndex();
        std::vector<ContextMenuControlEntry> entries;
        for (auto i = offset; i < std::min(m_reactionChoices.size(), offset + visible); ++i) {
          if (m_channelSubmenu && i < m_channelSubmenuEntries.size()) {
            auto entry = m_channelSubmenuEntries[i];
            entry.id = static_cast<int>(i);
            entries.push_back(std::move(entry));
          } else
            entries.push_back(
                {.id = static_cast<int>(i), .label = m_reactionChoices[i].second, .emoji = m_reactionChoices[i].first}
            );
        }
        m_reactionControl->setEntries(std::move(entries));
        m_reactionControl->setHighlightedIndex(std::min(highlighted, visible - 1));
        m_reactionSubmenuDirty = false;
      }
      const float sw = std::min(260.0F, w - Style::spaceLg);
      if (m_reactionControl->width() != sw)
        m_reactionControl->setMenuWidth(sw);
      const auto sh = m_reactionControl->preferredHeight();
      m_reactionSubmenuHeight = sh;
      const float sx = x + width + sw + 8 <= w ? x + width + 3 : std::max(8.0F, x - sw - 3);
      const float sy = std::clamp(
          y + header + m_reactionSubmenuAnchor - m_contextScroll - (Style::spaceXl - Style::spaceXxs), 40.0F,
          std::max(40.0F, h - sh - 8)
      );
      m_reactionSubmenuBounds = {sx, sy, sw, sh, Action::None, {}};
      rectangle(
          root, sx - Style::borderWidth, sy - Style::borderWidth, sw + Style::spaceXxs, sh + Style::spaceXxs, raised,
          Style::radiusLg
      );
      rectangle(root, sx, sy, sw, sh, panel, Style::surfaceRadius);
      child->setPosition(sx, sy);
      root.addChild(std::move(child));
      if (m_reactionChoices.size() > visible) {
        const float track = sh - 16, thumb = std::max(20.0F, track * visible / m_reactionChoices.size());
        rectangle(
            root, sx + sw - 5, sy + Style::spaceSm + (track - thumb) * offset / std::max<std::size_t>(1, maxOffset), 2,
            thumb, muted, 1
        );
      }
    }
    m_tooltip.clear();
  }
  void ClientApplication::openReactionSubmenu(float center) {
    if (m_channelSubmenu)
      m_reactionChoices.clear();
    m_channelSubmenu = false;
    m_channelSubmenuEntries.clear();
    if (m_reactionChoices.empty()) {
      // Common choices lead the complete bundled Unicode catalog. Labels are
      // plain text, so emoji names can never become Pango markup.
      for (const auto& [emoji, name] : std::vector<std::pair<std::string, std::string>>{
               {"❤️", "heart"},
               {"👍", "thumbsup"},
               {"😂", "joy"},
               {"🔥", "fire"},
               {"😭", "sob"},
               {"✅", "white_check_mark"},
               {"🤣", "rofl"},
               {"💀", "skull"},
               {"💯", "100"},
               {"🫡", "saluting_face"},
               {"🫂", "people_hugging"},
               {"🎉", "tada"},
               {"👀", "eyes"},
               {"🙏", "pray"},
               {"👎", "thumbsdown"},
               {"✨", "sparkles"}
           })
        m_reactionChoices.emplace_back(emoji, ":" + name + ":");
      std::ifstream file(paths::assetPath("emoji.json"), std::ios::binary);
      std::string bytes(512 * 1024 + 1, '\0');
      file.read(bytes.data(), bytes.size());
      bytes.resize(file.gcount());
      if (bytes.size() <= 512 * 1024) {
        const auto catalog = nlohmann::json::parse(bytes, nullptr, false);
        if (catalog.is_array())
          for (const auto& item : catalog) {
            if (m_reactionChoices.size() >= 2048)
              break;
            if (!item.is_object()
                || !item.contains("emoji")
                || !item["emoji"].is_string()
                || !item.contains("name")
                || !item["name"].is_string())
              continue;
            const auto emoji = item["emoji"].get<std::string>();
            auto name = item["name"].get<std::string>();
            if (emoji.size() > 160
                || name.size() > 160
                || std::ranges::any_of(m_reactionChoices, [&](const auto& value) { return value.first == emoji; }))
              continue;
            std::ranges::replace(name, ' ', '_');
            m_reactionChoices.emplace_back(emoji, ":" + name + ":");
          }
      }
    }
    if (!m_reactionSubmenu) {
      m_reactionSubmenuScroll = 0;
      m_reactionSubmenuDirty = true;
    }
    m_reactionSubmenu = true;
    m_reactionSubmenuAnchor = center;
    m_window.invalidate();
  }
  void ClientApplication::contextMotion(float x, float y) {
    if (!m_contextOpen)
      return;
    m_contextQuickIndex = -1;
    if (!m_reactionSubmenu)
      return;
    if (m_reactionSubmenuBounds.contains(x, y))
      m_reactionSubmenuFocus = true;
    else if (m_contextBounds.contains(x, y)) {
      m_reactionSubmenuFocus = false;
      const auto index = m_contextControl->highlightedIndex();
      if (y < m_contextBounds.y + (m_contextMessage.empty() ? 0 : 58)
          || (index < m_contextEntries.size() && !m_contextEntries[index].hasSubmenu))
        m_reactionSubmenu = false;
    }
  }
  void ClientApplication::scrollContext(float delta) {
    if (!m_reactionSubmenu || !m_reactionSubmenuBounds.contains(m_pointerX, m_pointerY))
      return;
    const auto visible = std::max(1.0F, std::floor((m_reactionSubmenuHeight - Style::spaceMd) / 32));
    m_reactionSubmenuScroll = std::clamp(
        m_reactionSubmenuScroll + (delta > 0 ? 3 : -3), 0.0F,
        std::max(0.0F, static_cast<float>(m_reactionChoices.size()) - visible)
    );
    m_reactionSubmenuDirty = true;
    m_window.invalidate();
  }
  void ClientApplication::chooseReaction(const std::string& emoji) {
    const auto message = std::ranges::find(m_messages, m_contextMessage, &api::Message::id);
    if (message == m_messages.end() || message->deleted)
      return;
    const bool own = std::ranges::any_of(message->reactions, [&](const auto& reaction) {
      return api::reactionKey(reaction) == emoji && reaction.me;
    });
    startInteraction(
        {.kind = own ? api::InteractionKind::RemoveReaction : api::InteractionKind::AddReaction,
         .channel = m_contextChannel,
         .message = m_contextMessage,
         .emoji = emoji}
    );
    closeContext();
  }
  bool ClientApplication::overlayClick(float x, float y) {
    if (m_dialogKind == "sidebar-invite" && m_channelInvite.dropdown) {
      if (m_channelInvite.dropdownBounds.contains(x, y))
        return false;
      channelInviteAction({0, 0, 0, 0, Action::InviteDropdown, "close"});
      return true;
    }
    if (m_contextOpen) {
      if (m_reactionSubmenu && m_reactionSubmenuBounds.contains(x, y)) {
        m_reactionSubmenuFocus = true;
        m_controlInput.pointerButton(
            x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
        );
      } else if (!m_contextBounds.contains(x, y))
        closeContext();
      else {
        for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it)
          if (it->action == Action::QuickReaction && it->contains(x, y)) {
            chooseReaction(it->id);
            return true;
          }
        m_controlInput.pointerButton(
            x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
        );
      }
      return true;
    }
    if ((m_dialogKind == "profile" || m_dialogKind == "guild-profile") && m_dialogBounds.contains(x, y)) {
      // Only this overlay's retained scrollbar may receive native input here.
      // The conversation's scrollbar/composer can be directly behind the card.
      const auto bar = m_widgets.find("profile-scrollbar");
      if (bar != m_widgets.end()
          && bar->second->parent()
          && x >= bar->second->x()
          && x < bar->second->x() + bar->second->width()
          && y >= bar->second->y()
          && y < bar->second->y() + bar->second->height()) {
        m_controlInput.pointerButton(
            x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
        );
      } else {
        for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it)
          if (it->contains(x, y)) {
            const auto hit = *it;
            activate(hit);
            break;
          }
      }
      return true;
    }
    if (!m_dialogKind.empty() && !m_dialogBounds.contains(x, y)) {
      if (channelEditClose())
        return true;
      if (!m_dialogPending
          || m_dialogKind == "profile"
          || m_dialogKind == "guild-profile"
          || m_dialogKind == "sidebar-invite")
        closeDialog();
      return true;
    }
    return false;
  }
  bool ClientApplication::overlayKey(const Key& key) {
    if (channelInviteKey(key))
      return true;
    if (m_contextOpen) {
      if (key.control && (key.symbol == XKB_KEY_q || key.symbol == XKB_KEY_Q))
        return false;
      if (!m_contextMessage.empty()) {
        if (key.symbol == XKB_KEY_Tab) {
          m_contextQuickIndex = m_contextQuickIndex >= 3 ? -1 : m_contextQuickIndex + 1;
          m_reactionSubmenu = m_reactionSubmenuFocus = false;
          m_window.invalidate();
          return true;
        }
        if (m_contextQuickIndex >= 0) {
          if (key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter || key.symbol == XKB_KEY_space)
            chooseReaction(m_frequentReactions[m_contextQuickIndex]);
          else if (key.symbol == XKB_KEY_Left || key.symbol == XKB_KEY_Right) {
            m_contextQuickIndex = (m_contextQuickIndex + (key.symbol == XKB_KEY_Left ? 3 : 1)) % 4;
            m_window.invalidate();
          } else if (key.symbol == XKB_KEY_Down || key.symbol == XKB_KEY_Up) {
            m_contextQuickIndex = -1;
            m_window.invalidate();
          } else if (key.symbol == XKB_KEY_Escape)
            closeContext();
          return true;
        }
      }
      auto* menu = m_reactionSubmenu && m_reactionSubmenuFocus ? m_reactionControl : m_contextControl;
      if (key.symbol == XKB_KEY_Escape)
        closeContext();
      else if (key.symbol == XKB_KEY_Left && m_reactionSubmenu) {
        m_reactionSubmenu = m_reactionSubmenuFocus = false;
        m_window.invalidate();
      } else if (
          key.symbol == XKB_KEY_Right
          && m_contextControl
          && m_contextControl->highlightedIndex() < m_contextEntries.size()
          && m_contextEntries[m_contextControl->highlightedIndex()].hasSubmenu
      ) {
        (void)m_contextControl->activateHighlighted();
        m_reactionSubmenuFocus = true;
      } else if (menu && (key.symbol == XKB_KEY_Up || key.symbol == XKB_KEY_Down)) {
        const bool down = key.symbol == XKB_KEY_Down;
        const auto index = menu->highlightedIndex();
        if (menu == m_reactionControl
            && ((!down && index == 0 && m_reactionSubmenuScroll > 0)
                || (down
                    && index + 1 == menu->entryCount()
                    && m_reactionSubmenuScroll + menu->entryCount() < m_reactionChoices.size()))) {
          m_reactionSubmenuScroll += down ? 1 : -1;
          m_reactionSubmenuDirty = true;
          m_window.invalidate();
        } else {
          (void)menu->moveHighlight(down ? 1 : -1);
          if (menu == m_contextControl) {
            const float header =
                m_contextMessage.empty() || m_messageLog.known(m_contextChannel, m_contextMessage) ? 0 : 58;
            const auto top = menu->rowTop(menu->highlightedIndex()), bottom = menu->rowBottom(menu->highlightedIndex());
            if (top < m_contextScroll)
              m_contextScroll = top;
            else if (bottom > m_contextScroll + m_contextBounds.height - header)
              m_contextScroll = bottom - (m_contextBounds.height - header);
            m_scrolling.stop(m_contextScroll);
          }
          if (menu == m_contextControl
              && menu->highlightedIndex() < m_contextEntries.size()
              && !m_contextEntries[menu->highlightedIndex()].hasSubmenu)
            m_reactionSubmenu = false;
        }
      } else if (menu && (key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter)) {
        (void)menu->activateHighlighted();
        if (m_reactionSubmenu)
          m_reactionSubmenuFocus = true;
      }
      return true;
    }
    if (!m_dialogKind.empty() && key.symbol == XKB_KEY_Escape) {
      if (channelEditClose())
        return true;
      if (!m_dialogPending || m_dialogKind == "profile" || m_dialogKind == "guild-profile")
        closeDialog();
      return true;
    }
    if (!m_dialogKind.empty()) {
      if (key.control && (key.symbol == XKB_KEY_q || key.symbol == XKB_KEY_Q))
        return false;
      if (key.control && key.symbol == XKB_KEY_comma)
        return true;
      if (!m_focusedInput.starts_with("channel-")
          && !m_focusedInput.starts_with("application-field:")
          && !m_focusedInput.starts_with("server-menu:")
          && m_focusedInput != "dm-create-search"
          && m_focusedInput != "dialog"
          && !(m_dialogKind == "forum-create" && m_focusedInput == "forum-body")
          && key.symbol != XKB_KEY_Tab)
        return true;
    }
    return false;
  }
  bool ClientApplication::startInteraction(api::Interaction action) {
    using Kind = api::InteractionKind;
    if (action.kind == Kind::RemoveEmbeds) {
      const auto message = std::ranges::find(m_messages, action.message, &api::Message::id);
      if (action.channel != m_channelId
          || message == m_messages.end()
          || message->channelId != action.channel
          || !api::canRemoveMessageEmbeds(*message, m_account.id, channelPermissionsFor(action.channel))) {
        m_status = "This message is no longer available or you cannot remove its embeds.";
        return false;
      }
      action.messageFlags = message->flags;
    }
    if (action.kind == Kind::AutoModerationFeedback) {
      const auto message = std::ranges::find(m_messages, action.message, &api::Message::id);
      if (action.channel != m_channelId
          || action.guild != m_guildId
          || message == m_messages.end()
          || message->deleted
          || message->type != autoModerationActionType
          || autoModerationField(autoModerationEmbed(*message), "decision_id") != action.customId
          || !autoModerationEmbed(*message)
          || autoModerationEmbed(*message)->description != action.content) {
        m_status = "This AutoMod alert is no longer available.";
        return false;
      }
    }
    if (!action.message.empty()
        && m_messageLog.known(action.channel, action.message)
        && (action.kind == Kind::AddReaction
            || action.kind == Kind::RemoveReaction
            || action.kind == Kind::ListReactions
            || action.kind == Kind::EditMessage
            || action.kind == Kind::RemoveEmbeds
            || action.kind == Kind::DeleteMessage
            || action.kind == Kind::CreateThread
            || action.kind == Kind::MessageButton))
      return false;
    if (m_demo || !m_connected) {
      m_status = "Demo mode is read-only.";
      return false;
    }
    if (action.kind == Kind::Mute || action.kind == Kind::Unmute || action.kind == Kind::ChannelNotifications)
      action.stateRevision = m_userDirectory.notificationRevision(action.channel);
    if (action.kind == Kind::GuildNotifications)
      action.stateRevision = m_userDirectory.guildNotificationRevision(action.guild);
    if (action.kind == Kind::MoveGuild || action.kind == Kind::EditGuildFolder || action.kind == Kind::ReorderGuilds)
      action.stateRevision = m_guildLayoutRevision;
    const auto key = interactionKey(action);
    if (m_pendingInteractions.contains(key))
      return false;
    if (!m_client.interact(action)) {
      m_status = "Could not queue this action. Check the input and try again.";
      return false;
    }
    m_pendingInteractions.insert(key);
    return true;
  }
  void ClientApplication::removeMessageEmbeds(const std::string& id, bool immediate) {
    const auto message = std::ranges::find(m_messages, id, &api::Message::id);
    if (message == m_messages.end()
        || message->channelId != m_channelId
        || !api::canRemoveMessageEmbeds(*message, m_account.id, channelPermissionsFor(m_channelId)))
      return;
    api::Interaction request{
        .kind = api::InteractionKind::RemoveEmbeds,
        .channel = m_channelId,
        .message = id,
        .revision = ++m_dialogGeneration,
        .messageFlags = message->flags
    };
    if (immediate) {
      startInteraction(request);
      return;
    }
    m_dialogKind = "confirm";
    m_dialogTitle = "Are you sure?";
    m_dialogText.clear();
    m_dialogError.clear();
    m_dialogPending = false;
    m_dialogAction = request;
  }
  void ClientApplication::contextAction(Action action) {
    if (action == Action::CopyImage
        || action == Action::SaveImage
        || action == Action::CopyMediaLink
        || action == Action::OpenMediaLink) {
      const bool current = !m_viewImage.empty() && m_imageContext == m_viewImage;
      closeContext();
      if (current)
        imageViewerAction(action);
      return;
    }
    if (action == Action::ServerSettings || action == Action::CopyServerId || action == Action::ServerInvitePeople) {
      const auto guild = m_contextGuild;
      closeContext();
      if (action == Action::ServerSettings)
        openServerSettings(guild);
      else if (action == Action::CopyServerId)
        m_window.copy(guild);
      else if (guild == m_guildId) {
        const auto channel = std::ranges::find_if(m_channels, [&](const auto& value) {
          return value.type != 4 && !value.isThread() && (channelPermissionsFor(value.id).value_or(0) & 1);
        });
        if (channel != m_channels.end())
          openChannelInvite({.kind = api::InteractionKind::CreateInvite, .channel = channel->id, .guild = guild});
      }
      return;
    }
    if (action == Action::PinConversation || action == Action::UnpinConversation) {
      const auto id = m_contextChannel;
      closeContext();
      pinDm(id, action == Action::PinConversation);
      return;
    }
    if (action == Action::BookmarkMessage || action == Action::RemindMessage) {
      const auto message = std::ranges::find(m_messages, m_contextMessage, &api::Message::id);
      if (message != m_messages.end() && !message->deleted && !(message->flags & 64))
        inboxAction({.action = action, .id = m_contextMessage});
      return;
    }
    closeContext();
    const auto found = std::ranges::find(m_messages, m_contextMessage, &api::Message::id);
    if (found != m_messages.end()
        && found->deleted
        && (action == Action::Reply
            || action == Action::EditMessage
            || action == Action::RemoveEmbeds
            || action == Action::DeleteMessage
            || action == Action::AddReaction
            || action == Action::CreateThread))
      return;
    const auto user = lookupUser(m_contextUser);
    using Kind = api::InteractionKind;
    api::Interaction request;
    request.user = m_contextUser;
    request.channel = m_contextChannel;
    request.message = m_contextMessage;
    switch (action) {
    case Action::RemoveEmbeds:
      if (m_contextChannel == m_channelId)
        removeMessageEmbeds(m_contextMessage, m_window.shiftPressed());
      break;
    case Action::CreateThread:
      if (found != m_messages.end()) {
        if (found->hasThread || found->thread || m_threads.find(m_guildId, found->id))
          openThread(found->thread ? found->thread->id : found->id);
        else
          createThread(m_contextChannel, found->id);
      }
      break;
    case Action::Reply:
      if (found != m_messages.end() && m_contextChannel == m_channelId) {
        m_replies[m_channelId] = {found->id, m_channelId, found->authorId, found->author, found->content};
        if (found->content.empty() && !found->stickers.empty())
          m_replies[m_channelId].content = "Sticker: " + found->stickers.front().name;
        m_focusedInput = "composer";
      }
      break;
    case Action::AddReaction:
      if (found != m_messages.end()) {
        m_reactionMessage = found->id;
        m_reactionChannel = m_channelId;
        m_emojiMenu = true;
      }
      break;
    case Action::CopyMessage:
      if (found != m_messages.end())
        m_window.copy(
            found->type == 7 ? joinNoticeText(*found, userName(found->authorId, found->author)) : found->content
        );
      break;
    case Action::CopyLink:
      if (found != m_messages.end())
        m_window.copy(
            "https://discord.com/channels/"
            + (m_guildId.empty() ? "@me" : m_guildId)
            + "/"
            + m_contextChannel
            + "/"
            + found->id
        );
      break;
    case Action::CopyId:
      m_window.copy(m_contextMessage.empty() ? m_contextUser : m_contextMessage);
      break;
    case Action::UserProfile:
      showProfile(m_contextUser);
      break;
    case Action::MentionUser:
      m_fieldFocused = true;
      m_focusedInput = "composer";
      m_edit.end(m_drafts[m_channelId]);
      insert("<@" + m_contextUser + "> ");
      break;
    case Action::OpenDm: {
      const auto channel = std::ranges::find_if(m_privateChannels, [&](const auto& channel) {
        return channel.type == 1 && std::ranges::any_of(channel.recipients, [&](const auto& recipient) {
                 return recipient.id == m_contextUser;
               });
      });
      if (channel != m_privateChannels.end()) {
        const auto id = channel->id;
        if (channel->isMessageRequest()) {
          openDmPage("requests");
          dmAction("open-request:" + id);
          break;
        }
        m_restoreChannel = id;
        showDirectMessages();
        if (m_channelId != id)
          selectChannel(id);
      } else {
        request.kind = Kind::OpenDm;
        startInteraction(request);
      }
      break;
    }
    case Action::RemoveFromLog:
      forgetMessageLog(m_contextChannel, m_contextMessage, true, true);
      break;
    case Action::ClearEditHistory:
      forgetMessageLog(m_contextChannel, m_contextMessage, false, true);
      break;
    case Action::MuteConversation:
    case Action::UnmuteConversation:
      request.kind = action == Action::MuteConversation ? Kind::Mute : Kind::Unmute;
      startInteraction(request);
      break;
    case Action::UnblockUser:
    case Action::AddFriend:
      request.kind = action == Action::UnblockUser ? Kind::Unblock : Kind::AddFriend;
      startInteraction(request);
      break;
    case Action::SetNickname:
    case Action::DeleteMessage:
    case Action::BlockUser:
    case Action::RemoveFriend:
      if (action == Action::DeleteMessage && (found == m_messages.end() || found->authorId != m_account.id))
        break;
      request.kind = action == Action::SetNickname ? Kind::Nickname
          : action == Action::DeleteMessage        ? Kind::DeleteMessage
          : action == Action::BlockUser            ? Kind::Block
                                                   : Kind::RemoveFriend;
      m_dialogKind = action == Action::SetNickname ? "nickname" : "confirm";
      ++m_dialogGeneration;
      m_dialogTitle = action == Action::SetNickname ? "Change friend nickname"
          : action == Action::DeleteMessage         ? "Delete message?"
          : action == Action::BlockUser             ? "Block " + userName(user.id, user.name) + "?"
                                                    : "Remove " + userName(user.id, user.name) + " as a friend?";
      m_dialogText = action == Action::SetNickname ? m_userDirectory.name(user.id, "") : "";
      m_dialogError.clear();
      m_dialogPending = false;
      m_dialogAction = request;
      if (m_dialogKind != "confirm")
        m_focusedInput = "dialog";
      break;
    case Action::EditMessage:
      if (m_contextChannel == m_channelId)
        beginMessageEdit(m_contextMessage);
      break;
    default:
      break;
    }
    m_window.invalidate();
  }
  void ClientApplication::showProfile(const std::string& user) {
    if (!api::validSnowflake(user))
      return;
    closeContext();
    closeGifPicker();
    closeEmojiPicker();
    m_scrolling.stopAll();
    ++m_dialogGeneration;
    m_dialogKind = "profile";
    m_dialogTitle = "Profile";
    m_dialogAccount = lookupUser(user);
    m_profileCard = {};
    m_profileCard.user = m_dialogAccount;
    m_profileCard.guild = m_dmMode ? "" : m_guildId;
    m_profileExpanded = false;
    m_profileRevealSpoilers = false;
    m_profileScroll = 0;
    m_profileAnchorX = m_pointerX;
    m_profileAnchorY = m_pointerY;
    m_dialogText.clear();
    m_dialogError.clear();
    m_dialogAction = api::Interaction{
        .kind = api::InteractionKind::UserProfile,
        .user = user,
        .guild = m_profileCard.guild,
        .revision = m_dialogGeneration
    };
    if (m_dmMode && m_dmProfile.user == user && m_dmProfile.data) {
      m_profileCard = *m_dmProfile.data;
      m_dialogAccount = m_profileCard.user;
      m_dialogPending = false;
      return;
    }
    m_dialogPending = !m_demo && startInteraction(*m_dialogAction);
    if (m_demo)
      prepareProfileDemo();
    else if (!m_dialogPending)
      m_dialogError = m_status;
  }
  void ClientApplication::closeDialog() {
    m_serverInvitePending.clear();
    m_serverDialogGuild.clear();
    m_serverDialogFolder.clear();
    m_serverFolderColor.clear();
    m_serverPrivacyLoaded = false;
    m_scrolling.stop(m_serverDialogScroll);
    m_serverDialogScroll = 0;
    m_scrolling.stop(m_dmCreateScroll);
    m_dmCreateScroll = 0;
    m_dmCreateBounds = {};
    m_dmCreateRecipients.clear();
    m_dmCreateCandidates.clear();
    m_dmCreateSearch.clear();
    m_dmCreateError.clear();
    m_dmCreateRequest = 0;
    if (m_inputs.contains("dm-create-search"))
      m_inputs["dm-create-search"]->setValue("");
    m_scrolling.stop(m_channelInvite.scroll);
    m_scrolling.stop(m_channelInvite.formScroll);
    m_channelInvite = {};
    m_channelCreateAccessStep = false;
    m_channelCreateFilter.clear();
    m_scrolling.stop(m_channelCreateScroll);
    m_channelCreateScroll = 0;
    m_applicationModal.reset();
    m_applicationValues.clear();
    m_modalNonce.clear();
    for (auto& [id, input] : m_inputs)
      if (id.starts_with("application-field:")) {
        input->setValue("");
      }
    ++m_dialogGeneration;
    m_archiveQuery.reset();
    m_scrolling.stop(m_threadScroll);
    m_threadScroll = 0;
    m_dialogKind.clear();
    m_dialogAction.reset();
    m_dialogPending = false;
    m_dialogText.clear();
    m_dialogAccount = {};
    m_scrolling.stop(m_profileScroll);
    m_profileCard = {};
    m_profileExpanded = false;
    m_guildProfile = {};
    m_guildPreviewJoined = m_guildPreviewVerification = false;
    m_profileScroll = 0;
    if (m_inputs.contains("dialog"))
      m_inputs["dialog"]->setValue("");
    if (m_inputs.contains("channel-create-access"))
      m_inputs["channel-create-access"]->setValue("");
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_window.invalidate();
  }
  void ClientApplication::clearInteractions() {
    m_scrolling.stop(m_dmProfile.scroll);
    m_dmProfile = {};
    m_dmDemoAction.reset();
    m_dmPinRequests.clear();
    m_dmPage.clear();
    m_dmSearch.clear();
    m_dmFriendName.clear();
    m_dmStatus.clear();
    m_dmConfirm.clear();
    m_dmPending.clear();
    m_dmPreviewId.clear();
    m_dmPreview.clear();
    m_dmPreviewLoading = false;
    m_dmPreviewBounds = {};
    m_scrolling.stop(m_dmPreviewScroll);
    m_dmPreviewScroll = 0;
    m_dmPreviewRequest = m_dmFriendRequest = 0;
    m_privateReady = false;
    m_privateChannels.clear();
    m_channelInviteSettings.clear();
    closeCompletion();
    cancelMessageEdit(false);
    closeContext();
    closeDialog();
    m_pendingInteractions.clear();
    m_reactorHoverTimer.stop();
    m_reactorPages.clear();
    m_hoverReactionKey.clear();
    m_frequentReactions = {"❤️", "👍", "😂", "🔥"};
    m_replies.clear();
    closeSearchMenu();
    m_searchHistory.clear();
    clearInbox();
    clearOutgoing();
    m_reactionMessage.clear();
    m_reactionChannel.clear();
    m_revealedBlocked.clear();
  }
  void ClientApplication::drawRemoveEmbedsDialog(Node& root, float w, float h) {
    const std::string description = "This will remove all embeds on this message for everyone.";
    const std::string hint = "Hold Shift when removing embeds to skip this confirmation.";
    const std::string caption = m_dialogPending ? "Removing…" : "Remove All Embeds";
    const float width = std::min(layout::EmbedRemoval::maximumWidth, w - Style::space2Xl);
    const float textWidth = width - layout::EmbedRemoval::padding * 2;
    auto& renderer = m_window.target().renderer();
    const auto textHeight = [&](const std::string& text, float size) {
      if (text.empty())
        return 0.0F;
      const auto metrics = renderer.measureText(text, size, FontWeight::Normal, textWidth, 0);
      return std::ceil(metrics.bottom - metrics.top);
    };
    const auto frames = layout::embedRemovalLayout(
        w, h, textHeight(m_dialogTitle, Style::fontSizeHeading), textHeight(description, Style::fontSizeBody),
        textHeight(hint, Style::fontSizeBody), textHeight(m_dialogError, Style::fontSizeDescription),
        renderer.measureText("Cancel", Style::fontSizeBody).width + Style::spaceXl * 2,
        renderer.measureText("Remove All Embeds", Style::fontSizeBody).width + Style::spaceXl * 2
    );
    const auto& frame = frames.dialog;
    rectangle(
        root, 0, layout::Window::titlebarHeight, w, h - layout::Window::titlebarHeight,
        withAlpha(background, Style::modalBackdropOpacity)
    );
    m_hits.push_back({0, layout::Window::titlebarHeight, w, h - layout::Window::titlebarHeight, Action::None, {}});
    rectangle(root, frame.x, frame.y, frame.width, frame.height, sidebar, Style::radiusXl);
    m_dialogBounds = {frame.x, frame.y, frame.width, frame.height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    const float textX = frame.x + layout::EmbedRemoval::padding;
    label(
        root, textX, frame.y + frames.title, textWidth, m_dialogTitle, Style::fontSizeHeading, foreground,
        FontWeight::SemiBold
    );
    label(
        root, textX, frame.y + frames.description, textWidth, description, Style::fontSizeBody, foreground,
        FontWeight::Normal, 0
    );
    label(root, textX, frame.y + frames.hint, textWidth, hint, Style::fontSizeBody, muted, FontWeight::Normal, 0);
    if (!m_dialogError.empty())
      label(
          root, textX, frame.y + frames.error, textWidth, m_dialogError, Style::fontSizeDescription, danger,
          FontWeight::Normal, 0
      );
    const auto control = [&](const layout::Frame& bounds, const std::string& text, Action action, bool destructive) {
      Hit hit{frame.x + bounds.x,
              frame.y + bounds.y,
              bounds.width,
              bounds.height,
              m_dialogPending ? Action::None : action,
              {}};
      const bool over = !m_dialogPending && hit.contains(m_pointerX, m_pointerY);
      rectangle(
          root, hit.x, hit.y, hit.width, hit.height,
          destructive
              ? (over ? lerpColor(theme::destructive, foreground, Style::destructiveHoverBlend) : theme::destructive)
              : over ? raised
                     : panel,
          Style::controlRadius
      );
      const auto metrics = renderer.measureText(text, Style::fontSizeBody, FontWeight::Medium);
      label(
          root, hit.x + (hit.width - metrics.width) / 2, hit.y + (hit.height - (metrics.bottom - metrics.top)) / 2,
          metrics.width + Style::spaceXxs, text, Style::fontSizeBody,
          m_dialogPending   ? muted
              : destructive ? ink
                            : foreground,
          FontWeight::Medium
      );
      m_hits.push_back(hit);
    };
    control(frames.cancel, "Cancel", Action::CloseDialog, false);
    control(frames.remove, caption, Action::SubmitDialog, true);
    m_tooltip.clear();
  }
  void ClientApplication::drawDialog(Node& root, float w, float h) {
    if (m_dialogKind.starts_with("server-")) {
      drawServerMenuDialog(root, w, h);
      return;
    }
    if (m_dialogKind == "dm-create") {
      drawDmCreate(root, w, h);
      return;
    }
    if (m_dialogKind.empty())
      return;
    if (m_dialogKind.starts_with("sidebar-")) {
      drawSidebarDialog(root, w, h);
      return;
    }
    if (m_dialogKind == "application-modal") {
      drawApplicationModal(root, w, h);
      return;
    }
    if (m_dialogKind.starts_with("channel-")) {
      drawChannelDialog(root, w, h);
      return;
    }
    if (m_dialogKind == "forum-create") {
      drawForumPostDialog(root, w, h);
      return;
    }
    if (m_dialogKind == "threads") {
      drawThreads(root, w, h);
      return;
    }
    if (m_dialogKind == "guild-profile") {
      drawGuildProfile(root, w, h);
      return;
    }
    if (m_dialogKind == "profile") {
      drawProfileCard(root, w, h);
      return;
    }
    if (m_dialogKind == "confirm" && m_dialogAction && m_dialogAction->kind == api::InteractionKind::DeleteMessage) {
      drawMessageDialog(root, w, h);
      return;
    }
    if (m_dialogKind == "confirm" && m_dialogAction && m_dialogAction->kind == api::InteractionKind::RemoveEmbeds) {
      drawRemoveEmbedsDialog(root, w, h);
      return;
    }
    const float width = std::min(500.0F, w - Style::space2Xl), height = 230;
    const float x = (w - width) / 2, y = std::max(44.0F, (h - height) / 2);
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, Color{0, 0, 0, 0.65F});
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::None, {}});
    m_dialogBounds = {x, y, width, height, Action::None, {}};
    rectangle(root, x, y, width, height, sidebar, Style::radiusXl);
    m_hits.push_back(m_dialogBounds);
    label(
        root, x + Style::spaceXl, y + (Style::spaceXl - Style::spaceXxs), width - 70, m_dialogTitle,
        Style::fontSizeProfile, foreground, FontWeight::SemiBold
    );
    iconButton(
        root,
        {x + width - 42,
         y + Style::spaceMd,
         30,
         30,
         m_dialogPending && m_dialogKind != "profile" ? Action::None : Action::CloseDialog,
         {}},
        "x", "Close"
    );
    {
      if (m_dialogKind == "nickname" || m_dialogKind == "thread-create")
        inputControl(
            root, x + Style::spaceXl, y + 70, width - 48, 44, "dialog", m_dialogText,
            m_dialogKind == "nickname" ? "Nickname (leave empty to clear)" : "Thread name"
        );
      else
        label(
            root, x + Style::spaceXl, y + 76, width - Style::spaceGroup,
            m_dialogAction && m_dialogAction->kind == api::InteractionKind::DeleteMessage
                ? "This permanently deletes the message."
                : m_dialogAction && m_dialogAction->kind == api::InteractionKind::AutoModerationFeedback
                ? "Send this AutoMod decision and its flagged content to Discord to report a problem?"
                : "This changes your Discord relationship with this user.",
            Style::fontSizeBody, foreground, FontWeight::Normal, 2
        );
      label(
          root, x + Style::spaceXl, y + height - 92, width - Style::spaceGroup, m_dialogError,
          Style::fontSizeDescription, theme::danger, FontWeight::Normal, 2
      );
      button(
          root,
          {x + Style::spaceXl, y + height - 52, 100, 32, m_dialogPending ? Action::None : Action::CloseDialog, {}},
          "Cancel"
      );
      button(
          root, {x + width - 130, y + height - 52, 106, 32, m_dialogPending ? Action::None : Action::SubmitDialog, {}},
          m_dialogPending                                                                              ? "Saving…"
              : m_dialogAction && m_dialogAction->kind == api::InteractionKind::AutoModerationFeedback ? "Send report"
              : m_dialogKind == "thread-create"                                                        ? "Create Thread"
              : m_dialogKind == "confirm"                                                              ? "Confirm"
                                                                                                       : "Save",
          true
      );
    }
    m_tooltip.clear();
  }
  void ClientApplication::submitDialog() {
    if (m_dialogKind.starts_with("server-")) {
      submitServerMenuDialog();
      return;
    }
    if (m_dialogKind.starts_with("sidebar-")) {
      submitSidebarDialog();
      return;
    }
    if (!m_dialogAction || m_dialogPending)
      return;
    auto action = *m_dialogAction;
    action.text = m_dialogText;
    if (action.kind == api::InteractionKind::CreateThread || action.kind == api::InteractionKind::CreateForumPost)
      action.stateRevision = m_threads.revision();
    if (action.kind == api::InteractionKind::CreateForumPost) {
      const auto channel = std::ranges::find(m_channels, action.channel, &api::Channel::id);
      if (channel == m_channels.end())
        return;
      if (channel->requireTag && m_forumPostTag.empty()) {
        m_dialogError = "Choose a tag for this post.";
        m_window.invalidate();
        return;
      }
      action.content = m_emojiCatalog.messageContent(
          m_drafts[action.channel], action.guild, m_account.premiumType.value_or(0), m_preferences.emojiImageLinks
      );
      action.tags = m_forumPostTag.empty() ? std::vector<std::string>{} : std::vector{m_forumPostTag};
    }
    if (startInteraction(action)) {
      m_dialogAction = action;
      m_dialogPending = true;
      m_dialogError.clear();
    } else
      m_dialogError = m_status;
    m_window.invalidate();
  }
  void ClientApplication::interactionResult(const api::Event& event) {
    if (!event.interaction)
      return;
    const auto& action = *event.interaction;
    const auto key = interactionKey(action);
    if (action.kind == api::InteractionKind::ListReactions) {
      const auto found = m_reactorPages.find(key);
      if (found == m_reactorPages.end() || found->second.revision != action.revision)
        return; // A Gateway update, eviction or logout invalidated this request.
      auto& page = found->second;
      page.pending = false;
      page.loaded = event.error.empty();
      page.error = event.error;
      page.users = event.reactors;
      page.touched = std::chrono::steady_clock::now();
      m_window.invalidate();
      return;
    }
    m_pendingInteractions.erase(key);
    if (dmProfileResult(event))
      return;
    if (serverMenuInteractionResult(event))
      return;
    if (dmInteractionResult(event))
      return;
    if (channelInviteResult(event))
      return;
    if (m_messageEdit
        && m_messageEdit->request
        && interactionKey(*m_messageEdit->request) == key
        && m_messageEdit->request->revision == action.revision) {
      if (!event.error.empty()) {
        m_messageEdit->request.reset();
        m_messageEdit->error = event.error;
        m_window.invalidate();
        return;
      }
      cancelMessageEdit();
    }
    if (action.kind == api::InteractionKind::MessageButton || action.kind == api::InteractionKind::SubmitModal) {
      componentResult(event);
      return;
    }
    m_threadPending.erase(action.channel);
    const bool dialog = m_dialogAction
        && interactionKey(*m_dialogAction) == key
        && (action.kind != api::InteractionKind::RemoveEmbeds || m_dialogAction->revision == action.revision);
    if (!event.error.empty()) {
      if (dialog) {
        if (m_dialogKind.empty())
          m_dialogKind = "sidebar-error";
        m_dialogPending = false;
        m_dialogError = event.error;
      }
      m_status = event.error;
      return;
    }
    if (sidebarInteraction(event, dialog))
      return;
    if (channelFeatureInteraction(event, dialog))
      return;
    if (threadInteraction(event, dialog))
      return;
    using Kind = api::InteractionKind;
    if (action.kind == Kind::JoinGuild && !event.guildVerificationRequired)
      for (const auto& guild : event.guilds)
        if (guild.id == action.guild && std::ranges::find(m_guilds, guild.id, &api::Guild::id) == m_guilds.end()) {
          m_guilds.push_back(guild);
          m_folders.push_back({{}, {}, 0, {guild.id}});
        }
    if (action.kind == Kind::AddReaction) {
      m_sessionStore.recordReaction(m_account.id, action.emoji);
      m_frequentReactions = m_sessionStore.frequentReactions(m_account.id);
      const auto colon = action.emoji.find(':');
      recordEmojiUse(colon == std::string::npos ? action.emoji : action.emoji.substr(colon + 1));
    }
    if (action.kind == Kind::AddReaction || action.kind == Kind::RemoveReaction || action.kind == Kind::DeleteMessage)
      invalidateReactors(action.channel, action.message);
    api::UserUpdates changes;
    if (action.kind == Kind::Nickname)
      changes.users[action.user].nickname = action.text;
    else if (action.kind == Kind::Block)
      changes.users[action.user].relationship = 2;
    else if (action.kind == Kind::Unblock || action.kind == Kind::RemoveFriend) {
      changes.users[action.user].relationship = 0;
      changes.users[action.user].nickname = "";
    } else if (action.kind == Kind::Mute || action.kind == Kind::Unmute)
      changes.mutedChannels[action.channel] = action.kind == Kind::Mute;
    m_userDirectory.apply(changes);
    if (!changes.users.empty())
      m_messageLayouts.clear();
    if (action.kind == Kind::DeleteMessage) {
      deleteMessages(action.channel, {action.message});
    } else if (action.kind == Kind::RemoveEmbeds) {
      if (event.patch.flags && (*event.patch.flags & api::messageSuppressEmbeds)) {
        auto patch = event.patch;
        const auto retainFlags = [&](const std::vector<api::Message>& messages) {
          const auto message = std::ranges::find(messages, action.message, &api::Message::id);
          if (message == messages.end())
            return false;
          patch.flags = message->flags | api::messageSuppressEmbeds;
          return true;
        };
        if (action.channel != m_channelId || !retainFlags(m_messages))
          if (const auto cached = m_channelMessages.find(action.channel); cached != m_channelMessages.end())
            retainFlags(cached->second);
        updateMessage(action.channel, action.message, patch);
        const api::Event update{
            .operation = api::Operation::MessageUpdate,
            .resource = action.channel,
            .messageId = action.message,
            .patch = patch
        };
        inboxEvent(update);
      }
    } else if (action.kind == Kind::EditMessage && !m_messageLog.known(action.channel, action.message)) {
      const api::Message* prior = nullptr;
      if (action.channel == m_channelId) {
        const auto found = std::ranges::find(m_messages, action.message, &api::Message::id);
        if (found != m_messages.end())
          prior = &*found;
      }
      if (!prior)
        if (const auto cache = m_channelMessages.find(action.channel); cache != m_channelMessages.end()) {
          const auto found = std::ranges::find(cache->second, action.message, &api::Message::id);
          if (found != cache->second.end())
            prior = &*found;
        }
      if (prior && !event.messages.empty()) {
        if (MessageLog::stale(*prior, event.messages.front()))
          return;
        logMessageEdit(*prior, event.messages.front());
      }
      auto update = [&](std::vector<api::Message>& messages) {
        if (!event.messages.empty())
          for (auto& message : messages)
            if (message.id == action.message)
              message = event.messages.front();
      };
      if (const auto cached = m_channelMessages.find(action.channel); cached != m_channelMessages.end())
        update(cached->second);
      if (action.channel == m_channelId) {
        update(m_messages);
        m_liveChanges[action.message] = event.messages.empty() ? std::optional<api::Message>{} : event.messages.front();
        m_messageLayouts.erase(action.message);
      }
    } else if (action.kind == Kind::OpenDm && !event.channels.empty()) {
      const auto& channel = event.channels.front();
      std::erase_if(m_privateChannels, [&](const auto& old) { return old.id == channel.id; });
      m_privateChannels.insert(m_privateChannels.begin(), channel);
      showDirectMessages();
      selectChannel(channel.id);
    } else if (action.kind == Kind::GuildProfile && dialog) {
      if (event.guildProfile)
        m_guildProfile = *event.guildProfile;
      m_dialogPending = false;
      return;
    } else if (action.kind == Kind::JoinGuild && dialog) {
      m_guildPreviewJoined = true;
      m_guildPreviewVerification = event.guildVerificationRequired || m_guildProfile.recruitment;
      if (!m_guildPreviewVerification && std::ranges::find(m_guilds, action.guild, &api::Guild::id) == m_guilds.end()) {
        m_guilds.push_back({m_guildProfile.id, m_guildProfile.name, m_guildProfile.icon});
        m_folders.push_back({{}, {}, 0, {action.guild}});
      }
      m_dialogPending = false;
      return;
    } else if (action.kind == Kind::UserProfile && dialog) {
      m_dialogAccount = event.account;
      if (event.profile) {
        {
          m_profileCard = *event.profile;
          api::UserUpdates update;
          update.users[event.profile->user.id].cosmetics = event.profile->user.cosmetics;
          m_userDirectory.apply(update);
        }
        if (m_profileCard.roles
            && !m_profileCard.guild.empty()
            && !m_roleDirectory.memberships(m_profileCard.guild, m_profileCard.user.id)) {
          api::GuildRoleUpdate update{.guild = m_profileCard.guild};
          update.members[m_profileCard.user.id] = *m_profileCard.roles;
          m_roleDirectory.apply({false, {std::move(update)}});
        }
      }
      m_dialogPending = false;
      return;
    }
    if (dialog)
      closeDialog();
    m_status.clear();
  }
  void ClientApplication::invalidateReactors(const std::string& channel, const std::string& message) {
    const auto prefix =
        std::to_string(static_cast<int>(api::InteractionKind::ListReactions)) + ":" + channel + ":" + message + ":";
    std::erase_if(m_reactorPages, [&](const auto& item) { return item.first.starts_with(prefix); });
    m_hoverReactionKey.clear();
  }
  void ClientApplication::requestReactors(api::Interaction action) {
    if (m_demo
        || m_messageLog.known(action.channel, action.message)
        || !m_connected
        || m_channelId != action.channel
        || m_contextOpen
        || !m_dialogKind.empty()
        || settingsVisible()
        || !m_viewImage.empty())
      return;
    const auto key = interactionKey(action);
    const auto now = std::chrono::steady_clock::now();
    if (const auto old = m_reactorPages.find(key); old != m_reactorPages.end())
      if (old->second.pending || now - old->second.touched < std::chrono::seconds(30))
        return;
    if (!m_reactorPages.contains(key) && m_reactorPages.size() >= 128) {
      const auto oldest =
          std::ranges::min_element(m_reactorPages, {}, [](const auto& item) { return item.second.touched; });
      m_reactorPages.erase(oldest);
    }
    action.revision = ++m_reactorSerial;
    auto& page = m_reactorPages[key];
    page = {};
    page.revision = action.revision;
    page.touched = now;
    page.pending = m_client.interact(action);
    if (!page.pending)
      page.error = "Could not load reactions.";
    m_window.invalidate();
  }
  void ClientApplication::hoverReaction(const api::Message& message, const api::Reaction& reaction, const Hit& chip) {
    if (m_contextOpen
        || !m_dialogKind.empty()
        || settingsVisible()
        || !m_viewImage.empty()
        || chip.y < 114
        || chip.y + chip.height > m_composerTop
        || !chip.contains(m_pointerX, m_pointerY))
      return;
    m_reactionHoveredThisFrame = true;
    api::Interaction action{
        .kind = api::InteractionKind::ListReactions,
        .channel = message.channelId,
        .message = message.id,
        .emoji = api::reactionKey(reaction)
    };
    const auto key = interactionKey(action);
    if (m_hoverReactionKey != key) {
      m_hoverReactionKey = key;
      m_reactorHoverTimer.start(std::chrono::milliseconds(200), [this, action, burst = reaction.burstCount > 0, key] {
        if (m_hoverReactionKey != key)
          return;
        requestReactors(action);
        if (burst) {
          auto super = action;
          super.burst = true;
          requestReactors(std::move(super));
        }
      });
    }
    std::vector<api::Account> people;
    bool pending = false, failed = false, loaded = false;
    for (bool burst : {false, true}) {
      if (burst && reaction.burstCount == 0)
        continue;
      action.burst = burst;
      const auto found = m_reactorPages.find(interactionKey(action));
      if (found == m_reactorPages.end()) {
        pending = true;
        continue;
      }
      const auto& page = found->second;
      pending |= page.pending;
      failed |= !page.error.empty();
      loaded |= page.loaded;
      for (const auto& person : page.users)
        if (std::ranges::none_of(people, [&](const auto& old) { return old.id == person.id; }))
          people.push_back(person);
    }
    if (people.empty())
      m_tooltip = pending ? "Loading people who reacted…"
          : failed        ? "Could not load people who reacted."
          : loaded        ? "No reactions remain."
                          : "Loading people who reacted…";
    else {
      const auto shown = std::min<std::size_t>(people.size(), 8);
      std::string names;
      for (std::size_t i = 0; i < shown; ++i) {
        if (i)
          names += i + 1 == shown ? " and " : ", ";
        names += people[i].id == m_account.id ? "You" : userName(people[i].id, people[i].name);
      }
      // A person can have both normal and super reactions. Only report the
      // number of distinct people actually returned; do not invent a total.
      if (people.size() > shown)
        names += " and " + std::to_string(people.size() - shown) + " more";
      m_tooltip = names + " reacted with " + (reaction.id.empty() ? reaction.name : ":" + reaction.name + ":");
      if (people.size() >= 100)
        m_tooltip += " (showing the first people who reacted)";
      if (pending)
        m_tooltip += "\nLoading more reactions…";
      else if (failed)
        m_tooltip += "\nSome reactions could not be loaded.";
    }
    m_tooltipX = chip.x + chip.width / 2;
    m_tooltipY = chip.y;
  }
  float ClientApplication::reactionStrip(
      Node* parent, const api::Message& message, float x, float y, float width, bool addButton
  ) {
    if (!m_preferences.showReactions || message.reactions.empty())
      return 0;
    float rx = 0, ry = 0;
    for (const auto& reaction : message.reactions) {
      const auto count = std::to_string(reaction.count);
      const float chip =
          42 + m_window.target().renderer().measureText(count, Style::fontSizeCaption, FontWeight::Medium).width;
      if (rx && rx + chip > width) {
        rx = 0;
        ry += Style::space2Xl;
      }
      if (parent) {
        const auto ax = x + rx, ay = y + ry;
        float ox = 0, oy = 0;
        for (auto* node = parent; node && node != m_scene.get(); node = node->parent()) {
          ox += node->x();
          oy += node->y();
        }
        Hit hit{ax + ox, ay + oy, chip, 28, Action::ToggleReaction, message.id + "\n" + api::reactionKey(reaction)};
        const bool over = !message.deleted && hit.contains(m_pointerX, m_pointerY);
        const bool mine = reaction.me || reaction.meBurst;
        auto frame = std::make_unique<RectNode>();
        frame->setPosition(ax, ay);
        frame->setSize(chip, 28);
        frame->setStyle(
            RoundedRectStyle{
                .fill = mine ? lerpColor(background, accent, over ? 0.19F : 0.10F)
                    : over   ? raised
                             : panel,
                .border = mine || over ? withAlpha(accent, mine ? 0.8F : 0.45F) : withAlpha(muted, 0.16F),
                .radius = Style::scaledRadius(Style::surfaceRadius),
                .borderWidth = 1
            }
        );
        parent->addChild(std::move(frame));
        if (reaction.id.empty()) {
          // Color-emoji fallback fonts can have a wider em than the requested
          // point size. Fit the actual metrics, so Pango cannot ellipsize the
          // reaction into a row of dots inside its reserved square.
          const auto natural = m_window.target().renderer().measureText(reaction.name, Style::fontSizeHeader);
          const float size = 20
              * std::min({1.0F, 20 / std::max(1.0F, natural.width), 20 / std::max(1.0F, natural.bottom - natural.top)});
          const auto emoji = m_window.target().renderer().measureText(reaction.name, size);
          label(
              *parent, ax + 7 + (20 - emoji.width) / 2, ay + (28 - (emoji.bottom - emoji.top)) / 2,
              std::ceil(emoji.width) + Style::spaceXxs, reaction.name, size, foreground
          );
        } else
          emojiImage(*parent, ax + 7, ay + Style::spaceXs, 20, api::emojiPath(reaction.id));
        const auto metrics =
            m_window.target().renderer().measureText(count, Style::fontSizeCaption, FontWeight::Medium);
        label(
            *parent, ax + 34, ay + (28 - (metrics.bottom - metrics.top)) / 2, chip - 39, count, Style::fontSizeCaption,
            mine ? accent : foreground, FontWeight::Medium
        );
        if (!message.deleted) {
          m_hits.push_back(hit);
          hoverReaction(message, reaction, hit);
        }
      }
      rx += chip + 6;
    }
    if (addButton && !message.deleted) {
      if (rx + Style::spaceWide > width) {
        rx = 0;
        ry += Style::space2Xl;
      }
      if (parent)
        iconButton(*parent, {x + rx, y + ry, 28, 28, Action::AddReaction, message.id}, "mood-smile", "Add reaction");
    }
    return ry + Style::space2Xl;
  }
  bool ClientApplication::interactionAction(const Hit& hit) {
    if (inboxAction(hit))
      return true;
    if (channelFeatureAction(hit))
      return true;
    if (threadAction(hit))
      return true;
    if (guildProfileAction(hit))
      return true;
    if (profileAction(hit))
      return true;
    switch (hit.action) {
    case Action::UserProfile:
      showProfile(hit.id);
      break;
    case Action::MessageMenu:
      openContext(m_pointerX, m_pointerY);
      break;
    case Action::Reply:
    case Action::AddReaction:
    case Action::RemoveEmbeds:
      m_contextMessage = hit.id;
      m_contextChannel = m_channelId;
      contextAction(hit.action);
      break;
    case Action::CancelReply:
      m_replies.erase(m_channelId);
      focusControl("composer");
      break;
    case Action::AutoModActions:
    case Action::AutoModJump:
    case Action::AutoModReport:
      autoModerationAction(hit);
      break;
    case Action::ToggleReplyMention:
      if (m_replies.contains(m_channelId))
        m_replies[m_channelId].mention = !m_replies[m_channelId].mention;
      break;
    case Action::SubmitDialog:
      submitDialog();
      break;
    case Action::CloseDialog:
      if (channelEditClose())
        break;
      if (!m_dialogPending
          || m_dialogKind == "profile"
          || m_dialogKind == "guild-profile"
          || m_dialogKind == "sidebar-invite")
        closeDialog();
      break;
    case Action::RevealBlocked:
      m_revealedBlocked.insert(hit.id);
      break;
    case Action::JumpReply:
      jumpToMessage(m_channelId, hit.id);
      break;
    case Action::ToggleReaction: {
      const auto split = hit.id.find('\n');
      if (split == std::string::npos)
        break;
      const auto message = std::ranges::find(m_messages, hit.id.substr(0, split), &api::Message::id);
      if (message == m_messages.end())
        break;
      const auto key = hit.id.substr(split + 1);
      const auto reaction =
          std::ranges::find_if(message->reactions, [&](const auto& value) { return api::reactionKey(value) == key; });
      if (reaction == message->reactions.end())
        break;
      startInteraction(
          {.kind = reaction->me ? api::InteractionKind::RemoveReaction : api::InteractionKind::AddReaction,
           .channel = m_channelId,
           .message = message->id,
           .emoji = key}
      );
      break;
    }
    case Action::InsertEmoji:
      if (m_reactionMessage.empty())
        return false;
      startInteraction(
          {.kind = api::InteractionKind::AddReaction,
           .channel = m_reactionChannel,
           .message = m_reactionMessage,
           .emoji = hit.id}
      );
      m_reactionMessage.clear();
      m_reactionChannel.clear();
      m_emojiMenu = false;
      break;
    case Action::ToggleEmoji:
      m_reactionMessage.clear();
      m_reactionChannel.clear();
      return false;
    default:
      return false;
    }
    m_window.invalidate();
    return true;
  }
  void ClientApplication::demoInteractionStep() {
    // Exercise actual window callbacks and scene rebuilds, entirely offline.
    const auto step = m_demoInteractionStep++ % 16;
    const auto titleOffset = m_window.titlebarHeight() - layout::Window::titlebarHeight;
    auto require = [](bool condition, const char* text) {
      if (!condition)
        throw std::runtime_error(text);
    };
    const auto message = std::ranges::find_if(m_contextTargets, [](const auto& hit) {
      return hit.action == Action::MessageMenu && hit.id == "3";
    });
    require(message != m_contextTargets.end(), "Interaction replay needs a visible message");
    const float x = message->x + 110, y = message->y + Style::spaceDense;
    if (step == 0) {
      closeContext();
      m_window.motion(x, y + titleOffset);
    } else if (step == 1 || step == 5) {
      if (step == 5)
        closeContext();
      const auto button = std::ranges::find_if(m_hits, [](const auto& hit) {
        return hit.action == Action::AddReaction && hit.id == "3";
      });
      if (button != m_hits.end()) {
        m_window.motion(button->x + Style::spaceSm, button->y + Style::spaceSm + titleOffset);
        m_window.click(button->x + Style::spaceSm, button->y + Style::spaceSm + titleOffset, 272);
        m_window.released();
      } else {
        // The prior submenu may have moved hover away from the toolbar.
        (void)interactionAction({0, 0, 0, 0, Action::AddReaction, "3"});
      }
      require(m_emojiMenu && !m_contextOpen, "Reaction toolbar must open the picker");
    } else if (step == 2 || step == 6) {
      require(m_emojiMenu, "Picker must survive a scene rebuild before right-click");
      const auto target = std::ranges::find_if(m_contextTargets, [&](const auto& hit) {
        return hit.action == Action::MessageMenu && hit.id == (step == 2 ? "1" : "4");
      });
      require(target != m_contextTargets.end(), "Replay needs a different visible message");
      m_window.motion(target->x + 110, target->y + Style::spaceDense + titleOffset);
      m_window.contextClick(target->x + 110, target->y + Style::spaceDense + titleOffset);
      require(m_contextOpen && !m_emojiMenu, "Right-click must replace the picker with the message menu");
    } else if (step == 3 || step == 7) {
      require(m_contextControl != nullptr && m_contextOpen, "Context menu must survive the picker transition");
      m_window.motion(
          m_contextBounds.x + (Style::space2Xl - Style::spaceXxs),
          m_contextBounds.y + 58 + m_contextControl->rowTop(0) + 12 + titleOffset
      );
      require(m_reactionSubmenu, "Hover must open the reaction submenu");
    } else if (step == 4) {
      require(m_reactionControl != nullptr, "Reaction submenu must survive a scene rebuild");
      m_window.motion(
          m_reactionSubmenuBounds.x + Style::spaceRelaxed, m_reactionSubmenuBounds.y + Style::spaceRelaxed + titleOffset
      );
      m_window.scroll(30);
    }
    if (step >= 8) {
      const auto other = std::ranges::find_if(m_contextTargets, [](const auto& hit) {
        return hit.action == Action::MessageMenu && hit.id == "2";
      });
      require(other != m_contextTargets.end(), "Rapid replay needs another message");
      if (step == 8 || step == 11) {
        closeContext();
        (void)interactionAction({0, 0, 0, 0, Action::AddReaction, "3"});
        require(m_emojiMenu, "Rapid toolbar activation opens the picker");
        m_window.motion(other->x + 110, other->y + Style::spaceDense + titleOffset);
        m_window.contextClick(other->x + 110, other->y + Style::spaceDense + titleOffset);
        require(m_contextOpen && !m_emojiMenu, "Rapid right-click replaces the picker");
      } else if (step == 9) {
        const auto user = std::ranges::find_if(m_contextTargets, [](const auto& hit) {
          return hit.action == Action::UserMenu && hit.id == "11";
        });
        require(user != m_contextTargets.end(), "Replay needs a visible user");
        m_window.motion(
            user->x + (Style::spaceXs + Style::borderWidth),
            user->y + (Style::spaceXs + Style::borderWidth) + titleOffset
        );
        m_window.contextClick(
            user->x + (Style::spaceXs + Style::borderWidth),
            user->y + (Style::spaceXs + Style::borderWidth) + titleOffset
        );
      } else if (step == 10) {
        m_window.motion(
            m_contextBounds.x + (Style::space2Xl - Style::spaceXxs),
            m_contextBounds.y + Style::spaceRelaxed + titleOffset
        );
        m_window.contextClick(x, y + titleOffset);
      } else if (step == 12) {
        m_window.motion(
            m_contextBounds.x + (Style::space2Xl - Style::spaceXxs), m_contextBounds.y + 58 + 16 + titleOffset
        );
        require(m_reactionSubmenu, "Rapid parent menu opens a submenu");
      } else if (step == 13) {
        m_window.motion(
            m_reactionSubmenuBounds.x + Style::spaceXl, m_reactionSubmenuBounds.y + Style::spaceRelaxed + titleOffset
        );
        m_window.click(
            m_reactionSubmenuBounds.x + Style::spaceXl, m_reactionSubmenuBounds.y + Style::spaceRelaxed + titleOffset,
            272
        );
        m_window.scroll(30);
      } else if (step == 14) {
        m_window.released();
        closeContext();
        (void)interactionAction({0, 0, 0, 0, Action::AddReaction, "3"});
      } else if (step == 15)
        m_window.contextClick(x, y + titleOffset);
    }
    std::clog << "Offline interaction replay step " << m_demoInteractionStep << " passed\n";
    m_window.invalidate();
  }
} // namespace app
