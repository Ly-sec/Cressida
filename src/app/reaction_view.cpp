#include "app/client_application.h"
#include "app/interaction_key.h"
#include "app/theme.h"
#include "core/input/key_modifiers.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    constexpr std::size_t reactorCacheLimit = 128;
    constexpr std::size_t reactorHoverNames = 3;
    constexpr auto reactorReuse = std::chrono::seconds(30);
    constexpr auto reactorHoverDelay = std::chrono::milliseconds(200);
    constexpr auto reactorRetryDelay = std::chrono::milliseconds(1000);
    constexpr auto reactorRefreshDelay = std::chrono::milliseconds(250);
    int normalReactions(const api::Reaction& reaction) { return std::max(0, reaction.count - reaction.burstCount); }
  } // namespace

  const api::Message* ClientApplication::reactionBrowserMessage() const {
    if (m_reactionBrowser.channel != m_channelId)
      return nullptr;
    const auto message = std::ranges::find(m_messages, m_reactionBrowser.message, &api::Message::id);
    return message == m_messages.end() || message->deleted || blockedMessage(*message) ? nullptr : &*message;
  }

  api::Interaction ClientApplication::reactionBrowserQuery() const {
    return {
        .kind = api::InteractionKind::ListReactions,
        .channel = m_reactionBrowser.channel,
        .message = m_reactionBrowser.message,
        .emoji = m_reactionBrowser.emoji,
        .burst = m_reactionBrowser.burst,
        .reactionAfter = m_reactionBrowser.cursors.at(m_reactionBrowser.cursor)
    };
  }

  void ClientApplication::closeReactionBrowser() {
    m_reactionRefreshTimer.stop();
    m_reactionRetryTimer.stop();
    if (!m_reactionBrowser.message.empty()) {
      const auto prefix = std::to_string(static_cast<int>(api::InteractionKind::ListReactions))
          + ":"
          + m_reactionBrowser.channel
          + ":"
          + m_reactionBrowser.message
          + ":";
      std::erase_if(m_reactorPages, [&](const auto& item) {
        return item.second.pending && item.first.starts_with(prefix);
      });
    }
    m_scrolling.stop(m_reactionBrowser.emojiScroll);
    m_scrolling.stop(m_reactionBrowser.peopleScroll);
    m_reactionBrowser = {};
    m_reactionBrowserRoot = nullptr;
    m_reactionEmojiBounds = m_reactionPeopleBounds = {};
  }

  void ClientApplication::showReactions(const std::string& id, const std::string& emoji) {
    const auto found = std::ranges::find(m_messages, id, &api::Message::id);
    if (found == m_messages.end()
        || found->deleted
        || blockedMessage(*found)
        || m_messageLog.known(m_channelId, id)
        || found->reactions.empty())
      return;
    const auto selected =
        std::ranges::find_if(found->reactions, [&](const auto& item) { return api::reactionKey(item) == emoji; });
    const auto index = selected == found->reactions.end() ? 0 : selected - found->reactions.begin();
    const auto reaction = selected == found->reactions.end() ? found->reactions.front() : *selected;
    closeContext();
    closeDialog();
    closeEmojiPicker();
    closeGifPicker();
    closeInbox();
    closeSearchMenu();
    m_controlInput.cancelPointerCapture();
    m_controlInput.setFocus(nullptr);
    m_controlInput.pointerLeave();
    m_scrolling.stopAll();
    m_dragDraft = m_dragMessage = false;
    m_chatTextClick.reset();
    m_fieldFocused = false;
    m_reactorHoverTimer.stop();
    m_hoverReactionKey.clear();
    m_reactionTooltip.reset();
    m_reactionTooltipBounds = {};
    m_dialogKind = "reactions";
    ++m_dialogGeneration;
    m_reactionBrowser.channel = m_channelId;
    m_reactionBrowser.message = id;
    m_reactionBrowser.emoji = api::reactionKey(reaction);
    m_reactionBrowser.emojiScroll = index * layout::Reactions::emojiRowHeight;
    m_reactionBrowser.burst = normalReactions(reaction) == 0 && reaction.burstCount > 0;
    loadReactionBrowser();
    m_window.invalidate();
  }

  void ClientApplication::loadReactionBrowser(bool retry) {
    const auto* message = reactionBrowserMessage();
    if (m_dialogKind != "reactions" || !message)
      return;
    const auto reaction = std::ranges::find_if(message->reactions, [&](const auto& item) {
      return api::reactionKey(item) == m_reactionBrowser.emoji;
    });
    if (reaction == message->reactions.end()) {
      if (message->reactions.empty())
        return;
      m_reactionBrowser.emoji = api::reactionKey(message->reactions.front());
      m_reactionBrowser.burst = normalReactions(message->reactions.front()) == 0;
    } else if ((m_reactionBrowser.burst ? reaction->burstCount : normalReactions(*reaction)) == 0)
      m_reactionBrowser.burst = !m_reactionBrowser.burst;
    m_reactionBrowser.changed = false;
    const auto query = reactionBrowserQuery();
    const auto cached = m_reactorPages.find(interactionKey(query));
    if (retry
        || cached == m_reactorPages.end()
        || !cached->second.loaded
        || (m_reactionBrowser.loaded == 0 && std::chrono::steady_clock::now() - cached->second.touched >= reactorReuse))
      requestReactors(query, retry);
    acceptReactionBrowserPage(query);
  }

  void ClientApplication::invalidateReactors(const std::string& channel, const std::string& message) {
    const auto prefix =
        std::to_string(static_cast<int>(api::InteractionKind::ListReactions)) + ":" + channel + ":" + message + ":";
    std::erase_if(m_reactorPages, [&](const auto& item) { return item.first.starts_with(prefix); });
    m_hoverReactionKey.clear();
    if (m_dialogKind == "reactions" && m_reactionBrowser.channel == channel && m_reactionBrowser.message == message) {
      m_reactionBrowser.changed = true;
      m_reactionBrowser.cursors = {""};
      m_reactionBrowser.cursor = m_reactionBrowser.loaded = 0;
      m_reactionBrowser.more = true;
      m_reactionRetryTimer.stop();
      m_scrolling.stop(m_reactionBrowser.peopleScroll);
      m_reactionBrowser.peopleScroll = 0;
      m_reactionRefreshTimer.start(reactorRefreshDelay, [this] {
        loadReactionBrowser(true);
        m_window.invalidate();
      });
    }
  }

  void ClientApplication::scheduleReactionBrowserRetry(std::chrono::milliseconds delay) {
    if (m_dialogKind != "reactions" || !reactionBrowserMessage())
      return;
    const auto due = std::chrono::steady_clock::now() + delay;
    if (m_reactionRetryTimer.active() && m_reactionRetryAt <= due)
      return;
    m_reactionRetryAt = due;
    m_reactionRetryTimer.start(delay, [this] {
      if (m_dialogKind == "reactions")
        m_window.invalidate();
    });
  }

  void ClientApplication::acceptReactionBrowserPage(const api::Interaction& action) {
    auto& browser = m_reactionBrowser;
    if (m_dialogKind != "reactions"
        || browser.channel != action.channel
        || browser.message != action.message
        || browser.emoji != action.emoji
        || browser.burst != action.burst)
      return;
    const auto cursor = std::ranges::find(browser.cursors, action.reactionAfter);
    const auto found = m_reactorPages.find(interactionKey(action));
    if (cursor == browser.cursors.end() || found == m_reactorPages.end() || !found->second.loaded)
      return;
    const auto index = static_cast<std::size_t>(cursor - browser.cursors.begin());
    auto& page = found->second;
    page.touched = std::chrono::steady_clock::now();
    browser.loaded = std::max(browser.loaded, index * api::reactorPageSize + page.users.size());
    if (index + 1 == browser.cursors.size()) {
      browser.more = page.more && !page.next.empty() && page.next != action.reactionAfter;
      if (browser.more)
        browser.cursors.push_back(page.next);
    }
  }

  void ClientApplication::requestReactors(api::Interaction action, bool retry) {
    const bool browser = m_dialogKind == "reactions"
        && m_reactionBrowser.channel == action.channel
        && m_reactionBrowser.message == action.message
        && m_reactionBrowser.emoji == action.emoji
        && m_reactionBrowser.burst == action.burst;
    if (m_messageLog.known(action.channel, action.message)
        || m_channelId != action.channel
        || m_contextOpen
        || (!m_dialogKind.empty() && !browser)
        || settingsVisible()
        || !m_viewImage.empty())
      return;
    if (!m_connected) {
      if (browser)
        scheduleReactionBrowserRetry(reactorRetryDelay);
      return;
    }
    const auto key = interactionKey(action);
    const auto now = std::chrono::steady_clock::now();
    if (const auto old = m_reactorPages.find(key); old != m_reactorPages.end()) {
      if (old->second.pending)
        return;
      if (!retry && !old->second.error.empty()) {
        if (!browser)
          return;
        const auto delay = reactorRetryDelay * (1U << std::min(old->second.failures, reactorRetryExponent));
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(old->second.touched + delay - now);
        if (remaining.count() >= 0) {
          scheduleReactionBrowserRetry(remaining + std::chrono::milliseconds(1));
          return;
        }
      } else if (!retry && now - old->second.touched < reactorReuse)
        return;
    }
    if (!m_reactorPages.contains(key) && m_reactorPages.size() >= reactorCacheLimit) {
      const auto oldest =
          std::ranges::min_element(m_reactorPages, {}, [](const auto& item) { return item.second.touched; });
      m_reactorPages.erase(oldest);
    }
    action.revision = ++m_reactorSerial;
    auto& page = m_reactorPages[key];
    const auto failures = page.failures;
    page = {};
    page.failures = failures;
    page.revision = action.revision;
    page.touched = now;
    page.pending = m_demo || m_client.interact(action);
    if (!page.pending) {
      page.error = "Could not load people who reacted.";
      page.failures = std::min(page.failures + 1, reactorRetryExponent);
      if (browser)
        scheduleReactionBrowserRetry(reactorRetryDelay);
    }
    m_window.invalidate();
  }

  void ClientApplication::hoverReaction(const api::Message& message, const api::Reaction& reaction, const Hit& chip) {
    if (!chip.contains(m_pointerX, m_pointerY) && chip.id != m_reactionHoverChip.id)
      return;
    if (m_contextOpen
        || !m_dialogKind.empty()
        || settingsVisible()
        || !m_viewImage.empty()
        || m_inboxOpen
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || m_shareOpen
        || chip.y < m_messageViewportTop
        || chip.y + chip.height > m_messageViewportBottom
        || (!chip.contains(m_pointerX, m_pointerY)
            && !layout::reactionTooltipContainsPointer(
                {chip.x, chip.y, chip.width, chip.height}, m_reactionTooltipBounds, m_pointerX, m_pointerY
            )))
      return;
    m_reactionHoverChip = chip;
    m_reactionTooltip = reaction;
    api::Interaction action{
        .kind = api::InteractionKind::ListReactions,
        .channel = message.channelId,
        .message = message.id,
        .emoji = api::reactionKey(reaction)
    };
    const auto key = interactionKey(action);
    if (m_hoverReactionKey != key) {
      m_hoverReactionKey = key;
      m_reactorHoverTimer.start(
          reactorHoverDelay,
          [this, action, normal = normalReactions(reaction) > 0, burst = reaction.burstCount > 0, key] {
            if (m_hoverReactionKey != key)
              return;
            if (normal)
              requestReactors(action);
            if (burst) {
              auto super = action;
              super.burst = true;
              requestReactors(std::move(super));
            }
          }
      );
    }
    std::vector<api::Account> people;
    bool pending = false, failed = false, loaded = false;
    for (bool burst : {false, true}) {
      if ((burst ? reaction.burstCount : normalReactions(reaction)) == 0)
        continue;
      action.burst = burst;
      const auto found = m_reactorPages.find(interactionKey(action));
      if (found == m_reactorPages.end()) {
        pending = true;
        continue;
      }
      const auto& page = found->second;
      pending |= page.pending;
      failed |= !page.error.empty();
      loaded |= page.loaded;
      for (const auto& person : page.users)
        if (std::ranges::none_of(people, [&](const auto& old) { return old.id == person.id; }))
          people.push_back(person);
    }
    if (people.empty())
      m_tooltip = pending ? "Loading people who reacted…"
          : failed        ? "Could not load people who reacted."
          : loaded        ? "No reactions remain."
                          : "Loading people who reacted…";
    else {
      const auto shown = std::min(people.size(), reactorHoverNames);
      std::string names;
      for (std::size_t i = 0; i < shown; ++i) {
        if (i)
          names += i + 1 == shown ? " and " : ", ";
        names += people[i].id == m_account.id ? "You" : userName(people[i].id, people[i].name);
      }
      m_tooltip = names + " reacted with " + (reaction.id.empty() ? reaction.name : ":" + reaction.name + ":");
      if (failed)
        m_tooltip += "\nSome people could not be loaded.";
    }
    m_tooltipX = chip.x + chip.width / 2;
    m_tooltipY = chip.y;
  }

  void ClientApplication::updateReactionHover() {
    const auto clear = [this] {
      if (m_reactionTooltip || !m_hoverReactionKey.empty())
        m_tooltip.clear();
      m_hoverReactionKey.clear();
      m_reactorHoverTimer.stop();
      m_reactionTooltip.reset();
      m_reactionTooltipBounds = {};
      m_reactionHoverChip = {};
    };
    if (m_hoverReactionKey.empty()
        || !m_reactionTooltip
        || !m_dialogKind.empty()
        || m_contextOpen
        || settingsVisible()
        || m_inboxOpen
        || m_shareOpen
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || m_videoExpanded
        || !m_viewImage.empty()) {
      clear();
      return;
    }
    const auto chip = std::ranges::find_if(m_hits, [&](const auto& hit) {
      return hit.action == Action::ToggleReaction && hit.id == m_reactionHoverChip.id;
    });
    const auto split = m_reactionHoverChip.id.find('\n');
    const auto message = std::ranges::find(m_messages, m_reactionHoverChip.id.substr(0, split), &api::Message::id);
    if (chip == m_hits.end()
        || message == m_messages.end()
        || message->deleted
        || blockedMessage(*message)
        || chip->y < m_messageViewportTop
        || chip->y + chip->height > m_messageViewportBottom
        || !layout::reactionTooltipContainsPointer(
            {chip->x, chip->y, chip->width, chip->height}, m_reactionTooltipBounds, m_pointerX, m_pointerY
        )) {
      clear();
      return;
    }
    const auto reaction = std::ranges::find_if(message->reactions, [&](const auto& item) {
      return api::reactionKey(item) == api::reactionKey(*m_reactionTooltip);
    });
    if (reaction == message->reactions.end()) {
      clear();
      return;
    }
    hoverReaction(*message, *reaction, *chip);
  }

  void ClientApplication::drawReactionEmoji(Node& root, const api::Reaction& reaction, const layout::Frame& frame) {
    if (!reaction.id.empty()) {
      if (!emojiImage(root, frame.x, frame.y, frame.width, api::emojiPath(reaction.id)))
        glyph(root, frame.x, frame.y, frame.width, "mood-smile", theme::muted);
      return;
    }
    auto& renderer = m_window.target().renderer();
    const auto natural = renderer.measureText(reaction.name, Style::fontSizeHeader);
    const float size = Style::fontSizeHeader
        * std::min(
                           {1.0F, frame.width / std::max(1.0F, natural.width),
                            frame.height / std::max(1.0F, natural.bottom - natural.top)}
        );
    const auto fitted = renderer.measureText(reaction.name, size);
    label(
        root, frame.x + (frame.width - fitted.width) / 2, frame.y + (frame.height - (fitted.bottom - fitted.top)) / 2,
        fitted.width + Style::spaceXxs, reaction.name, size, theme::foreground
    );
  }

  void ClientApplication::drawReactionTooltip(Node& root, float width, float height) {
    if (!m_reactionTooltip || m_hoverReactionKey.empty())
      return;
    const auto& reaction = *m_reactionTooltip;
    auto& renderer = m_window.target().renderer();
    const auto linkCaption = "View all " + std::to_string(reaction.count) + " reactions";
    const auto natural = renderer.measureText(m_tooltip, Style::fontSizeDescription);
    const auto linkWidth = renderer.measureText(linkCaption, Style::fontSizeDescription, FontWeight::Medium).width;
    const float w = layout::reactionTooltipWidth(width, std::ceil(natural.width) + Style::spaceXxs, linkWidth);
    const layout::Frame anchor{
        m_reactionHoverChip.x, m_reactionHoverChip.y, m_reactionHoverChip.width, m_reactionHoverChip.height
    };
    auto frames = layout::reactionTooltipFrames(width, height, anchor, w, 0);
    const auto metrics =
        renderer.measureText(m_tooltip, Style::fontSizeDescription, FontWeight::Normal, frames.text.width, 0);
    const float captionHeight =
        std::max(Style::fontSizeDescription + Style::spaceXs, std::ceil(metrics.bottom - metrics.top));
    frames = layout::reactionTooltipFrames(width, height, anchor, w, captionHeight);
    const auto [x, y, cardWidth, h] = frames.card;
    m_reactionTooltipBounds = frames.card;
    auto card = std::make_unique<Node>();
    card->setPosition(x, y);
    card->setSize(w, h);
    card->setClipChildren(true);
    auto* content = card.get();
    root.addChild(std::move(card));
    rectangle(*content, 0, 0, w, h, theme::raised, Style::radiusMd);
    m_hits.push_back({x, y, w, h, Action::None, {}});
    const auto bridge = layout::reactionTooltipBridge(anchor, frames.card);
    if (bridge.width > 0 && bridge.height > 0)
      m_hits.push_back({bridge.x, bridge.y, bridge.width, bridge.height, Action::None, {}});
    drawReactionEmoji(
        *content, reaction, {frames.icon.x - x, frames.icon.y - y, frames.icon.width, frames.icon.height}
    );
    auto caption = std::make_unique<Node>();
    caption->setPosition(frames.text.x - x, frames.text.y - y);
    caption->setSize(frames.text.width, frames.text.height);
    caption->setClipChildren(true);
    caption->setHitTestVisible(false);
    label(
        *caption, 0, 0, frames.text.width, m_tooltip, Style::fontSizeDescription, theme::foreground, FontWeight::Normal,
        0
    );
    content->addChild(std::move(caption));
    auto link = frames.link;
    link.width = std::min(link.width, std::ceil(linkWidth));
    const bool over = link.contains(m_pointerX, m_pointerY);
    label(
        *content, link.x - x, link.y - y + Style::spaceXxs, link.width, messageMarkup(linkCaption, {.underline = over}),
        Style::fontSizeDescription, theme::accent, FontWeight::Medium, 1, true
    );
    m_hits.push_back({link.x, link.y, link.width, link.height, Action::ViewReactions, m_reactionHoverChip.id});
  }

  void ClientApplication::reactionBrowserAction(const std::string& id) {
    if (m_dialogKind != "reactions")
      return;
    if (id == "close") {
      closeDialog();
      return;
    }
    if (id.starts_with("user:")) {
      const auto user = id.substr(5);
      for (const auto& cursor : m_reactionBrowser.cursors) {
        auto query = reactionBrowserQuery();
        query.reactionAfter = cursor;
        const auto page = m_reactorPages.find(interactionKey(query));
        if (page != m_reactorPages.end()
            && std::ranges::any_of(page->second.users, [&](const auto& person) { return person.id == user; })) {
          showProfile(user);
          break;
        }
      }
      return;
    }
    if (id.starts_with("emoji:")) {
      const auto* message = reactionBrowserMessage();
      if (!message)
        return;
      const auto found = std::ranges::find_if(message->reactions, [&](const auto& reaction) {
        return api::reactionKey(reaction) == id.substr(6);
      });
      if (found == message->reactions.end())
        return;
      m_reactionBrowser.emoji = api::reactionKey(*found);
      m_reactionBrowser.burst = normalReactions(*found) == 0 && found->burstCount > 0;
    } else if (id == "normal" || id == "super")
      m_reactionBrowser.burst = id == "super";
    else
      return;
    m_reactionRetryTimer.stop();
    m_reactionBrowser.cursors = {""};
    m_reactionBrowser.cursor = m_reactionBrowser.loaded = 0;
    m_reactionBrowser.more = true;
    m_scrolling.stop(m_reactionBrowser.peopleScroll);
    m_reactionBrowser.peopleScroll = 0;
    loadReactionBrowser();
    m_window.invalidate();
  }

  bool ClientApplication::reactionBrowserClick(float x, float y) {
    if (m_dialogKind != "reactions")
      return false;
    if (!m_dialogBounds.contains(x, y))
      closeDialog();
    else
      m_controlInput.pointerButton(
          x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
      );
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::reactionBrowserKey(const Key& key) {
    if (m_dialogKind != "reactions" || (key.control && (key.symbol == XKB_KEY_q || key.symbol == XKB_KEY_Q)))
      return false;
    if (key.symbol == XKB_KEY_Escape)
      closeDialog();
    else if (key.symbol == XKB_KEY_Tab && m_reactionBrowserRoot)
      (void)m_controlInput.cycleTabFocusInSubtree(m_reactionBrowserRoot, key.shift);
    else if (key.symbol == XKB_KEY_Page_Down || key.symbol == XKB_KEY_Page_Up) {
      m_scrolling.stop(m_reactionBrowser.peopleScroll);
      m_reactionBrowser.peopleScroll += (key.symbol == XKB_KEY_Page_Down ? 1 : -1) * m_reactionPeopleBounds.height;
    } else if (m_reactionBrowserRoot) {
      bool owned = false;
      for (auto* node = static_cast<Node*>(m_controlInput.focusedArea()); node; node = node->parent())
        owned |= node == m_reactionBrowserRoot;
      if (!owned)
        m_controlInput.setFocus(m_controlInput.firstTabFocusUnder(m_reactionBrowserRoot));
      m_controlInput.keyEvent(
          key.symbol, 0, (key.control ? KeyMod::Ctrl : 0) | (key.shift ? KeyMod::Shift : 0), true, false
      );
    }
    m_window.invalidate();
    return true;
  }

  void ClientApplication::drawReactionBrowser(Node& root, float width, float height) {
    using R = layout::Reactions;
    const auto* message = reactionBrowserMessage();
    const api::Reaction* chosen = nullptr;
    if (message)
      for (const auto& reaction : message->reactions)
        if (api::reactionKey(reaction) == m_reactionBrowser.emoji)
          chosen = &reaction;
    const bool filters = chosen && chosen->burstCount > 0;
    const auto frames = layout::reactionBrowserFrames(width, height, filters);
    auto surface = std::make_unique<Node>();
    surface->setSize(width, height);
    m_reactionBrowserRoot = surface.get();
    root.addChild(std::move(surface));
    auto& modal = *m_reactionBrowserRoot;
    rectangle(
        modal, 0, layout::Window::titlebarHeight, width, height - layout::Window::titlebarHeight,
        withAlpha(theme::background, Style::modalBackdropOpacity)
    );
    m_hits.push_back(
        {0, layout::Window::titlebarHeight, width, height - layout::Window::titlebarHeight, Action::None, {}}
    );
    const auto& frame = frames.dialog;
    m_dialogBounds = {frame.x, frame.y, frame.width, frame.height, Action::None, {}};
    rectangle(modal, frame.x, frame.y, frame.width, frame.height, theme::sidebar, Style::radiusXl);
    label(
        modal, frame.x + R::padding, frame.y + R::padding, frame.width - R::padding * 2 - frames.close.width,
        "Reactions", Style::fontSizeHeading, theme::foreground, FontWeight::SemiBold
    );
    rectangle(
        modal, frame.x, frame.y + R::headerHeight - Style::spaceSm, frame.width, Style::borderWidth, theme::raised
    );
    const auto generation = m_dialogGeneration;
    const auto control = [&](Node& parent, const std::string& id, const layout::Frame& bounds,
                             const std::string& caption, const std::string& action, bool enabled = true,
                             bool selected = false, const std::string& icon = "") {
      const auto key = "reactions:" + id;
      std::unique_ptr<Node> saved;
      if (auto old = m_detachedInputs.find(key); old != m_detachedInputs.end()) {
        saved = std::move(old->second);
        m_detachedInputs.erase(old);
      }
      auto button = saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
      auto* result = button.get();
      m_widgets[key] = result;
      button->setText(caption);
      if (!icon.empty())
        button->setGlyph(icon);
      if (button->glyph())
        button->glyph()->setVisible(!icon.empty());
      button->setFontSize(Style::fontSizeCaption);
      button->setPosition(bounds.x, bounds.y);
      button->setFrameSize(bounds.width, bounds.height);
      button->setControlHeight(bounds.height);
      button->setRadius(Style::radiusMd);
      button->setVariant(selected ? ButtonVariant::TabActive : ButtonVariant::Ghost);
      button->setEnabled(enabled);
      button->setOnClick([this, action, generation] {
        if (m_dialogKind == "reactions" && generation == m_dialogGeneration)
          reactionBrowserAction(action);
      });
      parent.addChild(std::move(button));
      return result;
    };
    control(modal, "close", frames.close, "", "close", true, false, "x");
    if (filters) {
      control(
          modal, "normal", frames.normal, "Normal" + (chosen ? " " + std::to_string(normalReactions(*chosen)) : ""),
          "normal", chosen && normalReactions(*chosen) > 0, !m_reactionBrowser.burst
      );
      control(
          modal, "super", frames.super, "Super" + (chosen ? " " + std::to_string(chosen->burstCount) : ""), "super",
          chosen && chosen->burstCount > 0, m_reactionBrowser.burst
      );
    }
    rectangle(
        modal, frames.emojis.x + frames.emojis.width, frames.emojis.y, Style::borderWidth, frames.emojis.height,
        theme::raised
    );
    m_reactionEmojiBounds = frames.emojis;
    m_reactionPeopleBounds = frames.people;
    const auto pane = [&](const layout::Frame& bounds) {
      auto node = std::make_unique<Node>();
      node->setPosition(bounds.x, bounds.y);
      node->setSize(bounds.width, bounds.height);
      node->setClipChildren(true);
      auto* result = node.get();
      modal.addChild(std::move(node));
      return result;
    };
    auto& emojis = *pane(frames.emojis);
    const float emojiHeight = message ? message->reactions.size() * R::emojiRowHeight : 0;
    m_scrolling.limit(m_reactionBrowser.emojiScroll, std::max(0.0F, emojiHeight - frames.emojis.height));
    const float emojiWidth = std::max(0.0F, frames.emojis.width - R::scrollbarGutter);
    if (message)
      for (std::size_t i = 0; i < message->reactions.size(); ++i) {
        const auto& reaction = message->reactions[i];
        const float y = i * R::emojiRowHeight - m_reactionBrowser.emojiScroll;
        if (y + R::emojiRowHeight <= 0 || y >= frames.emojis.height)
          continue;
        const auto key = api::reactionKey(reaction);
        auto* button = control(
            emojis, "emoji:" + key, {0, y, emojiWidth, R::emojiRowHeight}, "", "emoji:" + key, true,
            key == m_reactionBrowser.emoji
        );
        auto art = std::make_unique<Node>();
        art->setHitTestVisible(false);
        art->setPosition(0, y);
        art->setSize(emojiWidth, R::emojiRowHeight);
        auto* decoration = art.get();
        emojis.addChild(std::move(art));
        drawReactionEmoji(
            *decoration, reaction, {Style::spaceXs, (R::emojiRowHeight - R::emojiSize) / 2, R::emojiSize, R::emojiSize}
        );
        label(
            *decoration, R::emojiSize + Style::spaceSm,
            (R::emojiRowHeight - Style::fontSizeCaption - Style::spaceXs) / 2,
            std::max(0.0F, emojiWidth - R::emojiSize - Style::spaceSm), std::to_string(reaction.count),
            Style::fontSizeCaption, button->label()->color()
        );
      }
    auto& browser = m_reactionBrowser;
    auto& people = *pane(frames.people);
    const bool available = message && chosen && !browser.changed;
    const bool incomplete = available && browser.more;
    const float fullHeight = (browser.loaded + (incomplete ? 1 : 0)) * R::rowHeight;
    m_scrolling.limit(browser.peopleScroll, std::max(0.0F, fullHeight - frames.people.height));
    const float rowWidth = std::max(0.0F, frames.people.width - R::scrollbarGutter);
    const auto firstRow = static_cast<std::size_t>(std::max(0.0F, browser.peopleScroll) / R::rowHeight);
    const auto lastRow = std::min(
        browser.loaded,
        static_cast<std::size_t>(std::ceil((browser.peopleScroll + frames.people.height) / R::rowHeight))
    );
    if (available) {
      // Keep names bounded in the page cache. Retain cursors so scrolling back can reload evicted pages.
      const auto firstPage = firstRow / api::reactorPageSize;
      const auto lastPage =
          std::min(browser.cursors.size() - 1, (lastRow + api::reactorPageSize - 1) / api::reactorPageSize);
      for (auto index = std::min(firstPage, lastPage); index <= lastPage; ++index) {
        if (index * api::reactorPageSize > browser.loaded)
          break;
        if (index * api::reactorPageSize == browser.loaded
            && browser.peopleScroll + frames.people.height + R::rowHeight < browser.loaded * R::rowHeight)
          break;
        browser.cursor = index;
        loadReactionBrowser();
      }
    }
    if (available)
      for (auto i = firstRow; i < lastRow; ++i) {
        auto query = reactionBrowserQuery();
        query.reactionAfter = browser.cursors.at(i / api::reactorPageSize);
        const auto cached = m_reactorPages.find(interactionKey(query));
        const auto offset = i % api::reactorPageSize;
        if (cached == m_reactorPages.end() || offset >= cached->second.users.size()) {
          if (i == firstRow || offset == 0)
            label(
                people, Style::spaceSm, i * R::rowHeight - browser.peopleScroll + Style::spaceSm,
                std::max(0.0F, rowWidth - Style::spaceLg),
                cached != m_reactorPages.end() && !cached->second.error.empty()
                    ? "Could not load people. Retrying shortly…"
                    : "Loading people who reacted…",
                Style::fontSizeCaption, theme::muted, FontWeight::Normal, 0
            );
          continue;
        }
        const auto& person = cached->second.users[offset];
        const float y = i * R::rowHeight - m_reactionBrowser.peopleScroll;
        if (y + R::rowHeight <= 0 || y >= frames.people.height)
          continue;
        auto* button = control(people, "user:" + person.id, {0, y, rowWidth, R::rowHeight}, "", "user:" + person.id);
        auto art = std::make_unique<Node>();
        art->setHitTestVisible(false);
        art->setPosition(0, y);
        art->setSize(rowWidth, R::rowHeight);
        auto* decoration = art.get();
        people.addChild(std::move(art));
        const auto name = userName(person.id, person.name);
        const bool blocked = m_userDirectory.relationship(person.id) == 2 || m_userDirectory.ignored(person.id);
        avatar(
            *decoration, Style::spaceXs, (R::rowHeight - R::avatarSize) / 2, R::avatarSize,
            blocked ? "" : person.avatar, name, person.id
        );
        const float textX = R::avatarSize + Style::spaceMd;
        const float textWidth = std::max(0.0F, rowWidth - textX - Style::spaceXs);
        label(
            *decoration, textX, Style::spaceSm, textWidth, name, Style::fontSizeCaption,
            button->hovered() ? theme::onHover : theme::foreground, FontWeight::Medium
        );
        label(
            *decoration, textX, Style::spaceSm + Style::fontSizeCaption + Style::spaceXs, textWidth,
            person.username.empty() ? person.name : person.username, Style::fontSizeDetail,
            button->hovered() ? theme::onHover : theme::muted
        );
      }
    const auto scrollbar = [&](const std::string& id, const layout::Frame& bounds, float full, float& scroll) {
      if (full > bounds.height && bounds.height > 0)
        settingsScrollbar(
            modal, "reactions:" + id,
            {bounds.x + bounds.width - Style::scrollbarHoverWidth,
             bounds.y,
             Style::scrollbarHoverWidth,
             bounds.height,
             Action::None,
             {}},
            full, scroll
        );
    };
    scrollbar("emoji-scrollbar", frames.emojis, emojiHeight, m_reactionBrowser.emojiScroll);
    scrollbar("people-scrollbar", frames.people, fullHeight, m_reactionBrowser.peopleScroll);
    if (browser.loaded == 0 || incomplete) {
      const auto text = !message                 ? "This message is no longer available."
          : message->reactions.empty()           ? "No reactions remain."
          : browser.loaded == 0 && !browser.more ? "No people who reacted."
          : m_reactionRetryTimer.active()        ? "Could not load people. Retrying shortly…"
                                                 : "Loading people who reacted…";
      const float y = browser.loaded * R::rowHeight - browser.peopleScroll;
      label(
          people, Style::spaceSm, y + Style::spaceSm, std::max(0.0F, rowWidth - Style::spaceLg), text,
          Style::fontSizeCaption, theme::muted, FontWeight::Normal, 0
      );
    }
    m_tooltip.clear();
  }

  void ClientApplication::prepareReactionBrowserDemo() {
    m_showMembers = false;
    m_preferences.reducedMotion = true;
    m_messages = {{"9601", m_channelId, "Maya", "People can react to this message.", "2026-10-15T12:00:00Z", "11"}};
    m_messages.front().reactions = {{"123456789", "owl", 103}, {"", "❤️", 5, false, 2}, {"", "🔥", 2, false, 2}};
    for (int i = 0; i < 25; ++i)
      m_messages.front().reactions.push_back({std::to_string(200000000 + i), "fixture", i + 1});
    constexpr std::size_t fixtureEdge = 32, rgbaChannels = 4;
    std::vector<unsigned char> pixels(fixtureEdge * fixtureEdge * rgbaChannels, 255);
    for (std::size_t i = 0; i < pixels.size(); i += rgbaChannels) {
      pixels[i] = 100;
      pixels[i + 1] = 180;
    }
    const auto path = api::emojiPath("123456789");
    m_images[path].texture = m_window.context().textureManager().loadFromRgba(pixels.data(), fixtureEdge, fixtureEdge);
    api::Interaction query{
        .kind = api::InteractionKind::ListReactions, .channel = m_channelId, .message = "9601", .emoji = "owl:123456789"
    };
    auto& page = m_reactorPages[interactionKey(query)];
    page.loaded = true;
    page.more = true;
    page.next = "1099";
    page.touched = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < api::reactorPageSize; ++i)
      page.users.push_back(
          {.id = std::to_string(1000 + i),
           .name = "Person " + std::to_string(i + 1),
           .avatar = path,
           .username = "person" + std::to_string(i + 1)}
      );
    m_drafts[m_channelId] = "Retained reactions draft";
    m_messageLayouts.clear();
    m_unreadTimeline.snapshot({});
    m_unreadTimeline.positioned();
  }

  void ClientApplication::demoReactionBrowserStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto move = [&](float x, float y) {
      m_window.motion(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight);
    };
    const auto clickWidget = [&](const std::string& id) {
      const auto widget = m_widgets.find("reactions:" + id);
      require(widget != m_widgets.end() && widget->second->parent(), "The reaction browser control is mounted");
      auto* button = static_cast<Button*>(widget->second);
      require(button->enabled(), "The reaction browser control is enabled");
      float x = button->width() / 2, y = button->height() / 2;
      for (const auto* node = static_cast<Node*>(button); node && node != m_scene.get(); node = node->parent()) {
        x += node->x();
        y += node->y();
      }
      move(x, y);
      m_window.click(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272);
      m_window.released();
    };
    const auto response = [&](std::size_t count, const std::string& error = "") {
      auto query = reactionBrowserQuery();
      const auto found = m_reactorPages.find(interactionKey(query));
      require(
          found != m_reactorPages.end() && found->second.pending, "Only a pending private read accepts a fixture reply"
      );
      query.revision = found->second.revision;
      api::Event event;
      event.interaction = query;
      event.error = error;
      for (std::size_t i = 0; i < count; ++i)
        event.reactors.push_back(
            {.id = std::to_string(2200 + i),
             .name = "Second person " + std::to_string(i + 1),
             .username = "second" + std::to_string(i + 1)}
        );
      if (!event.reactors.empty())
        event.reactorNext = event.reactors.back().id;
      interactionResult(event);
    };
    switch (m_demoInteractionStep++) {
    case 0: {
      const auto chip = std::ranges::find_if(m_hits, [](const auto& hit) {
        return hit.action == Action::ToggleReaction && hit.id == "9601\nowl:123456789";
      });
      require(chip != m_hits.end(), "The source reaction has a visible chip");
      move(chip->x + chip->width / 2, chip->y + chip->height / 2);
      break;
    }
    case 1:
      require(m_reactionTooltip && m_reactionTooltip->id == "123456789", "Hover opens the requested reaction summary");
      require(
          m_reactionTooltipBounds.height <= Style::spaceSection * 2 + Style::spaceLg,
          "Short reaction summaries do not reserve a blank footer-sized gap"
      );
      move(
          m_reactionTooltipBounds.x
              + layout::Reactions::tooltipPadding
              + layout::Reactions::tooltipIcon
              + Style::spaceMd
              + Style::spaceXxs,
          m_reactionTooltipBounds.y + layout::Reactions::tooltipPadding + Style::spaceXxs
      );
      break;
    case 2:
      require(
          m_reactionTooltip && m_reactionTooltip->id == "123456789", "The tooltip text retains the original reaction"
      );
      move(Style::spaceSm, layout::Window::titlebarHeight + Style::spaceSm);
      break;
    case 3:
      require(
          !m_reactionTooltip && m_hoverReactionKey.empty() && m_reactionTooltipBounds.width == 0,
          "Leaving the tooltip dismisses it on the next frame without a close timer"
      );
      require(
          std::ranges::none_of(m_hits, [](const auto& hit) { return hit.action == Action::ViewReactions; }),
          "Dismissal removes the clickable tooltip immediately"
      );
      break;
    case 4: {
      require(!m_reactionTooltip, "Unrelated repaints cannot restore a dismissed reaction card");
      const auto chip = std::ranges::find_if(m_hits, [](const auto& hit) {
        return hit.action == Action::ToggleReaction && hit.id == "9601\nowl:123456789";
      });
      require(chip != m_hits.end(), "The reaction badge remains available after dismissal");
      move(chip->x + chip->width / 2, chip->y + chip->height / 2);
      break;
    }
    case 5:
      require(m_reactionTooltip.has_value(), "Re-entering the badge opens its reaction card");
      m_window.pointerLeft();
      break;
    case 6:
      require(!m_reactionTooltip, "Leaving the window also dismisses the reaction card");
      {
        const auto chip = std::ranges::find_if(m_hits, [](const auto& hit) {
          return hit.action == Action::ToggleReaction && hit.id == "9601\nowl:123456789";
        });
        require(chip != m_hits.end(), "The reaction badge is still mounted");
        move(chip->x + chip->width / 2, chip->y + chip->height / 2);
      }
      break;
    case 7: {
      require(m_reactionTooltip.has_value(), "Returning from outside the window restores ordinary hover");
      const auto bridge = layout::reactionTooltipBridge(
          {m_reactionHoverChip.x, m_reactionHoverChip.y, m_reactionHoverChip.width, m_reactionHoverChip.height},
          m_reactionTooltipBounds
      );
      require(bridge.width > 0 && bridge.height > 0, "The tooltip has a narrow travel path from its badge");
      move(bridge.x + bridge.width / 2, bridge.y + bridge.height / 2);
      break;
    }
    case 8:
      require(m_reactionTooltip.has_value(), "Crossing the gap to the card keeps its link accessible without a delay");
      move(
          m_reactionTooltipBounds.x + layout::Reactions::tooltipPadding,
          m_reactionTooltipBounds.y + layout::Reactions::tooltipPadding
      );
      break;
    case 9: {
      require(
          m_reactionTooltip && m_reactionTooltip->id == "123456789",
          "Moving onto tooltip text cannot substitute another reaction"
      );
      const auto link =
          std::ranges::find_if(m_hits, [](const auto& hit) { return hit.action == Action::ViewReactions; });
      require(link != m_hits.end(), "The hover count opens the full browser");
      const auto hit = *link;
      move(hit.x + hit.width / 2, hit.y + hit.height / 2);
      m_window.click(m_pointerX, m_pointerY + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272);
      m_window.released();
      break;
    }
    case 10: {
      require(
          m_dialogKind == "reactions" && m_reactionBrowser.emoji == "owl:123456789",
          "Clicking the count selects that emoji in the browser"
      );
      require(
          m_reactionEmojiBounds.width - layout::Reactions::scrollbarGutter >= Style::spaceSection * 2,
          "Emoji counts keep usable width beside their scrollbar"
      );
      for (const auto& [id, node] : m_widgets)
        if (id.starts_with("reactions:") && id != "reactions:close" && !id.ends_with("scrollbar") && node->parent()) {
          const auto* button = static_cast<const Button*>(node);
          require(
              !button->glyph() || !button->glyph()->visible(), "Text and custom rows never instantiate an empty glyph"
          );
        }
      bool firstImage = false;
      const auto inspect = [&](auto&& self, const Node& node, float x, float y) -> void {
        x += node.x();
        y += node.y();
        if (node.type() == NodeType::Image
            && static_cast<const ImageNode&>(node).textureId() == m_images.at(api::emojiPath("123456789")).texture.id
            && m_reactionEmojiBounds.contains(x, y)
            && y < m_reactionEmojiBounds.y + layout::Reactions::emojiRowHeight)
          firstImage = true;
        for (const auto& child : node.children())
          self(self, *child, x, y);
      };
      inspect(inspect, *m_reactionBrowserRoot, 0, 0);
      require(firstImage, "The first emoji row mounts its image inside the attached viewport");
      m_scrolling.stop(m_reactionBrowser.peopleScroll);
      m_reactionBrowser.peopleScroll = api::reactorPageSize * layout::Reactions::rowHeight;
      break;
    }
    case 11:
      require(reactionBrowserQuery().reactionAfter == "1099", "Scrolling to the end uses the last page's user cursor");
      response(0, "Synthetic later-page failure");
      break;
    case 12:
      require(
          m_reactionBrowser.loaded == 100 && m_reactionRetryTimer.active(),
          "Later-page failure keeps the preceding people and schedules automatic retry"
      );
      {
        const auto revision = m_reactorPages.at(interactionKey(reactionBrowserQuery())).revision;
        loadReactionBrowser();
        require(
            m_reactorPages.at(interactionKey(reactionBrowserQuery())).revision == revision,
            "Repainting cannot bypass the retry delay"
        );
        m_reactionRetryTimer.stop();
        m_reactorPages.at(interactionKey(reactionBrowserQuery())).touched -= std::chrono::minutes(1);
        loadReactionBrowser();
        response(3);
      }
      require(m_reactionBrowser.loaded == 103 && !m_reactionBrowser.more, "A short page ends the continuous list");
      require(!m_widgets.contains("reactions:next"), "The list has no paging footer");
      {
        auto first = reactionBrowserQuery();
        first.reactionAfter.clear();
        const auto people = m_reactorPages.at(interactionKey(first)).users;
        m_reactorPages.erase(interactionKey(first));
        m_reactionBrowser.cursor = 0;
        loadReactionBrowser();
        require(
            m_reactorPages.at(interactionKey(first)).pending && m_reactionBrowser.loaded == 103,
            "Scrolling back can reload an evicted page without losing the list extent"
        );
        first.revision = m_reactorPages.at(interactionKey(first)).revision;
        api::Event restored;
        restored.interaction = first;
        restored.reactors = people;
        restored.reactorNext = "1099";
        restored.reactorMore = true;
        interactionResult(restored);
      }
      m_scrolling.stop(m_reactionBrowser.peopleScroll);
      m_reactionBrowser.peopleScroll = 0;
      break;
    case 13:
      require(m_widgets.at("reactions:user:1000")->parent(), "Scrolling back retains the earlier people");
      clickWidget("emoji:❤️");
      break;
    case 14:
      require(!m_reactionBrowser.burst, "Mixed reaction types initially show ordinary reactions");
      response(0, "Synthetic read failure");
      break;
    case 15: {
      require(m_reactionRetryTimer.active(), "A failed read schedules automatic retry");
      m_reactionRetryTimer.stop();
      m_reactorPages.at(interactionKey(reactionBrowserQuery())).touched -= std::chrono::minutes(1);
      loadReactionBrowser();
      require(
          m_reactorPages.at(interactionKey(reactionBrowserQuery())).pending, "An expired delay retries without a button"
      );
      auto stale = reactionBrowserQuery();
      stale.revision = m_reactorPages.at(interactionKey(stale)).revision;
      invalidateReactors(stale.channel, stale.message);
      m_reactionRefreshTimer.stop();
      loadReactionBrowser(true);
      api::Event old;
      old.interaction = stale;
      old.reactors = {{"9999", "Stale user"}};
      interactionResult(old);
      require(
          m_reactorPages.at(interactionKey(stale)).pending && m_reactorPages.at(interactionKey(stale)).users.empty(),
          "Live invalidation rejects a late page without restoring stale people"
      );
      response(0);
      break;
    }
    case 16:
      require(
          m_reactorPages.at(interactionKey(reactionBrowserQuery())).loaded,
          "Empty results settle independently of errors"
      );
      clickWidget("super");
      break;
    case 17:
      require(m_reactionBrowser.burst, "Super reactions have their own read identity");
      response(2);
      break;
    case 18:
      m_window.key({.symbol = XKB_KEY_Tab});
      require(m_controlInput.focusedArea(), "Keyboard focus remains in the browser controls");
      clickWidget("user:2200");
      break;
    case 19:
      require(m_dialogKind == "profile", "Clicking a person opens their native profile");
      closeDialog();
      m_contextMessage = "9601";
      m_contextChannel = m_channelId;
      contextAction(Action::ViewReactions);
      require(m_dialogKind == "reactions", "The message menu opens the same reactions browser");
      break;
    case 20: {
      requestReactors(reactionBrowserQuery(), true);
      auto late = reactionBrowserQuery();
      late.revision = m_reactorPages.at(interactionKey(late)).revision;
      closeDialog();
      api::Event event;
      event.interaction = late;
      event.reactors = {{"9999", "Closed user"}};
      interactionResult(event);
      require(!m_reactorPages.contains(interactionKey(late)), "Closing the browser revokes its pending read");
      showReactions("9601");
      m_messages.front().reactions.clear();
      invalidateReactors(m_channelId, "9601");
      break;
    }
    case 21:
      require(
          reactionBrowserMessage() && reactionBrowserMessage()->reactions.empty(),
          "Removing every emoji leaves a settled empty browser"
      );
      m_window.key({.symbol = XKB_KEY_Escape});
      break;
    case 22:
      require(
          m_dialogKind.empty()
              && !m_reactionRefreshTimer.active()
              && !m_reactionRetryTimer.active()
              && m_drafts.at(m_channelId) == "Retained reactions draft"
              && m_reactorPages.size() <= reactorCacheLimit,
          "Dismissal preserves the draft and retires browser reads and timers"
      );
      std::clog << "Offline reaction hover, people browser, pagination, stale reads and native controls passed\n";
      break;
    }
    m_window.invalidate();
  }
} // namespace app
