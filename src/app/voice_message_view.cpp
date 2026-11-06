#include "api/video_source.h"
#include "app/client_application.h"
#include "app/theme.h"
#include "ui/controls/button.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace app {
  namespace {
    constexpr std::array playbackSpeeds{1.0, 1.5, 2.0};
    std::string recordingTime(double seconds) {
      const auto total = static_cast<int>(std::clamp(seconds, 0.0, api::voiceMessageMaximumSeconds));
      return std::format("{}:{:02}", total / 60, total % 60);
    }
    void applicationPosition(const Node* scene, const Node* node, float& x, float& y) {
      float sx = 0, sy = 0;
      Node::mapToScene(node, 0, 0, sx, sy);
      (void)Node::mapFromScene(scene, sx, sy, x, y);
    }
  } // namespace

  bool ClientApplication::canRecordVoiceMessage() const {
    return m_connected
        && !m_channelId.empty()
        && selectedChannelWriteAccess() == api::MessageWriteAccess::Allowed
        && (m_dmMode || selectedCan(can::sendVoiceMessage));
  }

  void ClientApplication::stopVoiceMessagePlayback() {
    m_voiceMessagePlayer.close();
    m_voiceMessagePlaying.clear();
    m_voiceMessagePlayRequested = false;
    m_voiceMessageVolumeOpen = false;
  }

  void ClientApplication::clearVoiceMessage() {
    ++m_voiceMessageGeneration;
    m_voiceMessageRecorder.cancel();
    m_voiceMessageDraft.reset();
    m_voiceMessageChannel.clear();
    m_voiceMessageError.clear();
    stopVoiceMessagePlayback();
  }

  void ClientApplication::openVoiceMessage(bool record) {
    if (!canRecordVoiceMessage() || !m_dialogKind.empty() || !m_voiceMessageChannel.empty())
      return;
    closeContext();
    closeCompletion();
    closeGifPicker();
    closeEmojiPicker();
    cancelMessageEdit(false);
    cancelSlashCommand(false);
    m_scrolling.stopAll();
    m_voiceMessageChannel = m_channelId;
    m_voiceMessageError.clear();
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    if (record)
      m_voiceMessageRecorder.start(m_preferences.voice.inputDevice, m_preferences.voice.inputVolume);
    m_window.invalidate();
  }

  void ClientApplication::voiceMessageAction(const Hit& hit) {
    if (!m_dialogKind.empty() || settingsVisible())
      return;
    const auto message = std::ranges::find(m_messages, hit.id, &api::Message::id);
    if (message == m_messages.end()
        || message->deleted
        || blockedMessage(*message)
        || !(message->flags & api::messageVoice)
        || message->attachments.size() != 1
        || !message->attachments.front().contentType.starts_with("audio/"))
      return;
    const auto url = api::mediaUrl(message->attachments.front().url);
    if (url.empty()) {
      m_status = "This voice message has no supported audio source.";
      m_window.invalidate();
      return;
    }
    if (m_voiceMessagePlaying == hit.id) {
      const auto& state = m_voiceMessagePlayer.state();
      if (state.ended)
        m_voiceMessagePlayer.seek(0);
      m_voiceMessagePlayer.pause(state.playing);
      m_window.invalidate();
      return;
    }
    stopVoiceMessagePlayback();
    const auto input = !m_demo ? api::videoSource(url, api::MediaSourceKind::OggAudio)
        : m_voiceMessageDraft  ? media::memoryVideoInput(m_voiceMessageDraft->bytes)
                               : nullptr;
    if (!input || !m_voiceMessagePlayer.openAudio(input, m_demo))
      m_status = "Could not open this voice message.";
    else {
      constexpr double voicePlaybackVolume = 100;
      m_voiceMessagePlayer.volume(voicePlaybackVolume);
      m_voiceMessagePlaying = hit.id;
      m_voiceMessagePlayRequested = true;
    }
    m_window.invalidate();
  }

  void ClientApplication::drawVoiceMessage(
      Node& parent, const api::Message& message, const MessageLayout& body, float x, float y
  ) {
    using namespace theme;
    using V = layout::VoiceMessage;
    if (!body.voice || blockedMessage(message))
      return;
    auto frames = *body.voice;
    for (auto* f : {&frames.card, &frames.play, &frames.waveform, &frames.time, &frames.speed, &frames.volume}) {
      f->x += x;
      f->y += y;
    }
    const auto* attachment = message.attachments.size() == 1 ? &message.attachments.front() : nullptr;
    const auto* metadata = attachment && attachment->voice ? &*attachment->voice : nullptr;
    const bool current = m_voiceMessagePlaying == message.id;
    const auto& state = m_voiceMessagePlayer.state();
    const bool enabled = !message.deleted
        && attachment
        && attachment->contentType.starts_with("audio/")
        && !api::mediaUrl(attachment->url).empty()
        && (!m_demo || m_demoOverlay.starts_with("voice-messages-cycle"));
    rectangle(
        parent, frames.card.x, frames.card.y, frames.card.width, frames.card.height, panel, frames.card.height / 2
    );
    const auto id = "voice-message:" + message.id;
    pollButton(
        parent, id + ":play", frames.play, "",
        [this, messageId = message.id] { voiceMessageAction({0, 0, 0, 0, Action::VoiceMessagePlayback, messageId}); },
        enabled, true
    );
    auto* play = static_cast<Button*>(m_widgets.at(id + ":play"));
    play->setRadius(V::button / 2);
    play->setGlyph(current && state.playing ? "player-pause-filled" : "player-play-filled");
    play->setGlyphSize(Style::glyphSizeSmall);
    play->setTooltip(current && state.playing ? "Pause" : "Play");
    if (current && !state.error.empty()) {
      label(
          parent, frames.waveform.x, frames.waveform.y, frames.waveform.width, "Playback failed", Style::fontSizeMini,
          danger
      );
    } else if (frames.waveform.width > 0) {
      pollButton(
          parent, id + ":seek", frames.waveform, "",
          [this, id, messageId = message.id] {
            if (m_voiceMessagePlaying != messageId || m_voiceMessagePlayer.state().loading)
              return;
            const auto found = m_widgets.find(id + ":seek");
            if (found == m_widgets.end())
              return;
            float sx = 0, sy = 0;
            applicationPosition(m_scene.get(), found->second, sx, sy);
            const auto fraction = std::clamp((m_pointerX - sx) / std::max(1.0F, found->second->width()), 0.0F, 1.0F);
            m_voiceMessagePlayer.seek(fraction * m_voiceMessagePlayer.state().duration);
          },
          enabled && current && !state.loading
      );
      auto* seek = static_cast<Button*>(m_widgets.at(id + ":seek"));
      seek->setOpacity(0);
      seek->setOnPointerMotion([this](float, float) { m_window.invalidate(); });
      float seekX = 0, seekY = 0;
      applicationPosition(m_scene.get(), seek, seekX, seekY);
      const bool preview = seek->enabled() && seek->hovered();
      const auto previewFraction = std::clamp((m_pointerX - seekX) / std::max(1.0F, frames.waveform.width), 0.0F, 1.0F);
      const auto count = metadata ? metadata->waveform.size() : std::size_t{1};
      const float step = std::max(V::barWidth * 2, frames.waveform.width / count);
      const auto bars = std::max(std::size_t{1}, static_cast<std::size_t>(frames.waveform.width / step));
      const double duration = current && state.duration > 0 ? state.duration : metadata ? metadata->duration : 0;
      seek->setTooltip(preview ? "Seek to " + recordingTime(previewFraction * duration) : "Seek");
      auto waveform = std::make_unique<Node>();
      waveform->setHitTestVisible(false);
      for (std::size_t i = 0; i < bars; ++i) {
        const auto sample = metadata ? metadata->waveform[std::min(count - 1, i * count / bars)] : 0;
        const float h = std::max(V::minimumBarHeight, frames.waveform.height * sample / 255.0F);
        const auto fraction = (static_cast<double>(i) + .5) / bars;
        const auto color = preview && fraction <= previewFraction             ? hover
            : current && duration > 0 && fraction < state.position / duration ? accent
                                                                              : muted;
        rectangle(
            *waveform, frames.waveform.x + i * step, frames.waveform.y + (frames.waveform.height - h) / 2, V::barWidth,
            h, color, V::barWidth / 2
        );
      }
      parent.addChild(std::move(waveform));
    }
    const auto text = current && state.loading ? "..."
        : current                              ? recordingTime(state.position)
        : metadata                             ? recordingTime(metadata->duration)
                                               : "--:--";
    const auto metrics = m_window.target().renderer().measureText(
        text, Style::fontSizeDescription, FontWeight::Normal, frames.time.width, 1
    );
    label(
        parent, frames.time.x,
        layout::Chat::centeredTextTop(
            frames.time.y + frames.time.height / 2, metrics.top, metrics.inkTop, metrics.inkBottom
        ),
        frames.time.width, text, Style::fontSizeDescription, foreground
    );
    if (frames.speed.width > 0) {
      pollButton(
          parent, id + ":speed", frames.speed, std::format("{:g}X", current ? state.speed : 1.0),
          [this] {
            const auto at = std::ranges::find(playbackSpeeds, m_voiceMessagePlayer.state().speed);
            m_voiceMessagePlayer.speed(
                at == playbackSpeeds.end() || std::next(at) == playbackSpeeds.end() ? playbackSpeeds.front()
                                                                                    : *std::next(at)
            );
          },
          enabled && current
      );
      auto* speed = static_cast<Button*>(m_widgets.at(id + ":speed"));
      speed->setFontSize(Style::fontSizeMini);
      speed->setTooltip("Playback speed");
      pollButton(
          parent, id + ":volume", frames.volume, "", [this] { m_voiceMessageVolumeOpen = !m_voiceMessageVolumeOpen; },
          enabled && current
      );
      auto* volume = static_cast<Button*>(m_widgets.at(id + ":volume"));
      volume->setGlyph(current && (state.muted || state.volume == 0) ? "volume-off" : "volume");
      volume->setGlyphSize(Style::glyphSizeSmall);
      volume->setTooltip("Volume");
      if (current) {
        applicationPosition(m_scene.get(), volume, m_voiceMessageVolumeAnchor.x, m_voiceMessageVolumeAnchor.y);
        m_voiceMessageVolumeAnchor.width = frames.volume.width;
        m_voiceMessageVolumeAnchor.height = frames.volume.height;
      }
    }
  }

  void ClientApplication::drawVoiceMessageVolume(Node& root, float width, float height) {
    using namespace theme;
    using V = layout::VoiceMessage;
    if (!m_voiceMessageVolumeOpen)
      return;
    if (settingsVisible() || !m_dialogKind.empty() || m_contextOpen || m_voiceMessagePlaying.empty()) {
      m_voiceMessageVolumeOpen = false;
      return;
    }
    const float w = V::button + V::padding * 2, h = V::volumeHeight + V::padding * 2;
    const float x =
        std::clamp(m_voiceMessageVolumeAnchor.x - V::padding, V::padding, std::max(V::padding, width - w - V::padding));
    const float y = std::clamp(
        m_voiceMessageVolumeAnchor.y - h - V::gap, layout::Window::titlebarHeight,
        std::max(layout::Window::titlebarHeight, height - h - V::padding)
    );
    m_voiceMessageVolumeBounds = {x, y, w, h};
    pollButton(root, "voice-message:volume-dismiss", {0, 0, width, height}, "", [this] {
      m_voiceMessageVolumeOpen = false;
    });
    auto* dismiss = static_cast<Button*>(m_widgets.at("voice-message:volume-dismiss"));
    auto palette = Button::defaultPalette(ButtonVariant::Ghost);
    palette.normal.bg = palette.hover.bg = palette.pressed.bg = clearColorSpec();
    dismiss->setCustomPalette(palette);
    dismiss->setTabStop(false);
    rectangle(root, x, y, w, h, raised, Style::radiusLg);
    pollButton(
        root, "voice-message:volume-slider", {x + V::padding, y + V::padding, V::button, V::volumeHeight}, "", [] {}
    );
    auto* slider = static_cast<Button*>(m_widgets.at("voice-message:volume-slider"));
    slider->setCustomPalette(palette);
    slider->setTooltip("Volume");
    const auto adjust = [this](float localY) {
      using V = layout::VoiceMessage;
      m_voiceMessagePlayer.volume(std::clamp(1.0 - localY / V::volumeHeight, 0.0, 1.0) * 100);
      m_voiceMessagePlayer.mute(false);
      m_window.invalidate();
    };
    slider->setOnPress([adjust](float, float localY, bool pressed) {
      if (pressed)
        adjust(localY);
    });
    slider->setOnPointerMotion([slider, adjust](float, float localY) {
      if (slider->pressed())
        adjust(localY);
    });
    const float trackX = x + w / 2 - Style::sliderTrackHeight / 2;
    const auto& state = m_voiceMessagePlayer.state();
    const float filled = V::volumeHeight * (state.muted ? 0 : state.volume / 100);
    auto decoration = std::make_unique<Node>();
    decoration->setHitTestVisible(false);
    rectangle(*decoration, trackX, y + V::padding, Style::sliderTrackHeight, V::volumeHeight, muted, Style::radiusXs);
    rectangle(
        *decoration, trackX, y + V::padding + V::volumeHeight - filled, Style::sliderTrackHeight, filled, accent,
        Style::radiusXs
    );
    rectangle(
        *decoration, x + (w - Style::sliderThumbSize) / 2,
        y + V::padding + V::volumeHeight - filled - Style::sliderThumbSize / 2, Style::sliderThumbSize,
        Style::sliderThumbSize, foreground, Style::sliderThumbSize / 2
    );
    root.addChild(std::move(decoration));
  }

  float ClientApplication::voiceMessageRecorderHeight(float width) {
    const auto state = m_voiceMessageRecorder.state();
    const auto& error = m_voiceMessageError.empty() ? state.error : m_voiceMessageError;
    if (error.empty())
      return layout::VoiceRecorder::height;
    const auto metrics = m_window.target().renderer().measureText(
        error, Style::fontSizeDescription, FontWeight::Normal,
        std::max(Style::spaceLg, width - layout::VoiceRecorder::padding * 2), 0
    );
    return layout::VoiceRecorder::height + std::ceil(metrics.bottom - metrics.top) + layout::VoiceRecorder::padding;
  }

  void ClientApplication::drawVoiceMessageRecorder(Node& root, layout::Frame frame) {
    using namespace theme;
    const auto frames = layout::voiceRecorderFrames(frame);
    const auto state = m_voiceMessageRecorder.state();
    const bool busy =
        state.phase == voice::MessageRecorderPhase::Loading || state.phase == voice::MessageRecorderPhase::Recording;
    const auto scoped = [this, channel = m_channelId,
                         generation = m_voiceMessageGeneration](std::function<void()> action) {
      return [this, channel, generation, action = std::move(action)] {
        if (m_voiceMessageGeneration == generation && m_channelId == channel && m_voiceMessageChannel == channel)
          action();
      };
    };
    const auto status = state.phase == voice::MessageRecorderPhase::Loading ? "Preparing microphone..."
        : busy                ? "Recording " + recordingTime(state.durationSeconds)
        : m_voiceMessageDraft ? recordingTime(m_voiceMessageDraft->voice->duration) + " ready to send"
                              : "Recording stopped";
    const auto metrics = m_window.target().renderer().measureText(status, Style::fontSizeDescription);
    label(
        root, frames.status.x,
        layout::Chat::centeredTextTop(
            frames.status.y + frames.status.height / 2, metrics.top, metrics.inkTop, metrics.inkBottom
        ),
        frames.status.width, status, Style::fontSizeDescription, busy ? danger : foreground
    );
    pollButton(root, "voice-message:discard", frames.discard, "", scoped([this] {
                 clearVoiceMessage();
                 focusChatComposer();
               }));
    auto* discard = static_cast<Button*>(m_widgets.at("voice-message:discard"));
    discard->setGlyph("trash");
    discard->setTooltip("Discard recording");
    pollButton(
        root, "voice-message:record", frames.record, "", scoped([this, busy] {
          if (busy)
            m_voiceMessageRecorder.stop();
          else if (!m_voiceMessageDraft && canRecordVoiceMessage()) {
            m_voiceMessageError.clear();
            m_voiceMessageRecorder.start(m_preferences.voice.inputDevice, m_preferences.voice.inputVolume);
          }
        }),
        busy || (!m_voiceMessageDraft && canRecordVoiceMessage())
    );
    auto* record = static_cast<Button*>(m_widgets.at("voice-message:record"));
    record->setGlyph(busy ? "player-stop-filled" : "microphone");
    record->setTooltip(busy ? "Stop recording" : "Record again");
    pollButton(
        root, "voice-message:send", frames.send, "", scoped([this] {
          if (!m_voiceMessageDraft || !canRecordVoiceMessage()) {
            m_voiceMessageError = "You can no longer send this recording here.";
            return;
          }
          if (queueOutgoing("", {*m_voiceMessageDraft}, {}, {}, false)) {
            clearVoiceMessage();
            focusChatComposer();
          } else
            m_voiceMessageError = m_status.empty() ? "Could not queue this recording. It is retained." : m_status;
        }),
        m_voiceMessageDraft.has_value() && !busy && canRecordVoiceMessage(), true
    );
    auto* send = static_cast<Button*>(m_widgets.at("voice-message:send"));
    send->setGlyph("send");
    send->setTooltip("Send recording");
    const auto& error = m_voiceMessageError.empty() ? state.error : m_voiceMessageError;
    if (!error.empty())
      label(
          root, frame.x + layout::VoiceRecorder::padding, frame.y + layout::VoiceRecorder::height,
          frame.width - layout::VoiceRecorder::padding * 2, error, Style::fontSizeDescription, danger,
          FontWeight::Normal, 0
      );
  }

  void ClientApplication::voiceMessageTick() {
    const auto state = m_voiceMessageRecorder.drain();
    if (state.revision != m_voiceMessageRecorderRevision) {
      m_voiceMessageRecorderRevision = state.revision;
      if (state.phase == voice::MessageRecorderPhase::Ready
          && state.recording
          && !m_voiceMessageChannel.empty()
          && m_voiceMessageChannel == m_channelId
          && !m_voiceMessageDraft) {
        m_voiceMessageDraft = api::Upload{
            "voice-message.ogg", "audio/ogg", state.recording->ogg,
            api::VoiceMessageMetadata{state.recording->durationSeconds, state.recording->waveform}
        };
        m_voiceMessageRecorder.cancel();
      }
      m_window.invalidate();
    }
    if (!m_voiceMessageChannel.empty() && (m_voiceMessageChannel != m_channelId || !m_connected || settingsVisible()))
      clearVoiceMessage();
    if ((state.phase == voice::MessageRecorderPhase::Recording || state.phase == voice::MessageRecorderPhase::Loading)
        && ((!m_demo && (!m_window.active() || !m_window.visible())) || !canRecordVoiceMessage())) {
      m_voiceMessageRecorder.stop();
      m_voiceMessageError = "Recording stopped because focus or channel access changed.";
    }
    if (m_voiceMessagePlayer.drain()) {
      if (!m_voiceMessagePlayer.state().error.empty())
        m_status = "Voice message playback failed: " + m_voiceMessagePlayer.state().error;
      m_window.invalidate();
    }
    if (m_voiceMessagePlaying.empty())
      return;
    const auto message = std::ranges::find(m_messages, m_voiceMessagePlaying, &api::Message::id);
    if ((!m_demo && (!m_window.active() || !m_window.visible()))
        || settingsVisible()
        || !m_dialogKind.empty()
        || message == m_messages.end()
        || message->deleted
        || blockedMessage(*message)) {
      stopVoiceMessagePlayback();
      return;
    }
    if (m_voiceMessagePlayRequested
        && !m_voiceMessagePlayer.state().loading
        && m_voiceMessagePlayer.state().error.empty()
        && m_voiceMessagePlayer.state().hasAudio) {
      m_voiceMessagePlayRequested = false;
      m_voiceMessagePlayer.pause(false);
    }
  }
} // namespace app
