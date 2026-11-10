#include "app/client_application.h"
#include "app/theme.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::chatEmojiTarget(const MessageRun& run, float x, float y) {
    if (!run.style.emoji || run.style.code || !run.style.link.empty())
      return;
    const auto emoji = m_emojiCatalog.describe(run.text);
    if (!emoji)
      return;
    const float top = std::max(y, m_messageViewportTop);
    const float bottom = std::min(y + run.height, m_messageViewportBottom);
    if (bottom <= top)
      return;
    const auto key = emoji->id.empty() ? emoji->unicode : emoji->id;
    m_chatEmojiTargets[key] = *emoji;
    m_hits.push_back({x, top, run.width, bottom - top, Action::EmojiInfo, key});
  }

  void ClientApplication::inspectEmoji(const Hit& hit) {
    if (m_forwardEditor)
      return;
    const auto found = m_chatEmojiTargets.find(hit.id);
    if (found == m_chatEmojiTargets.end())
      return;
    auto emoji = found->second;
    closeGifPicker();
    closeEmojiPicker();
    closeContext();
    closeCompletion();
    m_inspectedEmoji = std::move(emoji);
    m_expressionAnchor = {hit.x, hit.y, hit.width, hit.height};
    m_emojiMenu = m_emojiOpened = true;
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_scrolling.stopAll();
  }

  void ClientApplication::expressionEmojiImage(Node& root, const api::Emoji& emoji, const layout::Frame& frame) {
    if (emoji.id.empty())
      label(
          root, frame.x, frame.y, frame.width, emoji.unicode,
          frame.height * Style::fontSizeBody / Style::glyphSizeMedium, theme::foreground
      );
    else if (m_preferences.showImages)
      emojiImage(root, frame.x, frame.y, frame.width, api::emojiPath(emoji.id, emoji.animated));
  }

  void ClientApplication::drawEmojiInfo(Node& root, float w, float h) {
    using E = layout::Expression;
    const auto& emoji = *m_inspectedEmoji;
    const auto initial = layout::expressionFrame(m_expressionAnchor, w, h, true);
    const float nameWidth = initial.width - E::padding * 2 - E::detailPreview - E::gap - Style::controlHeightSm;
    const auto name = ":" + emoji.name + ":";
    const auto metrics =
        m_window.target().renderer().measureText(name, Style::fontSizeBody, FontWeight::SemiBold, nameWidth, 0);
    const float nameHeight = std::ceil(metrics.bottom - metrics.top);
    const auto card =
        layout::expressionFrame(m_expressionAnchor, w, h, true, nameHeight + E::gap + Style::fontSizeDescription * 2);
    m_emojiBounds = {card.x, card.y, card.width, card.height};
    m_hits.push_back(
        {0, layout::Window::titlebarHeight, w, h - layout::Window::titlebarHeight, Action::CloseEmoji, {}}
    );
    m_hits.push_back(m_emojiBounds);
    rectangle(root, card.x, card.y, card.width, card.height, theme::raised, Style::radiusMd);
    const layout::Frame preview{card.x + E::padding, card.y + E::padding, E::detailPreview, E::detailPreview};
    expressionEmojiImage(root, emoji, preview);
    const float textX = preview.x + preview.width + E::gap;
    const float textWidth = card.x + card.width - E::padding - textX;
    label(root, textX, preview.y, nameWidth, name, Style::fontSizeBody, theme::foreground, FontWeight::SemiBold, 0);
    label(
        root, textX, preview.y + nameHeight + E::gap, textWidth,
        emoji.id.empty()     ? "Unicode emoji"
            : emoji.animated ? "Animated custom emoji"
                             : "Custom emoji",
        Style::fontSizeDescription, theme::muted, FontWeight::Normal, 2
    );
    iconButton(
        root,
        {card.x + card.width - E::padding - Style::controlHeightSm,
         card.y + E::padding,
         Style::controlHeightSm,
         Style::controlHeightSm,
         Action::CloseEmoji,
         {}},
        "x", "Close emoji details"
    );
    const float footerY = card.y + card.height - E::padding - Style::controlHeightSm;
    const auto guild = std::ranges::find(m_guilds, emoji.guild, &api::Guild::id);
    if (!emoji.guild.empty() && guild != m_guilds.end())
      button(
          root,
          {card.x + E::padding, footerY, card.width - E::padding * 2, Style::controlHeightSm, Action::Guild, guild->id},
          "View Server: " + guild->name
      );
    else
      label(
          root, card.x + E::padding, footerY, card.width - E::padding * 2,
          emoji.id.empty() ? "Built-in emoji" : "Source server unavailable", Style::fontSizeDescription, theme::muted
      );
  }

  bool ClientApplication::drawExpressionTooltip(Node& root, float w, float h) {
    if (!m_expressionFocused
        || (!m_window.active() && !m_demo)
        || m_dragDraft
        || m_dragImage
        || m_resizingSidebar
        || m_railDrag
        || m_controlInput.pointerCaptured()
        || m_dragMessage
        || settingsVisible()
        || !m_dialogKind.empty()
        || m_contextOpen
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || m_inboxOpen
        || m_shareOpen
        || m_videoExpanded
        || !m_viewImage.empty()
        || userAccountDialogOpen()
        || m_selectRequest
        || m_completionQuery
        || m_fontPickerOpen
        || !m_searchSuggestions.empty())
      return false;
    if (const auto preview = sharingPreviewFrame(); preview && preview->card.contains(m_pointerX, m_pointerY))
      return false;
    const auto hit = std::find_if(m_hits.rbegin(), m_hits.rend(), [&](const auto& target) {
      return target.contains(m_pointerX, m_pointerY);
    });
    if (hit == m_hits.rend())
      return false;
    const auto emoji = m_chatEmojiTargets.find(hit->id);
    const auto sticker = m_stickerTargets.find(hit->id);
    const bool isEmoji = hit->action == Action::EmojiInfo && emoji != m_chatEmojiTargets.end();
    if (!isEmoji && !(hit->action == Action::StickerInfo && sticker != m_stickerTargets.end()))
      return false;
    using E = layout::Expression;
    const layout::Frame anchor{hit->x, hit->y, hit->width, hit->height};
    const auto initial = layout::expressionFrame(anchor, w, h, false);
    const float textWidth = initial.width - E::padding * 2 - E::preview - E::gap;
    const auto name = isEmoji ? ":" + emoji->second.name + ":" : sticker->second.name;
    const auto metrics =
        m_window.target().renderer().measureText(name, Style::fontSizeBody, FontWeight::SemiBold, textWidth, 0);
    const float nameHeight = std::ceil(metrics.bottom - metrics.top);
    const auto card =
        layout::expressionFrame(anchor, w, h, false, nameHeight + E::gap + Style::fontSizeDescription + Style::spaceXs);
    rectangle(root, card.x, card.y, card.width, card.height, theme::raised, Style::radiusMd);
    const layout::Frame preview{card.x + E::padding, card.y + E::padding, E::preview, E::preview};
    if (isEmoji)
      expressionEmojiImage(root, emoji->second, preview);
    else if (m_preferences.showImages)
      picture(
          root, preview.x, preview.y, preview.width, preview.height, api::stickerAsset(sticker->second), 0, false,
          false, ImageFitMode::Contain
      );
    const float textX = preview.x + preview.width + E::gap;

    label(root, textX, preview.y, textWidth, name, Style::fontSizeBody, theme::foreground, FontWeight::SemiBold, 0);
    label(
        root, textX, preview.y + nameHeight + E::gap, textWidth, "Click to learn more", Style::fontSizeDescription,
        theme::muted
    );
    m_expressionTooltipVisible = true;
    return true;
  }

  void ClientApplication::prepareExpressionDemo() {
    prepareEmojiPicker();
    prepareStickers();
    m_showMembers = false;
    const auto* emoji = m_emojiCatalog.find(m_emojiCatalog.guild(m_guilds.front().id).front());
    m_messages = {
        {"89001", m_channelId, "Maya", "Names " + emoji->token() + " 👍🏽 and ||<:hidden:899999999999999998>||",
         "2026-11-02T14:00:00Z", "11"},
        {"89002", m_channelId, "Maya", "<:outside:899999999999999999>", "2026-11-02T14:01:00Z", "11"},
        {"89003", m_channelId, "Maya", "A sticker", "2026-11-02T14:02:00Z", "11"}
    };
    m_messages.back().stickers = {m_stickers.at(m_guilds.front().id).front()};
    m_history.restoreLatest(m_messages, true);
  }

  void ClientApplication::demoExpressionStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto find = [&](Action action, const std::string& id = {}) {
      const auto found = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == action && (id.empty() || hit.id == id);
      });
      require(found != m_hits.end(), "Expression replay target must be visible");
      return *found;
    };
    const auto move = [&](const Hit& hit) {
      m_window.motion(
          hit.x + hit.width / 2, hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
    };
    const auto click = [&](const Hit& hit) {
      m_window.click(
          hit.x + hit.width / 2, hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          272
      );
      m_window.released();
    };
    const auto known = m_emojiCatalog.guild(m_guilds.front().id).front();
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(
          m_chatEmojiTargets.contains(known) && m_chatEmojiTargets.contains("👍🏽"),
          "Custom and whole-grapheme Unicode emoji expose information targets"
      );
      require(!m_chatEmojiTargets.contains("899999999999999998"), "Concealed spoilers do not expose emoji names");
      for (const auto& hit : m_hits)
        if (hit.action == Action::EmojiInfo || hit.action == Action::StickerInfo)
          require(
              hit.y >= m_messageViewportTop && hit.y + hit.height <= m_messageViewportBottom,
              "Expression targets remain inside the visible conversation"
          );
      move(find(Action::EmojiInfo, known));
    } else if (step == 1) {
      require(m_expressionTooltipVisible, "Hovering a custom emoji shows its native name preview");
      m_window.pointerLeft();
    } else if (step == 2) {
      require(!m_expressionTooltipVisible, "Leaving an emoji closes its tooltip immediately");
      click(find(Action::EmojiInfo, known));
    } else if (step == 3) {
      require(
          m_inspectedEmoji && m_inspectedEmoji->guild == m_guilds.front().id && !m_expressionTooltipVisible,
          "A real pointer click opens known emoji details and suppresses the hover preview"
      );
      const auto card = m_emojiBounds;
      require(
          card.x >= 0
              && card.y >= layout::Window::titlebarHeight
              && card.x + card.width <= m_window.width()
              && card.y + card.height <= m_window.height(),
          "The anchored details card stays within the application"
      );
      const auto channel = m_channelId;
      click({Style::spaceXs, layout::Window::titlebarHeight + Style::spaceXs, Style::spaceXs, Style::spaceXs});
      require(!m_emojiMenu && m_channelId == channel, "Outside click dismisses details without activating the rail");
    } else if (step == 4) {
      move(find(Action::EmojiInfo, "👍🏽"));
    } else if (step == 5) {
      require(m_expressionTooltipVisible, "Unicode emoji names are available without opening the picker");
      click(find(Action::EmojiInfo, "👍🏽"));
    } else if (step == 6) {
      require(
          m_inspectedEmoji && m_inspectedEmoji->unicode == "👍🏽" && m_inspectedEmoji->id.empty(),
          "Unicode details retain the exact skin-tone grapheme"
      );
      onKey({.symbol = XKB_KEY_Escape});
      require(!m_emojiMenu, "Escape closes emoji details");
    } else if (step == 7) {
      click(find(Action::EmojiInfo, "899999999999999999"));
    } else if (step == 8) {
      require(
          m_inspectedEmoji && m_inspectedEmoji->name == "outside" && m_inspectedEmoji->guild.empty(),
          "An uncached custom emoji retains its supplied name without a fabricated source"
      );
      require(
          std::ranges::none_of(
              m_hits,
              [&](const auto& hit) { return hit.action == Action::Guild && m_emojiBounds.contains(hit.x, hit.y); }
          ),
          "Unknown source details do not expose an unrelated server action"
      );
      onKey({.symbol = XKB_KEY_Escape});
    } else if (step == 9) {
      move(find(Action::StickerInfo));
    } else if (step == 10) {
      require(m_expressionTooltipVisible, "Hovering a sticker shows its name and image");
      click(find(Action::StickerInfo));
    } else if (step == 11) {
      require(
          m_stickerInfo && m_inspectedSticker && m_inspectedSticker->name == "Hello",
          "Sticker clicks retain the existing native details card"
      );
      onKey({.symbol = XKB_KEY_Escape});
    } else if (step == 12) {
      const auto hit = find(Action::EmojiInfo, known);
      m_window.click(
          hit.x + hit.width / 2, hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          272
      );
      require(!m_inspectedEmoji, "Emoji activation waits for pointer release");
      m_window.motion(
          hit.x + hit.width + Style::dragStartThreshold * 2,
          hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      m_window.released();
      require(
          !m_inspectedEmoji && !m_emojiMenu && m_chatEdit.selected(),
          "Dragging across an emoji selects chat text instead of opening details"
      );
    } else if (step == 13) {
      move(find(Action::EmojiInfo, known));
      windowFocusChanged(false);
    } else if (step == 14) {
      require(!m_expressionTooltipVisible, "Unfocused chat does not retain an expression tooltip");
      windowFocusChanged(true);
      move(find(Action::EmojiInfo, known));
    } else if (step == 15) {
      require(m_expressionTooltipVisible, "Expression hover resumes after focus returns");
      m_window.pointerLeft();
    }
    std::clog << "Offline expression replay step " << step + 1 << " passed\n";
    m_window.invalidate();
  }
} // namespace app
