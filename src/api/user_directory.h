#pragma once

#include "api/models.h"
#include "api/server_preferences.h"
#include "rpc/activity.h"

#include <chrono>

namespace api {
  struct ChannelNotification {
    std::optional<int> level;
    std::optional<bool> muted;
    std::optional<std::string> muteUntil;
    std::optional<std::uint64_t> flags;
  };
  std::map<std::string, ChannelNotification> channelNotifications(const nlohmann::json& settings);
  std::optional<std::chrono::system_clock::time_point> muteDeadline(std::string_view text);
  struct UserCustomStatus {
    std::string text, emojiId, emojiName;
    bool animated = false;
    bool empty() const { return text.empty() && emojiId.empty() && emojiName.empty(); }
    bool operator==(const UserCustomStatus&) const = default;
  };
  struct UserUpdate {
    std::optional<std::string> nickname, status;
    std::optional<int> relationship;
    std::optional<std::optional<GuildTag>> guildTag;
    std::optional<bool> ignored;
    std::optional<Account> account;
    std::optional<UserCustomStatus> customStatus;
    std::optional<std::vector<rpc::Activity>> activities;
    UserCosmetics cosmetics;
  };
  struct UserUpdates {
    bool reset = false;
    std::map<std::string, UserUpdate> users;
    std::map<std::string, bool> mutedChannels;
    std::map<std::string, ChannelNotification> notifications;
    bool relationshipsReset = false;
    std::map<std::string, std::map<std::string, UserCosmetics>> guildCosmetics;
    std::map<std::string, GuildNotification> guildNotifications;
  };

  // Gateway-owned protocol tracking, or an event-loop-owned view of typed updates.
  // Keep personal nicknames separate from global and guild display names so that
  // removing an alias immediately restores the original name everywhere.
  class UserDirectory {
  public:
    UserUpdates consume(std::string_view type, const nlohmann::json& data);
    void apply(const UserUpdates& update);
    std::string name(const std::string& user, const std::string& fallback) const;
    std::optional<std::string> alias(const std::string& user) const;
    std::string status(const std::string& user, const std::string& fallback = "offline") const;
    UserCustomStatus customStatus(const std::string& user) const;
    std::vector<rpc::Activity> activities(const std::string& user) const;
    UserCosmetics cosmetics(const std::string& user, const std::string& guild = {}) const;
    int relationship(const std::string& user) const;
    bool ignored(const std::string& user) const;
    std::vector<Account> relatedUsers() const;
    std::uint64_t relationshipRevision() const { return m_relationshipRevision; }
    bool muted(const std::string& channel) const;
    std::uint64_t notificationFlags(const std::string& channel) const;
    bool pinnedDm(const std::string& channel) const { return (notificationFlags(channel) & 2048) != 0; }
    int notificationLevel(const std::string& channel) const;
    std::uint64_t notificationRevision(const std::string& channel) const;
    GuildNotification guildPreferences(const std::string& guild) const;
    std::uint64_t guildNotificationRevision(const std::string& guild) const;
    std::optional<std::chrono::system_clock::time_point> nextMuteExpiry() const;
    std::optional<GuildTag> guildTag(const std::string& user, const std::optional<GuildTag>& fallback = {}) const;

  private:
    std::map<std::string, UserCosmetics> m_cosmetics;
    std::map<std::string, std::map<std::string, UserCosmetics>> m_guildCosmetics;
    std::map<std::string, std::string> m_names, m_statuses, m_globalStatuses;
    std::map<std::string, std::map<std::string, std::string>> m_guildStatuses;
    std::map<std::string, UserCustomStatus> m_customStatuses, m_globalCustomStatuses;
    std::map<std::string, std::vector<rpc::Activity>> m_activities, m_globalActivities;
    std::map<std::string, std::map<std::string, std::vector<rpc::Activity>>> m_guildActivities;
    std::map<std::string, std::map<std::string, UserCustomStatus>> m_guildCustomStatuses;
    std::map<std::string, int> m_relationships;
    std::map<std::string, Account> m_accounts;
    std::map<std::string, bool> m_ignored;
    std::uint64_t m_relationshipRevision = 0;
    std::map<std::string, std::optional<GuildTag>> m_tags;
    std::map<std::string, bool> m_mutedChannels;
    std::map<std::string, ChannelNotification> m_notifications;
    std::map<std::string, std::uint64_t> m_notificationRevisions;
    std::uint64_t m_notificationSerial = 0;
    std::map<std::string, GuildNotification> m_guildNotifications;
    std::map<std::string, std::uint64_t> m_guildNotificationRevisions;
  };
} // namespace api
