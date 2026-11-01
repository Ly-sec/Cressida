#include "app/theme_catalog.h"

#include <array>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <toml++/toml.hpp>

namespace app {
  namespace {
    struct Field {
      const char* name;
      std::uint32_t ThemePalette::* member;
      bool required = false;
    };
    constexpr Field fields[] = {
        {"background", &ThemePalette::background, true},
        {"sidebar", &ThemePalette::sidebar, true},
        {"panel", &ThemePalette::panel, true},
        {"raised", &ThemePalette::raised, true},
        {"foreground", &ThemePalette::foreground, true},
        {"muted", &ThemePalette::muted, true},
        {"accent", &ThemePalette::accent, true},
        {"ink", &ThemePalette::ink, true},
        {"rail", &ThemePalette::rail},
        {"danger", &ThemePalette::danger},
        {"destructive", &ThemePalette::destructive},
        {"success", &ThemePalette::success},
        {"keyword", &ThemePalette::keyword},
        {"string", &ThemePalette::string},
        {"number", &ThemePalette::number},
        {"function", &ThemePalette::function},
        {"type", &ThemePalette::type},
        {"property", &ThemePalette::property},
        {"preprocessor", &ThemePalette::preprocessor},
        {"added", &ThemePalette::added},
        {"removed", &ThemePalette::removed},
        {"hover", &ThemePalette::hover},
        {"on_hover", &ThemePalette::onHover}
    };
    std::expected<std::uint32_t, std::string> color(const toml::node& node) {
      const auto value = node.value<std::string>();
      if (!value || value->size() != 7 || value->front() != '#')
        return std::unexpected("Use a quoted #RRGGBB color.");
      std::uint32_t result = 0;
      const auto [end, error] = std::from_chars(value->data() + 1, value->data() + 7, result, 16);
      if (error != std::errc{} || end != value->data() + 7)
        return std::unexpected("Use a quoted #RRGGBB color.");
      return result;
    }
    std::expected<ThemePalette, std::string> palette(const toml::node_view<const toml::node>& node, bool light) {
      const auto* table = node.as_table();
      const auto variant = light ? "light" : "dark";
      if (!table)
        return std::unexpected(std::string("Missing [") + variant + "] palette.");
      auto result = defaultThemePalette(light);
      for (const auto& [key, value] : *table)
        if (std::ranges::none_of(fields, [&](const auto& field) { return key.str() == field.name; }))
          return std::unexpected(std::string(variant) + "." + std::string(key.str()) + ": Unknown color key.");
      for (const auto& field : fields) {
        const auto* value = table->get(field.name);
        if (!value) {
          if (field.required)
            return std::unexpected(std::string(variant) + "." + field.name + ": Missing color.");
          continue;
        }
        const auto parsed = color(*value);
        if (!parsed)
          return std::unexpected(std::string(variant) + "." + field.name + ": " + parsed.error());
        result.*(field.member) = *parsed;
      }
      if (!table->contains("rail"))
        result.rail = result.sidebar;
      if (!table->contains("hover"))
        result.hover = result.property;
      if (!table->contains("on_hover"))
        result.onHover = result.ink;
      return result;
    }
    std::expected<std::string, std::string> text(const toml::table& table, const char* key, bool required = false) {
      const auto* node = table.get(key);
      if (!node && !required)
        return std::string{};
      const auto value = node ? node->value<std::string>() : std::nullopt;
      if (!value
          || (required && value->empty())
          || value->size() > 512
          || std::ranges::any_of(*value, [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
        return std::unexpected(std::string(key) + ": Use a single-line string of up to 512 bytes.");
      return *value;
    }
  } // namespace
  ColorTheme defaultColorTheme() {
    return {.id = "cressida", .name = "Cressida", .description = "The original Cressida colors.", .bundled = true};
  }
  const ColorTheme& ThemeSnapshot::selected(std::string_view id) const {
    auto found = std::ranges::find(themes, id, &ColorTheme::id);
    if (found != themes.end())
      return *found;
    found = std::ranges::find(themes, "cressida", &ColorTheme::id);
    if (found != themes.end())
      return *found;
    static const auto fallback = defaultColorTheme();
    return fallback;
  }
  bool resolveLightMode(std::string_view mode, bool systemLight) {
    return mode == "light" || (mode == "system" && systemLight);
  }
  std::filesystem::path userThemeDirectory() {
    if (const auto* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
      return std::filesystem::path(config) / "cressida/themes";
    if (const auto* home = std::getenv("HOME"); home && *home)
      return std::filesystem::path(home) / ".config/cressida/themes";
    return {};
  }
  std::expected<ColorTheme, std::string> parseColorTheme(std::string_view bytes, const std::string& id) {
    if (!validThemeId(id))
      return std::unexpected("Use a filename containing lowercase letters, digits, hyphens or underscores.");
    if (bytes.size() > 65536)
      return std::unexpected("Theme files must fit within 64 KiB.");
    try {
      const auto table = toml::parse(bytes);
      if (table["version"].value<std::int64_t>() != 1)
        return std::unexpected("Set version = 1.");
      for (const auto& [key, value] : table)
        if (key != "version"
            && key != "name"
            && key != "description"
            && key != "author"
            && key != "dark"
            && key != "light")
          return std::unexpected(std::string(key.str()) + ": Unknown theme key.");
      auto name = text(table, "name", true), description = text(table, "description"), author = text(table, "author");
      if (!name || !description || !author)
        return std::unexpected(!name ? name.error() : !description ? description.error() : author.error());
      const auto dark = palette(table["dark"], false), light = palette(table["light"], true);
      if (!dark || !light)
        return std::unexpected(!dark ? dark.error() : light.error());
      return ColorTheme{
          .id = id, .name = *name, .description = *description, .author = *author, .dark = *dark, .light = *light
      };
    } catch (const toml::parse_error& error) {
      return std::unexpected(
          "TOML line " + std::to_string(error.source().begin.line) + ": " + std::string(error.description())
      );
    }
  }
  std::string themeTemplate() {
    auto definition = defaultColorTheme();
    toml::table result{{"version", 1}, {"name", "My theme"}, {"description", "A custom Cressida theme."}};
    for (const auto light : {false, true}) {
      toml::table colors;
      const auto& value = light ? definition.light : definition.dark;
      for (const auto& field : fields)
        colors.insert(field.name, std::format("#{:06x}", value.*(field.member)));
      result.insert(light ? "light" : "dark", std::move(colors));
    }
    std::ostringstream output;
    output << result << '\n';
    return output.str();
  }
  ThemeCatalog::ThemeCatalog(std::filesystem::path bundled, std::filesystem::path user)
      : m_bundled(std::move(bundled)), m_user(std::move(user)) {}
  ThemeSnapshot ThemeCatalog::reload() {
    ThemeSnapshot result;
    std::map<std::string, ColorTheme> themes{{"cressida", defaultColorTheme()}};
    std::map<std::filesystem::path, ColorTheme> retained;
    const auto scan = [&](const std::filesystem::path& directory, bool bundled) {
      if (directory.empty())
        return;
      std::error_code error;
      if (!std::filesystem::exists(directory, error))
        return;
      std::vector<std::filesystem::path> files;
      for (std::filesystem::directory_iterator at(directory, error), end; !error && at != end; at.increment(error)) {
        if (at->path().extension() == ".toml" && at->is_regular_file(error))
          files.push_back(at->path());
        if (files.size() > 256) {
          result.issues.push_back({directory.string(), "Keep at most 256 theme files in this directory."});
          return;
        }
      }
      if (error)
        result.issues.push_back({directory.string(), "Could not read the theme directory."});
      std::ranges::sort(files);
      for (const auto& path : files) {
        std::ifstream input(path, std::ios::binary);
        std::string bytes(65537, '\0');
        input.read(bytes.data(), bytes.size());
        bytes.resize(input.gcount());
        auto parsed = input.bad() || (!input && !input.eof())
            ? std::expected<ColorTheme, std::string>{std::unexpected("Could not read this theme file.")}
            : parseColorTheme(bytes, path.stem().string());
        if (!parsed) {
          result.issues.push_back({path.filename().string(), parsed.error()});
          if (const auto previous = m_good.find(path); previous != m_good.end()) {
            themes[previous->second.id] = previous->second;
            retained[path] = previous->second;
          }
          continue;
        }
        parsed->source = path;
        parsed->bundled = bundled;
        retained[path] = *parsed;
        themes[parsed->id] = std::move(*parsed);
      }
    };
    scan(m_bundled, true);
    scan(m_user, false);
    m_good = std::move(retained);
    result.themes.push_back(themes.at("cressida"));
    themes.erase("cressida");
    for (auto& [id, value] : themes)
      result.themes.push_back(std::move(value));
    return result;
  }
} // namespace app
