#pragma once
#include "api/models.h"
#include "api/role_styles.h"

#include <span>

namespace api {
  inline constexpr std::uint64_t viewChannel = 1ULL << 10, manageChannel = 1ULL << 4, managePermissions = 1ULL << 28;
  inline constexpr std::uint64_t manageThreads = 1ULL << 34;
  inline constexpr std::uint64_t manageMessages = 1ULL << 13;
  inline constexpr std::uint64_t pinMessages = 1ULL << 51;
  inline constexpr std::uint64_t sendMessages = 1ULL << 11, sendMessagesInThreads = 1ULL << 38;
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
