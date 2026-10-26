#include "app/message_group.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
  void require(bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  }
  void setTestTimezone(const char* value) {
    require(setenv("TZ", value, 1) == 0, "The test timezone can be set");
    tzset();
  }
  bool grouped(const char* before, const char* after, bool pending = false) {
    const api::Message first{"100", "10", "Alex", "First", before, "11"};
    auto next = first;
    next.id = "101";
    next.timestamp = after;
    return app::continuesMessageGroup(first, next, pending);
  }
} // namespace

int main() {
  try {
    setTestTimezone("UTC0");
    require(app::formatMessageTime("2026-10-24T09:16:12Z") == "09:16", "UTC displays without an offset");
    require(
        app::formatMessageTime("2026-10-24T14:46:12.123456+05:30") == "09:16",
        "Explicit wire offsets normalize before display"
    );
    require(
        app::formatMessageTime("1969-12-31T23:59:59.999Z") == "23:59",
        "Subsecond times before the epoch round down to the correct second"
    );
    require(
        app::formatMessageTime("0001-01-01T00:00:00Z") == "00:00"
            && app::formatMessageTime("9999-12-31T23:59:59Z") == "23:59",
        "Valid calendar extremes do not overflow a nanosecond system clock"
    );
    for (const auto* invalid :
         {"", "invalid", "2026-02-30T09:16:12Z", "2026-10-24T09:16:12", "2026-10-24T09:16:12Zjunk"})
      require(app::formatMessageTime(invalid).empty(), "Invalid times never display a misleading clock value");

    setTestTimezone("IST-5:30");
    require(
        app::formatMessageTime("2026-10-24T09:16:12.123456Z") == "14:46",
        "Half-hour system timezones apply to UTC message times"
    );
    require(
        app::formatMessageTime("2026-12-31T20:00:00Z", "%Y-%m-%d") == "2027-01-01",
        "Displayed dates follow local year boundaries"
    );
    for (const bool pending : {false, true}) {
      require(
          !grouped("2026-10-24T18:29:59Z", "2026-10-24T18:30:01Z", pending),
          "Local midnight starts a group even within the same UTC date"
      );
      require(
          grouped("2026-10-24T23:59:59Z", "2026-10-25T00:00:01Z", pending),
          "UTC midnight preserves a group within the same local day"
      );
    }

    setTestTimezone("NPT-5:45");
    require(app::formatMessageTime("2026-10-24T09:16:12Z") == "15:01", "Quarter-hour offsets are preserved");
    setTestTimezone("PST8PDT,M3.2.0/2,M11.1.0/2");
    require(
        app::formatMessageTime("2026-01-01T02:00:00Z", "%Y-%m-%d %H:%M") == "2025-12-31 18:00",
        "Negative offsets can move a message to the previous year"
    );
    require(
        !grouped("2026-10-24T06:59:59Z", "2026-10-24T07:00:01Z")
            && grouped("2026-10-24T23:59:59Z", "2026-10-25T00:00:01Z"),
        "Local day grouping also applies west of UTC"
    );

    setTestTimezone("CET-1CEST,M3.5.0/2,M10.5.0/3");
    require(
        app::formatMessageTime("2026-01-24T09:16:12Z") == "10:16"
            && app::formatMessageTime("2026-07-24T09:16:12Z") == "11:16",
        "Each message uses the daylight-saving offset for its own date"
    );
    require(
        app::formatMessageTime("2026-03-29T00:59:59Z") == "01:59"
            && app::formatMessageTime("2026-03-29T01:00:00Z") == "03:00"
            && grouped("2026-03-29T00:59:59Z", "2026-03-29T01:00:00Z"),
        "The spring clock jump does not create an artificial message gap"
    );
    require(
        app::formatMessageTime("2026-10-25T00:59:59Z") == "02:59"
            && app::formatMessageTime("2026-10-25T01:00:00Z") == "02:00"
            && grouped("2026-10-25T00:59:59Z", "2026-10-25T01:00:00Z"),
        "The autumn clock rollback preserves chronological grouping"
    );
    require(
        !grouped("2026-10-25T00:00:00Z", "2026-10-25T01:00:00Z"),
        "Repeated wall-clock times do not hide a real hour-long gap"
    );
    std::cout << "Local message times, calendar boundaries and daylight-saving transitions passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
