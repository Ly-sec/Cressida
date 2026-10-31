#include "app/client_application.h"
#include "app/stream_popout.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::prepareSharingDemo(const std::string& view) {
    prepareVoiceDemo(view);
    m_settingsOpen = false;
    m_sharingAudioExpanded = m_sharingAdvanced = false;
    m_account.premiumType = 2;
    voiceAction("join:107");
    m_drafts[m_channelId] = "Keep this draft while sharing";
    auto frame = std::make_shared<voice::VideoFrame>();
    frame->width = 320;
    frame->height = 180;
    frame->stride = frame->width * 4;
    frame->rgba.resize(frame->stride * frame->height);
    for (int y = 0; y < frame->height; ++y)
      for (int x = 0; x < frame->width; ++x) {
        auto* pixel = frame->rgba.data() + y * frame->stride + x * 4;
        pixel[0] = x * 255 / frame->width;
        pixel[1] = y * 255 / frame->height;
        pixel[2] = 128;
        pixel[3] = 255;
      }
    m_client.streams().preview()->source(std::move(frame));
    m_sharingDemoSource = {
        .phase = capture::ScreenPhase::Ready, .revision = 1, .name = "Synthetic window", .width = 320, .height = 180
    };
    m_sharingDemoAudio.inputs = {{"application:browser", "Browser"}, {"application:music", "Music"}};
    m_streamSnapshot.channel = m_voiceState.channel;
    m_streamSnapshot.available = {{{m_voiceGuild, m_voiceState.channel, "11"}, {m_account.id}}};
    if (view.starts_with("sharing-cycle"))
      m_sharingDemoSource.phase = capture::ScreenPhase::Idle;
    sharingAction("open");
    if (view == "sharing-picker-options")
      sharingAction("advanced");
    else if (view == "sharing-picker-audio")
      sharingAction("audio-expand");
    if (view.starts_with("sharing-stage")) {
      sharingAction("start");
      const auto publisher =
          std::ranges::find_if(m_streamSnapshot.active, [](const auto& stream) { return stream.publishing; });
      publisher->connection = {.phase = voice::Phase::Connected, .encrypted = true};
      sharingAction("view:" + publisher->stream.key.value());
      m_status.clear();
    }
  }
  void ClientApplication::demoSharingStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto reveal = [&](const std::string& id) {
      if (m_shareOpen && (m_sharingAdvanced || m_sharingAudioExpanded))
        return;
      const auto hit = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == Action::ShareAction && hit.id == id;
      });
      require(hit != m_hits.end(), "A sharing control exists in the native scene");
      const float height = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
      const auto body = layout::Sharing::body(layout::Sharing::dialog(m_window.width(), height, m_voiceStageOpen));
      m_shareScroll =
          std::clamp(m_shareScroll + hit->y + hit->height / 2 - body.y - body.height / 2, 0.0F, m_shareMaxScroll);
    };
    const auto click = [&](const std::string& id) {
      const auto& hits = m_shareOpen && (m_sharingAdvanced || m_sharingAudioExpanded) ? m_sharingMenuHits : m_shareHits;
      const auto hit = std::ranges::find(hits, id, &Hit::id);
      require(hit != hits.end(), "A sharing action has a visible clipped hit region");
      const auto copy = *hit;
      const auto y = copy.y + copy.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(copy.x + copy.width / 2, y);
      m_window.click(copy.x + copy.width / 2, y, BTN_LEFT);
      m_window.released();
    };
    const auto key = "guild:" + m_voiceGuild + ":" + m_voiceState.channel + ":11";
    const auto publishing = [&] {
      return std::ranges::find_if(m_streamSnapshot.active, [](const auto& stream) { return stream.publishing; });
    };
    const auto text = [&](const std::string& value) {
      const auto find = [&](auto&& self, const Node& node) -> bool {
        if (node.type() == NodeType::Text && static_cast<const TextNode&>(node).text() == value)
          return true;
        for (const auto& child : node.children())
          if (self(self, *child))
            return true;
        return false;
      };
      return find(find, *m_scene);
    };
    switch (m_demoInteractionStep) {
    case 0:
      require(
          m_shareAwaitingSource && !m_shareOpen && !m_voiceStageOpen,
          "Share requests the desktop selector before showing settings"
      );
      m_sharingDemoSource.phase = capture::ScreenPhase::Ready;
      ++m_sharingDemoSource.revision;
      break;
    case 1:
      require(
          m_shareOpen && m_streamTextures.contains("preview") && m_streamTextures["preview"].texture.valid(),
          "Successful selection opens settings with a real preview texture"
      );
      m_window.key({.symbol = XKB_KEY_a, .text = "a"});
      require(m_drafts[m_channelId] == "Keep this draft while sharing", "The picker isolates composer input");
      m_sharingDemoPickerExtent = m_shareMaxScroll;
      reveal("resolution:2160");
      break;
    case 2:
      click("resolution:2160");
      require(m_preferences.sharing.resolution == 2160, "The highest preset is selectable");
      reveal("fps:60");
      break;
    case 3:
      click("fps:60");
      require(m_preferences.sharing.frameRate == 60, "The high frame rate stays selected");
      reveal("clarity");
      break;
    case 4:
      click("clarity");
      require(
          m_preferences.sharing.content == voice::ContentPreference::Clarity,
          "Clarity selects resolution-preserving adaptation"
      );
      click("advanced");
      break;
    case 5:
      require(
          m_sharingMenuBounds.height > 0 && m_shareMaxScroll == m_sharingDemoPickerExtent,
          "More options floats over the picker without expanding its content"
      );
      m_window.key({.symbol = XKB_KEY_Down});
      m_window.key({.symbol = XKB_KEY_Down});
      m_window.key({.symbol = XKB_KEY_Return});
      require(!m_preferences.sharing.hardware, "Keyboard activation changes encoding preference");
      m_window.key({.symbol = XKB_KEY_Escape});
      require(m_shareOpen && !m_sharingAdvanced, "Escape dismisses the overflow menu before the picker");
      reveal("audio-expand");
      sharingAction("audio-expand");
      break;
    case 6: {
      require(
          m_sharingMenuBounds.height > 0 && m_shareMaxScroll == m_sharingDemoPickerExtent,
          "Audio choices float without expanding the picker"
      );
      const auto close = *std::ranges::find(m_shareHits, std::string("close"), &Hit::id);
      m_window.click(
          close.x + close.width / 2,
          close.y + close.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight, BTN_LEFT
      );
      m_window.released();
      require(m_shareOpen && !m_sharingAudioExpanded, "An outside click dismisses only the audio menu");
      sharingAction("audio-expand");
      reveal("audio:application:browser");
      break;
    }
    case 7:
      click("audio:application:browser");
      reveal("audio:application:music");
      break;
    case 8:
      click("audio:application:music");
      require(m_preferences.sharing.audioSources.size() == 2, "Multiple application sources can be combined");
      m_window.key({.symbol = XKB_KEY_Escape});
      require(m_shareOpen && !m_sharingAudioExpanded, "Audio selections survive closing their dropdown");
      click("start");
      require(
          !m_shareOpen && !m_voiceStageOpen && m_sharingPublicationRequested,
          "Go Live closes settings and returns to chat"
      );
      break;
    case 9:
      require(
          text("Starting screen share…") && !text("LIVE · Your screen") && !m_window.fullscreen(),
          "A queued stream shows Starting without claiming live or opening fullscreen"
      );
      require(
          m_sharingPreviewBounds.width > 0 && m_sharingPreviewBounds.width <= Style::profileCardWidth,
          "Publication uses a compact nonmodal preview"
      );
      publishing()->connection.phase = voice::Phase::WaitingForParticipants;
      break;
    case 10: {
      const auto card = sharingPreviewFrame();
      require(card.has_value(), "The floating preview has a measured frame");
      m_window.motion(
          card->title.x + Style::spaceSm,
          card->title.y + card->title.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      break;
    }
    case 11: {
      require(
          m_tooltip.empty() && m_window.cursorShape() == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB,
          "The preview body owns hover and its move cursor without an underlying tooltip"
      );
      const auto card = *sharingPreviewFrame();
      const float x = card.title.x + Style::spaceSm,
                  y = card.title.y + card.title.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.click(x, y, BTN_LEFT);
      require(m_sharingPreviewDragging, "Pressing the preview background starts a native drag");
      m_window.motion(x - Style::spaceGroup, y - Style::spaceGroup);
      break;
    }
    case 12: {
      require(
          m_sharingPreviewVertical < 1 && m_window.cursorShape() == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING,
          "Native pointer motion moves the preview instead of the covered conversation"
      );
      m_window.released();
      require(!m_sharingPreviewDragging, "Releasing the pointer ends the preview drag");
      const auto card = *sharingPreviewFrame();
      const float x = card.video.x + card.video.width / 2,
                  y = card.video.y + card.video.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      const auto scroll = m_messageScroll;
      m_window.scroll(Style::scrollWheelStep);
      require(m_messageScroll == scroll, "Scrolling the preview cannot scroll chat behind it");
      m_window.contextClick(x, y);
      require(!m_contextOpen, "A covered message cannot open its context menu through the preview");
      break;
    }
    case 13: {
      require(
          m_tooltip.empty()
              && std::ranges::none_of(
                  m_hits,
                  [&](const auto& hit) {
                    return hit.action == Action::MessageMenu && hit.contains(m_pointerX, m_pointerY);
                  }
              ),
          "Hovering the preview never exposes a covered message action"
      );
      const auto card = *sharingPreviewFrame();
      const float x = card.title.x + Style::spaceSm,
                  y = card.title.y + card.title.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      m_window.motion(m_window.width() * 2, m_window.height() * 2);
      require(
          m_sharingPreviewHorizontal == 1 && m_sharingPreviewVertical == 1,
          "Dragging past the window edge clamps the preview inside the conversation"
      );
      m_window.focusChanged(false);
      require(!m_sharingPreviewDragging && !m_sharingPointerDown, "Focus loss retires an unfinished preview drag");
      break;
    }
    case 14:
      require(text("LIVE · Your screen"), "An authenticated empty stream can be live while waiting for viewers");
      click("pause");
      require(publishing()->paused, "Pause updates the stream");
      break;
    case 15:
      require(text("Sharing paused"), "Pause has an explicit status");
      click("pause");
      publishing()->connection.phase = voice::Phase::Failed;
      publishing()->connection.error = "The synthetic stream server refused setup.";
      break;
    case 16:
      require(text("Screen share failed") && !text("LIVE · Your screen"), "A failed stream is never presented as live");
      click("preview-settings");
      break;
    case 17:
      require(text("Try again"), "Failed publication exposes an explicit retry");
      click("start");
      break;
    case 18:
      require(text("Starting screen share…"), "Retry has a fresh pending state");
      publishing()->connection.phase = voice::Phase::Connected;
      sharingAction("streams");
      break;
    case 19:
      require(voiceStageVisible() && !m_shareOpen, "A stream opens in its voice channel without a modal");
      {
        const float height = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
        const auto panes = layout::Sharing::stage(chatLeft(), m_window.width(), height);
        require(
            conversationLeft() == panes.chat.x
                && (panes.chat.x >= panes.video.x + panes.video.width
                    || panes.chat.y >= panes.video.y + panes.video.height),
            "Voice video and text chat have separate responsive panes"
        );
      }
      require(
          std::ranges::none_of(m_shareHits, [](const auto& hit) { return hit.id.starts_with("focus:"); }),
          "A single stream does not offer ineffective focus or show-all controls"
      );
      reveal("watch:" + key);
      break;
    case 20:
      click("watch:" + key);
      require(m_streamSnapshot.active.size() == 2 && m_streamFocus == key, "Watching preserves the local publication");
      break;
    case 21:
      require(text("Show all"), "A focused stream offers Show all when another stream is active");
      click("focus:" + key);
      require(m_streamFocus.empty(), "Show all clears stream focus through its native control");
      break;
    case 22:
      require(
          m_visibleStreamFrames.contains(publishing()->stream.key.value()) && text("Focus"),
          "Show all returns to the stream grid and exposes meaningful focus controls"
      );
      {
        const auto target = m_visibleStreamFrames.contains(key) ? key : publishing()->stream.key.value();
        click("focus:" + target);
        require(m_streamFocus == target, "Focus selects the chosen stream through its native control");
      }
      break;
    case 23:
      require(
          m_visibleStreamFrames.size() == 1 && m_visibleStreamFrames.contains(m_streamFocus),
          "Focus renders only the selected stream"
      );
      sharingAction("view:" + key);
      break;
    case 24:
      require(
          m_visibleStreamFrames.size() == 1 && m_visibleStreamFrames.contains(key),
          "Focus renders only the selected stream"
      );
      click("popout:" + key);
      require(m_streamPopout && m_streamPopout->eventFd() >= 0, "Pop out creates a native window on the owning loop");
      break;
    case 25:
      sharingAction("volume-down:" + key);
      require(m_streamVolumes[key] < 1, "Watch volume is independent");
      sharingAction("unwatch:" + key);
      break;
    case 26:
      require(!m_streamPopout && m_streamSnapshot.active.size() == 1, "Stopping a watch retires its popout");
      sharingAction("open");
      break;
    case 27:
      require(
          m_shareOpen && m_preferences.sharing.resolution == 2160 && m_preferences.sharing.audioSources.size() == 2,
          "Editing an active stream retains preferences and source"
      );
      m_window.key({.symbol = XKB_KEY_Escape});
      require(!m_shareOpen, "Escape dismisses settings");
      sharingAction("streams");
      break;
    case 28:
      trayEvent({.command = TrayCommand::StopSharing});
      require(publishing() == m_streamSnapshot.active.end(), "Tray stop ends the publication");
      break;
    case 29:
      m_window.key({.symbol = XKB_KEY_Escape});
      require(
          !m_shareOpen && !m_voiceStageOpen && m_drafts["101"] == "Keep this draft while sharing",
          "Sharing preserves chat drafts"
      );
      require(!m_voiceMuted && !m_voiceDeafened && !m_voicePtt, "Microphone state stays independent");
      break;
    case 30:
      require(
          m_client.streams().sharedAudio()->state().inputs.empty()
              && m_screen.state().phase == capture::ScreenPhase::Idle,
          "The replay never accesses real capture or audio services"
      );
      break;
    case 31:
      sharingAction("open");
      require(m_shareAwaitingSource && !m_shareOpen, "A new share starts with consent again");
      m_sharingDemoSource.phase = capture::ScreenPhase::Idle;
      ++m_sharingDemoSource.revision;
      break;
    case 32:
      require(
          !m_shareAwaitingSource && !m_shareOpen && !m_voiceStageOpen,
          "Cancelling source selection returns directly to chat"
      );
      std::clog << "Offline sharing, preview dragging and pointer isolation, focus and native popout passed\n";
      break;
    default:
      break;
    }
    ++m_demoInteractionStep;
    m_window.invalidate();
  }
} // namespace app
