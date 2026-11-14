#pragma once

#include "ui/controls/input.h"

#include <span>

namespace app {
  enum ComposerInk : unsigned {
    ComposerPlain = 0,
    ComposerBold = 1 << 0,
    ComposerItalic = 1 << 1,
    ComposerUnderline = 1 << 2,
    ComposerStrike = 1 << 3,
    ComposerCode = 1 << 4,
    ComposerSpoiler = 1 << 5,
    ComposerMarker = 1 << 6,
    ComposerLink = 1 << 7,
    ComposerQuote = 1 << 8,
  };

  // Styles address source bytes. Rendering projects them through token replacements.
  std::vector<unsigned> composerPreviewStyles(std::string_view source);
  std::vector<TextReplacement> composerQuoteReplacements(std::string_view source);
  std::string composerPreviewMarkup(
      std::string_view source, const TextPresentation& presentation, std::span<const unsigned> styles,
      std::size_t begin, std::size_t end
  );
  std::string composerPreviewMarkup(std::string_view source, const TextPresentation& presentation);

  class ChatInput : public Input {
  public:
    ChatInput();
    bool completeTrademark(std::string_view inserted = ":");
    void textInputApplyEdit(const TextInputEdit& edit) override;
  };
} // namespace app
