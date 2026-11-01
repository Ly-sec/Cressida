#pragma once
#include "app/syntax_highlight.h"
#include "app/theme_catalog.h"
#include "render/core/color.h"

#include <algorithm>
#include <string_view>

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
    background = accessible(rgbHex(colors.background));
    sidebar = accessible(rgbHex(colors.sidebar));
    panel = accessible(rgbHex(colors.panel));
    raised = accessible(rgbHex(colors.raised));
    foreground = accessible(rgbHex(colors.foreground));
    muted = accessible(rgbHex(colors.muted));
    accent = accessible(rgbHex(colors.accent));
    ink = accessible(rgbHex(colors.ink));
    railBackground = accessible(rgbHex(colors.rail));
    danger = accessible(rgbHex(colors.danger));
    destructive = accessible(rgbHex(colors.destructive));
    success = accessible(rgbHex(colors.success));
    hover = accessible(rgbHex(colors.hover));
    onHover = accessible(rgbHex(colors.onHover));
  }
  inline void select(std::string_view mode) { apply(defaultThemePalette(mode == "light"), mode == "light"); }
  inline Color syntaxColor(CodeToken token) {
    switch (token) {
    case CodeToken::Keyword:
      return accessible(rgbHex(palette.keyword));
    case CodeToken::String:
      return accessible(rgbHex(palette.string));
    case CodeToken::Comment:
      return muted;
    case CodeToken::Number:
      return accessible(rgbHex(palette.number));
    case CodeToken::Function:
      return accessible(rgbHex(palette.function));
    case CodeToken::Type:
      return accessible(rgbHex(palette.type));
    case CodeToken::Property:
      return accessible(rgbHex(palette.property));
    case CodeToken::Preprocessor:
      return accessible(rgbHex(palette.preprocessor));
    case CodeToken::Added:
      return accessible(rgbHex(palette.added));
    case CodeToken::Removed:
      return accessible(rgbHex(palette.removed));
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
