#include "app/message_preferences.h"
#include "app/session_store.h"
#include "core/environment.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  struct EnvironmentOverride {
    std::string name;
    std::optional<std::string> previous;
    EnvironmentOverride(const char* key, const char* value) : name(key) {
      if (const auto* old = std::getenv(key))
        previous = old;
      setenv(key, value, 1);
    }
    ~EnvironmentOverride() {
      if (previous)
        setenv(name.c_str(), previous->c_str(), 1);
      else
        unsetenv(name.c_str());
    }
  };
} // namespace

int main() {
  char directory[] = "/tmp/cressida-state-XXXXXX";
  if (!mkdtemp(directory))
    return 1;
  const auto path = std::filesystem::path(directory) / "session.json";
  auto require = [](bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  };
  try {
    {
      app::SessionStore store(std::filesystem::path(directory) / "cosmetics.json");
      app::Preferences prefs;
      bool app::Preferences::* fields[] = {
          &app::Preferences::showAvatarDecorations, &app::Preferences::showNameplates,
          &app::Preferences::showProfileEffects,    &app::Preferences::showProfileFrames,
          &app::Preferences::showProfileBanners,    &app::Preferences::showProfileThemes
      };
      for (const auto field : fields)
        require(prefs.*field, "Cosmetic display defaults on");
      for (const auto field : fields) {
        prefs.*field = false;
        store.savePreferences("123", prefs);
        const auto loaded = store.preferences("123");
        for (const auto check : fields)
          require(
              loaded.*check == prefs.*check && store.preferences("456").*check,
              "Individual cosmetic choices persist without changing another account"
          );
      }
    }
    {
      const auto mediaPath = std::filesystem::path(directory) / "media-preferences.json";
      app::SessionStore store(mediaPath);
      require(!store.preferences("123").randomizeMediaFilenames, "Filename randomization defaults off");
      app::Preferences prefs;
      prefs.randomizeMediaFilenames = true;
      store.savePreferences("123", prefs);
      app::SessionStore restarted(mediaPath);
      require(
          restarted.preferences("123").randomizeMediaFilenames
              && !restarted.preferences("456").randomizeMediaFilenames
              && !restarted.startupPreferences().randomizeMediaFilenames,
          "Random media names survive restart and remain isolated to the account"
      );
      prefs.randomizeMediaFilenames = false;
      store.savePreferences("123", prefs);
      require(!restarted.preferences("123").randomizeMediaFilenames, "Random media names can be disabled again");
      std::ofstream(mediaPath) << R"({"123":{"preferences":{"theme":"light","randomize_media_filenames":"yes"}}})";
      require(
          !store.preferences("123").randomizeMediaFilenames && store.preferences("123").theme == "light",
          "Malformed filename preferences retain safe defaults without discarding other settings"
      );
    }
    {
      const auto foldersPath = std::filesystem::path(directory) / "folders.json";
      app::SessionStore store(foldersPath);
      require(!store.expandedFolders("123"), "Missing expansion state retains legacy startup behavior");
      store.savePreferences("123", {.theme = "light"});
      store.saveExpandedFolders("123", {"0", "77", "../bad"});
      store.saveExpandedFolders("456", {"88"});
      app::SessionStore restarted(foldersPath);
      require(
          restarted.expandedFolders("123") == std::set<std::string>{"0", "77"}
              && restarted.expandedFolders("456") == std::set<std::string>{"88"}
              && !restarted.expandedFolders("789"),
          "Expanded folders survive restart, validate IDs and stay account-scoped"
      );
      store.saveExpandedFolders("123", {});
      require(
          restarted.expandedFolders("123")
              && restarted.expandedFolders("123")->empty()
              && restarted.preferences("123").theme == "light",
          "Closing all folders is explicit persisted state and preserves account preferences"
      );
      std::ofstream(foldersPath) << R"({"123":{"expanded_folders":["77",false,"bad", "0"]}})";
      require(
          restarted.expandedFolders("123") == std::set<std::string>{"0", "77"},
          "Malformed persisted folder IDs cannot enter expansion state"
      );
    }
    {
      app::SessionStore store(std::filesystem::path(directory) / "channel-lists.json");
      store.savePreferences("123", {.theme = "light", .colorTheme = "fen"});
      store.saveChannelList("123", "10", {true, {"20", "20", "bad", "21"}});
      const auto value = store.channelList("123", "10");
      require(
          value.hideMuted && value.pinned == std::vector<std::string>{"20", "21"},
          "Pinned channels and hide-muted persist with bounded valid unique IDs"
      );
      require(
          !store.channelList("123", "11").hideMuted && store.channelList("456", "10").pinned.empty(),
          "Channel-list preferences stay scoped to account and server"
      );
      require(
          store.preferences("123").colorTheme == "fen" && store.startupPreferences().theme == "light",
          "Sidebar saves preserve account and startup appearance"
      );
    }
    {
      const auto appearancePath = std::filesystem::path(directory) / "appearance.json";
      app::SessionStore store(appearancePath);
      require(store.startupPreferences().colorTheme == "cressida", "Fresh startup uses the default palette");
      app::Preferences selected;
      selected.theme = "light";
      selected.colorTheme = "fen";
      selected.logDeletedMessages = selected.logEditHistory = true;
      selected.fontFamily = "Custom Font";
      store.savePreferences("123", selected);
      store.save("123", {"10", "20", 1});
      const auto startup = app::SessionStore(appearancePath).startupPreferences();
      require(
          startup.theme == "light" && startup.colorTheme == "fen",
          "The last chosen colors survive restart before sign-in"
      );
      require(
          !startup.logDeletedMessages && !startup.logEditHistory && startup.fontFamily.empty(),
          "Startup never inherits account-specific settings"
      );
      app::Preferences other;
      other.theme = "system";
      other.colorTheme = "catppuccin";
      store.savePreferences("456", other);
      require(
          store.startupPreferences().theme == "system" && store.startupPreferences().colorTheme == "catppuccin",
          "The most recent account's selection drives startup"
      );
      store.saveAppearance(selected);
      require(
          store.startupPreferences().colorTheme == "fen" && store.preferences("456").colorTheme == "catppuccin",
          "Signing in remembers that account's appearance without modifying other accounts"
      );
      std::ifstream input(appearancePath);
      auto json = nlohmann::json::parse(input);
      require(
          json["appearance"].size() == 2
              && json["appearance"].contains("theme")
              && json["appearance"].contains("color_theme"),
          "Only the mode and palette are saved for signed-out screens"
      );
      json.erase("appearance");
      json.erase("456");
      std::ofstream(appearancePath) << json;
      require(
          store.startupPreferences().colorTheme == "fen",
          "Existing single-account installations reuse their theme before sign-in"
      );
      json["appearance"] = {{"theme", false}, {"color_theme", "../unsafe"}};
      std::ofstream(appearancePath) << json;
      require(
          store.startupPreferences().theme == "dark" && store.startupPreferences().colorTheme == "cressida",
          "Malformed startup colors safely fall back without restoring account data"
      );
      std::ofstream(appearancePath) << "damaged";
      require(store.startupPreferences().colorTheme == "cressida", "Damaged startup state falls back safely");
    }
    {
      app::SessionStore store(std::filesystem::path(directory) / "client-preferences.json");
      app::Preferences selected;
      selected.reducedMotion = true;
      selected.gifsWithReducedMotion = true;
      selected.emojiWithReducedMotion = true;
      selected.saturation = 45;
      selected.roleColorMode = 1;
      selected.spoilerMode = 2;
      selected.desktopNotifications = false;
      selected.desktopDecorations = false;
      selected.windowControls = {
          .minimize = false,
          .maximize = true,
          .close = false,
          .left = true,
          .hoverHighlight = false,
          .order = 2,
          .style = 3,
          .buttonWidth = static_cast<int>(Style::windowButtonMinimumWidth),
          .icons = {"/tmp/minimize.svg", "", "/tmp/restore.png", ""}
      };
      selected.cornerRadius = 0;
      selected.notificationPrivacy = 2;
      selected.keybinds = {{"settings", "Ctrl+Alt+s"}, {"quit", ""}};
      store.savePreferences("123", selected);
      const auto actual = store.preferences("123");
      require(
          actual.reducedMotion
              && actual.gifsWithReducedMotion
              && actual.emojiWithReducedMotion
              && actual.saturation == 45
              && actual.roleColorMode == 1
              && actual.spoilerMode == 2
              && !actual.desktopDecorations
              && actual.windowControls == selected.windowControls
              && actual.cornerRadius == 0
              && !actual.desktopNotifications
              && actual.notificationPrivacy == 2
              && actual.keybinds == selected.keybinds,
          "Client behavior and shortcuts persist per account"
      );
      require(
          !store.preferences("456").reducedMotion
              && !store.preferences("456").gifsWithReducedMotion
              && !store.preferences("456").emojiWithReducedMotion
              && !store.startupPreferences().gifsWithReducedMotion
              && !store.startupPreferences().emojiWithReducedMotion
              && store.preferences("456").desktopDecorations
              && store.startupPreferences().desktopDecorations
              && store.startupPreferences().saturation == 100,
          "Accessibility and notification choices never leak to other accounts or sign-in"
      );
      require(
          store.preferences("456").cornerRadius == Style::cornerRadiusDefaultPercent
              && store.startupPreferences().cornerRadius == Style::cornerRadiusDefaultPercent,
          "Corner choices stay per account and never leak into sign-in defaults"
      );
      require(
          store.preferences("456").windowControls == app::WindowControlPreferences{}
              && store.startupPreferences().windowControls == app::WindowControlPreferences{},
          "Window button choices and local paths do not leak to other accounts or sign-in"
      );
      app::SessionStore restarted(std::filesystem::path(directory) / "client-preferences.json");
      require(
          restarted.preferences("123").windowControls == selected.windowControls,
          "Custom titlebar choices and all icon slots survive restart"
      );
      require(
          !restarted.preferences("123").desktopDecorations && restarted.preferences("123").cornerRadius == 0,
          "The desktop-decoration choice survives a new application session"
      );
      require(
          restarted.preferences("123").gifsWithReducedMotion && restarted.preferences("123").emojiWithReducedMotion,
          "Media overrides survive application restart"
      );
      auto separateMotion = selected;
      separateMotion.gifsWithReducedMotion = false;
      restarted.savePreferences("123", separateMotion);
      require(
          !restarted.preferences("123").gifsWithReducedMotion && restarted.preferences("123").emojiWithReducedMotion,
          "GIF and emoji overrides persist independently"
      );
      const auto motionPath = std::filesystem::path(directory) / "malformed-motion.json";
      std::ofstream(motionPath) << nlohmann::json{
          {"123",
           {{"preferences",
             {{"reduced_motion", true},
              {"saturation", 45},
              {"gifs_with_reduced_motion", "true"},
              {"emoji_with_reduced_motion", 1}}}}}
      };
      const auto malformedMotion = app::SessionStore(motionPath).preferences("123");
      require(
          malformedMotion.reducedMotion
              && malformedMotion.saturation == 45
              && !malformedMotion.gifsWithReducedMotion
              && !malformedMotion.emojiWithReducedMotion,
          "Malformed overrides stay off without discarding other accessibility choices"
      );
      std::ofstream(motionPath) << nlohmann::json{{"123", {{"preferences", {{"reduced_motion", true}}}}}};
      const auto legacyMotion = app::SessionStore(motionPath).preferences("123");
      require(
          legacyMotion.reducedMotion && !legacyMotion.gifsWithReducedMotion && !legacyMotion.emojiWithReducedMotion,
          "Existing reduced-motion settings retain still media until an explicit opt-in"
      );
      restarted.savePreferences("123", {});
      require(
          store.preferences("123").cornerRadius == Style::cornerRadiusDefaultPercent,
          "Default corner rounding can replace a saved square preference"
      );
      auto bounded = app::Preferences{};
      bounded.cornerRadius = Style::cornerRadiusMaximumPercent + 1;
      restarted.savePreferences("456", bounded);
      require(
          restarted.preferences("456").cornerRadius == Style::cornerRadiusMaximumPercent,
          "Persisted corner values remain within the supported range"
      );
      require(
          store.preferences("123").desktopDecorations,
          "Re-enabling desktop decorations persists over a previously disabled choice"
      );
      bounded.windowControls.order = app::windowButtonOrderCount;
      bounded.windowControls.style = app::windowIconStyleCount;
      bounded.windowControls.buttonWidth = static_cast<int>(Style::windowButtonMaximumWidth) + 1;
      bounded.windowControls.icons = {
          "relative.svg", "https://example.com/icon.png", std::string("/tmp/a\0.svg", 11), ""
      };
      restarted.savePreferences("456", bounded);
      const auto sanitized = restarted.preferences("456").windowControls;
      require(
          sanitized.order == app::windowButtonOrderCount - 1
              && sanitized.style == app::windowIconStyleCount - 1
              && sanitized.buttonWidth == Style::windowButtonMaximumWidth
              && std::ranges::all_of(sanitized.icons, [](const auto& path) { return path.empty(); }),
          "Persisted titlebar indices and custom paths are bounded and local"
      );
      const auto malformedPath = std::filesystem::path(directory) / "malformed-window-controls.json";
      std::ofstream(malformedPath) << nlohmann::json{
          {"123",
           {{"preferences",
             {{"theme", "light"},
              {"window_controls",
               {{"minimize", "invalid"},
                {"maximize", false},
                {"order", "invalid"},
                {"style", 2},
                {"hover_highlight", "invalid"},
                {"button_width", "invalid"},
                {"icons", {"/tmp/icon.svg", 12, nullptr, "relative.png"}}}}}}}}
      };
      const auto malformed = app::SessionStore(malformedPath).preferences("123");
      require(
          malformed.theme == "light"
              && malformed.windowControls.minimize
              && !malformed.windowControls.maximize
              && malformed.windowControls.order == 0
              && malformed.windowControls.style == 2
              && malformed.windowControls.hoverHighlight
              && malformed.windowControls.buttonWidth == Style::windowButtonWidth
              && malformed.windowControls.icons[0] == "/tmp/icon.svg"
              && malformed.windowControls.icons[3].empty(),
          "Malformed titlebar fields cannot reset unrelated account preferences"
      );
      bounded.windowControls.buttonWidth = 0;
      restarted.savePreferences("456", bounded);
      require(
          restarted.preferences("456").windowControls.buttonWidth == Style::windowButtonMinimumWidth,
          "Persisted spacing cannot create undersized or overlapping native button cells"
      );
      const auto legacyPath = std::filesystem::path(directory) / "legacy-titlebar.json";
      std::ofstream(legacyPath) << nlohmann::json{{"123", {{"preferences", {{"window_controls", {{"left", true}}}}}}}};
      const auto legacy = app::SessionStore(legacyPath).preferences("123").windowControls;
      require(
          legacy.left && legacy.hoverHighlight && legacy.buttonWidth == Style::windowButtonWidth,
          "Existing titlebar settings retain their appearance when new fields are absent"
      );
      require(
          app::notificationMessagePreview("hello ||private\nmessage|| and `a || b` \\||literal")
              == "hello [Spoiler] and `a || b` \\||literal",
          "Desktop previews hide spoilers without reinterpreting code or escapes"
      );
      require(
          app::notificationMessagePreview("unclosed ` code ||private||") == "unclosed ` code [Spoiler]",
          "Malformed inline code cannot disclose marked spoiler text in desktop previews"
      );
      require(
          app::convertMessageEmoticons("hi :) <3 `:)` https://example.com/:) \\:) :D")
              == "hi 🙂 ❤️ `:)` https://example.com/:) \\:) 😃",
          "Emoticons skip code, links and escaped text"
      );
      require(
          app::convertMessageEmoticons("```\n:)\n```\n:)") == "```\n:)\n```\n🙂",
          "Fenced code stays unchanged while later emoticons convert"
      );
    }
    {
      app::SessionStore store(path);
      require(!store.load("123"), "First login must not invent a previous server");
      store.save("123", {"10", "20", 1.2F});
      store.save("123", {"11", "21", 1.2F});
      store.save("456", {"", "30", 0.8F});
      store.savePreferences("123", {"system", 18, true, false, true});
      store.save("123", {"10", "20", 1.2F});
      store.savePreferences("789", {"light", 16});
    }
    app::SessionStore restored(path);
    require(
        restored.frequentReactions("123") == std::vector<std::string>{"❤️", "👍", "😂", "🔥"},
        "New accounts get familiar quick reaction defaults"
    );
    restored.recordReaction("123", "🎉");
    restored.recordEmoji("123", "heart");
    restored.recordEmoji("123", "123456789012345678");
    restored.recordEmoji("123", "heart");
    restored.recordSticker("123", "101");
    restored.recordSticker("123", "102");
    restored.recordSticker("123", "102");
    restored.recordSticker("123", "../bad");
    require(
        app::SessionStore(path).frequentStickers("123") == std::vector<std::string>{"102", "101"}
            && app::SessionStore(path).frequentStickers("456").empty(),
        "Sticker frequency survives restart, stays account-scoped and ignores malformed IDs"
    );
    require(
        app::SessionStore(path).frequentEmojis("123").front() == "heart"
            && app::SessionStore(path).frequentEmojis("456").empty(),
        "Emoji usage survives restart and remains account-scoped"
    );
    restored.recordReaction("123", "🎉");
    restored.recordReaction("123", "wave:123");
    restored.recordReaction("123", "../invalid");
    require(restored.frequentReactions("123").front() == "🎉", "Quick reactions rank successful local usage");
    require(restored.frequentReactions("456").front() == "❤️", "Reaction history stays account-scoped");
    restored.savePreferences("123", {"system", 18, true, false, true});
    restored.save("123", {"10", "20", 1.2F});
    require(
        app::SessionStore(path).frequentReactions("123").front() == "🎉",
        "Channel and preference saves preserve reaction usage across restarts"
    );
    const auto first = restored.load("123"), second = restored.load("456");
    require(
        first && first->guild == "10" && first->channel == "20" && std::abs(first->scale - 1.2F) < 0.01F,
        "Restore selected server/channel and zoom after restarting"
    );
    require(second && second->guild.empty() && second->channel == "30", "Remember DMs independently for each account");
    const auto preferences = restored.preferences("123");
    require(preferences.autoplayGifs, "GIF autoplay defaults on for existing accounts");
    auto pausedGifs = preferences;
    require(preferences.fontFamily.empty(), "Existing accounts use the system font by default");
    pausedGifs.fontFamily = "École Mono";
    pausedGifs.colorTheme = "catppuccin";
    require(!preferences.emojiImageLinks, "Emoji image links default off for existing accounts");
    require(
        !preferences.logDeletedMessages && !preferences.logEditHistory,
        "Both logger options default off for existing accounts"
    );
    pausedGifs.logDeletedMessages = true;
    pausedGifs.emojiImageLinks = true;
    pausedGifs.autoplayGifs = false;
    require(preferences.roleColors && preferences.animateRoleColors, "Role colors and hover animation default on");
    pausedGifs.roleColors = false;
    pausedGifs.animateRoleColors = false;
    require(preferences.showGuildTags, "Chat server tags default on for existing accounts");
    pausedGifs.showGuildTags = false;
    pausedGifs.channelSidebarWidth = 372;
    restored.savePreferences("123", pausedGifs);
    restored.save("123", {"10", "20", 1.2F});
    require(
        app::SessionStore(path).preferences("123").fontFamily == "École Mono"
            && app::SessionStore(path).preferences("456").fontFamily.empty(),
        "Installed font preference survives navigation saves and remains account-scoped"
    );
    require(
        app::SessionStore(path).preferences("123").colorTheme == "catppuccin"
            && app::SessionStore(path).preferences("123").theme == "system"
            && app::SessionStore(path).preferences("456").colorTheme == "cressida",
        "Color theme and system mode persist independently for each account"
    );
    require(
        app::SessionStore(path).preferences("123").channelSidebarWidth == 372
            && app::SessionStore(path).preferences("456").channelSidebarWidth == 248,
        "Sidebar width survives navigation saves and stays account-scoped"
    );
    require(
        app::SessionStore(path).preferences("123").logDeletedMessages
            && !app::SessionStore(path).preferences("123").logEditHistory
            && !app::SessionStore(path).preferences("456").logDeletedMessages
            && !app::SessionStore(path).preferences("456").logEditHistory,
        "Deletion-only logging persists independently for each account"
    );
    auto historyOnly = pausedGifs;
    historyOnly.logDeletedMessages = false;
    historyOnly.logEditHistory = true;
    restored.savePreferences("123", historyOnly);
    require(
        !app::SessionStore(path).preferences("123").logDeletedMessages
            && app::SessionStore(path).preferences("123").logEditHistory,
        "History-only logging persists independently of deletion logging"
    );
    pausedGifs.logEditHistory = true;
    restored.savePreferences("123", pausedGifs);
    require(
        app::SessionStore(path).preferences("123").logDeletedMessages
            && app::SessionStore(path).preferences("123").logEditHistory,
        "Both logger options can be enabled together"
    );
    auto invalidWidth = pausedGifs;
    invalidWidth.channelSidebarWidth = 900;
    restored.savePreferences("789", invalidWidth);
    require(app::SessionStore(path).preferences("789").channelSidebarWidth == 480, "Sidebar width is bounded on disk");
    restored.savePreferences("789", {"light", 16});
    require(
        !app::SessionStore(path).preferences("123").showGuildTags
            && app::SessionStore(path).preferences("456").showGuildTags,
        "Chat tag visibility persists independently for each account"
    );
    require(
        app::SessionStore(path).preferences("123").emojiImageLinks
            && !app::SessionStore(path).preferences("456").emojiImageLinks,
        "Emoji image links persist only for the account that enabled them"
    );
    require(
        !app::SessionStore(path).preferences("123").autoplayGifs
            && app::SessionStore(path).preferences("456").autoplayGifs,
        "GIF autoplay persists per account"
    );
    require(
        !app::SessionStore(path).preferences("123").roleColors
            && !app::SessionStore(path).preferences("123").animateRoleColors
            && app::SessionStore(path).preferences("456").roleColors
            && app::SessionStore(path).preferences("456").animateRoleColors,
        "Role colors and animation preferences persist independently per account"
    );
    require(
        preferences.theme == "system"
            && preferences.messageSize == 18
            && preferences.compact
            && !preferences.showEmbeds
            && preferences.showImages,
        "Selection saves preserve account appearance preferences"
    );
    require(
        restored.preferences("456").theme == "dark" && restored.preferences("789").theme == "light",
        "Store preferences independently, including accounts without a previous channel"
    );
    require(
        restored.channelForGuild("123", "10") == "20" && restored.channelForGuild("123", "11") == "21",
        "Keep the last channel independently for every server"
    );
    require(
        restored.channelForGuild("456", "") == "30" && restored.channelForGuild("123", "99").empty(),
        "DM history and account histories do not leak across servers"
    );
    struct stat state{};
    for (int i = 1; i <= 75; ++i)
      restored.recordReaction("123", "sample:" + std::to_string(i));
    require(
        restored.frequentReactions("123").size() <= 68, "Reaction usage is bounded to 64 records plus missing defaults"
    );
    require(
        restored.load("456")->channel == "30" && restored.preferences("123").theme == "system",
        "Pruning emoji history preserves other account data"
    );
    require(
        stat(path.c_str(), &state) == 0 && (state.st_mode & 0777) == 0600,
        "Private UI state must be owner-readable only"
    );
    auto invalidFont = pausedGifs;
    invalidFont.fontFamily = "Bad\nFamily";
    restored.savePreferences("789", invalidFont);
    require(
        restored.preferences("789").fontFamily.empty() && restored.preferences("789").theme == "system",
        "Invalid font names fall back without dropping other preferences"
    );
    invalidFont.fontFamily.assign(513, 'a');
    restored.savePreferences("789", invalidFont);
    require(restored.preferences("789").fontFamily.empty(), "Oversized font names fall back to the system default");
    pausedGifs.fontFamily.clear();
    restored.savePreferences("123", pausedGifs);
    require(restored.preferences("123").fontFamily.empty(), "System default font selection persists");
    auto badTheme = pausedGifs;
    badTheme.theme = "unsupported-mode";
    badTheme.colorTheme = "../outside";
    restored.savePreferences("789", badTheme);
    require(
        restored.preferences("789").theme == "dark" && restored.preferences("789").colorTheme == "cressida",
        "Unknown modes and unsafe theme IDs use defaults without aliases"
    );
    pausedGifs.colorTheme = "my-uninstalled-theme";
    restored.savePreferences("123", pausedGifs);
    require(
        restored.preferences("123").colorTheme == "my-uninstalled-theme",
        "Unavailable themes keep their preference so restoring the file restores the choice"
    );
    std::ofstream(path) << "broken JSON";
    require(!restored.load("123"), "Ignore damaged UI preferences");
    {
      const auto root = std::filesystem::path(directory) / "migration";
      EnvironmentOverride stateHome("XDG_STATE_HOME", root.c_str());
      const auto oldPath = root / "discord-cpp/session.json";
      const auto newPath = root / "cressida/session.json";
      app::SessionStore legacy(oldPath);
      legacy.save("123", {"10", "20", 1.2F});
      legacy.save("123", {"11", "21", 1.2F});
      legacy.savePreferences("123", {"system", 18, true, false, true});
      legacy.save("456", {"", "30", 0.8F});
      app::SessionStore migrated;
      require(migrated.load("123")->channel == "21", "Read the old name's last channel before migration");
      require(migrated.preferences("123").theme == "system", "Read old appearance preferences");
      require(!std::filesystem::exists(newPath), "Reading old state must not create new files");
      migrated.save("123", {"10", "22", 1.1F});
      require(std::filesystem::exists(newPath), "Saving migrates to the Cressida directory");
      require(legacy.load("123")->channel == "21", "Migration leaves the original file intact");
      require(migrated.channelForGuild("123", "11") == "21", "Migration preserves other servers");
      require(migrated.load("456")->channel == "30", "Migration preserves other accounts");
      require(migrated.preferences("123").theme == "system", "Migration preserves display preferences");
      require(stat(newPath.c_str(), &state) == 0 && (state.st_mode & 0777) == 0600, "Migrated state remains private");
      legacy.save("456", {"", "31", 0.8F});
      require(migrated.load("456")->channel == "30", "The new state takes precedence over legacy changes");
      std::ofstream(newPath) << "broken JSON";
      require(!migrated.load("123"), "Damaged new state must not resurrect legacy selections");
    }
    {
      EnvironmentOverride oldName("DISCORD_CPP_TEST_OVERRIDE", "old");
      EnvironmentOverride newName("CRESSIDA_TEST_OVERRIDE", "new");
      require(
          std::string(cressida::environment("CRESSIDA_TEST_OVERRIDE", "DISCORD_CPP_TEST_OVERRIDE")) == "new",
          "Cressida overrides take precedence"
      );
      unsetenv("CRESSIDA_TEST_OVERRIDE");
      require(
          std::string(cressida::environment("CRESSIDA_TEST_OVERRIDE", "DISCORD_CPP_TEST_OVERRIDE")) == "old",
          "Legacy overrides remain supported"
      );
      setenv("CRESSIDA_TEST_OVERRIDE", "", 1);
      require(
          *cressida::environment("CRESSIDA_TEST_OVERRIDE", "DISCORD_CPP_TEST_OVERRIDE") == '\0',
          "An empty override must not resurrect the legacy value"
      );
    }
    std::filesystem::remove_all(directory);
  } catch (const std::exception& error) {
    std::filesystem::remove_all(directory);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
