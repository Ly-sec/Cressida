#include "app/client_application.h"
#include "app/theme.h"
#include "app/voice_command.h"
#include "render/scene/rect_node.h"
#include "render/scene/text_node.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <linux/input-event-codes.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    constexpr std::string_view voiceDemoDraft = "Keep this draft while talking";
    constexpr std::string_view voiceDemoChannel = "107";
  } // namespace
  void ClientApplication::prepareVoiceDemo(const std::string& view) {
    m_demoOverlay = view;
    m_demoInteractionStep = 0;
    m_dmMode = false;
    m_dmPage.clear();
    m_showMembers = false;
    m_voiceState = {};
    m_voiceMuted = m_voiceDeafened = m_voicePtt = false;
    m_voicePttKey = 0;
    m_preferences.voice = {};
    m_preferences.keybinds.erase("push_to_talk");
    if (view.ends_with("light"))
      m_preferences.theme = "light";
    if (view.ends_with("narrow"))
      m_window.setUiScale(1.6F);
    auto channel = std::ranges::find(m_channels, voiceDemoChannel, &api::Channel::id);
    if (channel == m_channels.end()) {
      m_channels.push_back({std::string(voiceDemoChannel), "Voice lounge", 2});
      channel = std::prev(m_channels.end());
    }
    channel->type = 2;
    channel->overwrites.emplace();
    m_channels = api::orderChannels(std::move(m_channels));
    m_guildChannels[m_guildId] = m_channels;
    const auto roster = api::voiceRosterUpdate(
        "READY",
        {{"users", {{{"id", "11"}, {"username", "Maya, Jr."}}, {{"id", "12"}, {"username", "Noah"}}}},
         {"guilds",
          {{{"id", m_guildId},
            {"voice_states",
             {{{"user_id", "11"}, {"channel_id", voiceDemoChannel}, {"self_mute", false}},
              {{"user_id", "12"}, {"channel_id", voiceDemoChannel}, {"self_mute", true}, {"self_deaf", true}}}}}}}}
    );
    if (!roster)
      throw std::runtime_error("The voice replay requires a parsed channel roster");
    m_voiceRoster.apply(*roster);
    m_roleDirectory.apply({false, {{.guild = m_guildId, .owner = m_account.id}}});
    m_drafts["101"] = voiceDemoDraft;
    m_voiceAudioState = {
        .inputs = {{"demo-mic-one", "Desk microphone"}, {"demo-mic-two", "Headset microphone"}},
        .outputs = {{"demo-output-one", "Desktop speakers"}, {"demo-output-two", "Headphones"}}
    };
    openSettings();
    selectSettingsPage("voice");
  }
  void ClientApplication::demoVoiceStep() {
    const auto require = [](bool condition, const char* message) {
      if (!condition)
        throw std::runtime_error(message);
    };
    const auto widget = [&](const std::string& id) -> Node* {
      const auto found = m_widgets.find(id);
      require(
          found != m_widgets.end() && found->second && found->second->parent(),
          "The voice replay uses a mounted native settings control"
      );
      return found->second;
    };
    const auto clickWidget = [&](const std::string& id, float fraction = 0.5F) {
      auto* control = widget(id);
      float x = 0, y = 0;
      Node::mapToScene(control, control->width() * fraction, control->height() / 2, x, y);
      for (auto* ancestor = control->parent(); ancestor; ancestor = ancestor->parent()) {
        if (!ancestor->clipChildren())
          continue;
        float left = 0, top = 0;
        Node::mapToScene(ancestor, 0, 0, left, top);
        require(
            x >= left && x < left + ancestor->width() && y >= top && y < top + ancestor->height(),
            "Voice settings clicks stay inside the native clipped viewport"
        );
      }
      y += m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      m_window.released();
    };
    const auto revealWidget = [&](const std::string& id) {
      auto* control = widget(id);
      auto* viewport = control->parent();
      while (viewport && !viewport->clipChildren())
        viewport = viewport->parent();
      require(viewport, "Voice settings have a clipped scrolling viewport");
      float x = 0, y = 0, viewportX = 0, viewportY = 0;
      Node::mapToScene(control, 0, control->height() / 2, x, y);
      Node::mapToScene(viewport, 0, 0, viewportX, viewportY);
      m_scrolling.stop(m_settingsScroll);
      m_settingsScroll = std::max(0.0F, m_settingsScroll + y - viewportY - viewport->height() / 2);
    };
    const auto clickAction = [&](Action action, const std::string& id = "") {
      const auto found = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == action && (id.empty() || hit.id == id) && hit.width > 0 && hit.height > 0;
      });
      require(found != m_hits.end(), "The voice replay requires a visible native action target");
      const auto hit = *found;
      const float x = hit.x + hit.width / 2,
                  y = hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      m_window.released();
    };
    const auto chooseLast = [&] {
      require(isSelectDropdownOpen(), "The native voice Select opens its retained dropdown");
      m_window.key({.symbol = XKB_KEY_End});
      m_window.key({.symbol = XKB_KEY_Return});
      require(!isSelectDropdownOpen() && m_settingsOpen, "Choosing a voice option closes only its dropdown");
    };
    const auto ptt = [&] { m_window.key({.symbol = XKB_KEY_space, .control = true, .shift = true}); };
    const auto command = [&](VoiceCommand value) {
      const auto result = applyVoiceCommand(value);
      require(result.has_value(), "The voice command succeeds for the current account and call state");
    };
    const auto speakingVisible = [&] {
      bool ring = false, name = false;
      const auto inspect = [&](auto&& self, const Node& node, float x, float y) -> void {
        x += node.x();
        y += node.y();
        if (x >= layout::Sidebar::railWidth && x < chatLeft()) {
          if (node.type() == NodeType::Rect
              && node.width() == layout::Voice::memberAvatar + layout::Voice::memberSpeakingStroke * 2
              && static_cast<const RectNode&>(node).style().fill == theme::success)
            ring = true;
          if (node.type() == NodeType::Text) {
            const auto& text = static_cast<const TextNode&>(node);
            if (text.fontWeight() == FontWeight::Medium
                && text.fontSize() == Style::fontSizeCaption
                && text.text() == userName("11", "Maya, Jr."))
              name = true;
          }
        }
        for (const auto& child : node.children())
          self(self, *child, x, y);
      };
      inspect(inspect, *m_scene, 0, 0);
      return ring && name;
    };
    const auto step = m_demoInteractionStep++;
    std::clog << "Offline voice frame " << step << '\n';
    switch (step) {
    case 0:
      require(m_settingsOpen && m_settingsPage == "voice", "The voice replay starts on its native settings page");
      {
        const auto account = std::exchange(m_account.id, {});
        require(
            !applyVoiceCommand(VoiceCommand::InputMuteToggle)
                && !applyVoiceCommand(VoiceCommand::InputMuteOn)
                && !applyVoiceCommand(VoiceCommand::InputMuteOff)
                && !m_voiceMuted,
            "Mute commands cannot change microphone state without a signed-in account"
        );
        m_account.id = account;
      }
      command(VoiceCommand::InputMuteOn);
      command(VoiceCommand::InputMuteOn);
      require(m_voiceMuted, "Repeated explicit mute commands remain muted before joining a call");
      command(VoiceCommand::InputMuteOff);
      command(VoiceCommand::InputMuteOff);
      require(!m_voiceMuted, "Repeated explicit unmute commands remain unmuted before joining a call");
      command(VoiceCommand::InputMuteToggle);
      require(m_voiceMuted, "The mute toggle changes the current account's local state");
      command(VoiceCommand::InputMuteToggle);
      require(!m_voiceMuted, "A second mute toggle restores the original local state");
      require(
          static_cast<Select*>(widget("user-account-select:voice-input"))->selectedIndex() == 0,
          "Input starts with the system default"
      );
      require(
          static_cast<Select*>(widget("user-account-select:voice-output"))->selectedIndex() == 0,
          "Output starts with the system default"
      );
      clickWidget("user-account-select:voice-input");
      break;
    case 1:
      chooseLast();
      require(
          m_preferences.voice.inputDevice == "demo-mic-two", "The native input selector saves the actual device ID"
      );
      break;
    case 2:
      clickWidget("user-account-select:voice-output");
      break;
    case 3:
      chooseLast();
      require(
          m_preferences.voice.outputDevice == "demo-output-two", "The native output selector saves the actual device ID"
      );
      revealWidget("user-account-slider:voice-input-volume");
      break;
    case 4:
      clickWidget("user-account-slider:voice-input-volume", 0.75F);
      clickWidget("user-account-slider:voice-output-volume", 0.25F);
      require(
          m_preferences.voice.inputVolume > 1 && m_preferences.voice.outputVolume < 1,
          "Real slider pointer actions save independent input and output gains"
      );
      require(
          std::abs(
              static_cast<Slider*>(widget("user-account-slider:voice-input-volume"))->value() / 100
              - m_preferences.voice.inputVolume
          ) < 0.001,
          "The retained input slider and saved gain agree"
      );
      revealWidget("user-account-select:voice-input-mode");
      break;
    case 5:
      clickWidget("user-account-select:voice-input-mode");
      break;
    case 6:
      chooseLast();
      require(m_preferences.voice.pushToTalk, "The native input-mode selector enables push to talk");
      revealWidget("user-account-button:voice:test");
      break;
    case 7:
      clickWidget("user-account-button:voice:test");
      require(
          m_voiceAudioState.testing && m_voiceAudioState.inputLevel > 0,
          "The actual microphone-test button starts local demo metering"
      );
      clickWidget("user-account-button:voice:test");
      require(
          !m_voiceAudioState.testing && m_voiceAudioState.inputLevel == 0,
          "A second test click stops the local microphone test"
      );
      require(
          !m_profileDirty && m_settingsDraft == m_accountSettings,
          "Voice preferences remain local without creating account-setting mutations"
      );
      clickAction(Action::CloseSettings);
      break;
    case 8:
      require(!m_settingsOpen, "The native close control returns from voice settings to chat");
      {
        const auto text = std::ranges::find_if(m_hits, [&](const auto& hit) {
          return hit.action == Action::Channel && hit.id == "101";
        });
        const auto voice = std::ranges::find_if(m_hits, [&](const auto& hit) {
          return hit.action == Action::Channel && hit.id == voiceDemoChannel;
        });
        require(
            text != m_hits.end() && voice != m_hits.end() && text->y + text->height <= voice->y,
            "Voice channels follow text channels even when their server positions overlap"
        );
      }
      for (const auto& user : {"11", "12"}) {
        const auto participant = std::ranges::find_if(m_hits, [&](const auto& hit) {
          return hit.action == Action::UserProfile && hit.id == user && hit.x < chatLeft();
        });
        require(participant != m_hits.end(), "Connected people appear below the voice channel before joining it");
        for (const auto& hit : m_hits)
          if (hit.action == Action::Channel)
            require(
                participant->y + participant->height <= hit.y || participant->y >= hit.y + hit.height,
                "Voice member profile hits stay separate from channel selection hits"
            );
      }
      clickAction(Action::Channel, std::string(voiceDemoChannel));
      require(
          m_voiceState.phase == voice::Phase::Connected
              && m_voiceState.channel == voiceDemoChannel
              && m_voiceState.encrypted,
          "Clicking a real voice-channel row joins the simulated encrypted conversation"
      );
      require(m_drafts["101"] == voiceDemoDraft, "Joining voice preserves the previous conversation draft");
      break;
    case 9: {
      require(
          speakingVisible(), "A speaking participant paints a visible success-color avatar ring and emphasized name"
      );
      const float height = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
      const auto bar = layout::Voice::frame(height, chatLeft());
      const auto account = layout::accountFrame(height, chatLeft());
      require(
          bar.y > layout::Floating::inset && bar.y + bar.height + layout::Floating::gap == account.y,
          "The voice bar shares the floating sidebar gutter above the account card"
      );
      for (const auto& hit : m_hits) {
        const bool overlap = hit.x < bar.x + bar.width
            && hit.x + hit.width > bar.x
            && hit.y < bar.y + bar.height
            && hit.y + hit.height > bar.y;
        if (hit.action == Action::Channel)
          require(!overlap, "Voice controls never overlap a covered channel-row hit region");
        if (hit.action == Action::VoiceAction
            && (hit.id == "mute" || hit.id == "deafen" || hit.id == "leave" || hit.id == "settings"))
          require(
              hit.x >= bar.x
                  && hit.x + hit.width <= bar.x + bar.width
                  && hit.y >= bar.y
                  && hit.y + hit.height <= bar.y + bar.height,
              "Every voice action stays inside its shared bar bounds"
          );
      }
      ptt();
      require(m_voicePtt, "The configured shortcut opens native push to talk");
      command(VoiceCommand::InputMuteOn);
      command(VoiceCommand::InputMuteOn);
      require(
          m_voiceMuted && m_voiceState.muted && !m_voicePtt,
          "An idempotent mute command mutes the current call and releases a native held key"
      );
      command(VoiceCommand::InputMuteOff);
      require(!m_voiceMuted && !m_voiceState.muted, "An explicit unmute command restores the call's local input state");
      ptt();
      require(m_voicePtt, "Native push to talk can be pressed after an explicit unmute");
      command(VoiceCommand::InputMuteOff);
      require(!m_voicePtt && !m_voiceMuted, "An idempotent unmute command also releases a native held key");
      clickAction(Action::VoiceAction, "mute");
      require(m_voiceMuted && m_voiceState.muted, "The native mute control updates the joined conversation");
      clickAction(Action::VoiceAction, "deafen");
      require(m_voiceDeafened && m_voiceState.deafened, "The native deafen control updates the joined conversation");
      clickAction(Action::VoiceAction, "deafen");
      clickAction(Action::VoiceAction, "mute");
      ptt();
      require(m_voicePtt, "The configured shortcut opens push to talk in the focused chat");
      m_window.keyReleased(XKB_KEY_Shift_L);
      require(!m_voicePtt, "Releasing a shortcut modifier cancels push to talk");
      ptt();
      require(m_voicePtt, "Push to talk can be pressed again after release");
      m_window.focusChanged(false);
      require(!m_voicePtt, "Native focus loss cancels a held push-to-talk key");
      for (auto& participant : m_voiceState.participants)
        participant.speaking = false;
      break;
    }
    case 10:
      require(!speakingVisible(), "A participant returning to silence removes the painted speaking indicator");
      clickAction(Action::VoiceAction, "settings");
      require(
          m_settingsOpen && m_settingsPage == "voice" && m_voiceState.phase == voice::Phase::Connected,
          "The voice-bar settings control preserves the joined conversation"
      );
      ptt();
      require(!m_voicePtt, "Covered settings controls cannot activate native push to talk");
      break;
    case 11:
      require(
          m_preferences.voice.inputDevice == "demo-mic-two"
              && m_preferences.voice.outputDevice == "demo-output-two"
              && m_preferences.voice.pushToTalk,
          "Device and activation preferences survive settings navigation"
      );
      require(
          static_cast<Select*>(widget("user-account-select:voice-input-mode"))->selectedIndex() == 1,
          "The retained input-mode selector reflects the saved push-to-talk choice"
      );
      clickAction(Action::CloseSettings);
      m_scrolling.stop(m_channelScroll);
      m_channelScroll = 0;
      break;
    case 12:
      clickAction(Action::VoiceAction, "leave");
      require(
          !voiceBarVisible() && !m_voicePtt && m_voiceState.phase == voice::Phase::Disconnected,
          "The native disconnect control retires the call and held shortcut"
      );
      clickAction(Action::Channel, "101");
      require(
          m_channelId == "101" && m_drafts["101"] == voiceDemoDraft && m_pendingInteractions.empty(),
          "Returning to the original chat retains its draft without account requests"
      );
      m_voiceRoster.clear();
      voiceAction("join:" + std::string(voiceDemoChannel));
      m_voiceState.phase = voice::Phase::WaitingForParticipants;
      m_voiceState.encrypted = false;
      m_voiceState.participants = {{m_account.id, false}};
      break;
    case 13: {
      bool connected = false, alone = false;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text) {
          const auto& text = static_cast<const TextNode&>(node);
          connected |= text.text() == "Voice connected";
          alone |= text.text() == "No other participants";
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(
          connected && alone && !speakingVisible(),
          "A lone connected member sees an explicit alone state without a stuck server message"
      );
      m_window.focusChanged(true);
      ptt();
      require(m_voicePtt, "The native shortcut also works in a joined solo channel");
      command(VoiceCommand::InputMuteOff);
      require(!m_voicePtt && !m_voiceMuted, "A mute command releases the native shortcut in a solo call");
      clickAction(Action::VoiceAction, "leave");
      break;
    }
    case 14:
      require(
          !voiceBarVisible() && m_voiceRoster.count(m_guildId, std::string(voiceDemoChannel)) == 0,
          "Leaving a solo channel removes its local participant and connected state"
      );
      std::clog << "Offline voice settings, native controls, call actions and focus lifecycle passed\n";
      break;
    default:
      require(false, "The voice replay has a fixed bounded frame count");
    }
    m_window.invalidate();
  }
} // namespace app
