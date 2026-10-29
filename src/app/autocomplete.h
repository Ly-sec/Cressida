#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace app {
  struct CompletionQuery {
    char trigger;
    std::size_t begin, end;
    std::string text;
    bool operator==(const CompletionQuery&) const = default;
  };
  inline std::optional<CompletionQuery> completionQuery(std::string_view text, std::size_t cursor, std::size_t anchor) {
    if (cursor != anchor || cursor > text.size())
      return {};
    if (cursor
        && text.front() == '/'
        && text.substr(0, cursor).find_first_of("\n\r\t") == std::string_view::npos
        && text.substr(0, cursor).find("://") == std::string_view::npos)
      return CompletionQuery{'/', 0, cursor, std::string(text.substr(1, cursor - 1))};
    std::size_t fence = 0;
    for (std::size_t i = 0; i < cursor;) {
      if (text[i] == '\\') {
        i += std::min<std::size_t>(2, cursor - i);
        continue;
      }
      if (text[i] == '`') {
        auto end = i + 1;
        while (end < cursor && text[end] == '`')
          ++end;
        if (!fence)
          fence = end - i;
        else if (end - i == fence)
          fence = 0;
        i = end;
      } else
        ++i;
    }
    if (fence)
      return {};
    auto begin = cursor;
    while (begin > 0 && cursor - begin <= 128) {
      const auto c = text[begin - 1];
      if (c == '@' || c == ':' || c == '#') {
        --begin;
        const auto prior = begin ? text[begin - 1] : ' ';
        if (prior != ' ' && prior != '\n' && prior != '\t' && prior != '(' && prior != '[' && prior != '{')
          return {};
        return CompletionQuery{c, begin, cursor, std::string(text.substr(begin + 1, cursor - begin - 1))};
      }
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '/' || c == '<' || c == '>' || c == '`')
        break;
      --begin;
    }
    return {};
  }
} // namespace app
