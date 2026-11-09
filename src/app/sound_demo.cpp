#include "app/client_application.h"
#include "ui/controls/button.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"

#include <algorithm>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::demoSoundStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto widget = [&](const std::string& id) -> Node& {
      const auto found = m_widgets.find(id);
      require(found != m_widgets.end() && found->second->parent(), "Sound controls remain mounted across redraws");
      return *found->second;
    };
    const auto click = [&](const std::string& id) {
      auto& node = widget(id);
      float x = 0, y = 0;
      Node::mapToScene(&node, node.width() / 2, node.height() / 2, x, y);
      y += m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      m_window.released();
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(m_preferences.sounds.pack == "soft" && m_preferences.notificationSounds, "Soft and sounds default on");
      click("toggle:notification_sounds");
    } else if (step == 1) {
      require(!m_preferences.notificationSounds, "The native master toggle disables sounds");
      click("toggle:notification_sounds");
      static_cast<Select&>(widget("user-account-select:sound-pack")).setSelectedIndex(1);
    } else if (step == 2) {
      require(m_preferences.sounds.pack == "crisp", "The retained selector chooses Crisp");
      static_cast<Select&>(widget("user-account-select:sound-pack")).setSelectedIndex(2);
    } else if (step == 3) {
      require(m_preferences.sounds.pack == "warm", "The retained selector chooses Warm");
      static_cast<Select&>(widget("user-account-select:sound-pack")).setSelectedIndex(0);
    } else if (step == 4) {
      require(m_preferences.sounds.volume == defaultSoundVolume, "Interface sounds start at a quiet volume");
      auto& slider = widget("user-account-slider:sound-volume");
      float x = 0, y = 0;
      Node::mapToScene(&slider, 0, 0, x, y);
      m_scrolling.stop(m_settingsScroll);
      m_settingsScroll = std::max(0.0F, m_settingsScroll + y - m_scene->height() / 2);
    } else if (step == 5 || step == 6) {
      auto& slider = widget("user-account-slider:sound-volume");
      float x = 0, y = 0;
      Node::mapToScene(&slider, slider.width() / 2, slider.height() / 2, x, y);
      y += m_window.titlebarHeight() - layout::Window::titlebarHeight;
      if (step == 5) {
        m_window.motion(x, y);
        m_window.click(x, y, BTN_LEFT);
        m_window.motion(x + slider.width(), y);
        require(m_preferences.sounds.volume == maximumSoundVolume, "Native sound volume reaches its upper bound");
      } else {
        require(m_preferences.sounds.volume == maximumSoundVolume, "Sound volume survives scene rebuilds");
        m_window.motion(x - slider.width(), y);
        m_window.released();
        require(m_preferences.sounds.volume == 0, "Sound volume drag capture survives repaint and reaches zero");
        soundPreference("sound-preview:message");
        require(m_settingsStatus == "Increase sound volume to preview sounds.", "Silent previews explain zero volume");
      }
    } else if (step == 7) {
      static_cast<Slider&>(widget("user-account-slider:sound-volume")).setValue(defaultSoundVolume);
      require(m_preferences.sounds.volume == defaultSoundVolume, "Sound volume can return to its quiet default");
    } else if (step < soundReplayFrames - 1) {
      const auto index = static_cast<std::size_t>((step - 8) / 2);
      const auto id = std::string(soundEvents[index].id);
      const auto key = "toggle:sound-toggle:" + id;
      if (step % 2 == 0) {
        auto& control = widget(key);
        auto* viewport = control.parent();
        while (viewport && !viewport->clipChildren())
          viewport = viewport->parent();
        require(viewport, "Sounds have a native clipped settings viewport");
        float x = 0, y = 0, left = 0, top = 0;
        Node::mapToScene(&control, 0, control.height() / 2, x, y);
        Node::mapToScene(viewport, 0, 0, left, top);
        m_scrolling.stop(m_settingsScroll);
        m_settingsScroll = std::max(0.0F, m_settingsScroll + y - top - viewport->height() / 2);
      } else {
        click(key);
        require(!m_preferences.sounds.enabled[index], "Each sound has a working native disable switch");
        click(key);
        require(m_preferences.sounds.enabled[index], "Each sound can be reenabled");
        for (bool enabled : m_preferences.sounds.enabled)
          require(enabled, "Sound switches do not alter their neighbors");
        auto& preview = static_cast<Button&>(widget("sound-preview:" + id));
        auto& toggle = widget(key);
        require(preview.glyph() && !preview.label(), "Sound previews use a glyph without a duplicated caption");
        require(
            preview.parent() == toggle.parent()
                && preview.x() + preview.width() < toggle.x()
                && preview.y() < toggle.y() + toggle.height()
                && preview.y() + preview.height() > toggle.y() - toggle.height(),
            "The preview glyph stays beside its heading and clear of the toggle"
        );
        click("sound-preview:" + id);
        require(m_settingsStatus == "Playing sound preview.", "Native preview actions remain usable");
        require(m_preferences.sounds.enabled[index], "Previewing never toggles the event");
        m_settingsStatus.clear();
        m_controlInput.setFocus(preview.inputArea());
        m_window.key({.symbol = XKB_KEY_Return});
        require(m_settingsStatus == "Playing sound preview.", "Preview glyphs support native keyboard activation");
      }
    } else {
      require(
          m_preferences.sounds.pack == "soft"
              && m_preferences.notificationSounds
              && m_preferences.sounds.volume == defaultSoundVolume,
          "Sound choices retain final defaults"
      );
      require(!m_notificationSounds.pollResult(), "Offline sound previews never open an audio device");
      std::clog << "Offline sound settings replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
