#include "app/client_application.h"
#include "ui/controls/button.h"

#include <linux/input-event-codes.h>
#include <stdexcept>

namespace app {
  void ClientApplication::demoUpdatesStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto click = [&](const std::string& action) {
      const auto at = m_widgets.find("user-account-button:" + action);
      require(at != m_widgets.end(), "Updates settings mounts the native action button");
      auto* button = static_cast<Button*>(at->second);
      require(button->enabled(), "The selected update action is enabled");
      float left, top, right, bottom;
      Node::transformedBounds(button, left, top, right, bottom);
      const float y = (top + bottom) / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion((left + right) / 2, y);
      m_window.click((left + right) / 2, y, BTN_LEFT);
      m_window.released();
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      click("updates:check");
      require(m_updateSnapshot.phase == updates::Phase::Checking, "A native click starts the offline update preview");
    } else if (step == 1) {
      require(
          !static_cast<Button*>(m_widgets.at("user-account-button:updates:check"))->enabled(),
          "Checking disables duplicate update requests"
      );
      click("updates:cancel");
      require(m_updateSnapshot.phase == updates::Phase::Idle, "Cancelling an update restores its idle state");
    } else if (step == 2) {
      m_updateSnapshot.phase = updates::Phase::Available;
      m_updateSnapshot.release = updates::Release{
          .version = "v99.0.0",
          .notes = "Synthetic update notes",
          .url = std::string(updates::releasesUrl) + "/tag/v99.0.0"
      };
    } else if (step == 3) {
      m_updateSnapshot.phase = updates::Phase::Downloading;
      m_updateSnapshot.downloaded = 42;
      m_updateSnapshot.total = 100;
    } else if (step == 4) {
      require(
          m_widgets.contains("user-account-button:updates:cancel"), "A download can be cancelled from native settings"
      );
      m_updateSnapshot.phase = updates::Phase::Ready;
    } else if (step == 5) {
      m_voiceState.channel = "demo-call";
      m_voiceState.phase = voice::Phase::Connected;
    } else if (step == 6) {
      require(
          !static_cast<Button*>(m_widgets.at("user-account-button:updates:restart"))->enabled(),
          "Update restart stays disabled during a call"
      );
      updateAction("updates:restart");
      require(
          m_updateSnapshot.phase == updates::Phase::Ready && !m_updateRestartPending,
          "A stale restart action cannot interrupt a call"
      );
      m_voiceState = {};
    } else if (step == 7) {
      click("updates:restart");
      require(
          m_updateSnapshot.phase == updates::Phase::Installing && m_updateRestartPending,
          "Explicit restart prepares the update"
      );
    } else if (step == 8) {
      m_updateSnapshot.phase = updates::Phase::Error;
      m_updateSnapshot.error = "Synthetic download verification failure";
      updateTick();
      require(!m_updateRestartPending, "An update failure cancels the pending restart");
    } else if (step == 9) {
      click("updates:check");
      require(m_updateSnapshot.error.empty(), "An explicit retry clears the previous update error");
      m_updateSnapshot.phase = updates::Phase::Current;
    } else if (step == 10) {
      require(
          m_widgets.contains("user-account-toggle:updates:automatic"), "Automatic checks remain an explicit preference"
      );
      m_updateSnapshot.phase = updates::Phase::Installed;
    } else {
      require(
          !static_cast<Button*>(m_widgets.at("user-account-button:updates:check"))->enabled(),
          "An installed update waits for restart instead of downloading itself again"
      );
      require(m_drafts[m_channelId] == "Keep my draft", "The update UI preserves the composer draft");
    }
    m_window.invalidate();
  }
} // namespace app
