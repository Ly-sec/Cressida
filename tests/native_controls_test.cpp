#include "app/brand_logo.h"
#include "app/controls/chat_input.h"
#include "app/member_list_layout.h"
#include "app/theme.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/keybind_matcher.h"
#include "core/text_clipboard.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/rect_node.h"
#include "render/scene/text_node.h"
#include "render/text/cairo_text_renderer.h"
#include "render/text/glyph_registry.h"
#include "ui/controls/button.h"
#include "ui/controls/collapsible.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include "ui/controls/toggle.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  // Input geometry uses the production Pango metrics without a graphics context.
  class MetricsRenderer final : public Renderer {
    CairoTextRenderer m_text;
    float m_scale;

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
    MetricsRenderer(std::string family, float scale) : m_scale(scale) {
      m_text.initialize(nullptr, nullptr);
      m_text.setFontFamily(std::move(family));
    }
    TextMetrics measureText(
        std::string_view text, float size, FontWeight weight, float width, int lines, TextAlign align,
        std::string_view family, TextEllipsize ellipsize, bool markup
    ) override {
      return convert(m_text.measure(m_scale, text, size, weight, width, lines, align, family, ellipsize, markup));
    }
    TextMetrics measureFont(float size, FontWeight weight) override {
      return convert(m_text.measureFont(m_scale, size, weight));
    }
    void measureTextCursorStops(
        std::string_view text, float size, const std::vector<std::size_t>& offsets, std::vector<float>& stops,
        FontWeight weight, bool markup = false
    ) override {
      m_text.measureCursorStops(m_scale, text, size, offsets, stops, weight, markup);
    }
    void measureTextCursorStopsWrapped(
        std::string_view text, float size, const std::vector<std::size_t>& offsets, float width,
        std::vector<TextCursorStop>& stops, FontWeight weight, bool markup = false
    ) override {
      m_text.measureCursorStopsWrapped(m_scale, text, size, offsets, width, stops, weight, markup);
    }
    TextMetrics measureGlyph(char32_t, float size) override { return {.width = size, .right = size, .top = -size}; }
    TextureManager& textureManager() override { throw std::runtime_error("Input layout must not allocate textures"); }
    float renderScale() const noexcept override { return m_scale; }
  };

  const TextNode* findTextNode(const Node& node) {
    if (node.type() == NodeType::Text)
      return static_cast<const TextNode*>(&node);
    for (const auto& child : node.children())
      if (const auto* text = findTextNode(*child))
        return text;
    return nullptr;
  }

  void settleDeferredCalls() {
    for (auto& callback : DeferredCall::takePending())
      callback();
  }

  void requireStableInputLayout(Input& field, MetricsRenderer& renderer, const char* message) {
    field.layout(renderer);
    const auto* text = findTextNode(field);
    if (!text)
      throw std::runtime_error("Input layout retains its text node");
    float initialX = 0, initialY = 0;
    Node::absolutePosition(text, initialX, initialY);
    const auto initialCaret = field.textInputState();
    constexpr int kRepeatedLayoutPasses = 8;
    constexpr float kGeometryTolerance = 0.001F;
    for (int pass = 0; pass < kRepeatedLayoutPasses; ++pass) {
      field.markLayoutDirty();
      field.layout(renderer);
      settleDeferredCalls();
      float x = 0, y = 0;
      Node::absolutePosition(text, x, y);
      const auto caret = field.textInputState();
      if (std::abs(x - initialX) > kGeometryTolerance
          || std::abs(y - initialY) > kGeometryTolerance
          || caret.cursorRectX != initialCaret.cursorRectX
          || caret.cursorRectY != initialCaret.cursorRectY
          || caret.cursorRectHeight != initialCaret.cursorRectHeight)
        throw std::runtime_error(message);
    }
  }

  void inputVerticalScrollCases() {
    for (const auto* family : {"sans-serif", "JetBrains Mono", "Noto Sans Math"}) {
      for (const float scale : {1.0F, 1.25F, 2.0F}) {
        MetricsRenderer renderer(family, scale);
        const float width = Style::controlHeightLg * 6;
        const float composerHeight = app::layout::Composer::minimumHeight - Style::spaceSm;
        Input field;
        field.setMultiline(true);
        field.setMultilineCenterWhenFits(true);
        field.setFontSize(Style::fontSizeBody);
        field.setHorizontalPadding(Style::spaceXs);
        field.setPlaceholder("Message this channel…");
        field.setFrameVisible(false);
        field.setFrameSize(width, composerHeight);
        field.layout(renderer);
        field.inputArea()->dispatchFocusGain();
        settleDeferredCalls();
        requireStableInputLayout(field, renderer, "A focused composer placeholder stays still across redraws");

        // Force an oversized empty caret independently of the installed font.
        constexpr float kViewportShortfall = Style::spaceXs - Style::borderWidth;
        const float emptyRow = renderer.fontRowExtent(Style::fontSizeBody);
        const float shortHeight = emptyRow + Style::spaceSm * 2 - kViewportShortfall;
        field.setFrameSize(width, shortHeight);
        requireStableInputLayout(field, renderer, "An empty row taller than its viewport does not oscillate");
        field.setPlaceholder("Short hint");
        requireStableInputLayout(field, renderer, "Replacing a placeholder preserves stable vertical placement");
        field.inputArea()->dispatchFocusLoss();
        requireStableInputLayout(field, renderer, "An unfocused placeholder stays still across redraws");
        field.inputArea()->dispatchFocusGain();
        settleDeferredCalls();
        requireStableInputLayout(field, renderer, "Refocusing an oversized empty row does not restart jitter");

        field.setValue("x");
        std::vector<TextCursorStop> stops;
        renderer.measureTextCursorStopsWrapped("x", Style::fontSizeBody, {0, 1}, width, stops, FontWeight::Normal);
        if (stops.size() != 2)
          throw std::runtime_error("A shaped test character supplies both cursor stops");
        field.setFrameSize(width, stops.front().height + Style::spaceSm * 2 - kViewportShortfall);
        requireStableInputLayout(field, renderer, "An oversized typed row stays still across redraws");

        field.setFrameSize(width, composerHeight + Style::controlHeightSm);
        field.setValue("First line\nSecond line\nThird line\nFourth line\nLast line");
        requireStableInputLayout(field, renderer, "The last line remains visible without scroll oscillation");
        const auto* text = findTextNode(field);
        float bottomX = 0, bottomY = 0;
        Node::absolutePosition(text, bottomX, bottomY);
        field.inputArea()->dispatchKey(XKB_KEY_Home, 0, KeyMod::Ctrl, true);
        requireStableInputLayout(field, renderer, "Returning to the first line settles vertical scrolling");
        float topX = 0, topY = 0;
        Node::absolutePosition(text, topX, topY);
        if (field.textInputState().cursor != 0 || topY <= bottomY)
          throw std::runtime_error("Moving to the first line reveals the beginning of a scrolled input");
        field.inputArea()->dispatchKey(XKB_KEY_End, 0, KeyMod::Ctrl, true);
        requireStableInputLayout(field, renderer, "Returning to the last line settles vertical scrolling");
        if (field.textInputState().cursor != static_cast<std::int32_t>(field.value().size()))
          throw std::runtime_error("Moving to the last line retains the final text offset");

        field.setFrameSize(Style::controlHeightLg * 2, composerHeight);
        field.setValue("A long paragraph wraps across several rows while the caret follows its final word.");
        requireStableInputLayout(field, renderer, "Soft-wrapped text keeps its final row stable across redraws");
        Node::absolutePosition(text, bottomX, bottomY);
        field.inputArea()->dispatchKey(XKB_KEY_Home, 0, KeyMod::Ctrl, true);
        requireStableInputLayout(field, renderer, "Soft-wrapped text can scroll back to its first row");
        Node::absolutePosition(text, topX, topY);
        if (topY <= bottomY)
          throw std::runtime_error("Soft wrapping extends the paragraph beyond the input viewport");
        field.inputArea()->dispatchKey(XKB_KEY_End, 0, KeyMod::Ctrl, true);
        requireStableInputLayout(field, renderer, "Soft-wrapped text can scroll back to its final row");
        field.setFrameSize(width, composerHeight);
        field.setValue({});
        requireStableInputLayout(field, renderer, "Clearing scrolled text restores a stable placeholder");
        field.inputArea()->dispatchFocusLoss();
      }
    }
  }
} // namespace

