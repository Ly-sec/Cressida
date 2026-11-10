#include "app/client_application.h"
#include "app/theme.h"
#include "render/scene/image_node.h"
#include "render/scene/text_node.h"
#include "ui/controls/button.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::demoForwardStep() {
    const auto require = [](bool valid, const char* reason) {
      if (!valid)
        throw std::runtime_error(reason);
    };
    const auto click = [&](const std::string& id) {
      const auto found = m_widgets.find(id);
      require(found != m_widgets.end() && found->second->parent(), "Forward controls remain mounted across redraws");
      float left, top, right, bottom;
      Node::transformedBounds(found->second, left, top, right, bottom);
      m_window.motion((left + right) / 2, (top + bottom) / 2);
      m_window.click((left + right) / 2, (top + bottom) / 2, BTN_LEFT);
      m_window.released();
    };
    const auto step = m_demoInteractionStep++;
    std::clog << "Forward replay step " << step << '\n';
    const auto pending = [&] {
      return std::ranges::find_if(m_pendingSends, [](const auto& entry) { return entry.second.forward.has_value(); });
    };
    if (step == 0) {
      prepareEmojiPicker();
      const api::Emoji emoji{.id = "800000000000000000", .name = "umbriel", .guild = m_guildId, .animated = true};
      m_messages.front().content = api::emojiImageLink(emoji);
      m_guilds.front().icon = api::emojiPath(emoji.id, emoji.animated);
      m_channelCacheOrder = {"99001", "99100"};
      m_contextMessage = m_messages.front().id;
      m_contextChannel = m_channelId;
      contextAction(Action::ForwardMessage);
      require(
          m_forwardEditor && m_dialogKind == "forward", "The message Forward action opens the native destination dialog"
      );
    } else if (step == 1) {
      const auto savedUsage = m_emojiPreferences.frequentChannels;
      m_emojiPreferences.frequentChannels = {"99101", "99001", "99100"};
      require(
          forwardDestinations().front().channel == "99101",
          "Account channel usage takes precedence over local cache and message activity"
      );
      m_emojiPreferences.frequentChannels = savedUsage;
      require(
          forwardDestinations().front().channel == "99100",
          "Recent cached channels and conversations share one destination ordering"
      );
      bool previewImage = false;
      const auto inspect = [&](auto&& self, const Node& node, float y) -> void {
        y += node.y();
        if (node.type() == NodeType::Image
            && y >= m_dialogBounds.y + m_dialogBounds.height - layout::ForwardDialog::footer)
          previewImage = true;
        for (const auto& child : node.children())
          self(self, *child, y);
      };
      inspect(inspect, *m_scene, -m_scene->y());
      require(previewImage, "The source preview renders emoji image links through native image nodes");
      require(
          m_forwardEditor->selected.empty() && !static_cast<Button*>(m_widgets.at("forward:send"))->enabled(),
          "Forwarding always requires an explicit destination choice"
      );
      pasteControl("forward:search", m_guilds.front().name);
      float left, top, right, bottom;
      Node::transformedBounds(m_widgets.at("forward:destination:99100"), left, top, right, bottom);
      m_window.motion((left + right) / 2, (top + bottom) / 2);
    } else if (step == 2) {
      const auto* row = static_cast<Button*>(m_widgets.at("forward:destination:99100"));
      bool readableName = false, readableServer = false;
      for (const auto& child : row->parent()->children())
        if (child->type() == NodeType::Text) {
          const auto& text = static_cast<const TextNode&>(*child);
          if (text.text() == "destination-0")
            readableName = text.color() == theme::onHover;
          if (text.text() == m_guilds.front().name && std::abs(text.y() - row->y()) < layout::ForwardDialog::rowHeight)
            readableServer = text.color() == theme::onHover;
        }
      require(
          row->hovered() && readableName && readableServer,
          "Hovered destination names and server subtitles use the paired theme foreground"
      );
      float left, top, right, bottom;
      Node::transformedBounds(row, left, top, right, bottom);
      for (const auto& point :
           {std::pair{left + Style::spaceSm + layout::ForwardDialog::avatar / 2, (top + bottom) / 2},
            std::pair{
                left + layout::ForwardDialog::avatar + Style::spaceLg + Style::spaceXs,
                top + Style::spaceSm + Style::fontSizeBody / 2
            },
            std::pair{
                left + layout::ForwardDialog::avatar + Style::spaceLg + Style::spaceXs,
                top + Style::spaceSm + Style::spaceXl + Style::fontSizeDescription / 2
            },
            std::pair{right - Style::controlHeightSm / 2, (top + bottom) / 2}}) {
        m_window.motion(point.first, point.second);
        require(
            m_controlInput.inputAreaAt(point.first, point.second) == row->inputArea(),
            "The icon, title, subtitle and checkbox share one continuous native row target"
        );
      }
      const auto choices = forwardDestinations();
      require(
          !choices.empty()
              && std::ranges::all_of(choices, [&](const auto& destination) { return destination.guild == m_guildId; }),
          "Search narrows destinations by server name without opening another conversation"
      );
      m_inputs.at("forward:search")->setValue("");
      m_forwardEditor->query.clear();
    } else if (step == 3) {
      click("forward:check:99001");
      pasteControl("forward:comment", "A small note ");
      click("forward:emoji");
    } else if (step == 4) {
      require(
          m_emojiMenu && m_forwardEditor->emoji && m_inputs.at("emoji-search")->enabled(),
          "The comment opens the native searchable emoji picker above its dialog"
      );
      require(
          m_emojiBounds.y >= layout::Window::titlebarHeight
              && m_emojiBounds.x + m_emojiBounds.width <= m_window.width(),
          "The comment emoji picker fits inside the application scene"
      );
      pickEmoji("heart");
      require(
          m_forwardEditor->comment.find("❤") != std::string::npos && !m_emojiMenu,
          "Emoji selection inserts into the optional comment"
      );
      require(
          m_drafts[m_channelId] == "Keep the composer draft", "Comment emoji never changes the main composer draft"
      );
    } else if (step == 5) {
      click("forward:send");
      require(
          !m_forwardEditor && m_pendingSends.size() == 1 && pending() != m_pendingSends.end(),
          "Send queues one forward while deferring its optional comment"
      );
      require(
          !pending()->second.forwardComment.empty() && pending()->second.preview.forwarded,
          "Pending forwarding retains its snapshot and unsent comment"
      );
    } else if (step == 6) {
      auto it = pending();
      require(it != m_pendingSends.end(), "The forward remains pending");
      auto wrong = it->second.preview;
      wrong.id = "99500";
      wrong.forwarded->channel = "99999";
      confirmOutgoing(wrong);
      require(
          m_pendingSends.size() == 1 && pending()->second.confirmed.empty(),
          "A wrong source identity cannot deliver a forward or release its comment"
      );
      auto delivered = it->second.preview;
      delivered.id = "99501";
      confirmOutgoing(delivered);
      require(
          m_pendingSends.size() == 2, "A first validated confirmation releases the comment as a separate queued message"
      );
      confirmOutgoing(delivered);
      require(m_pendingSends.size() == 2, "Duplicate confirmations never send the comment twice");
    } else if (step == 7) {
      clearOutgoing();
      m_messages.front().content.clear();
      m_messages.front().attachments = {{.filename = "preview.png", .image = {m_guilds.front().icon, 48, 48}}};
      openForwardMessage(m_messages.front());
    } else if (step == 8) {
      click("forward:destination:99001");
      pasteControl("forward:comment", "Retain this note");
      focusControl("forward:comment");
      onKey({.symbol = XKB_KEY_Return});
      auto it = pending();
      require(it != m_pendingSends.end(), "The second selected destination queues a forward before failure injection");
      api::Event failed{
          .operation = api::Operation::Send,
          .resource = it->second.preview.channelId,
          .error = "Synthetic forward failure",
          .nonce = it->first
      };
      outgoingResult(failed);
      require(
          m_pendingSends.size() == 1
              && pending()->second.completed
              && pending()->second.forwardComment == "Retain this note",
          "A failed forward retains its unsent comment and never posts it alone"
      );
      const auto nonce = pending()->first;
      const auto old = m_channelId;
      m_channelId = "99001";
      restoreOutgoing(nonce);
      m_channelId = old;
      require(
          m_forwardEditor && m_forwardEditor->comment == "Retain this note" && m_pendingSends.empty(),
          "Explicit restoration reopens destination selection with the complete comment"
      );
    } else if (step == 9) {
      click("forward:clear");
      require(m_forwardEditor->selected.empty(), "Clearing selected destinations is explicit and keyboard accessible");
      click("forward:destination:99001");
      m_privateChannels.erase(std::ranges::find(m_privateChannels, std::string("99001"), &api::Channel::id));
      forwardAction("send");
      require(
          m_forwardEditor && !m_dialogError.empty() && m_pendingSends.empty(),
          "A stale destination grant cannot queue a forward"
      );
    } else if (step == 10) {
      closeDialog();
      openForwardMessage(m_messages.front());
    } else if (step == 11) {
      auto visits = std::exchange(m_channelCacheOrder, {});
      for (auto& channel : m_guildChannels.at(m_guildId)) {
        if (channel.id == "99100")
          channel.lastMessageId = "200";
        if (channel.id == "99101")
          channel.lastMessageId = "201";
      }
      const auto activity = forwardDestinations();
      require(
          std::ranges::find(activity, std::string("99101"), &ForwardDestination::channel)
              < std::ranges::find(activity, std::string("99100"), &ForwardDestination::channel),
          "Known latest message activity orders unvisited destinations across the picker"
      );
      m_channelCacheOrder = std::move(visits);
      const auto candidates = forwardDestinations();
      require(!candidates.empty(), "Cached writable server channels remain available");
      for (const auto& destination : candidates)
        if (m_forwardEditor->selected.size() < api::maximumForwardDestinations)
          forwardAction("select:" + destination.channel);
      require(
          m_forwardEditor->selected.size() <= api::maximumForwardDestinations,
          "Multi-destination forwarding stays bounded"
      );
      for (const auto& destination : candidates)
        if (!m_forwardEditor->selected.contains(destination.channel)) {
          forwardAction("select:" + destination.channel);
          break;
        }
      require(
          m_forwardEditor->selected.size() == api::maximumForwardDestinations && !m_dialogError.empty(),
          "A sixth destination cannot bypass the native selection limit"
      );
      closeDialog();
      openForwardMessage(m_messages.front());
    } else if (step == 12) {
      const auto candidates = forwardDestinations();
      forwardAction("select:" + candidates.front().channel);
      m_forwardEditor->comment = "Keep after cancellation";
      forwardAction("send");
      require(
          m_pendingSends.size() == 1 && pending() != m_pendingSends.end(),
          "A comment is optional and adds no extra message"
      );
      deleteMessages(m_channelId, {"98001"});
      require(
          pending()->second.revoked->load() && pending()->second.completed, "Source deletion revokes queued forwards"
      );
      auto late = pending()->second.preview;
      late.id = "99502";
      confirmOutgoing(late);
      require(
          m_pendingSends.size() == 1
              && m_pendingSends.begin()->second.completed
              && !m_pendingSends.begin()->second.forward
              && m_pendingSends.begin()->second.preview.content == "Keep after cancellation",
          "Late confirmation after revocation retains the comment without sending another request"
      );
    } else if (step == 13) {
      clearOutgoing();
      require(
          m_drafts[m_channelId] == "Keep the composer draft",
          "Forwarding and restoration preserve unrelated composer content"
      );
      m_messages = {{"98002", m_channelId, "Maya", "Another source message", "2026-11-10T10:00:00Z", "11"}};
      openForwardMessage(m_messages.front());
    } else if (step == 14) {
      click("forward:emoji");
    } else if (step == 15) {
      onKey({.symbol = XKB_KEY_Escape});
      require(!m_emojiMenu && m_forwardEditor, "Escape closes only the comment emoji picker first");
      onKey({.symbol = XKB_KEY_Escape});
      require(!m_forwardEditor && m_dialogKind.empty(), "A second Escape cancels forwarding without sending");
    } else if (step == 16) {
      auto unavailable = m_messages.front();
      unavailable.poll.emplace();
      openForwardMessage(unavailable);
      require(
          !m_forwardEditor && m_pendingSends.empty(), "Unsupported source kinds cannot open a stale forward action"
      );
    } else if (step == forwardReplayFrames - 1) {
      require(m_pendingSends.empty() && !m_emojiMenu, "The replay leaves no queued mutations or popup state");
      std::clog << "Offline native forwarding, comment emoji, confirmation, recovery and access replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
