#include "app/client_application.h"
#include "app/message_group.h"
#include "app/theme.h"
#include "render/core/texture_manager.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::demoDecorationsStep() {
    const auto require = [](bool condition, const char* message) {
      if (!condition)
        throw std::runtime_error(message);
    };
    const auto control = [&]() -> Toggle& {
      const auto at = m_widgets.find("toggle:desktop_decorations");
      require(
          at != m_widgets.end() && at->second->parent(),
          "Advanced settings mounts a retained native desktop-decoration toggle"
      );
      return *static_cast<Toggle*>(at->second);
    };
    const auto click = [&] {
      auto& toggle = control();
      float x = 0, y = 0;
      Node::mapToScene(&toggle, toggle.width() / 2, toggle.height() / 2, x, y);
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      m_window.released();
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(m_preferences.desktopDecorations && control().checked(), "Desktop decorations start enabled");
      m_profileDirty = false;
      click();
      require(
          !m_preferences.desktopDecorations && !m_window.desktopDecorations() && !m_profileDirty,
          "Clicking the native toggle requests Cressida decorations without dirtying a profile"
      );
    } else if (step == 1) {
      require(
          !control().checked() && !m_window.desktopDecorations(),
          "The decoration choice survives a retained settings rebuild"
      );
      if (!m_window.desktopDecorationsAvailable())
        require(
            m_window.titlebarHeight() == layout::Window::titlebarHeight,
            "Compositors without xdg-decoration retain Cressida's working title bar"
        );
      click();
      require(
          m_preferences.desktopDecorations && m_window.desktopDecorations(),
          "The native toggle can request desktop decorations again"
      );
    } else if (step == 2) {
      require(control().checked(), "Re-enabled decorations remain checked on the following frame");
      selectSettingsPage("appearance");
      selectSettingsPage("advanced");
    } else if (step == 3) {
      require(
          control().checked() && m_preferences.desktopDecorations && !m_profileDirty,
          "Settings navigation preserves the decoration preference without profile mutations"
      );
    }
    m_window.invalidate();
  }
  void ClientApplication::demoUserPreferencesStep() {
    const auto require = [](bool ok, const char* reason) {
      if (!ok)
        throw std::runtime_error(reason);
    };
    const auto click = [&](const std::string& id) {
      const auto found = m_widgets.find(id);
      require(found != m_widgets.end() && found->second->parent(), "Preference control is retained and attached");
      float x = found->second->width() / 2, y = found->second->height() / 2;
      for (const auto* node = found->second; node; node = node->parent()) {
        x += node->x();
        y += node->y();
      }
      m_window.click(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272);
      m_window.released();
    };
    const auto record = [&](const std::string& name) {
      click("settings-button:" + std::to_string(static_cast<int>(Action::Preference)) + ":keybind-record:" + name);
    };
    const auto step = m_demoInteractionStep++;
    std::clog << "Offline client preference frame " << step << '\n';
    switch (step) {
    case 0:
      require(m_settingsPage == "keybinds", "Shortcuts replay starts on its native settings page");
      record("settings");
      break;
    case 1:
      require(m_shortcutCapture == "settings", "Record button begins keyboard capture");
      onKey({.symbol = XKB_KEY_s, .control = true, .alt = true});
      require(
          m_shortcutCapture.empty() && shortcutLabel("settings") == "Ctrl+Alt+s",
          "Native keyboard recording saves a chord"
      );
      break;
    case 2:
      activate({0, 0, 0, 0, Action::CloseSettings, {}});
      onKey({.symbol = XKB_KEY_comma, .control = true});
      require(!m_settingsOpen, "The old shortcut stops firing after remapping");
      onKey({.symbol = XKB_KEY_s, .control = true, .alt = true});
      require(m_settingsOpen, "The recorded shortcut opens settings");
      selectSettingsPage("keybinds");
      break;
    case 3:
      record("settings");
      onKey({.symbol = XKB_KEY_f, .control = true});
      require(
          m_shortcutCapture == "settings" && m_settingsStatus.find("already assigned") != std::string::npos,
          "Shortcut conflicts retain the preceding binding and recording state"
      );
      onKey({.symbol = XKB_KEY_Escape});
      require(m_shortcutCapture.empty() && m_settingsOpen, "Escape cancels recording before closing settings");
      break;
    case 4:
      record("settings");
      onKey({.symbol = XKB_KEY_BackSpace});
      require(shortcutLabel("settings") == "Unassigned", "Backspace clears the captured shortcut");
      extendedPreference("keybind-reset:settings");
      require(shortcutLabel("settings") == "Ctrl+comma", "Reset restores the original shortcut");
      selectSettingsPage("accessibility");
      break;
    case 5:
      click("toggle:reduced_motion");
      require(m_preferences.reducedMotion, "Reduced motion toggles through the native settings control");
      extendedPreference("saturation:0");
      extendedPreference("role_color_mode:1");
      break;
    case 6:
      require(
          std::abs(theme::accent.r - theme::accent.g) < .0001F && std::abs(theme::accent.g - theme::accent.b) < .0001F,
          "Saturation changes the actual active interface palette"
      );
      require(m_preferences.roleColors && m_preferences.roleColorMode == 1, "Color dots retain role metadata");
      extendedPreference("role_color_mode:2");
      require(!m_preferences.roleColors, "Disabling role colors updates the shared renderer preference");
      extendedPreference("show_reactions");
      require(
          reactionStrip(nullptr, m_messages.front(), 0, 0, 400) == 0,
          "The reaction preference removes rendered strips and their reserved height"
      );
      extendedPreference("spoiler_mode:2");
      require(spoilersVisible("not-explicitly-revealed"), "Always-visible spoilers affect actual message rendering");
      break;
    case 7:
      extendedPreference("message_group_spacing:4");
      require(
          messageRowPadding(true, false, false, m_preferences.messageGroupSpacing) == 4
              && messageRowPadding(true, true, false, m_preferences.messageGroupSpacing) == 0,
          "Group spacing preserves zero space between continuation rows"
      );
      extendedPreference("keybind-record:settings");
      onKey({.symbol = XKB_KEY_x, .text = "x"});
      require(!m_shortcutCapture.empty(), "Unmodified typing is rejected as a global shortcut");
      onKey({.symbol = XKB_KEY_Escape});
      selectSettingsPage("notifications");
      break;
    case 8:
      click("toggle:desktop_notifications");
      require(!m_preferences.desktopNotifications, "Desktop notification control changes real delivery gating");
      extendedPreference("notification_privacy:2");
      require(m_preferences.notificationPrivacy == 2, "Notification privacy stores the selected disclosure level");
      break;
    case 9:
      extendedPreference("saturation:100");
      extendedPreference("reduced_motion");
      extendedPreference("show_reactions");
      extendedPreference("role_color_mode:0");
      extendedPreference("spoiler_mode:0");
      require(
          !m_preferences.reducedMotion && m_preferences.showReactions && m_preferences.roleColors,
          "Client preferences can be restored independently"
      );
      selectSettingsPage("privacy");
      break;
    case 10:
      require(theme::accent == rgbHex(theme::palette.accent), "Restoring saturation restores exact theme colors");
      focusControl("settings:search");
      m_profileDirty = false;
      onKey({.symbol = XKB_KEY_p, .text = "p"});
      require(
          !m_profileDirty && m_settingsFields["search"] == "p", "Native settings search never edits account profiles"
      );
      break;
    case 11:
      extendedPreference("keybind-reset-all");
      require(m_preferences.keybinds.empty() && m_shortcutCapture.empty(), "Reset all restores default keybindings");
      m_window.invalidate();
      break;
    }
    m_window.invalidate();
  }
  void ClientApplication::demoReducedMotionStep() {
    const auto require = [](bool ok, const char* message) {
      if (!ok)
        throw std::runtime_error(message);
    };
    const std::array<std::string, 5> paths{
        "https://static.klipy.com/ii/cressida-motion-demo.gif", "/emojis/99001.gif?size=128",
        "/avatars/99002/a_motion.gif", "/banners/99002/a_motion.gif",
        "https://cdn.discordapp.com/assets/content/cressida-motion-demo"
    };
    constexpr auto frameDuration = std::chrono::milliseconds(80);
    const auto click = [&](const std::string& id) {
      const auto found = m_widgets.find("toggle:" + id);
      require(found != m_widgets.end() && found->second->parent(), "Motion setting has an attached native toggle");
      auto& toggle = *static_cast<Toggle*>(found->second);
      float x = 0, y = 0;
      Node::mapToScene(&toggle, toggle.width() / 2, toggle.height() / 2, x, y);
      m_window.motion(x, y);
      m_window.click(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, BTN_LEFT);
      m_window.released();
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(
          !m_preferences.gifsWithReducedMotion && !m_preferences.emojiWithReducedMotion,
          "Reduced-motion media overrides require an explicit opt-in"
      );
      const int size = static_cast<int>(Style::controlHeightSm);
      for (const auto& path : paths) {
        auto animation = std::make_shared<media::Animation>();
        animation->width = animation->height = size;
        animation->duration = frameDuration * 2;
        for (int frame = 0; frame < 2; ++frame)
          animation->frames.push_back(
              {.rgba = std::vector<std::uint8_t>(size * size * 4, frame ? 255 : 64), .duration = frameDuration}
          );
        auto& image = m_images[path];
        image.animation = animation;
        image.texture =
            m_window.context().textureManager().loadFromRgba(animation->frames.front().rgba.data(), size, size);
        require(image.texture.valid(), "Offline media fixtures have actual native textures");
      }
      click("reduced_motion");
    } else if (step == 1)
      click("gifs_with_reduced_motion");
    else if (step == 2)
      click("emoji_with_reduced_motion");
    else if (step == 3)
      click("animate_emoji");
    else if (step == 4)
      click("gifs_with_reduced_motion");
    else if (step == 5) {
      click("gifs_with_reduced_motion");
      click("animate_emoji");
    } else if (step == 6)
      click("autoplay_gifs");
    else if (step == 8)
      click("gifs_with_reduced_motion");
    else if (step == 9)
      click("reduced_motion");
    else if (step == 10)
      click("reduced_motion");
    else if (step == 11)
      click("emoji_with_reduced_motion");

    require(m_preferences.reducedMotion == (step != 9), "Media overrides preserve reduced interface motion");
    const std::array<bool, reducedMotionReplayFrames> gifPlaying{false, true, true,  true, false, true,
                                                                 false, true, false, true, false, false};
    const std::array<bool, reducedMotionReplayFrames> emojiPlaying{false, false, true, false, false, true,
                                                                   true,  true,  true, true,  true,  false};
    const std::array<bool, 5> expected{gifPlaying[step], emojiPlaying[step], step == 9, false, step == 9};
    Node probe;
    probe.setPosition(Style::spaceSection * 2, Style::spaceSection * 3);
    const float size = Style::controlHeightSm;
    m_pointerX = step >= 7 && step <= 9 ? probe.x() + size / 2 : -1;
    m_pointerY = step >= 7 && step <= 9 ? probe.y() + size / 2 : -1;
    m_animationDelay = std::chrono::milliseconds::max();
    for (std::size_t i = 0; i < paths.size(); ++i) {
      const auto use = i == 3 ? ImageUse::ProfileBanner : i == 4 ? ImageUse::ProfileEffect : ImageUse::Content;
      require(
          picture(
              probe, i * (size + Style::spaceMd), 0, size, size, paths[i], 0, false, false, ImageFitMode::Contain,
              frameDuration, use
          ),
          "Visible media fixtures use the real shared picture renderer"
      );
      const auto& image = m_images.at(paths[i]);
      if (image.animationFrame != static_cast<std::size_t>(expected[i])
          || (image.playingDraw == m_animationDraw) != expected[i])
        throw std::runtime_error(
            "Reduced motion frame " + std::to_string(step) + " has unexpected playback for " + paths[i]
        );
    }
    require(
        (m_animationDelay != std::chrono::milliseconds::max())
            == std::ranges::any_of(expected, [](bool value) { return value; }),
        "Only permitted visible media schedules another animation frame"
    );
    if (step == reducedMotionReplayFrames - 1)
      require(
          !m_preferences.gifsWithReducedMotion && !m_preferences.emojiWithReducedMotion,
          "Turning both overrides off restores still media"
      );
    std::clog << "Offline reduced motion replay step " << step + 1 << " passed\n";
    m_window.invalidate();
  }
} // namespace app
