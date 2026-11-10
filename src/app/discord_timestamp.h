#pragma once
#include "app/message_time.h"

#include <charconv>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace app {
  struct TimestampMention {
    static constexpr std::int64_t minimumSeconds = -62135596800LL, maximumSeconds = 253402300799LL;
    std::int64_t seconds;
    char style;
  };
  inline std::optional<TimestampMention> parseTimestampMention(std::string_view token) {
    constexpr std::size_t maximumTokenBytes = 26;
    if (!token.starts_with("t:") || token.size() > maximumTokenBytes)
      return {};
    token.remove_prefix(2);
    const auto separator = token.find(':');
    const auto number = token.substr(0, separator);
    std::int64_t seconds = 0;
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), seconds);
    if (parsed.ec != std::errc{}
        || parsed.ptr != number.data() + number.size()
        || number.empty()
        || seconds < TimestampMention::minimumSeconds
        || seconds > TimestampMention::maximumSeconds)
      return {};
    const char style = separator == std::string_view::npos ? 'f' : separator + 2 == token.size() ? token.back() : '\0';
    if (!std::string_view("tTdDfFR").contains(style))
      return {};
    return TimestampMention{seconds, style};
  }
  inline std::optional<std::string>
  formatTimestampMention(const TimestampMention& timestamp, std::time_t now = std::time(nullptr)) {
    const auto [seconds, style] = timestamp;
    if (style == 'R') {
      const auto difference = seconds - static_cast<std::int64_t>(now);
      const auto distance = difference < 0 ? -difference : difference;
      std::int64_t count = distance;
      const char* unit = "second";
      for (auto [size, name] :
           {std::pair{31536000LL, "year"}, {2592000LL, "month"}, {86400LL, "day"}, {3600LL, "hour"}, {60LL, "minute"}})
        if (distance >= size) {
          count = distance / size;
          unit = name;
          break;
        }
      auto text = std::to_string(count) + " " + unit + (count == 1 ? "" : "s");
      return difference > 0 ? "in " + text : text + " ago";
    }
    const char* format = style == 't' ? "%H:%M"
        : style == 'T'                ? "%H:%M:%S"
        : style == 'd'                ? "%d/%m/%Y"
        : style == 'D'                ? "%e %B %Y"
        : style == 'f'                ? "%e %B %Y %H:%M"
        : style == 'F'                ? "%A, %e %B %Y %H:%M"
                                      : nullptr;
    if (!format)
      return {};
    const auto time = localMessageTime(std::chrono::sys_seconds{std::chrono::seconds{seconds}});
    if (!time)
      return {};
    constexpr std::size_t maximumFormattedTimeBytes = 128;
    char buffer[maximumFormattedTimeBytes];
    if (!std::strftime(buffer, sizeof(buffer), format, &*time))
      return {};
    std::string result(buffer);
    if (result.starts_with(' '))
      result.erase(0, 1);
    return result;
  }
  inline std::optional<std::string> discordTimestamp(std::string_view token, std::time_t now = std::time(nullptr)) {
    const auto timestamp = parseTimestampMention(token);
    return timestamp ? formatTimestampMention(*timestamp, now) : std::nullopt;
  }
  inline std::string timestampMentionTooltip(std::int64_t seconds, bool twelveHourClock = false) {
    if (seconds < TimestampMention::minimumSeconds || seconds > TimestampMention::maximumSeconds)
      return {};
    const auto time = localMessageTime(std::chrono::sys_seconds{std::chrono::seconds{seconds}});
    if (!time)
      return {};
    const auto format = [&](const char* pattern) -> std::string {
      constexpr std::size_t maximumFormattedTimeBytes = 128;
      char buffer[maximumFormattedTimeBytes];
      return std::strftime(buffer, sizeof(buffer), pattern, &*time) ? buffer : "";
    };
    auto clock = format(twelveHourClock ? "%I:%M %p" : "%H:%M");
    if (twelveHourClock && clock.starts_with('0'))
      clock.erase(0, 1);
    return format("%A, ") + std::to_string(time->tm_mday) + format(" %B %Y at ") + clock;
  }
} // namespace app
