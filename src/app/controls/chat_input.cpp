#include "app/controls/chat_input.h"

#include "app/composer_format.h"
#include "app/theme.h"

#include <glib.h>
#include <map>
#include <md4c.h>
#include <numeric>

namespace app {
  namespace {
    constexpr std::string_view customMarker = "\uE000";
    constexpr std::size_t maximumQuoteIndent = 3;
    constexpr std::size_t multilineQuoteMarkerLength = 3;
    constexpr std::size_t customDelimiterLength = 2;
    struct PreviewParser {
      std::string_view input;
      std::vector<std::size_t> sourceOffsets;
      std::map<std::size_t, unsigned> markers;
      std::vector<unsigned> styles, scopes{ComposerPlain};
      unsigned custom = ComposerPlain;
      bool code = false;
      unsigned quoteDepth = 0;

      std::optional<std::size_t> offset(const char* text, std::size_t size) const {
        const auto address = reinterpret_cast<std::uintptr_t>(text);
        const auto start = reinterpret_cast<std::uintptr_t>(input.data());
        if (address < start || address - start > input.size() || size > input.size() - (address - start))
          return {};
        return address - start;
      }
      void paint(std::size_t begin, std::size_t end, unsigned ink) {
        std::fill(styles.begin() + sourceOffsets[begin], styles.begin() + sourceOffsets[end], ink);
      }
    };

    int enterBlock(MD_BLOCKTYPE type, void*, void* context) {
      auto& parser = *static_cast<PreviewParser*>(context);
      if (type == MD_BLOCK_CODE)
        parser.code = true;
      if (type == MD_BLOCK_QUOTE)
        ++parser.quoteDepth;
      return 0;
    }
    int leaveBlock(MD_BLOCKTYPE type, void*, void* context) {
      auto& parser = *static_cast<PreviewParser*>(context);
      if (type == MD_BLOCK_CODE)
        parser.code = false;
      if (type == MD_BLOCK_QUOTE)
        --parser.quoteDepth;
      return 0;
    }
    int enterSpan(MD_SPANTYPE type, void*, void* context) {
      auto& parser = *static_cast<PreviewParser*>(context);
      auto ink = parser.scopes.back();
      switch (type) {
      case MD_SPAN_STRONG:
        ink |= ComposerBold;
        break;
      case MD_SPAN_EM:
        ink |= ComposerItalic;
        break;
      case MD_SPAN_DEL:
        ink |= ComposerStrike;
        break;
      case MD_SPAN_CODE:
        ink = ComposerCode;
        break;
      case MD_SPAN_A:
        ink |= ComposerLink;
        break;
      default:
        break;
      }
      parser.scopes.push_back(ink);
      return 0;
    }
    int leaveSpan(MD_SPANTYPE, void*, void* context) {
      static_cast<PreviewParser*>(context)->scopes.pop_back();
      return 0;
    }
    int text(MD_TEXTTYPE type, const MD_CHAR* value, MD_SIZE size, void* context) {
      auto& parser = *static_cast<PreviewParser*>(context);
      const auto begin = parser.offset(value, size);
      if (!begin)
        return 0;
      const bool code = parser.code || type == MD_TEXT_CODE || (parser.scopes.back() & ComposerCode);
      auto at = *begin;
      while (at < *begin + size) {
        if (!code)
          if (const auto marker = parser.markers.find(at); marker != parser.markers.end()) {
            parser.paint(at, at + customMarker.size(), ComposerMarker);
            parser.custom ^= marker->second;
            at += customMarker.size();
            continue;
          }
        auto end = at + 1;
        while (end < *begin + size && (code || !parser.markers.contains(end)))
          ++end;
        parser.paint(
            at, end,
            (code ? ComposerCode : parser.scopes.back() | parser.custom)
                | (parser.quoteDepth ? ComposerQuote : ComposerPlain)
        );
        at = end;
      }
      return 0;
    }
    void parse(PreviewParser& state) {
      const MD_PARSER parser{
          .abi_version = 0,
          .flags = MD_FLAG_NOHTML | MD_FLAG_NOINDENTEDCODEBLOCKS | MD_FLAG_STRIKETHROUGH,
          .enter_block = enterBlock,
          .leave_block = leaveBlock,
          .enter_span = enterSpan,
          .leave_span = leaveSpan,
          .text = text
      };
      md_parse(state.input.data(), state.input.size(), &parser, &state);
    }
    PreviewParser plainParser(std::string_view source) {
      PreviewParser parser;
      parser.input = source;
      parser.styles.resize(source.size(), ComposerMarker);
      parser.sourceOffsets.resize(source.size() + 1);
      std::iota(parser.sourceOffsets.begin(), parser.sourceOffsets.end(), 0);
      parse(parser);
      return parser;
    }
    std::string escaped(std::string_view value) {
      auto* markup = g_markup_escape_text(value.data(), value.size());
      std::string result(markup);
      g_free(markup);
      return result;
    }
    std::string inkAttributes(unsigned ink) {
      std::string result;
      if (ink & ComposerBold)
        result += " weight=\"bold\"";
      if (ink & ComposerItalic)
        result += " style=\"italic\"";
      if (ink & ComposerUnderline)
        result += " underline=\"single\"";
      if (ink & ComposerStrike)
        result += " strikethrough=\"true\"";
      if (ink & ComposerCode)
        result += " font_family=\"monospace\"";
      if (ink & ComposerMarker)
        result += " foreground=\"" + formatRgbHex(theme::muted) + "\"";
      else if (ink & ComposerLink)
        result += " foreground=\"" + formatRgbHex(theme::accent) + "\"";
      if (ink & (ComposerCode | ComposerSpoiler))
        result += " background=\"" + formatRgbHex(theme::raised) + "\"";
      return result;
    }
  } // namespace

