#pragma once

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace api {
  inline constexpr std::size_t commandOptionLimit = 25;
  inline constexpr std::size_t commandChoiceLimit = 25;
  inline constexpr std::size_t commandIndexLimit = 4096;
  inline constexpr std::size_t commandApplicationLimit = 512;
  inline constexpr std::size_t commandStringLimit = 6000;
  inline constexpr std::size_t commandUploadLimit = 10;
  inline constexpr std::int64_t commandIntegerLimit = 1LL << 53;
  inline constexpr std::uint64_t useApplicationCommands = 1ULL << 31;
  inline constexpr std::uint64_t commandAdministratorPermission = 1ULL << 3;

  enum class CommandOptionType {
    Subcommand = 1,
    Group,
    String,
    Integer,
    Boolean,
    User,
    Channel,
    Role,
    Mentionable,
    Number,
    Attachment
  };
  struct CommandChoice {
    std::string name;
    nlohmann::json value;
  };
  struct CommandOption {
    CommandOptionType type = CommandOptionType::String;
    std::string name, label, description;
    bool required = false, autocomplete = false;
    std::vector<CommandChoice> choices;
    std::vector<int> channelTypes;
    std::optional<double> minimum, maximum;
    std::size_t minLength = 0, maxLength = commandStringLimit;
  };
  struct CommandPermissions {
    std::optional<bool> user;
    std::map<std::string, bool> roles, channels;
    bool specified = false;
  };
  struct CommandApplication {
    std::string id, name, icon, bot;
    CommandPermissions permissions;
  };
  struct ApplicationCommand {
    std::string id, version, application, guild, name, label, description;
    CommandApplication app;
    std::vector<std::pair<CommandOptionType, std::string>> path;
    std::vector<CommandOption> options;
    CommandPermissions permissions;
    std::optional<std::uint64_t> defaultPermissions;
    std::vector<int> contexts;
    bool dmPermission = true, nsfw = false;
    std::string key() const;
    std::string displayName() const;
  };
  struct CommandIndexQuery {
    std::string channel, guild;
    bool user = false;
    std::uint64_t revision = 0;
    bool operator==(const CommandIndexQuery&) const = default;
  };
  struct CommandContext {
    std::string channel, parent, guild, user, recipient;
    std::vector<std::string> roles;
    std::optional<std::uint64_t> permissions;
    bool administrator = false, nsfw = false, thread = false;
  };
  std::string commandIndexPath(const CommandIndexQuery& query);
  std::vector<ApplicationCommand> parseCommandIndex(const nlohmann::json& value, const CommandIndexQuery& query);
  bool commandAvailable(const ApplicationCommand& command, const CommandContext& context);
  std::vector<CommandChoice> parseCommandChoices(const nlohmann::json& value);
  std::string commandChoiceText(const nlohmann::json& value);
  struct Upload;
  bool validCommandUploads(const std::vector<Upload>& uploads);
  bool validCommandAttachments(
      const ApplicationCommand& command, const std::vector<std::string>& values, const std::vector<Upload>& uploads
  );
  // Values follow the selected leaf's option order. Empty optional values are omitted.
  nlohmann::json applicationCommandData(
      const ApplicationCommand& command, const std::vector<std::string>& values, std::optional<std::size_t> focused = {}
  );
} // namespace api
