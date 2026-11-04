#pragma once
#include "api/models.h"
#include "api/role_styles.h"

#include <span>

namespace api {
  // Every permission bit the client evaluates. Use these names instead of literal
  // shifts so each check reads as the permission it enforces.
  namespace permission {
    inline constexpr std::uint64_t createInstantInvite = 1ULL << 0, kickMembers = 1ULL << 1, banMembers = 1ULL << 2;
    inline constexpr std::uint64_t administrator = 1ULL << 3, manageChannels = 1ULL << 4, manageGuild = 1ULL << 5;
    inline constexpr std::uint64_t addReactions = 1ULL << 6, viewAuditLog = 1ULL << 7, stream = 1ULL << 9;
    inline constexpr std::uint64_t viewChannel = 1ULL << 10, sendMessages = 1ULL << 11;
    inline constexpr std::uint64_t manageMessages = 1ULL << 13, embedLinks = 1ULL << 14, attachFiles = 1ULL << 15;
    inline constexpr std::uint64_t readMessageHistory = 1ULL << 16, mentionEveryone = 1ULL << 17;
    inline constexpr std::uint64_t useExternalEmojis = 1ULL << 18, viewGuildInsights = 1ULL << 19;
    inline constexpr std::uint64_t connect = 1ULL << 20, speak = 1ULL << 21, muteMembers = 1ULL << 22;
    inline constexpr std::uint64_t deafenMembers = 1ULL << 23, moveMembers = 1ULL << 24, useVoiceActivity = 1ULL << 25;
    inline constexpr std::uint64_t changeNickname = 1ULL << 26, manageNicknames = 1ULL << 27;
    // One bit: Manage Roles at server scope, Manage Permissions on a channel.
    inline constexpr std::uint64_t manageRoles = 1ULL << 28, managePermissions = manageRoles;
    inline constexpr std::uint64_t manageWebhooks = 1ULL << 29, manageGuildExpressions = 1ULL << 30;
    inline constexpr std::uint64_t useApplicationCommands = 1ULL << 31, manageEvents = 1ULL << 33;
    inline constexpr std::uint64_t manageThreads = 1ULL << 34, createPublicThreads = 1ULL << 35;
    inline constexpr std::uint64_t createPrivateThreads = 1ULL << 36, useExternalStickers = 1ULL << 37;
    inline constexpr std::uint64_t sendMessagesInThreads = 1ULL << 38, moderateMembers = 1ULL << 40;
    inline constexpr std::uint64_t createGuildExpressions = 1ULL << 43, createEvents = 1ULL << 44;
    inline constexpr std::uint64_t sendPolls = 1ULL << 49, pinMessages = 1ULL << 51;

    // Server-scope check: every requested bit, with Administrator granting all.
    // Unknown permissions never grant anything.
    constexpr bool allows(std::optional<std::uint64_t> permissions, std::uint64_t bits) {
      return permissions && ((*permissions & administrator) || (*permissions & bits) == bits);
    }
    // At least one of the requested bits, with the same Administrator bypass.
    constexpr bool allowsAny(std::optional<std::uint64_t> permissions, std::uint64_t bits) {
      return permissions && ((*permissions & administrator) || (*permissions & bits));
    }
  } // namespace permission
  inline constexpr std::uint64_t viewChannel = permission::viewChannel, manageChannel = permission::manageChannels;
  inline constexpr std::uint64_t managePermissions = permission::managePermissions;
  inline constexpr std::uint64_t manageThreads = permission::manageThreads;
  inline constexpr std::uint64_t manageMessages = permission::manageMessages;
  inline constexpr std::uint64_t pinMessages = permission::pinMessages;
  inline constexpr std::uint64_t sendMessages = permission::sendMessages;
  inline constexpr std::uint64_t sendMessagesInThreads = permission::sendMessagesInThreads;
  enum class MessageWriteAccess { Allowed, Unknown, Unsupported, Hidden, Denied, Archived, Locked, NotJoined };
  // Thread permissions must be resolved using the parent's overwrites.
  MessageWriteAccess messageWriteAccess(const Channel& channel, std::optional<std::uint64_t> permissions);
  std::optional<std::uint64_t> permissionBits(std::string_view decimal);
  // Modify VIEW_CHANNEL only, retaining arbitrarily higher, unknown permission bits.
  std::string withViewPermission(std::string decimal, bool enabled);
  void parseChannelDetails(Channel& channel, const nlohmann::json& value);
  std::optional<std::uint64_t> channelPermissions(
      const std::string& guild, const std::string& user, const std::string& owner, std::span<const ColoredRole> roles,
      const std::optional<std::vector<std::string>>& memberRoles, const Channel& channel
  );
  std::vector<ColoredRole>
  channelAccessRoles(const std::string& guild, std::span<const ColoredRole> roles, const Channel& channel);
  std::vector<std::vector<Message::Button>> parseMessageButtons(const nlohmann::json& value);
  bool externalLink(std::string_view url);
  struct ModalField {
    std::string customId, label, description, placeholder, value;
    bool multiline = false, required = true;
    unsigned minLength = 0, maxLength = 4000, wrapperType = 1;
    std::optional<std::uint32_t> id, wrapperId;
  };
  struct ApplicationModal {
    std::string id, application, channel, customId, title;
    std::vector<ModalField> fields;
  };
  ApplicationModal parseApplicationModal(const nlohmann::json& value);
  // Validate all required fields before constructing a bounded submission.
  nlohmann::json modalSubmission(const ApplicationModal& modal, std::span<const std::string> values);
  enum class ChannelPage { Search, Pins };
  struct ChannelPageQuery {
    ChannelPage kind = ChannelPage::Search;
    std::string guild, channel, text, before;
    unsigned offset = 0;
    std::uint64_t request = 0;
    bool operator==(const ChannelPageQuery&) const = default;
  };
  struct ChannelPageResult {
    std::vector<Message> messages;
    unsigned total = 0;
    bool more = false;
    std::string before;
  };
  std::string channelPagePath(const ChannelPageQuery& query);
  ChannelPageResult parseChannelPage(const nlohmann::json& value, const ChannelPageQuery& query);
} // namespace api
