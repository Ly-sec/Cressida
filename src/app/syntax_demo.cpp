#include "app/client_application.h"
#include "app/message_layout.h"
#include "app/theme.h"
#include "render/scene/rect_node.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace app {
  namespace {
    constexpr std::string_view terminalSyntaxBody =
        "❯ sudo dnf in example-devel                                                     2.4s\n"
        "Updating and loading repositories:\n"
        "Repositories loaded.\n"
        "Failed to resolve the transaction:\n"
        "No match for argument: example-devel\n"
        "You can try to add to command line:\n"
        "  --skip-unavailable to skip unavailable packages\n"
        "user@workstation ~ [1]\n"
        "❯                                                                               1.2s";

    constexpr std::string_view nixSyntaxBody = R"NIX(```nix
{
  description = "My NixOS config";
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    nur.url = "github:nix-community/NUR";
    home-manager = {
      url = "github:nix-community/home-manager";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };
  # An indented string with escaped and real interpolation.
  script = ''
    echo ''${PATH}
    echo ${if enabled then "yes" else "no"}
  '';
}
```)NIX";
  } // namespace

  bool ClientApplication::isSyntaxDemoView(std::string_view view) {
    return view == "syntax"
        || view == "syntax-light"
        || view == "syntax-compact"
        || view == "syntax-terminal"
        || view == "syntax-terminal-light"
        || view == "syntax-nix"
        || view == "syntax-nix-light"
        || view == "syntax-nix-compact"
        || view.starts_with("syntax-nix-flat-rail");
  }

  void ClientApplication::prepareSyntaxDemo(std::string_view view) {
    m_demoOverlay = std::string(view);
    m_showMembers = false;
    m_preferences.compact = view.ends_with("-compact");
    if (view.ends_with("-light"))
      m_preferences.theme = "light";
    if (view.starts_with("syntax-nix-flat-rail")) {
      auto& palette = m_themeSnapshot.themes.front();
      palette.dark.rail = palette.dark.background;
      palette.light.rail = palette.light.background;
    }
    m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
      {"id":"9701","author":{"id":"11","username":"Lysec"},"timestamp":"2026-10-18T11:26:00Z","content":"```cpp\n// Keep the original source when copying.\nconstexpr int count = 42;\nif (count > 0) {\n  print(\"Hello, world!\");\n}\n```"},
      {"id":"9702","author":{"id":"12","username":"Maya"},"timestamp":"2026-10-18T11:27:00Z","content":"```python\ndef greet(name):\n    # Comments and strings stay separate.\n    return \"Hello, \" + name\n\nprint(greet(\"Cressida\"))\n```"},
      {"id":"9703","author":{"id":"12","username":"Maya"},"timestamp":"2026-10-18T11:28:00Z","content":"```json\n{\n  \"enabled\": true, \"count\": 12,\n  \"label\": \"Syntax highlighting\"\n}\n```"}
    ])JSON"));
    if (view.starts_with("syntax-nix")) {
      m_messages.resize(1);
      m_messages.front().content = std::string(nixSyntaxBody);
    }
    if (view.starts_with("syntax-terminal"))
      m_messages = api::parseMessages(
          nlohmann::json::array(
              {{{"id", "9701"},
                {"author", {{"id", "11"}, {"username", "Maya"}}},
                {"timestamp", "2026-10-18T11:26:00Z"},
                {"content",
                 "The terminal output:\n```"
                     + std::string(terminalSyntaxBody)
                     + "```\nThe command output remains complete."}},
               {{"id", "9702"},
                {"author", {{"id", "12"}, {"username", "Alex"}}},
                {"timestamp", "2026-10-18T11:27:00Z"},
                {"content", "The following message remains readable too."}}}
          )
      );
  }

  void ClientApplication::demoSyntaxCheck() {
    if (m_demoOverlay.starts_with("syntax-nix-flat-rail")) {
      const auto& boxes = m_messageLayouts.at(m_messages.front().id).layout.boxes;
      const auto code = std::ranges::find_if(boxes, [](const auto& box) { return !box.code.empty(); });
      if (code == boxes.end() || theme::panel == theme::background || theme::railBackground != theme::background)
        throw std::runtime_error("Code surface fixture requires a fence and matching rail/chat colors");
      int surfaces = 0;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Rect
            && node.width() == code->width
            && node.height() == code->height
            && static_cast<const RectNode&>(node).style().fill == theme::panel)
          ++surfaces;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      if (surfaces != 1)
        throw std::runtime_error("Fenced code must retain its surface when rail and chat colors match");
      std::clog << "Offline code surface check passed\n";
    }
    if (m_demoOverlay.starts_with("syntax-terminal") && m_demoInteractionStep == 0) {
      const auto& message = m_messages.front();
      const auto& layout = m_messageLayouts.at(message.id).layout;
      const auto code = std::ranges::find_if(layout.boxes, [](const auto& box) { return !box.code.empty(); });
      if (code == layout.boxes.end()
          || code->code != terminalSyntaxBody
          || !layout.plainText.contains(terminalSyntaxBody)
          || !layout.plainText.contains("The terminal output:")
          || !layout.plainText.contains("The command output remains complete.")
          || !m_messageText.at(m_messages.back().id).contains("The following message remains readable too."))
        throw std::runtime_error("Terminal fences must preserve commands, output and surrounding messages");
      std::vector<std::string> fragments;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (const auto* text = dynamic_cast<const TextNode*>(&node); text && text->useMarkup())
          fragments.push_back(text->text());
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      for (const auto& run : layout.runs)
        if (layout.plainText.substr(run.offset, run.text.size()) != run.text
            || std::ranges::find(fragments, messageMarkup(run.text, run.style, false)) == fragments.end())
          throw std::runtime_error("Terminal text must retain native fragments and selection byte offsets");
      ++m_demoInteractionStep;
      std::clog << "Offline terminal fence check passed\n";
    }
  }
} // namespace app
