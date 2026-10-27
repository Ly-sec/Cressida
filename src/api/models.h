#pragma once
#include "api/cosmetics.h"
#include "api/stickers.h"

#include <cstdint>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace api {

  inline constexpr std::uint64_t messageSuppressEmbeds = 1ULL << 2;

  struct GuildTag {
    std::string guild, tag, badge;
    bool operator==(const GuildTag&) const = default;
  };
  bool validAssetHash(std::string_view value);
  std::optional<GuildTag> parseGuildTag(const nlohmann::json& user);

  struct Account {
    std::string id;
    std::string name;
    std::string avatar;
    std::string username, displayName, bio, pronouns;
    std::optional<int> premiumType;
    std::optional<GuildTag> guildTag;
    UserCosmetics cosmetics;
  };

  struct Presence {
    std::string status = "unknown", text, emojiId, emojiName;
  };
  bool validStatus(std::string_view status);
  std::optional<Presence> parsePresenceSettings(const nlohmann::json& value);
  nlohmann::json presenceSettingsPatch(
      const nlohmann::json& current, const std::string& status, const std::optional<std::string>& customText
  );

  using SettingValues = std::map<std::string, int>;
  SettingValues parseAccountSettings(const nlohmann::json& value);
  bool validSettingChanges(const SettingValues& changes);
  nlohmann::json accountSettingsPatch(const nlohmann::json& snapshot, const SettingValues& changes);
  struct SettingsEntry {
    std::string id, name, detail;
  };
  std::vector<SettingsEntry> parseSettingsEntries(const std::string& section, const nlohmann::json& value);

  struct Guild {
    std::string id;
    std::string name;
    std::string icon;
    std::vector<std::string> features;
    std::optional<int> members, online;
    // Absent means a partial snapshot did not provide the setting; an empty
    // value explicitly means this server has no rules channel.
    std::optional<std::string> rulesChannelId;
  };

  enum class GuildBadge { None, Community, Partnered, Verified };
  GuildBadge guildBadge(const Guild& guild);
  struct GuildUpdate {
    std::string id;
    std::optional<std::string> name, icon;
    std::optional<std::vector<std::string>> features;
    std::optional<std::optional<int>> members, online;
    std::optional<std::string> rulesChannelId;
    void apply(Guild& guild) const;
    void merge(const GuildUpdate& update);
  };
  std::vector<GuildUpdate> parseGuildUpdates(std::string_view event, const nlohmann::json& data);

  struct GuildFolder {
    std::string id;
    std::string name;
    std::uint32_t color = 0x5865F2;
    std::vector<std::string> guildIds;
  };
  struct GuildLayout {
    std::vector<GuildFolder> folders;
    std::vector<std::string> positions;
  };

  struct MemberRow {
    int index = 0;
    Account user;
    std::string status = "offline";
    std::string group;
    int count = -1;
    std::string groupId;
  };
  class MemberList {
  public:
    void apply(
        const nlohmann::json& update, const std::map<std::string, std::string>& roles,
        const std::map<std::string, Account>& users = {}
    );
    std::vector<MemberRow> rows() const;
    int total = 0;

  private:
    std::map<int, MemberRow> m_rows;
    std::map<std::string, int> m_groupCounts;
  };

  struct ForumTag {
    std::string id, name, emojiId, emojiName;
  };
  struct PermissionOverwrite {
    std::string id, allow, deny;
    int type = 0;
  };
  enum class ChannelType : int {
    GuildText = 0,
    DirectMessage = 1,
    GuildVoice = 2,
    GroupDirectMessage = 3,
    GuildCategory = 4,
    GuildAnnouncement = 5,
    AnnouncementThread = 10,
    PublicThread = 11,
    PrivateThread = 12,
    GuildStageVoice = 13,
    GuildDirectory = 14,
    GuildForum = 15,
    GuildMedia = 16
  };
  struct Channel {
    std::string id;
    std::string name;
    int type = 0;
    int position = 0;
    std::string parentId;
    std::string icon;
    std::vector<Account> recipients;
    std::string lastMessageId;
    bool named = false;
    // Optional fields preserve thread metadata across partial Gateway updates.
    std::optional<bool> archived, locked, joined;
    std::string archiveTimestamp;
    std::optional<int> messageCount, threadMemberCount;
    std::vector<ForumTag> availableTags;
    std::optional<std::vector<std::string>> appliedTags;
    bool requireTag = false;
    std::optional<std::string> topic;
    std::optional<std::vector<PermissionOverwrite>> overwrites;
    std::optional<bool> nsfw;
    std::optional<int> rateLimitPerUser;
    ChannelType kind() const { return static_cast<ChannelType>(type); }
    bool isThread() const {
      return kind() == ChannelType::AnnouncementThread
          || kind() == ChannelType::PublicThread
          || kind() == ChannelType::PrivateThread;
    }
    bool isForum() const { return kind() == ChannelType::GuildForum || kind() == ChannelType::GuildMedia; }
    std::optional<bool> messageRequest, spam;
    std::optional<std::string> messageRequestTimestamp;
    bool isMessageRequest() const { return messageRequest.value_or(false) || spam.value_or(false); }
  };
  void mergePrivateChannel(Channel& current, Channel update);

  struct Media {
    std::string url;
    int width = 0, height = 0;
    bool operator==(const Media&) const = default;
  };
  struct Attachment {
    std::string id, filename, url, contentType;
    Media image;
    bool operator==(const Attachment&) const = default;
  };
  struct EmbedField {
    std::string name, value;
    bool operator==(const EmbedField&) const = default;
  };
  struct EmbedComponentMedia {
    Media media;
    std::string description;
    bool spoiler = false, video = false;
    bool operator==(const EmbedComponentMedia&) const = default;
  };
  struct EmbedComponentLink {
    std::string label, url;
    bool disabled = false;
    bool operator==(const EmbedComponentLink&) const = default;
  };
  struct EmbedComponent {
    int type = 0, spacing = 1;
    std::string content;
    std::vector<EmbedComponent> children;
    std::vector<EmbedComponentMedia> gallery;
    std::optional<EmbedComponentMedia> thumbnail;
    std::optional<EmbedComponentLink> link;
    std::optional<std::uint32_t> accent;
    bool spoiler = false, divider = true;
    bool operator==(const EmbedComponent&) const = default;
  };
  struct Embed {
    std::string title, description, url, provider, author, footer, type;
    std::uint32_t color = 0x5865F2;
    Media image, thumbnail;
    std::vector<EmbedField> fields;
    Media video;
    std::vector<EmbedComponent> components;
    bool operator==(const Embed&) const = default;
  };
  struct Upload {
    std::string filename, mime;
    std::vector<unsigned char> bytes;
  };
  constexpr std::size_t maxUploadBytes = 20 * 1024 * 1024;
  bool validUploads(const std::vector<Upload>& uploads);
  // Fixed Discord CDN/proxy and public GIF provider origins, without credentials.
  std::string mediaUrl(std::string_view value);

  struct Reaction {
    std::string id, name;
    int count = 0;
    bool me = false;
    int burstCount = 0;
    bool meBurst = false;
  };
  struct ForwardedMessage {
    std::string message, channel, guild, timestamp, content;
    std::vector<Attachment> attachments;
    std::vector<Embed> embeds;
    std::map<std::string, std::string> mentions;
    bool available = false;
    std::vector<Sticker> stickers;
    bool operator==(const ForwardedMessage&) const = default;
  };
  struct MessageReference {
    std::string message, channel, authorId, author, content;
    bool deleted = false;
    bool mention = false;
    // A reference-only payload must not erase an already received preview.
    bool available = false;
    std::string avatar;
    bool operator==(const MessageReference&) const = default;
  };
  struct ReactionUpdate {
    enum class Kind { Add, Remove, RemoveEmoji, RemoveAll };
    Kind kind = Kind::Add;
    std::string channel, message, user;
    Reaction emoji;
    bool burst = false;
  };
  std::optional<ReactionUpdate> parseReactionUpdate(std::string_view type, const nlohmann::json& data);
  inline constexpr int defaultMessageType = 0;
  inline constexpr int replyMessageType = 19;
  struct Message {
    std::string id;
    std::string channelId;
    std::string author;
    std::string content;
    std::string timestamp;
    std::string authorId;
    std::string avatar;
    std::vector<Attachment> attachments;
    std::vector<Embed> embeds;
    std::map<std::string, std::string> mentions;
    std::optional<MessageReference> reply;
    std::vector<Reaction> reactions;
    std::optional<std::vector<std::string>> memberRoles;
    std::optional<GuildTag> authorTag;
    std::optional<Channel> thread;
    bool hasThread = false;
    struct Button {
      std::string label, customId, url, emojiId, emojiName;
      int style = 2;
      bool disabled = false, animated = false;
    };
    std::vector<std::vector<Button>> buttons;
    std::string applicationId;
    std::uint64_t flags = 0;
    int type = defaultMessageType;
    std::string webhookId;
    std::vector<std::string> mentionRoles;
    bool mentionEveryone = false;
    std::string editedTimestamp;
    // Local session-only retention state; never read from or sent to Discord.
    bool deleted = false;
    std::optional<ForwardedMessage> forwarded;
    std::vector<Sticker> stickers;
    std::string nonce;
  };

  struct MessagePatch {
    std::optional<std::string> content;
    std::optional<std::vector<Attachment>> attachments;
    std::optional<std::vector<Embed>> embeds;
    std::optional<std::map<std::string, std::string>> mentions;
    std::optional<std::optional<MessageReference>> reply;
    std::optional<std::vector<Reaction>> reactions;
    std::optional<std::vector<std::string>> memberRoles;
    std::optional<std::optional<GuildTag>> authorTag;
    std::optional<std::optional<Channel>> thread;
    std::optional<bool> hasThread;
    std::optional<std::vector<std::vector<Message::Button>>> buttons;
    std::optional<std::string> applicationId;
    std::optional<std::uint64_t> flags;
    std::optional<int> type;
    std::optional<std::string> webhookId;
    std::optional<std::vector<std::string>> mentionRoles;
    std::optional<bool> mentionEveryone;
    std::optional<std::string> editedTimestamp;
    std::optional<std::optional<ForwardedMessage>> forwarded;
    std::optional<std::vector<Sticker>> stickers;
    void apply(Message& message) const;
  };
  MessagePatch parseMessagePatch(const nlohmann::json& value);
  void applyReaction(Message& message, const ReactionUpdate& update, const std::string& self);

  // Parse only protocol data; presentation and account lookups stay outside these records.
  Account parseAccount(const nlohmann::json& value);
  std::vector<Guild> parseGuilds(const nlohmann::json& value);
  std::vector<Channel> parseChannels(const nlohmann::json& value);
  std::vector<Channel> orderChannels(std::vector<Channel> channels);
  std::vector<Channel>
  parsePrivateChannels(const nlohmann::json& value, const std::map<std::string, Account>& users = {});
  std::vector<Message> parseMessages(const nlohmann::json& value);
  bool validSnowflake(const std::string& value);
  std::size_t utf8Length(const std::string& value);
  GuildLayout parseGuildLayout(const nlohmann::json& value);
  std::vector<GuildFolder> arrangeGuilds(const std::vector<Guild>& guilds, const GuildLayout& layout);
  std::string avatarPath(const nlohmann::json& user);
  std::string emojiPath(const std::string& id, bool animated = false);

} // namespace api