  std::vector<unsigned> composerPreviewStyles(std::string_view source) {
    if (source.size() > maximumComposerBytes)
      return std::vector<unsigned>(source.size(), ComposerPlain);
    const auto literal = plainParser(source);
    std::vector<bool> tokenBytes(source.size());
    for (const auto& token : composerTokens(source))
      std::fill(tokenBytes.begin() + token.begin, tokenBytes.begin() + token.end, true);
    std::map<std::size_t, unsigned> pairs;
    for (const auto& [delimiter, ink] : {std::pair{"__", ComposerUnderline}, std::pair{"||", ComposerSpoiler}}) {
      std::optional<std::size_t> opening;
      for (std::size_t at = 0; at < source.size();) {
        if (source[at] == '\\') {
          at += std::min<std::size_t>(2, source.size() - at);
          continue;
        }
        if (!tokenBytes[at] && !(literal.styles[at] & ComposerCode) && source.substr(at).starts_with(delimiter)) {
          if (opening) {
            pairs[*opening] = ink;
            pairs[at] = ink;
            opening.reset();
          } else
            opening = at;
          at += std::string_view(delimiter).size();
        } else
          ++at;
      }
    }
    PreviewParser parser;
    std::string prepared;
    parser.styles.resize(source.size(), ComposerMarker);
    for (std::size_t at = 0; at < source.size();) {
      if (const auto marker = pairs.find(at); marker != pairs.end()) {
        parser.markers[prepared.size()] = marker->second;
        prepared += customMarker;
        parser.sourceOffsets.insert(parser.sourceOffsets.end(), customMarker.size(), at);
        at += customDelimiterLength;
      } else {
        parser.sourceOffsets.push_back(at);
        prepared += tokenBytes[at] ? 'x' : source[at];
        ++at;
      }
    }
    parser.sourceOffsets.push_back(source.size());
    parser.input = prepared;
    parse(parser);
    return parser.styles;
  }

  std::vector<TextReplacement> composerQuoteReplacements(std::string_view source) {
    std::vector<TextReplacement> result;
    if (source.empty() || source.size() > maximumComposerBytes || !source.contains('>'))
      return result;
    const auto parser = plainParser(source);
    bool multilineQuote = false;
    for (std::size_t at = 0; at < source.size();) {
      auto marker = at;
      while (marker < source.size() && source[marker] == ' ' && marker - at < maximumQuoteIndent)
        ++marker;
      if (!multilineQuote && source.substr(marker).starts_with(">>> ") && !(parser.styles[marker] & ComposerCode)) {
        result.push_back({marker, marker + multilineQuoteMarkerLength, "▎"});
        multilineQuote = true;
      }
      while (!multilineQuote
             && marker < source.size()
             && source[marker] == '>'
             && !(parser.styles[marker] & ComposerCode)) {
        const auto end = marker + 1;
        if (end < source.size() && source[end] != ' ' && source[end] != '\t' && source[end] != '\n')
          break;
        result.push_back({marker, end, "▎"});
        marker = end;
        while (marker < source.size() && (source[marker] == ' ' || source[marker] == '\t'))
          ++marker;
      }
      const auto next = source.find('\n', at);
      if (next == std::string_view::npos)
        break;
      if (multilineQuote)
        result.push_back({next, next + 1, "\n▎ "});
      at = next + 1;
    }
    return result;
  }

  std::string composerPreviewMarkup(
      std::string_view source, const TextPresentation& presentation, std::span<const unsigned> styles,
      std::size_t begin, std::size_t end
  ) {
    std::string result;
    for (std::size_t at = 0; at < source.size();) {
      const auto ink = at < styles.size() ? styles[at] : ComposerPlain;
      auto next = at + 1;
      if (const auto* token = presentation.replacementAt(at))
        next = token->end;
      else
        while (next < source.size() && next < styles.size() && styles[next] == ink && !presentation.replacementAt(next))
          ++next;
      const auto first = std::max(begin, presentation.displayOffset(at));
      const auto last = std::min(end, presentation.displayOffset(next));
      if (first < last) {
        const auto attributes = inkAttributes(ink);
        if (!attributes.empty())
          result += "<span" + attributes + ">";
        result += escaped(std::string_view(presentation.text()).substr(first, last - first));
        if (!attributes.empty())
          result += "</span>";
      }
      at = next;
    }
    return result;
  }
  std::string composerPreviewMarkup(std::string_view source, const TextPresentation& presentation) {
    return composerPreviewMarkup(source, presentation, composerPreviewStyles(source), 0, presentation.text().size());
  }
  ChatInput::ChatInput() {
    setTextPresenter(composerQuoteReplacements);
    setMarkupPresenter([styledSource = std::string{}, styles = std::vector<unsigned>{}](
                           std::string_view source, const TextPresentation& presentation, std::size_t begin,
                           std::size_t end
                       ) mutable {
      if (source != styledSource) {
        styledSource = source;
        styles = composerPreviewStyles(source);
      }
      return composerPreviewMarkup(source, presentation, styles, begin, end);
    });
  }
} // namespace app
