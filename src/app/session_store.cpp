#include "app/session_store.h"

#include "api/actions.h"
#include "api/emoji.h"
#include "api/models.h"
#include "app/font_catalog.h"
#include "app/theme_catalog.h"
#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace app {
  namespace {
    constexpr Logger logger{"session"};
    enum class StateFailure : unsigned {
      Open,
      Read,
      Size,
      Parse,
      Inspect,
      Directory,
      Write,
      Temporary,
      Permissions,
      Close,
      Commit
    };
    void stateFailure(StateFailure failure, const char* stage) {
      static std::atomic_uint reported = 0;
      const auto bit = 1U << static_cast<unsigned>(failure);
      if (!(reported.fetch_or(bit, std::memory_order_relaxed) & bit))
        logger.warn("local state failed stage={} repeated_warnings=suppressed", stage);
    }
    nlohmann::json appearance(const Preferences& preferences) {
      return {
          {"theme", validColorMode(preferences.theme) ? preferences.theme : "dark"},
          {"color_theme", validThemeId(preferences.colorTheme) ? preferences.colorTheme : "cressida"}
      };
    }
    Preferences colorPreferences(const nlohmann::json& value) {
      Preferences result;
      if (!value.is_object())
        return result;
      if (const auto at = value.find("theme");
          at != value.end() && at->is_string() && validColorMode(at->get<std::string>()))
        result.theme = at->get<std::string>();
      if (const auto at = value.find("color_theme");
          at != value.end() && at->is_string() && validThemeId(at->get<std::string>()))
        result.colorTheme = at->get<std::string>();
      return result;
    }
    nlohmann::json read(const std::filesystem::path& path) {
      std::ifstream file(path, std::ios::binary);
      if (!file.is_open()) {
        std::error_code error;
        if (std::filesystem::exists(path, error) || error)
          stateFailure(StateFailure::Open, "read-open");
        else {
          static std::atomic_flag reported = ATOMIC_FLAG_INIT;
          if (!reported.test_and_set(std::memory_order_relaxed))
            logger.debug("local state absent; using defaults");
        }
        return nlohmann::json::object();
      }
      std::string bytes(65537, '\0');
      file.read(bytes.data(), bytes.size());
      bytes.resize(file.gcount());
      if (file.bad())
        stateFailure(StateFailure::Read, "read");
      if (bytes.size() > 65536) {
        stateFailure(StateFailure::Size, "read-limit");
        return nlohmann::json::object();
      }
      auto value = nlohmann::json::parse(bytes, nullptr, false);
      if (!value.is_object())
        stateFailure(StateFailure::Parse, "parse");
      return value.is_object() ? value : nlohmann::json::object();
    }
    nlohmann::json readState(const std::filesystem::path& path, const std::filesystem::path& legacy) {
      // The new file is authoritative, even when empty or damaged. Only fall
      // back before the first save; keep the previous installation's file intact.
      try {
        if (legacy.empty() || std::filesystem::exists(path))
          return read(path);
        return read(legacy);
      } catch (const std::filesystem::filesystem_error&) {
        stateFailure(StateFailure::Inspect, "inspect");
        throw;
      }
    }
    void writeState(const std::filesystem::path& path, const nlohmann::json& value) {
      const auto bytes = value.dump();
      try {
        std::filesystem::create_directories(path.parent_path());
      } catch (const std::filesystem::filesystem_error&) {
        stateFailure(StateFailure::Directory, "write-directory");
        throw;
      }
      std::string temporary = (path.parent_path() / ".session-XXXXXX").string();
      const int fd = mkstemp(temporary.data());
      if (fd < 0) {
        stateFailure(StateFailure::Temporary, "write-open");
        return;
      }
      if (fchmod(fd, 0600) != 0)
        stateFailure(StateFailure::Permissions, "write-permissions");
      std::size_t written = 0;
      while (written < bytes.size()) {
        const auto count = write(fd, bytes.data() + written, bytes.size() - written);
        if (count <= 0)
          break;
        written += count;
      }
      if (close(fd) != 0)
        stateFailure(StateFailure::Close, "write-close");
      if (written == bytes.size()) {
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (!error)
          return;
        stateFailure(StateFailure::Commit, "write-commit");
      } else
        stateFailure(StateFailure::Write, "write");
      unlink(temporary.c_str());
    }
  } // namespace
  namespace {
    constexpr std::size_t maximumExpandedFolders = 1000;
    bool validFolderId(const std::string& id) {
      std::uint64_t value = 0;
      const auto result = std::from_chars(id.data(), id.data() + id.size(), value);
      return result.ec == std::errc{}
      && result.ptr == id.data() + id.size()
          && value <= std::numeric_limits<std::int64_t>::max()
          && std::to_string(value) == id;
    }
  } // namespace
  std::optional<std::set<std::string>> SessionStore::expandedFolders(const std::string& account) const {
    try {
      const auto all = readState(m_path, m_legacyPath);
      if (!all.contains(account) || !all[account].is_object())
        return {};
      const auto at = all[account].find("expanded_folders");
      if (at == all[account].end() || !at->is_array() || at->size() > maximumExpandedFolders)
        return {};
      std::set<std::string> result;
      for (const auto& id : *at)
        if (id.is_string() && validFolderId(id.get<std::string>()))
          result.insert(id.get<std::string>());
      return result;
    } catch (...) {
      return {};
    }
  }
  void SessionStore::saveExpandedFolders(const std::string& account, const std::set<std::string>& folders) const {
    if (m_path.empty() || !api::validSnowflake(account) || folders.size() > maximumExpandedFolders)
      return;
    try {
      auto all = readState(m_path, m_legacyPath);
      auto ids = nlohmann::json::array();
      for (const auto& id : folders)
        if (validFolderId(id))
          ids.push_back(id);
      all[account]["expanded_folders"] = std::move(ids);
      writeState(m_path, all);
    } catch (...) {
    }
  }
  SessionStore::SessionStore(std::filesystem::path path) : m_path(std::move(path)) {
    if (!m_path.empty())
      return;
    if (const auto* state = std::getenv("XDG_STATE_HOME"); state && *state) {
      m_path = std::filesystem::path(state) / "cressida/session.json";
      m_legacyPath = std::filesystem::path(state) / "discord-cpp/session.json";
    } else if (const auto* home = std::getenv("HOME"); home && *home) {
      m_path = std::filesystem::path(home) / ".local/state/cressida/session.json";
      m_legacyPath = std::filesystem::path(home) / ".local/state/discord-cpp/session.json";
    }
  }
  std::optional<LastChannel> SessionStore::load(const std::string& account) const {
    try {
      const auto all = readState(m_path, m_legacyPath);
      if (!all.contains(account) || !all[account].is_object())
        return std::nullopt;
      const auto& value = all[account];
      LastChannel result{value.value("guild", ""), value.value("channel", ""), value.value("scale", 1.0F)};
      if ((!result.guild.empty() && !api::validSnowflake(result.guild)) || !api::validSnowflake(result.channel))
        return std::nullopt;
      result.scale = std::clamp(result.scale, 0.67F, 1.5F);
      return result;
    } catch (...) {
      return std::nullopt;
    }
  }
  void SessionStore::save(const std::string& account, const LastChannel& channel) const {
    if (m_path.empty() || !api::validSnowflake(account) || !api::validSnowflake(channel.channel))
      return;
    try {
      auto value = readState(m_path, m_legacyPath);
      value[account]["guild"] = channel.guild;
      value[account]["channel"] = channel.channel;
      value[account]["scale"] = channel.scale;
      value[account]["channels"][channel.guild.empty() ? "@me" : channel.guild] = channel.channel;
      writeState(m_path, value);
    } catch (...) { /* UI preference failures do not interrupt the account session. */
    }
  }
  std::string SessionStore::channelForGuild(const std::string& account, const std::string& guild) const {
    try {
      const auto all = readState(m_path, m_legacyPath);
      if (!all.contains(account) || !all[account].is_object())
        return {};
      const auto& value = all[account];
      const auto key = guild.empty() ? "@me" : guild;
      if (value.contains("channels") && value["channels"].is_object()) {
        auto channel = value["channels"].value(key, std::string{});
        if (api::validSnowflake(channel))
          return channel;
      }
      // Migrate the previous single-selection format without losing its last channel.
      if (value.value("guild", std::string{}) == guild) {
        auto channel = value.value("channel", std::string{});
        if (api::validSnowflake(channel))
          return channel;
      }
    } catch (...) {
    }
    return {};
  }
  Preferences SessionStore::preferences(const std::string& account) const {
    Preferences result;
    try {
      const auto all = readState(m_path, m_legacyPath);
      if (!all.contains(account) || !all[account].contains("preferences"))
        return result;
      const auto& value = all[account]["preferences"];
      result.theme = value.value("theme", "dark");
      if (!validColorMode(result.theme))
        result.theme = "dark";
      result.colorTheme = value.value("color_theme", "cressida");
      if (!validThemeId(result.colorTheme))
        result.colorTheme = "cressida";
      result.messageSize = std::clamp(value.value("message_size", 14.0F), 12.0F, 20.0F);
      result.fontFamily = value.value("font_family", std::string{});
      if (!validFontFamily(result.fontFamily))
        result.fontFamily.clear();
      result.compact = value.value("compact", false);
      result.showEmbeds = value.value("embeds", true);
      result.showImages = value.value("images", true);
      result.autoplayGifs = value.value("autoplay_gifs", true);
      result.emojiImageLinks = value.value("emoji_image_links", false);
      if (const auto at = value.find("randomize_media_filenames"); at != value.end() && at->is_boolean())
        result.randomizeMediaFilenames = at->get<bool>();
      if (const auto at = value.find("twelve_hour_clock"); at != value.end() && at->is_boolean())
        result.twelveHourClock = at->get<bool>();
      result.logDeletedMessages = value.value("log_deleted_messages", false);
      result.logEditHistory = value.value("log_edit_history", false);
      result.roleColors = value.value("role_colors", true);
      result.animateRoleColors = value.value("animate_role_colors", true);
      result.showGuildTags = value.value("guild_tags", true);
      result.showAvatarDecorations = !value.contains("avatar_decorations")
          || !value["avatar_decorations"].is_boolean()
          || value["avatar_decorations"].get<bool>();
      result.showNameplates =
          !value.contains("nameplates") || !value["nameplates"].is_boolean() || value["nameplates"].get<bool>();
      result.showProfileEffects = !value.contains("profile_effects")
          || !value["profile_effects"].is_boolean()
          || value["profile_effects"].get<bool>();
      result.showProfileFrames = !value.contains("profile_frames")
          || !value["profile_frames"].is_boolean()
          || value["profile_frames"].get<bool>();
      result.showProfileBanners = !value.contains("profile_banners")
          || !value["profile_banners"].is_boolean()
          || value["profile_banners"].get<bool>();
      result.showProfileThemes = !value.contains("profile_themes")
          || !value["profile_themes"].is_boolean()
          || value["profile_themes"].get<bool>();

      result.reducedMotion = value.value("reduced_motion", false);
      result.gifsWithReducedMotion = value.contains("gifs_with_reduced_motion")
          && value["gifs_with_reduced_motion"].is_boolean()
          && value["gifs_with_reduced_motion"].get<bool>();
      result.emojiWithReducedMotion = value.contains("emoji_with_reduced_motion")
          && value["emoji_with_reduced_motion"].is_boolean()
          && value["emoji_with_reduced_motion"].get<bool>();
      result.animateEmoji = value.value("animate_emoji", true);
      result.animateAvatars = value.value("animate_avatars", true);
      result.developerMode = value.value("developer_mode", false);
      result.convertEmoticons = value.value("convert_emoticons", true);
      result.showReactions = value.value("show_reactions", true);
      result.showRoleIcons = value.value("show_role_icons", true);
      result.desktopNotifications = value.value("desktop_notifications", true);
      result.desktopDecorations = value.value("desktop_decorations", true);
      if (const auto at = value.find("window_controls"); at != value.end() && at->is_object()) {
        auto& controls = result.windowControls;
        const auto flag = [&](const char* name, bool fallback) {
          const auto value = at->find(name);
          return value != at->end() && value->is_boolean() ? value->get<bool>() : fallback;
        };
        const auto number = [&](const char* name, int count) {
          const auto value = at->find(name);
          return value != at->end() && value->is_number_integer()
              ? static_cast<int>(std::clamp(value->get<std::int64_t>(), std::int64_t{0}, std::int64_t{count - 1}))
              : 0;
        };
        controls.minimize = flag("minimize", true);
        controls.maximize = flag("maximize", true);
        controls.close = flag("close", true);
        controls.left = flag("left", false);
        controls.hoverHighlight = flag("hover_highlight", true);
        if (const auto size = at->find("button_width"); size != at->end() && size->is_number_integer()) {
          const auto bounded = size->is_number_unsigned()
              ? static_cast<std::int64_t>(
                    std::min(size->get<std::uint64_t>(), static_cast<std::uint64_t>(Style::windowButtonMaximumWidth))
                )
              : size->get<std::int64_t>();
          controls.buttonWidth = static_cast<int>(std::clamp(
              bounded, static_cast<std::int64_t>(Style::windowButtonMinimumWidth),
              static_cast<std::int64_t>(Style::windowButtonMaximumWidth)
          ));
        }
        controls.order = number("order", windowButtonOrderCount);
        controls.style = number("style", windowIconStyleCount);
        if (const auto paths = at->find("icons"); paths != at->end() && paths->is_array())
          for (std::size_t i = 0; i < std::min(paths->size(), controls.icons.size()); ++i)
            if ((*paths)[i].is_string()) {
              const auto path = (*paths)[i].get<std::string>();
              if (validWindowIconPath(path))
                controls.icons[i] = path;
            }
      }
      result.notificationSounds = value.value("notification_sounds", true);
      result.saturation = std::clamp(value.value("saturation", 100), 0, 100);
      result.spoilerMode = std::clamp(value.value("spoiler_mode", 0), 0, 2);
      result.roleColorMode = std::clamp(value.value("role_color_mode", result.roleColors ? 0 : 2), 0, 2);
      result.messageGroupSpacing = std::clamp(value.value("message_group_spacing", 16), 0, 24);
      result.notificationPrivacy = std::clamp(value.value("notification_privacy", 0), 0, 2);
      result.cornerRadius = std::clamp(
          value.value("corner_radius", Style::cornerRadiusDefaultPercent), 0, Style::cornerRadiusMaximumPercent
      );
      result.roleColors = result.roleColorMode != 2;
      if (value.contains("keybinds") && value["keybinds"].is_object() && value["keybinds"].size() <= 20)
        for (const auto& [key, chord] : value["keybinds"].items())
          if (key.size() <= 32 && chord.is_string() && chord.get_ref<const std::string&>().size() <= 80)
            result.keybinds[key] = chord.get<std::string>();
      const float width = value.value("channel_sidebar_width", layout::Sidebar::preferredWidth);
      result.channelSidebarWidth = std::isfinite(width) ? std::clamp(width, 200.0F, 480.0F) : 248.0F;
    } catch (...) {
      return {};
    }
    return result;
  }
  Preferences SessionStore::startupPreferences() const {
    try {
      const auto all = readState(m_path, m_legacyPath);
      if (const auto at = all.find("appearance"); at != all.end())
        return colorPreferences(*at);
      // Existing installations have only account preferences. Reuse their color
      // choice when it is unambiguous; no account is selected by this fallback.
      std::optional<Preferences> previous;
      for (const auto& [id, account] : all.items()) {
        if (!api::validSnowflake(id)
            || !account.is_object()
            || !account.contains("preferences")
            || !account["preferences"].is_object())
          continue;
        const auto colors = colorPreferences(account["preferences"]);
        if (previous && (previous->theme != colors.theme || previous->colorTheme != colors.colorTheme))
          return {};
        previous = colors;
      }
      return previous.value_or(Preferences{});
    } catch (...) {
      return {};
    }
  }
  void SessionStore::saveAppearance(const Preferences& preferences) const {
    if (m_path.empty())
      return;
    try {
      auto all = readState(m_path, m_legacyPath);
      all["appearance"] = appearance(preferences);
      writeState(m_path, all);
    } catch (...) {
    }
  }
  void SessionStore::savePreferences(const std::string& account, const Preferences& preferences) const {
    if (m_path.empty() || !api::validSnowflake(account))
      return;
    try {
      auto all = readState(m_path, m_legacyPath);
      auto icons = preferences.windowControls.icons;
      for (auto& icon : icons)
        if (!validWindowIconPath(icon))
          icon.clear();
      all[account]["preferences"] = {
          {"theme", validColorMode(preferences.theme) ? preferences.theme : "dark"},
          {"color_theme", validThemeId(preferences.colorTheme) ? preferences.colorTheme : "cressida"},
          {"message_size", preferences.messageSize},
          {"font_family", validFontFamily(preferences.fontFamily) ? preferences.fontFamily : std::string{}},
          {"compact", preferences.compact},
          {"embeds", preferences.showEmbeds},
          {"images", preferences.showImages},
          {"autoplay_gifs", preferences.autoplayGifs},
          {"emoji_image_links", preferences.emojiImageLinks},
          {"randomize_media_filenames", preferences.randomizeMediaFilenames},
          {"twelve_hour_clock", preferences.twelveHourClock},
          {"log_deleted_messages", preferences.logDeletedMessages},
          {"log_edit_history", preferences.logEditHistory},
          {"role_colors", preferences.roleColors},
          {"animate_role_colors", preferences.animateRoleColors},
          {"guild_tags", preferences.showGuildTags},
          {"avatar_decorations", preferences.showAvatarDecorations},
          {"nameplates", preferences.showNameplates},
          {"profile_effects", preferences.showProfileEffects},
          {"profile_frames", preferences.showProfileFrames},
          {"profile_banners", preferences.showProfileBanners},
          {"profile_themes", preferences.showProfileThemes},

          {"reduced_motion", preferences.reducedMotion},
          {"gifs_with_reduced_motion", preferences.gifsWithReducedMotion},
          {"emoji_with_reduced_motion", preferences.emojiWithReducedMotion},
          {"animate_emoji", preferences.animateEmoji},
          {"animate_avatars", preferences.animateAvatars},
          {"developer_mode", preferences.developerMode},
          {"convert_emoticons", preferences.convertEmoticons},
          {"show_reactions", preferences.showReactions},
          {"show_role_icons", preferences.showRoleIcons},
          {"desktop_notifications", preferences.desktopNotifications},
          {"desktop_decorations", preferences.desktopDecorations},
          {"window_controls",
           {{"minimize", preferences.windowControls.minimize},
            {"maximize", preferences.windowControls.maximize},
            {"close", preferences.windowControls.close},
            {"left", preferences.windowControls.left},
            {"hover_highlight", preferences.windowControls.hoverHighlight},
            {"button_width",
             std::clamp(
                 preferences.windowControls.buttonWidth, static_cast<int>(Style::windowButtonMinimumWidth),
                 static_cast<int>(Style::windowButtonMaximumWidth)
             )},
            {"order", std::clamp(preferences.windowControls.order, 0, windowButtonOrderCount - 1)},
            {"style", std::clamp(preferences.windowControls.style, 0, windowIconStyleCount - 1)},
            {"icons", icons}}},
          {"corner_radius", std::clamp(preferences.cornerRadius, 0, Style::cornerRadiusMaximumPercent)},
          {"notification_sounds", preferences.notificationSounds},
          {"saturation", preferences.saturation},
          {"spoiler_mode", preferences.spoilerMode},
          {"role_color_mode", preferences.roleColors ? std::clamp(preferences.roleColorMode, 0, 1) : 2},
          {"message_group_spacing", preferences.messageGroupSpacing},
          {"notification_privacy", preferences.notificationPrivacy},
          {"keybinds", preferences.keybinds},
          {"channel_sidebar_width",
           std::isfinite(preferences.channelSidebarWidth) ? std::clamp(preferences.channelSidebarWidth, 200.0F, 480.0F)
                                                          : 248.0F}
      };
      all["appearance"] = appearance(preferences);
      writeState(m_path, all);
    } catch (...) {
    }
  }
  ChannelListPreferences SessionStore::channelList(const std::string& account, const std::string& guild) const {
    ChannelListPreferences result;
    try {
      const auto all = readState(m_path, m_legacyPath);
      if (!all.contains(account)
          || !all[account].contains("channel_lists")
          || !all[account]["channel_lists"].contains(guild))
        return result;
      const auto& value = all[account]["channel_lists"][guild];
      result.hideMuted = value.value("hide_muted", false);
      if (value.contains("pinned") && value["pinned"].is_array())
        for (const auto& item : value["pinned"])
          if (item.is_string()
              && api::validSnowflake(item.get<std::string>())
              && result.pinned.size() < 200
              && std::ranges::find(result.pinned, item.get<std::string>()) == result.pinned.end())
            result.pinned.push_back(item.get<std::string>());
    } catch (...) {
      return {};
    }
    return result;
  }
  void SessionStore::saveChannelList(
      const std::string& account, const std::string& guild, const ChannelListPreferences& preferences
  ) const {
    if (m_path.empty() || !api::validSnowflake(account) || !api::validSnowflake(guild))
      return;
    try {
      auto all = readState(m_path, m_legacyPath);
      std::vector<std::string> pinned;
      for (const auto& id : preferences.pinned)
        if (api::validSnowflake(id) && pinned.size() < 200 && std::ranges::find(pinned, id) == pinned.end())
          pinned.push_back(id);
      all[account]["channel_lists"][guild] = {{"hide_muted", preferences.hideMuted}, {"pinned", pinned}};
      writeState(m_path, all);
    } catch (...) {
    }
  }
  std::vector<std::string> SessionStore::frequentEmojis(const std::string& account) const {
    std::vector<std::pair<int, std::string>> ranked;
    try {
      const auto all = readState(m_path, m_legacyPath);
      if (all.contains(account) && all[account].contains("emoji_usage") && all[account]["emoji_usage"].is_object())
        for (const auto& [key, count] : all[account]["emoji_usage"].items()) {
          if (ranked.size() >= 64)
            break;
          if (api::validEmojiKey(key) && count.is_number_integer() && count > 0 && count <= 1000000)
            ranked.emplace_back(count.get<int>(), key);
        }
    } catch (...) {
    }
    std::ranges::sort(ranked, [](const auto& a, const auto& b) {
      return a.first > b.first || (a.first == b.first && a.second < b.second);
    });
    std::vector<std::string> result;
    for (const auto& [_, key] : ranked)
      result.push_back(key);
    return result;
  }
  void SessionStore::recordEmoji(const std::string& account, const std::string& key) const {
    if (m_path.empty() || !api::validSnowflake(account) || !api::validEmojiKey(key))
      return;
    try {
      auto all = readState(m_path, m_legacyPath);
      auto& usage = all[account]["emoji_usage"];
      if (!usage.is_object())
        usage = nlohmann::json::object();
      if (!usage.contains(key) && usage.size() >= 64) {
        auto lowest = usage.begin();
        for (auto it = usage.begin(); it != usage.end(); ++it)
          if (!it.value().is_number_integer() || (lowest.value().is_number_integer() && it.value() < lowest.value()))
            lowest = it;
        usage.erase(lowest);
      }
      const auto previous =
          usage.contains(key) && usage[key].is_number_integer() ? std::clamp(usage[key].get<int>(), 0, 999999) : 0;
      usage[key] = previous + 1;
      writeState(m_path, all);
    } catch (...) {
    }
  }
  std::vector<std::string> SessionStore::frequentStickers(const std::string& account) const {
    std::vector<std::pair<int, std::string>> ranked;
    try {
      const auto all = readState(m_path, m_legacyPath);
      if (all.contains(account) && all[account].contains("sticker_usage") && all[account]["sticker_usage"].is_object())
        for (const auto& [key, count] : all[account]["sticker_usage"].items()) {
          if (ranked.size() >= 64)
            break;
          if (api::validSnowflake(key) && count.is_number_integer() && count > 0 && count <= 1000000)
            ranked.emplace_back(count.get<int>(), key);
        }
    } catch (...) {
    }
    std::ranges::sort(ranked, [](const auto& a, const auto& b) {
      return a.first > b.first || (a.first == b.first && a.second < b.second);
    });
    std::vector<std::string> result;
    for (const auto& [_, key] : ranked)
      result.push_back(key);
    return result;
  }
  void SessionStore::recordSticker(const std::string& account, const std::string& key) const {
    if (m_path.empty() || !api::validSnowflake(account) || !api::validSnowflake(key))
      return;
    try {
      auto all = readState(m_path, m_legacyPath);
      auto& usage = all[account]["sticker_usage"];
      if (!usage.is_object())
        usage = nlohmann::json::object();
      if (!usage.contains(key) && usage.size() >= 64) {
        auto lowest = usage.begin();
        for (auto it = usage.begin(); it != usage.end(); ++it)
          if (!it.value().is_number_integer() || (lowest.value().is_number_integer() && it.value() < lowest.value()))
            lowest = it;
        usage.erase(lowest);
      }
      const auto previous =
          usage.contains(key) && usage[key].is_number_integer() ? std::clamp(usage[key].get<int>(), 0, 999999) : 0;
      usage[key] = previous + 1;
      writeState(m_path, all);
    } catch (...) {
    }
  }
  std::vector<std::string> SessionStore::frequentReactions(const std::string& account) const {
    std::vector<std::pair<int, std::string>> ranked;
    try {
      const auto all = readState(m_path, m_legacyPath);
      if (all.contains(account) && all[account].contains("reaction_usage")) {
        const auto& usage = all[account]["reaction_usage"];
        if (usage.is_object())
          for (const auto& [emoji, count] : usage.items()) {
            if (ranked.size() == 64)
              break;
            if (count.is_number_integer()
                && count > 0
                && count <= 1000000
                && api::interactionRequest({.channel = "1", .message = "1", .emoji = emoji}))
              ranked.emplace_back(count.get<int>(), emoji);
          }
      }
    } catch (...) {
    }
    std::ranges::sort(ranked, [](const auto& a, const auto& b) {
      return a.first > b.first || (a.first == b.first && a.second < b.second);
    });
    std::vector<std::string> result;
    for (const auto& [_, emoji] : ranked)
      result.push_back(emoji);
    for (const auto* emoji : {"❤️", "👍", "😂", "🔥"})
      if (std::ranges::find(result, emoji) == result.end())
        result.emplace_back(emoji);
    return result;
  }
  void SessionStore::recordReaction(const std::string& account, const std::string& emoji) const {
    if (m_path.empty()
        || !api::validSnowflake(account)
        || !api::interactionRequest({.channel = "1", .message = "1", .emoji = emoji}))
      return;
    try {
      auto all = readState(m_path, m_legacyPath);
      auto& usage = all[account]["reaction_usage"];
      if (!usage.is_object())
        usage = nlohmann::json::object();
      if (!usage.contains(emoji) && usage.size() >= 64) {
        // Retain popular entries when a new reaction joins the bounded history.
        auto lowest = usage.begin();
        for (auto it = usage.begin(); it != usage.end(); ++it)
          if (!it.value().is_number_integer() || (lowest.value().is_number_integer() && it.value() < lowest.value()))
            lowest = it;
        usage.erase(lowest);
      }
      const auto previous = usage.contains(emoji) && usage[emoji].is_number_integer()
          ? std::clamp(usage[emoji].get<int>(), 0, 999999)
          : 0;
      usage[emoji] = previous + 1;
      writeState(m_path, all);
    } catch (...) {
    }
  }
} // namespace app
