#include "voice/media_clock.h"

#include "voice/protocol.h"

#include <algorithm>
#include <limits>

namespace voice {
  namespace {
    constexpr std::uint64_t ntpEpochSeconds = 2208988800ULL, ntpFraction = 1ULL << 32, microsecondsPerSecond = 1000000;
    constexpr std::size_t senderReportBytes = 28, maximumCnameBytes = 255, maximumClockSources = 4;
    constexpr auto maximumReportGap = std::chrono::hours{1};
    std::chrono::microseconds ntpDifference(std::uint64_t from, std::uint64_t to) {
      const auto delta = static_cast<std::int64_t>(to - from);
      const auto seconds = delta / static_cast<std::int64_t>(ntpFraction);
      const auto fraction = delta % static_cast<std::int64_t>(ntpFraction);
      return std::chrono::microseconds{
          seconds * microsecondsPerSecond
          + fraction * static_cast<std::int64_t>(microsecondsPerSecond) / static_cast<std::int64_t>(ntpFraction)
      };
    }
  } // namespace
  std::uint64_t ntpTimestamp(std::chrono::system_clock::time_point time) {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
    if (micros < 0)
      return 0;
    const auto seconds = static_cast<std::uint64_t>(micros) / microsecondsPerSecond + ntpEpochSeconds;
    return (seconds << 32)
        | ((static_cast<std::uint64_t>(micros) % microsecondsPerSecond) << 32) / microsecondsPerSecond;
  }
  std::optional<std::vector<SenderReport>> senderReports(std::span<const std::uint8_t> compound) {
    if (compound.empty() || compound.size() > protocol::maximumDatagramBytes)
      return {};
    std::vector<SenderReport> reports;
    while (!compound.empty()) {
      if (compound.size() < 4 || compound[0] >> 6 != 2)
        return {};
      const auto length = (static_cast<std::size_t>(protocol::read16(compound, 2)) + 1) * 4;
      if (length < 4 || length > compound.size())
        return {};
      auto packet = compound.first(length);
      compound = compound.subspan(length);
      if (packet[0] & 0x20) {
        if (!compound.empty() || !packet.back() || packet.back() > packet.size() - 4)
          return {};
        packet = packet.first(packet.size() - packet.back());
      }
      if (packet[1] != 200)
        continue;
      if (packet.size() != senderReportBytes + static_cast<std::size_t>(packet[0] & 0x1f) * 24
          || reports.size() >= maximumClockSources)
        return {};
      const auto ssrc = protocol::read32(packet, 4);
      if (!ssrc)
        return {};
      reports.push_back(
          {ssrc, protocol::read32(packet, 16), protocol::read32(packet, 20), protocol::read32(packet, 24),
           (static_cast<std::uint64_t>(protocol::read32(packet, 8)) << 32) | protocol::read32(packet, 12)}
      );
    }
    return reports;
  }
  std::vector<std::uint8_t> senderReport(SenderReport report, std::string_view cname) {
    if (!report.ssrc
        || cname.empty()
        || cname.size() > maximumCnameBytes
        || std::ranges::any_of(cname, [](unsigned char c) { return c < 32 || c == 127; }))
      return {};
    constexpr std::size_t sdesHeaderBytes = 8, sdesItemBytes = 3, wordBytes = 4;
    const auto sourceBytes = (sdesHeaderBytes + sdesItemBytes + cname.size() + wordBytes - 1) / wordBytes * wordBytes;
    std::vector<std::uint8_t> packet(senderReportBytes + sourceBytes);
    packet[0] = 0x80;
    packet[1] = 200;
    protocol::write16(packet, 2, senderReportBytes / wordBytes - 1);
    protocol::write32(packet, 4, report.ssrc);
    protocol::write32(packet, 8, report.ntp >> 32);
    protocol::write32(packet, 12, static_cast<std::uint32_t>(report.ntp));
    protocol::write32(packet, 16, report.timestamp);
    protocol::write32(packet, 20, report.packets);
    protocol::write32(packet, 24, report.octets);
    auto source = std::span{packet}.subspan(senderReportBytes);
    source[0] = 0x81;
    source[1] = 202;
    protocol::write16(source, 2, sourceBytes / wordBytes - 1);
    protocol::write32(source, 4, report.ssrc);
    source[8] = 1;
    source[9] = static_cast<std::uint8_t>(cname.size());
    std::ranges::copy(cname, source.begin() + 10);
    return packet;
  }
  void MediaClock::clear() {
    m_anchors.clear();
    m_remote.reset();
    m_local = {};
  }
  bool MediaClock::report(SenderReport report, int rate, Clock::time_point now) {
    if (!report.ssrc || !report.ntp || (rate != 48000 && rate != 90000))
      return false;
    const auto found = m_anchors.find(report.ssrc);
    if (found == m_anchors.end() && m_anchors.size() >= maximumClockSources)
      return false;
    if (found != m_anchors.end()) {
      const auto progress = ntpDifference(found->second.report.ntp, report.ntp);
      if (progress < std::chrono::microseconds::zero() || progress > maximumReportGap || found->second.rate != rate)
        return false;
    }
    if (!m_remote) {
      m_remote = report.ntp;
      m_local = now;
    }
    const auto elapsed = ntpDifference(*m_remote, report.ntp);
    if (elapsed < -maximumReportGap || elapsed > now - m_local + maximumStreamFuture)
      return false;
    m_anchors[report.ssrc] = {report, rate};
    return true;
  }
  MediaClock::Clock::time_point
  MediaClock::due(std::uint32_t ssrc, std::uint32_t timestamp, int rate, Clock::time_point now) const {
    const auto found = m_anchors.find(ssrc);
    if (!m_remote || found == m_anchors.end() || found->second.rate != rate)
      return now + streamPlayoutDelay;
    const auto& anchor = found->second;
    const auto ticks = static_cast<std::int32_t>(timestamp - anchor.report.timestamp);
    const auto elapsed = std::chrono::microseconds{
        static_cast<std::int64_t>(ticks) * static_cast<std::int64_t>(microsecondsPerSecond) / rate
    };
    const auto result = m_local + ntpDifference(*m_remote, anchor.report.ntp) + elapsed + streamPlayoutDelay;
    return std::clamp(result, now - maximumStreamLateness, now + maximumStreamFuture);
  }
} // namespace voice
