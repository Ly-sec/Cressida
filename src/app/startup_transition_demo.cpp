#include "app/client_application.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    constexpr std::string_view transitionChannel = "101";
    constexpr std::string_view transitionDraft = "Keep this draft through the startup reveal";
    constexpr float initialUiScale = 1.0F;
    constexpr float restoredUiScale = 1.5F;

    struct PhysicalLogo {
      float left, top, width, height;
    };
    std::optional<PhysicalLogo> initialLogo;
  } // namespace

  void ClientApplication::demoStartupTransitionStep() {
    const auto require = [](bool condition, const char* reason) {
      if (!condition)
        throw std::runtime_error(reason);
    };
    require(m_demo, "Startup transition replay must remain offline");
    const auto logo = [&] {
      const ImageNode* found = nullptr;
      const auto visit = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Image) {
          const auto& image = static_cast<const ImageNode&>(node);
          const bool branded = std::ranges::any_of(m_images, [&](const auto& entry) {
            return entry.first.starts_with("builtin:signin-logo:")
                && entry.second.texture.valid()
                && entry.second.texture.id == image.textureId();
          });
          if (branded) {
            require(!found, "The full-window loader paints exactly one logo");
            found = &image;
          }
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      visit(visit, *m_scene);
      require(found, "The full-window loader uses the bundled Cressida logo");
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(found, left, top, right, bottom);
      const float scale = m_window.target().contentScale();
      const PhysicalLogo result{left * scale, top * scale, (right - left) * scale, (bottom - top) * scale};
      require(
          std::abs(result.left + result.width / 2 - m_window.target().bufferWidth() / 2.0F) < Style::borderWidth
              && std::abs(result.top + result.height / 2 - m_window.target().bufferHeight() / 2.0F)
                  < Style::borderWidth,
          "The startup logo remains centered in both physical surface dimensions"
      );
      return result;
    };
    const auto isolation = [&] {
      require(startupCovered(), "The transition keeps conversation input covered until its reveal completes");
      focusChatComposer();
      require(!m_controlInput.focusedArea(), "A covered composer cannot acquire native input focus");
      m_messageScroll = Style::spaceGroup;
      m_scrolling.limit(m_messageScroll, Style::spaceGroup * 3);
      m_scrolling.stop(m_messageScroll);
      m_pointerX = chatLeft() + Style::spaceXl;
      m_pointerY = layout::Window::titlebarHeight + Style::spaceGroup;
      m_window.scroll(Style::scrollWheelStep);
      require(
          m_messageScroll == Style::spaceGroup && m_scrolling.target(m_messageScroll) == Style::spaceGroup,
          "Wheel input cannot scroll the conversation beneath a startup transition"
      );
      onKey({.symbol = XKB_KEY_x, .text = "x"});
      onKey({.symbol = XKB_KEY_i, .control = true});
      require(
          !m_controlInput.focusedArea() && !m_inboxOpen && m_drafts.at(m_channelId) == transitionDraft,
          "Typing and application shortcuts cannot reach the covered composer or inbox"
      );
      m_messageScroll = 0;
      m_scrolling.stop(m_messageScroll);
      const auto overlay = std::exchange(m_demoOverlay, "unread-cycle");
      const bool reading = readingChannel(m_channelId);
      m_demoOverlay = overlay;
      require(!reading, "A startup transition cannot acknowledge the conversation it covers");
    };

    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      initialLogo.reset();
      m_preferences.reducedMotion = true;
      finishStartupLoading();
      finishStartupReveal();
      ++m_session;
      m_connected = true;
      m_connecting = m_restoringLogin = false;
      m_window.setUiScale(initialUiScale);
      m_startupReferenceScale = initialUiScale;
      m_preferences.fontFamily.clear();
      m_dmMode = false;
      m_showMembers = false;
      m_dmPage.clear();
      m_dmPreviewId.clear();
      m_dmPreviewLoading = false;
      m_guildId = "1";
      m_channelId = transitionChannel;
      m_channels = {{std::string(transitionChannel), "general"}};
      m_guildChannels[m_guildId] = m_channels;
      m_loadingChannels = m_loadingPrivate = m_loadingMessages = false;
      m_openThreadQuery.reset();
      m_live = false;
      m_status.clear();
      m_messages = {{"9901", m_channelId, "Maya", "The restored conversation is ready beneath the loader."}};
      m_liveChanges.clear();
      m_messageLayouts.clear();
      m_history.reset();
      m_history.restoreLatest(m_messages, true);
      m_drafts[m_channelId] = transitionDraft;
      m_readyBootstrapPending = true;
      beginStartupLoading();
      updateStartupLoading();
      require(
          m_startupLoading && m_startupLoadingTimer.active(), "The fixture waits for its synthetic initial snapshot"
      );
    } else if (step == 1) {
      initialLogo = logo();
      isolation();
      m_window.setUiScale(restoredUiScale);
      m_preferences.fontFamily = "serif";
    } else if (step == 2) {
      const auto restored = logo();
      require(
          initialLogo.has_value(), "The transition captures its initial physical logo before restoring preferences"
      );
      require(
          std::abs(restored.width - initialLogo->width) < Style::borderWidth
              && std::abs(restored.height - initialLogo->height) < Style::borderWidth
              && std::abs(restored.left - initialLogo->left) < Style::borderWidth
              && std::abs(restored.top - initialLogo->top) < Style::borderWidth,
          "Restoring saved zoom and font preferences preserves the startup logo's physical size and position"
      );
      m_preferences.reducedMotion = false;
      m_readyBootstrapPending = false;
      updateStartupLoading();
      require(
          !m_startupLoading
              && startupCovered()
              && m_startupRevealStart
              && *m_startupRevealStart == std::chrono::steady_clock::time_point{}
              && startupRevealOpacity() == 1
              && !m_startupRevealTimer.active(),
          "Ready history starts an opaque reveal pending its first fully prepared conversation frame"
      );
      isolation();
    } else if (step == 3) {
      require(
          m_startupRevealStart
              && *m_startupRevealStart != std::chrono::steady_clock::time_point{}
              && m_startupRevealTimer.active(),
          "The reveal timer starts after the first covered conversation frame is rendered"
      );
      m_startupRevealTimer.stop();
      isolation();
    } else if (step == 4) {
      require(m_startupRevealStart.has_value(), "The deterministic fade sample retains its covered transition");
      m_startupRevealStart = std::chrono::steady_clock::now() - std::chrono::milliseconds(Style::animSlow / 2);
      require(
          startupRevealOpacity() > 0 && startupRevealOpacity() < 1,
          "The startup reveal interpolates through intermediate opacity"
      );
      draw();
      require(
          std::ranges::any_of(
              m_scene->children(), [](const auto& child) { return child->opacity() > 0 && child->opacity() < 1; }
          ),
          "The rendered startup overlay exposes the prepared conversation through an intermediate fade"
      );
      isolation();
      finishStartupReveal();
      require(
          !startupCovered() && !m_startupRevealTimer.active(), "Finishing the fade releases its coverage and timer"
      );
    } else if (step == 5) {
      require(
          m_inputs.contains("composer")
              && m_inputs.at("composer")->enabled()
              && m_controlInput.focusedArea() == m_inputs.at("composer")->inputArea()
              && m_drafts.at(m_channelId) == transitionDraft
              && m_window.uiScale() == restoredUiScale,
          "Revealing the app restores its mounted composer focus and draft while preserving saved chat zoom"
      );
      m_preferences.reducedMotion = true;
      ++m_session;
      beginStartupLoading();
      updateStartupLoading();
      require(
          !startupCovered() && !m_startupLoadingTimer.active() && !m_startupRevealTimer.active(),
          "Reduced motion completes preparation immediately without a fading overlay"
      );
      m_preferences.reducedMotion = false;
      ++m_session;
      beginStartupLoading();
      updateStartupLoading();
      require(startupCovered() && m_startupRevealStart, "A new synthetic session owns its reveal state");
      ++m_session;
      updateStartupLoading();
      require(
          !startupCovered() && !m_startupLoadingTimer.active() && !m_startupRevealTimer.active(),
          "Replacing the session retires stale reveal coverage and timers"
      );
      m_preferences.reducedMotion = true;
      std::clog << "Offline startup transition replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
