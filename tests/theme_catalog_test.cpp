#include "app/theme.h"
#include "app/theme_catalog.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

int main(int argc, char** argv) {
  const auto require = [](bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  char temporary[] = "/tmp/cressida-theme-catalog-XXXXXX";
  if (!mkdtemp(temporary))
    return 1;
  const auto root = std::filesystem::path(temporary);
  try {
    require(argc == 2, "Pass the bundled theme directory");
    const auto sample = app::themeTemplate();
    const auto parsed = app::parseColorTheme(sample, "my-theme");
    require(parsed.has_value(), "The copied template must be valid TOML");
    require(
        parsed->dark == app::defaultThemePalette(false) && parsed->light == app::defaultThemePalette(true),
        "The template contains complete, independent dark and light palettes"
    );
    require(
        parsed->dark.hover != parsed->dark.raised && parsed->light.hover != parsed->light.raised,
        "Button hover colors remain distinct from ordinary raised surfaces"
    );
    for (const bool light : {false, true}) {
      const auto& colors = light ? parsed->light : parsed->dark;
      app::theme::apply(colors, light);
      require(
          app::theme::hover == rgbHex(colors.hover) && app::theme::onHover == rgbHex(colors.onHover),
          "Theme application preserves paired hover background and text colors in both modes"
      );
    }
    auto legacy = sample;
    for (const std::string key : {"hover", "on_hover"}) {
      for (auto at = legacy.find("\n" + key + " ="); at != std::string::npos; at = legacy.find("\n" + key + " ="))
        legacy.erase(at, legacy.find('\n', at + 1) - at);
    }
    const auto fallback = app::parseColorTheme(legacy, "without-hover");
    require(
        fallback && fallback->dark.hover == fallback->dark.property && fallback->dark.onHover == fallback->dark.ink,
        "Existing theme files derive paired hover colors without adding required fields"
    );
    const auto overridden =
        app::parseColorTheme(legacy + "\nhover = '#123456'\non_hover = '#fedcba'\n", "hover-override");
    require(
        overridden && overridden->light.hover == 0x123456 && overridden->light.onHover == 0xfedcba,
        "Explicit hover colors override the fallback pair independently"
    );
    require(!app::parseColorTheme(sample, "../bad"), "Theme IDs cannot escape the theme directory");
    require(!app::parseColorTheme(sample + "\n[dark]\n", "bad"), "Duplicate TOML tables are rejected");
    require(!app::parseColorTheme("version=1\nname='Missing light'\n[dark]\n", "bad"), "Both variants are required");
    auto broken = sample;
    const auto color = broken.find("#17181d");
    require(color != std::string::npos, "Template contains the default background");
    broken.replace(color, 7, "#gggggg");
    require(!app::parseColorTheme(broken, "bad"), "Invalid colors reject the entire theme");
    require(!app::parseColorTheme(std::string(65537, ' '), "large"), "Theme parsing is bounded");
    require(
        !app::parseColorTheme(sample + "\nunknown_color = '#ffffff'\n", "typo"), "Unknown palette keys identify typos"
    );
    const auto user = root / "themes";
    std::filesystem::create_directories(user);
    app::ThemeCatalog catalog(argv[1], user);
    auto snapshot = catalog.reload();
    require(snapshot.themes.size() == 4 && snapshot.issues.empty(), "Every bundled theme parses");
    require(
        snapshot.selected("cressida").dark == app::defaultThemePalette(false),
        "Bundled defaults match the emergency palette"
    );
    require(
        snapshot.selected("catppuccin").dark.background == 0x1e1e2e
            && snapshot.selected("catppuccin").light.background == 0xeff1f5,
        "Catppuccin pairs Mocha with Latte"
    );
    require(snapshot.selected("rose-pine").light.background == 0xfaf4ed, "Rosé Pine provides Dawn");
    require(
        snapshot.selected("gruvbox").dark.background == 0x282828
            && snapshot.selected("gruvbox").dark.accent == 0xfabd2f
            && snapshot.selected("gruvbox").light.background == 0xfbf1c7
            && snapshot.selected("gruvbox").light.accent == 0xaf3a03,
        "Gruvbox provides distinct warm dark and light palettes"
    );
    std::ofstream(user / "custom.toml") << sample;
    snapshot = catalog.reload();
    const auto good = snapshot.selected("custom");
    require(good.id == "custom" && !good.bundled, "Custom files join the catalog");
    std::ofstream(user / "custom.toml") << broken;
    snapshot = catalog.reload();
    require(
        snapshot.selected("custom") == good && !snapshot.issues.empty(),
        "Invalid saves retain the last complete valid theme"
    );
    std::ofstream(user / "catppuccin.toml") << sample;
    snapshot = catalog.reload();
    require(
        !snapshot.selected("catppuccin").bundled && snapshot.selected("catppuccin").name == "My theme",
        "A custom filename can override a bundled theme without duplicates"
    );
    std::filesystem::remove(user / "catppuccin.toml");
    std::filesystem::remove(user / "custom.toml");
    snapshot = catalog.reload();
    require(
        snapshot.selected("custom").id == "cressida" && snapshot.selected("catppuccin").bundled,
        "Deleting a custom theme restores its bundled or default fallback"
    );
    require(
        app::resolveLightMode("light", false)
            && !app::resolveLightMode("dark", true)
            && app::resolveLightMode("system", true)
            && !app::resolveLightMode("system", false),
        "Fixed modes override the system while System follows it"
    );
    app::theme::apply(snapshot.selected("catppuccin").light, true);
    require(
        app::theme::syntaxColor(app::CodeToken::String) == rgbHex(0x40a02b) && app::theme::lightTheme,
        "Syntax and presentation use the resolved palette"
    );
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    std::filesystem::remove_all(root);
    return 1;
  }
  std::filesystem::remove_all(root);
}
