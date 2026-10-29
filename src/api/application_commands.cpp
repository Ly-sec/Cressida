#include "api/application_commands.h"

#include "api/models.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>

namespace api {
  namespace {
    using Json = nlohmann::json;
    using Type = CommandOptionType;
    constexpr std::size_t commandNameBytes = 128, commandDescriptionBytes = 400, commandPathLimit = 2;
    constexpr std::size_t maximumCommandFilenameBytes = 255, maximumCommandMimeBytes = 128;
    constexpr int maximumCommandEnumValue = 64;
    bool snowflake(const std::string& text) {
      if (!validSnowflake(text) || text.front() == '0')
        return false;
      std::uint64_t value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      return value && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    }
    bool text(const Json& value, std::size_t limit, bool empty = false) {
      if (!value.is_string())
        return false;
      const auto& string = value.get_ref<const std::string&>();
      return (empty || !string.empty())
          && string.size() <= limit
          && std::ranges::none_of(string, [](unsigned char ch) { return ch < ' ' || ch == 127; });
    }
    std::string name(const Json& value) {
      const auto& canonical = value.contains("name_default") ? value.at("name_default") : value.at("name");
      if (!text(canonical, commandNameBytes)
          || canonical.get_ref<const std::string&>().find_first_of(" /\\:\t\n") != std::string::npos)
        throw std::runtime_error("Invalid application command name.");
      return canonical.get<std::string>();
    }
    std::string description(const Json& value) {
      if (!value.contains("description") || !text(value["description"], commandDescriptionBytes, true))
        throw std::runtime_error("Invalid application command description.");
      return value["description"].get<std::string>();
    }
    std::vector<int> integers(const Json& value, std::size_t limit) {
      if (!value.is_array() || value.size() > limit)
        throw std::runtime_error("Invalid application command constraints.");
      std::vector<int> result;
      for (const auto& item : value) {
        if (!item.is_number_integer() || item < 0 || item > maximumCommandEnumValue)
          throw std::runtime_error("Invalid application command constraint.");
        result.push_back(item.get<int>());
      }
      return result;
    }
    CommandPermissions permissions(const Json& value) {
      CommandPermissions result;
      if (value.is_null())
        return result;
      if (!value.is_object())
        throw std::runtime_error("Invalid application command permissions.");
      result.specified = true;
      if (value.contains("user"))
        result.user = value.at("user").get<bool>();
      for (const auto field : {"roles", "channels"}) {
        if (!value.contains(field))
          continue;
        if (!value[field].is_object() || value[field].size() > commandIndexLimit)
          throw std::runtime_error("Invalid application command permissions.");
        auto& target = std::string_view(field) == "roles" ? result.roles : result.channels;
        for (const auto& [id, allow] : value[field].items()) {
          if (!snowflake(id) || !allow.is_boolean())
            throw std::runtime_error("Invalid application command permission identity.");
          target[id] = allow.get<bool>();
        }
      }
      return result;
    }
    std::uint64_t permissionBits(const Json& value) {
      if (!value.is_string())
        throw std::runtime_error("Invalid default command permissions.");
      const auto& string = value.get_ref<const std::string&>();
      std::uint64_t result = 0;
      const auto parsed = std::from_chars(string.data(), string.data() + string.size(), result);
      if (parsed.ec != std::errc{} || parsed.ptr != string.data() + string.size() || string.empty())
        throw std::runtime_error("Invalid default command permissions.");
      return result;
    }
    CommandOption option(const Json& value) {
      CommandOption result;
      const int type = value.at("type").get<int>();
      if (type < int(Type::String) || type > int(Type::Attachment))
        throw std::runtime_error("Unsupported application command option.");
      result.type = Type(type);
      result.name = name(value);
      result.label = value.at("name").get<std::string>();
      result.description = description(value);
      result.required = value.value("required", false);
      result.autocomplete = value.value("autocomplete", false);
      if (value.contains("choices"))
        result.choices = parseCommandChoices(value["choices"]);
      if ((!result.choices.empty() || result.autocomplete)
          && result.type != Type::String
          && result.type != Type::Integer
          && result.type != Type::Number)
        throw std::runtime_error("Invalid application command choices.");
      if (value.contains("channel_types"))
        result.channelTypes = integers(value["channel_types"], commandOptionLimit);
      if (value.contains("min_value"))
        result.minimum = value["min_value"].get<double>();
      if (value.contains("max_value"))
        result.maximum = value["max_value"].get<double>();
      if ((result.minimum && !std::isfinite(*result.minimum))
          || (result.maximum && !std::isfinite(*result.maximum))
          || (result.minimum && result.maximum && *result.minimum > *result.maximum))
        throw std::runtime_error("Invalid application command numeric limits.");
      const auto minLength = value.value("min_length", std::int64_t(0));
      const auto maxLength = value.value("max_length", std::int64_t(commandStringLimit));
      if (minLength < 0 || maxLength < minLength || maxLength > std::int64_t(commandStringLimit))
        throw std::runtime_error("Invalid application command text limits.");
      result.minLength = minLength;
      result.maxLength = maxLength;
      return result;
    }
    void leaves(const Json& values, ApplicationCommand command, std::vector<ApplicationCommand>& result) {
      if (!values.is_array() || values.size() > commandOptionLimit || command.path.size() > commandPathLimit)
        throw std::runtime_error("Invalid application command options.");
      bool branching = false, arguments = false;
      std::set<std::string> names;
      for (const auto& value : values) {
        if (!value.is_object() || !names.insert(name(value)).second)
          throw std::runtime_error("Duplicate application command option.");
        const auto type = value.at("type").get<int>();
        if (type == int(Type::Subcommand) || type == int(Type::Group)) {
          if (command.path.size() == commandPathLimit
              || (!command.path.empty() && (command.path.back().first != Type::Group || type != int(Type::Subcommand))))
            throw std::runtime_error("Invalid application command nesting.");
          branching = true;
          auto child = command;
          child.path.emplace_back(Type(type), name(value));
          child.label += " " + value.at("name").get<std::string>();
          child.description = description(value);
          const auto children = value.value("options", Json::array());
          if (type == int(Type::Group) && children.empty())
            throw std::runtime_error("Empty application command group.");
          leaves(children, std::move(child), result);
        } else {
          if (!command.path.empty() && command.path.back().first == Type::Group)
            throw std::runtime_error("A command group requires a subcommand.");
          arguments = true;
          command.options.push_back(option(value));
        }
      }
      if (arguments && branching)
        throw std::runtime_error("Invalid mixed application command options.");
      if (!branching) {
        if (result.size() >= commandIndexLimit)
          throw std::runtime_error("The application command index is too large.");
        result.push_back(std::move(command));
      }
    }
    std::string allChannels(std::string guild) {
      for (std::size_t i = guild.size(); i; --i) {
        if (guild[i - 1] != '0') {
          --guild[i - 1];
          break;
        }
        guild[i - 1] = '9';
      }
      const auto first = guild.find_first_not_of('0');
      return first == std::string::npos ? "0" : guild.substr(first);
    }
    std::optional<bool> memberPermission(const CommandPermissions& permissions, const CommandContext& context) {
      if (permissions.user)
        return permissions.user;
      std::optional<bool> roles;
      for (const auto& role : context.roles)
        if (const auto at = permissions.roles.find(role); at != permissions.roles.end())
          roles = roles.value_or(false) || at->second;
      if (roles)
        return roles;
      if (const auto at = permissions.roles.find(context.guild); at != permissions.roles.end())
        return at->second;
      return {};
    }
    std::optional<bool> channelPermission(const CommandPermissions& permissions, const CommandContext& context) {
      if (const auto at = permissions.channels.find(context.thread ? context.parent : context.channel);
          at != permissions.channels.end())
        return at->second;
      if (const auto at = permissions.channels.find(allChannels(context.guild)); at != permissions.channels.end())
        return at->second;
      return {};
    }
    Json argument(const CommandOption& option, const std::string& text, bool partial) {
      Json result;
      if (text.size() > commandStringLimit * 4)
        throw std::runtime_error("This command value is too long.");
      switch (option.type) {
      case Type::String:
        if (!partial && (utf8Length(text) < option.minLength || utf8Length(text) > option.maxLength))
          throw std::runtime_error("Check the text length for " + option.label + ".");
        result = text;
        break;
      case Type::Integer: {
        std::int64_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{}
            || parsed.ptr != text.data() + text.size()
            || value < -commandIntegerLimit
            || value > commandIntegerLimit)
          throw std::runtime_error("Enter a whole number for " + option.label + ".");
        result = value;
        break;
      }
      case Type::Number: {
        double value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{}
            || parsed.ptr != text.data() + text.size()
            || !std::isfinite(value)
            || value < -commandIntegerLimit
            || value > commandIntegerLimit)
          throw std::runtime_error("Enter a number for " + option.label + ".");
        result = value;
        break;
      }
      case Type::Boolean:
        if (text != "true" && text != "false")
          throw std::runtime_error("Choose true or false for " + option.label + ".");
        result = text == "true";
        break;
      case Type::Attachment: {
        std::size_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty()
            || parsed.ec != std::errc{}
            || parsed.ptr != text.data() + text.size()
            || value >= commandOptionLimit)
          throw std::runtime_error("Choose an attachment for " + option.label + ".");
        result = std::to_string(value);
        break;
      }
      default:
        if (!snowflake(text))
          throw std::runtime_error("Choose a person, role or channel for " + option.label + ".");
        result = text;
        break;
      }
      if (!partial
          && result.is_number()
          && ((option.minimum && result.get<double>() < *option.minimum)
              || (option.maximum && result.get<double>() > *option.maximum)))
        throw std::runtime_error("The number is outside the allowed range for " + option.label + ".");
      if (!partial && !option.choices.empty() && std::ranges::none_of(option.choices, [&](const auto& choice) {
            return choice.value == result;
          }))
        throw std::runtime_error("Choose one of the listed values for " + option.label + ".");
      return result;
    }
  } // namespace
  std::string ApplicationCommand::key() const {
    std::string result = application + ":" + id;
    for (const auto& [type, name] : path)
      result += ":" + name;
    return result;
  }
  std::string ApplicationCommand::displayName() const { return "/" + label; }
  std::string commandIndexPath(const CommandIndexQuery& query) {
    if (!snowflake(query.channel) || (!query.guild.empty() && !snowflake(query.guild)))
      return {};
    if (query.user)
      return "/users/@me/application-command-index";
    return query.guild.empty() ? "/channels/" + query.channel + "/application-command-index"
                               : "/guilds/" + query.guild + "/application-command-index";
  }
  std::vector<CommandChoice> parseCommandChoices(const Json& value) {
    if (!value.is_array() || value.size() > commandChoiceLimit)
      throw std::runtime_error("Invalid application command choices.");
    std::vector<CommandChoice> result;
    for (const auto& entry : value) {
      if (!entry.is_object()
          || !entry.contains("name")
          || !text(entry["name"], commandDescriptionBytes)
          || !entry.contains("value")
          || !(
              text(entry["value"], commandDescriptionBytes, true)
              || (entry["value"].is_number()
                  && std::isfinite(entry["value"].get<double>())
                  && std::abs(entry["value"].get<double>()) <= commandIntegerLimit)
          ))
        throw std::runtime_error("Invalid application command choice.");
      std::string label = entry["name"].get<std::string>();
      if (entry.contains("name_localized") && !entry["name_localized"].is_null()) {
        if (!text(entry["name_localized"], commandDescriptionBytes))
          throw std::runtime_error("Invalid localized command choice.");
        label = entry["name_localized"].get<std::string>();
      }
      result.push_back({std::move(label), entry["value"]});
    }
    return result;
  }
  std::string commandChoiceText(const Json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
  }
  bool validCommandUploads(const std::vector<Upload>& uploads) {
    if (uploads.size() > commandUploadLimit)
      return false;
    std::size_t total = 0;
    for (const auto& upload : uploads) {
      if (upload.bytes.empty()
          || upload.bytes.size() > maxUploadBytes - total
          || upload.filename.empty()
          || upload.filename.size() > maximumCommandFilenameBytes
          || upload.filename.find_first_of("/\\\r\n") != std::string::npos
          || std::ranges::any_of(upload.filename, [](unsigned char c) { return c < 32 || c == 127; })
          || upload.mime.empty()
          || upload.mime.size() > maximumCommandMimeBytes
          || upload.mime.find('/') == std::string::npos
          || std::ranges::any_of(upload.mime, [](unsigned char c) { return c <= 32 || c >= 127; }))
        return false;
      total += upload.bytes.size();
    }
    return true;
  }
  bool validCommandAttachments(
      const ApplicationCommand& command, const std::vector<std::string>& values, const std::vector<Upload>& uploads
  ) {
    if (values.size() != command.options.size() || !validCommandUploads(uploads))
      return false;
    std::set<std::size_t> used;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (command.options[i].type != Type::Attachment || values[i].empty())
        continue;
      std::size_t index = 0;
      const auto parsed = std::from_chars(values[i].data(), values[i].data() + values[i].size(), index);
      if (parsed.ec != std::errc{} || parsed.ptr != values[i].data() + values[i].size() || index >= uploads.size())
        return false;
      used.insert(index);
    }
    return used.size() == uploads.size();
  }
  std::vector<ApplicationCommand> parseCommandIndex(const Json& value, const CommandIndexQuery& query) {
    if (commandIndexPath(query).empty()
        || !value.is_object()
        || !value.contains("applications")
        || !value["applications"].is_array()
        || value["applications"].size() > commandApplicationLimit
        || !value.contains("application_commands")
        || !value["application_commands"].is_array()
        || value["application_commands"].size() > commandIndexLimit)
      throw std::runtime_error("The command list is unavailable. Try loading it again.");
    std::map<std::string, CommandApplication> apps;
    for (const auto& entry : value["applications"]) {
      CommandApplication app;
      app.id = entry.at("id").get<std::string>();
      if (!snowflake(app.id) || !text(entry.at("name"), commandDescriptionBytes))
        throw std::runtime_error("Invalid command application identity.");
      app.name = entry["name"].get<std::string>();
      if (entry.contains("icon") && entry["icon"].is_string() && validAssetHash(entry["icon"].get<std::string>()))
        app.icon = entry["icon"].get<std::string>();
      app.permissions = permissions(entry.value("permissions", Json{}));
      if (entry.contains("bot_id") && entry["bot_id"].is_string() && snowflake(entry["bot_id"].get<std::string>()))
        app.bot = entry["bot_id"].get<std::string>();
      if (entry.contains("bot") && entry["bot"].is_object())
        app.bot = entry["bot"].value("id", app.bot);
      if (!app.bot.empty() && !snowflake(app.bot))
        throw std::runtime_error("Invalid command bot identity.");
      if (!apps.emplace(app.id, std::move(app)).second)
        throw std::runtime_error("Duplicate command application identity.");
    }
    std::vector<ApplicationCommand> result;
    std::set<std::string> seen;
    for (const auto& entry : value["application_commands"]) {
      if (entry.value("type", 1) != 1)
        continue;
      ApplicationCommand command;
      command.id = entry.at("id").get<std::string>();
      command.version = entry.at("version").get<std::string>();
      command.application = entry.at("application_id").get<std::string>();
      command.guild = entry.value("guild_id", std::string{});
      if (!snowflake(command.id)
          || !snowflake(command.version)
          || !snowflake(command.application)
          || (!command.guild.empty() && (command.guild != query.guild || query.user))
          || !apps.contains(command.application)
          || !seen.insert(command.id).second)
        throw std::runtime_error("Unexpected command identity.");
      command.app = apps.at(command.application);
      command.name = name(entry);
      command.label = entry.at("name").get<std::string>();
      command.description = description(entry);
      command.permissions = permissions(entry.value("permissions", Json{}));
      if (entry.contains("default_member_permissions") && !entry["default_member_permissions"].is_null())
        command.defaultPermissions = permissionBits(entry["default_member_permissions"]);
      command.dmPermission = entry.value("dm_permission", true);
      command.nsfw = entry.value("nsfw", false);
      if (entry.contains("contexts") && !entry["contexts"].is_null())
        command.contexts = integers(entry["contexts"], commandOptionLimit);
      leaves(entry.value("options", Json::array()), std::move(command), result);
    }
    return result;
  }
  bool commandAvailable(const ApplicationCommand& command, const CommandContext& context) {
    if (!command.guild.empty() && command.guild != context.guild)
      return false;
    const int location = !context.guild.empty()                              ? 0
        : !context.recipient.empty() && context.recipient == command.app.bot ? 1
                                                                             : 2;
    if ((!command.contexts.empty() && std::ranges::find(command.contexts, location) == command.contexts.end())
        || (context.guild.empty() && command.contexts.empty() && !command.dmPermission)
        || (command.nsfw && !context.nsfw))
      return false;
    if (context.guild.empty())
      return true;
    if (context.administrator)
      return true;
    if (context.permissions && !(*context.permissions & useApplicationCommands))
      return false;
    const auto channel = channelPermission(command.permissions, context);
    if (channel == false || (channel != true && channelPermission(command.app.permissions, context) == false))
      return false;
    if (const auto member = memberPermission(command.permissions, context))
      return *member;
    if (memberPermission(command.app.permissions, context) == false)
      return false;
    const bool defaults = !command.defaultPermissions
        || (*command.defaultPermissions
            && context.permissions
            && (*context.permissions & *command.defaultPermissions) == *command.defaultPermissions);
    return defaults;
  }
  Json applicationCommandData(
      const ApplicationCommand& command, const std::vector<std::string>& values, std::optional<std::size_t> focused
  ) {
    if (!snowflake(command.id)
        || !snowflake(command.application)
        || !snowflake(command.version)
        || (!command.guild.empty() && !snowflake(command.guild))
        || command.name.empty()
        || command.options.size() > commandOptionLimit
        || values.size() != command.options.size()
        || command.path.size() > commandPathLimit
        || (focused && *focused >= values.size()))
      throw std::runtime_error("Invalid application command invocation.");
    Json options = Json::array();
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto& option = command.options[i];
      const bool focus = focused && *focused == i;
      if (focused && option.type == Type::Attachment)
        continue;
      if (values[i].empty() && !focus) {
        if (option.required && !focused)
          throw std::runtime_error("Enter a value for " + option.label + ".");
        continue;
      }
      Json value;
      if (focus) {
        if (!option.autocomplete)
          throw std::runtime_error("This option does not provide suggestions.");
        value = values[i];
        if (option.type == Type::Integer || option.type == Type::Number)
          try {
            value = argument(option, values[i], true);
          } catch (const std::exception&) {
          }
      } else {
        try {
          value = argument(option, values[i], focused.has_value());
        } catch (const std::exception&) {
          if (focused)
            continue;
          throw;
        }
      }
      Json entry{{"type", int(option.type)}, {"name", option.name}, {"value", std::move(value)}};
      if (focus)
        entry["focused"] = true;
      options.push_back(std::move(entry));
    }
    for (auto at = command.path.rbegin(); at != command.path.rend(); ++at)
      options = Json::array({{{"type", int(at->first)}, {"name", at->second}, {"options", std::move(options)}}});
    Json result{
        {"id", command.id},
        {"version", command.version},
        {"name", command.name},
        {"type", 1},
        {"options", std::move(options)}
    };
    if (!command.guild.empty())
      result["guild_id"] = command.guild;
    return result;
  }
} // namespace api
