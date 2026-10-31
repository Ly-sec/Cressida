#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace voice {
  inline constexpr auto streamPlayoutDelay = std::chrono::milliseconds{100};
  inline constexpr auto maximumStreamLateness = std::chrono::milliseconds{250};
  inline constexpr auto maximumStreamFuture = std::chrono::milliseconds{300};
  struct SenderReport {
    std::uint32_t ssrc = 0, timestamp = 0, packets = 0, octets = 0;
    std::uint64_t ntp = 0;
  };
  std::optional<std::vector<SenderReport>> senderReports(std::span<const std::uint8_t> compound);
  std::vector<std::uint8_t> senderReport(SenderReport report, std::string_view cname);
  std::uint64_t ntpTimestamp(std::chrono::system_clock::time_point time);
  class MediaClock {
  public:
    using Clock = std::chrono::steady_clock;
    bool report(SenderReport report, int rate, Clock::time_point now);
    Clock::time_point due(std::uint32_t ssrc, std::uint32_t timestamp, int rate, Clock::time_point now) const;
    void clear();

  private:
    struct Anchor {
      SenderReport report;
      int rate;
    };
    std::map<std::uint32_t, Anchor> m_anchors;
    std::optional<std::uint64_t> m_remote;
    Clock::time_point m_local{};
  };
} // namespace voice
