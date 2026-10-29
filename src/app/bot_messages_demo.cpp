#include "app/client_application.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace app {
  void ClientApplication::prepareBotMessagesDemo() {
    m_showMembers = false;
    m_preferences.logDeletedMessages = false;
    m_preferences.showRoleIcons = true;
    m_guildChannels[m_guildId] = m_channels;
    m_roleDirectory.apply(
        {false,
         {{.guild = m_guildId,
           .roles = {{.id = "501", .position = 1, .name = "Lunar", .unicodeEmoji = "🌙"}},
           .members = {{"30", {"501"}}},
           .owner = "99"}}}
    );
    m_messages = api::parseMessages(
        nlohmann::json::array(
            {{{"id", "9401"},
              {"channel_id", m_channelId},
              {"content", "A response from Orbit"},
              {"timestamp", "2026-10-29T12:00:00Z"},
              {"author", {{"id", "30"}, {"username", "Orbit"}, {"bot", true}}},
              {"member", {{"roles", {"501"}}}}}}
        )
    );
    m_channelMessages[m_channelId] = m_messages;
    m_messageLayouts.clear();
    m_drafts[m_channelId] = "Retained bot message draft";
    m_unreadTimeline.snapshot({});
    m_unreadTimeline.positioned();
  }
  void ClientApplication::demoBotMessagesStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto permission = [&](bool allowed) {
      m_roleDirectory.apply({false, {{.guild = m_guildId, .owner = allowed ? m_account.id : "99"}}});
    };
    const auto menu = [&] {
      const auto target = std::ranges::find_if(m_contextTargets, [](const auto& hit) {
        return hit.action == Action::MessageMenu && hit.id == "9401";
      });
      require(target != m_contextTargets.end(), "The app response has a native message context target");
      openContext(target->x + target->width / 2, target->y + target->height / 2);
    };
    const auto offersDelete = [&] {
      return std::ranges::any_of(m_contextEntries, [](const auto& entry) {
        return entry.id == static_cast<int>(Action::DeleteMessage);
      });
    };
    const auto click = [&](Action action) {
      const auto hit = std::ranges::find(m_hits, action, &Hit::action);
      require(hit != m_hits.end(), "The deletion confirmation exposes its native controls");
      m_window.click(
          hit->x + hit->width / 2,
          hit->y + hit->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272
      );
      m_window.released();
    };
    const auto countText = [&](const std::string& value) {
      unsigned count = 0;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text && static_cast<const TextNode&>(node).text() == value)
          ++count;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      return count;
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      const TextNode* badge = nullptr;
      const TextNode* role = nullptr;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text) {
          const auto& text = static_cast<const TextNode&>(node);
          if (text.text() == "APP")
            badge = &text;
          if (text.text() == "🌙")
            role = &text;
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(
          badge && role && badge->parent() == role->parent() && badge->x() > role->x(),
          "Bot authors show an APP chip after their role icon"
      );
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(badge, left, top, right, bottom);
      require(left >= chatLeft() && right <= m_window.width(), "The APP chip stays inside narrow message headers");
      menu();
      require(!offersDelete(), "Unknown moderation permissions do not offer deletion of bot messages");
      closeContext();
      permission(true);
      menu();
    } else if (step == 1) {
      require(offersDelete(), "Manage Messages offers deletion for another author's app response");
      contextAction(Action::DeleteMessage);
      require(
          m_dialogKind == "confirm" && m_dialogAction && !m_dialogPending,
          "Deleting an app response opens the existing confirmation without submitting"
      );
      m_preferences.showRoleIcons = false;
    } else if (step == 2) {
      require(
          countText("APP") == 1 && countText("🌙") == 0, "The APP chip remains visible when role icons are disabled"
      );
      click(Action::CloseDialog);
      require(m_dialogKind.empty() && m_messages.size() == 1, "Cancel leaves the bot response intact");
      m_contextChannel = m_channelId;
      m_contextMessage = "9401";
      contextAction(Action::DeleteMessage);
    } else if (step == 3) {
      permission(false);
      click(Action::SubmitDialog);
      require(
          !m_dialogPending && m_dialogError.contains("cannot delete") && m_messages.size() == 1,
          "Deletion rechecks permissions when confirmation is submitted"
      );
      permission(true);
      click(Action::SubmitDialog);
      require(
          !m_dialogPending && m_dialogError == "Demo mode is read-only.",
          "An allowed deletion reaches the existing mutation gate without sending in demo mode"
      );
      m_dialogPending = true;
      interactionResult(
          {.operation = api::Operation::Interaction,
           .error = "Synthetic deletion failure",
           .interaction = m_dialogAction}
      );
    } else if (step == 4) {
      require(
          !m_dialogPending && m_dialogError == "Synthetic deletion failure" && m_messages.size() == 1,
          "A failed deletion preserves the response and reports the error in confirmation"
      );
      m_dialogPending = true;
      interactionResult({.operation = api::Operation::Interaction, .interaction = m_dialogAction});
      require(
          m_messages.empty() && m_channelMessages[m_channelId].empty() && m_dialogKind.empty(),
          "Acknowledged deletion removes the bot response from the current and cached conversation"
      );
    } else if (step == 5) {
      permission(false);
      m_contextChannel = m_channelId;
      m_contextMessage = "9401";
      contextAction(Action::DeleteMessage);
      require(m_dialogKind.empty(), "A removed app response cannot reopen deletion confirmation");
      require(m_drafts[m_channelId] == "Retained bot message draft", "Bot message deletion retains the composer draft");
      auto own =
          api::Message{.id = "9402", .channelId = m_channelId, .content = "My message", .authorId = m_account.id};
      m_messages = {own};
      m_contextMessage = own.id;
      contextAction(Action::DeleteMessage);
      require(m_dialogKind == "confirm", "Authors can still delete their own messages without moderation permission");
    } else if (step == 6) {
      require(countText("APP") == 0, "Ordinary personal message headers do not acquire an APP chip");
      closeDialog();
      m_messages.front().flags = api::messageEphemeral;
      m_contextMessage = "9402";
      contextAction(Action::DeleteMessage);
      require(m_dialogKind.empty(), "Private app responses retain local dismissal instead of server deletion");
      m_messages.front().flags = 0;
    } else {
      require(
          m_drafts[m_channelId] == "Retained bot message draft" && m_pendingInteractions.empty(),
          "The replay completes without queued requests or lost drafts"
      );
    }
    m_window.invalidate();
  }
} // namespace app
