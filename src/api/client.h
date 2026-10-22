#pragma once

#include "api/actions.h"
#include "api/channel_features.h"
#include "api/connection_bootstrap.h"
#include "api/emoji.h"
#include "api/gateway.h"
#include "api/gifs.h"
#include "api/guild_settings.h"
#include "api/history.h"
#include "api/inbox.h"
#include "api/member_list_identity.h"
#include "api/models.h"
#include "api/profile.h"
#include "api/read_state.h"
#include "api/rest_rate_limits.h"
#include "api/rest_restrictions.h"
#include "api/role_styles.h"
#include "api/threads.h"
#include "api/typing.h"
#include "api/user_directory.h"
#include "api/user_settings.h"
#include "auth/credentials.h"
#include "core/process/wake_event.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace api {
  class RestTransportCache;
  struct RestReadIntent;

  enum class Operation {
    Connect,
    Guilds,
    BootstrapSettings,
    ReadState,
    ChannelSnapshot,
    InvalidateChannels,
    Acknowledge,
    AccountSettings,
    SaveAccountSettings,
    SettingsList,
    Profile,
    SaveProfile,
    SavePresence,
    PresenceChanged,
    UserState,
    Interaction,
    Reaction,
    AccountUpdate,
    Channels,
    Messages,
    Send,
    Layout,
    Members,
    MessageCreate,
    MessageUpdate,
    MessageDelete,
    Gateway,
    PrivateChannels,
    PrivateChannelUpsert,
    PrivateChannelDelete,
    Credentials,
    GifSearch,
    GifFavorites,
    SaveGifFavorite,
    GuildEmojis,
    GuildStickers,
    StickerPacks,
    StickerDetails,
    EmojiPreferences,
    SaveEmojiFavorite,
    RoleStyles,
    Threads,
    ThreadUpdate,
    ChannelDetails,
    ChannelPage,
    ComponentResult,
    MemberSearch,
    GuildMetadata,
    GuildSettings,
    UserSettings,
    TypingStart,
    InboxRead,
    InboxWrite,
    InboxUpdate,
    AccountRestriction
  };

  struct Event {
    Operation operation = Operation::Connect;
    std::uint64_t session = 0;
    std::string resource;
    std::string error;
    bool unauthorized = false;
    Account account;
    std::vector<Guild> guilds;
    bool guildsComplete = false;
    std::vector<GuildUpdate> guildUpdates;
    std::vector<Channel> channels;
    std::vector<Message> messages;
    std::optional<HistoryQuery> history;
    GuildLayout layout;
    std::vector<MemberRow> members;
    std::optional<MemberListIdentity> memberIdentity;
    RoleUpdates roleUpdates;
    ThreadUpdates threadUpdates;
    std::optional<ThreadQuery> threadQuery;
    std::optional<ThreadPage> threadPage;
    std::optional<ChannelPageQuery> channelQuery;
    std::optional<ChannelPageResult> channelPage;
    std::string nonce;
    std::optional<ApplicationModal> modal;
    int memberCount = 0;
    std::vector<std::string> removed;
    std::string messageId;
    std::optional<std::string> content;
    bool live = false;
    MessagePatch patch;
    std::optional<Presence> presence;
    std::optional<SettingValues> settings;
    std::vector<SettingsEntry> settingsEntries;
    std::map<std::string, std::string> unread;
    std::map<std::string, UnreadCursor> unreadCursors;
    UserUpdates users;
    std::optional<api::Interaction> interaction;
    std::optional<ReactionUpdate> reaction;
    std::vector<Account> reactors;
    std::vector<Account> memberMatches;
    std::optional<UserProfile> profile;
    std::optional<GuildProfile> guildProfile;
    bool guildVerificationRequired = false;
    std::string inviteUrl;
    bool noSavedLogin = false, restoredLogin = false;
    std::optional<auth::CredentialOperation> credentialOperation;
    std::vector<Gif> gifs;
    std::vector<Emoji> emojis;
    std::vector<Sticker> stickers;
    std::optional<EmojiPreferences> emojiPreferences;
    std::optional<GuildSettingsRequest> guildSettingsRequest;
    nlohmann::json guildSettings;
    bool guildSettingsMore = false;
    std::string guildSettingsCursor;
    std::uint64_t guildSettingsMfaChallenge = 0;
    std::vector<std::string> guildSettingsMfaMethods;
    std::optional<UserSettingsRequest> userSettingsRequest;
    nlohmann::json userSettings;
    bool userSettingsMore = false;
    std::string userSettingsCursor;
    std::uint64_t userSettingsMfaChallenge = 0;
    std::vector<std::string> userSettingsMfaMethods;
    bool userSettingsSessionEnded = false, userSettingsSensitive = false;
    std::optional<api::GuildPrivacy> guildPrivacy;
    std::optional<api::TypingStart> typing;
    std::optional<InboxQuery> inboxQuery;
    std::optional<InboxPage> inboxPage;
  };

  // An ordered account/mutation owner and three independent, bounded GET lanes.
  // No rendering or window-system dependency.
  class Client {
  public:
    explicit Client(
        std::unique_ptr<auth::CredentialBackend> credentials = {}, std::filesystem::path disabledLogin = {}
    );
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    std::uint64_t connect(std::string token);
    std::uint64_t connectHandoff(std::string key, std::string handoffToken);
    std::uint64_t restoreLogin();
    std::uint64_t logout();
    std::uint64_t disconnect();
    bool channels(const std::string& guildId);
    bool privateChannels();
    bool threads(ThreadQuery query);
    bool channelDetails(const std::string& channel);
    bool channelPage(ChannelPageQuery query);
    bool inbox(InboxQuery query);
    bool guildSettings(GuildSettingsRequest request);
    bool userSettings(UserSettingsRequest request);
    void clearUserSettingsSecrets();
    bool acknowledge(const std::string& channel, const std::string& message);
    bool messages(const std::string& channelId, HistoryQuery query = {});
    bool promotePreload(const std::string& channel, const HistoryQuery& query);
    bool send(
        const std::string& channelId, std::string content, std::vector<Upload> uploads = {},
        std::optional<MessageReference> reply = {}, std::vector<std::string> stickers = {}, std::string nonce = {}
    );
    bool interact(api::Interaction action);
    bool profile();
    bool accountSettings();
    bool searchGifs(const std::string& query);
    bool gifFavorites();
    bool saveGifFavorite(const Gif& gif, bool add);
    bool guildEmojis(const std::string& guild);
    bool guildStickers(const std::string& guild);
    bool stickerPacks();
    bool sticker(const std::string& id);
    bool emojiPreferences();
    bool saveEmojiFavorite(const std::string& key, bool add);
    bool saveAccountSettings(SettingValues changes);
    bool settingsList(const std::string& section);
    bool saveProfile(const Account& profile);
    bool setPresence(std::string status, std::optional<std::string> customText = {});
    void subscribe(const std::string& guild, const std::string& channel, int first = 0);
    void subscribe(
        const std::string& guild, const std::string& channel, int first, std::optional<MemberListIdentity> identity
    );
    void requestMembers(const std::string& guild, const std::vector<std::string>& users);
    bool searchMembers(const std::string& guild, const std::string& query, const std::string& nonce);
    std::vector<Event> drain();
    int eventFd() const noexcept { return m_wake.fd(); }

  private:
    struct Command {
      Operation operation;
      std::uint64_t session;
      std::string resource;
      std::string content;
      std::optional<std::string> handoffKey;
      std::vector<Upload> uploads;
      std::optional<std::string> customStatus;
      std::uint64_t presenceRevision = 0;
      std::optional<MessageReference> reply;
      std::optional<api::Interaction> interaction;
      bool restoredLogin = false;
      std::optional<HistoryQuery> history;
      std::optional<ThreadQuery> threadQuery;
      std::optional<ChannelPageQuery> channelQuery;
      std::optional<GuildSettingsRequest> guildSettings;
      bool background = false;
      std::uint64_t readRevision = 0;
      std::size_t guildsLoaded = 0;
      std::optional<UserSettingsRequest> userSettings;
      std::shared_ptr<RestReadIntent> readIntent;
      std::chrono::steady_clock::time_point queuedAt = std::chrono::steady_clock::now();
      std::vector<std::string> stickers;
      std::string nonce;
      std::optional<InboxQuery> inboxQuery;
    };
    struct PreloadRead {
      std::uint64_t session;
      std::string channel;
      HistoryQuery query;
      std::weak_ptr<RestReadIntent> intent;
    };
    std::uint64_t beginConnect(std::string credential, std::optional<std::string> handoffKey);
    bool enqueue(Command command);
    void run(std::stop_token stop);
    void runReads(std::stop_token stop, std::size_t lane);
    void wakeRequests();
    bool completeConnect(Event event, BootstrapSource source);
    void queueBootstrap(std::uint64_t session, bool guilds, bool settings);
    void publish(Event event);
    void saveLogin(std::uint64_t session, const std::string& token);
    void forgetLogin(std::uint64_t session);
    void gatewayEvent(std::uint64_t session, const std::string& type, const nlohmann::json& data);

    process::WakeEvent m_wake;
    process::WakeEvent m_requestWake;
    std::atomic<std::uint64_t> m_session{0};
    std::atomic<std::uint64_t> m_authenticatedSession{0};
    std::atomic<std::uint64_t> m_restoringSession{0};
    std::atomic<std::uint64_t> m_presenceRevision{0};
    std::atomic<std::uint64_t> m_foregroundRevision{0}, m_historyRevision{0}, m_readCredentialRevision{0};
    std::array<process::WakeEvent, 3> m_readWake;
    std::string m_readToken, m_authenticatedUser; // Under m_mutex; readers require the authenticated generation.
    ConnectionBootstrap m_bootstrap;
    bool m_readsPaused = false;
    unsigned m_backgroundReads = 0;
    RestRateLimits m_rateLimits;
    RestRestrictions m_restRestrictions;
    std::unique_ptr<RestTransportCache> m_transportCache;
    std::atomic<std::uint64_t> m_guildsReadySession{0}, m_bootstrapSettingsReadySession{0};
    std::atomic<std::uint64_t> m_userSettingsSecretsEpoch{0};
    std::mutex m_mutex;
    std::condition_variable_any m_ready;
    std::deque<Command> m_commands;
    std::vector<PreloadRead> m_preloadReads;
    std::vector<Event> m_events;
    std::unique_ptr<Gateway> m_gateway;
    std::unique_ptr<auth::CredentialStore> m_credentials;
    std::string m_selectedGuild, m_selectedChannel;
    std::optional<MemberListIdentity> m_selectedMemberIdentity;
    // Private Gateway session identity is only read by the REST worker for explicit component actions.
    std::string m_gatewaySessionId;
    std::string m_authSessionIdHash;
    std::optional<bool> m_authTotpEnabled;
    std::uint64_t m_gatewaySessionGeneration = 0;
    int m_selectedMemberFirst = 0;
    // Shared only under m_mutex: latest Gateway/acknowledged override flags.
    std::map<std::string, std::uint64_t> m_channelNotificationFlags;
    bool m_channelNotificationFlagsReady = false;
    std::map<std::string, std::uint64_t> m_guildNotificationFlags;
    bool m_guildNotificationFlagsReady = false;
    std::map<std::string, std::uint64_t> m_guildPreferenceRevisions;
    std::map<std::string, GuildNotification> m_guildPreferenceAcks;
    // These caches belong exclusively to the Gateway worker.
    std::uint64_t m_memberSession = 0;
    MemberListStream m_memberStream;
    MemberList m_memberList;
    api::ReadState m_readState;
    UserDirectory m_userDirectory;
    std::string m_self;
    std::map<std::string, std::string> m_roleNames;
    std::map<std::string, Account> m_users;
    // Declared last so the worker joins before any state it uses is destroyed.
    std::vector<std::jthread> m_readWorkers;
    std::jthread m_worker;
  };

} // namespace api
