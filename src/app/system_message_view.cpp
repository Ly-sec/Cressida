#include "app/automod_message.h"
#include "app/client_application.h"
#include "app/discord_timestamp.h"
#include "app/message_time.h"
#include "app/system_message.h"
#include "app/theme.h"
#include "render/scene/image_node.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::messageTimestampHover(Node& parent, const MessageRun& run, float x, float y) {
    if (!run.style.timestamp
        || run.style.code
        || m_contextOpen
        || settingsVisible()
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || m_selectRequest
        || userAccountDialogOpen()
        || !m_searchInput.empty()
        || (!m_dialogKind.empty() && m_dialogKind != "profile"))
      return;
    float right = x + run.width, bottom = y + run.height;
    for (const auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      if (node->clipChildren()) {
        x = std::max(x, 0.0F);
        y = std::max(y, 0.0F);
        right = std::min(right, node->width());
        bottom = std::min(bottom, node->height());
      }
      x += node->x();
      y += node->y();
      right += node->x();
      bottom += node->y();
    }
    if (right > x && bottom > y && hovered(x, y, right - x, bottom - y)) {
      m_tooltip = timestampMentionTooltip(*run.style.timestamp, m_preferences.twelveHourClock);
      m_tooltipX = (x + right) / 2;
      m_tooltipY = y;
    }
  }
  float ClientApplication::timestampLabel(
      Node& parent, float x, float y, float width, const std::string& caption, std::string_view timestamp, float size,
      Color color
  ) {
    if (width <= 0 || caption.empty())
      return 0;
    const float height = label(parent, x, y, width, caption, size, color);
    if (m_contextOpen
        || !m_dialogKind.empty()
        || settingsVisible()
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || m_selectRequest
        || userAccountDialogOpen()
        || m_inboxOpen
        || !m_searchInput.empty())
      return height;
    float right = x + std::min(width, m_window.target().renderer().measureText(caption, size).width);
    float bottom = y + height;
    for (const auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      if (node->clipChildren()) {
        x = std::max(x, 0.0F);
        y = std::max(y, 0.0F);
        right = std::min(right, node->width());
        bottom = std::min(bottom, node->height());
      }
      x += node->x();
      y += node->y();
      right += node->x();
      bottom += node->y();
    }
    if (right > x && bottom > y && hovered(x, y, right - x, bottom - y)) {
      m_tooltip = formatMessageTimeTooltip(timestamp, m_preferences.twelveHourClock);
      m_tooltipX = (x + right) / 2;
      m_tooltipY = y;
    }
    return height;
  }
  void ClientApplication::demoTimestampStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto findText = [&](const std::string& caption) {
      const TextNode* result = nullptr;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text && static_cast<const TextNode&>(node).text() == caption)
          result = static_cast<const TextNode*>(&node);
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      return result;
    };
    const auto move = [&](float x, float y) {
      m_window.motion(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight);
    };
    const auto area = [&](auto predicate) {
      const auto found = std::ranges::find_if(m_textAreas, predicate);
      require(found != m_textAreas.end(), "Selectable message fragments retain native pointer bounds");
      return *found;
    };
    const auto name = [&] {
      return area([](const auto& text) { return text.message == "9401" && text.header && text.run.offset == 0; });
    };
    const auto time = [&] {
      return area([](const auto& text) { return text.message == "9401" && text.header && text.run.offset > 0; });
    };
    const auto body = [&](bool last) {
      std::vector<TextArea> fragments;
      for (const auto& text : m_textAreas)
        if (text.message == "9401" && !text.header)
          fragments.push_back(text);
      require(!fragments.empty(), "The message body retains selectable fragments");
      return last ? fragments.back() : fragments.front();
    };
    const auto press = [&](const TextArea& text, bool right = false) {
      const float x = text.x + (right ? text.run.width : 0);
      const float y = text.y + text.run.height / 2;
      move(x, y);
      m_window.click(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272);
    };
    const auto dragTo = [&](const TextArea& text) {
      move(text.x + text.run.width + Style::spaceXxs, text.y + text.run.height / 2);
      m_window.released();
      require(m_dialogKind.empty(), "Dragging selectable names never opens a profile");
    };
    const auto hoverCaption = [&](std::size_t index) {
      const auto caption = formatMessageTimestamp(m_messages[index].timestamp, m_preferences.twelveHourClock);
      const auto* text = findText(caption);
      require(text != nullptr, "Older messages paint a date and time beside the author or system notice");
      float x = 0, y = 0;
      for (const auto* node = static_cast<const Node*>(text); node && node != m_scene.get(); node = node->parent()) {
        x += node->x();
        y += node->y();
      }
      const auto metrics = m_window.target().renderer().measureText(caption, text->fontSize());
      move(x + Style::spaceXs, y + (metrics.inkTop + metrics.inkBottom) / 2);
    };
    const auto hoverContinuation = [&] {
      const auto row = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == Action::SelectMessage && hit.id == m_messages[1].id;
      });
      require(row != m_hits.end(), "The grouped follow-up keeps its own visible row");
      const auto& runs = m_messageLayouts.at(m_messages[1].id).layout.runs;
      require(!runs.empty(), "The grouped follow-up has a first text line");
      move(row->x - layout::Chat::authorGutter + Style::spaceXs, row->y + runs.front().height / 2);
    };
    const auto tooltip = [&](std::size_t index) {
      const auto caption = formatMessageTimeTooltip(m_messages[index].timestamp, m_preferences.twelveHourClock);
      if (m_tooltip != caption || !findText(caption))
        throw std::runtime_error(
            "Timestamp hover failed at replay step "
            + std::to_string(m_demoInteractionStep - 1)
            + " for item "
            + std::to_string(index)
            + ": "
            + m_tooltip
        );
      require(
          m_tooltipY >= m_messageViewportTop && m_tooltipY < m_messageViewportBottom,
          "Timestamp hover anchors stay inside the clipped conversation"
      );
    };
    constexpr std::int64_t mentionSeconds = 1793041200;
    constexpr std::size_t clippedParagraphs = 40;
    const auto mentionArea = [&](const char* message) {
      return area([&](const auto& text) {
        return text.message == message && !text.header && text.run.style.timestamp;
      });
    };
    const auto hoverArea = [&](const TextArea& text) {
      move(text.x + text.run.width / 2, text.y + text.run.height / 2);
    };
    const auto mentionTooltip = [&] {
      const auto caption = timestampMentionTooltip(mentionSeconds, m_preferences.twelveHourClock);
      require(
          m_tooltip == caption && findText(caption),
          "Inline timestamp hover paints the full local date and preferred time"
      );
    };
    const auto hoverProfileMention = [&] {
      const auto caption = discordTimestamp("t:1793041200:t");
      require(caption.has_value(), "The profile fixture contains a valid timestamp mention");
      const auto* text = findText(*caption);
      require(text != nullptr, "The profile paints its formatted timestamp mention");
      float x = 0, y = 0;
      for (const auto* node = static_cast<const Node*>(text); node && node != m_scene.get(); node = node->parent()) {
        x += node->x();
        y += node->y();
      }
      const auto metrics = m_window.target().renderer().measureText(*caption, text->fontSize());
      const float pointerY = y + (metrics.inkTop + metrics.inkBottom) / 2;
      move(x + metrics.width / 2, pointerY);
      return pointerY;
    };
    switch (m_demoInteractionStep++) {
    case 0:
      hoverCaption(0);
      break;
    case 1:
      tooltip(0);
      hoverContinuation();
      break;
    case 2:
      tooltip(1);
      m_preferences.twelveHourClock = true;
      break;
    case 3:
      hoverCaption(0);
      break;
    case 4:
      tooltip(0);
      hoverContinuation();
      break;
    case 5:
      tooltip(1);
      m_preferences.compact = true;
      m_messageLayouts.clear();
      break;
    case 6:
      hoverCaption(0);
      break;
    case 7:
      tooltip(0);
      hoverCaption(2);
      break;
    case 8:
      tooltip(2);
      m_inboxOpen = true;
      break;
    case 9:
      require(m_tooltip.empty(), "The inbox suppresses covered message timestamp tooltips");
      m_inboxOpen = false;
      move(Style::spaceXs, layout::Window::titlebarHeight + Style::spaceXs);
      break;
    case 10:
      require(m_tooltip.empty(), "Moving away clears the timestamp tooltip");
      press(name());
      require(m_dialogKind.empty(), "A name press defers its profile until release");
      dragTo(name());
      break;
    case 11:
      require(m_chatEdit.selection(m_selectedText) == name().run.text, "Dragging a name copies its complete text");
      press(time());
      dragTo(time());
      break;
    case 12:
      require(m_chatEdit.selection(m_selectedText) == time().run.text, "Dragging a timestamp copies its display text");
      press(name());
      dragTo(body(true));
      break;
    case 13:
      require(
          m_chatEdit.selection(m_selectedText)
              == name().run.text + " " + time().run.text + "\n" + m_messages.front().content,
          "One drag selects the author, timestamp and body without an invisible paragraph terminator"
      );
      press(body(true), true);
      move(name().x, name().y + name().run.height / 2);
      m_window.released();
      require(
          m_chatEdit.selection(m_selectedText)
              == name().run.text + " " + time().run.text + "\n" + m_messages.front().content,
          "Dragging backwards from the body through the header keeps the same copied text"
      );
      press(body(false));
      dragTo(body(true));
      break;
    case 14:
      require(
          m_chatEdit.selection(m_selectedText) == m_messages.front().content,
          "Starting in the body continues to copy only body text"
      );
      press(name());
      require(m_dialogKind.empty(), "A profile does not open on pointer press");
      m_window.released();
      break;
    case 15:
      require(m_dialogKind == "profile", "A simple name click still opens the profile on release");
      closeDialog();
      break;
    case 16:
      press(name());
      m_window.focusChanged(false);
      m_window.released();
      require(m_dialogKind.empty() && !m_dragMessage, "Focus loss cancels the pending profile gesture");
      m_window.focusChanged(true);
      m_messages.front().content = "Select this <#102> channel and the remaining words.";
      std::erase_if(m_channels, [](const auto& channel) { return channel.id == "102"; });
      m_channels.push_back(
          {.id = "102", .name = "🌙-chat", .type = 0, .overwrites = std::vector<api::PermissionOverwrite>{}}
      );
      m_guildChannels[m_guildId] = m_channels;
      m_roleDirectory.apply(
          {false,
           {{.guild = m_guildId,
             .roles = {{.id = m_guildId, .name = "@everyone", .permissions = std::to_string(api::viewChannel)}},
             .members = {{m_account.id, {}}}}}}
      );
      m_messageLayouts.clear();
      break;
    case 17: {
      const auto chip = area([](const auto& text) { return text.message == "9401" && text.run.channelChip; });
      press(chip);
      require(m_chatTextClick && m_chatTextClick->action == Action::OpenUrl, "Channel navigation waits for release");
      dragTo(chip);
      break;
    }
    case 18: {
      const auto chip = area([](const auto& text) { return text.message == "9401" && text.run.channelChip; });
      require(m_chatEdit.selection(m_selectedText) == chip.run.text, "A channel chip contributes its readable name");
      require(m_channelId != "102", "Dragging a channel chip never follows its link");
      press(body(false));
      dragTo(body(true));
      break;
    }
    case 19: {
      require(
          m_chatEdit.selection(m_selectedText) == "Select this #🌙-chat channel and the remaining words.",
          "Selection crosses channel mentions and spaces without losing surrounding text"
      );
      const auto chip = area([](const auto& text) { return text.message == "9401" && text.run.channelChip; });
      press(chip);
      m_window.released();
      break;
    }
    case 20:
      require(m_channelId == "102", "A simple channel chip click still navigates on release");
      m_messages = {
          {"9501", m_channelId, "Maya", "First selected message.", "2026-10-15T14:22:00Z", "11"},
          {"9502", m_channelId, "Noah", "Second selected message.", "2026-10-15T14:23:00Z", "12"}
      };
      m_messageLayouts.clear();
      break;
    case 21: {
      const auto first = area([](const auto& text) { return text.message == "9501" && !text.header; });
      const auto last = std::ranges::find_if(m_textAreas.rbegin(), m_textAreas.rend(), [](const auto& text) {
        return text.message == "9502" && !text.header;
      });
      require(last != m_textAreas.rend(), "Both messages expose selectable body fragments");
      press(first);
      dragTo(*last);
      break;
    }
    case 22:
      require(
          m_chatEdit.selection(
              m_selectedText
          ) == "First selected message.\n" + messageSelectionHeader(m_messages[1], false) + "Second selected message.",
          "One drag crosses message boundaries and includes the intervening author and time"
      );
      m_messages.push_back({"9503", m_channelId, "Maya", "A new arrival.", "2026-10-15T14:24:00Z", "11"});
      m_messageLayouts.clear();
      break;
    case 23:
      require(
          m_chatEdit.selection(m_selectedText).starts_with("First selected message.")
              && m_chatEdit.selection(m_selectedText).ends_with("Second selected message.")
              && !m_chatEdit.selection(m_selectedText).contains("A new arrival."),
          "Incoming messages preserve the existing selection endpoints"
      );
      m_window.key({.symbol = XKB_KEY_a, .control = true});
      break;
    case 24:
      require(
          m_chatEdit.selection(m_selectedText) == m_chatSelectionDocument.text
              && m_selectedText.contains("A new arrival."),
          "Select all copies the full loaded conversation after selecting chat text"
      );
      std::clog << "Offline message dates, header/channel selection and multi-message copying check passed\n";
      break;
    case 25:
      m_selectedMessage.clear();
      m_selectedText.clear();
      m_chatEdit = {};
      m_preferences.twelveHourClock = false;
      m_preferences.spoilerMode = 0;
      m_revealedSpoilers.erase("9604");
      m_messages = {
          {"9601", m_channelId, "Maya", "<t:1793041200:t>", "2026-10-15T14:22:00Z", "11"},
          {"9602", m_channelId, "Maya", "<t:1793041200:R>", "2026-10-15T14:23:00Z", "11"},
          {"9603", m_channelId, "Maya", "`<t:1793041200:t>`", "2026-10-15T14:24:00Z", "11"},
          {"9604", m_channelId, "Maya", "||<t:1793041200:t>||", "2026-10-15T14:25:00Z", "11"}
      };
      m_messageLayouts.clear();
      m_scrolling.stop(m_messageScroll);
      m_messageScroll = 0;
      move(Style::spaceXs, layout::Window::titlebarHeight + Style::spaceXs);
      break;
    case 26:
      hoverArea(mentionArea("9601"));
      break;
    case 27:
      mentionTooltip();
      m_preferences.twelveHourClock = true;
      hoverArea(mentionArea("9602"));
      break;
    case 28:
      mentionTooltip();
      hoverArea(area([](const auto& text) { return text.message == "9603" && text.run.style.code; }));
      break;
    case 29: {
      require(m_tooltip.empty(), "Timestamp syntax inside code has no date tooltip");
      const auto hidden = std::ranges::find_if(m_hits, [](const auto& hit) {
        return hit.action == Action::RevealSpoiler && hit.id == "9604";
      });
      require(hidden != m_hits.end(), "The concealed timestamp has a native spoiler target");
      move(hidden->x + hidden->width / 2, hidden->y + hidden->height / 2);
      break;
    }
    case 30:
      require(m_tooltip.empty(), "Concealed timestamp mentions never expose their date on hover");
      m_window.click(m_pointerX, m_pointerY + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272);
      m_window.released();
      break;
    case 31:
      require(spoilersVisible("9604"), "A native spoiler click reveals the timestamp mention");
      hoverArea(mentionArea("9604"));
      break;
    case 32:
      mentionTooltip();
      move(Style::spaceXs, layout::Window::titlebarHeight + Style::spaceXs);
      break;
    case 33:
      require(m_tooltip.empty(), "Leaving an inline timestamp clears its tooltip");
      showProfile("11");
      m_dialogAccount.bio = "Profile <t:1793041200:t>";
      m_profileCard.user = m_dialogAccount;
      break;
    case 34:
      hoverProfileMention();
      break;
    case 35:
      mentionTooltip();
      m_dialogAccount.bio.clear();
      for (std::size_t index = 0; index < clippedParagraphs; ++index)
        m_dialogAccount.bio += "Profile padding.\n\n";
      m_dialogAccount.bio += "<t:1793041200:t>";
      m_profileCard.user = m_dialogAccount;
      break;
    case 36:
      require(
          hoverProfileMention() >= m_dialogBounds.y + m_dialogBounds.height,
          "The collapsed profile fixture clips its final timestamp out of view"
      );
      break;
    case 37: {
      require(m_tooltip.empty(), "A timestamp beyond the profile clip has no hover tooltip");
      closeDialog();
      std::string content = "<t:1793041200:t>\n\n";
      for (std::size_t index = 0; index < clippedParagraphs; ++index)
        content += "Message padding.\n\n";
      content += "Visible message tail.";
      m_messages = {{"9701", m_channelId, "Maya", std::move(content), "2026-10-15T14:22:00Z", "11"}};
      m_messageLayouts.clear();
      m_scrolling.stop(m_messageScroll);
      m_messageScroll = 0;
      break;
    }
    case 38: {
      const auto clipped = mentionArea("9701");
      require(
          clipped.y + clipped.run.height <= m_messageViewportTop,
          "The scrolled conversation fixture clips its initial timestamp above the viewport"
      );
      hoverArea(clipped);
      break;
    }
    case 39:
      require(m_tooltip.empty(), "A timestamp outside the conversation viewport has no hover tooltip");
      std::clog << "Offline inline timestamp formatting, hover, spoiler and clipping check passed\n";
      break;
    }
    m_window.invalidate();
  }
  void ClientApplication::drawMessageStrikes(
      Node& parent, const MessageLayout& message, float x, float y, Color color, bool showSpoilers,
      std::optional<float> baseline, std::optional<float> height
  ) {
    for (const auto& area : messageStrikeAreas(message)) {
      const auto& style = message.runs[area.first].style;
      if (height && area.y + area.height > *height)
        continue;
      if (style.spoiler && !showSpoilers)
        continue;
      const auto metrics = m_window.target().renderer().measureText(
          messageMarkup("x", style, false), style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
          TextEllipsize::End, true
      );
      const float thickness = layout::Markdown::strikeThickness(style.size);
      const float center = (metrics.inkTop + metrics.inkBottom) / 2;
      const float top = y + baseline.value_or(area.y - metrics.top) + center - thickness / 2;
      rectangle(parent, x + area.x, top, area.width, thickness, style.muted ? theme::muted : color);
      parent.children().back()->setHitTestVisible(false);
    }
  }
  void ClientApplication::drawSystemNotice(
      Node& parent, const api::Message& message, const MessageLayout& layout, float y, float width
  ) {
    using namespace theme;
    const float absoluteX = conversationLeft() + 74, absoluteY = m_messageViewportTop + y;
    const bool over = hovered(conversationLeft() + Style::spaceXl, absoluteY, width + 50, layout.height);
    const auto& automod = layout.autoModeration;
    const bool pin = message.type == pinNoticeType;
    const auto pinLinkEnd = pin ? layout.plainText.find(" to this channel") : std::string::npos;
    glyph(
        parent, Style::spaceSm, y + Style::spaceXxs, Style::glyphSizeLarge,
        automod   ? "shield-check"
            : pin ? "pin"
                  : "arrow-right",
        automod   ? accent
            : pin ? muted
                  : success
    );
    if (automod) {
      avatar(
          parent, layout::Chat::authorGutter, y + automod->authorTop, layout::AutoModeration::avatarSize,
          message.avatar, userName(message.authorId, message.author), message.authorId
      );
    }
    m_messageText[message.id] = layout.plainText;
    const auto timestamp = formatMessageTimestamp(message.timestamp, m_preferences.twelveHourClock);
    const auto caption = timestamp + (message.deleted ? " · Deleted" : "");
    m_messageSelectionText[message.id] = layout.plainText + (caption.empty() ? "" : " " + caption);
    m_hits.push_back({absoluteX, absoluteY, width, layout.height, Action::SelectMessage, message.id});
    drawChatSelection(parent, layout, message.id, layout::Chat::authorGutter, y);
    if (automod) {
      Hit hit{
          absoluteX,
          absoluteY + automod->authorTop,
          layout::AutoModeration::avatarSize,
          layout::AutoModeration::avatarSize,
          Action::UserProfile,
          message.authorId
      };
      m_hits.push_back(hit);
      hit.action = Action::UserMenu;
      m_contextTargets.push_back(std::move(hit));
    }
    for (const auto& run : layout.runs) {
      const float x = 50 + run.x, top = y + run.y;
      if (automod
          && !automod->matched.empty()
          && run.offset < automod->contentEnd
          && run.offset + run.text.size() > automod->contentStart) {
        for (auto at = layout.plainText.find(automod->matched, automod->contentStart);
             at != std::string::npos && at + automod->matched.size() <= automod->contentEnd;
             at = layout.plainText.find(automod->matched, at + automod->matched.size())) {
          const auto from = std::max(at, run.offset),
                     to = std::min(at + automod->matched.size(), run.offset + run.text.size());
          if (to <= from || !run.image.empty())
            continue;
          const auto measure = [&](std::string_view text) {
            return m_window.target()
                .renderer()
                .measureText(
                    messageMarkup(text, run.style), run.style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
                    TextEllipsize::End, true
                )
                .width;
          };
          rectangle(
              parent, x + measure(std::string_view(run.text).substr(0, from - run.offset)), top,
              measure(std::string_view(run.text).substr(from - run.offset, to - from)), run.height,
              withAlpha(accent, Style::textHighlightAlpha), Style::radiusXs
          );
        }
      }
      chatSelectionText(parent, run, message.id, x, top, absoluteX + run.x, absoluteY + run.y);
      if (run.channelChip) {
        drawChannelLink(parent, run, message.id, x, top, absoluteX + run.x, absoluteY + run.y);
        continue;
      }
      if (!run.image.empty()) {
        if (!emojiImage(parent, x, top, run.width, run.image))
          label(parent, x, top, run.width, "…", run.style.size, foreground);
      } else if ((!automod && run.style.mention) || !run.style.userMention.empty())
        roleLabel(
            parent, x, top, run.width + Style::spaceXxs, run.text, run.style.size, foreground,
            run.style.bold ? FontWeight::Bold : FontWeight::Normal, automod ? run.style.userMention : message.authorId,
            "system:" + message.id, over,
            automod && run.style.userMention != message.authorId ? std::nullopt : message.memberRoles
        );
      else if (pin && run.style.bold) {
        const bool jump = run.offset < pinLinkEnd;
        const auto target = jump && message.reply && api::validSnowflake(message.reply->message)
            ? message.reply->message
            : std::string{};
        Hit hit{
            absoluteX + run.x,
            absoluteY + run.y,
            run.width,
            run.height,
            jump ? (target.empty() ? Action::None : Action::JumpReply) : Action::ChannelPins,
            jump ? target : "open"
        };
        const float bottom = std::min(m_messageViewportBottom, hit.y + hit.height);
        hit.y = std::max(m_messageViewportTop, hit.y);
        hit.height = std::max(0.0F, bottom - hit.y);
        const bool active = hit.action != Action::None;
        auto style = run.style;
        style.underline = active && hit.contains(m_pointerX, m_pointerY);
        label(
            parent, x, top, run.width + Style::spaceXxs, messageMarkup(run.text, style, false), run.style.size,
            active ? foreground : muted, FontWeight::Normal, 1, true
        );
        if (active)
          m_hits.push_back(std::move(hit));
      } else
        label(
            parent, x, top, run.width + Style::spaceXxs, messageMarkup(run.text, run.style, false), run.style.size,
            automod && !run.style.muted ? foreground : muted, FontWeight::Normal, 1, true
        );
      const auto& user = automod ? run.style.userMention : message.authorId;
      if ((run.style.mention || !run.style.userMention.empty()) && !user.empty()) {
        Hit hit{absoluteX + run.x, absoluteY + run.y, run.width, run.height, Action::UserProfile, user};
        const float bottom = std::min(m_messageViewportBottom, hit.y + hit.height);
        hit.y = std::max(m_messageViewportTop, hit.y);
        hit.height = std::max(0.0F, bottom - hit.y);
        m_hits.push_back(hit);
        hit.action = Action::UserMenu;
        m_contextTargets.push_back(std::move(hit));
      }
    }
    drawMessageStrikes(parent, layout, layout::Chat::authorGutter, y, automod ? foreground : muted);
    if (!layout.runs.empty() && !timestamp.empty()) {
      const auto headerEnd = automod
          ? std::ranges::find_if(layout.runs, [&](const auto& run) { return run.y >= automod->authorTop; })
          : layout.runs.end();
      const auto& last = *(headerEnd == layout.runs.begin() ? headerEnd : std::prev(headerEnd));
      const auto timestampWidth = m_window.target().renderer().measureText(caption, Style::fontSizeDetail).width;
      const float timestampX = layout::Chat::authorGutter + last.x + last.width + Style::spaceCompact;
      const float timestampY = y + last.y + Style::spaceXs;
      const float available =
          std::max(0.0F, std::min(timestampWidth + Style::spaceXxs, layout::Chat::authorGutter + width - timestampX));
      MessageRun time{
          .text = caption,
          .width = available,
          .style = {.size = Style::fontSizeDetail},
          .offset = layout.plainText.size() + 1,
          .height = Style::fontSizeDetail + Style::spaceXs
      };
      chatSelectionText(
          parent, time, message.id, timestampX, timestampY, absoluteX - layout::Chat::authorGutter + timestampX,
          absoluteY + last.y + Style::spaceXs, FontWeight::Normal, false, true
      );
      timestampLabel(
          parent, timestampX, timestampY, available, caption, message.timestamp, Style::fontSizeDetail,
          message.deleted ? theme::danger : muted
      );
    }
    if (automod) {
      const std::array<std::pair<std::string_view, Action>, 3> actions{
          {{"Actions", Action::AutoModActions},
           {"Jump to Surrounding", Action::AutoModJump},
           {"Report Issues", Action::AutoModReport}}
      };
      for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto& frame = automod->actions.controls[i];
        const auto& [caption, action] = actions[i];
        bool enabled = !message.deleted;
        if (action == Action::AutoModJump) {
          const auto target = autoModerationJump(message);
          enabled &= target && !channelMention(m_guildId, target->first).link.empty();
        } else if (action == Action::AutoModReport)
          enabled &= !autoModerationField(autoModerationEmbed(message), "decision_id").empty();
        const float localX = layout::Chat::authorGutter + frame.x;
        const float localY = y + automod->actionsTop + frame.y;
        const Hit hit{absoluteX + frame.x, absoluteY + automod->actionsTop + frame.y, frame.width,
                      frame.height,        enabled ? action : Action::None,           message.id};
        const bool hovered = enabled && hit.contains(m_pointerX, m_pointerY);
        if (hovered)
          rectangle(parent, localX, localY, frame.width, frame.height, raised, Style::controlRadius);
        const auto color = enabled ? accent : muted;
        float captionX = localX + layout::AutoModeration::actionPadding;
        if (i == 0) {
          glyph(
              parent, captionX, localY + (frame.height - layout::AutoModeration::actionGlyph) / 2,
              layout::AutoModeration::actionGlyph, "shield-check", color
          );
          captionX += layout::AutoModeration::actionGlyph + Style::spaceXs;
        }
        const auto metrics =
            m_window.target().renderer().measureText(std::string(caption), layout::AutoModeration::actionSize);
        label(
            parent, captionX, localY + (frame.height - (metrics.bottom - metrics.top)) / 2,
            frame.width - (captionX - localX) - layout::AutoModeration::actionPadding,
            messageMarkup(caption, {.underline = hovered}), layout::AutoModeration::actionSize, color,
            FontWeight::Normal, 1, true
        );
        m_hits.push_back(hit);
      }
    }
    reactionStrip(&parent, message, 50, y + std::max(22.0F, layout.height) + 6, width, true);
  }

  void ClientApplication::demoJoinStep() {
    const auto require = [](bool ok, const char* reason) {
      if (!ok)
        throw std::runtime_error(reason);
    };
    const auto step = m_demoInteractionStep++;
    int joinArrows = 0;
    const auto inspect = [&](auto&& self, const Node& node) -> void {
      if (node.type() == NodeType::Text) {
        const auto& text = static_cast<const TextNode&>(node);
        require(text.text() != "[System message]", "Join notices must not show the generic placeholder");
      }
      for (const auto& child : node.children())
        self(self, *child);
    };
    inspect(inspect, *m_scene);
    for (const auto& message : m_messages) {
      require(
          compactSystemNotice(message.type) && message.content.empty(),
          "System notices must preserve empty protocol content"
      );
      require(
          m_messageText.at(message.id) == systemNoticeText(message, userName(message.authorId, message.author)),
          "Selection/copy text must contain the generated notice and current display name"
      );
      const auto text = m_messageLayouts.at(message.id).layout.plainText;
      require(text.find("@") == std::string::npos, "Join names must not gain a mention prefix");
      if (m_messageTops.at(message.id) >= m_messageViewportTop)
        ++joinArrows;
    }
    require(joinArrows > 0, "Join notices must be visible");
    const auto pin = std::ranges::find(m_messages, std::string("9410"), &api::Message::id);
    require(pin != m_messages.end() && pin->reply && pin->reply->message == "9400", "Pin notices keep their target");
    require(
        m_messageText.at(pin->id).find("pinned a message to this channel. See all pinned messages.")
            != std::string::npos,
        "Pin notices use readable copy text"
    );
    if (m_messageTops.at(pin->id) >= m_messageViewportTop) {
      require(
          std::ranges::any_of(
              m_hits, [](const auto& hit) { return hit.action == Action::JumpReply && hit.id == "9400"; }
          ),
          "Pin notices link to the pinned message"
      );
      require(
          std::ranges::any_of(
              m_hits, [](const auto& hit) { return hit.action == Action::ChannelPins && hit.id == "open"; }
          ),
          "Pin notices open the pinned messages panel"
      );
    }
    const auto reaction =
        std::ranges::find_if(m_hits, [](const auto& hit) { return hit.action == Action::ToggleReaction; });
    require(step == 1 || reaction != m_hits.end(), "Join notices retain their normal reaction targets");
    const auto add = std::ranges::find_if(m_hits, [](const auto& hit) { return hit.action == Action::AddReaction; });
    require(step == 1 || add != m_hits.end(), "Join reaction rows expose the native add-reaction control");
    if (step == 0) {
      m_window.click(
          add->x + Style::spaceSm, add->y + Style::spaceSm + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          272
      );
      m_window.released();
      require(m_emojiMenu, "Join reaction buttons must open the real emoji picker");
    } else if (step == 1) {
      require(m_emojiMenu, "Join reaction picker survives the next frame");
      closeEmojiPicker();
      api::UserUpdates names;
      names.users["11"].nickname = "Local nickname <b>literal</b>";
      m_userDirectory.apply(names);
      m_messageLayouts.clear();
    } else if (step == 2) {
      require(
          m_messageText.at(m_messages.front().id).find("Local nickname <b>literal</b>") != std::string::npos,
          "Live personal nicknames update system notices without markup injection"
      );
      m_preferences.compact = true;
      m_preferences.channelSidebarWidth = 420;
      m_messageLayouts.clear();
    } else {
      require(
          m_messageText.at(m_messages.front().id).find("Local nickname") != std::string::npos,
          "Join notices survive compact mode and sidebar resizing"
      );
      std::clog << "Offline server join replay passed\n";
    }
    m_window.invalidate();
  }
  void ClientApplication::autoModerationAction(const Hit& hit) {
    const auto message = std::ranges::find(m_messages, hit.id, &api::Message::id);
    if (message == m_messages.end() || message->deleted || message->type != autoModerationActionType)
      return;
    if (hit.action == Action::AutoModActions) {
      auto target = hit;
      target.action = Action::UserMenu;
      target.id = message->authorId;
      m_contextTargets.push_back(target);
      openContext(target.x + target.width / 2, target.y + target.height / 2);
    } else if (hit.action == Action::AutoModJump) {
      const auto target = autoModerationJump(*message);
      if (target && !channelMention(m_guildId, target->first).link.empty())
        jumpToMessage(target->first, target->second);
    } else if (hit.action == Action::AutoModReport) {
      if (autoModerationField(autoModerationEmbed(*message), "decision_id").empty())
        return;
      closeContext();
      m_dialogKind = "confirm";
      m_dialogTitle = "Report an AutoMod issue?";
      m_dialogText.clear();
      m_dialogError.clear();
      m_dialogPending = false;
      m_dialogAction = api::Interaction{
          .kind = api::InteractionKind::AutoModerationFeedback,
          .channel = m_channelId,
          .message = message->id,
          .guild = m_guildId,
          .revision = ++m_dialogGeneration,
          .content = autoModerationEmbed(*message)->description,
          .customId = std::string(autoModerationField(autoModerationEmbed(*message), "decision_id")),
          .markAutoModerationFeedback = can::manageMessages(channelGrantFor(m_channelId))
      };
    }
    m_window.invalidate();
  }
  void ClientApplication::demoMarkdownStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto step = m_demoInteractionStep++;
    const auto& message = m_messageLayouts.at("9401").layout;
    const auto& quoted = m_messageLayouts.at("9400").layout;
    require(
        std::ranges::count_if(quoted.boxes, [](const auto& box) { return box.quote; }) == 2,
        "Explicitly quoted lines form two independent native rules"
    );
    const auto ordinary = std::ranges::find_if(quoted.runs, [](const auto& run) { return run.text == "Ordinary"; });
    require(
        ordinary != quoted.runs.end()
            && ordinary->x == 0
            && !ordinary->style.muted
            && quoted.runs.front().style.muted
            && quoted.boxes.front().y + quoted.boxes.front().height <= ordinary->y,
        "Native quote layout restores ordinary indentation and color after the quoted source line"
    );
    const auto links = messageLinkAreas(message);
    const auto spoilers = messageSpoilerAreas(message);
    const auto strikes = messageStrikeAreas(message);
    require(!links.empty() && !spoilers.empty() && !strikes.empty(), "The Markdown fixture contains all span types");
    const auto textArea = [&](std::size_t index) {
      const auto header = m_chatSelectionDocument.find("9401")->offset
          + m_messageSelectionText.at("9401").size()
          - message.plainText.size();
      const auto found = std::ranges::find_if(m_textAreas, [&](const auto& area) {
        return area.message == "9401" && !area.header && area.run.offset == header + message.runs[index].offset;
      });
      require(found != m_textAreas.end(), "The formatted text is mounted");
      return *found;
    };
    const auto inspect = [&](auto&& self, const Node& node, float x, float y, auto&& predicate) -> bool {
      x += node.x();
      y += node.y();
      if (predicate(node, x, y))
        return true;
      return std::ranges::any_of(node.children(), [&](const auto& child) {
        return self(self, *child, x, y, predicate);
      });
    };
    for (std::size_t i = 0; i < message.runs.size(); ++i) {
      const auto& run = message.runs[i];
      if (run.style.spoiler && !spoilersVisible("9401"))
        continue;
      auto style = run.style;
      style.linkHovered = step == 1 && style.link == "https://example.com/markdown";
      const auto markup = messageMarkup(run.text, style, false);
      require(
          inspect(
              inspect, *m_scene, 0, 0,
              [&](const Node& node, float, float) {
                return node.type() == NodeType::Text
                    && static_cast<const TextNode&>(node).text() == markup
                    && static_cast<const TextNode&>(node).fontSize() == run.style.size;
              }
          ),
          "Rendered Markdown retains its measured style and hover-only link underline"
      );
      if (run.style.muted)
        require(run.style.size < m_preferences.messageSize, "Wrapped subtext stays smaller than body text");
    }
    for (const auto& area : strikes) {
      const auto first = textArea(area.first);
      require(
          inspect(
              inspect, *m_scene, 0, 0,
              [&](const Node& node, float x, float y) {
                return node.type() == NodeType::Rect
                    && x == first.x
                    && node.width() == area.width
                    && node.height() == layout::Markdown::strikeThickness(first.run.style.size)
                    && y >= first.y
                    && y < first.y + first.run.height;
              }
          ),
          "The scene paints one uninterrupted strike line across each span, including spaces and nested emphasis"
      );
    }
    const auto reveal = std::ranges::find_if(m_hits, [](const auto& hit) {
      return hit.action == Action::RevealSpoiler && hit.id == "9401";
    });
    if (step == 0) {
      require(
          std::ranges::count_if(
              m_hits, [](const auto& hit) { return hit.action == Action::RevealSpoiler && hit.id == "9401"; }
          ) == std::ssize(spoilers),
          "Spoiler clicks cover complete line masks"
      );
      const auto gap = std::ranges::find_if(message.runs, [](const auto& run) {
        return run.text == " " && run.style.link == "https://example.com/markdown";
      });
      require(gap != message.runs.end(), "The link contains a styled whitespace run");
      const auto target = textArea(gap - message.runs.begin());
      m_window.motion(
          target.x + target.run.width / 2,
          target.y + target.run.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
    } else if (step == 1) {
      require(reveal != m_hits.end(), "The spoiler exposes a continuous click target");
      const float x = reveal->x + reveal->width / 2;
      const float y = reveal->y + reveal->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 1);
      m_window.released();
      m_window.motion(0, 0);
    } else if (step == 2) {
      require(
          spoilersVisible("9401") && reveal == m_hits.end(), "Clicking the spoiler reveals its whole formatted span"
      );
      m_preferences.messageSize = Style::fontSizeHeader;
      m_preferences.channelSidebarWidth = layout::Sidebar::maximumWidth - Style::spaceSection * 2;
      m_messageLayouts.clear();
      m_window.setUiScale(1.5F);
    } else if (step == markdownReplayFrames - 1)
      std::clog << "Offline Markdown hover, continuous spans and subtext replay passed\n";
    m_window.invalidate();
  }
  void ClientApplication::demoEmbedStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto step = m_demoInteractionStep++;
    std::clog << "Offline embed replay step " << step << '\n';
    if (step == embedReplayFrames - 1) {
      require(m_drafts[m_channelId] == "Retained embed removal draft", "Embed removal preserves the composer draft");
      std::clog << "Offline AutoMod, embed removal and continuous link hover replay passed\n";
      return;
    }
    const auto click = [&](Action action) {
      const auto found = std::ranges::find(m_hits, action, &Hit::action);
      require(found != m_hits.end() && found->height > 0, "The actual message action control is visible");
      const float x = found->x + found->width / 2;
      const float y = found->y + found->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 1);
      m_window.released();
    };
    if (step >= 11) {
      constexpr std::uint64_t retainedFlag = 1ULL << 12;
      constexpr std::uint64_t concurrentFlag = 1ULL << 14;
      if (step == 11) {
        require(m_channelId == "102", "Jump to Surrounding navigates to the cached source channel");
        m_window.setUiScale(m_demoOverlay.ends_with("narrow") ? 1.5F : 1.0F);
        selectChannel("101");
        api::Message message{
            .id = "9301",
            .channelId = m_channelId,
            .author = m_account.name,
            .content = "https://example.com/preview",
            .timestamp = "2026-10-20T10:45:00Z",
            .authorId = m_account.id
        };
        message.flags = retainedFlag;
        message.embeds = {
            {.title = "A removable link preview", .url = "https://example.com/preview"}, {.title = "A second preview"}
        };
        message.attachments.push_back({.id = "9302", .filename = "keep.txt", .url = "https://example.com/keep.txt"});
        m_messages = {message};
        m_channelMessages[m_channelId] = m_messages;
        m_messageLayouts.clear();
        m_messageScroll = 0;
        m_preferences.compact = false;
        m_preferences.logDeletedMessages = false;
        m_drafts[m_channelId] = "Retained embed removal draft";
      } else if (step == 12) {
        const auto target = std::ranges::find_if(m_contextTargets, [](const auto& hit) {
          return hit.action == Action::MessageMenu && hit.id == "9301";
        });
        require(target != m_contextTargets.end(), "The removable message has a native context target");
        m_window.motion(
            target->x + Style::spaceXl,
            target->y + Style::spaceSm + m_window.titlebarHeight() - layout::Window::titlebarHeight
        );
      } else if (step == 13) {
        require(
            std::ranges::count(m_hits, Action::RemoveEmbeds, &Hit::action) == 2,
            "Each embed exposes its own close control on message hover"
        );
        click(Action::RemoveEmbeds);
        require(
            m_dialogKind == "confirm" && m_dialogAction && m_dialogAction->kind == api::InteractionKind::RemoveEmbeds,
            "The close control opens embed removal confirmation"
        );
        require(
            !(m_messages.front().flags & api::messageSuppressEmbeds), "Opening confirmation does not remove embeds"
        );
      } else if (step == 14) {
        bool title = false, explanation = false, hint = false, remove = false;
        const auto inspect = [&](auto&& self, const Node& node) -> void {
          if (node.type() == NodeType::Text) {
            const auto& text = static_cast<const TextNode&>(node);
            title |= text.text() == "Are you sure?";
            explanation |= text.text() == "This will remove all embeds on this message for everyone.";
            hint |= text.text().contains("Hold Shift");
            remove |= text.text() == "Remove All Embeds";
          }
          for (const auto& child : node.children())
            self(self, *child);
        };
        inspect(inspect, *m_scene);
        require(
            title && explanation && hint && remove,
            "The confirmation paints the warning, Shift hint and explicit action"
        );
        require(
            m_dialogBounds.x >= 0 && m_dialogBounds.x + m_dialogBounds.width <= m_window.width(),
            "Embed confirmation stays inside the window"
        );
        click(Action::CloseDialog);
        require(
            m_dialogKind.empty() && !(m_messages.front().flags & api::messageSuppressEmbeds),
            "Cancel keeps all message embeds"
        );
        const auto target = std::ranges::find_if(m_contextTargets, [](const auto& hit) {
          return hit.action == Action::MessageMenu && hit.id == "9301";
        });
        require(target != m_contextTargets.end(), "The message remains available after cancel");
        openContext(target->x + target->width / 2, target->y + target->height / 2);
        require(
            std::ranges::any_of(
                m_contextEntries, [](const auto& entry) { return entry.id == static_cast<int>(Action::RemoveEmbeds); }
            ),
            "The message menu also offers embed removal"
        );
      } else if (step == 15) {
        contextAction(Action::RemoveEmbeds);
        require(m_dialogKind == "confirm", "The context menu uses the same confirmation");
      } else if (step == 16) {
        click(Action::SubmitDialog);
        require(m_status == "Demo mode is read-only.", "The offline confirmation never submits account requests");
        m_dialogPending = true;
        interactionResult(
            {.operation = api::Operation::Interaction,
             .error = "Offline embed removal failure",
             .interaction = *m_dialogAction}
        );
        require(
            !m_dialogPending && !m_dialogError.empty() && !(m_messages.front().flags & api::messageSuppressEmbeds),
            "A failed removal keeps the confirmation, error and original embeds"
        );
      } else if (step == 17) {
        updateMessage(
            m_channelId, "9301",
            api::MessagePatch{.content = "Newer message text", .flags = retainedFlag | concurrentFlag}
        );
        auto request = *m_dialogAction;
        const auto patch = api::removedEmbedsPatch(
            request,
            {{"id", "9301"},
             {"channel_id", m_channelId},
             {"flags", retainedFlag | api::messageSuppressEmbeds},
             {"content", "Old response text"},
             {"embeds", nlohmann::json::array()}}
        );
        require(patch.has_value(), "The synthetic response confirms the selected message");
        interactionResult({.operation = api::Operation::Interaction, .patch = *patch, .interaction = request});
        require(
            m_dialogKind.empty()
                && m_messages.front().content == "Newer message text"
                && m_messages.front().attachments.size() == 1
                && m_messages.front().flags == (retainedFlag | concurrentFlag | api::messageSuppressEmbeds),
            "Confirmed removal preserves concurrent edits, attachments and unrelated flags"
        );
      } else if (step == 18) {
        require(m_messageLayouts.at("9301").layout.embeds.empty(), "Confirmed removal hides every embed");
        require(
            std::ranges::none_of(m_hits, [](const auto& hit) { return hit.action == Action::RemoveEmbeds; }),
            "Suppressed previews no longer expose removal controls"
        );
        updateMessage(m_channelId, "9301", api::MessagePatch{.flags = retainedFlag});
        m_messages.front().authorId = "11";
        m_roleDirectory.apply({false, {{.guild = m_guildId, .owner = "12"}}});
      } else if (step == 19) {
        require(
            std::ranges::none_of(m_hits, [](const auto& hit) { return hit.action == Action::RemoveEmbeds; }),
            "Another author's embeds have no close control without permission"
        );
        removeMessageEmbeds("9301", false);
        require(m_dialogKind.empty(), "Unknown permissions cannot open a removal confirmation");
        m_messages.front().authorId = m_account.id;
        removeMessageEmbeds("9301", true);
        require(
            m_dialogKind.empty()
                && m_status == "Demo mode is read-only."
                && !(m_messages.front().flags & api::messageSuppressEmbeds),
            "The Shift path skips confirmation and still uses the normal request gate"
        );
        removeMessageEmbeds("9301", false);
      } else if (step == 20) {
        m_messages.front().authorId = "11";
        click(Action::SubmitDialog);
        require(
            !m_dialogPending && m_dialogError.contains("cannot remove"),
            "Submission rechecks author and permissions after the popup opens"
        );
      } else if (step == 21) {
        const auto request = *m_dialogAction;
        deleteMessages(m_channelId, {"9301"});
        require(m_dialogKind.empty(), "Live deletion closes the removal confirmation");
        interactionResult(
            {.operation = api::Operation::Interaction,
             .patch = api::MessagePatch{.flags = retainedFlag | api::messageSuppressEmbeds},
             .interaction = request}
        );
        require(m_messages.empty(), "A late removal response cannot recreate a deleted message");
      }
      m_window.invalidate();
      return;
    }
    const auto& alert = m_messageLayouts.at("9201").layout;
    require(
        alert.autoModeration
            && alert.plainText.starts_with("AutoMod")
            && !alert.plainText.contains("decision_id")
            && !alert.plainText.contains("opaque-decision"),
        "The parsed AutoMod fixture paints its dedicated notice instead of raw embed metadata"
    );
    if (step == 9) {
      require(
          alert.plainText.contains("Flagged content unavailable"),
          "Hiding embeds retains an explicit AutoMod summary without raw fields"
      );
      m_preferences.showEmbeds = true;
      m_messageLayouts.clear();
      m_window.setUiScale(1);
      m_messageScroll = 0;
      m_window.invalidate();
      return;
    }
    if (step == 10) {
      click(Action::AutoModJump);
      m_window.invalidate();
      return;
    }
    if (step == 3) {
      require(m_contextOpen && m_contextUser == "11", "Actions opens the affected user's native context menu");
      closeContext();
      m_window.invalidate();
      return;
    }
    if (step == 5 || step == 6) {
      require(
          m_dialogKind == "confirm"
              && m_dialogAction
              && m_dialogAction->kind == api::InteractionKind::AutoModerationFeedback,
          "Report Issues opens a native confirmation tied to the actual alert"
      );
      if (step == 5)
        click(Action::SubmitDialog);
      const auto request = *m_dialogAction;
      m_dialogPending = true;
      api::Event event{.operation = api::Operation::Interaction, .interaction = request};
      if (step == 5)
        event.error = "Offline simulated feedback failure";
      interactionResult(event);
      if (step == 5)
        require(
            !m_dialogKind.empty() && m_dialogError == event.error && !m_dialogPending,
            "Failed reports retain the confirmation and show a retryable error"
        );
      else {
        require(m_dialogKind.empty(), "Only a successful report acknowledgement closes the confirmation");
        m_window.motion(0, 0);
        m_window.setUiScale(1.5F);
      }
      m_window.invalidate();
      return;
    }
    const auto& preview = m_messageLayouts.at("9203").layout;
    require(preview.embedIcons.size() == 2, "Parsed author and footer icons share the embed layout");
    const auto& embed = m_messages.back().embeds.front();
    require(
        preview.plainText.contains(formatEmbedTimestamp(embed.timestamp, m_preferences.twelveHourClock)),
        "The rendered footer uses the embed timestamp and current clock choice"
    );
    if (step == 0) {
      for (const auto& icon : preview.embedIcons) {
        const auto texture = m_images.at(icon.url).texture.id;
        const auto mounted = [&](auto&& self, const Node& node) -> bool {
          if (node.type() == NodeType::Image) {
            const auto& image = static_cast<const ImageNode&>(node);
            if (image.textureId() == texture)
              return image.radius() > 0 && image.width() == icon.frame.width;
          }
          return std::ranges::any_of(node.children(), [&](const auto& child) { return self(self, *child); });
        };
        require(mounted(mounted, *m_scene), "Visible embed metadata artwork paints as small round images");
        require(
            std::ranges::none_of(
                m_hits, [&](const auto& hit) { return hit.action == Action::ViewImage && hit.id == icon.url; }
            ),
            "Metadata icons do not become media-viewer targets"
        );
      }
    }
    const auto areas = messageLinkAreas(preview);
    require(!areas.empty(), "The preview has a linked title");
    require(
        std::ranges::count_if(
            m_hits,
            [](const auto& hit) { return hit.action == Action::OpenUrl && hit.id == "https://example.com/preview"; }
        ) == std::ranges::count(areas, "https://example.com/preview", &MessageLinkArea::url),
        "Actual embed hit targets span complete rendered lines instead of individual words"
    );
    const auto hasText = [&](std::string_view expected, float size) {
      bool found = false;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text) {
          const auto& text = static_cast<const TextNode&>(node);
          found |= text.text() == expected && text.fontSize() == size;
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      return found;
    };
    for (const auto& run : preview.runs) {
      require(run.style.size <= layout::Embed::titleSize, "Chat font changes do not enlarge embed text");
      if (run.style.link.empty())
        continue;
      auto style = run.style;
      style.linkHovered = step == 1 && run.style.link == "https://example.com/preview";
      require(
          hasText(messageMarkup(run.text, style), run.style.size),
          "Rendered linked title glyphs underline together only while their logical link is hovered"
      );
    }
    if (step == 0) {
      const auto region = std::ranges::find_if(areas, [](const auto& area) {
        return area.url == "https://example.com/preview" && area.last > area.first;
      });
      require(region != areas.end(), "The fixture has multiple linked words on one line");
      const auto& first = preview.runs[region->first];
      const auto& second = preview.runs[region->first + 1];
      const auto header = m_chatSelectionDocument.find("9203")->offset
          + m_messageSelectionText.at("9203").size()
          - preview.plainText.size();
      const auto target = std::ranges::find_if(m_textAreas, [&](const auto& area) {
        return area.message == "9203" && !area.header && area.run.offset == header + first.offset;
      });
      require(target != m_textAreas.end(), "The native preview text is mounted");
      const float gapX = target->x + (first.width + second.x - first.x) / 2;
      const float gapY = target->y + first.height / 2;
      require(
          std::ranges::any_of(
              m_hits,
              [&](const auto& hit) {
                return hit.action == Action::OpenUrl && hit.id == region->url && hit.contains(gapX, gapY);
              }
          ),
          "Whitespace between linked words remains inside the actual click target"
      );
      m_window.motion(gapX, gapY + m_window.titlebarHeight() - layout::Window::titlebarHeight);
      m_preferences.twelveHourClock = true;
    } else if (step == 1) {
      m_window.motion(0, 0);
      m_preferences.messageSize = Style::fontSizePage;
      m_messageLayouts.clear();
    } else if (step == 2) {
      click(Action::AutoModActions);
    } else if (step == 4) {
      click(Action::AutoModReport);
    } else if (step == 7) {
      m_preferences.compact = true;
      m_messageLayouts.clear();
    } else if (step == 8) {
      m_preferences.showEmbeds = false;
      m_messageLayouts.clear();
    }
    m_window.invalidate();
  }
} // namespace app