struct Clipboard final : TextClipboard {
  std::string text;
  std::optional<std::string> clipboardText() override { return text; }
  void setClipboardText(std::string value) override { text = std::move(value); }
};
int main() {
  auto require = [](bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  };
  try {
    inputVerticalScrollCases();
    {
      app::ChatInput chat;
      chat.setOnKeyEvent([&](std::uint32_t key, std::uint32_t modifiers) {
        return key == XKB_KEY_colon
            && !(modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super))
            && chat.completeTrademark();
      });
      chat.setValue("Cressida:tm");
      chat.inputArea()->dispatchKey(XKB_KEY_colon, ':', KeyMod::Shift, true);
      const std::string expected = "Cressida\u2122\uFE0E";
      require(chat.value() == expected, "Typing the closing colon converts a trademark directly after a word");
      require(
          chat.textInputState().cursor == static_cast<std::int32_t>(expected.size()),
          "Trademark conversion retains the native caret"
      );
      require(chat.edit(Input::EditCommand::Undo) && chat.value() == "Cressida:tm", "Trademark conversion is undoable");
      require(
          chat.edit(Input::EditCommand::Redo) && chat.value() == expected, "Redo restores the text-style trademark"
      );
      Clipboard copied;
      Input::setTextClipboard(&copied);
      chat.selectAll();
      require(chat.edit(Input::EditCommand::Copy) && copied.text == expected, "Copy retains the converted trademark");
      Input::setTextClipboard(nullptr);
      std::string sent;
      chat.setMultiline(true);
      chat.setSubmitOnEnter(true);
      chat.clearSelection();
      chat.setOnSubmit([&](const std::string& text) { sent = text; });
      chat.textInputApplyEdit({.submit = true});
      require(sent == expected, "Submitting uses the same converted text as the draft");
      chat.setValue("prefix  suffix");
      chat.applyEdit(chat.value(), 7, 7);
      chat.textInputApplyEdit({.commitText = ":tm:", .hasCommitText = true});
      require(chat.value() == "prefix \u2122\uFE0E suffix", "Committed shortcodes retain surrounding text");
      for (const std::string literal :
           {"`word:tm", "```\nword:tm", "~~~\nword:tm", "    word:tm", "\\:tm", "https://example.test/:tm", "<:tm"}) {
        chat.setValue(literal);
        chat.inputArea()->dispatchKey(XKB_KEY_colon, ':', 0, true);
        require(chat.value() == literal + ':', "Code, escaped text, URLs and custom emoji tokens stay literal");
      }
      chat.setValue("word:tm");
      chat.textInputApplyEdit({.preeditText = ":", .hasPreedit = true});
      require(chat.hasPreedit() && chat.value() == "word:tm", "Preedit never converts a trademark");
      chat.textInputResetPreedit();
    }
    for (const float scale : {1.0F, 1.25F, 2.0F}) {
      app::theme::select("dark");
      MetricsRenderer renderer("sans-serif", scale);
      app::ChatInput chat;
      chat.setMultiline(true);
      chat.setFrameSize(Style::controlHeightLg * 6, Style::controlHeightLg * 4);
      chat.setValue("> **Wide bold draft** with _italic_ and `code` 👋");
      chat.inputArea()->dispatchFocusGain();
      requireStableInputLayout(chat, renderer, "Styled drafts retain stable wrapping and cursor geometry");
      const auto* text = findTextNode(chat);
      require(text && text->useMarkup(), "Chat fields paint the styled layout");
      const auto plain = chat.displayValue();
      std::vector<TextCursorStop> stops;
      renderer.measureTextCursorStopsWrapped(
          text->text(), text->fontSize(), {plain.size()}, text->maxWidth(), stops, text->fontWeight(), true
      );
      float x = 0, y = 0;
      Node::absolutePosition(text, x, y);
      require(
          chat.textInputState().cursorRectX == std::round(x + stops.back().x),
          "The native caret uses the same styled shaping and wrap as the painted text"
      );
      const auto source = chat.value();
      chat.selectAll();
      Clipboard copied;
      Input::setTextClipboard(&copied);
      require(
          chat.edit(Input::EditCommand::Copy) && copied.text == source,
          "Copying a styled quote retains the source marker and formatting"
      );
      Input::setTextClipboard(nullptr);
      chat.textInputApplyEdit({.preeditText = "仮", .hasPreedit = true});
      require(
          chat.value() == source && chat.displayValue() == "仮", "Styled preedit keeps the committed draft separate"
      );
      chat.textInputResetPreedit();
      require(chat.value() == source && chat.displayValue() == plain, "Cancelling composition restores styled quotes");
      require(
          chat.applyEdit("**replacement**", 15, 15) && chat.edit(Input::EditCommand::Undo) && chat.value() == source,
          "Styling leaves the native undo snapshot intact"
      );
      chat.setMultiline(false);
      chat.setFrameSize(Style::controlHeightLg * 2, Style::controlHeight);
      chat.setValue("**Long bold content** and `monospace` that scroll beyond the visible single-line field");
      requireStableInputLayout(chat, renderer, "Styled slices retain stable single-line scrolling");
      chat.setPasswordMode(true);
      chat.layout(renderer);
      require(!findTextNode(chat)->useMarkup(), "Password fields cannot retain draft markup");
    }
    {
      const auto saved = palette;
      auto colors = saved;
      colors.hover = colors.primary;
      colors.onHover = colors.onPrimary;
      setPalette(colors);
      for (const auto variant : {ButtonVariant::Default, ButtonVariant::Primary, ButtonVariant::Secondary}) {
        Button button;
        button.setText("Hover");
        button.setVariant(variant);
        button.setEnabled(true);
        button.setHoveredVisual(true);
        const bool hoverFill = std::ranges::any_of(button.children(), [&](const auto& child) {
          return child->type() == NodeType::Rect && static_cast<const RectNode&>(*child).style().fill == colors.hover;
        });
        require(
            hoverFill && button.label()->color() == colors.onHover,
            "Native buttons pair their hovered background with the hovered text color"
        );
        button.setHoveredVisual(false);
        require(
            button.label()->color() == resolveColorSpec(Button::defaultPalette(variant).normal.label),
            "Leaving a button restores its ordinary text color"
        );
      }
      setPalette(saved);
    }
    {
      Collapsible collapsible;
      collapsible.setBody(std::make_unique<Node>());
      InputDispatcher input;
      auto* header = input.firstTabFocusUnder(&collapsible);
      require(header && !collapsible.expanded(), "A collapsed section exposes its header to keyboard focus");
      int changes = 0;
      collapsible.setOnToggle([&](bool) { ++changes; });
      header->dispatchKey(XKB_KEY_Right, 0, 0, true);
      require(collapsible.expanded() && changes == 1, "Right expands a native collapsible once");
      header->dispatchKey(XKB_KEY_Right, 0, 0, true);
      require(changes == 1, "Repeated expansion does not send duplicate changes");
      header->dispatchKey(XKB_KEY_Left, 0, 0, true);
      require(!collapsible.expanded() && changes == 2, "Left collapses the native section");
      header->dispatchKey(XKB_KEY_space, 0, 0, true);
      header->dispatchKey(XKB_KEY_Return, 0, 0, true);
      require(!collapsible.expanded() && changes == 4, "Space and Enter toggle the same native control");
      using Card = app::layout::ActivityCard;
      const auto wide =
          Card::timeline({0, 0, Style::profileCardWidth, Card::row}, Style::spaceSection, Style::spaceSection);
      const auto narrow =
          Card::timeline({0, 0, Style::spaceSection * 2, Card::row}, Style::spaceSection, Style::spaceSection);
      require(
          wide.track.y < wide.elapsed.height && wide.track.x >= wide.elapsed.width,
          "A wide activity timeline places its progress between the time labels"
      );
      require(
          narrow.track.y >= narrow.elapsed.height
              && narrow.track.width == Style::spaceSection * 2
              && narrow.height >= narrow.track.y + narrow.track.height,
          "A narrow activity timeline puts its full-width track below the labels without overlap"
      );
    }
    std::ifstream logoFile(paths::assetPath("cressida-logo.svg"), std::ios::binary);
    const std::string logo(std::istreambuf_iterator<char>{logoFile}, std::istreambuf_iterator<char>{});
    require(
        !logo.empty() && logo.find("<path") != std::string::npos,
        "The native UI ships the original Cressida vector artwork"
    );
    for (const bool light : {false, true}) {
      const auto colors = app::logoGradientColors(
          rgbHex(0xA6B975), rgbHex(light ? 0x162027 : 0xF1EBD5), rgbHex(light ? 0xF8F8F2 : 0x0B1924), light
      );
      const auto svg = app::themedLogoSvg(logo, colors);
      require(
          svg.substr(svg.find("<path")) == logo.substr(logo.find("<path")),
          "Theme adaptation preserves every logo path and its drawing order"
      );
      require(
          colors[0] != colors[2] && colors[1] != colors[3],
          "The logo retains distinct highlight and shadow gradients in both modes"
      );
      for (const auto color : colors)
        require(
            color.a == 1 && svg.find(app::logoColorHex(color)) != std::string::npos,
            "Themed gradients remain opaque and visible instead of becoming transparency"
        );
    }
    struct MemberRow {
      int index;
      std::string group;
    };
    const std::vector<MemberRow> members{{0, "Online"}, {1, {}}, {2, "Offline"}, {3, {}}, {1000, "Other"}};
    const app::layout::MemberListLayout layout(members, 1002);
    require(
        layout.top(1) == app::layout::Members::groupHeight
            && layout.top(3) == app::layout::Members::groupHeight * 2 + app::layout::Members::rowHeight,
        "Member groups compact independently of ordinary member rows"
    );
    for (const auto index : {0, 1, 2, 3, 999, 1000, 1001})
      require(
          layout.indexAt(layout.top(index)) == index,
          "Sparse remote member indices map back from their compact scroll positions"
      );
    require(
        layout.maximumScroll(layout.height() + app::layout::Members::rowHeight) == 0
            && layout.indexAt(layout.height()) == 1001,
        "Member scrolling stays bounded across unloaded pages and at the last member"
    );
    for (const auto scale : {0.0F, 0.5F, 2.0F}) {
      Style::setCornerRadiusScale(scale);
      require(
          Style::scaledRadiusXl() == Style::radiusXl * scale
              && Style::scaledShapeRadius(Style::surfaceRadius, Style::spaceSection, Style::spaceXl)
                  == Style::surfaceRadius * scale,
          "All surface and control corners use the same global radius scale"
      );
      require(
          Style::scaledShapeRadius(Style::spaceSm, Style::spaceLg, Style::spaceLg) == Style::spaceSm,
          "Circular identities retain their shape while UI corners change"
      );
    }
    Style::setCornerRadiusScale(-1);
    require(Style::cornerRadiusScale() == 0, "Corner scaling clamps below square");
    Style::setCornerRadiusScale(3);
    require(Style::cornerRadiusScale() == 2, "Corner scaling clamps to its maximum");
    Style::setCornerRadiusScale(1);
    FT_Library fonts = nullptr;
    require(FT_Init_FreeType(&fonts) == 0, "Initialize bundled icon font verification");
    FT_Face face = nullptr;
    const auto font = paths::assetPath("fonts/tabler-icons.ttf").string();
    require(FT_New_Face(fonts, font.c_str(), 0, &face) == 0, "Load the copied Tabler font");
    for (const auto* icon :
         {"message-circle",
          "settings",
          "users",
          "clipboard-plus",
          "mood-smile",
          "x",
          "minus",
          "square",
          "squares",
          "chevron-left",
          "chevron-right",
          "chevron-down",
          "zoom-in",
          "zoom-out",
          "loader",
          "activity-listening",
          "activity-watching",
          "activity-playing",
          "activity-competing",
          "bold",
          "italic",
          "underline",
          "strikethrough",
          "blockquote",
          "code",
          "eye-off",
          "arrow-back-up",
          "arrow-forward-up",
          "scissors",
          "clipboard",
          "clipboard-text",
          "select-all",
          "typography"})
      require(
          GlyphRegistry::contains(icon) && FT_Get_Char_Index(face, GlyphRegistry::lookup(icon)) != 0,
          "Every native action glyph resolves to a bundled font character"
      );
    FT_Done_Face(face);
    FT_Done_FreeType(fonts);
    Clipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input::setValidateKeyMatcher([](std::uint32_t key, std::uint32_t) { return key == XKB_KEY_Return; });
    Input field;
    require(!field.textInputState().multiline, "Single-line input-method state does not advertise line-break support");
    auto key = [&](std::uint32_t symbol, std::uint32_t modifiers = 0, std::uint32_t unicode = 0) {
      field.inputArea()->dispatchKey(symbol, unicode, modifiers, true);
    };
    field.setTextPresenter([](std::string_view text) {
      std::vector<TextReplacement> result;
      if (const auto at = text.find("<@123>"); at != std::string_view::npos)
        result.push_back({at, at + std::string_view("<@123>").size(), "@Maya"});
      return result;
    });
    field.setValue("Hello <@123>!");
    require(field.displayValue() == "Hello @Maya!", "Inputs present readable text while retaining protocol syntax");
    field.moveCaretLeft();
    field.moveCaretLeft();
    require(field.textInputState().cursor == 6, "Caret navigation treats a presented mention as one editing unit");
    field.moveCaretRight();
    key(XKB_KEY_BackSpace);
    require(field.value() == "Hello !", "Backspace removes a complete mention instead of corrupting its ID");
    key(XKB_KEY_z, KeyMod::Ctrl);
    require(
        field.value() == "Hello <@123>!" && field.displayValue() == "Hello @Maya!",
        "Undo restores mention source and presentation together"
    );
    key(XKB_KEY_a, KeyMod::Ctrl);
    key(XKB_KEY_c, KeyMod::Ctrl);
    require(clipboard.text == "Hello <@123>!", "Copy preserves the original mention identity");
    TextInputEdit preedit;
    preedit.hasPreedit = true;
    preedit.preeditText = "日本";
    field.clearSelection();
    field.textInputApplyEdit(preedit);
    require(
        field.textInputState().surroundingText == "Hello <@123>!", "IME surrounding text retains source byte offsets"
    );
    field.textInputResetPreedit();
    field.setValue("<@123>");
    TextInputEdit eraseMention;
    eraseMention.hasDelete = true;
    eraseMention.deleteBeforeLength = 1;
    field.textInputApplyEdit(eraseMention);
    require(field.value().empty(), "IME deletion expands to the complete displayed mention");
    key(XKB_KEY_z, KeyMod::Ctrl);
    require(
        field.value() == "<@123>" && field.displayValue() == "@Maya", "IME deletion remains one undoable source edit"
    );
    key(XKB_KEY_Home);
    TextInputEdit insertBefore;
    insertBefore.hasDelete = insertBefore.hasCommitText = true;
    insertBefore.commitText = "前 ";
    field.textInputApplyEdit(insertBefore);
    require(
        field.value() == "前 <@123>" && field.displayValue() == "前 @Maya",
        "A zero-length IME delete does not remove the following mention"
    );
    field.setValue("<@123>tail");
    key(XKB_KEY_Home);
    auto mentionPreedit = preedit;
    mentionPreedit.preeditText = "漢";
    field.textInputApplyEdit(mentionPreedit);
    key(XKB_KEY_Delete);
    require(
        field.value() == "tail" && field.displayValue() == "tail" && !field.hasPreedit(),
        "Native Delete after cancelling preedit removes the complete mention using restored source offsets"
    );
    key(XKB_KEY_z, KeyMod::Ctrl);
    require(
        field.value() == "<@123>tail" && field.displayValue() == "@Mayatail",
        "Undo restores the original mention without retaining its cancelled preedit"
    );
    field.setTextPresenter({});
    field.setValue("héllo 👋");
    field.selectAll();
    require(
        field.applyEdit("**héllo 👋**", 2, std::string("**héllo 👋").size()),
        "Host editing applies one committed transaction with its inner Unicode selection"
    );
    require(
        field.edit(Input::EditCommand::Undo) && field.value() == "héllo 👋",
        "Native menu undo restores both text and selection"
    );
    require(
        field.edit(Input::EditCommand::Redo) && field.value() == "**héllo 👋**",
        "Native menu redo restores the formatted selection"
    );
    require(
        field.edit(Input::EditCommand::Copy) && clipboard.text == "héllo 👋",
        "Native menu copy retains the inner selection"
    );
    require(!field.applyEdit("é", 1, 2), "Host editing rejects a split UTF-8 selection");
    field.setEnabled(false);
    require(
        !field.canEdit(Input::EditCommand::Cut) && !field.applyEdit("replacement", 0, 0),
        "Disabled inputs reject host edits and menu actions"
    );
    field.setEnabled(true);
    field.setTextPresenter({});
    field.setValue("one two");
    key(XKB_KEY_a, KeyMod::Ctrl);
    key(XKB_KEY_c, KeyMod::Ctrl);
    require(clipboard.text == "one two", "Native inputs implement Ctrl+A and Ctrl+C");
    key(XKB_KEY_x, KeyMod::Ctrl);
    require(field.value().empty(), "Native inputs cut the selection");
    key(XKB_KEY_z, KeyMod::Ctrl);
    require(field.value() == "one two", "Native input undo restores a cut");
    key(XKB_KEY_z, KeyMod::Ctrl | KeyMod::Shift);
    require(field.value().empty(), "Native input redo restores the cut");
    key(XKB_KEY_v, KeyMod::Ctrl);
    require(field.value() == "one two", "Native input paste uses its clipboard adapter");
    key(XKB_KEY_BackSpace, KeyMod::Ctrl);
    require(field.value() == "one ", "Ctrl+Backspace deletes the previous word");
    key(XKB_KEY_Home);
    key(XKB_KEY_Right, KeyMod::Ctrl | KeyMod::Shift);
    key(XKB_KEY_c, KeyMod::Ctrl);
    require(clipboard.text == "one ", "Ctrl+Shift+Arrow selects words");
    field.setValue("hello 👩‍💻");
    key(XKB_KEY_BackSpace);
    require(field.value() == "hello ", "Native composer deletes complete graphemes and joined emoji");
    field.setMultiline(true);
    require(field.textInputState().multiline, "Multiline input-method state advertises line-break support");
    field.setSubmitOnEnter(true);
    field.setValue("first");
    int submitted = 0;
    field.setOnSubmit([&](const std::string&) { ++submitted; });
    key(XKB_KEY_Return, KeyMod::Shift);
    require(field.value() == "first\n" && submitted == 0, "Shift+Enter inserts a line in chat mode");
    key(XKB_KEY_Return);
    require(submitted == 1 && field.value() == "first\n", "Enter submits a multiline message once");
    {
      InputDispatcher focus;
      Input ime;
      std::vector<std::string> changes, submissions;
      int shortcutCalls = 0;
      ime.setOnChange([&](const std::string& value) { changes.push_back(value); });
      ime.setOnSubmit([&](const std::string& value) { submissions.push_back(value); });
      ime.setOnKeyEvent([&](std::uint32_t, std::uint32_t) {
        ++shortcutCalls;
        return false;
      });
      const auto imeKey = [&](std::uint32_t symbol, std::uint32_t modifiers = 0, std::uint32_t unicode = 0,
                              bool composing = false) {
        ime.inputArea()->dispatchKey(symbol, unicode, modifiers, true, composing);
      };
      TextInputEdit compose;
      compose.hasPreedit = true;
      compose.preeditText = "as";
      compose.preeditCursorBegin = compose.preeditCursorEnd = 2;
      ime.setValue("base ");
      ime.textInputApplyEdit(compose);
      require(
          ime.hasPreedit()
              && ime.value() == "base "
              && ime.displayValue() == "base as"
              && ime.textInputState().surroundingText == "base "
              && changes.empty()
              && submissions.empty(),
          "Preedit is visible without changing committed drafts, surrounding text or callbacks"
      );
      compose.preeditText = "á";
      ime.textInputApplyEdit(compose);
      require(
          ime.value() == "base " && ime.displayValue() == "base á" && changes.empty(),
          "Replacing a Telex-style preedit never publishes intermediate text"
      );
      imeKey(XKB_KEY_Return);
      require(
          ime.hasPreedit() && shortcutCalls == 0 && submissions.empty(),
          "Return during composition cannot trigger a field shortcut or submit action"
      );
      TextInputEdit commit;
      commit.hasCommitText = true;
      commit.commitText = "á";
      ime.textInputApplyEdit(commit);
      require(
          !ime.hasPreedit() && ime.value() == "base á" && changes == std::vector<std::string>{"base á"},
          "A completed composition publishes exactly one committed change"
      );
      imeKey(XKB_KEY_z, KeyMod::Ctrl);
      require(ime.value() == "base ", "Undo of an IME commit restores committed text without resurrecting preedit");
      imeKey(XKB_KEY_z, KeyMod::Ctrl | KeyMod::Shift);
      require(ime.value() == "base á", "Redo restores the committed composition as one edit");
      imeKey(XKB_KEY_Return);
      require(submissions == std::vector<std::string>{"base á"}, "A forwarded Return after commit submits normally");

      ime.setValue("selected 日本");
      ime.selectAll();
      changes.clear();
      ime.textInputApplyEdit(compose);
      require(
          ime.value() == "selected 日本" && ime.displayValue() == "á" && ime.textInputState().surroundingText.empty(),
          "Composition replaces the displayed selection while committed text remains available to drafts"
      );
      ime.textInputResetPreedit();
      require(
          !ime.hasPreedit()
              && ime.value() == "selected 日本"
              && changes.empty()
              && ime.textInputState().cursor == static_cast<std::int32_t>(ime.value().size())
              && ime.textInputState().anchor == 0,
          "Cancelling composition restores the original selection without a committed change"
      );
      ime.textInputApplyEdit(compose);
      ime.textInputApplyEdit(commit);
      require(ime.value() == "á", "Committing composition replaces the original selection once");
      imeKey(XKB_KEY_z, KeyMod::Ctrl);
      require(
          ime.value() == "selected 日本" && ime.textInputState().anchor == 0,
          "Undo after replacing a selection restores its original text and selection"
      );

      changes.clear();
      submissions.clear();
      ime.setValue("A");
      TextInputEdit next = compose;
      next.hasCommitText = true;
      next.commitText = "B";
      next.submit = true;
      ime.textInputApplyEdit(next);
      require(
          ime.value() == "AB"
              && ime.displayValue() == "ABá"
              && changes == std::vector<std::string>{"AB"}
              && submissions.empty(),
          "A commit with another preedit publishes committed text and suppresses premature submit"
      );
      TextInputEdit action;
      action.submit = true;
      ime.textInputApplyEdit(action);
      require(
          ime.value() == "AB" && !ime.hasPreedit() && submissions.empty(),
          "An action without a composition commit cannot submit transient text"
      );
      ime.textInputApplyEdit(compose);
      commit.submit = true;
      ime.textInputApplyEdit(commit);
      require(
          ime.value() == "ABá" && submissions == std::vector<std::string>{"ABá"},
          "A final commit and submit action use only committed text"
      );
      commit.submit = false;

      ime.setValue("aé日本z");
      imeKey(XKB_KEY_Home);
      imeKey(XKB_KEY_Right);
      imeKey(XKB_KEY_Right);
      TextInputEdit erase;
      erase.hasDelete = true;
      erase.deleteBeforeLength = 2;
      erase.deleteAfterLength = 3;
      ime.textInputApplyEdit(erase);
      require(ime.value() == "a本z", "Surrounding deletion counts UTF-8 bytes on both sides of the cursor");
      imeKey(XKB_KEY_z, KeyMod::Ctrl);
      require(ime.value() == "aé日本z", "A surrounding deletion remains one reversible edit");
      ime.setValue("aé日z");
      imeKey(XKB_KEY_Home);
      imeKey(XKB_KEY_Right);
      imeKey(XKB_KEY_Right, KeyMod::Shift);
      erase.hasCommitText = true;
      erase.commitText = "á";
      erase.deleteBeforeLength = 1;
      ime.textInputApplyEdit(erase);
      require(ime.value() == "áz", "Surrounding deletion excludes the selection before the commit replaces it");

      ime.setValue("committed");
      changes.clear();
      ime.textInputApplyEdit(compose);
      ime.setEnabled(false);
      ime.textInputApplyEdit(commit);
      require(
          !ime.hasPreedit() && ime.value() == "committed" && changes.empty(),
          "Disabling an input cancels composition and rejects late commits"
      );
      ime.setEnabled(true);
      TextInputEdit invalid = commit;
      invalid.commitText = std::string(1, static_cast<char>(0xFF));
      ime.textInputApplyEdit(invalid);
      require(
          ime.value() == "committed" && changes.empty(), "Malformed UTF-8 input-method edits are rejected atomically"
      );

      ime.selectAll();
      imeKey(XKB_KEY_acute, 0, U'´', true);
      require(
          ime.value() == "committed" && ime.displayValue() == "´" && changes.empty(),
          "Native dead-key previews preserve the same committed-value contract"
      );
      ime.textInputResetPreedit();
      require(
          ime.value() == "committed" && ime.textInputState().anchor == 0,
          "Cancelling a native dead-key preview restores its selection"
      );
      ime.setValue("cursor");
      ime.textInputApplyEdit(compose);
      imeKey(XKB_KEY_Left);
      require(
          !ime.hasPreedit() && ime.value() == "cursor" && changes.empty(),
          "Caret movement that cancels preedit does not emit a draft or search change"
      );
      ime.setValue("committed");

      focus.setSceneRoot(&ime);
      focus.setFocus(ime.inputArea());
      ime.textInputApplyEdit(compose);
      focus.setFocus(nullptr);
      require(
          !ime.hasPreedit() && ime.value() == "committed" && changes.empty(),
          "Native focus loss cancels preedit without publishing or submitting it"
      );
    }
    field.setMultiline(false);
    field.setPasswordMode(true);
    field.setValue("synthetic-secret");
    clipboard.text = "previous";
    key(XKB_KEY_a, KeyMod::Ctrl);
    key(XKB_KEY_c, KeyMod::Ctrl);
    require(clipboard.text == "previous", "Password inputs do not expose their selection through copy");
    const auto privateState = field.textInputState();
    require(
        privateState.surroundingText.empty()
            && privateState.cursor == 0
            && privateState.anchor == 0
            && !privateState.sendSurroundingText
            && privateState.sensitiveData
            && privateState.hiddenText
            && !privateState.preeditVisible
            && !privateState.multiline
            && privateState.purpose == TextInputPurpose::Password,
        "Password input-method state never exposes text, selection offsets or visible composition hints"
    );
    Input::setTextClipboard(nullptr);
    KeybindMatcher::setMatcher(KeybindAction::Validate, [](std::uint32_t key, std::uint32_t modifiers) {
      return key == XKB_KEY_space && modifiers == 0;
    });
    Toggle toggle;
    bool changed = false;
    toggle.setOnChange([&](bool value) { changed = value; });
    auto* area = dynamic_cast<InputArea*>(toggle.children().back().get());
    require(area != nullptr, "Toggle owns a native input area");
    area->dispatchKey(XKB_KEY_space, ' ', 0, true);
    require(changed && toggle.checked(), "Toggle supports keyboard activation");
    toggle.setEnabled(false);
    changed = false;
    area->dispatchKey(XKB_KEY_space, ' ', 0, true);
    require(!changed && toggle.checked(), "Disabled account toggles cannot mutate values");
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
