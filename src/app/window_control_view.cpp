#include "app/brand_logo.h"
#include "app/client_application.h"
#include "app/settings_style.h"
#include "app/theme.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"

#include <charconv>
#include <cmath>

namespace app {
  void ClientApplication::updateWindowIcons() {
    WindowIconRequest request{
        m_preferences.windowControls.style == windowIconStyleCount - 1 ? m_preferences.windowControls.icons
                                                                       : decltype(WindowIconRequest::paths){},
        logoColorHex(theme::foreground),
        std::clamp(
            static_cast<int>(std::ceil(layout::Titlebar::iconSize * m_window.target().contentScale())), 1,
            WindowIcons::maximumRasterSize
        )
    };
    if (m_windowIconRequest == request)
      return;
    m_windowIconRequest = request;
    for (auto& texture : m_windowIconTextures)
      m_window.context().textureManager().unload(texture);
    for (std::size_t i = 0; i < request.paths.size(); ++i)
      m_windowIconErrors[i] = request.paths[i].empty() ? "" : "Loading icon…";
    if (m_demo) {
      ++m_windowIconGeneration;
      for (std::size_t i = 0; i < request.paths.size(); ++i)
        m_windowIconErrors[i] = request.paths[i].empty() ? "" : "Icon files are not read in this offline preview.";
    } else
      m_windowIconGeneration = m_windowIcons.request(std::move(request));
  }
  void ClientApplication::drainWindowIcons() {
    if (auto result = m_windowIcons.poll(); result && result->generation == m_windowIconGeneration) {
      if (!m_window.context().makeCurrent(m_window.target()))
        return;
      m_windowIconErrors = std::move(result->errors);
      for (std::size_t i = 0; i < result->images.size(); ++i) {
        auto& texture = m_windowIconTextures[i];
        m_window.context().textureManager().unload(texture);
        if (const auto& image = result->images[i])
          texture = m_window.context().textureManager().loadFromRgba(image->rgba.data(), image->width, image->height);
      }
      m_window.invalidate();
    }
  }
  void ClientApplication::drawWindowIcon(Node& node, float x, float y, WindowIcon icon, bool over) {
    const auto index = static_cast<std::size_t>(icon);
    const auto style = m_preferences.windowControls.style;
    if (style == windowIconStyleCount - 1 && m_windowIconTextures[index].valid()) {
      const auto& texture = m_windowIconTextures[index];
      auto image = std::make_unique<ImageNode>();
      image->setPosition(x, y);
      image->setSize(layout::Titlebar::iconSize, layout::Titlebar::iconSize);
      image->setTextureId(texture.id);
      image->setTextureSize(texture.width, texture.height);
      image->setFitMode(ImageFitMode::Contain);
      node.addChild(std::move(image));
    } else if (style == static_cast<int>(WindowIconStyle::Dots)) {
      const auto inset = (layout::Titlebar::iconSize - layout::Titlebar::dotSize) / 2;
      if (over)
        glyph(node, x, y, layout::Titlebar::iconSize, std::string(windowIconGlyph(icon, 0)), theme::foreground);
      else
        glyph(node, x + inset, y + inset, layout::Titlebar::dotSize, "circle-filled", theme::foreground);
    } else {
      glyph(node, x, y, layout::Titlebar::iconSize, std::string(windowIconGlyph(icon, style)), theme::foreground);
    }
  }
  void ClientApplication::drawTitlebar(Node& root, float width) {
    const auto& controls = m_preferences.windowControls;
    const auto buttons = visibleWindowButtons(controls, m_window.capabilities());
    const auto area = layout::Titlebar::buttons(width, buttons.size(), controls.left, controls.buttonWidth);
    rectangle(root, 0, 0, width, layout::Titlebar::height, theme::sidebar);
    const auto labelLeft = controls.left ? area.width + layout::Titlebar::labelInset : layout::Titlebar::labelInset;
    const auto labelWidth = std::max(0.0F, width - area.width - layout::Titlebar::labelInset * 2);
    const auto text = m_window.target().renderer().measureText("CRESSIDA", Style::fontSizeMini, FontWeight::SemiBold);
    label(
        root, labelLeft, (layout::Titlebar::height - (text.bottom - text.top)) / 2, labelWidth, "CRESSIDA",
        Style::fontSizeMini, theme::muted, FontWeight::SemiBold
    );
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      const auto frame = layout::Titlebar::button(width, buttons.size(), controls.left, i, controls.buttonWidth);
      const auto button = buttons[i];
      const bool over = hovered(frame.x, frame.y, frame.width, frame.height);
      if (over && controls.hoverHighlight)
        rectangle(
            root, frame.x, frame.y, frame.width, frame.height,
            button == WindowButton::Close ? theme::destructive : theme::raised
        );
      const auto icon = windowButtonIcon(button, m_window.maximized());
      drawWindowIcon(
          root, frame.x + (frame.width - layout::Titlebar::iconSize) / 2,
          (frame.height - layout::Titlebar::iconSize) / 2, icon, over
      );
      m_hits.push_back(
          {frame.x,
           frame.y,
           frame.width,
           frame.height,
           button == WindowButton::Minimize       ? Action::Minimize
               : button == WindowButton::Maximize ? Action::Maximize
                                                  : Action::Close,
           {}}
      );
      if (over) {
        m_tooltip = windowIconNames[static_cast<std::size_t>(icon)];
        m_tooltipX = frame.x + frame.width / 2;
        m_tooltipY = frame.height;
      }
    }
  }
  bool ClientApplication::windowControlPreference(const std::string& action) {
    if (!action.starts_with("window-"))
      return false;
    if (!m_settingsOpen || m_settingsPage != "advanced")
      return true;
    auto& controls = m_preferences.windowControls;
    m_settingsStatus.clear();
    if (action == "window-reset") {
      controls = {};
      for (std::size_t i = 0; i < controls.icons.size(); ++i)
        m_settingsFields["window-icon:" + std::to_string(i)].clear();
      m_settingsStatus = "Titlebar settings reset.";
    } else if (action == "window-minimize")
      controls.minimize = !controls.minimize;
    else if (action == "window-maximize")
      controls.maximize = !controls.maximize;
    else if (action == "window-close")
      controls.close = !controls.close;
    else if (action == "window-hover")
      controls.hoverHighlight = !controls.hoverHighlight;
    else if (action == "window-left")
      controls.left = !controls.left;
    else {
      const auto colon = action.find(':');
      if (colon == std::string::npos)
        return true;
      const auto name = action.substr(0, colon);
      const auto number = std::string_view(action).substr(colon + 1);
      int value = 0;
      const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
      if (error != std::errc{} || end != number.data() + number.size() || value < 0)
        return true;
      if (name == "window-order" && value < windowButtonOrderCount)
        controls.order = value;
      else if (name == "window-style" && value < windowIconStyleCount)
        controls.style = value;
      else if (
          name == "window-spacing"
          && value >= Style::windowButtonMinimumWidth
          && value <= Style::windowButtonMaximumWidth
      )
        controls.buttonWidth = value;
      else if (
          (name == "window-icon-apply" || name == "window-icon-reset") && std::size_t(value) < controls.icons.size()
      ) {
        const auto key = "window-icon:" + std::to_string(value);
        const auto path = name == "window-icon-reset" ? std::string{} : m_settingsFields[key];
        if (!WindowIcons::validPath(path)) {
          m_windowIconErrors[value] = "Enter an absolute local file path.";
          m_window.invalidate();
          return true;
        }
        controls.icons[value] = path;
        m_settingsFields[key] = path;
        m_windowIconRequest.reset(); // Applying the same path explicitly reloads an edited file.
      } else
        return true;
    }
    savePreferences();
    return true;
  }
  void ClientApplication::drawWindowControlSettings(UserSettingsForm& form) {
    const auto& controls = m_preferences.windowControls;
    form.section("Titlebar controls");
    const auto toggle = [&](const std::string& key, const std::string& title, const std::string& description,
                            bool checked) {
      form.toggle(key, title, description, checked, [this, key](bool) { windowControlPreference(key); });
    };
    toggle(
        "window-minimize", "Show minimize",
        m_window.capabilities().minimize()
            ? "Show the minimize button when the desktop supports it."
            : "Your desktop does not support minimizing, so this button is currently hidden.",
        controls.minimize
    );
    toggle(
        "window-maximize", "Show maximize",
        m_window.capabilities().maximize()
            ? "Show maximize and restore when the desktop supports them."
            : "Your desktop does not support maximizing, so this button is currently hidden.",
        controls.maximize
    );
    toggle(
        "window-close", "Show close", "Hide the close button if you prefer desktop shortcuts or the tray menu.",
        controls.close
    );
    toggle(
        "window-left", "Buttons on the left", "Place window buttons at the left end of the titlebar.", controls.left
    );
    toggle(
        "window-hover", "Hover highlighting",
        "Highlight button backgrounds on hover, including the close button's red highlight.", controls.hoverHighlight
    );
    form.slider(
        "window-spacing", "Button spacing",
        "Choose the width of each titlebar button. Smaller widths bring icons closer together without resizing them.",
        controls.buttonWidth, Style::windowButtonMinimumWidth, Style::windowButtonMaximumWidth, 1,
        [this](double value) {
          windowControlPreference("window-spacing:" + std::to_string(static_cast<int>(std::lround(value))));
        },
        " px"
    );
    const auto choose = [&](const std::string& key, const std::string& title, const std::string& description, int value,
                            std::vector<std::string> choices) {
      form.choose(key, title, description, std::move(choices), value, [this, key](std::size_t index) {
        windowControlPreference(key + ":" + std::to_string(index));
      });
    };
    choose(
        "window-order", "Button order", "Arrange the visible buttons from left to right.", controls.order,
        {"Minimize, Maximize, Close", "Close, Minimize, Maximize", "Close, Maximize, Minimize",
         "Minimize, Close, Maximize", "Maximize, Minimize, Close", "Maximize, Close, Minimize"}
    );
    choose(
        "window-style", "Button icons",
        "Customize Cressida's titlebar; desktop decorations remain controlled by your compositor. Bundled icons follow "
        "your palette, and Custom uses the files below.",
        controls.style, {"Standard", "Circular", "Dots", "Custom"}
    );
    form.row("Icon preview", "Minimize, maximize, restore and close.");
    for (std::size_t i = 0; i < windowIconNames.size(); ++i)
      drawWindowIcon(
          form.node, static_cast<float>(i) * layout::Titlebar::buttonWidth(controls.buttonWidth),
          form.y + Style::spaceXs, static_cast<WindowIcon>(i), false
      );
    form.y += layout::Titlebar::height + Style::spaceSm;
    if (controls.style == windowIconStyleCount - 1) {
      form.section("Custom button icons");
      for (std::size_t i = 0; i < controls.icons.size(); ++i) {
        const auto key = "window-icon:" + std::to_string(i);
        const auto description = m_windowIconErrors[i].empty()
            ? "Absolute local SVG or PNG path, up to 256 KiB. SVG currentColor follows the palette. Apply reloads the "
              "file; empty uses the standard icon."
            : m_windowIconErrors[i];
        form.field(
            key, std::string(windowIconNames[i]) + " icon", description, m_settingsFields[key],
            [this, key](const std::string& path) { m_settingsFields[key] = path; }, layout::Titlebar::pathFieldHeight
        );
        const auto buttonWidth = std::min(form.width / 2 - Style::spaceXs, layout::Titlebar::defaultButtonWidth * 2);
        form.buttonControl(
            form.node, 0, form.y, buttonWidth, "Apply", "window-icon-apply:" + std::to_string(i),
            ButtonVariant::Default, true
        );
        form.buttonControl(
            form.node, buttonWidth + Style::spaceSm, form.y, buttonWidth, "Reset",
            "window-icon-reset:" + std::to_string(i), ButtonVariant::Default, true
        );
        form.y += layout::Settings::actionHeight + Style::spaceMd;
      }
    }
    form.buttonControl(
        form.node, 0, form.y, std::min(form.width, layout::Titlebar::defaultButtonWidth * 3), "Restore defaults",
        "window-reset", ButtonVariant::Default, true
    );
    form.y += layout::Settings::actionHeight + Style::spaceSm;
  }
} // namespace app
