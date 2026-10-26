#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace app {
  enum class VoiceCommand : std::uint8_t { InputMuteToggle = 1, InputMuteOn = 2, InputMuteOff = 3 };

  inline constexpr auto voiceCommandNames = std::array{
      std::pair{VoiceCommand::InputMuteToggle, std::string_view{"input-mute toggle"}},
      std::pair{VoiceCommand::InputMuteOn, std::string_view{"input-mute on"}},
      std::pair{VoiceCommand::InputMuteOff, std::string_view{"input-mute off"}}
  };

  inline constexpr std::string_view voiceCommandName(VoiceCommand command) {
    for (const auto& [value, name] : voiceCommandNames)
      if (value == command)
        return name;
    return {};
  }
  inline constexpr std::optional<VoiceCommand> voiceCommandFromName(std::string_view name) {
    for (const auto& [command, value] : voiceCommandNames)
      if (value == name)
        return command;
    return {};
  }

  // Arguments exclude the executable and include the leading voice command.
  inline std::expected<VoiceCommand, std::string> parseVoiceCommand(std::span<const std::string_view> arguments) {
    constexpr std::size_t requiredArguments = 3;
    if (arguments.empty())
      return std::unexpected("Missing voice command. Use --help.");
    if (arguments.front() != "voice")
      return std::unexpected("Unknown command. Use --help.");
    if (arguments.size() < requiredArguments - 1)
      return std::unexpected("Missing voice action. Use input-mute.");
    if (arguments[1] == "push-to-talk")
      return std::unexpected("Push-to-talk voice commands are unavailable. Use the native shortcut.");
    if (arguments[1] != "input-mute")
      return std::unexpected("Unknown voice action. Use input-mute.");
    if (arguments.size() < requiredArguments)
      return std::unexpected("Missing input-mute value. Use toggle, on or off.");
    if (arguments.size() > requiredArguments)
      return std::unexpected("Too many voice command arguments.");
    if (arguments[2] == "toggle")
      return VoiceCommand::InputMuteToggle;
    if (arguments[2] == "on")
      return VoiceCommand::InputMuteOn;
    if (arguments[2] == "off")
      return VoiceCommand::InputMuteOff;
    return std::unexpected("Invalid input-mute value. Use toggle, on or off.");
  }
} // namespace app
