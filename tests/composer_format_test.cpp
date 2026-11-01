#include "app/composer_format.h"

#include <iostream>
#include <stdexcept>

int main() {
  const auto require = [](bool value, const char* reason) {
    if (!value)
      throw std::runtime_error(reason);
  };
  try {
    const std::string text = "héllo <#123> 👋";
    for (const auto& info : app::composerFormats) {
      if (info.format == app::ComposerFormat::Link)
        continue;
      const auto edit = app::formatComposer(text, 0, text.size(), info.format);
      require(edit.has_value() && edit->text.contains("<#123>"), "Formatting retains protocol identities");
      const auto restored = app::formatComposer(edit->text, edit->anchor, edit->cursor, info.format);
      require(restored && restored->text == text, "A second activation removes the format around the selection");
      const auto reversed = app::formatComposer(text, text.size(), 0, info.format);
      require(
          reversed
              && reversed->text == edit->text
              && reversed->anchor == edit->cursor
              && reversed->cursor == edit->anchor,
          "Formatting retains reverse selections"
      );
    }
    const auto italic = app::formatComposer("**word**", 2, 6, app::ComposerFormat::Italic);
    require(italic && italic->text == "***word***", "Italic nests inside bold instead of stripping one star");
    const auto unitalic =
        app::formatComposer(italic->text, italic->anchor, italic->cursor, app::ComposerFormat::Italic);
    require(unitalic && unitalic->text == "**word**", "Italic toggles off without removing surrounding bold");
    const auto quote = app::formatComposer("first line\nsecond line\ntail", 3, 17, app::ComposerFormat::Quote);
    require(
        quote && quote->text == "> first line\n> second line\ntail",
        "Quotes expand partial selections to complete lines"
    );
    const auto token = app::formatComposer("a <#123> z", 4, 6, app::ComposerFormat::Bold);
    require(token && token->text == "a **<#123>** z", "Partial protocol selections expand to the complete token");
    const auto code = app::formatComposer("`x`", 0, 3, app::ComposerFormat::Code);
    require(code && code->text == "`` `x` ``", "Inline code uses a longer padded delimiter for embedded backticks");
    const std::string block = "first\n```\nsecond";
    const auto fenced = app::formatComposer(block, 0, block.size(), app::ComposerFormat::Code);
    require(fenced && fenced->text == "````\n" + block + "\n````", "Code fences cannot terminate inside the selection");
    const auto unfenced = app::formatComposer(fenced->text, fenced->anchor, fenced->cursor, app::ComposerFormat::Code);
    require(unfenced && unfenced->text == block, "Multiline code toggles without losing line breaks");
    require(!app::formatComposer(text, 0, 0, app::ComposerFormat::Bold), "Empty selections do not insert stray markup");
    const auto large = std::string(app::maximumComposerBytes, 'x');
    require(
        !app::formatComposer(large, 0, large.size(), app::ComposerFormat::Bold),
        "Formatting honors the draft byte limit"
    );
    const auto link = app::linkComposer("before label after", 7, 12, "my [label]", "https://example.com/path(a)");
    require(
        link && link->text == "before [my \\[label\\]](https://example.com/path%28a%29) after",
        "Links escape their label and destination without changing surrounding text"
    );
    require(
        app::linkComposer("", 0, 0, "Project", "https://example.com").has_value(),
        "Links can be inserted at an empty caret"
    );
    require(
        !app::linkComposer(text, 0, text.size(), "Project", "javascript:alert(1)"), "Links accept web destinations only"
    );
    require(
        !app::composerWebUrl("https:///missing") && !app::composerWebUrl("https://example.com/a b"),
        "Links require a host and a valid single-line destination"
    );
    require(!app::linkComposer(text, 0, 0, "", "https://example.com"), "Links require display text");
    std::cout << "Composer formatting, Unicode selections, protected tokens and limits passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
