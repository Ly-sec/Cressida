#include "app/controls/chat_input.h"
#include "app/theme.h"

#include <iostream>
#include <pango/pango.h>
#include <stdexcept>

namespace {
  void require(bool value, const char* reason) {
    if (!value)
      throw std::runtime_error(reason);
  }
  std::string plain(std::string_view markup) {
    char* text = nullptr;
    GError* error = nullptr;
    if (!pango_parse_markup(markup.data(), markup.size(), 0, nullptr, &text, nullptr, &error)) {
      std::string reason = error->message;
      g_error_free(error);
      throw std::runtime_error(reason);
    }
    std::string result(text);
    g_free(text);
    return result;
  }
  unsigned style(std::string_view source, std::string_view word) {
    const auto at = source.find(word);
    require(at != std::string_view::npos, "The fixture contains its test word");
    return app::composerPreviewStyles(source)[at];
  }
} // namespace

int main() {
  using namespace app;
  try {
    theme::select("dark");
    const std::string draft = "_italic_ **bold** __under__ ~~strike~~ ||spoiler|| `code`";
    for (const auto& [word, ink] :
         {std::pair{"italic", ComposerItalic},
          {"bold", ComposerBold},
          {"under", ComposerUnderline},
          {"strike", ComposerStrike},
          {"spoiler", ComposerSpoiler},
          {"code", ComposerCode}})
      require(style(draft, word) & ink, "Completed markup styles its content");
    require(style(draft, "**") == ComposerMarker, "Formatting markers remain separate from styled content");
    require(
        (style("__**nested**__", "nested") & (ComposerUnderline | ComposerBold)) == (ComposerUnderline | ComposerBold),
        "Underline composes with nested bold"
    );
    require(
        (style("***both***", "both") & (ComposerItalic | ComposerBold)) == (ComposerItalic | ComposerBold),
        "Nested emphasis combines fonts"
    );
    for (const auto& source : {"\\*literal*", "unfinished **bold", "snake_case_name"})
      require(
          !(style(source, source == std::string_view("\\*literal*") ? "literal" : source)
            & (ComposerBold | ComposerItalic)),
          "Escaped and incomplete markup stays literal"
      );
    require(style("`**plain** __raw__ ||shown||`", "plain") == ComposerCode, "Inline code suppresses other formatting");
    require(
        style("```\n> __raw__ **plain**\n```", "raw") == ComposerCode,
        "Fenced code suppresses custom delimiters and quotes"
    );
    require(style("> **quoted**", "quoted") & ComposerQuote, "Quoted text retains nested formatting");
    require(composerQuoteReplacements("```\n> literal\n```").empty(), "Quote-like code is never replaced");
    require(composerQuoteReplacements("\\> literal").empty(), "Escaped quote markers stay literal");
    const std::string quote = "> **quoted**\n> second";
    const TextPresentation quoted(quote, composerQuoteReplacements(quote));
    require(quoted.text() == "▎ **quoted**\n▎ second", "Quote drafts show a bar instead of their source marker");
    require(
        plain(composerPreviewMarkup(quote, quoted)) == quoted.text(), "Quote rendering preserves its displayed content"
    );
    const std::string multiline = ">>> **first**\nsecond\n\nlast";
    require(
        TextPresentation(multiline, composerQuoteReplacements(multiline)).text() == "▎ **first**\n▎ second\n▎ \n▎ last",
        "Multiline quotes show bars through the rest of the draft, including empty lines"
    );
    require(
        !(style("<:a__b:123> plain <:c__d:456>", "plain") & ComposerUnderline),
        "Custom emoji names cannot become Markdown delimiters"
    );
    require(!(style("unfinished **bold", "bold") & ComposerBold), "Unfinished bold stays editable literal text");
    const std::string tokens = "**<@123>** ||<:wave:456>|| <span>&amp;</span>";
    const TextPresentation projected(tokens, {{2, 8, "@A<&B"}, {13, 24, "👋"}});
    const auto styles = composerPreviewStyles(tokens);
    require(
        plain(composerPreviewMarkup(tokens, projected, styles, 0, projected.text().size())) == projected.text(),
        "Mention labels, emoji and markup-like text are escaped without changing the display"
    );
    const auto start = projected.displayOffset(2), end = projected.displayOffset(8);
    require(
        plain(composerPreviewMarkup(tokens, projected, styles, start, end)) == "@A<&B",
        "Clipped styled slices retain complete tokens"
    );
    for (const auto* mode : {"dark", "light"}) {
      theme::select(mode);
      require(
          plain(composerPreviewMarkup(draft, TextPresentation(draft, {}))) == draft,
          "Both themes retain every editable delimiter"
      );
    }
    const std::string untrusted = "\uE000 **bold** & <i>raw</i>";
    require(
        plain(composerPreviewMarkup(untrusted, TextPresentation(untrusted, {}))) == untrusted,
        "User-entered private-use characters cannot become formatting controls"
    );
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
