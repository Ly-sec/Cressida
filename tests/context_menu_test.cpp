#include "render/scene/input_dispatcher.h"
#include "render/text/cairo_text_renderer.h"
#include "ui/controls/context_menu.h"

#include <iostream>
#include <stdexcept>

namespace {
  // Real CPU text metrics; no GL context or texture operations are needed for
  // menu layout, keyboard navigation and pointer dispatch.
  class MetricsRenderer final : public Renderer {
    CairoTextRenderer m_text;
    static TextMetrics convert(const CairoTextRenderer::TextMetrics& value) {
      return {
          .width = value.width,
          .left = value.left,
          .right = value.right,
          .top = value.top,
          .bottom = value.bottom,
          .inkTop = value.inkTop,
          .inkBottom = value.inkBottom,
          .inkLeft = value.inkLeft,
          .inkRight = value.inkRight,
          .capHeight = value.capHeight,
          .lineCount = value.lineCount
      };
    }

  public:
    MetricsRenderer() { m_text.initialize(nullptr, nullptr); }
    TextMetrics measureText(
        std::string_view text, float size, FontWeight weight, float width, int lines, TextAlign align,
        std::string_view family, TextEllipsize ellipsize, bool markup
    ) override {
      return convert(m_text.measure(1, text, size, weight, width, lines, align, family, ellipsize, markup));
    }
    TextMetrics measureFont(float size, FontWeight weight) override {
      return convert(m_text.measureFont(1, size, weight));
    }
    void measureTextCursorStops(
        std::string_view text, float size, const std::vector<std::size_t>& offsets, std::vector<float>& stops,
        FontWeight weight, bool markup = false
    ) override {
      m_text.measureCursorStops(1, text, size, offsets, stops, weight, markup);
    }
    void measureTextCursorStopsWrapped(
        std::string_view text, float size, const std::vector<std::size_t>& offsets, float width,
        std::vector<TextCursorStop>& stops, FontWeight weight, bool markup = false
    ) override {
      m_text.measureCursorStopsWrapped(1, text, size, offsets, width, stops, weight, markup);
    }
    TextMetrics measureGlyph(char32_t, float size) override { return {.width = size, .right = size, .top = -size}; }
    TextureManager& textureManager() override { throw std::runtime_error("CPU menu tests must not allocate textures"); }
    float renderScale() const noexcept override { return 1; }
  };
  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }
} // namespace
int main() {
  try {
    require(
        ContextMenuControl::compiledEntrySize() == sizeof(ContextMenuControlEntry),
        "Host and shared library must agree on menu entry layout before exchanging records"
    );
    MetricsRenderer renderer;
    InputDispatcher input;
    Node root;
    root.setSize(600, 500);
    auto control = std::make_unique<ContextMenuControl>();
    auto* menu = control.get();
    menu->setPosition(20, 20);
    menu->setMenuWidth(250);
    menu->setEntries(
        {{.id = 1, .label = "Heading", .header = true},
         {.id = 2, .label = "Unavailable", .enabled = false},
         {.separator = true},
         {.id = 3, .label = "Reply"},
         {.id = 4, .label = "Copy message link"}}
    );
    int selected = 0, activations = 0;
    menu->setOnActivate([&](const auto& entry) {
      selected = entry.id;
      ++activations;
    });
    root.addChild(std::move(control));
    root.layout(renderer);
    input.setSceneRoot(&root);
    require(menu->activateHighlighted() && selected == 3, "Keyboard activation starts on the first enabled action");
    require(menu->moveHighlight(1), "Move past the current row");
    require(menu->activateHighlighted() && selected == 4, "Keyboard navigation advances to the next action");
    require(menu->moveHighlight(1), "Wrap to an enabled row");
    require(
        menu->activateHighlighted() && selected == 3, "Keyboard wrapping skips disabled rows, headers and separators"
    );
    const auto before = activations;
    const float x = 32, y = 20 + menu->rowTop(3) + 10;
    input.pointerMotion(x, y, 0);
    input.pointerButton(x, y, 273, true, 0, 0, false);
    input.pointerButton(x, y, 273, false, 0, 0, false);
    require(activations == before, "Right-clicking a menu row must not activate an account action");
    input.pointerButton(x, y, 272, true, 0, 0, false);
    auto retained = root.removeChild(menu);
    root.addChild(std::move(retained));
    root.layout(renderer);
    input.setSceneRoot(&root);
    input.pointerButton(x, y, 272, false, 0, 0, false);
    require(
        activations == before + 1 && selected == 3, "Retained native menu rows preserve click capture across repaint"
    );
    const auto shortWidth = ContextMenuControl::preferredWidth(renderer, {{.label = "Reply"}});
    const auto longWidth = ContextMenuControl::preferredWidth(
        renderer, {{.label = "Mute this conversation until I turn notifications back on"}}
    );
    require(longWidth > shortWidth, "Native menu width responds to actual text metrics");
    const auto iconWidth = ContextMenuControl::preferredWidth(renderer, {{.label = "Reply", .icon = "corner-up-left"}});
    require(iconWidth > shortWidth, "Icon slots contribute to content-sized menu width");
    const auto emojiWidth = ContextMenuControl::preferredWidth(renderer, {{.label = ":heart:", .emoji = "❤️"}});
    require(
        emojiWidth > ContextMenuControl::preferredWidth(renderer, {{.label = ":heart:"}}),
        "Large emoji occupy a separate leading slot without crowding their names"
    );
    menu->setEntries(
        {{.id = 5, .label = "Add Reaction", .hasSubmenu = true, .icon = "mood-smile"},
         {.id = 6, .label = "Delete Message", .icon = "trash", .danger = true}}
    );
    int opened = 0;
    float anchor = 0;
    menu->setOnSubmenuOpen([&](const auto& entry, float y) {
      require(entry.id == 5, "Submenu callback identifies its parent action");
      ++opened;
      anchor = y;
    });
    input.pointerLeave();
    root.layout(renderer);
    input.pointerMotion(32, 20 + menu->rowTop(0) + 10, 0);
    require(opened == 1 && anchor > 0, "Hover opens the reaction submenu without clicking");
    const auto beforeSubmenu = activations;
    require(
        menu->activateHighlighted() && opened == 2 && activations == beforeSubmenu,
        "Keyboard activation opens a submenu without invoking a message mutation"
    );
    input.setSceneRoot(nullptr);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
