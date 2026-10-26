#include "app/brand_logo.h"
#include "app/member_list_layout.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/keybind_matcher.h"
#include "core/text_clipboard.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/text/glyph_registry.h"
#include "ui/controls/input.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include "ui/controls/toggle.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

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
         {"message-circle", "settings", "users", "clipboard-plus", "mood-smile", "x", "minus", "square", "squares",
          "chevron-left", "chevron-right", "chevron-down", "zoom-in", "zoom-out", "loader"})
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
