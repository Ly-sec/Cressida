#include "api/video_source.h"
#include "app/client_application.h"
#include "app/theme.h"
#include "render/scene/rect_node.h"
#include "ui/controls/button.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    class SyntheticMessageCapture final : public voice::MessageCapture {
    public:
      bool open(const std::string&, int, int, std::string&) override { return true; }
      int read(std::span<float> output, int wakeFd, int timeout, std::string&) override {
        constexpr std::size_t durationSamples = voice::messageSampleRate;
        const auto count = std::min(output.size(), durationSamples - m_samples);
        for (std::size_t i = 0; i < count; ++i)
          output[i] = std::sin(static_cast<float>(m_samples + i) * .1F) * .25F;
        m_samples += count;
        if (count)
          return static_cast<int>(count);
        pollfd wake{wakeFd, POLLIN, 0};
        poll(&wake, 1, timeout);
        return 0;
      }

    private:
      std::size_t m_samples = 0;
    };
  } // namespace
  voice::MessageRecorder::CaptureFactory ClientApplication::demoVoiceMessageCapture() {
    return [] { return std::make_unique<SyntheticMessageCapture>(); };
  }

  void ClientApplication::demoVoiceMessageStep() {
    const auto require = [](bool condition, const char* message) {
      if (!condition)
        throw std::runtime_error(message);
    };
    const auto click = [&](const std::string& id) {
      const auto at = m_widgets.find(id);
      require(at != m_widgets.end() && at->second->parent(), "Voice message controls are mounted native widgets");
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(at->second, left, top, right, bottom);
      m_window.motion((left + right) / 2, (top + bottom) / 2);
      m_window.click((left + right) / 2, (top + bottom) / 2, BTN_LEFT);
      require(static_cast<Button*>(at->second)->pressed(), "Voice message pointer presses reach the visible button");
      m_window.released();
    };
    const auto access = [&](bool allowed) {
      for (auto& channel : m_channels)
        channel.overwrites.emplace();
      m_roleDirectory.apply(
          {.guilds = {
               {.guild = m_guildId,
                .replaceRoles = true,
                .roles =
                    {{.id = m_guildId,
                      .permissions = std::to_string(
                          api::viewChannel | api::sendMessages | (allowed ? api::permission::sendVoiceMessages : 0)
                      )}},
                .members = {{m_account.id, {}}},
                .owner = "other-owner"}
           }}
      );
    };
    const auto step = m_demoInteractionStep;
    if (step == 0) {
      require(
          m_voiceMessageRecorder.state().phase == voice::MessageRecorderPhase::Idle,
          "Offline construction does not open a microphone"
      );
      m_replies[m_channelId] = api::MessageReference{"900", m_channelId};
      const auto microphone = std::ranges::find(m_hits, Action::RecordVoiceMessage, &Hit::action);
      require(microphone != m_hits.end(), "An accessible chat exposes its inline microphone action");
      const auto emoji = std::ranges::find(m_hits, Action::ToggleEmoji, &Hit::action);
      require(
          emoji != m_hits.end() && microphone->x + microphone->width < emoji->x,
          "The microphone sits to the left of the expression controls"
      );
      float x = 0, y = 0;
      Node::mapToScene(
          m_scene.get(), microphone->x + microphone->width / 2, microphone->y + microphone->height / 2, x, y
      );
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      m_window.released();
      require(
          m_dialogKind.empty() && m_voiceMessageChannel == m_channelId,
          "Recording starts inline without opening a dialog"
      );
    } else if (step == 1) {
      require(
          !m_widgets.contains("composer") && m_widgets.contains("voice-message:discard"),
          "The inline recorder temporarily replaces the ordinary composer controls"
      );
      require(
          std::ranges::any_of(
              m_floatingFrames,
              [this](const auto& frame) {
                return frame.y == m_composerTop && frame.height == layout::Composer::minimumHeight;
              }
          ),
          "Recording retains the normal single-line composer height"
      );
    } else if (step == 2) {
      const auto state = m_voiceMessageRecorder.state();
      require(state.error.empty(), "Synthetic microphone recording starts without an error");
      if (state.durationSeconds < .1) {
        m_window.invalidate();
        return;
      }
      click("voice-message:record");
    } else if (step == 3) {
      require(m_voiceMessageRecorder.state().error.empty(), "Synthetic recording finalizes without an error");
      if (!m_voiceMessageDraft) {
        m_window.invalidate();
        return;
      }
      require(
          api::validUploads({*m_voiceMessageDraft}) && m_voiceMessageDraft->voice->duration > 0,
          "Stop produces a genuine typed Ogg recording with waveform metadata"
      );
      require(
          api::mediaSourceHeader(
              {reinterpret_cast<const char*>(m_voiceMessageDraft->bytes.data()), m_voiceMessageDraft->bytes.size()},
              api::MediaSourceKind::OggAudio
          ),
          "Genuine recordings pass the same Ogg header validation as received CDN audio"
      );
      require(m_pendingSends.empty(), "Stopping never automatically sends the recording");
    } else if (step == 4) {
      require(
          m_voiceMessagePlaying.empty() && !m_widgets.contains("voice-preview"),
          "Stopping stays inline without opening a separate preview"
      );
    } else if (step == 5) {
      click("voice-message:send");
      require(
          m_pendingSends.size() == 1 && m_dialogKind.empty(),
          "Explicit Send queues one voice message and restores the ordinary composer"
      );
      require(
          m_drafts[m_channelId] == "Keep this text draft" && m_replies.contains(m_channelId),
          "Recording sends preserve unrelated text and reply drafts"
      );
    } else if (step == 6) {
      const auto nonce = m_pendingSends.begin()->first;
      const auto& pending = m_pendingSends.begin()->second;
      require(
          pending.voice
              && (pending.preview.flags & api::messageVoice)
              && !pending.preview.reply
              && pending.preview.content.empty(),
          "Pending voice messages stay independent of the ordinary composer"
      );
      api::Event failure{.operation = api::Operation::Send, .resource = m_channelId, .error = "Synthetic send failure"};
      failure.nonce = nonce;
      outgoingResult(failure);
      restoreOutgoing(nonce);
      require(m_voiceMessageDraft && m_pendingSends.empty(), "Explicit Restore recovers an uncertain recording");
    } else if (step == 7) {
      click("voice-message:send");
    } else if (step == 8) {
      require(m_pendingSends.size() == 1, "The restored recording sends only after another explicit click");
      access(false);
      revokeUnavailableOutgoing();
      auto& pending = m_pendingSends.begin()->second;
      require(pending.completed && pending.revoked->load(), "Voice permission loss revokes queued delivery");
      access(true);
      restoreOutgoing(m_pendingSends.begin()->first);
      require(m_voiceMessageDraft && m_pendingSends.empty(), "Permission recovery does not replay the failed send");
      api::Message received{
          .id = "9400",
          .channelId = m_channelId,
          .author = "Maya",
          .timestamp = "2026-11-02T14:00:00Z",
          .authorId = "11"
      };
      received.flags = api::messageVoice;
      received.attachments.push_back(
          {.filename = "voice-message.ogg",
           .url = "https://cdn.discordapp.com/attachments/1/2/voice-message.ogg",
           .contentType = "audio/ogg",
           .voice = m_voiceMessageDraft->voice}
      );
      m_messages = {received};
      received.id = "9401";
      received.timestamp = "2026-11-02T14:01:00Z";
      m_messages.push_back(received);
      received.id = "9402";
      received.timestamp = "2026-11-02T14:02:00Z";
      m_messages.push_back(received);
      m_history.restoreLatest(m_messages, true);
    } else if (step == 9) {
      require(m_voiceMessagePlaying.empty(), "Received voice messages never autoplay");
      const auto& layout = m_messageLayouts.at("9400").layout;
      require(layout.voice && layout.pictures.empty(), "Received audio uses the native waveform card");
      float previousTop = 0;
      for (const auto* id : {"9400", "9401", "9402"}) {
        float left = 0, top = 0, right = 0, bottom = 0;
        Node::transformedBounds(m_widgets.at(std::string("voice-message:") + id + ":play"), left, top, right, bottom);
        require(
            !previousTop
                || std::abs(top - previousTop - layout::VoiceMessage::height - layout::VoiceMessage::rowGap) < 1,
            "Grouped voice cards retain their measured gap through native scene layout"
        );
        previousTop = top;
      }
      require(!beginMessageEdit("9400"), "Voice messages cannot open the inline text editor");
      require(m_voiceMessageDraft && !m_voiceMessageDraft->bytes.empty(), "Playback retains the restored Ogg bytes");
      click("voice-message:9400:play");
      require(m_voiceMessagePlaying == "9400", "A native Play click opens the received audio player");
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(m_widgets.at("voice-message:9402:play"), left, top, right, bottom);
      m_window.motion((left + right) / 2, (top + bottom) / 2);
    } else if (step == 10) {
      const auto hoverColor = lerpColor(theme::background, theme::foreground, theme::lightTheme ? .035F : .04F);
      const auto band = std::ranges::find_if(m_scene->children(), [&](const auto& node) {
        return node->type() == NodeType::Rect
            && static_cast<const RectNode&>(*node).style().fill == hoverColor
            && node->x() >= chatLeft();
      });
      require(band != m_scene->children().end(), "Hovering a recording paints its message highlight");
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(band->get(), left, top, right, bottom);
      const auto bandCenter = (top + bottom) / 2;
      Node::transformedBounds(m_widgets.at("voice-message:9402:play"), left, top, right, bottom);
      require(
          std::abs((top + bottom) / 2 - bandCenter) < 1,
          "The received recording is vertically centered within its actual hover highlight"
      );
      const auto& state = m_voiceMessagePlayer.state();
      require(state.error.empty(), "The received Ogg recording opens in the audio-only player");
      if (state.loading || !state.hasAudio) {
        m_window.invalidate();
        return;
      }
      require(state.volume == 100, "Received recordings start at full playback volume");
      click("voice-message:9400:speed");
      require(m_voiceMessagePlayer.state().speed == 1.5, "Playback speed changes the actual player");
      Node::transformedBounds(m_widgets.at("voice-message:9400:seek"), left, top, right, bottom);
      m_window.motion(left + (right - left) / 4, (top + bottom) / 2);
    } else if (step == 11) {
      auto* seek = static_cast<Button*>(m_widgets.at("voice-message:9400:seek"));
      require(seek->hovered(), "The waveform receives native pointer hover");
      std::size_t previewDots = 0, otherDots = 0;
      const auto inspect = [&](const auto& self, const Node& node) -> void {
        if (node.type() == NodeType::Rect && node.width() == layout::VoiceMessage::barWidth) {
          const auto& rect = static_cast<const RectNode&>(node);
          if (rect.style().fill == theme::hover)
            ++previewDots;
          else
            ++otherDots;
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(previewDots > 0 && otherDots > previewDots, "Only dots up to the pointed seek position are highlighted");
      require(seek->opacity() == 0, "The entire seek control stays invisible, including its own painted surface");
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(seek, left, top, right, bottom);
      m_window.click(left + (right - left) / 4, (top + bottom) / 2, BTN_LEFT);
      m_window.released();
      click("voice-message:9400:volume");
      require(m_voiceMessageVolumeOpen, "The speaker opens a bounded volume popup");
    } else if (step == 12) {
      click("voice-message:volume-slider");
      require(
          std::abs(m_voiceMessagePlayer.state().volume - 50) < 1,
          "The vertical volume control changes actual playback volume"
      );
      onKey({XKB_KEY_Up});
      require(std::abs(m_voiceMessagePlayer.state().volume - 55) < 1, "The volume popup supports keyboard adjustment");
    } else if (step == 13) {
      auto* slider = static_cast<Button*>(m_widgets.at("voice-message:volume-slider"));
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(slider, left, top, right, bottom);
      const auto y = top + (bottom - top) * (1 - m_voiceMessagePlayer.state().volume / 100);
      m_window.motion((left + right) / 2, y);
      m_window.click((left + right) / 2, y, BTN_LEFT);
      require(slider->pressed() && m_controlInput.pointerCaptured(), "The painted volume thumb permits native capture");
    } else if (step == 14) {
      auto* slider = static_cast<Button*>(m_widgets.at("voice-message:volume-slider"));
      require(slider->pressed() && m_controlInput.pointerCaptured(), "Volume dragging survives a complete redraw");
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(slider, left, top, right, bottom);
      m_window.motion((left + right) / 2, top + (bottom - top) / 4);
      require(std::abs(m_voiceMessagePlayer.state().volume - 75) < 1, "Dragging the thumb changes actual volume");
    } else if (step == 15) {
      auto* slider = static_cast<Button*>(m_widgets.at("voice-message:volume-slider"));
      require(slider->pressed(), "The slider retains its press through another redraw");
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(slider, left, top, right, bottom);
      m_window.motion(right + Style::spaceLg, top - Style::spaceLg);
      require(std::abs(m_voiceMessagePlayer.state().volume - 100) < 1, "Captured drags clamp outside the popup");
      m_window.released();
      require(!slider->pressed() && !m_controlInput.pointerCaptured(), "Release ends volume capture");
      m_window.motion((left + right) / 2, (top + bottom) / 2);
      require(std::abs(m_voiceMessagePlayer.state().volume - 100) < 1, "Ordinary hover cannot change volume");
      onKey({XKB_KEY_Escape});
      require(
          !m_voiceMessageVolumeOpen && m_replies.contains(m_channelId),
          "Escape dismisses volume without cancelling an unrelated reply"
      );
    } else if (step == 16) {
      windowFocusChanged(false);
      require(m_voiceMessagePlaying.empty(), "Focus loss stops received recording playback");
      windowFocusChanged(true);
      clearVoiceMessage();
      openVoiceMessage(false);
    } else if (step == 17) {
      click("voice-message:discard");
      require(
          m_dialogKind.empty() && !m_voiceMessageDraft && m_voiceMessageChannel.empty(),
          "Discard releases local recordings without a send"
      );
      require(
          m_drafts[m_channelId] == "Keep this text draft" && m_replies.contains(m_channelId),
          "Discard and playback leave unrelated composer state intact"
      );
    } else if (step == 18) {
      openVoiceMessage(false);
      onKey({XKB_KEY_Escape});
      require(
          m_voiceMessageChannel.empty() && m_replies.contains(m_channelId),
          "Escape discards the inline recorder before cancelling a preserved reply"
      );
      m_window.close();
    }
    ++m_demoInteractionStep;
    std::clog << "Offline voice message step " << m_demoInteractionStep << " passed\n";
    m_window.invalidate();
  }
} // namespace app
