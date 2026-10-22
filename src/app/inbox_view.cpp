#include "app/client_application.h"
#include "app/notification_preview.h"
#include "app/theme.h"
#include "core/files/resource_paths.h"
#include "render/core/texture_manager.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <array>
#include <glib.h>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace app {
  namespace {
    constexpr std::size_t maximumPreviewText = 4096;
    constexpr std::size_t maximumInboxWrites = 16;
    constexpr std::array<int, 4> reminderMinutes{20, 60, 180, 1440};
    const std::array<std::string, 4> inboxNames{"Unread", "Mentions", "Bookmarks", "Reminders"};
    std::string key(const api::SavedMessage& saved) { return saved.channel + ":" + saved.id; }
    std::string bounded(std::string value) {
      if (value.size() > maximumPreviewText) {
        auto end = maximumPreviewText;
        while (end && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80)
          --end;
        value.resize(end);
        value += "…";
      }
      return value;
    }
    std::string readableTime(const std::string& timestamp) {
      auto* date = g_date_time_new_from_iso8601(timestamp.c_str(), nullptr);
      if (!date)
        return "";
      auto* local = g_date_time_to_local(date);
      auto* formatted = g_date_time_format(local, "%d %b, %H:%M");
      std::string result = formatted ? formatted : "";
      g_free(formatted);
      g_date_time_unref(local);
      g_date_time_unref(date);
      return result;
    }
  } // namespace

  void ClientApplication::closeInbox() {
    m_inboxOpen = m_inboxFilters = false;
    m_inboxBounds = {};
    m_inboxRemindTarget.clear();
    m_scrolling.stop(m_inboxScroll);
    m_window.invalidate();
  }
  void ClientApplication::clearInbox() {
    closeInbox();
    m_inboxReminderTimer.stop();
    m_inboxTabs = {};
    m_inboxPreviews.clear();
    m_inboxUnread.clear();
    m_inboxVisibleUnread.clear();
    m_inboxWrites.clear();
    m_inboxNotified.clear();
    m_inboxDemoWrite.reset();
    m_inboxStatus.clear();
    m_inboxBootstrap = false;
    m_inboxGuild = m_inboxMuted = false;
    m_inboxRoles = m_inboxEveryone = true;
    m_inboxTab = 0;
    m_inboxScroll = 0;
    m_inboxUnreadPage = 0;
    ++m_inboxRevision;
  }
  void ClientApplication::toggleInbox() {
    if (!m_connected || settingsVisible() || !m_dialogKind.empty() || !m_viewImage.empty() || m_videoExpanded)
      return;
    if (m_inboxOpen) {
      closeInbox();
      return;
    }
    closeContext();
    closeSearchMenu(true);
    closeCompletion();
    closeGifPicker();
    m_emojiMenu = m_presenceMenu = false;
    m_controlInput.cancelPointerCapture();
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_fieldFocused = false;
    m_scrolling.stopAll();
    m_inboxOpen = true;
    updateChatPreload();
    m_inboxScroll = 0;
    if (m_inboxTab && !m_inboxTabs[m_inboxTab - 1].pending)
      m_inboxTabs[m_inboxTab - 1].loaded = false;
    requestInbox();
    m_window.invalidate();
  }
  void ClientApplication::requestInbox(bool more) {
    if (!m_connected || m_inboxTab == 0)
      return;
    auto& tab = m_inboxTabs[m_inboxTab - 1];
    if (tab.pending || (!more && tab.loaded) || (more && !tab.page.more))
      return;
    api::InboxQuery query;
    query.kind = m_inboxTab == 1 ? api::InboxKind::Mentions
        : m_inboxTab == 2        ? api::InboxKind::Bookmarks
                                 : api::InboxKind::Reminders;
    query.request = ++m_inboxSerial;
    query.before = more ? tab.page.before : "";
    if (query.kind == api::InboxKind::Mentions) {
      query.guild = m_inboxGuild && !m_dmMode ? m_guildId : "";
      query.roles = m_inboxRoles;
      query.everyone = m_inboxEveryone;
    }
    tab.error.clear();
    if (m_demo || m_client.inbox(query)) {
      tab.pending = query;
      tab.revision = m_inboxRevision;
    } else
      tab.error = "Could not load the inbox. Please try again.";
  }
  void ClientApplication::updateInboxPreviews() {
    if (!m_inboxOpen || m_inboxTab != 0)
      return;
    auto active =
        std::ranges::count_if(m_inboxPreviews, [](const auto& pair) { return pair.second.pending.has_value(); });
    for (const auto& channel : m_inboxVisibleUnread) {
      const auto cursor = m_unreadTimeline.cursor(channel);
      if (!cursor)
        continue;
      auto found = m_inboxPreviews.find(channel);
      if (found == m_inboxPreviews.end() && m_inboxPreviews.size() >= layout::Inbox::maximumPreviews) {
        const auto victim = std::ranges::find_if(m_inboxPreviews, [&](const auto& entry) {
          return !entry.second.pending && !m_inboxVisibleUnread.contains(entry.first);
        });
        if (victim == m_inboxPreviews.end())
          break;
        m_inboxPreviews.erase(victim);
      }
      auto& preview = m_inboxPreviews[channel];
      if (preview.latest != cursor->latest || preview.after != cursor->after) {
        if (preview.pending)
          continue;
        preview = {};
        preview.latest = cursor->latest;
        preview.after = cursor->after;
      }
      if (preview.pending
          || !preview.messages.empty()
          || !preview.error.empty()
          || static_cast<std::size_t>(active) >= layout::Inbox::previewConcurrency)
        continue;
      api::InboxQuery query{
          .kind = api::InboxKind::Unread, .channel = channel, .before = cursor->after, .request = ++m_inboxSerial
      };
      if (m_demo || m_client.inbox(query)) {
        preview.pending = query;
        ++active;
      } else
        preview.error = "Could not load this preview.";
    }
  }

  void ClientApplication::inboxEvent(const api::Event& event) {
    if (event.operation == api::Operation::MessageCreate) {
      std::string guild;
      if (const auto at = m_unreadGuilds.find(event.resource); at != m_unreadGuilds.end())
        guild = at->second;
      if (guild.empty())
        for (const auto& server : m_guilds)
          if (serverChannel(server.id, event.resource) || m_threads.find(server.id, event.resource)) {
            guild = server.id;
            break;
          }
      const auto roles = m_roleDirectory.memberships(guild, m_account.id).value_or(std::vector<std::string>{});
      const bool mentioned = !m_account.id.empty() && std::ranges::any_of(event.messages, [&](const auto& message) {
        const bool roleMention = m_inboxRoles && std::ranges::any_of(message.mentionRoles, [&](const auto& role) {
                                   return std::ranges::find(roles, role) != roles.end();
                                 });
        return !message.deleted
            && (message.mentions.contains(m_account.id) || (m_inboxEveryone && message.mentionEveryone) || roleMention);
      });
      if (mentioned) {
        ++m_inboxRevision;
        m_inboxTabs[0].loaded = false;
        if (m_inboxOpen && m_inboxTab == 1)
          requestInbox();
      }
      return;
    }
    if (event.operation == api::Operation::MessageUpdate || event.operation == api::Operation::MessageDelete) {
      const bool removed = event.operation == api::Operation::MessageDelete;
      const auto matches = [&](const api::Message& message) {
        return message.channelId == event.resource
            && (removed ? std::ranges::find(event.removed, message.id) != event.removed.end()
                        : message.id == event.messageId);
      };
      const auto update = [&](std::vector<api::Message>& messages) {
        if (removed)
          std::erase_if(messages, matches);
        else
          for (auto& message : messages)
            if (matches(message))
              event.patch.apply(message);
      };
      update(m_inboxTabs[0].page.messages);
      for (auto& [_, preview] : m_inboxPreviews)
        update(preview.messages);
      for (int index : {1, 2})
        for (auto& saved : m_inboxTabs[index].page.saved)
          if (saved.message && matches(*saved.message)) {
            if (removed)
              saved.message.reset();
            else
              event.patch.apply(*saved.message);
          }
      ++m_inboxRevision;
      return;
    }
    if (!event.inboxQuery)
      return;
    const auto& query = *event.inboxQuery;
    if (event.operation == api::Operation::InboxRead) {
      if (query.kind == api::InboxKind::Unread) {
        const auto at = m_inboxPreviews.find(query.channel);
        if (at == m_inboxPreviews.end() || !at->second.pending || *at->second.pending != query)
          return;
        auto& preview = at->second;
        preview.pending.reset();
        preview.error = event.error;
        if (event.inboxPage && event.error.empty()) {
          preview.messages = event.inboxPage->messages;
          if (preview.messages.empty())
            preview.error = "No preview is available. Open the channel to read it.";
        }
      } else {
        const int index = query.kind == api::InboxKind::Mentions ? 0 : query.kind == api::InboxKind::Bookmarks ? 1 : 2;
        auto& tab = m_inboxTabs[index];
        if (!tab.pending || *tab.pending != query)
          return;
        tab.pending.reset();
        if (tab.revision != m_inboxRevision) {
          tab.loaded = false;
          if ((m_inboxOpen && m_inboxTab == index + 1) || index == 2) {
            const auto selected = m_inboxTab;
            m_inboxTab = index + 1;
            requestInbox();
            m_inboxTab = selected;
          }
          return;
        }
        tab.error = event.error;
        if (event.inboxPage && event.error.empty()) {
          tab.page = *event.inboxPage;
          tab.loaded = true;
          m_scrolling.stop(m_inboxScroll);
          m_inboxScroll = 0;
          if (index == 2)
            scheduleInboxReminders();
        }
      }
      m_window.invalidate();
      return;
    }
    if (event.operation == api::Operation::InboxWrite) {
      const auto pending = m_inboxWrites.find(query.request);
      if (pending == m_inboxWrites.end() || pending->second != query)
        return;
      m_inboxWrites.erase(pending);
      m_inboxDemoWrite.reset();
      if (!event.error.empty()) {
        m_inboxStatus = event.error + " The change was not confirmed. Refresh before trying again.";
        m_window.invalidate();
        return;
      }
      m_inboxStatus = query.kind == api::InboxKind::Save ? query.due.empty() ? "Message bookmarked." : "Reminder saved."
          : query.kind == api::InboxKind::Dismiss        ? "Mention dismissed."
                                                         : "Saved message removed.";
    }

    if (event.operation == api::Operation::InboxUpdate) {
      std::erase_if(m_inboxWrites, [&](const auto& entry) {
        return entry.second.message == query.message
            && (query.channel.empty() || entry.second.channel == query.channel);
      });
      if (m_inboxDemoWrite && !m_inboxWrites.contains(m_inboxDemoWrite->request))
        m_inboxDemoWrite.reset();
      m_inboxStatus.clear();
    }
    ++m_inboxRevision;
    if (query.kind == api::InboxKind::Dismiss) {
      std::erase_if(m_inboxTabs[0].page.messages, [&](const auto& message) { return message.id == query.message; });
      m_inboxTabs[0].loaded = false;
    } else {
      for (int index : {1, 2}) {
        auto& tab = m_inboxTabs[index];
        std::erase_if(tab.page.saved, [&](const auto& saved) {
          return saved.channel == query.channel && saved.id == query.message;
        });
        tab.loaded = false;
      }
      if (query.kind == api::InboxKind::Save && event.inboxPage && !event.inboxPage->saved.empty()) {
        auto saved = event.inboxPage->saved.front();
        auto& page = m_inboxTabs[saved.due.empty() ? 1 : 2].page.saved;
        page.insert(page.begin(), std::move(saved));
        if (page.size() > api::inboxMaximumSaved)
          page.resize(api::inboxMaximumSaved);
      }

      m_inboxNotified.erase(query.channel + ":" + query.message);
      std::ranges::sort(m_inboxTabs[2].page.saved, {}, &api::SavedMessage::due);
      scheduleInboxReminders();
    }
    if (m_inboxOpen)
      requestInbox();
    m_window.invalidate();
  }
  void ClientApplication::scheduleInboxReminders() {
    m_inboxReminderTimer.stop();
    const auto now = std::chrono::system_clock::now();
    std::erase_if(m_inboxNotified, [&](const auto& id) {
      return std::ranges::none_of(m_inboxTabs[2].page.saved, [&](const auto& saved) { return key(saved) == id; });
    });
    std::optional<std::chrono::system_clock::time_point> next;
    for (const auto& saved : m_inboxTabs[2].page.saved) {
      const auto due = api::inboxTime(saved.due);
      if (!due || m_inboxNotified.contains(key(saved)))
        continue;
      if (*due <= now) {
        m_inboxNotified.insert(key(saved));
        continue; // Previously due reminders stay visible without a notification burst on sign-in.
      }
      if (!next || *due < *next)
        next = due;
    }
    if (!next || m_demo)
      return;
    const auto delay = std::min(
        std::chrono::duration_cast<std::chrono::milliseconds>(*next - now),
        std::chrono::duration_cast<std::chrono::milliseconds>(layout::Inbox::reminderWakeLimit)
    );
    m_inboxReminderTimer.start(delay, [this] {
      const auto now = std::chrono::system_clock::now();
      for (const auto& saved : m_inboxTabs[2].page.saved) {
        const auto due = api::inboxTime(saved.due);
        if (!due || *due > now || !m_inboxNotified.insert(key(saved)).second)
          continue;
        if (m_connected && m_preferences.desktopNotifications && m_presence.status != "dnd") {
          DesktopNotice notice;
          notice.account = m_account.id;
          notice.generation = m_session;
          notice.guild = saved.guild;
          notice.channel = saved.channel;
          notice.message = saved.id;
          notice.sound = m_preferences.notificationSounds;
          notice.title = "Message reminder";
          notice.body = "A saved message is ready to revisit.";
          if (!m_preferences.notificationPrivacy)
            notice.body = bounded(
                saved.message
                    ? desktopMessagePreview(
                          *saved.message, [&](char, const std::string& id) { return userName(id, "Unknown user"); }
                      )
                    : saved.summary
            );
          m_desktopNotifications.show(std::move(notice));
        }
      }
      scheduleInboxReminders();
      m_window.invalidate();
    });
  }

  bool ClientApplication::inboxAction(const Hit& hit) {
    if (hit.action == Action::Inbox) {
      toggleInbox();
      return true;
    }
    if (hit.action == Action::BookmarkMessage || hit.action == Action::RemindMessage) {
      const auto channel = m_contextChannel.empty() ? m_channelId : m_contextChannel;
      const auto message = hit.id.empty() ? m_contextMessage : hit.id;
      if (!api::validSnowflake(channel) || !api::validSnowflake(message))
        return true;
      closeContext();
      if (!m_inboxOpen)
        toggleInbox();
      if (hit.action == Action::RemindMessage) {
        m_inboxRemindTarget = channel + ":" + message;
        m_window.invalidate();
      } else
        inboxAction({.action = Action::InboxAction, .id = "save:" + channel + ":" + message});
      return true;
    }
    if (hit.action != Action::InboxAction)
      return false;
    const auto& command = hit.id;
    if (command.starts_with("tab:")) {
      m_inboxTab = std::clamp(std::stoi(command.substr(4)), 0, 3);
      m_inboxRemindTarget.clear();
      m_inboxStatus.clear();
      m_scrolling.stop(m_inboxScroll);
      m_inboxScroll = 0;
      requestInbox();
    } else if (command == "close")
      closeInbox();
    else if (command == "filters") {
      m_inboxFilters = !m_inboxFilters;
      m_scrolling.stop(m_inboxScroll);
    } else if (command == "muted" || command == "guild" || command == "roles" || command == "everyone") {
      if (command == "muted")
        m_inboxMuted = !m_inboxMuted;
      else if (command == "guild")
        m_inboxGuild = !m_inboxGuild;
      else if (command == "roles")
        m_inboxRoles = !m_inboxRoles;
      else
        m_inboxEveryone = !m_inboxEveryone;
      m_inboxUnreadPage = 0;
      if (command != "muted") {
        m_inboxTabs[0].pending.reset();
        m_inboxTabs[0].loaded = false;
        m_inboxTabs[0].page = {};
      }
      m_inboxScroll = 0;
      requestInbox();
    } else if (command == "refresh" || command == "older" || command == "newest") {
      m_inboxStatus.clear();
      if (m_inboxTab) {
        auto& tab = m_inboxTabs[m_inboxTab - 1];
        if (command != "older") {
          tab.pending.reset();
          tab.loaded = false;
        }
        requestInbox(command == "older");
      } else {
        m_inboxPreviews.clear();
        m_inboxUnreadPage = 0;
      }
    } else if (command == "next-unread" || command == "previous-unread") {
      if (command == "next-unread")
        ++m_inboxUnreadPage;
      else if (m_inboxUnreadPage)
        --m_inboxUnreadPage;
      m_scrolling.stop(m_inboxScroll);
      m_inboxScroll = 0;
    } else if (command.starts_with("retry-preview:")) {
      m_inboxPreviews.erase(command.substr(14));
    } else if (command == "cancel-remind") {
      m_inboxRemindTarget.clear();
    } else if (command.starts_with("remind:")) {
      m_inboxRemindTarget = command.substr(7);
    } else if (command.starts_with("jump:")) {
      const auto at = command.find(':', 5);
      const auto second = at == std::string::npos ? at : command.find(':', at + 1);
      if (at != std::string::npos && second != std::string::npos) {
        const auto guild = command.substr(5, at - 5);
        const auto channel = command.substr(at + 1, second - at - 1);
        const auto message = command.substr(second + 1);
        closeInbox();
        if (guild.empty()) {
          m_restoreChannel = channel;
          showDirectMessages(false);
          if (!message.empty() && m_channelId == channel)
            jumpToMessage(channel, message);
        } else if (!m_dmMode && guild == m_guildId) {
          jumpToMessage(channel, message);
        } else {
          selectGuild(guild);
          m_pendingDiscordLink = DiscordLink{guild, channel, message};
          if (!m_loadingChannels)
            resumeDiscordLink();
        }
      }
    } else if (command.starts_with("read:")) {
      const auto channel = command.substr(5);
      const auto cursor = m_unreadTimeline.cursor(channel);
      if (!cursor || !acknowledgeChannelRead(channel, cursor->latest))
        m_inboxStatus = "Could not mark this channel as read.";
    } else {
      api::InboxQuery query;
      std::string target;
      if (command.starts_with("save:")) {
        query.kind = api::InboxKind::Save;
        target = command.substr(5);
      } else if (command.starts_with("remove:")) {
        query.kind = api::InboxKind::Remove;
        target = command.substr(7);
      } else if (command.starts_with("dismiss:")) {
        query.kind = api::InboxKind::Dismiss;
        query.message = command.substr(8);
      } else if (command.starts_with("due:")) {
        const auto separator = command.find(':', 4);
        if (separator == std::string::npos)
          return true;
        const auto minutes = std::stoi(command.substr(4, separator - 4));
        if (std::ranges::find(reminderMinutes, minutes) == reminderMinutes.end())
          return true;
        query.kind = api::InboxKind::Save;
        target = command.substr(separator + 1);
        query.due = api::inboxTimestamp(std::chrono::system_clock::now() + std::chrono::minutes{minutes});
      } else
        return true;
      if (!target.empty()) {
        const auto separator = target.find(':');
        if (separator == std::string::npos)
          return true;
        query.channel = target.substr(0, separator);
        query.message = target.substr(separator + 1);
      }
      const bool pending = std::ranges::any_of(m_inboxWrites, [&](const auto& entry) {
        return entry.second.message == query.message && entry.second.channel == query.channel;
      });
      if (pending || m_inboxWrites.size() >= maximumInboxWrites)
        return true;
      query.request = ++m_inboxSerial;
      if (api::inboxHttpRequest(query) && (m_demo || m_client.inbox(query))) {
        m_inboxWrites[query.request] = query;
        if (m_demo)
          m_inboxDemoWrite = query;
        m_inboxStatus = "Saving…";
        m_inboxRemindTarget.clear();
      } else
        m_inboxStatus = "Could not queue this inbox action.";
    }
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::inboxClick(float x, float y) {
    if (!m_inboxOpen || settingsVisible() || !m_dialogKind.empty() || !m_viewImage.empty() || m_videoExpanded)
      return false;
    const Hit trigger{
        m_window.width() - Style::spaceLg - layout::ChannelHeader::actionWidth, layout::ChannelHeader::controlTop,
        layout::ChannelHeader::actionWidth, layout::ChannelHeader::controlHeight
    };
    if (trigger.contains(x, y)) {
      toggleInbox();
      return true;
    }
    if (!m_inboxBounds.contains(x, y)) {
      closeInbox();
      return false;
    }
    if (const auto bar = m_widgets.find("inbox-scrollbar"); bar != m_widgets.end()
        && bar->second->parent()
        && x >= bar->second->x()
        && x < bar->second->x() + bar->second->width()
        && y >= bar->second->y()
        && y < bar->second->y() + bar->second->height()) {
      m_controlInput.pointerButton(
          x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
      );
      return true;
    }
    for (auto hit = m_hits.rbegin(); hit != m_hits.rend(); ++hit)
      if (hit->action == Action::InboxAction && hit->contains(x, y)) {
        const auto copy = *hit;
        inboxAction(copy);
        break;
      }
    return true;
  }
  bool ClientApplication::inboxKey(const Key& input) {
    if (!m_inboxOpen || settingsVisible() || !m_dialogKind.empty() || !m_viewImage.empty() || m_videoExpanded)
      return false;

    if (input.control || input.alt || input.super) {
      const auto panel = m_channelPanel;
      if (shortcutKey(input) && (settingsVisible() || m_channelPanel != panel))
        closeInbox();
      return true;
    }
    if (input.symbol == XKB_KEY_Escape) {
      if (!m_inboxRemindTarget.empty())
        m_inboxRemindTarget.clear();
      else
        closeInbox();
    } else if (
        !input.control && !input.alt && !input.super && (input.symbol == XKB_KEY_Left || input.symbol == XKB_KEY_Right)
    ) {
      inboxAction(
          {.action = Action::InboxAction,
           .id = "tab:" + std::to_string((m_inboxTab + (input.symbol == XKB_KEY_Right ? 1 : 3)) % 4)}
      );
    } else if (
        input.symbol == XKB_KEY_Down
        || input.symbol == XKB_KEY_Up
        || input.symbol == XKB_KEY_Page_Down
        || input.symbol == XKB_KEY_Page_Up
    ) {
      const float amount =
          input.symbol == XKB_KEY_Down || input.symbol == XKB_KEY_Up ? Style::scrollWheelStep : m_inboxViewport;
      m_scrolling.stop(m_inboxScroll);
      m_inboxScroll = std::max(
          0.0F, m_inboxScroll + (input.symbol == XKB_KEY_Down || input.symbol == XKB_KEY_Page_Down ? amount : -amount)
      );
    }
    m_window.invalidate();
    return true; // Keep chat editing and shortcuts behind the open inbox inactive.
  }

  void ClientApplication::drawInboxButton(Node& root, float width) {
    if (!m_connected || settingsVisible() || !m_dialogKind.empty() || !m_viewImage.empty() || m_videoExpanded)
      return;
    iconButton(
        root,
        {width - Style::spaceLg - layout::ChannelHeader::actionWidth,
         layout::ChannelHeader::controlTop,
         layout::ChannelHeader::actionWidth,
         layout::ChannelHeader::controlHeight,
         Action::Inbox,
         {}},
        "inbox", "Inbox (" + shortcutLabel("inbox") + ")", m_inboxOpen
    );
  }

  void ClientApplication::drawInbox(Node& root, float w, float h) {
    using namespace theme;
    m_inboxBounds = {};
    if (!m_inboxOpen
        || !m_connected
        || settingsVisible()
        || !m_dialogKind.empty()
        || !m_viewImage.empty()
        || m_videoExpanded)
      return;
    const float width = std::min(layout::Inbox::width, w - Style::spaceLg);
    const float x = w - width - Style::spaceSm, y = layout::Inbox::top;
    const float height = std::min(layout::Inbox::maximumHeight, h - y - Style::spaceSm);
    const int filterCount = m_inboxTab == 1 ? 3 : m_inboxTab == 0 ? 2 : 1;
    const float filters = m_inboxFilters ? Style::controlHeightSm * ((filterCount + 1) / 2) : 0;
    const float reminder = m_inboxRemindTarget.empty() ? 0 : Style::controlHeightSm * 2;
    const float top = y + layout::Inbox::header + layout::Inbox::tabs + filters + reminder;
    m_inboxViewport = std::max(0.0F, height - (top - y) - layout::Inbox::footer - Style::spaceSm);
    m_inboxBounds = {x, y, width, height};
    if (m_inboxBounds.contains(m_pointerX, m_pointerY))
      m_tooltip.clear();
    rectangle(
        root, x - Style::borderWidth, y - Style::borderWidth, width + Style::spaceXxs, height + Style::spaceXxs, raised,
        Style::surfaceRadius
    );
    rectangle(root, x, y, width, height, sidebar, Style::surfaceRadius);
    glyph(root, x + Style::spaceLg, y + Style::spaceMd, Style::glyphSizeFeature, "inbox", foreground);
    label(
        root, x + Style::spaceGroup, y + Style::spaceMd, width - Style::spaceSection * 4, "Inbox",
        Style::fontSizeHeading, foreground, FontWeight::SemiBold
    );
    const auto icon = [&](float right, const std::string& glyphName, const std::string& title,
                          const std::string& command) {
      iconButton(
          root,
          {x + width - right, y + Style::spaceSm, Style::controlHeightSm, Style::controlHeightSm, Action::InboxAction,
           command},
          glyphName, title
      );
    };
    icon(Style::controlHeightSm * 3 + Style::spaceMd, "adjustments", "Inbox filters", "filters");
    icon(Style::controlHeightSm * 2 + Style::spaceMd, "refresh", "Refresh inbox", "refresh");
    icon(Style::controlHeightSm + Style::spaceMd, "x", "Close inbox", "close");
    const float tabWidth = (width - Style::spaceLg) / inboxNames.size();
    for (std::size_t i = 0; i < inboxNames.size(); ++i) {
      Hit hit{x + Style::spaceSm + i * tabWidth,
              y + layout::Inbox::header,
              tabWidth,
              layout::Inbox::tabs,
              Action::InboxAction,
              "tab:" + std::to_string(i)};
      button(root, hit, inboxNames[i], false, m_inboxTab == static_cast<int>(i), false, true);
      if (m_inboxTab == static_cast<int>(i))
        rectangle(root, hit.x, hit.y + hit.height - Style::spaceXxs, hit.width, Style::spaceXxs, accent);
    }
    if (m_inboxFilters) {
      const float fy = y + layout::Inbox::header + layout::Inbox::tabs;
      const float half = (width - Style::spaceLg) / 2;
      const auto filter = [&](int index, const std::string& name, const std::string& command, bool checked) {
        button(
            root,
            {x + Style::spaceSm + (index % 2) * half, fy + (index / 2) * Style::controlHeightSm, half,
             Style::controlHeightSm, Action::InboxAction, command},
            std::string(checked ? "✓ " : "") + name, false, checked
        );
      };
      filter(0, "Current server", "guild", m_inboxGuild && !m_dmMode);
      if (m_inboxTab == 0)
        filter(1, "Include muted", "muted", m_inboxMuted);
      else if (m_inboxTab == 1) {
        filter(1, "Role mentions", "roles", m_inboxRoles);
        filter(2, "Everyone mentions", "everyone", m_inboxEveryone);
      }
    }
    if (!m_inboxRemindTarget.empty()) {
      const auto ry = top - reminder;
      label(
          root, x + Style::spaceLg, ry + Style::spaceSm, width - Style::spaceGroup, "Remind me about this message",
          Style::fontSizeCaption, foreground
      );
      iconButton(
          root,
          {x + width - Style::controlHeightSm - Style::spaceSm, ry, Style::controlHeightSm, Style::controlHeightSm,
           Action::InboxAction, "cancel-remind"},
          "x", "Cancel reminder"
      );
      const float part = (width - Style::spaceLg) / reminderMinutes.size();
      const std::array<std::string, 4> names{"20 minutes", "1 hour", "3 hours", "24 hours"};
      for (std::size_t i = 0; i < names.size(); ++i)
        button(
            root,
            {x + Style::spaceSm + i * part, ry + Style::controlHeightSm, part, Style::controlHeightSm,
             Action::InboxAction, "due:" + std::to_string(reminderMinutes[i]) + ":" + m_inboxRemindTarget},
            names[i], false, false, false, true
        );
    }
    struct Row {
      std::string channel, id, guild, title, subtitle, summary, due;
      const api::Message* message = nullptr;
    };
    std::vector<Row> rows;
    m_inboxUnread.clear();
    m_inboxVisibleUnread.clear();
    const auto append = [&](std::string channel, std::string id, std::string guild, const api::Message* message,
                            std::string summary = "", std::string due = "", std::string channelSummary = "") {
      if (m_inboxTab == 0
          && !m_inboxMuted
          && (channelMuted(channel) || (!guild.empty() && m_userDirectory.muted(guild))))
        return;
      if (m_inboxGuild && !m_dmMode && guild != m_guildId)
        return;

      const auto request = std::ranges::find(m_privateChannels, channel, &api::Channel::id);
      if (request != m_privateChannels.end() && request->isMessageRequest())
        return;
      std::string title = channelSummary.empty() ? "Channel" : channelSummary, subtitle = "Direct messages";
      if (guild.empty()) {
        const auto dm = std::ranges::find(m_privateChannels, channel, &api::Channel::id);
        if (dm != m_privateChannels.end())
          title = channelName(*dm);
      } else {
        const auto server = std::ranges::find(m_guilds, guild, &api::Guild::id);
        subtitle = server == m_guilds.end() ? "Server" : server->name;
        const auto* location = serverChannel(guild, channel);
        if (!location)
          location = m_threads.find(guild, channel);
        if (location) {
          title = "#" + location->name;
          if (const auto* parent = serverChannel(guild, location->parentId))
            subtitle += " > " + parent->name;
        }
      }
      rows.push_back(
          {channel, id, guild, std::move(title), std::move(subtitle), std::move(summary), std::move(due), message}
      );
    };
    if (!m_inboxTab) {

      for (const auto& [channel, guild] : m_unreadGuilds)
        if (std::ranges::none_of(
                m_privateChannels, [&](const auto& dm) { return dm.id == channel && dm.isMessageRequest(); }
            )
            && m_unreadTimeline.cursor(channel)
            && (m_inboxMuted || (!channelMuted(channel) && (guild.empty() || !m_userDirectory.muted(guild))))
            && (!m_inboxGuild || m_dmMode || guild == m_guildId))
          m_inboxUnread.emplace_back(channel, guild);
      std::ranges::sort(m_inboxUnread, [&](const auto& a, const auto& b) {
        const auto left = m_unreadTimeline.cursor(a.first)->latest, right = m_unreadTimeline.cursor(b.first)->latest;
        return left.size() != right.size() ? left.size() > right.size() : left > right;
      });
      if (m_inboxUnread.empty())
        m_inboxUnreadPage = 0;
      else
        m_inboxUnreadPage = std::min(m_inboxUnreadPage, (m_inboxUnread.size() - 1) / layout::Inbox::unreadPageSize);
      const auto start = m_inboxUnreadPage * layout::Inbox::unreadPageSize;
      for (auto i = start; i < std::min(m_inboxUnread.size(), start + layout::Inbox::unreadPageSize); ++i) {
        const auto& [channel, guild] = m_inboxUnread[i];
        const auto preview = m_inboxPreviews.find(channel);
        const auto* message = preview == m_inboxPreviews.end() || preview->second.messages.empty()
            ? nullptr
            : &preview->second.messages.back();
        append(
            channel, message ? message->id : "", guild, message,
            preview != m_inboxPreviews.end() && !preview->second.error.empty() ? preview->second.error
                                                                               : "Loading unread preview…"
        );
      }
    } else if (m_inboxTab == 1) {
      for (const auto& message : m_inboxTabs[0].page.messages) {
        const auto known = m_inboxTabs[0].page.guilds.find(message.channelId);
        std::string guild = known == m_inboxTabs[0].page.guilds.end() ? "" : known->second;
        if (guild.empty()) {
          const auto at = m_unreadGuilds.find(message.channelId);
          if (at != m_unreadGuilds.end())
            guild = at->second;
        }

        if (guild.empty())
          for (const auto& server : m_guilds)
            if (serverChannel(server.id, message.channelId) || m_threads.find(server.id, message.channelId)) {
              guild = server.id;
              break;
            }
        append(message.channelId, message.id, guild, &message);
      }
    } else {
      for (const auto& saved : m_inboxTabs[m_inboxTab - 1].page.saved)
        append(
            saved.channel, saved.id, saved.guild, saved.message ? &*saved.message : nullptr,
            saved.summary.empty() ? "This saved message is unavailable." : saved.summary, saved.due,
            saved.channelSummary
        );
    }
    auto list = std::make_unique<Node>();
    list->setPosition(x + Style::spaceSm, top);
    const float inner = width - Style::spaceSm * 2 - Style::scrollbarHoverWidth - Style::scrollbarGap;
    list->setSize(inner, m_inboxViewport);
    list->setClipChildren(true);
    list->setTextScrollOffset(-m_inboxScroll);
    const auto hitsStart = m_hits.size();
    float full = Style::spaceSm;
    const auto measure = [&](std::string_view value, const MessageStyle& style) {
      return m_window.target()
          .renderer()
          .measureText(
              messageMarkup(value, style), style.size, style.bold ? FontWeight::Bold : FontWeight::Normal, 0, 1,
              TextAlign::Start, style.code ? "monospace" : "", TextEllipsize::End, true
          )
          .width;
    };
    for (const auto& row : rows) {
      api::Message preview;
      MessageLayout body;
      if (row.message) {
        preview = *row.message;
        preview.content = bounded(preview.content);
        preview.embeds.clear();
        preview.attachments.clear();
        preview.stickers.clear();
        preview.forwarded.reset();
        for (auto& [id, name] : preview.mentions)
          name = userName(id, name);
        if (m_userDirectory.relationship(preview.authorId) == 2 || m_userDirectory.ignored(preview.authorId))
          preview.content = "Message from a blocked user.";
        body = layoutRichMessage(
            preview, std::max(Style::spaceSection, inner - layout::Inbox::bodyLeft - layout::Inbox::padding), measure,
            Style::fontSizeBody,
            {.unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); },
             .channelMention = [this, &row](
                                   std::string_view guild, std::string_view channel
                               ) { return channelMention(guild.empty() ? row.guild : guild, channel); },
             .roleMention = [this, &row](auto role) { return m_roleDirectory.roleName(row.guild, std::string(role)); }}
        );
      }

      const bool blocked = row.message
          && (m_userDirectory.relationship(row.message->authorId) == 2
              || m_userDirectory.ignored(row.message->authorId));
      const bool hasMedia = row.message
          && !blocked
          && ((m_preferences.showImages && !row.message->attachments.empty())
              || (m_preferences.showEmbeds && !row.message->embeds.empty())
              || !row.message->stickers.empty());
      const float reply = row.message && row.message->reply ? layout::Reply::height : 0;
      const float textHeight = std::min(
          layout::Inbox::previewHeight,
          std::max(Style::spaceSection, row.message ? body.height : Style::controlHeightLg)
      );
      const float mediaHeight = hasMedia ? Style::spaceSection * 2 + Style::spaceSm : 0;
      const float rowHeight = layout::Inbox::cardHeader
          + Style::spaceSm
          + reply
          + textHeight
          + mediaHeight
          + (row.message ? Style::spaceXl : 0)
          + layout::Inbox::padding
          + (row.due.empty() ? 0 : Style::spaceXl);
      const float ry = full - m_inboxScroll;
      full += rowHeight + layout::Inbox::gap;
      if (ry >= m_inboxViewport || ry + rowHeight <= 0)
        continue;
      if (!m_inboxTab)
        m_inboxVisibleUnread.insert(row.channel);
      rectangle(*list, 0, ry, inner, rowHeight, raised, Style::surfaceRadius);
      rectangle(
          *list, Style::borderWidth, ry + Style::borderWidth, inner - Style::spaceXxs, rowHeight - Style::spaceXxs,
          panel, Style::surfaceRadius
      );
      const float controls = Style::controlHeightSm * 3 + Style::spaceMd;
      const auto guild = std::ranges::find(m_guilds, row.guild, &api::Guild::id);
      if (guild != m_guilds.end())
        avatar(*list, Style::spaceMd, ry + Style::spaceMd, Style::space2Xl, guild->icon, guild->name);
      else
        glyph(*list, Style::spaceLg, ry + Style::spaceLg, Style::glyphSizeFeature, "message", muted);
      label(
          *list, layout::Inbox::bodyLeft, ry + Style::spaceDense,
          std::max(Style::spaceSection, inner - layout::Inbox::bodyLeft - controls), row.title, Style::fontSizeBody,
          foreground, FontWeight::SemiBold
      );
      label(
          *list, layout::Inbox::bodyLeft, ry + Style::spaceWide, inner - layout::Inbox::bodyLeft - controls,
          row.subtitle, Style::fontSizeDescription, muted
      );
      rectangle(*list, 0, ry + layout::Inbox::cardHeader, inner, Style::borderWidth, raised);
      const auto target = row.channel + ":" + row.id;
      const auto jump = "jump:" + row.guild + ":" + target;
      m_hits.push_back({x + Style::spaceSm, top + ry, inner, rowHeight, Action::InboxAction, jump});
      const auto rowIcon = [&](int slot, const std::string& glyphName, const std::string& title,
                               const std::string& command) {
        iconButton(
            *list,
            {inner - Style::spaceSm - Style::controlHeightSm * slot, ry + Style::spaceMd, Style::controlHeightSm,
             Style::controlHeightSm, Action::InboxAction, command},
            glyphName, m_pointerY >= top && m_pointerY < top + m_inboxViewport ? title : ""
        );
      };
      rowIcon(
          1, m_inboxTab == 2 ? "x" : "check",
          !m_inboxTab           ? "Mark channel as read"
              : m_inboxTab == 1 ? "Dismiss mention"
              : m_inboxTab == 3 ? "Complete reminder"
                                : "Remove saved message",
          !m_inboxTab           ? "read:" + row.channel
              : m_inboxTab == 1 ? "dismiss:" + row.id
                                : "remove:" + target
      );

      if (row.id.empty() && !m_inboxTab)
        rowIcon(2, "refresh", "Retry preview", "retry-preview:" + row.channel);
      if (!row.id.empty()) {
        rowIcon(2, "bell", "Remind me", "remind:" + target);
        rowIcon(3, "bookmark", row.due.empty() ? "Bookmark message" : "Keep as bookmark", "save:" + target);
      }
      float by = ry + layout::Inbox::cardHeader + Style::spaceSm;
      if (!row.due.empty()) {
        const auto due = api::inboxTime(row.due);
        label(
            *list, layout::Inbox::padding, by, inner - Style::spaceLg * 2,
            std::string(due && *due <= std::chrono::system_clock::now() ? "Due: " : "Remind: ") + readableTime(row.due),
            Style::fontSizeDescription, accent
        );
        by += Style::spaceXl;
      }
      if (row.message) {
        if (row.message->reply) {
          const auto& replyMessage = *row.message->reply;
          label(
              *list, layout::Inbox::bodyLeft, by, inner - layout::Inbox::bodyLeft - Style::spaceMd,
              "↳ " + replyMessage.author + ": " + bounded(notificationMessagePreview(replyMessage.content)),
              Style::fontSizeDescription, muted
          );
          by += reply;
        }
        const auto author = userName(row.message->authorId, row.message->author);
        avatar(*list, Style::spaceMd, by, layout::Inbox::avatar, row.message->avatar, author, row.message->authorId);
        label(
            *list, layout::Inbox::bodyLeft, by, inner - layout::Inbox::bodyLeft - Style::spaceSection * 3, author,
            Style::fontSizeBody, foreground, FontWeight::SemiBold
        );
        label(
            *list, inner - Style::spaceSection * 3, by + Style::spaceXxs, Style::spaceSection * 3 - Style::spaceSm,
            readableTime(row.message->timestamp), Style::fontSizeDetail, muted
        );
        by += Style::spaceXl;
        const float textX = layout::Inbox::bodyLeft;
        for (const auto& box : body.boxes)
          if (box.y + box.height <= textHeight)
            rectangle(*list, textX + box.x, by + box.y, box.width, box.height, raised, Style::radiusSm);
        for (const auto& run : body.runs) {
          if (run.y + run.height > textHeight || run.style.spoiler)
            continue;
          if (!run.image.empty())
            emojiImage(*list, textX + run.x, by + run.y, run.width, run.image);
          else
            label(
                *list, textX + run.x, by + run.y, run.width + Style::spaceXxs,
                messageMarkup(run.text, run.style, false), run.style.size, run.style.link.empty() ? foreground : accent,
                FontWeight::Normal, 1, true
            );
        }
        for (const auto& area : messageSpoilerAreas(body))
          if (area.y + area.height <= textHeight)
            rectangle(*list, textX + area.x, by + area.y, area.width, area.height, raised, Style::radiusSm);
        drawMessageStrikes(*list, body, textX, by, foreground, false, {}, textHeight);
        if (hasMedia) {
          std::string path;

          const auto media = std::ranges::find_if(row.message->attachments, [&](const auto& media) {
            return m_preferences.showImages && !media.image.url.empty() && !media.filename.starts_with("SPOILER_");
          });
          if (media != row.message->attachments.end())
            path = media->image.url;
          if (path.empty() && m_preferences.showEmbeds && !row.message->embeds.empty())
            path = row.message->embeds.front().image.url.empty() ? row.message->embeds.front().thumbnail.url
                                                                 : row.message->embeds.front().image.url;
          if (!path.empty())
            picture(
                *list, textX, by + textHeight + Style::spaceXs,
                std::min(Style::spaceSection * 5, inner - textX - Style::spaceMd), mediaHeight - Style::spaceSm, path,
                Style::radiusSm, true
            );
          else
            label(
                *list, textX, by + textHeight + Style::spaceSm, inner - textX - Style::spaceMd,
                "Attachment · Open message to view", Style::fontSizeDescription, muted
            );
        }
      } else
        label(
            *list, layout::Inbox::padding, by + Style::spaceSm, inner - Style::spaceLg * 2,
            notificationMessagePreview(row.summary), Style::fontSizeBody, muted, FontWeight::Normal, 2
        );
    }

    if (rows.empty()) {
      const auto* tab = m_inboxTab ? &m_inboxTabs[m_inboxTab - 1] : nullptr;
      const bool loading = tab && tab->pending.has_value();
      const bool failed = tab && !loading && !tab->error.empty();
      const bool filtered = tab && (!tab->page.messages.empty() || !tab->page.saved.empty());
      const bool savedEmpty = m_inboxTab >= 2 && !loading && !failed && !filtered;
      std::string heading = "You're all caught up.";
      std::string detail = "New unread conversations will appear here.";
      std::string illustration, emptyGlyph = "checks";
      if (loading) {
        heading = "Loading "
            + std::string(
                      m_inboxTab == 1       ? "mentions"
                          : m_inboxTab == 2 ? "bookmarks"
                                            : "reminders"
            )
            + "…";
        detail = "Your inbox will be ready in a moment.";
        emptyGlyph = "hourglass";
      } else if (failed) {
        heading = "Couldn't load "
            + std::string(
                      m_inboxTab == 1       ? "mentions"
                          : m_inboxTab == 2 ? "bookmarks"
                                            : "reminders"
            );
        detail = tab->error;
        emptyGlyph = "cloud-off";
      } else if (filtered) {
        heading = "No matches here";
        detail = "Try changing your inbox filters to see more messages.";
        emptyGlyph = "adjustments";
      } else if (m_inboxTab == 1) {
        heading = "All quiet for now";
        detail = "Messages that mention you will appear here.";
        emptyGlyph = "at";
      } else if (m_inboxTab == 2) {
        heading = "Keep your favourite moments";
        detail = "Open a message menu and choose Bookmark Message. Find it here whenever you need it.";
        illustration = "inbox-bookmarks";
      } else if (m_inboxTab == 3) {
        heading = "Come back at the right time";
        detail = "Choose Remind Me from a message menu and pick a time. We'll give you a nudge when it's due.";
        illustration = "inbox-reminders";
      }
      const float textWidth = std::min(layout::Inbox::emptyTextWidth, inner - layout::Inbox::padding * 2);
      const auto measure = [&](const std::string& text, float size, FontWeight weight) {
        const auto metrics = m_window.target().renderer().measureText(text, size, weight, textWidth, 0);
        return std::max(size + Style::spaceXs, metrics.bottom - metrics.top);
      };
      const auto centered = [&](const std::string& text, float top, float size, FontWeight weight, Color color) {
        auto caption = std::make_unique<TextNode>();
        const auto metrics =
            m_window.target().renderer().measureText(text, size, weight, textWidth, 0, TextAlign::Center);
        const auto height = std::max(size + Style::spaceXs, metrics.bottom - metrics.top);
        caption->setText(text);
        caption->setFontSize(size);
        caption->setFontWeight(weight);
        caption->setColor(color);
        caption->setMaxWidth(textWidth);
        caption->setMaxLines(0);
        caption->setTextAlign(TextAlign::Center);
        caption->setPosition((inner - textWidth) / 2, top - metrics.top);
        caption->setSize(textWidth, height);
        list->addChild(std::move(caption));
        return height;
      };
      const float headingHeight = measure(heading, Style::fontSizeHeading, FontWeight::SemiBold);
      const float detailHeight = measure(detail, Style::fontSizeBody, FontWeight::Normal);
      const float artHeight = illustration.empty() ? Style::spaceSection * 2 : layout::Inbox::emptyArtHeight;
      const float blockHeight = artHeight
          + layout::Inbox::emptyGap
          + headingHeight
          + Style::spaceMd
          + detailHeight
          + (savedEmpty ? layout::Inbox::emptyGap + layout::Inbox::emptyGuideHeight : 0);
      const float blockTop = std::max(layout::Inbox::padding, (m_inboxViewport - blockHeight) / 2 - Style::spaceLg);
      m_scrolling.limit(
          m_inboxScroll, std::max(0.0F, blockTop + blockHeight + layout::Inbox::padding - m_inboxViewport)
      );
      list->setTextScrollOffset(-m_inboxScroll);
      float emptyY = blockTop - m_inboxScroll;
      bool pictured = false;
      if (!illustration.empty()) {
        const float artWidth = std::min(layout::Inbox::emptyArtWidth, inner - layout::Inbox::padding * 2);
        const int pixels = std::max(1, static_cast<int>(std::ceil(artWidth * m_window.target().contentScale())));
        const auto path = "builtin:" + illustration + ":" + std::to_string(pixels);
        auto& artwork = m_images[path];
        artwork.uses |= imageUseMask(ImageUse::Content);
        artwork.touched = ++m_imageSerial;
        artwork.visibleDraw = m_animationDraw;
        if (!artwork.texture.valid())
          artwork.texture = m_window.context().textureManager().loadFromFile(
              paths::assetPath("illustrations/" + illustration + ".svg").string(), pixels
          );
        pictured = picture(
            *list, (inner - artWidth) / 2, emptyY, artWidth, artHeight, path, 0, false, false, ImageFitMode::Contain
        );
      }
      if (!pictured)
        glyph(
            *list, (inner - Style::glyphSizeDisplay) / 2, emptyY + (artHeight - Style::glyphSizeDisplay) / 2,
            Style::glyphSizeDisplay, emptyGlyph, failed ? danger : accent
        );
      emptyY += artHeight + layout::Inbox::emptyGap;
      emptyY += centered(heading, emptyY, Style::fontSizeHeading, FontWeight::SemiBold, foreground);
      emptyY += Style::spaceMd;
      emptyY += centered(detail, emptyY, Style::fontSizeBody, FontWeight::Normal, failed ? danger : muted);
      if (savedEmpty) {
        emptyY += layout::Inbox::emptyGap;
        const float guideWidth = std::min(layout::Inbox::emptyGuideWidth, textWidth);
        const float guideX = (inner - guideWidth) / 2;
        rectangle(*list, guideX, emptyY, guideWidth, layout::Inbox::emptyGuideHeight, panel, Style::surfaceRadius);
        glyph(
            *list, guideX + Style::spaceLg, emptyY + (layout::Inbox::emptyGuideHeight - Style::glyphSizeFeature) / 2,
            Style::glyphSizeFeature, m_inboxTab == 2 ? "bookmark" : "bell", accent
        );
        label(
            *list, guideX + Style::spaceGroup, emptyY + Style::spaceLg, guideWidth - Style::spaceGroup - Style::spaceMd,
            m_inboxTab == 2 ? "Bookmark Message" : "Remind Me", Style::fontSizeBody, foreground, FontWeight::SemiBold
        );
        label(
            *list, guideX + Style::spaceGroup, emptyY + Style::spaceSection,
            guideWidth - Style::spaceGroup - Style::spaceMd,
            m_inboxTab == 2 ? "Available in any message's menu" : "Choose a time that works for you",
            Style::fontSizeDescription, muted, FontWeight::Normal, 2
        );
        emptyY += layout::Inbox::emptyGuideHeight;
      }
      full = emptyY + m_inboxScroll + layout::Inbox::padding;
    }
    for (auto i = hitsStart; i < m_hits.size(); ++i) {
      auto& hit = m_hits[i];
      const auto bottom = std::min(top + m_inboxViewport, hit.y + hit.height);
      hit.y = std::max(top, hit.y);
      hit.height = std::max(0.0F, bottom - hit.y);
    }
    m_scrolling.limit(m_inboxScroll, std::max(0.0F, full - m_inboxViewport));
    root.addChild(std::move(list));
    if (full > m_inboxViewport)
      settingsScrollbar(
          root, "inbox-scrollbar",
          {x + width - Style::scrollbarHoverWidth - Style::spaceSm, top, Style::scrollbarHoverWidth, m_inboxViewport},
          full, m_inboxScroll
      );
    const float footerY = y + height - layout::Inbox::footer;
    if (!m_inboxStatus.empty())
      label(
          root, x + Style::spaceMd, footerY + Style::spaceMd, width - Style::spaceSection * 5, m_inboxStatus,
          Style::fontSizeDescription, muted
      );
    else if (m_inboxTab && !rows.empty() && !m_inboxTabs[m_inboxTab - 1].error.empty())
      label(
          root, x + Style::spaceMd, footerY, width - Style::spaceSection * 4, m_inboxTabs[m_inboxTab - 1].error,
          Style::fontSizeDescription, danger, FontWeight::Normal, 2
      );
    const auto footerButton = [&](float right, const std::string& title, const std::string& command) {
      button(
          root,
          {x + width - right, footerY + Style::spaceXs, Style::spaceSection * 2, Style::controlHeightSm,
           Action::InboxAction, command},
          title, false, false, false, true
      );
    };
    if (m_inboxTab) {
      const auto& tab = m_inboxTabs[m_inboxTab - 1];
      if (!tab.pending) {
        if (!tab.error.empty())
          footerButton(Style::spaceSection * 2 + Style::spaceSm, "Retry", "refresh");
        else if (tab.page.more)
          footerButton(Style::spaceSection * 2 + Style::spaceSm, "Older", "older");
        if (!tab.page.before.empty())
          footerButton(Style::spaceSection * 4 + Style::spaceMd, "Newest", "newest");
      }
    } else {
      if ((m_inboxUnreadPage + 1) * layout::Inbox::unreadPageSize < m_inboxUnread.size())
        footerButton(Style::spaceSection * 2 + Style::spaceSm, "Next", "next-unread");
      if (m_inboxUnreadPage)
        footerButton(Style::spaceSection * 4 + Style::spaceMd, "Previous", "previous-unread");
    }
    updateInboxPreviews();
  }
} // namespace app

