#include "app/voice_command.h"

#include <initializer_list>
#include <iostream>
#include <stdexcept>

namespace {
  void require(bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  }
  auto parse(std::initializer_list<std::string_view> arguments) {
    return app::parseVoiceCommand({arguments.begin(), arguments.size()});
  }
} // namespace

int main() {
  try {
    using Command = app::VoiceCommand;
    for (const auto& [name, expected] :
         {std::pair{"input-mute toggle", Command::InputMuteToggle}, std::pair{"input-mute on", Command::InputMuteOn},
          std::pair{"input-mute off", Command::InputMuteOff}}) {
      const std::string_view words = name;
      const auto separator = words.find(' ');
      const auto result = parse({"voice", words.substr(0, separator), words.substr(separator + 1)});
      require(result && *result == expected, "Every supported command maps to its own typed action");
      require(
          app::voiceCommandName(*result) == words && app::voiceCommandFromName(words) == expected,
          "Canonical command names round-trip through the strict shared domain mapping"
      );
    }
    for (const auto arguments :
         {std::initializer_list<std::string_view>{},
          {"voice"},
          {"voice", "input-mute"},
          {"voice", "push-to-talk"},
          {"voice", "push-to-talk", "on"},
          {"voice", "push-to-talk", "off"},
          {"input-mute", "on"},
          {"voice", "unknown", "on"},
          {"voice", "", "on"},
          {"voice", "input-mute", ""},
          {"voice", "input-mute", "true"},
          {"voice", "input-mute", "1"},
          {"voice", "input-mute", "Toggle"},
          {"voice", "push-to-talk", "toggle"},
          {"voice", "push-to-talk", "ON"},
          {"voice", "push-to-talk", "true"},
          {"voice", "push-to-talk", "-1"},
          {"voice", "input-mute", "on", "off"},
          {"voice", "push-to-talk", "on", "--demo"},
          {"voice", "input-mute", "on", "voice", "push-to-talk", "off"},
          {"--new-instance", "voice", "input-mute", "on"},
          {"voice", "input-mute", " on"},
          {"voice", "input-mute", "off\n"},
          {"voice", "push-to-talk", std::string_view{"on\0off", 6}}}) {
      const auto result = app::parseVoiceCommand({arguments.begin(), arguments.size()});
      require(
          !result && !result.error().empty(), "Missing, extra and malformed arguments fail with editable diagnostics"
      );
    }
    for (const auto name :
         {"", "voice input-mute on", "input-mute true", "input-mute on ", "input-mute  on", "push-to-talk toggle",
          "push-to-talk on", "push-to-talk off", "push-to-talk ON", "push-to-talk on\noff"})
      require(!app::voiceCommandFromName(name), "IPC names accept only exact supported canonical actions");
    for (const auto invalid : {std::uint8_t{0}, std::uint8_t{4}, std::uint8_t{5}, std::uint8_t{255}})
      require(
          app::voiceCommandName(static_cast<Command>(invalid)).empty(),
          "Invalid command values cannot acquire a valid wire name"
      );
    const auto missing = parse({"voice", "input-mute"});
    const auto extra = parse({"voice", "input-mute", "on", "extra"});
    const auto unknown = parse({"voice", "unknown", "on"});
    require(
        missing.error() != extra.error() && unknown.error() != missing.error(),
        "Missing values, extra arguments and unknown actions have distinct explanations"
    );
    const auto removed = parse({"voice", "push-to-talk", "on"});
    require(
        !removed && removed.error().find("native shortcut") != std::string::npos,
        "Removed push-to-talk commands direct users to the existing native shortcut"
    );
    std::cout << "Voice command parsing, typed actions and strict canonical names passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
