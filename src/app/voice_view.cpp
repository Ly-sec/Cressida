#include "app/client_application.h"
#include "app/theme.h"
#include "app/user_settings_form.h"
#include "render/scene/input_area.h"

#include <algorithm>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    constexpr auto voiceJoinDeadline = std::chrono::seconds{20};
    constexpr float percentage = 100;
    constexpr std::size_t maximumVisibleVoiceNames = 3;
  } // namespace
  bool ClientApplication::voiceBarVisible() const { return m_voiceState.phase != voice::Phase::Disconnected; }
  void ClientApplication::releasePushToTalk() {
    if (!m_voicePtt)
      return;
    m_voicePtt = false;
    m_voicePttKey = 0;
    configureVoice();
  }
  std::expected<std::string, std::string> ClientApplication::applyVoiceCommand(VoiceCommand command) {
    if (m_account.id.empty())
      return std::unexpected("Sign in to Cressida before using voice controls.");
    switch (command) {
    case VoiceCommand::InputMuteToggle:
    case VoiceCommand::InputMuteOn:
    case VoiceCommand::InputMuteOff:
      m_voiceMuted = command == VoiceCommand::InputMuteToggle ? !m_voiceMuted : command == VoiceCommand::InputMuteOn;
      releasePushToTalk();
      m_voiceState.muted = m_voiceMuted;
      configureVoice();
      m_window.invalidate();
      return m_voiceMuted   ? "Microphone muted."
          : m_voiceDeafened ? "Microphone unmuted; deafening still blocks transmission."
                            : "Microphone unmuted.";
    default:
      return std::unexpected("Unknown voice command.");
    }
  }
  void ClientApplication::configureVoice() {
    if (m_demo)
      return;
    auto settings = m_preferences.voice;
    settings.muted = m_voiceMuted;
    settings.deafened = m_voiceDeafened;
    settings.pttPressed = m_voicePtt;
    m_client.configureVoice(std::move(settings));
  }
  void ClientApplication::voiceTick() {
    if (m_demo)
      return;
    const auto audio = m_client.voiceAudio();
    audio->drainEvents();
    const auto state = audio->state();
    if (state.revision == m_voiceAudioState.revision)
      return;
    m_voiceAudioState = state;
    if (!state.error.empty()
        && (m_voiceState.phase == voice::Phase::Connected
            || m_voiceState.phase == voice::Phase::WaitingForParticipants
            || m_voiceState.phase == voice::Phase::Securing)) {
      m_client.leaveVoice();
      releasePushToTalk();
      m_voiceState.phase = voice::Phase::Failed;
      m_voiceState.error = state.error;
    }
    m_window.invalidate();
  }
  void ClientApplication::voiceAction(const std::string& action) {
    if (action == "share") {
      sharingAction("open");
    } else if (action == "settings") {
      openSettings();
      selectSettingsPage("voice");
    } else if (action == "refresh") {
      if (!m_demo)
        m_client.voiceAudio()->refreshDevices();
    } else if (action == "test") {
      if (m_demo) {
        m_voiceAudioState.testing = !m_voiceAudioState.testing;
        m_voiceAudioState.inputLevel = m_voiceAudioState.testing ? voice::defaultActivityThreshold : 0;
      } else if (m_voiceAudioState.testing || m_voiceAudioState.loading) {
        m_client.voiceAudio()->stopTest();
      } else {
        configureVoice();
        auto testSettings = m_preferences.voice;
        testSettings.muted = testSettings.deafened = testSettings.pushToTalk = testSettings.pttPressed = false;
        m_client.voiceAudio()->configure(std::move(testSettings));
        std::string error;
        if (!m_client.voiceAudio()->startTest(error))
          m_settingsStatus = std::move(error);
      }
    } else if (action == "leave") {
      releasePushToTalk();
      m_voiceJoinTimer.stop();
      if (!m_demo)
        m_client.leaveVoice();
      else
        m_voiceRoster.apply({.members = {{.guild = m_voiceGuild, .user = m_account.id}}});
      m_voiceState = {};
      m_voiceGuild.clear();
    } else if (action == "mute") {
      m_voiceMuted = !m_voiceMuted;
      releasePushToTalk();
      m_voiceState.muted = m_voiceMuted;
      configureVoice();
    } else if (action == "deafen") {
      m_voiceDeafened = !m_voiceDeafened;
      releasePushToTalk();
      m_voiceState.deafened = m_voiceDeafened;
      configureVoice();
    } else if (action.starts_with("join:")) {
      const auto id = action.substr(5);
      const auto channel = std::ranges::find(m_channels, id, &api::Channel::id);
      if (channel == m_channels.end()
          || channel->isMessageRequest()
          || (channel->type != 2 && channel->type != 1 && channel->type != 3))
        return;
      if (voiceBarVisible() && m_voiceState.channel == id && m_voiceState.phase != voice::Phase::Failed)
        return;
      if (!m_dmMode) {
        constexpr std::uint64_t connectPermission = 1ULL << 20, speakPermission = 1ULL << 21;
        const auto permissions = channelPermissionsFor(id).value_or(0);
        if (!(permissions & api::viewChannel) || !(permissions & connectPermission)) {
          m_status = "You do not have permission to join this voice channel.";
          return;
        }
        if (!(permissions & speakPermission))
          m_voiceMuted = true;
      }
      releasePushToTalk();
      m_voiceGuild = m_dmMode ? "" : m_guildId;
      auto settings = m_preferences.voice;
      settings.muted = m_voiceMuted;
      settings.deafened = m_voiceDeafened;
      if (m_demo) {
        m_voiceRoster.apply(
            {.members = {
                 {.guild = m_voiceGuild,
                  .channel = id,
                  .user = m_account.id,
                  .name = m_account.name,
                  .avatar = m_account.avatar}
             }}
        );
        m_voiceState = {
            .phase = voice::Phase::Connected,
            .channel = id,
            .muted = m_voiceMuted,
            .deafened = m_voiceDeafened,
            .encrypted = true,
            .participants = {{m_account.id, false}, {"11", true}}
        };
      } else if (m_client.joinVoice(m_dmMode ? "" : m_guildId, id, settings)) {
        m_voiceState = {
            .phase = voice::Phase::Connecting, .channel = id, .muted = m_voiceMuted, .deafened = m_voiceDeafened
        };
        m_voiceJoinTimer.start(voiceJoinDeadline, [this] {
          if (m_voiceState.phase != voice::Phase::Connecting && m_voiceState.phase != voice::Phase::Securing)
            return;
          m_client.leaveVoice();
          releasePushToTalk();
          m_voiceState.phase = voice::Phase::Failed;
          m_voiceState.error = "Voice connection timed out. Join again to retry.";
          m_window.invalidate();
        });
      } else {
        m_voiceState = {
            .phase = voice::Phase::Failed,
            .channel = id,
            .error = "Voice is unavailable. Check your connection and join again."
        };
      }
    }
    m_window.invalidate();
  }
  void ClientApplication::drawVoiceSettings(UserSettingsForm& form) {
    auto& settings = m_preferences.voice;
    const auto chooseDevice = [&](bool input) {
      const auto& available = input ? m_voiceAudioState.inputs : m_voiceAudioState.outputs;
      const auto& current = input ? settings.inputDevice : settings.outputDevice;
      std::vector<std::string> names{"System default"}, ids{""};
      for (const auto& device : available)
        if (!device.id.empty() && std::ranges::find(ids, device.id) == ids.end()) {
          names.push_back(device.name);
          ids.push_back(device.id);
        }
      if (std::ranges::find(ids, current) == ids.end()) {
        ids.push_back(current);
        names.push_back("Unavailable saved device");
      }
      const auto selected = static_cast<std::size_t>(std::ranges::find(ids, current) - ids.begin());
      form.choose(
          input ? "voice-input" : "voice-output", input ? "Input device" : "Output device",
          input ? "Choose the microphone used when you join or start a test." : "Choose where call audio plays.",
          std::move(names), selected, [this, ids, input](std::size_t index) {
            if (index >= ids.size())
              return;
            (input ? m_preferences.voice.inputDevice : m_preferences.voice.outputDevice) = ids[index];
            savePreferences();
          }
      );
    };
    chooseDevice(true);
    chooseDevice(false);
    const auto refresh = form.row("Audio devices", "Device choices are saved for this account.", 0, true);
    form.rowButton(refresh, "Refresh devices", "voice:refresh");
    form.slider(
        "voice-input-volume", "Input volume", "Adjust microphone gain.", settings.inputVolume * percentage, 0,
        voice::maximumAudioVolume * percentage, 1,
        [this](double value) {
          m_preferences.voice.inputVolume = value / percentage;
          savePreferences();
        },
        "%"
    );
    form.slider(
        "voice-output-volume", "Output volume", "Adjust the volume of received voices.",
        settings.outputVolume * percentage, 0, voice::maximumAudioVolume * percentage, 1,
        [this](double value) {
          m_preferences.voice.outputVolume = value / percentage;
          savePreferences();
        },
        "%"
    );
    form.section("Microphone activation");
    form.choose(
        "voice-input-mode", "Input mode", "Push-to-talk works while Cressida has keyboard focus.",
        {"Voice activity", "Push to talk"}, settings.pushToTalk ? 1 : 0, [this](std::size_t index) {
          releasePushToTalk();
          m_preferences.voice.pushToTalk = index == 1;
          savePreferences();
        }
    );
    if (settings.pushToTalk)
      form.row("Push-to-talk shortcut", shortcutLabel("push_to_talk") + ". Change it under Keybinds.");
    else
      form.slider(
          "voice-threshold", "Activity threshold", "Transmit when the microphone level exceeds this threshold.",
          settings.threshold * percentage, 0, percentage, 1,
          [this](double value) {
            m_preferences.voice.threshold = value / percentage;
            savePreferences();
          },
          "%"
      );
    form.section("Microphone test");
    const bool testing = m_voiceAudioState.testing || m_voiceAudioState.loading;
    const bool inCall = m_voiceState.phase != voice::Phase::Disconnected && m_voiceState.phase != voice::Phase::Failed;
    const auto test = form.row(
        "Listen to your microphone",
        "Start a local microphone test through the selected output. Stop it before joining a call.", 0, true
    );
    form.rowButton(
        test, testing ? "Stop test" : "Start test", "voice:test", ButtonVariant::Default, Style::spaceSection * 3,
        !inCall
    );
    form.row("Microphone level", std::to_string(static_cast<int>(m_voiceAudioState.inputLevel * percentage)) + "%");
    if (!m_voiceAudioState.error.empty())
      form.row("Audio unavailable", m_voiceAudioState.error);
    form.row("Video and processing", "Camera calls, echo cancellation and noise suppression are not available.");
  }
  void ClientApplication::drawVoiceBar(Node& root, float height) {
    if (!voiceBarVisible())
      return;
    using namespace theme;
    const auto frame = layout::Voice::frame(height, chatLeft());
    floatingPanel(root, frame, background);
    const auto statusIcon = layout::Voice::connectionIcon(frame), statusText = layout::Voice::connectionText(frame);
    const auto publisher =
        std::ranges::find_if(m_streamSnapshot.active, [](const auto& stream) { return stream.publishing; });
    const bool sharing = publisher != m_streamSnapshot.active.end() || m_sharingPublicationRequested;
    const auto sharePhase =
        publisher == m_streamSnapshot.active.end() ? voice::Phase::Connecting : publisher->connection.phase;
    const bool shareFailed = sharePhase == voice::Phase::Failed
        || (m_sharingPublicationRequested && publisher == m_streamSnapshot.active.end() && !m_sharingStatus.empty());
    const bool shareReady = sharePhase == voice::Phase::Connected || sharePhase == voice::Phase::WaitingForParticipants;
    const auto statusColor = m_voiceState.phase == voice::Phase::Failed || shareFailed ? danger
        : sharing && !shareReady                                                       ? muted
                                                                                       : success;
    rectangle(root, statusIcon.x, statusIcon.y, statusIcon.width, statusIcon.height, panel, Style::radiusMd);
    glyph(
        root, statusIcon.x + (statusIcon.width - Style::glyphSizeLarge) / 2,
        statusIcon.y + (statusIcon.height - Style::glyphSizeLarge) / 2, Style::glyphSizeLarge,
        sharing                                          ? "screen-share"
            : m_voiceState.phase == voice::Phase::Failed ? "broadcast-off"
                                                         : "broadcast",
        statusColor
    );
    const auto roster = m_voiceRoster.members(m_voiceGuild, m_voiceState.channel);
    const auto title = sharing                          ? shareFailed ? "Screen share failed"
            : !shareReady                    ? "Starting screen share…"
            : publisher->paused              ? "Sharing paused"
                                             : "Screen sharing live"
        : m_voiceState.phase == voice::Phase::Connected ? "Voice connected"
        : m_voiceState.phase == voice::Phase::WaitingForParticipants
        ? (roster.size() <= 1 ? "Voice connected" : "Securing voice…")
        : m_voiceState.phase == voice::Phase::Failed   ? "Voice disconnected"
        : m_voiceState.phase == voice::Phase::Securing ? "Securing voice…"
                                                       : "Connecting voice…";
    label(
        root, statusText.x, statusText.y, statusText.width, title, Style::fontSizeCaption, statusColor,
        FontWeight::Medium
    );
    std::string location;
    if (const auto* channel = serverChannel(m_voiceGuild, m_voiceState.channel))
      location = channel->name;
    else if (
        const auto channel = std::ranges::find(m_privateChannels, m_voiceState.channel, &api::Channel::id);
        channel != m_privateChannels.end()
    )
      location = channelName(*channel);
    if (const auto guild = std::ranges::find(m_guilds, m_voiceGuild, &api::Guild::id); guild != m_guilds.end()) {
      if (!location.empty())
        location += " / ";
      location += guild->name;
    }
    std::string detail = m_voiceState.error;
    if (detail.empty() && m_voiceState.phase == voice::Phase::WaitingForParticipants)
      detail = roster.size() <= 1 ? "No other participants" : "Waiting for encryption keys";
    if (detail.empty())
      detail = location;
    if (detail.empty() && !roster.empty()) {
      std::size_t shown = 0;
      for (const auto& member : roster) {
        if (!detail.empty())
          detail += ", ";
        detail += userName(member.user, member.name.empty() ? "Participant" : member.name);
        if (++shown >= maximumVisibleVoiceNames)
          break;
      }
    }
    if (detail.empty()) {
      std::size_t shown = 0;
      for (const auto& participant : m_voiceState.participants) {
        if (!detail.empty())
          detail += ", ";
        detail += userName(participant.user, participant.user == m_account.id ? m_account.name : "Participant");
        if (++shown >= maximumVisibleVoiceNames)
          break;
      }
      if (detail.empty())
        detail = "Waiting for the voice server";
    }
    label(
        root, statusText.x, statusText.y + Style::fontSizeCaption + Style::spaceXs, statusText.width, detail,
        Style::fontSizeDetail, muted
    );
    if (hovered(statusIcon.x, statusIcon.y, statusText.x + statusText.width - statusIcon.x, statusIcon.height)) {
      m_tooltip = location.empty() || location == detail ? detail : location + ": " + detail;
      m_tooltipX = statusText.x + statusText.width / 2;
      m_tooltipY = statusText.y;
    }
    const auto controls = layout::Voice::controls(frame);
    const std::array<std::string, 5> actions{"mute", "deafen", "share", "settings", "leave"};
    const std::array<std::string, 5> icons{
        m_voiceMuted || m_voiceDeafened ? "microphone-off" : "microphone",
        m_voiceDeafened ? "headphones-off" : "headphones", "screen-share", "settings", "phone-end"
    };
    const std::array<std::string, 5> captions{
        m_voiceMuted ? "Unmute microphone" : "Mute microphone", m_voiceDeafened ? "Undeafen" : "Deafen",
        "Screen sharing and live streams", "Voice settings", "Disconnect voice"
    };
    for (std::size_t i = 0; i < actions.size(); ++i) {
      const auto& control = controls[i];
      const bool over = hovered(control.x, control.y, control.width, control.height);
      const bool stopped = (i == 0 && (m_voiceMuted || m_voiceDeafened)) || (i == 1 && m_voiceDeafened);
      const bool sharingControl = actions[i] == "share" && sharing;
      const bool leave = actions[i] == "leave";
      const Hit hit{control.x, control.y, control.width, control.height, Action::VoiceAction, actions[i]};
      const auto id = "voice-button:" + actions[i];
      std::unique_ptr<Node> saved;
      if (auto found = m_detachedInputs.find(id); found != m_detachedInputs.end()) {
        saved = std::move(found->second);
        m_detachedInputs.erase(found);
      }
      auto button = saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
      m_widgets[id] = button.get();
      button->setGlyph(icons[i]);
      button->setGlyphSize(Style::glyphSizeLarge);
      button->setPosition(control.x, control.y);
      button->setFrameSize(control.width, control.height);
      button->setControlHeight(control.height);
      button->setPadding(0);
      button->setRadius(Style::scaledRadiusMd());
      button->setVariant(leave ? ButtonVariant::Ghost : ButtonVariant::Default);
      auto colors = Button::defaultPalette(leave ? ButtonVariant::Ghost : ButtonVariant::Default);
      colors.borderWidth = 0;
      const bool focused = button->inputArea() && button->inputArea()->focused();
      colors.selected = Button::ButtonStateColors{
          ColorSpec{.fixed = over || focused ? raised : panel}, clearColorSpec(), ColorSpec{.fixed = danger}
      };
      if (leave) {
        colors.normal = {clearColorSpec(), clearColorSpec(), ColorSpec{.fixed = muted}};
        colors.hover = {
            ColorSpec{.fixed = destructive}, clearColorSpec(),
            ColorSpec{.fixed = readableTextColorForBackground(destructive)}
        };
        colors.pressed = colors.hover;
      }
      if (sharingControl)
        colors.selected = Button::ButtonStateColors{
            ColorSpec{.fixed = raised}, clearColorSpec(), ColorSpec{.fixed = shareFailed ? danger : accent}
        };
      button->setCustomPalette(std::move(colors));
      button->setSelected(stopped || sharingControl);
      button->setTooltip(captions[i]);
      button->setOnClick([this, hit] {
        activate(hit);
        m_window.invalidate();
      });
      root.addChild(std::move(button));
      m_hits.push_back(hit);
      if (over) {
        m_tooltip = captions[i];
        m_tooltipX = control.x + control.width / 2;
        m_tooltipY = control.y;
      }
    }
  }
  std::size_t ClientApplication::voiceMemberCount(const api::Channel& channel) const {
    return !m_dmMode && channel.type == 2 ? m_voiceRoster.count(m_guildId, channel.id) : 0;
  }
  float ClientApplication::voiceMembersHeight(const api::Channel& channel) const {
    return layout::Voice::membersHeight(voiceMemberCount(channel));
  }
  void ClientApplication::drawVoiceMembers(Node& root, const api::Channel& channel, float y, float top, float bottom) {
    if (m_dmMode || channel.type != 2 || bottom <= top || y >= bottom || y + voiceMembersHeight(channel) <= top)
      return;
    const auto members = m_voiceRoster.members(m_guildId, channel.id);
    if (members.empty())
      return;
    auto list = std::make_unique<Node>();
    const float x = layout::Sidebar::railWidth + layout::Voice::memberIndent;
    const float width = std::max(0.0F, chatLeft() - layout::Floating::inset - Style::spaceSm - x);
    list->setPosition(x, top);
    list->setSize(width, bottom - top);
    list->setClipChildren(true);
    list->setHitTestVisible(false);
    for (const auto& member : members) {
      const float rowTop = y;
      y += layout::Voice::memberStep;
      const float clippedTop = std::max(top, rowTop),
                  clippedBottom = std::min(bottom, rowTop + layout::Voice::memberHeight);
      if (clippedBottom <= clippedTop)
        continue;
      const bool muted = member.muted || member.selfMuted || member.suppressed;
      const bool deafened = member.deafened || member.selfDeafened;
      const bool talking = !muted
          && m_voiceState.channel == channel.id
          && std::ranges::any_of(m_voiceState.participants, [&](const auto& participant) {
               return participant.user == member.user && participant.speaking;
             });
      const bool over = hovered(x, clippedTop, width, clippedBottom - clippedTop);
      const float localY = rowTop - top;
      if (over)
        rectangle(*list, 0, localY, width, layout::Voice::memberHeight, theme::panel, Style::radiusSm);
      const auto fallback = member.user == m_account.id ? m_account.name
          : member.name.empty()                         ? "Participant"
                                                        : member.name;
      const auto name = userName(member.user, fallback);
      const float avatarY = localY + (layout::Voice::memberHeight - layout::Voice::memberAvatar) / 2;
      if (talking)
        rectangle(
            *list, layout::Voice::memberAvatarX - layout::Voice::memberSpeakingStroke,
            avatarY - layout::Voice::memberSpeakingStroke,
            layout::Voice::memberAvatar + layout::Voice::memberSpeakingStroke * 2,
            layout::Voice::memberAvatar + layout::Voice::memberSpeakingStroke * 2, theme::success,
            layout::Voice::memberAvatar / 2 + layout::Voice::memberSpeakingStroke
        );
      avatar(
          *list, layout::Voice::memberAvatarX, avatarY, layout::Voice::memberAvatar,
          member.avatar.empty() && member.user == m_account.id ? m_account.avatar : member.avatar, name
      );
      const auto stream = std::ranges::find_if(m_streamSnapshot.available, [&](const auto& record) {
        return record.key.guild == m_guildId && record.key.channel == channel.id && record.key.owner == member.user;
      });
      float right = width;
      if (stream != m_streamSnapshot.available.end())
        right -= layout::Sharing::liveWidth + Style::spaceXs;
      if (deafened) {
        right -= layout::Voice::memberStatus + Style::spaceXs;
        glyph(
            *list, right, localY + (layout::Voice::memberHeight - layout::Voice::memberStatus) / 2,
            layout::Voice::memberStatus, "headphones-off", theme::muted
        );
      }
      if (muted || talking) {
        right -= layout::Voice::memberStatus + Style::spaceXs;
        glyph(
            *list, right, localY + (layout::Voice::memberHeight - layout::Voice::memberStatus) / 2,
            layout::Voice::memberStatus, muted ? "microphone-off" : "microphone",
            talking ? theme::success : theme::muted
        );
      }
      const float textX = layout::Voice::memberAvatarX + layout::Voice::memberAvatar + Style::spaceSm;
      const auto weight = talking ? FontWeight::Medium : FontWeight::Normal;
      const auto metrics = m_window.target().renderer().measureText(name, Style::fontSizeCaption, weight);
      label(
          *list, textX, localY + (layout::Voice::memberHeight - (metrics.bottom - metrics.top)) / 2,
          std::max(0.0F, right - textX - Style::spaceXs), name, Style::fontSizeCaption,
          talking    ? theme::success
              : over ? theme::foreground
                     : theme::muted,
          weight
      );
      m_hits.push_back({x, clippedTop, width, clippedBottom - clippedTop, Action::UserProfile, member.user});
      m_contextTargets.push_back({x, clippedTop, width, clippedBottom - clippedTop, Action::UserMenu, member.user});
      if (stream != m_streamSnapshot.available.end()) {
        const auto badge = layout::Sharing::liveBadge(width, localY);
        const float badgeTop = std::max(top, top + badge.y),
                    badgeBottom = std::min(bottom, top + badge.y + badge.height);
        const layout::Frame liveHit{x + badge.x, badgeTop, badge.width, std::max(0.0F, badgeBottom - badgeTop)};
        const bool liveOver = liveHit.contains(m_pointerX, m_pointerY);
        if (liveHit.contains(m_pointerX, m_pointerY) && member.user != m_account.id)
          hoverStreamPreview(stream->key.value(), liveHit);
        rectangle(
            *list, badge.x, badge.y, badge.width, badge.height,
            lerpColor(
                theme::sidebar, theme::danger, liveOver ? Style::textHighlightAlpha * 2 : Style::textHighlightAlpha
            ),
            Style::radiusSm
        );
        const auto liveMetrics =
            m_window.target().renderer().measureText("LIVE", Style::fontSizeDetail, FontWeight::Bold);
        label(
            *list, badge.x + (badge.width - liveMetrics.width) / 2,
            badge.y + (badge.height - (liveMetrics.bottom - liveMetrics.top)) / 2, badge.width - Style::spaceXs, "LIVE",
            Style::fontSizeDetail, theme::danger, FontWeight::Bold
        );
        if (liveHit.height > 0)
          m_hits.push_back(
              {liveHit.x, liveHit.y, liveHit.width, liveHit.height, Action::ShareAction,
               member.user == m_account.id ? "view:" + stream->key.value() : "watch:" + stream->key.value()}
          );
      }
    }
    root.addChild(std::move(list));
  }
} // namespace app
