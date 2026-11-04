#include "app/client_application.h"
#include "app/message_mentions.h"
#include "app/message_preferences.h"
#include "app/notification_preview.h"

#include <algorithm>

namespace app {
  void ClientApplication::notifyMessage(const api::Message& message, const std::string& channel) {
    if (m_demo
        || !m_connected
        || !m_live
        || !m_preferences.desktopNotifications
        || m_presence.status == "dnd"
        || message.authorId.empty()
        || message.authorId == m_account.id
        || message.deleted
        || (message.flags & 64)
        || m_userDirectory.relationship(message.authorId) == 2
        || m_userDirectory.ignored(message.authorId)
        || readingChannel(channel)
        || channelMuted(channel))
      return;
    if (std::ranges::find(m_notifiedMessages, message.id) != m_notifiedMessages.end())
      return;
    m_notifiedMessages.push_back(message.id);
    if (m_notifiedMessages.size() > 512)
      m_notifiedMessages.erase(m_notifiedMessages.begin());
    std::string guild;
    if (const auto found = m_unreadGuilds.find(channel); found != m_unreadGuilds.end())
      guild = found->second;
    if (guild.empty())
      for (const auto& [id, channels] : m_guildChannels)
        if (std::ranges::find(channels, channel, &api::Channel::id) != channels.end()) {
          guild = id;
          break;
        }
    if (guild.empty() && !m_dmMode && serverChannel(m_guildId, channel))
      guild = m_guildId;
    if (guild.empty())
      for (const auto& server : m_guilds)
        if (m_threads.find(server.id, channel)) {
          guild = server.id;
          break;
        }
    if (!guild.empty() && m_userDirectory.muted(guild))
      return;
    const auto dm = std::ranges::find(m_privateChannels, channel, &api::Channel::id);
    if (guild.empty() && dm == m_privateChannels.end())
      return;
    const auto roles =
        guild.empty() ? std::optional<std::vector<std::string>>{} : m_roleDirectory.memberships(guild, m_account.id);
    const auto preferences = m_userDirectory.guildPreferences(guild);
    const bool mentioned = message.mentions.contains(m_account.id)
        || (message.mentionEveryone && !preferences.suppressEveryone.value_or(false))
        || (roles
            && !preferences.suppressRoles.value_or(false)
            && std::ranges::any_of(message.mentionRoles, [&](const auto& role) {
                 return std::ranges::find(*roles, role) != roles->end();
               }));
    int level = m_userDirectory.notificationLevel(channel);
    if (level == 3 && !guild.empty())
      level = m_userDirectory.notificationLevel(guild);
    if (level == 2 || (!guild.empty() && level != 0 && !mentioned))
      return;
    DesktopNotice notice;
    notice.account = m_account.id;
    notice.guild = guild;
    notice.channel = channel;
    notice.message = message.id;
    notice.generation = m_session;
    notice.sound = m_preferences.notificationSounds;
    notice.title = "Cressida";
    notice.body = "You have a new message.";
    if (m_preferences.notificationPrivacy < 2) {
      notice.avatar = message.avatar;
      if (!notice.avatar.empty()) {
        auto& cached = m_images[notice.avatar];
        cached.uses |= imageUseMask(ImageUse::Content);
        notice.avatarBytes = cached.bytes;
      }
      std::string conversation, serverName;
      if (!guild.empty()) {
        const auto server = std::ranges::find(m_guilds, guild, &api::Guild::id);
        if (server != m_guilds.end())
          serverName = server->name;
        const auto* location = serverChannel(guild, channel);
        if (!location)
          location = m_threads.find(guild, channel);
        if (location && !location->name.empty())
          conversation = "#" + location->name;
      } else if (dm != m_privateChannels.end() && dm->kind() == api::ChannelType::GroupDirectMessage)
        conversation = channelName(*dm);
      notice.title = desktopMessageTitle(userName(message.authorId, message.author, guild), conversation, serverName);
      if (!m_preferences.notificationPrivacy) {
        const auto guildRoles = m_roleDirectory.allRoles(guild);
        notice.body = desktopMessagePreview(message, [&](char kind, const std::string& id) -> std::string {
          if (kind == '&') {
            const auto role = std::ranges::find(guildRoles, id, &api::ColoredRole::id);
            return role == guildRoles.end() ? "" : role->name;
          }
          if (kind == '#') {
            if (const auto channels = m_guildChannels.find(guild); channels != m_guildChannels.end()) {
              const auto entry = std::ranges::find(channels->second, id, &api::Channel::id);
              if (entry != channels->second.end())
                return entry->name;
            }
            return "";
          }
          const auto mentioned = message.mentions.find(id);
          if (mentioned != message.mentions.end())
            return userName(id, mentioned->second, guild);
          if (id == message.authorId)
            return userName(id, message.author, guild);
          const auto user = lookupUser(id);
          return user.name == "Unknown user" ? "" : userName(id, user.name, guild);
        });
      }
    }
    const auto avatar = notice.avatar;
    const bool needsAvatar = !avatar.empty() && notice.avatarBytes.empty();
    m_desktopNotifications.show(std::move(notice));
    if (needsAvatar && m_assets.request(avatar))
      m_pendingImages[avatar] |= imageUseMask(ImageUse::Content);
  }
  void ClientApplication::drainDesktopNotifications() {
    while (auto action = m_desktopNotifications.poll())
      desktopNoticeAction(std::move(*action));
  }
  void ClientApplication::desktopNoticeAction(DesktopNoticeAction action) {
    if (!m_connected
        || !m_preferences.desktopNotifications
        || action.generation != m_session
        || action.account != m_account.id)
      return;
    if (action.guild.empty()) {
      if (std::ranges::find(m_privateChannels, action.channel, &api::Channel::id) == m_privateChannels.end())
        return;
    } else if (std::ranges::find(m_guilds, action.guild, &api::Guild::id) == m_guilds.end())
      return;
    m_trayStartupTimer.stop();
    m_window.show(std::move(action.activationToken));
    // Bring an existing settings/dialog window forward without discarding its
    // edits. A notification for the current conversation also needs only focus.
    if (settingsVisible() || !m_dialogKind.empty() || userAccountDialogOpen())
      return;
    if (m_channelId != action.channel
        || (action.guild.empty() != m_dmMode)
        || (!action.guild.empty() && action.guild != m_guildId)) {
      if (action.guild.empty())
        showDirectMessages();
      else if (m_dmMode || m_guildId != action.guild)
        selectGuild(action.guild);
      selectChannel(action.channel);
    }
    m_window.invalidate();
  }
} // namespace app