namespace app {
  void ClientApplication::demoInboxStep() {
    const auto require = [](bool ok, const char* message) {
      if (!ok)
        throw std::runtime_error(message);
    };
    const auto action = [&](std::string value) {
      inboxAction({.action = Action::InboxAction, .id = std::move(value)});
    };
    const auto click = [&](const std::string& id) {
      const auto hit = std::ranges::find_if(m_hits, [&](const auto& value) {
        return value.action == Action::InboxAction && value.id == id && value.height > 0;
      });
      require(hit != m_hits.end(), "The requested inbox action has a visible native hit region");
      const auto target = *hit;
      m_window.click(
          target.x + target.width / 2,
          target.y + target.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272
      );
    };
    const auto message = [&] {
      auto result = m_messages.front();
      result.id = "90001";
      result.channelId = m_channelId;
      result.author = "Maya";
      result.authorId = "11";
      result.content = "An **important** message with a [link](https://example.com) and ||a hidden spoiler||.";
      result.timestamp = "2026-10-21T10:00:00Z";
      result.attachments = {{"1", "preview.png", "demo-preview", "image/png", {"demo-preview", 640, 360}}};
      return result;
    };
    const auto saved = [&] {
      api::SavedMessage result;
      result.channel = m_channelId;
      result.id = "90001";
      result.guild = m_guildId;
      result.savedAt = "2026-10-21T10:00:00Z";
      result.message = message();
      return result;
    };
    const auto response = [&](int index, api::InboxPage page, std::string error = "") {
      require(m_inboxTabs[index].pending.has_value(), "A tab owns its in-flight request");
      api::Event event{.operation = api::Operation::InboxRead, .session = m_session};
      event.inboxQuery = m_inboxTabs[index].pending;
      event.inboxPage = std::move(page);
      event.error = std::move(error);
      inboxEvent(event);
    };
    const auto write = [&](std::string error = "", std::optional<api::SavedMessage> record = {}) {
      require(m_inboxDemoWrite.has_value(), "An explicit action queues one scoped account mutation");
      api::Event event{.operation = api::Operation::InboxWrite, .session = m_session, .error = std::move(error)};
      event.inboxQuery = m_inboxDemoWrite;
      event.inboxPage = api::InboxPage{};
      if (record)
        event.inboxPage->saved.push_back(std::move(*record));
      inboxEvent(event);
    };
    const auto sceneContains = [&](std::string_view text) {
      const auto inspect = [&](auto&& self, const Node& node) -> bool {
        if (node.type() == NodeType::Text && static_cast<const TextNode&>(node).text().contains(text))
          return true;
        return std::ranges::any_of(node.children(), [&](const auto& child) { return self(self, *child); });
      };
      return inspect(inspect, *m_scene);
    };
    const auto rolePage = [&] {
      auto item = message();
      item.channelId = "99003";
      item.content = "A role mention: <@&99001>";
      return api::InboxPage{.messages = {item}, .guilds = {{"99003", "99000"}}};
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      m_drafts[m_channelId] = "Keep this draft";
      m_unreadGuilds[m_channelId] = m_guildId;
      m_unreadTimeline.snapshot({{m_channelId, {"90000", "90004", 2}}});
      toggleInbox();
      require(
          m_inboxOpen && m_unreadTimeline.cursor(m_channelId).has_value(),
          "Opening the inbox leaves unread cursors and the current draft untouched"
      );
    } else if (step == 1) {
      require(!m_inboxPreviews.empty(), "Only visible unread cards request bounded previews");
      for (const auto& [channel, preview] : m_inboxPreviews)
        if (preview.pending) {
          api::Event event{.operation = api::Operation::InboxRead};
          event.inboxQuery = preview.pending;
          event.inboxPage = api::InboxPage{.messages = {message()}};
          inboxEvent(event);
        }
      action("tab:1");
      response(0, {.messages = {message()}, .guilds = {{m_channelId, m_guildId}}});
    } else if (step == 2) {
      require(
          m_inboxTabs[0].page.messages.size() == 1, "Mentions display account results independently of chat history"
      );
      click("dismiss:90001");
      write("Synthetic dismissal failure");
      require(
          m_inboxTabs[0].page.messages.size() == 1 && !m_inboxStatus.empty(),
          "An unconfirmed dismissal keeps its mention and explains the failure"
      );
    } else if (step == 3) {
      require(m_tooltip == "Dismiss mention", "Inbox action tooltips describe their own actions over covered chat");
      click("dismiss:90001");
      write();
      require(m_inboxTabs[0].page.messages.empty(), "Acknowledged dismissal removes only the selected mention");
      response(0, {});
      m_contextChannel = m_channelId;
      m_contextMessage = m_messages.front().id;
      contextAction(Action::BookmarkMessage);
      require(
          m_inboxDemoWrite && m_inboxDemoWrite->message == m_messages.front().id,
          "The real message context action queues a bookmark for the selected message"
      );
      auto record = saved();
      record.id = m_messages.front().id;
      record.message = m_messages.front();
      write("", record);
      action("tab:2");
      response(1, {.saved = {saved()}});
    } else if (step == 4) {
      require(m_inboxTabs[1].page.saved.size() == 1, "Bookmarks render their saved content and original location");
      click("remind:" + m_channelId + ":90001");
      require(!m_inboxRemindTarget.empty(), "A reminder opens its duration choices without a write");
    } else if (step == 5) {
      click("due:60:" + m_channelId + ":90001");
      require(
          m_inboxDemoWrite && api::inboxTime(m_inboxDemoWrite->due).has_value(),
          "Reminder choices queue a valid future timestamp"
      );
      write("Synthetic reminder failure");
      require(m_inboxTabs[1].page.saved.size() == 1, "Failed reminder conversion preserves the bookmark");
    } else if (step == 6) {
      action("due:60:" + m_channelId + ":90001");
      auto record = saved();

      record.due = m_inboxDemoWrite->due;
      const auto submitted = *m_inboxDemoWrite;
      api::Event live{.operation = api::Operation::InboxUpdate};
      live.inboxQuery = submitted;
      live.inboxPage = api::InboxPage{.saved = {record}};
      inboxEvent(live);
      api::Event late{.operation = api::Operation::InboxWrite, .error = "Late synthetic transport failure"};
      late.inboxQuery = submitted;
      inboxEvent(late);
      require(
          m_inboxWrites.empty() && m_inboxTabs[2].page.saved.size() == 1 && m_inboxStatus.empty(),
          "A live saved-message confirmation wins over a later HTTP failure"
      );
      action("tab:3");
      response(2, {.saved = {record}});
      require(
          m_inboxTabs[1].page.saved.empty() && m_inboxTabs[2].page.saved.size() == 1,
          "A confirmed reminder moves the message to the correct tab"
      );
    } else if (step == 7) {
      click("remove:" + m_channelId + ":90001");
      write("Synthetic completion failure");
      require(m_inboxTabs[2].page.saved.size() == 1, "Failed completion keeps the reminder visible");
    } else if (step == 8) {
      click("remove:" + m_channelId + ":90001");
      write();
      require(m_inboxTabs[2].page.saved.empty(), "Successful completion removes the reminder");
      response(2, {});
      action("tab:1");
      action("refresh");
      const auto old = m_inboxTabs[0].pending;
      api::Event live{.operation = api::Operation::InboxUpdate};
      live.inboxQuery = api::InboxQuery{.kind = api::InboxKind::Dismiss, .message = "90001"};
      inboxEvent(live);
      api::Event stale{.operation = api::Operation::InboxRead};
      stale.inboxQuery = old;
      stale.inboxPage = api::InboxPage{.messages = {message()}};
      inboxEvent(stale);
      require(
          m_inboxTabs[0].page.messages.empty() && m_inboxTabs[0].pending,
          "A delayed list read cannot resurrect a mention dismissed by a live event"
      );
      api::InboxPage page;
      page.guilds[m_channelId] = m_guildId;
      for (unsigned i = 0; i < api::inboxPageSize; ++i) {
        auto item = message();
        item.id = std::to_string(90001 + i);
        page.messages.push_back(std::move(item));
      }
      page.more = true;
      page.before = "90001";
      response(0, std::move(page));
    } else if (step == 9) {
      require(
          m_inboxBounds.width > 0
              && m_inboxBounds.x >= 0
              && m_inboxBounds.x + m_inboxBounds.width <= m_window.width()
              && m_inboxBounds.y + m_inboxBounds.height <= m_window.height() + layout::Window::titlebarHeight,
          "The inbox stays inside wide and narrow windows"
      );
      for (const auto& hit : m_hits)
        if (hit.action == Action::InboxAction && hit.id.starts_with("jump:") && hit.height > 0)
          require(
              hit.y >= m_inboxBounds.y && hit.y + hit.height <= m_inboxBounds.y + m_inboxBounds.height,
              "Message card hit regions clip with their scrolling content"
          );
      const auto chat = m_messageScroll;
      m_pointerX = m_inboxBounds.x + Style::spaceSection;
      m_pointerY = m_inboxBounds.y + Style::spaceSection * 4;
      m_window.scroll(Style::spaceSection);
      require(
          m_scrolling.active(m_inboxScroll) && m_messageScroll == chat,
          "Inbox wheel input belongs to its own scroll owner"
      );
      m_window.contextClick(m_pointerX, m_pointerY + m_window.titlebarHeight() - layout::Window::titlebarHeight);
      require(!m_contextOpen, "Right clicking the inbox cannot reach a covered message menu");
      m_window.click(
          m_inboxBounds.x + m_inboxBounds.width - Style::spaceXs,
          m_inboxBounds.y + m_inboxBounds.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272
      );
      require(
          !m_controlInput.pointerCaptured() && !m_controlInput.focusedArea(),
          "Clicks in the inbox scrollbar gutter cannot capture a covered native control"
      );
      onKey({XKB_KEY_a, "a"});
      require(m_drafts[m_channelId] == "Keep this draft", "Typing with the inbox open cannot edit the covered draft");
    } else if (step == 10) {
      click("older");
      require(
          m_inboxTabs[0].pending && m_inboxTabs[0].pending->before == "90001", "Older mentions use the retained cursor"
      );
      response(0, {}, "Synthetic page failure");
      require(
          m_inboxTabs[0].page.messages.size() == api::inboxPageSize,
          "Pagination errors preserve the previously visible page"
      );
      action("refresh");
      response(0, {.messages = {message()}, .guilds = {{m_channelId, m_guildId}}});
    } else if (step == 11) {
      action("tab:0");
      action("read:" + m_channelId);
      require(!m_unreadTimeline.cursor(m_channelId), "The explicit check action marks the selected channel read");
      onKey({XKB_KEY_Escape});
      require(
          !m_inboxOpen && m_drafts[m_channelId] == "Keep this draft",
          "Escape dismisses the inbox without losing the draft"
      );
      toggleInbox();
      action("tab:1");
    } else if (step == 12) {
      const auto channel = m_channelId;
      action("jump:" + m_guildId + ":" + m_channelId + ":" + m_messages.front().id);
      require(
          !m_inboxOpen && m_channelId == channel && m_seekMessage == m_messages.front().id,
          "Jumping to a loaded message focuses its existing conversation"
      );
      toggleInbox();
    } else if (step == 13) {
      action("tab:2");
      action("refresh");
      response(1, {});
    } else if (step == 14) {
      require(
          m_inboxTabs[1].loaded && m_inboxTabs[1].error.empty(),
          "An empty bookmark collection is a successful loaded page"
      );
      require(
          std::ranges::any_of(
              m_images,
              [](const auto& image) {
                return image.first.starts_with("builtin:inbox-bookmarks:") && image.second.texture.valid();
              }
          ),
          "The bookmark empty state uses the bundled illustration without a network request"
      );
      if (m_demoOverlay.ends_with("narrow")) {
        m_pointerX = m_inboxBounds.x + Style::spaceSection;
        m_pointerY = m_inboxBounds.y + m_inboxBounds.height / 2;
        m_window.scroll(Style::spaceSection);
        require(m_scrolling.active(m_inboxScroll), "Small windows can scroll the empty-state guide");
      }
      action("tab:3");
      action("refresh");
      response(2, {});
    } else if (step == 15) {
      require(
          std::ranges::any_of(
              m_images,
              [](const auto& image) {
                return image.first.starts_with("builtin:inbox-reminders:") && image.second.texture.valid();
              }
          ),
          "The reminder empty state uses its own bundled illustration"
      );
      action("refresh");
      response(2, {}, "Synthetic read failure (HTTP 404, code 0).");
    } else if (step == 16) {
      int errors = 0;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text
            && static_cast<const TextNode&>(node).text() == "Synthetic read failure (HTTP 404, code 0).")
          ++errors;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(errors == 1, "An unknown 404 has one error message and is never presented as an empty collection");
      click("refresh");
      require(m_inboxTabs[2].pending && m_inboxTabs[2].error.empty(), "Retry clears the stale error while loading");
    } else if (step == 17) {
      require(
          m_inboxTabs[2].pending && !m_inboxTabs[2].loaded, "A loading screen does not claim that the list is empty"
      );
      response(2, {});
    } else if (step == 18) {
      require(m_inboxTabs[2].loaded && m_inboxTabs[2].error.empty(), "Retry recovers to the illustrated empty state");
      m_roleDirectory.apply(
          api::parseRoleUpdates(
              "READY",
              nlohmann::json{
                  {"guilds",
                   nlohmann::json::array(
                       {{{"id", m_guildId},
                         {"roles", nlohmann::json::array({{{"id", "99002"}, {"name", "Chatwatch"}}})}},
                        {{"id", "99000"},
                         {"roles", nlohmann::json::array({{{"id", "99001"}, {"name", "Inboxwatch"}}})}}}
                   )}
              }
          )
      );
      api::GuildRoleUpdate membership;
      membership.guild = m_guildId;
      membership.members[m_account.id] = {"99002"};
      m_roleDirectory.apply({false, {membership}});
      m_messages.back().content = "Chat role <@&99002>";
      m_messages.back().type = api::defaultMessageType;
      m_messageLayouts.clear();
      m_userDirectory.apply({.mutedChannels = {{"99003", true}}});
      action("tab:1");
      action("refresh");
      response(0, rolePage());
    } else if (step == 19) {
      require(
          !m_inboxMuted
              && std::ranges::any_of(
                  m_hits,
                  [](const auto& hit) {
                    return hit.action == Action::InboxAction && hit.id == "dismiss:90001" && hit.height > 0;
                  }
              ),
          "Mentions in muted conversations remain available without the unread mute filter"
      );
      require(sceneContains("@Inboxwatch"), "An inbox role uses its own server instead of the selected chat");
      require(
          std::ranges::any_of(
              m_messageLayouts, [](const auto& cached) { return cached.second.layout.plainText.contains("@Chatwatch"); }
          ),
          "Normal chat resolves roles through the shared message layout"
      );
      m_roleDirectory.apply(
          api::parseRoleUpdates(
              "GUILD_ROLE_UPDATE",
              nlohmann::json{{"guild_id", "99000"}, {"role", {{"id", "99001"}, {"name", "Moonwatch"}}}}
          )
      );
    } else if (step == 20) {
      require(sceneContains("@Moonwatch") && !sceneContains("@Inboxwatch"), "Role renames refresh visible inbox cards");
      action("filters");
    } else if (step == 21) {
      const auto filterVisible = [&](std::string_view id) {
        return std::ranges::any_of(m_hits, [&](const auto& hit) {
          return hit.action == Action::InboxAction && hit.id == id && hit.height > 0;
        });
      };
      require(
          filterVisible("roles") && filterVisible("everyone") && !filterVisible("muted"),
          "Mentions exposes only applicable filters"
      );
      action("everyone");
      response(0, rolePage());
      auto item = message();
      item.mentions.clear();
      item.mentionRoles = {"99002"};
      item.mentionEveryone = true;
      api::Event live{.operation = api::Operation::MessageCreate, .resource = m_channelId, .messages = {item}};
      inboxEvent(live);
      require(
          m_inboxTabs[0].pending.has_value(),
          "A role mention refreshes the inbox even when its everyone mention is filtered out"
      );
      response(0, rolePage());
      action("roles");
      response(0, rolePage());
      inboxEvent(live);
      require(!m_inboxTabs[0].pending, "Excluded role and everyone mentions cannot trigger a refresh");
      live.messages.front().mentions[m_account.id] = "Me";
      live.messages.front().deleted = true;
      inboxEvent(live);
      require(!m_inboxTabs[0].pending, "Deleted mentions do not refresh account results");
      live.messages.front().deleted = false;
      inboxEvent(live);
      require(m_inboxTabs[0].pending.has_value(), "Direct mentions refresh independently of role and everyone filters");
      response(0, rolePage());
      m_roleDirectory.apply(
          api::parseRoleUpdates("GUILD_ROLE_DELETE", nlohmann::json{{"guild_id", "99000"}, {"role_id", "99001"}})
      );
      action("filters");
    } else if (step == 22) {
      require(sceneContains("@unknown-role") && !sceneContains("@Moonwatch"), "Deleted roles lose stale names");
    } else {
      clearInbox();
      require(
          !m_inboxOpen
              && m_inboxPreviews.empty()
              && m_inboxWrites.empty()
              && m_inboxTabs[0].page.messages.empty()
              && !m_inboxReminderTimer.active(),
          "Account reset releases inbox pages, pending actions, previews and timers"
      );
    }
    std::clog << "Offline inbox replay step " << step + 1 << " passed\n";
    m_window.invalidate();
  }
} // namespace app
