#include "app/client_application.h"
#include "app/single_instance.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace app {
  namespace {
    constexpr auto startupTrayWait = std::chrono::seconds(5);
  }
  void ClientApplication::beginTrayStartup() {
    // Never attach the first window buffer while the tray host is being discovered.
    m_window.hide();
    m_trayStartupTimer.start(startupTrayWait, [this] {
      if (!m_trayAvailable && !m_window.visible())
        m_window.show();
    });
  }
  void ClientApplication::closeToTray() {
    sharingRelease();
    releasePushToTalk();
    m_client.voiceAudio()->stopTest();
    // A window must remain reachable when the desktop has no tray host.
    if (!m_trayAvailable) {
      m_window.close();
      return;
    }
    saveSelection();
    finishRailDrag(true);
    finishSidebarResize();
    closeContext();
    closeSelectDropdown();
    if (m_shareOpen || m_shareAwaitingSource) {
      m_shareOpen = m_shareAwaitingSource = false;
      ++m_shareSourceRequest;
      m_screen.cancelSelection();
      const bool publishing = std::ranges::any_of(m_streamSnapshot.active, [](const auto& stream) {
        return stream.publishing && stream.connection.phase != voice::Phase::Failed;
      });
      if (!publishing && !m_sharingPublicationRequested) {
        m_screen.stop();
        m_client.streams().preview()->clear();
      }
    }
    m_controlInput.cancelPointerCapture();
    m_controlInput.setFocus(nullptr);
    m_dragDraft = m_dragMessage = m_dragImage = false;
    m_scrolling.stopAll();
    if (m_videoExpanded)
      setVideoExpanded(false);
    if (!m_videoSource.empty()) {
      m_video.pause(true);
      m_videoPlayRequested = false;
    }
    if (!m_soundPreviewSource.empty())
      m_soundPreview.pause(true);
    m_window.hide();
  }
  void ClientApplication::trayEvent(TrayEvent event) {
    if (m_demo && m_demoOverlay == "tray-cycle")
      std::clog << "Offline tray command " << static_cast<int>(event.command) << '\n';
    switch (event.command) {
    case TrayCommand::Availability:
      m_trayAvailable = event.available;
      if (event.available)
        m_trayStartupTimer.stop();
      else if (!m_trayStartupTimer.active() && !m_window.visible())
        m_window.show();
      break;
    case TrayCommand::Open:
      m_trayStartupTimer.stop();
      m_window.show(std::move(event.activationToken));
      break;
    case TrayCommand::Restart:
      m_trayStartupTimer.stop();
      saveSelection();
      m_restartRequested = true;
      if (!m_demo)
        m_window.close();
      break;
    case TrayCommand::Close:
      m_trayStartupTimer.stop();
      m_restartRequested = false;
      m_window.close();
      break;
    case TrayCommand::StopSharing:
      sharingAction("stop");
      break;
    }
  }
  void ClientApplication::demoTrayStartup(bool verify) {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    if (verify) {
      require(
          m_demoInteractionStep++ == 0
              && m_window.visible()
              && !m_trayStartupTimer.active()
              && m_trayHiddenTicks > 0
              && m_window.toplevelGeneration() == 2,
          "Startup stays unpresented until one explicit open or fallback recreates its role"
      );
      require(
          m_trayAvailable
              == (m_demoOverlay == "tray-startup-host"
                  || m_demoOverlay == "tray-startup-open"
                  || m_demoOverlay == "tray-startup-notification"),
          "Startup host state matches the tested open, loss or timeout path"
      );
      std::clog
          << "Offline "
          << m_demoOverlay
          << ": no startup frame, hidden event-loop delivery, explicit open "
             "or fallback, timer cancellation and native restore passed\n";
      return;
    }
    m_trayDemoTimer.start(std::chrono::milliseconds(Style::animFast), [this, require] {
      require(!m_window.visible() && m_animationDraw == 0, "Start-minimized never presents a startup frame");
      trayEvent({.available = false});
      require(
          !m_window.visible() && m_trayStartupTimer.active(),
          "Initial host absence preserves the asynchronous startup grace period"
      );
      if (m_demoOverlay == "tray-startup-missing")
        return; // Exercise the actual one-shot deadline, without a polling loop.
      if (m_demoOverlay == "tray-startup-open" || m_demoOverlay == "tray-startup-notification") {
        if (m_demoOverlay == "tray-startup-notification")
          desktopNoticeAction(
              {.account = m_account.id, .guild = m_guildId, .channel = m_channelId, .generation = m_session}
          );
        else
          trayEvent({.command = TrayCommand::Open});
        require(
            m_window.visible() && !m_trayStartupTimer.active(),
            "An explicit open during discovery cancels the startup wait immediately"
        );
        trayEvent({.available = true});
        require(m_window.visible(), "Late tray registration never re-hides an explicitly opened window");
      } else {
        trayEvent({.available = true});
        require(
            !m_window.visible() && !m_trayStartupTimer.active(),
            "Successful host registration leaves startup hidden and cancels its deadline"
        );
        if (m_demoOverlay == "tray-startup-host-loss")
          trayEvent({.available = false});
        else
          trayEvent({.command = TrayCommand::Open});
      }
    });
  }
  void ClientApplication::demoTrayStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto step = m_demoInteractionStep++;
    std::clog << "Offline tray lifecycle step " << step << '\n';
    require(m_window.visible(), "Only mapped windows present tray replay frames");
    if (step == 0) {
      require(m_window.toplevelGeneration() == 1, "Start-minimized does not hide ordinary offline demos");
      m_drafts[m_channelId] = "Draft retained while hidden";
      m_window.closeRequested = [this] { closeToTray(); };
      trayEvent({.available = true});
      activate({.action = Action::Close});
      require(!m_window.visible() && !m_window.active(), "The window close button hides and unfocuses the window");
      m_trayDemoTimer.start(std::chrono::milliseconds(Style::animFast), [this, require] {
        const auto hiddenGeneration = m_window.toplevelGeneration();
        trayEvent({.command = TrayCommand::Open});
        const auto restoredGeneration = m_window.toplevelGeneration();
        require(restoredGeneration == hiddenGeneration + 1, "A genuinely hidden window recreates its shell role once");
        require(!m_window.active(), "Restored surface has not yet received desktop focus");
        trayEvent({.command = TrayCommand::Open});
        desktopNoticeAction(
            {.account = m_account.id, .guild = m_guildId, .channel = m_channelId, .generation = m_session}
        );
        require(
            m_window.toplevelGeneration() == restoredGeneration,
            "Tray and notification activation of an unfocused visible window must preserve its native role"
        );
        require(m_instance, "The tray replay owns an isolated activation socket");
        SingleInstance launcher(m_instance->runtimeDirectory());
        require(
            launcher.claim("offline-visible-launch") == SingleInstance::Launch::Forwarded,
            "Another launch queues activation for the visible client"
        );
      });
    } else if (step == 1) {
      const auto generation = m_window.toplevelGeneration();
      require(generation == 2, "Forwarded activation preserves the restored visible window's native role");
      trayEvent({.command = TrayCommand::Open, .activationToken = "offline-activation-token"});
      trayEvent({.command = TrayCommand::Open});
      desktopNoticeAction(
          {.account = m_account.id, .guild = m_guildId, .channel = m_channelId, .generation = m_session}
      );
      require(
          m_window.toplevelGeneration() == generation,
          "Repeated activation of a presented window preserves its native role"
      );
      require(
          m_trayHiddenTicks > 0 && m_connected && m_drafts.at(m_channelId) == "Draft retained while hidden",
          "Hidden windows keep the owning event loop, connection and draft alive"
      );
      m_window.requestClose();
      require(!m_window.visible(), "Compositor close requests use the same hide path");
      m_trayDemoTimer.start(std::chrono::milliseconds(Style::animFast), [this] { trayEvent({.available = false}); });
    } else if (step == 2) {
      require(!m_trayAvailable && m_window.visible(), "Losing the tray host restores a hidden window");
      trayEvent({.available = true});
      m_window.requestClose();
      m_trayDemoTimer.start(std::chrono::milliseconds(Style::animFast), [this, require] {
        const auto generation = m_window.toplevelGeneration();
        SingleInstance launcher(m_instance->runtimeDirectory());
        require(
            launcher.claim("offline-hidden-launch") == SingleInstance::Launch::Forwarded,
            "Another launch targets the running hidden client"
        );
        require(
            !m_window.visible() && m_window.toplevelGeneration() == generation,
            "The launch packet is delivered through the owning event loop"
        );
      });
    } else if (step == 3) {
      require(m_window.toplevelGeneration() == 4, "A launch packet restores the hidden window exactly once");
      trayEvent({.command = TrayCommand::Restart});
      require(m_restartRequested, "Restart requests an orderly relaunch independently of window hiding");
      m_restartRequested = false;
      require(
          m_drafts.at(m_channelId) == "Draft retained while hidden", "Repeated hide and restore never clears the draft"
      );
      std::clog << "Offline tray focus without remapping, hidden restore, notification activation, host loss, draft "
                   "and restart replay passed\n";
      trayEvent({.command = TrayCommand::Close});
    }
    m_window.invalidate();
  }
} // namespace app
