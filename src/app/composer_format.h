#pragma once

#include "app/composer_tokens.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace app {
  enum class ComposerFormat { Bold, Italic, Underline, Strike, Quote, Code, Spoiler, Link };
  struct ComposerFormatInfo {
    ComposerFormat format;
    std::string_view label, glyph, marker;
  };
  inline constexpr std::array composerFormats{
      ComposerFormatInfo{ComposerFormat::Bold, "Bold", "bold", "**"},
      ComposerFormatInfo{ComposerFormat::Italic, "Italic", "italic", "*"},
      ComposerFormatInfo{ComposerFormat::Underline, "Underline", "underline", "__"},
      ComposerFormatInfo{ComposerFormat::Strike, "Strikethrough", "strikethrough", "~~"},
      ComposerFormatInfo{ComposerFormat::Quote, "Quote", "blockquote", ""},
      ComposerFormatInfo{ComposerFormat::Code, "Code", "code", ""},
      ComposerFormatInfo{ComposerFormat::Spoiler, "Spoiler", "eye-off", "||"},
      ComposerFormatInfo{ComposerFormat::Link, "Insert Link", "link", ""},
  };
  inline constexpr std::size_t maximumComposerBytes = 8000;
  inline constexpr std::size_t minimumCodeFence = 3;
  struct FormattedComposer {
    std::string text;
    std::size_t anchor, cursor;
    bool removed = false;
  };

  inline std::optional<FormattedComposer>
  formatComposer(std::string_view text, std::size_t anchor, std::size_t cursor, ComposerFormat format) {
    if (format == ComposerFormat::Link || anchor > text.size() || cursor > text.size() || anchor == cursor)
      return {};
    const bool reversed = cursor < anchor;
    auto begin = std::min(anchor, cursor), end = std::max(anchor, cursor);
    for (const auto& token : composerTokens(text)) {
      if (begin > token.begin && begin < token.end)
        begin = token.begin;
      if (end > token.begin && end < token.end)
        end = token.end;
    }
    std::string replacement;
    std::size_t selectedBegin = 0, selectedEnd = 0;
    bool removed = false;
    if (format == ComposerFormat::Quote) {
      const auto before = begin == 0 ? std::string_view::npos : text.rfind('\n', begin - 1);
      begin = before == std::string_view::npos ? 0 : before + 1;
      const auto after = text.find('\n', end - 1);
      end = after == std::string_view::npos ? text.size() : after;
      const auto block = text.substr(begin, end - begin);
      removed = true;
      for (std::size_t at = 0; at <= block.size();) {
        if (!block.substr(at).starts_with("> "))
          removed = false;
        const auto next = block.find('\n', at);
        if (next == std::string_view::npos)
          break;
        at = next + 1;
      }
      for (std::size_t at = 0; at <= block.size();) {
        const auto next = block.find('\n', at);
        const auto line = block.substr(at, next == std::string_view::npos ? block.size() - at : next - at);
        replacement += removed ? std::string(line.substr(2)) : "> " + std::string(line);
        if (next == std::string_view::npos)
          break;
        replacement += '\n';
        at = next + 1;
      }
      selectedEnd = replacement.size();
    } else {
      const auto selected = text.substr(begin, end - begin);
      std::string prefix, suffix;
      if (format == ComposerFormat::Code) {
        std::size_t longest = 0, run = 0;
        for (const auto c : selected) {
          run = c == '`' ? run + 1 : 0;
          longest = std::max(longest, run);
        }
        const bool multiline = selected.contains('\n');
        const auto fence = std::string(std::max(multiline ? minimumCodeFence : std::size_t{1}, longest + 1), '`');
        const bool padded = !multiline && (selected.starts_with('`') || selected.ends_with('`'));
        prefix = fence + (multiline ? "\n" : padded ? " " : "");
        suffix = (multiline ? "\n" : padded ? " " : "") + fence;
      } else {
        const auto info = std::ranges::find(composerFormats, format, &ComposerFormatInfo::format);
        if (info == composerFormats.end())
          return {};
        prefix = suffix = info->marker;
      }
      const auto starRun = [](std::string_view value, bool fromEnd) {
        std::size_t count = 0;
        while (count < value.size() && value[fromEnd ? value.size() - count - 1 : count] == '*')
          ++count;
        return count;
      };
      const bool adjacentItalic = format != ComposerFormat::Italic
          || (starRun(text.substr(0, begin), true) % 2 && starRun(text.substr(end), false) % 2);
      const bool containedItalic =
          format != ComposerFormat::Italic || (starRun(selected, false) % 2 && starRun(selected, true) % 2);
      // Keep the content selected so another click can remove its surrounding markers.
      if (adjacentItalic
          && begin >= prefix.size()
          && text.substr(begin - prefix.size(), prefix.size()) == prefix
          && text.substr(end, suffix.size()) == suffix) {
        begin -= prefix.size();
        end += suffix.size();
        replacement = selected;
        selectedEnd = replacement.size();
        removed = true;
      } else if (
          containedItalic
          && format != ComposerFormat::Code
          && selected.size() >= prefix.size() + suffix.size()
          && selected.starts_with(prefix)
          && selected.ends_with(suffix)
      ) {
        replacement = selected.substr(prefix.size(), selected.size() - prefix.size() - suffix.size());
        selectedEnd = replacement.size();
        removed = true;
      } else {
        replacement = prefix + std::string(selected) + suffix;
        selectedBegin = prefix.size();
        selectedEnd = selectedBegin + selected.size();
      }
    }
    if (text.size() - (end - begin) + replacement.size() > maximumComposerBytes)
      return {};
    FormattedComposer result{std::string(text), begin + selectedBegin, begin + selectedEnd, removed};
    result.text.replace(begin, end - begin, replacement);
    if (reversed)
      std::swap(result.anchor, result.cursor);
    return result;
  }
  inline constexpr std::size_t maximumComposerLinkUrlBytes = 2048;
  inline bool composerWebUrl(std::string_view url) {
    const auto prefix = url.starts_with("https://") ? std::string_view("https://")
        : url.starts_with("http://")                ? std::string_view("http://")
                                                    : std::string_view{};
    if (prefix.empty() || url.size() > maximumComposerLinkUrlBytes || std::ranges::any_of(url, [](unsigned char c) {
          return c <= ' ' || c == 127 || c == '\\';
        }))
      return false;
    const auto authority = url.substr(prefix.size(), url.find_first_of("/?#", prefix.size()) - prefix.size());
    return !authority.empty() && !authority.contains('@') && !authority.contains('<') && !authority.contains('>');
  }

  inline std::optional<FormattedComposer> linkComposer(
      std::string_view text, std::size_t anchor, std::size_t cursor, std::string_view label, std::string_view url
  ) {
    if (anchor > text.size()
        || cursor > text.size()
        || !composerWebUrl(url)
        || label.empty()
        || label.find_first_not_of(" \t\r\n") == std::string_view::npos
        || std::ranges::any_of(label, [](unsigned char c) { return c < ' ' || c == 127; }))
      return {};
    std::string caption, destination;
    for (const auto c : label) {
      if (std::string_view("\\[]*_~`<>|").contains(c))
        caption += '\\';
      caption += c;
    }
    for (const auto c : url) {
      switch (c) {
      case '(':
        destination += "%28";
        break;
      case ')':
        destination += "%29";
        break;
      case '<':
        destination += "%3C";
        break;
      case '>':
        destination += "%3E";
        break;
      case '"':
        destination += "%22";
        break;
      default:
        destination += c;
        break;
      }
    }
    const auto begin = std::min(anchor, cursor), end = std::max(anchor, cursor);
    const auto markup = "[" + caption + "](" + destination + ")";
    if (text.size() - (end - begin) + markup.size() > maximumComposerBytes)
      return {};
    FormattedComposer result{std::string(text), begin + 1, begin + 1 + caption.size()};
    result.text.replace(begin, end - begin, markup);
    if (cursor < anchor)
      std::swap(result.anchor, result.cursor);
    return result;
  }
} // namespace app
