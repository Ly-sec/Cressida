#include "app/client_application.h"
#include "cursor-shape-v1-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace app {
  float ClientApplication::channelSidebarWidth() const {
    return layout::sidebarWidth(m_preferences.channelSidebarWidth, m_window.width());
  }
  float ClientApplication::chatLeft() const { return layout::Sidebar::railWidth + channelSidebarWidth(); }
  float ClientApplication::channelPanelWidth() const {
    const float available = std::max(0.0F, m_window.width() - conversationLeft());
    return std::min(
        {layout::Sidebar::panelPreferredWidth,
         std::max(layout::Sidebar::panelMinimumWidth, available - layout::Sidebar::panelMinimumWidth), available / 2}
    );
  }
  bool ClientApplication::sidebarResizeAvailable() const {
    return m_connected
        && !startupCovered()
        && !settingsVisible()
        && m_dialogKind.empty()
        && !m_contextOpen
        && m_viewImage.empty()
        && !m_videoExpanded
        && !m_presenceMenu
        && !m_emojiMenu
        && !m_gifMenu;
  }
  bool ClientApplication::sidebarResizeAt(float x, float y) const {
    return sidebarResizeAvailable()
        && std::abs(x - chatLeft()) <= layout::Sidebar::resizeSlop
        && y >= layout::Window::titlebarHeight
        && y < m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
  }
  void ClientApplication::finishSidebarResize() {
    if (!m_resizingSidebar)
      return;
    m_resizingSidebar = false;
    if (!m_demo && m_preferences.channelSidebarWidth != m_sidebarDragStart)
      m_sessionStore.savePreferences(m_account.id, m_preferences);
    m_window.invalidate();
  }
  void ClientApplication::sidebarDemoFrame() {
    const auto require = [](bool ok, const char* reason) {
      if (!ok)
        throw std::runtime_error(reason);
    };
    const auto move = [&](float x, float y) {
      m_window.motion(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight);
    };
    const auto click = [&](float x, float y) {
      m_window.click(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 1);
    };
    const auto controlPoint = [&](const std::string& id) {
      auto* control = m_inputs.at(id);
      float x = control->width() / 2, y = control->height() / 2;
      for (auto* node = static_cast<Node*>(control); node; node = node->parent()) {
        x += node->x();
        y += node->y();
      }
      return std::pair{x, y + layout::Window::titlebarHeight - m_window.titlebarHeight()};
    };
    const auto shape = [&](std::uint32_t expected) {
      if (m_window.cursorShape() != expected)
        throw std::runtime_error(
            "Sidebar replay frame "
            + std::to_string(m_demoInteractionStep - 1)
            + " expected cursor "
            + std::to_string(expected)
            + ", found "
            + std::to_string(m_window.cursorShape())
        );
    };
    const auto moveToServer = [&] {
      const auto rail = std::ranges::find(m_hits, Action::Guild, &Hit::action);
      require(rail != m_hits.end(), "Cursor replay includes a visible server target");
      move(rail->x + rail->width / 2, rail->y + rail->height / 2);
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      const auto [x, y] = controlPoint("composer");
      move(x, y);
      shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
      click(x, y);
      move(chatLeft(), 180);
      shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
      moveToServer();
      shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
    } else if (step == 1) {
      shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
      m_window.released();
      moveToServer();
      shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      move(chatLeft() + Style::spaceXxs, 180);
      shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE);
      click(chatLeft() + Style::spaceXxs, 180);
      move(78 + 360 + 2, 180);
    } else if (step == 2) {
      require(
          m_resizingSidebar && channelSidebarWidth() == 360, "Sidebar drag must survive a scene rebuild without jumping"
      );
      shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE);
      for (const auto& hit : m_hits)
        if (hit.action == Action::Channel)
          require(hit.width == 336, "Channel hit areas must follow the resized sidebar");
      const auto [x, y] = controlPoint("composer");
      require(x > chatLeft() + Style::spaceXl, "Composer must move with the sidebar");
      move(5000, 180);
    } else if (step == 3) {
      require(channelSidebarWidth() == 480, "Sidebar drag must respect the maximum width");
      m_window.released();
      move(chatLeft(), 180);
      click(chatLeft(), 180);
      move(-20, 180);
    } else if (step == 4) {
      require(channelSidebarWidth() == 200, "Sidebar drag must respect the minimum width");
      m_window.released();
      m_preferences.channelSidebarWidth = 480;
      m_window.setUiScale(1.5F);
    } else if (step == 5) {
      require(
          m_window.width() - chatLeft() >= 360 && m_preferences.channelSidebarWidth == 480,
          "Zoom must clamp the effective width without overwriting the preferred width"
      );
      m_window.setUiScale(1);
    } else if (step == 6) {
      require(channelSidebarWidth() == 480, "Widening the viewport must restore the preferred width");
      activate({0, 0, 0, 0, Action::ToggleEmoji, {}});
    } else if (step == 7) {
      require(!sidebarResizeAvailable(), "Picker overlays must block sidebar resizing");
      const auto [x, y] = controlPoint("emoji-search");
      move(x, y);
      shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
      move(chatLeft(), 180);
      require(
          m_window.cursorShape() != WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE,
          "Covered dividers must not show a resize cursor"
      );
      closeEmojiPicker();
      openSettings();
    } else if (step == 8) {
      require(!sidebarResizeAvailable(), "Settings must block sidebar resizing");
      const auto [x, y] = controlPoint("settings:search");
      move(x, y);
      shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
      activate({0, 0, 0, 0, Action::CloseSettings, {}});
    } else if (step == 9) {
      move(chatLeft(), 180);
      click(chatLeft(), 180);
      move(chatLeft() - Style::spaceSection, 180);
      m_window.pointerLeft();
      require(!m_resizingSidebar, "Leaving the surface must end sidebar dragging");
      shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      std::clog << "Offline cursor and sidebar replay passed\n";
    }
    m_window.invalidate();
  }
  void ClientApplication::prepareSidebarActivityDemo() {
    m_preferences.reducedMotion = true;
    m_channels = {{"100", "Activity", 4}, {"101", "selected", 0, 0, "100"}, {"103", "nearby", 0, 1, "100"}};
    for (int i = 0; i < 50; ++i)
      m_channels.push_back({std::to_string(1100 + i), "channel-" + std::to_string(i), 0, i + 2, "100"});
    m_guildChannels[m_guildId] = m_channels;
    m_channelListPreferences = {};
    m_collapsedCategories.clear();
    m_channelScroll = 0;
    m_unreadTimeline.snapshot({{"103", {"8999", "9000"}}, {"1130", {"8999", "9000"}}}, "reset");
    m_unreadTimeline.positioned();
    m_unreadChannels = {"103", "1130"};
    m_unreadGuilds = {{"103", m_guildId}, {"1130", m_guildId}};
    api::UserUpdates muted;
    muted.guildNotifications[m_guildId].muted = true;
    m_userDirectory.apply(muted);
    m_drafts[m_channelId] = "Retained sidebar draft";
  }

  void ClientApplication::demoSidebarActivityStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto edge = [&](const std::string& id) -> std::optional<Hit> {
      const auto at = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == Action::JumpChannelMention && (id.empty() || hit.id == id);
      });
      return at == m_hits.end() ? std::nullopt : std::optional<Hit>(*at);
    };
    const auto click = [&](const std::string& id) {
      const auto at = edge(id);
      require(at.has_value(), "Offscreen activity has a visible navigation pill");
      const float x = at->x + at->width / 2;
      const float y = at->y + at->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.released();
    };
    const auto mute = [&](const std::string& id, bool value) {
      api::UserUpdates updates;
      updates.mutedChannels[id] = value;
      m_userDirectory.apply(updates);
    };
    switch (m_demoInteractionStep++) {
    case 0:
      require(edge("1130").has_value(), "Server mute retains offscreen ordinary channel activity");
      click("1130");
      break;
    case 1:
      require(!edge("1130") && edge("103"), "Revealing below exposes earlier unread activity above");
      require(m_channelId == "101" && m_unreadTimeline.cursor("1130"), "Revealing never selects or acknowledges");
      click("103");
      break;
    case 2:
      require(!edge("103") && edge("1130"), "Both directions reveal their destination");
      m_unreadTimeline.snapshot(
          {{"103", {"8999", "9000", 2}}, {"1130", {"8999", "9000"}}, {"1135", {"8999", "9000", 100}}}, "reset"
      );
      mute("1135", true);
      m_channelScroll = layout::Sidebar::rowStep * 15;
      break;
    case 3:
      require(edge("103") && edge("1135"), "Mentions take priority above and below, including muted channels");
      click("1135");
      break;
    case 4:
      require(!edge("1135") && m_unreadTimeline.mentions("1135") == 100, "Revealing preserves the mention count");
      m_unreadTimeline.acknowledged("1135", "9000");
      m_channelScroll = 0;
      break;
    case 5:
      require(edge("1130") && !edge("1135"), "Acknowledged mentions no longer contribute");
      mute("1130", true);
      break;
    case 6:
      require(!edge(""), "Muted ordinary channels do not contribute offscreen activity");
      m_unreadTimeline.snapshot({{"1135", {"8999", "9001", 2}}}, "reset");
      mute("100", true);
      m_channelListPreferences.hideMuted = true;
      m_collapsedCategories.insert("100");
      break;
    case 7:
      require(
          !edge("") && m_unreadTimeline.mentions("1135") == 2, "Collapsed categories retain their visible mentioned row"
      );
      m_collapsedCategories.clear();
      m_channelListPreferences.hideMuted = false;
      break;
    case 8:
      require(edge("1135").has_value(), "Expanded muted categories still contribute mentions");
      m_preferences.channelSidebarWidth = layout::Sidebar::minimumWidth;
      break;
    case 9: {
      const auto at = edge("1135");
      require(
          at.has_value() && at->x >= layout::Sidebar::railWidth && at->x + at->width <= chatLeft(),
          "Activity pills remain inside a resized sidebar"
      );
      m_window.click(
          at->x + at->width / 2, at->y + at->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          273
      );
      require(!m_contextOpen, "Context clicks cannot reach a covered channel row");
      m_unreadTimeline.acknowledged("1135", "9001");
      break;
    }
    case 10:
      require(!edge(""), "Local acknowledgements clear offscreen activity immediately");
      break;
    case 11:
      require(
          m_channelId == "101" && m_drafts.at(m_channelId) == "Retained sidebar draft",
          "Activity navigation preserves the conversation and draft"
      );
      break;
    case 12:
      std::clog << "Offline sidebar activity navigation, mute, acknowledgement and bounds replay passed\n";
      break;
    }
    m_window.invalidate();
  }
} // namespace app
