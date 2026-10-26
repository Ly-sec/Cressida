#include "app/client_application.h"
#include "app/font_catalog.h"
#include "app/settings_style.h"
#include "app/theme.h"
#include "app/user_settings_form.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/controls/button.h"
#include "ui/controls/scrollbar.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;
  namespace {
    struct SettingsPage {
      const char* id;
      const char* name;
      const char* group;
      const char* keywords = "";
    };
    constexpr float navTop = layout::Settings::navigationTop, navBottom = layout::Settings::navigationBottom,
                    navRowHeight = layout::Settings::navigationRowHeight,
                    navHeadingHeight = layout::Settings::navigationHeadingHeight;
    constexpr SettingsPage pages[] = {
        {"cressida", "General", "CRESSIDA",
         "messages deleted edited history logger memory expressions emoji image links name badges server tags role "
         "colors gradients animation"},
        {"cosmetics", "Profile cosmetics", "CRESSIDA",
         "avatar decorations nameplates profile effects frames banners backgrounds colors"},
        {"themes", "Themes", "CRESSIDA", "theme palette catppuccin rose pine gruvbox custom toml colors dark light"},
        {"account", "My Account", "ACCOUNT"},
        {"profile", "Profiles", "ACCOUNT"},
        {"presence", "Status", "ACCOUNT"},
        {"privacy", "Privacy & Safety", "ACCOUNT"},
        {"content", "Content & Social", "ACCOUNT"},
        {"friends", "Friend Requests", "ACCOUNT"},
        {"relationships", "Friends & Blocked Users", "ACCOUNT", "ignore ignored nickname requests pending"},
        {"data", "Data & Privacy", "ACCOUNT", "export package harvest analytics personalization ads"},
        {"authorized", "Authorized Apps", "ACCOUNT"},
        {"devices", "Devices", "ACCOUNT"},
        {"connections", "Connections", "ACCOUNT"},
        {"subscriptions", "Subscriptions", "PAYMENT"},
        {"payments", "Billing", "PAYMENT", "payments payment sources invoice cards"},
        {"gifts", "Gift Inventory", "PAYMENT", "entitlements gifts rewards nitro collectibles"},
        {"appearance", "Appearance", "APP SETTINGS", "font family text size theme scale compact"},
        {"accessibility", "Accessibility", "APP SETTINGS"},
        {"voice", "Voice & Video", "APP SETTINGS"},
        {"text", "Text & Images", "APP SETTINGS"},
        {"notifications", "Notifications", "APP SETTINGS"},
        {"keybinds", "Keybinds", "APP SETTINGS"},
        {"language", "Language", "APP SETTINGS"},
        {"advanced", "Advanced", "APP SETTINGS"},
        {"activity", "Activity Privacy", "ACTIVITY SETTINGS"},
        {"games", "Registered Games", "ACTIVITY SETTINGS"},
        {"overlay", "Game Overlay", "ACTIVITY SETTINGS"}
    };
    std::string folded(std::string text) {
      for (auto& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return text;
    }
    bool matches(const SettingsPage& page, const std::string& query) {
      return query.empty() || folded(std::string(page.name) + " " + page.group + " " + page.keywords).contains(query);
    }
  } // namespace
  void ClientApplication::clearAccountSettings() {
    m_voiceJoinTimer.stop();
    releasePushToTalk();
    m_client.voiceAudio()->stopTest();
    m_voiceState = {};
    m_voiceRoster.clear();
    m_voiceGuild.clear();
    m_voiceMuted = m_voiceDeafened = false;
    m_desktopNotifications.clear();
    m_notifiedMessages.clear();
    resetUserAccountSettings();
    resetUserSettingsLists();
    m_shortcutCapture.clear();
    m_accountSettings.clear();
    m_settingsDraft.clear();
    m_settingsLists.clear();
    m_settingsListErrors.clear();
    m_loadingSettingsLists.clear();
    m_settingsLoaded = m_loadingSettings = m_savingSettings = false;
  }
  void ClientApplication::selectSettingsPage(const std::string& page) {
    if (m_userSettingsLists.busy && page != m_settingsPage) {
      m_userSettingsLists.status = "Wait for the current account operation to finish before changing pages.";
      m_window.invalidate();
      return;
    }
    if (page == "expressions" || page == "name-badges" || page == "role-styles") {
      selectSettingsPage("cressida");
      return;
    }
    m_scrolling.stopAll();
    closeSelectDropdown();
    if (std::ranges::none_of(pages, [&](const auto& item) { return item.id == page; }))
      return;
    m_settingsPage = page;
    releasePushToTalk();
    m_client.voiceAudio()->stopTest();
    if (page == "voice" && !m_demo)
      m_client.voiceAudio()->refreshDevices();
    if (page == "advanced")
      for (std::size_t i = 0; i < m_preferences.windowControls.icons.size(); ++i)
        m_settingsFields["window-icon:" + std::to_string(i)] = m_preferences.windowControls.icons[i];
    m_fontPickerOpen = false;
    m_fontPickerBounds = {};
    m_scrolling.stop(m_fontPickerScroll);
    m_fontPickerScroll = 0;
    m_installedFonts.clear();
    m_fontMatches.clear();
    m_settingsFields.erase("font_search");
    m_settingsScroll = 0;
    m_settingsField.clear();
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    float rowY = 0;
    std::string group;
    const auto query = folded(m_settingsFields["search"]);
    const float navHeight = std::max(30.0F, static_cast<float>(m_window.height()) - navTop - navBottom);
    for (const auto& entry : pages) {
      if (!matches(entry, query))
        continue;
      if (group != entry.group) {
        group = entry.group;
        rowY += navHeadingHeight;
      }
      if (page == entry.id) {
        if (page == "cressida")
          m_settingsNavScroll = 0;
        else if (rowY < m_settingsNavScroll)
          m_settingsNavScroll = rowY;
        else if (rowY + navRowHeight > m_settingsNavScroll + navHeight)
          m_settingsNavScroll = rowY + navRowHeight - navHeight;
        break;
      }
      rowY += navRowHeight;
    }
    m_shortcutCapture.clear();
    loadUserAccountSettings(page);
    loadUserSettingsLists();
  }

  void ClientApplication::resetProfile() {
    m_settingsFields["display_name"] = m_account.displayName;
    m_settingsFields["pronouns"] = m_account.pronouns;
    m_settingsFields["bio"] = m_account.bio;
    m_settingsField.clear();
    m_profileDirty = false;
  }
  void ClientApplication::openSettings() {
    if (m_serverSettings.open) {
      closeServerSettings();
      if (m_serverSettings.open)
        return;
    }
    closeCompletion();
    cancelMessageEdit(false);
    finishSidebarResize();
    closeChannelPanel();
    m_forumSearchTimer.stop();
    closeVideo();
    m_scrolling.stopAll();
    closeGifPicker();
    m_settingsOpen = true;
    m_presenceMenu = false;
    m_emojiMenu = false;
    m_settingsScroll = 0;
    m_settingsNavScroll = 0;
    m_fieldFocused = false;
    m_dragDraft = m_dragMessage = false;
    resetProfile();
    m_settingsFields["custom_status"] = m_presence.text;
    m_settingsStatus.clear();
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_loadingProfile = false;
    m_loadingSettings = !m_demo && m_client.accountSettings();
    if (m_demo) {
      m_accountSettings = api::parseAccountSettings(nlohmann::json::object());
      m_settingsDraft = m_accountSettings;
      m_settingsLoaded = true;
    }
    selectSettingsPage(m_settingsPage);
  }
  void ClientApplication::savePreferences() {
    configureVoice();
    releaseDisabledCosmetics();
    m_window.setDesktopDecorations(m_preferences.desktopDecorations);
    m_window.setWindowControls(m_preferences.windowControls);
    m_messageLayouts.clear();
    if (!m_demo)
      m_sessionStore.savePreferences(m_account.id, m_preferences);
    m_window.invalidate();
  }
  bool ClientApplication::fontPreference(const std::string& id) {
    if (!m_settingsOpen || m_settingsPage != "appearance")
      return false;
    if (id == "font-picker") {
      m_fontPickerOpen = !m_fontPickerOpen;
      m_settingsFields["font_search"].clear();
      m_scrolling.stop(m_fontPickerScroll);
      m_fontPickerScroll = 0;
      m_fontPickerBounds = {};
      if (m_fontPickerOpen) {
        m_fontPickerReturnScroll = m_settingsScroll;
        m_installedFonts = installedFontFamilies();
        m_fontMatches = m_installedFonts;
        m_fontFilter.clear();
        m_window.context().notifyFontConfigChanged();
        m_focusedInput = "settings:font_search";
      } else {
        m_controlInput.cancelPointerCapture();
        m_controlInput.setFocus(nullptr);
        m_focusedInput.clear();
        m_scrolling.stop(m_settingsScroll);
        m_settingsScroll = m_fontPickerReturnScroll;
      }
    } else if (id.starts_with("font-family:")) {
      if (!m_fontPickerOpen)
        return true;
      const auto family = id.substr(12);
      if (!family.empty() && std::ranges::find(m_installedFonts, family) == m_installedFonts.end())
        return true;
      m_preferences.fontFamily = family;
      m_fontPickerOpen = false;
      m_fontPickerBounds = {};
      m_scrolling.stop(m_fontPickerScroll);
      m_controlInput.cancelPointerCapture();
      m_settingsFields["font_search"].clear();
      m_controlInput.setFocus(nullptr);
      m_focusedInput.clear();
      m_scrolling.stop(m_settingsScroll);
      m_settingsScroll = m_fontPickerReturnScroll;
      savePreferences();
    } else
      return false;
    m_window.invalidate();
    return true;
  }
  bool ClientApplication::fontPickerClick(float x, float y) {
    if (!m_settingsOpen || !m_fontPickerOpen)
      return false;
    const auto& bounds = m_fontPickerBounds;
    if (!bounds.contains(x, y)) {
      fontPreference("font-picker");
      return true;
    }
    const bool search = x >= bounds.x + Style::spaceSm
        && x < bounds.x + bounds.width - 8
        && y >= bounds.y + Style::spaceSm
        && y < bounds.y + Style::spaceSection;
    const auto bar = m_widgets.find("font-picker-scrollbar");
    const bool scrollbar = x >= bounds.x + bounds.width - 16
        && y >= bounds.y + 80
        && bar != m_widgets.end()
        && static_cast<Scrollbar*>(bar->second)->visible();
    if (search || scrollbar) {
      m_controlInput.pointerButton(
          x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
      );
      m_window.invalidate();
      return true;
    }
    for (auto at = m_hits.rbegin(); at != m_hits.rend(); ++at)
      if (at->action == Action::Preference && at->id.starts_with("font-family:") && at->contains(x, y)) {
        const auto id = at->id;
        fontPreference(id);
        break;
      }
    return true;
  }
  void ClientApplication::drawFontPicker(Node& root, float w, float h) {
    if (!m_fontPickerOpen || m_settingsPage != "appearance")
      return;
    const auto& query = m_settingsFields["font_search"];
    if (query != m_fontFilter) {
      m_fontFilter = query;
      m_fontMatches.clear();
      for (const auto& name : m_installedFonts)
        if (fontFamilyMatches(name, query))
          m_fontMatches.push_back(name);
    }
    const float width = std::min(layout::FontPicker::preferredWidth, w - Style::space2Xl);
    const float x = std::clamp(m_fontPickerAnchor.x + m_fontPickerAnchor.width - width, 16.0F, w - width - 16);
    const float below = h - m_fontPickerAnchor.y - m_fontPickerAnchor.height - 20;
    const float above = m_fontPickerAnchor.y - 56;
    const bool down = below >= std::min(320.0F, above);
    const float full = std::max<std::size_t>(1, m_fontMatches.size()) * layout::FontPicker::rowStep;
    const float viewport = std::min(
        full,
        std::max(
            layout::FontPicker::rowStep,
            std::min(
                layout::FontPicker::maximumViewportHeight, (down ? below : above) - layout::FontPicker::chromeHeight
            )
        )
    );
    const float height = viewport + layout::FontPicker::chromeHeight;
    const float y = std::clamp(
        down ? m_fontPickerAnchor.y + m_fontPickerAnchor.height + 6 : m_fontPickerAnchor.y - height - 6, 40.0F,
        std::max(40.0F, h - height - 12)
    );
    m_fontPickerBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::None, {}});
    m_hits.push_back(m_fontPickerBounds);
    rectangle(root, x, y, width, height, raised, Style::surfaceRadius);
    rectangle(
        root, x + Style::borderWidth, y + Style::borderWidth, width - Style::spaceXxs, height - Style::spaceXxs,
        sidebar, Style::hoverRadius
    );
    inputControl(
        root, x + Style::spaceSm, y + Style::spaceSm, width - 16, 32, "settings:font_search", query,
        "Search installed fonts"
    );
    const auto option = [&](Node& parent, float localX, float localY, float rowWidth, const std::string& name,
                            const std::string& value, float hitY, float hitHeight) {
      const bool selected = m_preferences.fontFamily == value;
      const bool over = hitHeight > 0 && hovered(x + Style::spaceSm, hitY, rowWidth, hitHeight);
      if (selected || over)
        rectangle(
            parent, localX, localY, rowWidth, 26, selected ? lerpColor(sidebar, accent, .16F) : raised,
            Style::controlRadius
        );
      const auto metrics =
          m_window.target().renderer().measureText(name, Style::fontSizeDescription, FontWeight::Normal);
      label(
          parent, localX + Style::spaceSm, localY + (26 - (metrics.bottom - metrics.top)) / 2,
          rowWidth - Style::spaceLg, name, Style::fontSizeDescription, foreground
      );
      if (hitHeight > 0)
        m_hits.push_back({x + Style::spaceSm, hitY, rowWidth, hitHeight, Action::Preference, "font-family:" + value});
    };
    // Reset remains pinned while results scroll, including for an empty search.
    option(
        root, x + Style::spaceSm, y + (Style::spaceGroup - Style::spaceXxs), width - 16, "System default", {},
        y + (Style::spaceGroup - Style::spaceXxs), 26
    );
    auto list = std::make_unique<Node>();
    list->setPosition(x + Style::spaceSm, y + layout::FontPicker::listTop);
    list->setSize(width - Style::spaceWide, viewport);
    list->setClipChildren(true);
    m_scrolling.limit(m_fontPickerScroll, std::max(0.0F, full - viewport));
    list->setTextScrollOffset(-m_fontPickerScroll);
    // Keep a bounded set of rows prepared around the viewport; font metadata is inexpensive,
    // and the remaining families need no scene nodes or glyphs until approached.
    const std::size_t visible = static_cast<std::size_t>(std::ceil(viewport / layout::FontPicker::rowStep)) + 1;
    const std::size_t overscan =
        visible < layout::FontPicker::maximumPreparedRows ? (layout::FontPicker::maximumPreparedRows - visible) / 2 : 0;
    const auto top = static_cast<std::size_t>(m_fontPickerScroll / layout::FontPicker::rowStep);
    const auto first = std::min(
        top > overscan ? top - overscan : 0,
        m_fontMatches.size() > layout::FontPicker::maximumPreparedRows
            ? m_fontMatches.size() - layout::FontPicker::maximumPreparedRows
            : 0
    );
    const auto end = std::min(m_fontMatches.size(), first + layout::FontPicker::maximumPreparedRows);
    for (auto i = first; i < end; ++i) {
      const float rowY = i * layout::FontPicker::rowStep - m_fontPickerScroll + Style::spaceXxs;
      const float hitTop = std::max(0.0F, rowY),
                  hitBottom = std::min(viewport, rowY + (Style::spaceXl + Style::spaceXxs));
      option(
          *list, 0, rowY, width - 28, m_fontMatches[i], m_fontMatches[i], y + layout::FontPicker::listTop + hitTop,
          std::max(0.0F, hitBottom - hitTop)
      );
    }
    if (m_fontMatches.empty())
      label(
          *list, 8, 6, width - 44, m_installedFonts.empty() ? "No installed fonts found" : "No matching fonts",
          Style::fontSizeDescription, muted
      );
    root.addChild(std::move(list));
    settingsScrollbar(
        root, "font-picker-scrollbar", {x + width - 14, y + layout::FontPicker::listTop, 8, viewport}, full,
        m_fontPickerScroll
    );
  }
  void ClientApplication::setPresence(const std::string& status, bool custom) {
    m_presenceMenu = false;
    if (m_demo) {
      m_settingsStatus = "Demo mode is read-only.";
      return;
    }
    if (m_client.setPresence(status, custom ? std::optional(m_settingsFields["custom_status"]) : std::nullopt)) {
      m_desiredPresence = status;
      m_settingsStatus = "Saving status…";
    } else
      m_settingsStatus = "Could not update status. Check the connection.";
  }
  void ClientApplication::drawPresenceMenu(Node& root, float h) {
    m_hits.push_back(
        {0, 36, static_cast<float>(m_window.width()), h - layout::Window::titlebarHeight, Action::TogglePresence, {}}
    );
    const float x = layout::accountFrame(h, chatLeft()).x, y = h - 338;
    rectangle(root, x, y, 258, 254, background, Style::radiusXl);
    label(
        root, x + Style::spaceLg, y + Style::cardPadding, 220, m_account.name, Style::fontSizeBody, foreground,
        FontWeight::SemiBold
    );
    int index = 0;
    for (const auto& status : {"online", "idle", "dnd", "invisible"}) {
      const float sy = y + ((Style::spaceSection + Style::spaceXs) + Style::borderWidth) + index++ * 40;
      if (hovered(x + Style::spaceSm, sy, 242, 36))
        rectangle(root, x + Style::spaceSm, sy, 242, 36, panel, Style::radiusMd);
      rectangle(
          root, x + (Style::spaceLg + Style::spaceXxs), sy + Style::spaceMd, 10, 10, presenceColor(status),
          Style::controlRadius
      );
      label(
          root, x + Style::spaceSection, sy + (Style::spaceSm + Style::borderWidth), 190,
          std::string(presenceName(status)), Style::fontSizeBody, foreground
      );
      m_hits.push_back({x + Style::spaceSm, sy, 242, 36, Action::SetPresence, status});
    }
    button(root, {x + Style::spaceDense, y + 210, 238, 32, Action::SettingsPage, "presence"}, "Set a custom status");
  }
  void ClientApplication::drawSettings(Node& root, float w, float h) {
    std::erase_if(m_hits, [](const auto& hit) {
      return hit.action != Action::Close && hit.action != Action::Minimize && hit.action != Action::Maximize;
    });
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, background);
    const auto retained = [&](const std::string& id) {
      std::unique_ptr<Node> saved;
      if (auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
        saved = std::move(at->second);
        m_detachedInputs.erase(at);
      }
      return saved;
    };
    const auto settingsButton = [&](Node& parent, Hit hit, const std::string& caption, bool primary = false,
                                    bool selected = false, bool fill = false, bool centered = true) {
      float ox = 0, oy = 0;
      for (const auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
        ox += node->x();
        oy += node->y();
      }
      const auto natural =
          m_window.target().renderer().measureText(caption, Style::fontSizeDescription, FontWeight::Medium);
      if (!fill)
        hit.width = std::min(hit.width, std::ceil(natural.width) + 20);
      hit.y += (hit.height - layout::Settings::actionHeight) / 2;
      hit.height = layout::Settings::actionHeight;
      const bool enabled = hit.action != Action::None;
      const auto id =
          "settings-button:" + std::to_string(static_cast<int>(hit.action)) + ':' + (hit.id.empty() ? caption : hit.id);
      auto saved = retained(id);
      auto control =
          saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
      m_widgets[id] = control.get();
      control->setText(caption);
      control->setFontSize(settings::control.size);
      control->setPosition(hit.x, hit.y);
      control->setFrameSize(hit.width, layout::Settings::actionHeight);
      control->setControlHeight(layout::Settings::actionHeight);
      control->setRadius(Style::scaledRadius(layout::Settings::actionRadius));
      control->setPadding(0, Style::spaceDense);
      control->setContentAlign(centered ? ButtonContentAlign::Center : ButtonContentAlign::Start);
      control->setVariant(
          primary        ? ButtonVariant::Primary
              : selected ? ButtonVariant::TabActive
                         : ButtonVariant::Default
      );
      control->setEnabled(enabled);
      control->setOnClick([this, hit] {
        activate(hit);
        m_window.invalidate();
      });
      parent.addChild(std::move(control));
      hit.x += ox;
      hit.y += oy;
      m_hits.push_back(std::move(hit));
    };
    const auto frame = layout::settingsFrame(w);
    const float frameX = frame.x, navWidth = frame.navigationWidth;
    const float contentLeft = frame.contentLeft, contentWidth = frame.contentWidth;
    const float contentX = frame.contentX, width = frame.innerWidth;
    const auto textHeight = [&](const std::string& text, float size, FontWeight weight, float available) {
      const auto metrics = m_window.target().renderer().measureText(text, size, weight, std::max(1.0F, available), 0);
      return std::max(size + 4, metrics.bottom - metrics.top);
    };
    m_settingsNavRight = frameX + navWidth + Style::spaceXs;
    rectangle(root, 0, 36, m_settingsNavRight, h - layout::Window::titlebarHeight, sidebar);
    label(
        root, frameX + Style::spaceMd, 58, navWidth - Style::spaceXl, "SETTINGS", settings::navigationTitle.size, muted,
        settings::navigationTitle.weight
    );
    auto& search = inputControl(
        root, frameX + Style::spaceXs, 82, navWidth - Style::spaceSm, 32, "settings:search", m_settingsFields["search"],
        "Search settings"
    );
    search.setFrameRadius(6);
    search.setHorizontalPadding(10);
    struct NavRow {
      const SettingsPage* page;
      float y;
      bool heading;
    };
    std::vector<NavRow> navRows;
    float navHeight = 0;
    std::string group;
    const auto query = folded(m_settingsFields["search"]);
    for (const auto& entry : pages) {
      if (!matches(entry, query))
        continue;
      if (group != entry.group) {
        group = entry.group;
        navRows.push_back({&entry, navHeight, true});
        navHeight += navHeadingHeight;
      }
      navRows.push_back({&entry, navHeight, false});
      navHeight += navRowHeight;
    }
    const float navH = std::max(30.0F, h - navTop - navBottom);
    m_scrolling.limit(m_settingsNavScroll, std::max(0.0F, navHeight - navH));
    auto nav = std::make_unique<Node>();
    nav->setPosition(frameX, navTop);
    nav->setSize(frame.navigationContentWidth, navH);
    nav->setClipChildren(true);
    for (const auto& row : navRows) {
      const auto y = row.y - m_settingsNavScroll;
      if (y < 0 || y + navRowHeight > navH)
        continue;
      if (row.heading)
        label(
            *nav, 12, y + Style::spaceSm, frame.navigationContentWidth - Style::spaceXl, row.page->group,
            settings::navigationHeading.size, muted, settings::navigationHeading.weight
        );
      else {
        const auto controlId = "nav:" + std::string(row.page->id);
        std::unique_ptr<Node> saved;
        if (auto at = m_detachedInputs.find(controlId); at != m_detachedInputs.end()) {
          saved = std::move(at->second);
          m_detachedInputs.erase(at);
        }
        auto item = saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
        m_widgets[controlId] = item.get();
        item->setText(row.page->name);
        item->setFontSize(settings::control.size);
        item->setVariant(m_settingsPage == row.page->id ? ButtonVariant::TabActive : ButtonVariant::Tab);
        item->setContentAlign(ButtonContentAlign::Start);
        item->setPosition(4, y);
        item->setControlHeight(layout::Settings::actionHeight);
        item->setRadius(Style::scaledRadius(layout::Settings::actionRadius));
        item->setPadding(0, 8);
        item->setFrameSize(frame.navigationContentWidth - Style::spaceSm, layout::Settings::actionHeight);
        item->setOnClick([this, id = std::string(row.page->id)] {
          selectSettingsPage(id);
          m_window.invalidate();
        });
        nav->addChild(std::move(item));
      }
    }
    if (navRows.empty())
      label(
          *nav, 12, 12, frame.navigationContentWidth - Style::spaceXl, "No matching settings",
          Style::fontSizeDescription, muted
      );
    root.addChild(std::move(nav));
    settingsScrollbar(
        root, "settings-nav-scrollbar", {frame.navigationScrollbarX, navTop, layout::Settings::scrollbarWidth, navH},
        navHeight, m_settingsNavScroll
    );
    settingsButton(
        root, {frameX + Style::spaceXs, h - 62, navWidth - Style::spaceSm, 32, Action::SignOut, {}}, "Log Out"
    );
    iconButton(root, {w - 56, 52, 30, 30, Action::CloseSettings, {}}, "x", "Close settings (Esc)");
    const auto selected = std::ranges::find_if(pages, [&](const auto& entry) { return m_settingsPage == entry.id; });
    const std::string title = selected == std::end(pages) ? "Settings" : selected->name;
    label(root, contentLeft, 63, contentWidth, title, settings::pageTitle.size, foreground, settings::pageTitle.weight);
    const bool unsaved = m_settingsDraft != m_accountSettings && m_settingsLoaded;
    const float saveHeight = unsaved ? (width < layout::Settings::stackThreshold ? layout::Settings::stackedSaveHeight
                                                                                 : layout::Settings::saveHeight)
                                     : 0;
    const float statusHeight = m_settingsStatus.empty()
        ? 0
        : textHeight(m_settingsStatus, Style::fontSizeDescription, FontWeight::Normal, width - 24) + 20;
    const float viewportY = layout::Settings::viewportTop,
                viewportH = std::max(
                    40.0F,
                    h
                        - viewportY
                        - Style::spaceXl
                        - (saveHeight ? saveHeight + Style::spaceMd : 0)
                        - (statusHeight ? statusHeight + Style::spaceSm : 0)
                );
    const float previousScroll = m_settingsScroll;
    auto page = std::make_unique<Node>();
    page->setPosition(contentLeft, viewportY);
    page->setSize(contentWidth, viewportH);
    page->setClipChildren(true);
    auto body = std::make_unique<Node>();
    auto* bodyNode = body.get();
    body->setPosition(layout::Settings::contentInset, -m_settingsScroll);
    page->addChild(std::move(body));
    auto& p = *bodyNode;
    const auto firstHit = m_hits.size();
    float cursor = 0, categoryTop = 0;
    RectNode* category = nullptr;
    const auto finishCategory = [&] {
      if (!category)
        return;
      cursor += Style::spaceSm;
      category->setSize(contentWidth, cursor - categoryTop);
      category = nullptr;
    };
    const auto section = [&](const std::string& caption) {
      finishCategory();
      if (cursor)
        cursor += Style::spaceLg;
      categoryTop = cursor;
      auto card = std::make_unique<RectNode>();
      card->setPosition(-layout::Settings::contentInset, cursor);
      card->setStyle({.fill = sidebar, .radius = Style::scaledRadius(Style::surfaceRadius)});
      card->setZIndex(-1);
      category = card.get();
      p.addChild(std::move(card));
      cursor += caption.empty() ? 4 : 12;
      if (!caption.empty()) {
        cursor += label(p, 0, cursor, width, caption, settings::section.size, muted, settings::section.weight, 0);
        cursor += Style::spaceSm;
      }
    };
    const auto ensureCategory = [&] {
      if (!category)
        section("");
    };
    using SettingRow = UserSettingRow;
    const auto row = [&](const std::string& name, const std::string& description = std::string{}, float trailing = 0,
                         float minimum = 40, bool interactive = false, bool stackControl = false) {
      ensureCategory();
      const float below = stackControl && width < layout::Settings::stackThreshold ? 44 : 0;
      const float top = cursor, textWidth = std::max(48.0F, width - (below ? 0 : trailing));
      const float heading = textHeight(name, settings::rowTitle.size, settings::rowTitle.weight, textWidth);
      const float detail = description.empty()
          ? 0
          : textHeight(description, Style::fontSizeDescription, FontWeight::Normal, textWidth) + 4;
      const float height = std::max(minimum, heading + detail + 20 + below);
      if (interactive && hovered(contentX, viewportY - previousScroll + top, width, height))
        rectangle(
            p, -Style::spaceCompact, top, width + Style::spaceMd, height - 1, withAlpha(raised, .45F),
            Style::controlRadius
        );
      label(
          p, 0, top + (height - below - heading - detail) / 2, textWidth, name, settings::rowTitle.size, foreground,
          settings::rowTitle.weight, 0
      );
      if (!description.empty())
        label(
            p, 0, top + (height - below - heading - detail) / 2 + heading + Style::spaceXs, textWidth, description,
            Style::fontSizeDescription, muted, FontWeight::Normal, 0
        );
      cursor += height;
      return SettingRow{top, height};
    };
    const auto actionRow = [&](const std::string& name, const std::string& description) {
      return row(name, description, 144, 40, false, true);
    };
    const auto rowAction = [&](const SettingRow& item, Action action, const std::string& id, const std::string& caption,
                               bool primary = false) {
      settingsButton(
          p,
          {width < layout::Settings::stackThreshold ? 0 : width - 128,
           width < layout::Settings::stackThreshold ? item.top + item.height - 40 : item.top + (item.height - 32) / 2,
           128, 32, action, id},
          caption, primary, false, true, true
      );
    };
    const auto toggle = [&](const std::string& name, bool enabled, const std::string& id,
                            const std::string& description, bool remote = false) {
      const auto item = row(name, description, 62);
      const auto controlId = "toggle:" + id;
      std::unique_ptr<Node> saved;
      if (auto at = m_detachedInputs.find(controlId); at != m_detachedInputs.end()) {
        saved = std::move(at->second);
        m_detachedInputs.erase(at);
      }
      auto control =
          saved ? std::unique_ptr<Toggle>(static_cast<Toggle*>(saved.release())) : std::make_unique<Toggle>();
      m_widgets[controlId] = control.get();
      control->setToggleSize(ToggleSize::Small);
      control->setCheckedImmediate(enabled);
      control->setEnabled(!remote || (m_settingsLoaded && !m_loadingSettings && !m_savingSettings));
      // Small native geometry: 14px thumb + 2px insets + 12px travel.
      control->setPosition(
          width - layout::Settings::toggleWidth, item.top + (item.height - layout::Settings::toggleHeight) / 2
      );
      control->setFrameSize(layout::Settings::toggleWidth, layout::Settings::toggleHeight);
      control->setOnChange([this, id, remote](bool) {
        activate({0, 0, 0, 0, remote ? Action::AccountPreference : Action::Preference, id});
        m_window.invalidate();
      });
      p.addChild(std::move(control));
    };
    const auto accountToggle = [&](const std::string& name, const std::string& key, const std::string& description) {
      toggle(name, m_settingsDraft.contains(key) && m_settingsDraft.at(key) != 0, key, description, true);
    };
    const auto field = [&](const std::string& name, const std::string& key, float height,
                           const std::string& description) {
      ensureCategory();
      cursor += Style::spaceSm;
      cursor += label(p, 0, cursor, width, name, settings::rowTitle.size, foreground, settings::rowTitle.weight);
      cursor += Style::spaceXs;
      cursor +=
          label(p, 0, cursor, width, description, settings::description.size, muted, settings::description.weight, 0);
      cursor += Style::spaceSm;
      auto& input =
          inputControl(p, 0, cursor, width, height, "settings:" + key, m_settingsFields[key], {}, key == "bio");
      input.setFrameRadius(6);
      input.setHorizontalPadding(10);
      cursor += height + 16;
    };
    const auto formDetail = [&](const std::string& title, const std::string& description) {
      ensureCategory();
      cursor += Style::spaceSm;
      cursor += label(p, 0, cursor, width, title, settings::rowTitle.size, foreground, settings::rowTitle.weight, 0);
      if (!description.empty()) {
        cursor += Style::spaceXs;
        cursor +=
            label(p, 0, cursor, width, description, settings::description.size, muted, settings::description.weight, 0);
      }
      cursor += Style::spaceSm;
    };
    const auto formButton = [&](Node& parent, float x, float y, float buttonWidth, const std::string& caption,
                                const std::string& action, ButtonVariant variant, bool enabled) {
      const auto id = "user-account-button:" + action;
      auto saved = retained(id);
      auto control =
          saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
      m_widgets[id] = control.get();
      control->setText(caption);
      control->setFontSize(settings::control.size);
      control->setVariant(variant);
      control->setPosition(x, y);
      control->setFrameSize(buttonWidth, 26);
      control->setControlHeight(layout::Settings::actionHeight);
      control->setRadius(Style::scaledRadius(layout::Settings::actionRadius));
      control->setPadding(0, Style::spaceDense);
      control->setEnabled(enabled);
      control->setOnClick([this, action] {
        activate({0, 0, 0, 0, action.starts_with("window-") ? Action::Preference : Action::UserAccountAction, action});
        m_window.invalidate();
      });
      parent.addChild(std::move(control));
    };
    const auto formField = [&](const std::string& id, const std::string& title, const std::string& description,
                               const std::string& initial, std::function<void(const std::string&)> changed,
                               float height, bool password, bool enabled) {
      formDetail(title, description);
      auto& input = inputControl(p, 0, cursor, width, height, id, initial, {}, height > 40, password);
      input.setFontSize(settings::control.size);
      input.setFontWeight(settings::control.weight);
      input.setFrameRadius(5);
      input.setHorizontalPadding(10);
      input.setEnabled(enabled);
      input.setOnChange(std::move(changed));
      input.setOnSubmit([](const std::string&) {});
      cursor += height + 12;
    };
    const auto formChoose = [&](const std::string& id, const std::string& title, const std::string& description,
                                std::vector<std::string> choices, std::size_t selected,
                                std::function<void(std::size_t)> changed, bool enabled) {
      formDetail(title, description);
      const auto key = "user-account-select:" + id;
      auto saved = retained(key);
      auto control =
          saved ? std::unique_ptr<Select>(static_cast<Select*>(saved.release())) : std::make_unique<Select>();
      m_widgets[key] = control.get();
      control->setPosition(0, cursor);
      control->setOnSelectionChanged({});
      control->setOptions(std::move(choices));
      control->setSelectedIndexSilently(selected);
      control->setFontSize(settings::control.size);
      control->setControlHeight(30);
      control->setHorizontalPadding(10);
      control->setFrameSize(width, 30);
      control->setEnabled(enabled);
      control->setOnSelectionChanged([changed = std::move(changed)](std::size_t index, std::string_view) {
        changed(index);
      });
      p.addChild(std::move(control));
      cursor += (Style::spaceSection + Style::spaceXxs);
    };
    const auto formToggle = [&](const std::string& id, const std::string& title, const std::string& description,
                                bool checked, std::function<void(bool)> changed, bool enabled) {
      const auto item = row(title, description, 50);
      const auto key = "user-account-toggle:" + id;
      auto saved = retained(key);
      auto control =
          saved ? std::unique_ptr<Toggle>(static_cast<Toggle*>(saved.release())) : std::make_unique<Toggle>();
      m_widgets[key] = control.get();
      control->setToggleSize(ToggleSize::Small);
      control->setCheckedImmediate(checked);
      control->setEnabled(enabled);
      control->setFrameSize(layout::Settings::toggleWidth, layout::Settings::toggleHeight);
      control->setPosition(
          width - layout::Settings::toggleWidth, item.top + (item.height - layout::Settings::toggleHeight) / 2
      );
      control->setOnChange(std::move(changed));
      p.addChild(std::move(control));
    };
    const auto formSlider = [&](const std::string& id, const std::string& title, const std::string& description,
                                double current, double minimum, double maximum, double step,
                                std::function<void(double)> changed, const std::string& unit, bool enabled) {
      formDetail(title, description);
      const auto key = "user-account-slider:" + id;
      auto saved = retained(key);
      auto control =
          saved ? std::unique_ptr<Slider>(static_cast<Slider*>(saved.release())) : std::make_unique<Slider>();
      m_widgets[key] = control.get();
      control->setPosition(0, cursor);
      control->setSize(std::max(50.0F, width - 76), 26);
      control->setControlHeight(layout::Settings::actionHeight);
      control->setTrackHeight(3);
      control->setThumbSize(12);
      control->setOnValueChanged({});
      control->setRange(minimum, maximum);
      control->setStep(step);
      control->setValue(current);
      control->setEnabled(enabled);
      control->setWheelAdjustEnabled(false);
      control->setOnValueChanged(std::move(changed));
      p.addChild(std::move(control));
      label(
          p, width - 66, cursor + (Style::spaceXs + Style::borderWidth), 66,
          std::to_string(static_cast<int>(std::round(current))) + unit, Style::fontSizeDescription, foreground
      );
      cursor += (Style::spaceSection - Style::spaceXxs);
    };
    UserSettingsForm form{
        p,
        cursor,
        width,
        section,
        [&](const std::string& title, const std::string& description, float trailing, bool stack) {
          return row(title, description, trailing, 40, false, stack);
        },
        formButton,
        formField,
        formChoose,
        formToggle,
        formSlider
    };
    const auto remoteChoice = [&](const std::string& title, const std::string& key, const std::string& description,
                                  std::vector<std::string> names) {
      const auto value = m_settingsDraft.contains(key) ? m_settingsDraft.at(key) : 0;
      form.choose(
          "remote:" + key, title, description, std::move(names), std::max(0, value),
          [this, key](std::size_t selected) {
            activate({0, 0, 0, 0, Action::AccountPreference, key + ":" + std::to_string(selected)});
            m_window.invalidate();
          },
          m_settingsLoaded && !m_loadingSettings && !m_savingSettings
      );
    };
    const auto localChoice = [&](const std::string& title, const std::string& key, const std::string& description,
                                 int value, std::vector<std::string> names) {
      form.choose(
          "local:" + key, title, description, std::move(names), std::max(0, value), [this, key](std::size_t selected) {
            activate({0, 0, 0, 0, Action::Preference, key + ":" + std::to_string(selected)});
            m_window.invalidate();
          }
      );
    };
    const auto localSlider = [&](const std::string& title, const std::string& key, const std::string& description,
                                 int value, int minimum, int maximum, const std::string& unit) {
      form.slider(
          "local:" + key, title, description, value, minimum, maximum, 1,
          [this, key](double value) {
            activate({0, 0, 0, 0, Action::Preference, key + ":" + std::to_string(static_cast<int>(std::round(value)))});
            m_window.invalidate();
          },
          unit
      );
    };
    if (drawUserAccountSettings(form)) {
    } else if (drawUserSettingsLists(form)) {
    } else if (m_settingsPage == "cosmetics") {
      section("Profile cosmetics");
      toggle(
          "Avatar decorations", m_preferences.showAvatarDecorations, "avatar_decorations",
          "Show decorative frames around user avatars."
      );
      toggle(
          "Nameplates", m_preferences.showNameplates, "nameplates",
          "Show equipped backgrounds behind users in lists and the account area."
      );
      toggle(
          "Profile effects", m_preferences.showProfileEffects, "profile_effects",
          "Show equipped visual effects inside profiles."
      );
      toggle(
          "Profile frames", m_preferences.showProfileFrames, "profile_frames",
          "Show equipped borders around profile cards."
      );
      toggle(
          "Profile banners", m_preferences.showProfileBanners, "profile_banners",
          "Show banner images at the top of profiles."
      );
      toggle(
          "Profile colors", m_preferences.showProfileThemes, "profile_themes",
          "Use each profile’s chosen background colors."
      );
    } else if (m_settingsPage == "cressida") {
      section("Names & roles");
      toggle(
          "Show server tags in chat", m_preferences.showGuildTags, "guild_tags",
          "Show clickable server badges beside message authors."
      );
      toggle(
          "Show role colors", m_preferences.roleColors, "role_colors", "Use server role colors and gradients for names."
      );
      toggle(
          "Animate gradients on hover", m_preferences.animateRoleColors, "animate_role_colors",
          "Animate name gradients when hovering names, member rows or messages."
      );
      toggle(
          "Show role icons", m_preferences.showRoleIcons, "show_role_icons",
          "Display role icons beside message authors in chat."
      );
      section("Uploads");
      toggle(
          "Randomize media filenames", m_preferences.randomizeMediaFilenames, "randomize_media_filenames",
          "Replace outgoing image and video filenames with random names. File extensions and media contents stay the "
          "same."
      );
      section("Message logger");
      toggle(
          "Keep deleted messages", m_preferences.logDeletedMessages, "log_deleted_messages",
          "Keep deleted messages in chat until you sign out or close Cressida. Disabling this option clears them."
      );
      toggle(
          "Keep edit history", m_preferences.logEditHistory, "log_edit_history",
          "Show earlier message versions until you sign out or close Cressida. Disabling this option clears them."
      );
      section("Expressions");
      toggle(
          "Emoji image links", m_preferences.emojiImageLinks, "emoji_image_links",
          "Send unsupported server emoji and stickers as image links. Other clients may show a link or image, "
          "depending on channel "
          "embed permissions. Reactions use Discord's normal rules."
      );
    } else if (m_settingsPage == "themes") {
      section("Color themes");
      row("Choose a theme", "Each theme includes dark and light colors. Choose the mode in Appearance.");
      if (std::ranges::find(m_themeSnapshot.themes, m_preferences.colorTheme, &ColorTheme::id)
          == m_themeSnapshot.themes.end())
        row("Theme unavailable", "Using Cressida until " + m_preferences.colorTheme + " is available again.");
      cursor += Style::spaceCompact;
      const int columns = width >= 480 ? 2 : 1;
      const float cardWidth = (width - (columns - 1) * 12) / columns;
      constexpr float cardHeight = 184, cardStep = cardHeight + Style::spaceMd;
      std::size_t index = 0;
      for (const auto& value : m_themeSnapshot.themes) {
        const float x = (index % columns) * (cardWidth + Style::spaceMd), y = cursor + (index / columns) * cardStep;
        const bool selected = m_preferences.colorTheme == value.id;
        auto tile = std::make_unique<RectNode>();
        tile->setPosition(x, y);
        tile->setSize(cardWidth, cardHeight);
        tile->setStyle(
            {.fill = panel,
             .border = selected ? accent : withAlpha(muted, .18F),
             .radius = Style::scaledRadius(Style::hoverRadius),
             .borderWidth = 1}
        );
        auto* card = p.addChild(std::move(tile));
        label(
            *card, 12, 10, cardWidth - Style::spaceXl, value.name, settings::rowTitle.size, foreground,
            settings::rowTitle.weight
        );
        label(*card, 12, 30, cardWidth - Style::spaceXl, value.description, Style::fontSizeMini, muted);
        const float previewWidth = (cardWidth - (Style::space2Xl - Style::spaceXxs)) / 2;
        for (const auto light : {false, true}) {
          const auto& colors = light ? value.light : value.dark;
          const float px = 10 + (light ? previewWidth + Style::spaceDense : 0);
          rectangle(*card, px, 51, previewWidth, 91, rgbHex(colors.background), Style::controlRadius);
          rectangle(*card, px, 51, 28, 91, rgbHex(colors.sidebar), Style::radiusSm);
          rectangle(*card, px, 51, 7, 91, rgbHex(colors.rail), Style::radiusSm);
          rectangle(*card, px + (Style::spaceDense + Style::borderWidth), 61, 12, 4, rgbHex(colors.muted), 1);
          rectangle(*card, px + Style::spaceDense, 76, 14, 10, rgbHex(colors.accent), Style::radiusXs);
          rectangle(*card, px + (Style::spaceMd + Style::borderWidth), 79, 8, 3, rgbHex(colors.ink), 1);
          rectangle(*card, px + (Style::spaceDense + Style::borderWidth), 93, 12, 3, rgbHex(colors.muted), 1);
          rectangle(*card, px + (Style::spaceDense + Style::borderWidth), 103, 9, 3, rgbHex(colors.muted), 1);
          const float chatX = px + ((Style::space2Xl + Style::spaceXxs) + Style::borderWidth),
                      chatWidth = previewWidth - ((Style::spaceSection + Style::spaceXxs) + Style::borderWidth);
          label(*card, chatX, 57, chatWidth, light ? "Light" : "Dark", Style::fontSizeDetail, rgbHex(colors.muted));
          rectangle(*card, chatX, 77, 12, 12, rgbHex(colors.function), Style::radiusMd);
          rectangle(
              *card, chatX + (Style::spaceSm + Style::borderWidth), 85, 4, 4, rgbHex(colors.success), Style::radiusXs
          );
          rectangle(
              *card, chatX + (Style::spaceLg + Style::spaceXxs), 77, chatWidth * .55F, 4, rgbHex(colors.accent), 1
          );
          label(
              *card, chatX + (Style::spaceLg + Style::spaceXxs), 84, chatWidth - (Style::spaceLg + Style::spaceXxs),
              "Hello!", Style::fontSizeDetail, rgbHex(colors.foreground)
          );
          rectangle(*card, chatX, 104, chatWidth, 15, rgbHex(colors.panel), Style::radiusXs);
          float codeX = chatX + (Style::spaceXs + Style::borderWidth);
          const float codeUnit = (chatWidth - Style::spaceLg) / 7;
          for (const auto color : {colors.keyword, colors.function, colors.string}) {
            rectangle(*card, codeX, 110, codeUnit * 2, 3, rgbHex(color), 1);
            codeX += codeUnit * 2 + 3;
          }
          rectangle(*card, chatX, 125, chatWidth, 10, rgbHex(colors.raised), Style::radiusXs);
          rectangle(
              *card, chatX + (Style::spaceXs + Style::borderWidth), 129, chatWidth * .5F, 2, rgbHex(colors.muted), 1
          );
        }
        label(*card, 12, 158, cardWidth - 122, value.bundled ? "Built in" : "Custom", Style::fontSizeDetail, muted);
        m_hits.push_back(
            {contentX + x, viewportY - previousScroll + y, cardWidth, cardHeight, Action::Preference,
             "palette:" + value.id}
        );
        settingsButton(
            *card, {cardWidth - 108, 148, 96, 26, Action::Preference, "palette:" + value.id},
            selected ? "Selected" : "Use theme", selected, false, true, true
        );
        ++index;
      }
      cursor += ((index + columns - 1) / columns) * cardStep;
      section("Custom themes");
      const auto directory = userThemeDirectory();
      const auto custom = actionRow(
          "Theme files",
          directory.empty()
              ? "The theme folder is unavailable. Set a home or configuration directory."
              : "Save the template as a .toml file in " + directory.string() + ". Changes reload automatically."
      );
      rowAction(custom, Action::Preference, "theme-template", "Copy template");
      if (!m_themeSnapshot.issues.empty()) {
        section("Theme file errors");
        for (const auto& issue : m_themeSnapshot.issues)
          row(issue.file, issue.message + " The last valid version stays active when available.");
      }
    } else if (m_settingsPage == "appearance") {
      section("Corners");
      localSlider(
          "Corner radius", "corner_radius",
          "Adjust rounding across panels, controls, menus and media. Zero gives square corners.",
          m_preferences.cornerRadius, 0, Style::cornerRadiusMaximumPercent, "%"
      );
      section("Color mode");
      int index = 0;
      const bool stacked = width < layout::Settings::stackThreshold;
      const float part = std::min(width, 100.0F);
      const int columns = std::clamp(static_cast<int>((width + 8) / (part + 8)), 1, 3);
      for (const auto& name : {"dark", "light", "system"}) {
        settingsButton(
            p,
            {(index % columns) * (part + 8), cursor + (index / columns) * 30, part, 26, Action::Preference,
             "theme:" + std::string(name)},
            name, false, m_preferences.theme == name, true, true
        );
        ++index;
      }
      cursor += ((3 + columns - 1) / columns) * 30 + 8;
      section("Text & interface");
      toggle(
          "Use 12-hour clock", m_preferences.twelveHourClock, "twelve_hour_clock",
          "Show message and inbox times with AM or PM. Times still use your system timezone."
      );
      const auto family = actionRow("Font family", "Choose an installed font for the interface and messages.");
      rowAction(
          family, Action::Preference, "font-picker",
          m_preferences.fontFamily.empty() ? "System default" : m_preferences.fontFamily
      );
      m_fontPickerAnchor = m_hits.back();
      const auto font = row("Message text size", "Change the size of text in conversations.", 196, 40, false, true);
      const float controlsX = stacked ? 0 : width - 180;
      const float fontY = stacked ? font.top + font.height - 40 : font.top + (font.height - 32) / 2;
      settingsButton(p, {controlsX, fontY, 36, 32, Action::Preference, "font-"}, "−", false, false, true, true);
      const auto fontSize = std::to_string(static_cast<int>(m_preferences.messageSize)) + " px";
      const auto fontMetrics =
          m_window.target().renderer().measureText(fontSize, Style::fontSizeCaption, FontWeight::Normal);
      label(
          p, controlsX + 90 - fontMetrics.width / 2, fontY + (32 - (fontMetrics.bottom - fontMetrics.top)) / 2,
          fontMetrics.width + 1, fontSize, Style::fontSizeCaption, foreground
      );
      settingsButton(p, {controlsX + 144, fontY, 36, 32, Action::Preference, "font+"}, "+", false, false, true, true);
      const auto zoom =
          row("Interface scale", "Resize the entire interface, including text and controls.", 196, 40, false, true);
      const float zoomY = stacked ? zoom.top + zoom.height - 40 : zoom.top + (zoom.height - 32) / 2;
      settingsButton(p, {controlsX, zoomY, 36, 32, Action::Preference, "zoom-"}, "−", false, false, true, true);
      settingsButton(
          p, {controlsX + Style::spaceGroup, zoomY, 84, 32, Action::Preference, "zoom0"},
          std::to_string(static_cast<int>(m_window.uiScale() * 100)) + "%", false, false, true, true
      );
      settingsButton(p, {controlsX + 144, zoomY, 36, 32, Action::Preference, "zoom+"}, "+", false, false, true, true);
      toggle("Compact message spacing", m_preferences.compact, "compact", "Reduce the space between message groups.");
      localSlider(
          "Message group spacing", "message_group_spacing",
          "Set the space between message groups. Compact spacing caps this at eight pixels.",
          m_preferences.messageGroupSpacing, 0, 24, " px"
      );
    } else if (m_settingsPage == "text") {
      section("Images and embeds");
      toggle(
          "Show link previews and embeds", m_preferences.showEmbeds, "embeds",
          "Display preview cards attached to messages."
      );
      toggle(
          "Show image attachments", m_preferences.showImages, "images",
          "Display attached images in chat. Hidden images remain available as links."
      );
      toggle(
          "Automatically play GIFs", m_preferences.autoplayGifs, "autoplay_gifs",
          "Play visible GIFs while this window is focused. When disabled, hover a GIF to play it."
      );
      section("Messages");
      toggle(
          "Show reactions", m_preferences.showReactions, "show_reactions",
          "Display the reaction buttons attached to chat messages."
      );
      toggle(
          "Convert emoticons", m_preferences.convertEmoticons, "convert_emoticons",
          "Convert familiar emoticons such as :) into emoji when sending a message."
      );
      localChoice(
          "Show spoiler content", "spoiler_mode", "Choose when hidden message text is revealed.",
          m_preferences.spoilerMode, {"When clicked", "In servers I moderate", "Always"}
      );
      section("Video and streams");
      accountToggle(
          "Always preview videos", "always_preview_video", "Sync whether Discord previews video before joining a call."
      );
      accountToggle(
          "Hide stream previews", "disable_stream_previews",
          "Prevent Discord from displaying previews of streams you share."
      );
      accountToggle(
          "Allow voice in Clips", "clips_voice_recording",
          "Allow your voice to be included when another Discord participant saves a Clip."
      );
    } else if (m_settingsPage == "presence") {
      section("Availability");
      for (const auto& status : {"online", "idle", "dnd", "invisible"}) {
        const auto item =
            row(std::string(presenceName(status)),
                std::string_view(status) == "invisible"
                    ? "Appear offline. Other active Discord sessions may still affect your status."
                    : "",
                44, 40, true);
        const float cy = item.top + (item.height - 12) / 2;
        rectangle(p, width - 14, cy, 12, 12, presenceColor(status), Style::radiusMd);
        if (m_presence.status == status)
          glyph(p, width - Style::spaceSection, cy - Style::spaceXxs, Style::baseGlyphSize, "check", accent);
        m_hits.push_back(
            {contentX, viewportY - previousScroll + item.top, width, item.height, Action::SetPresence, status}
        );
      }
      cursor += Style::spaceMd;
      field("Custom status", "custom_status", 36, "Displayed with your availability. Up to 128 characters.");
      settingsButton(p, {0, cursor, 124, 32, Action::SaveCustomStatus, {}}, "Save Status", true, false, true, true);
      cursor += Style::spaceSection;
    } else if (m_settingsPage == "privacy") {
      section("Direct messages");
      accountToggle(
          "Block DMs from new server members", "restrict_new_guild_dms",
          "Disable server-member direct messages in servers you join from now on. Existing server overrides stay "
          "unchanged."
      );
      accountToggle(
          "Disable message requests in new servers", "restrict_message_requests",
          "Apply to servers you join from now on. Availability can depend on Discord's age and regional requirements."
      );
      accountToggle(
          "Allow game friend DMs", "game_friend_dms",
          "Let Discord contacts connected through a game send direct messages."
      );
      section("Discovery and profile");
      accountToggle(
          "Contact discovery", "contact_sync", "Allow Discord contact syncing to help people find your account."
      );
      accountToggle(
          "Detect linked platform accounts", "detect_platform_accounts",
          "Allow Discord to detect supported platform accounts for connection suggestions."
      );
      accountToggle(
          "Hide legacy username badge", "hide_legacy_username",
          "Hide the legacy username badge on your Discord profile."
      );
      accountToggle("Show local time", "show_local_time", "Allow Discord to display your local time where supported.");
      accountToggle(
          "Hide friend request notes", "hide_friend_request_notes",
          "Hide the notes attached to incoming friend requests in Discord."
      );
      remoteChoice(
          "Profile visibility", "profile_visibility", "Choose who can see your Discord profile.",
          {"Discord default", "Friends only", "Friends and small servers", "Friends and all servers"}
      );
      section("Accounts and data");
      const auto people = actionRow(
          "Blocked and ignored accounts",
          "Review friends, pending requests and the accounts you have blocked or ignored."
      );
      rowAction(people, Action::SettingsPage, "relationships", "Manage accounts");
      const auto data = actionRow(
          "Data and privacy", "Choose how Discord uses your data and request a copy of your account information."
      );
      rowAction(data, Action::SettingsPage, "data", "Data & Privacy");
    } else if (m_settingsPage == "content") {
      section("Sensitive content");
      for (const auto& [suffix, title] : std::vector<std::pair<std::string, std::string>>{
               {"guild", "Server media"}, {"friend", "Media from friends"}, {"other", "Other direct-message media"}
           }) {
        remoteChoice(
            title, "explicit_" + suffix + "_media", "Choose how Discord handles media detected as sexually explicit.",
            {"Discord default", "Show", "Blur", "Block"}
        );
        remoteChoice(
            title + " with graphic content", "gore_" + suffix + "_media",
            "Choose how Discord handles media detected as graphic or violent.",
            {"Discord default", "Show", "Blur", "Block"}
        );
      }
      section("Language filters");
      accountToggle("Filter profanity", "filter_profanity", "Apply Discord's profanity filtering where supported.");
      accountToggle(
          "Filter sexual language", "filter_sexual", "Apply Discord's sexual-language filtering where supported."
      );
      accountToggle("Filter slurs", "filter_slurs", "Apply Discord's slur filtering where supported.");
      section("Direct messages");
      remoteChoice(
          "Spam filter", "dm_spam_filter", "Send suspected spam to Discord's spam inbox.",
          {"Discord default", "Do not filter", "Filter non-friends", "Filter everyone"}
      );
      accountToggle(
          "Conversation safety warnings", "conversation_warnings", "Enable Discord's safety warnings for conversations."
      );
    } else if (m_settingsPage == "friends") {
      section("Who can send you a friend request");
      const auto flags = m_settingsDraft.contains("friend_sources") ? m_settingsDraft.at("friend_sources") : 14;
      toggle("Everyone", flags & 8, "friend_sources:8", "Allow anyone to send you a friend request.", true);
      toggle(
          "Friends of friends", flags & 2, "friend_sources:2",
          "Allow requests from people who share a friend with you.", true
      );
      toggle(
          "Server members", flags & 4, "friend_sources:4", "Allow requests from people who share a server with you.",
          true
      );
    } else if (m_settingsPage == "notifications") {
      section("Cressida desktop notifications");
      toggle(
          "Desktop notifications", m_preferences.desktopNotifications, "desktop_notifications",
          "Show desktop notifications for eligible new messages while Cressida is running."
      );
      toggle(
          "Notification sounds", m_preferences.notificationSounds, "notification_sounds",
          "Play a sound for eligible desktop message notifications."
      );
      localChoice(
          "Notification content", "notification_privacy",
          "Choose how much message information appears in desktop notifications.", m_preferences.notificationPrivacy,
          {"Name and message", "Name only", "Hide names and messages"}
      );
      section("Discord notifications");
      accountToggle(
          "In-app notifications", "in_app_notifications",
          "Sync Discord's in-app notification preference to your account."
      );
      accountToggle("Quiet mode", "quiet_mode", "Sync Discord's quiet-mode preference for notification interruptions.");
      accountToggle(
          "Friends coming online", "friend_online_notifications",
          "Receive Discord notifications when friends come online."
      );
      accountToggle(
          "Voice activity", "voice_activity_notifications",
          "Receive Discord notifications about friends' voice activity."
      );
      accountToggle(
          "Profile updates", "profile_notifications",
          "Receive Discord notifications when friends update their profiles."
      );
      accountToggle(
          "Upcoming server events", "server_event_notifications",
          "Receive Discord reminders for upcoming server events."
      );
      accountToggle(
          "Returning friends", "returning_friend_notifications",
          "Receive Discord notifications when a friend returns after being away."
      );
      accountToggle(
          "Friend anniversaries", "friend_anniversary_notifications",
          "Receive Discord notifications for friendship anniversaries."
      );
      accountToggle("Game updates", "game_update_notifications", "Receive game-update notifications in Discord.");
      accountToggle(
          "Trending servers", "trending_server_notifications",
          "Receive Discord notifications about trending server activity."
      );
      accountToggle(
          "Reply reminders", "dm_reply_reminders", "Allow Discord reminders for direct messages you have not answered."
      );
      accountToggle(
          "Summary reminders", "summary_reminders", "Allow Discord reminders with summaries of activity you missed."
      );
      accountToggle(
          "Friends' games", "friend_game_notifications", "Receive Discord notifications about friends' games."
      );
      accountToggle(
          "Downtime notifications", "downtime_notifications",
          "Allow Discord notifications during periods when you have been inactive."
      );
      remoteChoice(
          "Game activity notifications", "game_activity_notifications",
          "Choose how Discord notifies you about friends' game activity.",
          {"Discord default", "Disabled", "Enabled", "Only games played"}
      );
      remoteChoice(
          "Custom status notifications", "custom_status_notifications",
          "Choose which custom-status updates trigger Discord notifications.",
          {"Discord default", "Enabled", "Disabled"}
      );
      accountToggle(
          "Exclude Steam activity", "exclude_steam_notifications",
          "Exclude Steam activity from Discord's game notifications."
      );
      section("Reactions and streams");
      remoteChoice(
          "Reaction notifications", "reaction_notifications", "Choose which reactions Discord notifies you about.",
          {"All conversations", "Direct messages only", "Off"}
      );
      accountToggle(
          "Super reaction notifications", "burst_reaction_notifications",
          "Receive Discord notifications about Super Reactions."
      );
      accountToggle(
          "Group reaction notifications", "group_reaction_notifications",
          "Receive Discord reaction notifications in group conversations."
      );
      accountToggle(
          "Stream notifications", "stream_notifications", "Receive Discord notifications when people start streaming."
      );
      accountToggle(
          "Server stream notifications", "notify_server_stream",
          "Receive Discord notifications when a stream starts in a server."
      );
      {
        const std::vector<int> values{60, 300, 600, 900, 1800, 3600};
        const auto current = m_settingsDraft.contains("afk_timeout") ? m_settingsDraft.at("afk_timeout") : 600;
        auto choices =
            std::vector<std::string>{"1 minute", "5 minutes", "10 minutes", "15 minutes", "30 minutes", "1 hour"};
        auto seconds = values;
        const auto found = std::ranges::find(seconds, current);
        const auto selected =
            found == seconds.end() ? seconds.size() : static_cast<std::size_t>(found - seconds.begin());
        if (found == seconds.end()) {
          seconds.push_back(current);
          choices.push_back(std::to_string(current) + " seconds");
        }
        form.choose(
            "remote:afk_timeout", "Push notification inactivity",
            "Choose how long a Discord desktop client must be inactive before Discord sends mobile push notifications.",
            std::move(choices), selected,
            [this, seconds](std::size_t index) {
              activate({0, 0, 0, 0, Action::AccountPreference, "afk_timeout:" + std::to_string(seconds[index])});
              m_window.invalidate();
            },
            m_settingsLoaded && !m_loadingSettings && !m_savingSettings
        );
      }
      section("Notify my friends");
      accountToggle(
          "When I stream", "notify_friends_stream", "Let Discord notify your friends when you start streaming."
      );
      accountToggle(
          "When I update my profile", "notify_friends_profile",
          "Let Discord notify your friends when your profile changes."
      );
      accountToggle(
          "When I come online", "notify_friends_online", "Let Discord notify your friends when you become available."
      );
    } else if (m_settingsPage == "activity") {
      section("Activity sharing");
      accountToggle(
          "Share current activity", "show_activity",
          "Share activities published to Discord. Cressida does not currently detect running games."
      );
      accountToggle("Show recent games", "recent_games", "Show recently played games on your Discord profile.");
      accountToggle(
          "Hide from leaderboards", "leaderboard_opt_out", "Exclude your activity from Discord's game leaderboards."
      );
      section("Activity access");
      accountToggle("Allow friends to join", "join_friends", "Let friends join the activities you share on Discord.");
      accountToggle(
          "Allow voice participants to join", "join_voice",
          "Let people in your voice channel join your activities on Discord."
      );
    } else if (m_settingsPage == "keybinds") {
      section("Application shortcuts");
      for (const auto& [id, title] :
           {std::pair{"settings", "Open settings"},
            {"search", "Search this conversation"},
            {"jump_latest", "Jump to latest message"},
            {"toggle_members", "Toggle member list"},
            {"push_to_talk", "Push to talk"},
            {"inbox", "Open inbox"},
            {"next_channel", "Next channel"},
            {"previous_channel", "Previous channel"},
            {"next_server", "Next server"},
            {"previous_server", "Previous server"},
            {"quit", "Quit Cressida"},
            {"zoom_in", "Zoom in"},
            {"zoom_out", "Zoom out"},
            {"zoom_reset", "Reset zoom"}}) {
        const bool capturing = m_shortcutCapture == id;
        const auto item =
            row(title,
                capturing ? "Press a shortcut. Escape cancels; Backspace clears it."
                          : "Choose a shortcut that works while Cressida is focused.",
                208, 40, false, true);
        const float controlsY =
            width < layout::Settings::stackThreshold ? item.top + item.height - 38 : item.top + (item.height - 26) / 2;
        const float controlsX = width < layout::Settings::stackThreshold ? 0 : width - 196;
        settingsButton(
            p, {controlsX, controlsY, 124, 26, Action::Preference, "keybind-record:" + std::string(id)},
            capturing ? "Recording…" : shortcutLabel(id), capturing, false, true, true
        );
        settingsButton(
            p, {controlsX + 132, controlsY, 64, 26, Action::Preference, "keybind-reset:" + std::string(id)}, "Reset",
            false, false, true, true
        );
      }
      rowAction(
          actionRow("Reset shortcuts", "Restore Cressida's default application shortcuts."), Action::Preference,
          "keybind-reset-all", "Reset all"
      );
      section("Text editing");
      for (const auto& [key, title] :
           {std::pair{"Enter / Shift + Enter", "Send message / new line"},
            {"Ctrl + A / C / X / V", "Select all / copy / cut / paste"},
            {"Ctrl + Z / Shift + Ctrl + Z", "Undo / redo"},
            {"Ctrl + ← / →", "Move by word"},
            {"Ctrl + Backspace / Delete", "Delete previous / next word"}})
        row(title, key);
    } else if (m_settingsPage == "accessibility") {
      section("Motion");
      toggle(
          "Reduce motion", m_preferences.reducedMotion, "reduced_motion",
          "Limit interface motion. Media stays still unless you enable an override below."
      );
      toggle(
          "Allow GIFs with reduced motion", m_preferences.gifsWithReducedMotion, "gifs_with_reduced_motion",
          "Let message GIFs and GIF previews animate with reduced motion. GIF autoplay still applies."
      );
      toggle(
          "Allow emoji with reduced motion", m_preferences.emojiWithReducedMotion, "emoji_with_reduced_motion",
          "Let custom emoji animate with reduced motion when Animate emoji is enabled."
      );
      toggle(
          "Animate emoji", m_preferences.animateEmoji, "animate_emoji",
          "Play animated emoji, subject to reduced motion and its emoji override."
      );
      toggle(
          "Animate avatars", m_preferences.animateAvatars, "animate_avatars",
          "Play animated avatars, profile cosmetics and server images when reduced motion is off."
      );
      toggle(
          "Automatically play GIFs", m_preferences.autoplayGifs, "autoplay_gifs",
          "Play visible message GIFs automatically. Reduced motion requires the GIF override above."
      );
      section("Color and readability");
      localSlider(
          "Saturation", "saturation", "Reduce the intensity of interface colors without changing image content.",
          m_preferences.saturation, 0, 100, "%"
      );
      localChoice(
          "Role colors", "role_color_mode", "Show server role colors on names, beside names as dots, or hide them.",
          m_preferences.roleColorMode, {"Colored names", "Color dots", "Off"}
      );
      rowAction(
          actionRow("Text and interface size", "Choose a font, message text size and interface scale."),
          Action::SettingsPage, "appearance", "Appearance"
      );
    } else if (m_settingsPage == "advanced") {
      section("Developer tools");
      toggle(
          "Developer mode", m_preferences.developerMode, "developer_mode",
          "Show identifier-copy actions in supported user, server, channel and message menus."
      );
      section("Window");
      toggle(
          "Desktop decorations", m_preferences.desktopDecorations, "desktop_decorations",
          m_window.desktopDecorationsAvailable() ? "Use your desktop's window decorations. Turn off to request "
                                                   "Cressida's title bar. Your desktop may override this choice."
                                                 : "Prefer your desktop's window decorations when available. This "
                                                   "desktop does not provide them, so Cressida's title bar is used."
      );
      drawWindowControlSettings(form);
    } else if (m_settingsPage == "voice") {
      section("Voice and video");
      drawVoiceSettings(form);
      section("Video preferences");
      accountToggle(
          "Preview video before joining", "always_preview_video",
          "Show a camera preview before joining video calls in Discord clients that support calling."
      );
      accountToggle(
          "Disable stream previews", "disable_stream_previews",
          "Hide automatic stream previews in Discord clients that support streaming."
      );
      accountToggle(
          "Record voice in clips", "clips_voice_recording",
          "Allow your voice to be included in Discord clips where recording is supported."
      );
    } else if (m_settingsPage == "language") {
      section("Interface language");
      remoteChoice(
          "Discord account language", "locale",
          "Choose the language Discord uses for account content and supported communications. Cressida's native "
          "interface currently remains English.",
          {"English (United States)",
           "English (United Kingdom)",
           "Български",
           "中文（中国）",
           "中文（台灣）",
           "Hrvatski",
           "Čeština",
           "Dansk",
           "Nederlands",
           "Suomi",
           "Français",
           "Deutsch",
           "Ελληνικά",
           "हिन्दी",
           "Magyar",
           "Bahasa Indonesia",
           "Italiano",
           "日本語",
           "한국어",
           "Lietuvių",
           "Norsk",
           "Polski",
           "Português do Brasil",
           "Română",
           "Русский",
           "Español (España)",
           "Español (Latinoamérica)",
           "Svenska",
           "ไทย",
           "Türkçe",
           "Українська",
           "Tiếng Việt"}
      );
    } else if (m_settingsPage == "games") {
      section("Registered games");
      row("Game detection",
          "Cressida does not currently scan running processes or publish game activity. Activities shared by another "
          "Discord client remain visible according to your Activity Privacy preferences.");
      rowAction(
          actionRow("Activity sharing", "Choose who can see activities published to your Discord account."),
          Action::SettingsPage, "activity", "Activity Privacy"
      );
    } else if (m_settingsPage == "overlay") {
      section("Game overlay");
      row("In-game overlay",
          "This Cressida build does not inject an overlay into games. Chat remains available in the native desktop "
          "window.");
      rowAction(
          actionRow("Desktop notifications", "Choose notification visibility and sounds while Cressida is running."),
          Action::SettingsPage, "notifications", "Notifications"
      );
    }
    finishCategory();
    const float pageHeight = cursor + Style::spaceSm;
    m_scrolling.limit(m_settingsScroll, std::max(0.0F, pageHeight - viewportH));
    page->setTextScrollOffset(-m_settingsScroll);
    bodyNode->setPosition(layout::Settings::contentInset, -m_settingsScroll);
    const float correction = previousScroll - m_settingsScroll;
    for (auto i = firstHit; i < m_hits.size(); ++i) {
      auto& hit = m_hits[i];
      const auto left = std::max(hit.x, contentLeft), right = std::min(hit.x + hit.width, contentLeft + contentWidth);
      hit.x = left;
      hit.width = std::max(0.0F, right - left);
      hit.y += correction;
      const float end = std::min(viewportY + viewportH, hit.y + hit.height);
      hit.y = std::max(hit.y, viewportY);
      hit.height = std::max(0.0F, end - hit.y);
    }
    m_fontPickerAnchor.y += correction;
    root.addChild(std::move(page));
    settingsScrollbar(
        root, "settings-scrollbar", {frame.contentScrollbarX, viewportY, layout::Settings::scrollbarWidth, viewportH},
        pageHeight, m_settingsScroll
    );
    if (unsaved) {
      const float y = h - Style::spaceXl - saveHeight - (statusHeight ? statusHeight + Style::spaceSm : 0);
      rectangle(root, contentX, y, width, saveHeight, panel, Style::hoverRadius);
      label(
          root, contentX + Style::spaceMd, y + Style::cardPadding,
          width < layout::Settings::stackThreshold ? width - Style::spaceXl : width - 236, "Unsaved changes",
          Style::fontSizeDescription, foreground
      );
      const float buttonY = y + saveHeight - Style::spaceSection, saveWidth = std::min(124.0F, width * .55F),
                  resetWidth = std::min(76.0F, width * .35F);
      settingsButton(
          root,
          {contentX + width - saveWidth - resetWidth - Style::spaceXl,
           buttonY,
           resetWidth,
           32,
           m_savingSettings ? Action::None : Action::ResetAccountSettings,
           {}},
          "Reset", false, false, true, true
      );
      settingsButton(
          root,
          {contentX + width - saveWidth - Style::spaceMd,
           buttonY,
           saveWidth,
           32,
           m_savingSettings ? Action::None : Action::SaveAccountSettings,
           {}},
          m_savingSettings ? "Saving…" : "Save Changes", true, false, true, true
      );
    }
    if (statusHeight) {
      rectangle(root, contentX, h - Style::spaceXl - statusHeight, width, statusHeight, panel, Style::hoverRadius);
      label(
          root, contentX + Style::spaceMd, h - Style::cardPadding - statusHeight, width - Style::spaceXl,
          m_settingsStatus, settings::description.size, muted, settings::description.weight, 0
      );
    }
    drawFontPicker(root, w, h);
  }
  void ClientApplication::demoFontStep() {
    const auto require = [](bool ok, const char* message) {
      if (!ok)
        throw std::runtime_error(message);
    };
    const auto click = [&](const std::string& id) {
      const auto at = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == Action::Preference && hit.id == id && hit.height > 0;
      });
      require(at != m_hits.end(), "Font setting action must be visible");
      const auto hit = *at;
      m_window.click(
          hit.x + hit.width / 2, hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          272
      );
      m_window.released();
    };
    const auto drag = [&](const std::string& id) {
      const auto* bar = static_cast<Scrollbar*>(m_widgets.at(id));
      require(bar->visible(), "Overflowing settings lists require a native scrollbar");
      const auto* thumb = bar->children()[1].get();
      const float x = bar->x() + bar->width() / 2;
      const float y =
          bar->y() + thumb->y() + thumb->height() / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.motion(x, y + Style::spaceSection);
      require(m_controlInput.pointerCaptured(), "Settings scrollbar captures the pointer while dragging");
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      click("font-picker");
      // Exercise virtualization even on CI images with only a few fonts. This
      // metadata belongs only to the offline replay; real discovery is tested
      // independently. Keep a real family first for the selection/metrics check.
      if (m_installedFonts.empty())
        m_installedFonts.push_back("sans-serif");
      for (std::size_t i = 0; m_installedFonts.size() < layout::FontPicker::replayRows; ++i)
        m_installedFonts.push_back("Cressida replay family " + std::to_string(i));
      m_fontMatches = m_installedFonts;
    } else if (step == 1) {
      require(m_fontPickerOpen && m_inputs.contains("settings:font_search"), "Picker retains a native search field");
      require(
          m_fontPickerBounds.height <= layout::FontPicker::maximumViewportHeight + layout::FontPicker::chromeHeight,
          "Font choices stay inside a bounded picker"
      );
      require(
          std::cmp_less_equal(
              std::ranges::count_if(
                  m_hits,
                  [](const auto& hit) { return hit.action == Action::Preference && hit.id.starts_with("font-family:"); }
              ),
              layout::FontPicker::maximumPreparedRows + 1
          ),
          "Font picker only builds a bounded option window"
      );
      drag("font-picker-scrollbar");
    } else if (step == 2) {
      require(
          m_fontPickerScroll > 0 && m_settingsScroll == m_fontPickerReturnScroll,
          "Font scrollbar moves the picker independently of the settings page"
      );
      require(m_controlInput.pointerCaptured(), "Font scrollbar capture survives scene reconstruction");
      m_window.motion(m_pointerX, m_window.height() * 2);
      m_window.released();
      focusControl("settings:font_search");
      pasteControl("settings:font_search", m_installedFonts.front());
      require(!m_profileDirty, "Font search must not dirty account profile settings");
    } else if (step == 3) {
      require(
          m_settingsFields["font_search"] == m_installedFonts.front() && m_fontPickerScroll == 0,
          "Native font search updates the filter and resets its scrollbar"
      );
      click("font-family:" + m_installedFonts.front());
    } else if (step == 4) {
      require(!m_fontPickerOpen && m_preferences.fontFamily == m_installedFonts.front(), "Font selection applies");
      auto& renderer = m_window.target().renderer();
      const auto normal = renderer.measureText("Font preview Wi", Style::fontSizeBody);
      const auto chosen = renderer.measureText(
          "Font preview Wi", Style::fontSizeBody, FontWeight::Normal, 0, 0, TextAlign::Start, m_preferences.fontFamily
      );
      require(std::abs(normal.width - chosen.width) < .01F, "Selected font drives default text measurements");
      click("font-picker");
      m_settingsFields["font_search"] = "no-font-family-cressida-987654321";
    } else if (step == 5) {
      require(
          std::ranges::none_of(
              m_hits,
              [](const auto& hit) {
                return hit.action == Action::Preference
                    && hit.id.starts_with("font-family:")
                    && hit.id != "font-family:";
              }
          ),
          "Unmatched font search must hide installed family choices"
      );
      click("font-family:");
    } else if (step == 6) {
      require(m_preferences.fontFamily.empty() && !m_fontPickerOpen, "System default clears the saved family");
      const auto before = m_window.target().renderer().measureText("Font preview Wi", Style::fontSizeBody);
      const auto system = m_window.target().renderer().measureText(
          "Font preview Wi", Style::fontSizeBody, FontWeight::Normal, 0, 0, TextAlign::Start, "sans-serif"
      );
      require(std::abs(before.width - system.width) < .01F, "Reset restores system default measurements");
      click("font-picker");
    } else if (step == 7) {
      settingsKey({.symbol = XKB_KEY_Escape});
      require(!m_fontPickerOpen && m_settingsOpen, "Escape closes the font picker before settings");
      drag("settings-nav-scrollbar");
    } else if (step == 8) {
      require(
          m_settingsNavScroll > 0 && m_controlInput.pointerCaptured(),
          "Navigation scrollbar drags and retains capture across scene reconstruction"
      );
      m_window.motion(m_pointerX, m_window.height() * 2);
      m_window.released();
      selectSettingsPage("notifications");
      m_window.setUiScale(1.8F);
    } else if (step == 9) {
      drag("settings-scrollbar");
    } else if (step == 10) {
      require(
          m_settingsScroll > 0 && m_controlInput.pointerCaptured(),
          "Settings page scrollbar drags and retains capture across scene reconstruction"
      );
      m_window.motion(m_pointerX, m_window.height() * 2);
      m_window.released();
      selectSettingsPage("cressida");
    } else if (step == 11) {
      require(!m_fontPickerOpen && m_installedFonts.empty(), "Page navigation clears the font catalog");
    }
    std::clog << "Offline font settings replay step " << step + 1 << " passed\n";
    m_window.invalidate();
  }
  void ClientApplication::demoThemeStep() {
    const auto require = [](bool ok, const char* message) {
      if (!ok)
        throw std::runtime_error(message);
    };
    const auto click = [&](const std::string& id) {
      const auto found = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == Action::Preference && hit.id == id && hit.height > 0;
      });
      require(found != m_hits.end(), "Theme control must be visible");
      const auto hit = *found;
      m_window.click(
          hit.x + hit.width / 2, hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          272
      );
      m_window.released();
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(m_themeSnapshot.themes.size() >= 4, "Bundled TOML themes must be available offline");
      click("palette:catppuccin");
    } else if (step == 1) {
      require(
          m_preferences.colorTheme == "catppuccin" && background == rgbHex(0x1E1E2E),
          "Selecting Catppuccin applies Mocha"
      );
      selectSettingsPage("appearance");
    } else if (step == 2) {
      require(
          std::ranges::count_if(
              m_hits, [](const auto& hit) { return hit.action == Action::Preference && hit.id.starts_with("theme:"); }
          ) == 3,
          "Appearance offers exactly dark, light and system modes"
      );
      click("theme:light");
    } else if (step == 3) {
      require(
          m_preferences.colorTheme == "catppuccin" && lightTheme && background == rgbHex(0xEFF1F5),
          "Light mode applies Latte without changing the selected theme"
      );
      selectSettingsPage("themes");
    } else if (step == 4) {
      click("palette:rose-pine");
    } else if (step == 5) {
      require(lightTheme && background == rgbHex(0xFAF4ED), "Rosé Pine uses Dawn in light mode");
      selectSettingsPage("appearance");
      activate({0, 0, 0, 0, Action::Preference, "theme:system"});
      m_themeSnapshot.systemLight = false;
    } else if (step == 6) {
      require(!lightTheme && background == rgbHex(0x191724), "System dark uses the same theme's dark variant");
      m_themeSnapshot.systemLight = true;
    } else if (step == 7) {
      require(lightTheme && background == rgbHex(0xFAF4ED), "A desktop preference change switches the active variant");
      selectSettingsPage("themes");
      activate({0, 0, 0, 0, Action::Preference, "palette:../invalid"});
      require(m_preferences.colorTheme == "rose-pine", "Unknown palette IDs cannot replace the selection");
    } else if (step == 8) {
      click("palette:gruvbox");
    } else if (step == 9) {
      require(lightTheme && background == rgbHex(0xFBF1C7), "Gruvbox uses its warm light palette");
      m_themeSnapshot.systemLight = false;
    } else if (step == 10) {
      require(!lightTheme && background == rgbHex(0x282828), "Gruvbox also follows system dark mode");
    }
    std::clog << "Offline theme settings replay step " << step + 1 << " passed\n";
    m_window.invalidate();
  }
  void ClientApplication::settingsKey(const Key& key) {
    if (key.symbol == XKB_KEY_Escape) {
      if (m_fontPickerOpen) {
        fontPreference("font-picker");
        return;
      }
      activate({0, 0, 0, 0, Action::CloseSettings, {}});
      m_window.invalidate();
    }
  }
} // namespace app
