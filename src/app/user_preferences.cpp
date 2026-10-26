#include "app/client_application.h"
#include "app/theme.h"

#include <algorithm>
#include <charconv>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace app {
  namespace {
    const std::map<std::string, std::string> defaultShortcuts = {
        {"settings", "Ctrl+comma"},
        {"search", "Ctrl+f"},
        {"inbox", "Ctrl+i"},
        {"jump_latest", "Ctrl+End"},
        {"toggle_members", "Ctrl+u"},
        {"push_to_talk", "Ctrl+Shift+space"},
        {"next_channel", "Alt+Down"},
        {"previous_channel", "Alt+Up"},
        {"next_server", "Ctrl+Alt+Down"},
        {"previous_server", "Ctrl+Alt+Up"},
        {"quit", "Ctrl+q"},
        {"zoom_in", "Ctrl+equal"},
        {"zoom_out", "Ctrl+minus"},
        {"zoom_reset", "Ctrl+0"}
    };
    std::string chord(const Key& key) {
      char name[80]{};
      auto symbol = xkb_keysym_to_lower(key.symbol);
      if (symbol == XKB_KEY_plus || symbol == XKB_KEY_KP_Add)
        symbol = XKB_KEY_equal;
      if (symbol == XKB_KEY_KP_Subtract)
        symbol = XKB_KEY_minus;
      if (symbol == XKB_KEY_KP_0)
        symbol = XKB_KEY_0;
      if (!xkb_keysym_get_name(symbol, name, sizeof(name)))
        return {};
      return std::string(key.control ? "Ctrl+" : "")
          + (key.alt ? "Alt+" : "")
          + (key.super ? "Super+" : "")
          + (key.shift && symbol != XKB_KEY_equal ? "Shift+" : "")
          + name;
    }
  } // namespace
  std::string ClientApplication::shortcutLabel(const std::string& action) const {
    if (m_shortcutCapture == action)
      return "Press a shortcut…";
    if (const auto found = m_preferences.keybinds.find(action); found != m_preferences.keybinds.end())
      return found->second.empty() ? "Unassigned" : found->second;
    const auto found = defaultShortcuts.find(action);
    return found == defaultShortcuts.end() ? "Unassigned" : found->second;
  }
  bool ClientApplication::extendedPreference(const std::string& action) {
    if (windowControlPreference(action))
      return true;
    const std::pair<std::string_view, bool Preferences::*> flags[] = {
        {"reduced_motion", &Preferences::reducedMotion},
        {"gifs_with_reduced_motion", &Preferences::gifsWithReducedMotion},
        {"emoji_with_reduced_motion", &Preferences::emojiWithReducedMotion},
        {"animate_emoji", &Preferences::animateEmoji},
        {"animate_avatars", &Preferences::animateAvatars},
        {"avatar_decorations", &Preferences::showAvatarDecorations},
        {"nameplates", &Preferences::showNameplates},
        {"profile_effects", &Preferences::showProfileEffects},
        {"profile_frames", &Preferences::showProfileFrames},
        {"profile_banners", &Preferences::showProfileBanners},
        {"profile_themes", &Preferences::showProfileThemes},

        {"developer_mode", &Preferences::developerMode},
        {"desktop_decorations", &Preferences::desktopDecorations},
        {"convert_emoticons", &Preferences::convertEmoticons},
        {"randomize_media_filenames", &Preferences::randomizeMediaFilenames},
        {"twelve_hour_clock", &Preferences::twelveHourClock},
        {"show_reactions", &Preferences::showReactions},
        {"show_role_icons", &Preferences::showRoleIcons},
        {"desktop_notifications", &Preferences::desktopNotifications},
        {"notification_sounds", &Preferences::notificationSounds}
    };
    for (const auto& [name, member] : flags)
      if (action == name) {
        m_preferences.*member = !(m_preferences.*member);
        if (name == "reduced_motion")
          m_scrolling.stopAll();
        if (name == "desktop_notifications" && !m_preferences.desktopNotifications)
          m_desktopNotifications.clear();
        savePreferences();
        return true;
      }
    struct Number {
      std::string_view name;
      int Preferences::* member;
      int maximum;
    };
    const Number numbers[] = {
        {"saturation:", &Preferences::saturation, 100},
        {"corner_radius:", &Preferences::cornerRadius, Style::cornerRadiusMaximumPercent},
        {"spoiler_mode:", &Preferences::spoilerMode, 2},
        {"role_color_mode:", &Preferences::roleColorMode, 2},
        {"message_group_spacing:", &Preferences::messageGroupSpacing, 24},
        {"notification_privacy:", &Preferences::notificationPrivacy, 2}
    };
    for (const auto& setting : numbers)
      if (action.starts_with(setting.name)) {
        const auto number = std::string_view(action).substr(setting.name.size());
        int value = 0;
        const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (error == std::errc{} && end == number.data() + number.size() && value >= 0 && value <= setting.maximum) {
          if (setting.member == &Preferences::notificationPrivacy && value > m_preferences.notificationPrivacy)
            m_desktopNotifications.clear();
          m_preferences.*setting.member = value;
          m_preferences.roleColors = m_preferences.roleColorMode != 2;
          savePreferences();
        }
        return true;
      }
    if (action.starts_with("keybind-record:")) {
      const auto name = action.substr(15);
      if (defaultShortcuts.contains(name)) {
        m_shortcutCapture = name;
        m_controlInput.setFocus(nullptr);
        m_focusedInput.clear();
        m_settingsStatus.clear();
      }
      return true;
    }
    if (action == "keybind-reset-all") {
      m_preferences.keybinds.clear();
      m_shortcutCapture.clear();
      savePreferences();
      return true;
    }
    if (action.starts_with("keybind-reset:")) {
      m_preferences.keybinds.erase(action.substr(14));
      m_shortcutCapture.clear();
      savePreferences();
      return true;
    }
    return false;
  }
  bool ClientApplication::shortcutKey(const Key& key) {
    if (!m_shortcutCapture.empty()) {
      if (key.symbol == XKB_KEY_Escape) {
        m_shortcutCapture.clear();
      } else if (
          key.symbol != XKB_KEY_Control_L
          && key.symbol != XKB_KEY_Control_R
          && key.symbol != XKB_KEY_Shift_L
          && key.symbol != XKB_KEY_Shift_R
          && key.symbol != XKB_KEY_Alt_L
          && key.symbol != XKB_KEY_Alt_R
          && key.symbol != XKB_KEY_Super_L
          && key.symbol != XKB_KEY_Super_R
      ) {
        const auto value = key.symbol == XKB_KEY_BackSpace || key.symbol == XKB_KEY_Delete ? "" : chord(key);
        if (!value.empty() && !key.control && !key.alt && !key.super && key.symbol < XKB_KEY_F1) {
          m_settingsStatus = "Use Control, Alt or Super with a key, or a function key.";
        } else {
          bool duplicate = false;
          for (const auto& [name, fallback] : defaultShortcuts) {
            const auto at = m_preferences.keybinds.find(name);
            if (name != m_shortcutCapture
                && !value.empty()
                && value == (at == m_preferences.keybinds.end() ? fallback : at->second))
              duplicate = true;
          }
          if (duplicate)
            m_settingsStatus = "That shortcut is already assigned to another action.";
          else {
            m_preferences.keybinds[m_shortcutCapture] = value;
            m_shortcutCapture.clear();
            m_settingsStatus.clear();
            savePreferences();
          }
        }
      }
      m_window.invalidate();
      return true;
    }
    if (!m_connected
        || userAccountDialogOpen()
        || !m_dialogKind.empty()
        || m_contextOpen
        || m_selectRequest
        || !m_viewImage.empty())
      return false;
    const auto pressed = chord(key);
    for (const auto& [name, fallback] : defaultShortcuts) {
      const auto at = m_preferences.keybinds.find(name);
      if (pressed != (at == m_preferences.keybinds.end() ? fallback : at->second))
        continue;
      if (name == "quit")
        m_window.close();
      else if (name == "settings")
        openSettings();
      else if (name.starts_with("zoom_")) {
        m_window.setUiScale(name == "zoom_reset" ? 1 : m_window.uiScale() * (name == "zoom_in" ? 1.1F : 1 / 1.1F));
        saveSelection();
      } else if (settingsVisible())
        return false;
      else if (name == "push_to_talk") {
        if (!m_preferences.voice.pushToTalk
            || (m_voiceState.phase != voice::Phase::Connected
                && m_voiceState.phase != voice::Phase::WaitingForParticipants
                && m_voiceState.phase != voice::Phase::Securing))
          return false;
        if (!m_voicePtt) {
          m_voicePtt = true;
          m_voicePttKey = key.symbol;
          configureVoice();
        }
      } else if (name == "jump_latest")
        activate({0, 0, 0, 0, Action::JumpLatest, {}});
      else if (name == "inbox")
        toggleInbox();
      else if (name == "search") {
        m_channelPanel = "search";
        m_focusedInput = "channel-search-panel";
      } else if (name == "toggle_members") {
        m_showMembers = !m_showMembers;
      } else if (name == "next_channel" || name == "previous_channel") {
        const auto& channels = m_dmMode ? m_privateChannels : m_channels;
        if (channels.empty())
          return true;
        auto at = std::ranges::find(channels, m_channelId, &api::Channel::id);
        auto position = at == channels.end() ? 0 : static_cast<int>(at - channels.begin());
        for (std::size_t count = 0; count < channels.size(); ++count) {
          position =
              (position + static_cast<int>(channels.size()) + (name == "next_channel" ? 1 : -1)) % channels.size();
          if (channels[position].type == 4 || channels[position].type == 2 || channels[position].type == 13)
            continue;
          if (!m_dmMode && !(channelPermissionsFor(channels[position].id).value_or(0) & (1ULL << 10)))
            continue;
          selectChannel(channels[position].id);
          break;
        }
      } else if (name == "next_server" || name == "previous_server") {
        std::vector<std::string> servers;
        for (const auto& folder : m_folders) {
          for (const auto& id : folder.guildIds) {
            if (std::ranges::find(m_guilds, id, &api::Guild::id) != m_guilds.end())
              servers.push_back(id);
          }
        }
        if (!servers.empty()) {
          const auto at = std::ranges::find(servers, m_guildId);
          const auto step = name == "next_server" ? 1 : -1;
          auto position = at == servers.end() ? (step > 0 ? -1 : 0) : static_cast<int>(at - servers.begin());
          position = (position + static_cast<int>(servers.size()) + step) % static_cast<int>(servers.size());
          if (servers[position] != m_guildId)
            selectGuild(servers[position]);
          m_revealGuild = true;
        }
      }
      m_window.invalidate();
      return true;
    }
    return false;
  }
  bool ClientApplication::spoilersVisible(const std::string& message) const {
    if (m_preferences.spoilerMode == 2 || m_revealedSpoilers.contains(message))
      return true;
    return m_preferences.spoilerMode == 1
        && !m_dmMode
        && (channelPermissionsFor(m_channelId).value_or(0) & (1ULL << 13));
  }
} // namespace app
