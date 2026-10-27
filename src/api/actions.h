#pragma once

#include "api/channel_features.h"
#include "api/models.h"
#include "api/server_organization.h"
#include "api/server_preferences.h"
#include "api/server_privacy.h"

#include <nlohmann/json.hpp>

namespace api {
  enum class InteractionKind {
    AddReaction,
    RemoveReaction,
    EditMessage,
    RemoveEmbeds,
    DeleteMessage,
    Block,
    Unblock,
    AddFriend,
    RemoveFriend,
    Nickname,
    OpenDm,
    CreateDm,
    PinDm,
    UnpinDm,
    UserProfile,
    Mute,
    Unmute,
    ListReactions,
    GuildProfile,
    JoinGuild,
    CreateThread,
    CreateForumPost,
    JoinThread,
    LeaveThread,
    UnarchiveThread,
    MessageButton,
    EditChannel,
    GrantChannelAccess,
    SubmitModal,
    CreateChannel,
    DuplicateChannel,
    DeleteChannel,
    MoveChannel,
    CreateInvite,
    ChannelNotifications,
    SendInvite,
    EditChannelPermission,
    AcceptMessageRequest,
    IgnoreMessageRequest,
    GuildNotifications,
    MarkGuildRead,
    LeaveGuild,
    MoveGuild,
    EditGuildFolder,
    ReorderGuilds,
    GuildPrivacy,
    AutoModerationFeedback
  };
  inline constexpr int autoModerationSubmitFeedback = 4;
  bool
  canRemoveMessageEmbeds(const Message& message, std::string_view self, std::optional<std::uint64_t> permissions = {});
  inline constexpr std::size_t maxAutoModerationDecisionBytes = 128;
  inline constexpr std::size_t maxAutoModerationContentBytes = 16 * 1024;
  struct Interaction {
    InteractionKind kind = InteractionKind::AddReaction;
    std::string channel, message, user, emoji, text, guild;
    bool burst = false;
    std::uint64_t revision = 0;      // UI request identity, never sent to Discord.
    std::uint64_t stateRevision = 0; // Thread directory version, never sent to Discord.
    std::string content;
    std::vector<std::string> tags;
    std::vector<std::string> recipients;
    std::uint64_t notificationFlags = 0;
    std::string application, customId, nonce;
    std::uint64_t messageFlags = 0;
    std::optional<std::string> channelName, channelTopic;
    int overwriteType = 0;
    std::optional<ApplicationModal> modal;
    std::vector<std::string> values;
    std::optional<std::string> parentId;
    int channelType = 0, notificationLevel = 3, muteDuration = -1;
    bool privateChannel = false;
    int inviteMaxAge = 604800, inviteMaxUses = 0;
    bool inviteTemporary = false, inviteUnique = false;
    std::vector<PermissionOverwrite> channelAccess;
    std::uint64_t permissionBit = 0;
    int permissionValue = 0;
    std::optional<bool> channelNsfw;
    std::optional<int> channelRateLimit;
    bool syncPermissions = false;
    std::optional<GuildNotification> guildNotification;
    std::vector<std::pair<std::string, std::string>> readStates;
    std::uint64_t notificationFlagsMask = 0;
    std::optional<api::GuildPrivacy> guildPrivacy;
    std::optional<GuildFolderChange> folderChange;
    std::optional<GuildRailMove> railMove;
    bool markAutoModerationFeedback = false;
  };
  std::optional<MessagePatch> removedEmbedsPatch(const Interaction& action, const nlohmann::json& response);
  std::optional<nlohmann::json>
  autoModerationFeedbackBody(const Interaction& action, std::string_view analyticsToken, std::int64_t timestamp);
  struct InteractionRequest {
    std::string path, method;
    nlohmann::json body;
  };
  // Fixed routes and validated identifiers only; never accept an arbitrary URL.
  std::optional<InteractionRequest> interactionRequest(const Interaction& action);
  Channel
  createdDm(const nlohmann::json& response, const std::vector<std::string>& recipients, const std::string& self);
  nlohmann::json messageBody(
      const std::string& content, const std::optional<MessageReference>& reply = {},
      const std::vector<std::string>& stickers = {}, std::string_view nonce = {}
  );
  std::string reactionKey(const Reaction& reaction);
  nlohmann::json decodeRestBody(long status, std::string_view bytes);
  std::vector<Account> parseReactors(const nlohmann::json& value);
  // A private child channel requires a fresh parent snapshot so inherited permissions are preserved.
  nlohmann::json createChannelBody(const Interaction& action, const std::optional<nlohmann::json>& parent = {});
  nlohmann::json duplicateChannelBody(const nlohmann::json& source, const Interaction& action);
  nlohmann::json channelPermissionBody(const nlohmann::json& source, const Interaction& action);
  nlohmann::json moveChannelBody(const nlohmann::json& parent, const Interaction& action);
} // namespace api
