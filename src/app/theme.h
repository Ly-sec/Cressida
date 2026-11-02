#pragma once
#include "app/syntax_highlight.h"
#include "app/theme_catalog.h"
#include "render/core/color.h"

#include <algorithm>
#include <string_view>

namespace app {
  constexpr Color themeColor(ThemeColor value) { return withAlpha(rgbHex(value.rgb), colorByte(value.alpha)); }
} // namespace app

namespace app::theme {
  inline Color background, sidebar, panel, raised, foreground, muted, accent, ink;
  inline Color railBackground, danger, destructive, success;
  inline Color hover, onHover;
  inline ThemePalette palette = defaultThemePalette(false);
  inline bool lightTheme = false;
  inline float saturation = 1;
  inline Color accessible(Color color) {
    if (saturation >= 1)
      return color;
    const float gray = .2126F * color.r + .7152F * color.g + .0722F * color.b;
    color.r = gray + (color.r - gray) * saturation;
    color.g = gray + (color.g - gray) * saturation;
    color.b = gray + (color.b - gray) * saturation;
    return color;
  }
  inline void apply(const ThemePalette& colors, bool light, float colorSaturation = 1) {
    saturation = std::clamp(colorSaturation, 0.0F, 1.0F);
    palette = colors;
    lightTheme = light;
    background = accessible(themeColor(colors.background));
    sidebar = accessible(themeColor(colors.sidebar));
    panel = accessible(themeColor(colors.panel));
    raised = accessible(themeColor(colors.raised));
    foreground = accessible(themeColor(colors.foreground));
    muted = accessible(themeColor(colors.muted));
    accent = accessible(themeColor(colors.accent));
    ink = accessible(themeColor(colors.ink));
    railBackground = accessible(themeColor(colors.rail));
    danger = accessible(themeColor(colors.danger));
    destructive = accessible(themeColor(colors.destructive));
    success = accessible(themeColor(colors.success));
    hover = accessible(themeColor(colors.hover));
    onHover = accessible(themeColor(colors.onHover));
  }
  inline void select(std::string_view mode) { apply(defaultThemePalette(mode == "light"), mode == "light"); }
  inline Color syntaxColor(CodeToken token) {
    switch (token) {
    case CodeToken::Keyword:
      return accessible(themeColor(palette.keyword));
    case CodeToken::String:
      return accessible(themeColor(palette.string));
    case CodeToken::Comment:
      return muted;
    case CodeToken::Number:
      return accessible(themeColor(palette.number));
    case CodeToken::Function:
      return accessible(themeColor(palette.function));
    case CodeToken::Type:
      return accessible(themeColor(palette.type));
    case CodeToken::Property:
      return accessible(themeColor(palette.property));
    case CodeToken::Preprocessor:
      return accessible(themeColor(palette.preprocessor));
    case CodeToken::Added:
      return accessible(themeColor(palette.added));
    case CodeToken::Removed:
      return accessible(themeColor(palette.removed));
    default:
      return foreground;
    }
  }
  inline Color presenceColor(std::string_view status) {
    return accessible(rgbHex(
        status == "online"     ? 0x43B581
            : status == "idle" ? 0xFAA61A
            : status == "dnd"  ? 0xED4245
                               : 0x989BAE
    ));
  }
  inline std::string_view presenceName(std::string_view status) {
    return status == "online"   ? "Online"
        : status == "idle"      ? "Idle"
        : status == "dnd"       ? "Do Not Disturb"
        : status == "invisible" ? "Invisible"
                                : "Connected";
  }
} // namespace app::theme
