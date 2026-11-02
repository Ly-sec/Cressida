#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace app {
  inline bool validColorMode(std::string_view mode) { return mode == "dark" || mode == "light" || mode == "system"; }
  inline bool validThemeId(std::string_view id) {
    return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
  }
  struct ThemeColor {
    static constexpr std::uint8_t opaque = 0xff;
    std::uint32_t rgb;
    std::uint8_t alpha;
    constexpr ThemeColor(std::uint32_t color = 0, std::uint8_t opacity = opaque) : rgb(color), alpha(opacity) {}
    bool operator==(const ThemeColor&) const = default;
  };
  struct ThemePalette {
    ThemeColor background, sidebar, panel, raised, foreground, muted, accent, ink;
    ThemeColor rail, danger, destructive, success;
    ThemeColor keyword, string, number, function, type, property, preprocessor, added, removed;
    ThemeColor hover = property, onHover = ink;
    bool operator==(const ThemePalette&) const = default;
  };
  constexpr ThemePalette defaultThemePalette(bool light) {
    return light ? ThemePalette{0xF5F5F7, 0xE8E9EE, 0xDCDEE6, 0xCDD0DC, 0x20222C, 0x616678, 0x6D4DB5,
                                0xFFFFFF, 0xE8E9EE, 0xB52C39, 0xC32B3C, 0x2C723E, 0x7C3DA5, 0x326B39,
                                0x9A520C, 0x2159A8, 0x7A5400, 0x086B75, 0x086B75, 0x2C723E, 0xAC2E40}
                 : ThemePalette{0x17181D, 0x1E2027, 0x24262F, 0x2E313D, 0xEEEFF5, 0x989BAE, 0xC6B5F5,
                                0x211A36, 0x14151A, 0xEF7D86, 0xDA373C, 0x43B581, 0xC792EA, 0x9BCF9F,
                                0xF0B981, 0x82AAFF, 0xE6C07B, 0x82C9CF, 0x82C9CF, 0x8BD49C, 0xF08B96};
  }
  struct ColorTheme {
    std::string id, name, description, author;
    ThemePalette dark = defaultThemePalette(false), light = defaultThemePalette(true);
    std::filesystem::path source;
    bool bundled = false;
    bool operator==(const ColorTheme&) const = default;
  };
  struct ThemeIssue {
    std::string file, message;
    bool operator==(const ThemeIssue&) const = default;
  };
  struct ThemeSnapshot {
    std::vector<ColorTheme> themes;
    std::vector<ThemeIssue> issues;
    bool systemLight = false;
    bool operator==(const ThemeSnapshot&) const = default;
    const ColorTheme& selected(std::string_view id) const;
  };
  ColorTheme defaultColorTheme();
  std::expected<ColorTheme, std::string> parseColorTheme(std::string_view text, const std::string& id);
  std::string themeTemplate();
  bool resolveLightMode(std::string_view mode, bool systemLight);
  std::filesystem::path userThemeDirectory();
  class ThemeCatalog {
  public:
    ThemeCatalog(std::filesystem::path bundled, std::filesystem::path user = {});
    ThemeSnapshot reload();

  private:
    std::filesystem::path m_bundled, m_user;
    std::map<std::filesystem::path, ColorTheme> m_good;
  };
} // namespace app
