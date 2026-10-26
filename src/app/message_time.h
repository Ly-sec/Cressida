#pragma once

#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace app {
  inline std::optional<std::chrono::sys_time<std::chrono::milliseconds>> messageTime(std::string_view value) {
    using namespace std::chrono;
    if (value.size() < 20
        || value.size() > 40
        || value[4] != '-'
        || value[7] != '-'
        || value[10] != 'T'
        || value[13] != ':'
        || value[16] != ':')
      return {};
    const auto number = [&](std::size_t at, std::size_t count) {
      int result = 0;
      for (std::size_t i = at; i < at + count; ++i) {
        if (value[i] < '0' || value[i] > '9')
          return -1;
        result = result * 10 + value[i] - '0';
      }
      return result;
    };
    const int y = number(0, 4), m = number(5, 2), d = number(8, 2), h = number(11, 2), minute = number(14, 2),
              second = number(17, 2);
    const year_month_day date{year{y}, month{static_cast<unsigned>(m)}, day{static_cast<unsigned>(d)}};
    if (y < 1 || !date.ok() || h < 0 || h > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
      return {};
    std::size_t at = 19;
    int fraction = 0, digits = 0;
    if (value[at] == '.') {
      ++at;
      while (at < value.size() && value[at] >= '0' && value[at] <= '9') {
        if (digits < 3)
          fraction = fraction * 10 + value[at] - '0';
        ++digits;
        ++at;
      }
      if (!digits || digits > 9)
        return {};
      for (int i = digits; i < 3; ++i)
        fraction *= 10;
    }
    int zone = 0;
    if (at + 1 == value.size() && value[at] == 'Z') {
    } else if (at + 6 == value.size() && (value[at] == '+' || value[at] == '-') && value[at + 3] == ':') {
      const int zh = number(at + 1, 2), zm = number(at + 4, 2);
      if (zh < 0 || zh > 23 || zm < 0 || zm > 59)
        return {};
      zone = (zh * 60 + zm) * (value[at] == '+' ? 1 : -1);
    } else
      return {};
    return sys_days{date} + hours{h} + minutes{minute - zone} + seconds{second} + milliseconds{fraction};
  }
  inline std::optional<std::tm> localMessageTime(std::chrono::sys_time<std::chrono::milliseconds> time) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(time);
    if (!std::in_range<std::time_t>(seconds.time_since_epoch().count()))
      return {};
    const auto value = static_cast<std::time_t>(seconds.time_since_epoch().count());
    std::tm local{};
    if (!localtime_r(&value, &local))
      return {};
    return local;
  }
  inline std::string formatMessageTime(std::string_view value, const char* format = "%H:%M") {
    const auto time = messageTime(value);
    if (!time)
      return {};
    const auto local = localMessageTime(*time);
    if (!local)
      return {};
    constexpr std::size_t maximumFormattedTimeBytes = 128;
    char buffer[maximumFormattedTimeBytes]{};
    return std::strftime(buffer, sizeof(buffer), format, &*local) ? buffer : "";
  }
  inline bool sameLocalMessageDay(
      std::chrono::sys_time<std::chrono::milliseconds> before, std::chrono::sys_time<std::chrono::milliseconds> after
  ) {
    const auto first = localMessageTime(before), second = localMessageTime(after);
    return first && second && first->tm_year == second->tm_year && first->tm_yday == second->tm_yday;
  }
} // namespace app
