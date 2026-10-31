#include "voice/video_transport.h"

#include "voice/media_clock.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace voice {
  namespace {
    constexpr std::array<std::uint8_t, 4> startCode{0, 0, 0, 1};
    constexpr auto nackInterval = std::chrono::milliseconds{40};
    constexpr auto retransmitLifetime = std::chrono::seconds{1};
    constexpr std::size_t maximumFeedbackLosses = 64, maximumNals = 1024;
    constexpr std::size_t estimatedVideoHeaderBytes = 88;
    constexpr std::size_t maximumPacedVideoBytes = maximumVideoFrameBytes;
    constexpr auto videoBurstInterval = std::chrono::milliseconds{1};
    constexpr double maximumVideoCredit =
        maximumVideoBurstPackets * (videoPacketPayloadBytes + estimatedVideoHeaderBytes);
    constexpr std::size_t receiverReportBytes = 32, senderReportBytes = 28;
    constexpr std::uint32_t rtcpClockRate = 65536;
    std::size_t pacedBytes(const protocol::Packet& packet) { return packet.payload.size() + estimatedVideoHeaderBytes; }
    constexpr std::uint8_t nalTypeMask = 0x1f, nalFuA = 28, nalStapA = 24;
    struct Fragment {
      std::vector<std::uint8_t> bytes;
      bool first = false;
    };
    std::optional<Fragment> fragment(std::span<const std::uint8_t> payload, MediaCodec codec) {
      if (payload.empty())
        return {};
      Fragment result;
      std::size_t at = 1;
      if (codec == MediaCodec::VP8) {
        result.first = (payload[0] & 0x10) && !(payload[0] & 0x0f);
        if (payload[0] & 0x80) {
          if (at == payload.size())
            return {};
          const auto extra = payload[at++];
          if (extra & 0x80) {
            if (at == payload.size())
              return {};
            const auto picture = payload[at++];
            if (picture & 0x80)
              ++at;
          }
          if (extra & 0x40)
            ++at;
          if (extra & 0x30)
            ++at;
        }
      } else if (codec == MediaCodec::VP9) {
        const auto flags = payload[0];
        result.first = flags & 0x08;
        if (flags & 0x80) {
          if (at == payload.size())
            return {};
          if (payload[at++] & 0x80)
            ++at;
        }
        if (flags & 0x20) {
          ++at;
          if (!(flags & 0x10))
            ++at;
        }
        if ((flags & 0x50) == 0x50) {
          unsigned references = 0;
          bool more = true;
          while (more) {
            if (at >= payload.size() || ++references > 3)
              return {};
            more = payload[at++] & 1;
          }
        }
        if (flags & 0x02) {
          if (at >= payload.size())
            return {};
          const auto scalability = payload[at++];
          const std::size_t layers = (scalability >> 5) + 1;
          if (scalability & 0x10)
            at += layers * 4;
          if (scalability & 0x08) {
            if (at >= payload.size())
              return {};
            const auto groups = payload[at++];
            for (unsigned i = 0; i < groups; ++i) {
              if (at >= payload.size())
                return {};
              at += 1 + ((payload[at] >> 2) & 3);
            }
          }
        }
      } else if (codec == MediaCodec::H264) {
        const auto type = payload[0] & nalTypeMask;
        if (type > 0 && type < nalStapA) {
          result.bytes.assign(startCode.begin(), startCode.end());
          result.bytes.insert(result.bytes.end(), payload.begin(), payload.end());
          result.first = true;
          return result;
        }
        if (type == nalFuA) {
          if (payload.size() <= 2 || (payload[1] & 0x20) || !(payload[1] & nalTypeMask) || (payload[1] & 0xc0) == 0xc0)
            return {};
          result.first = payload[1] & 0x80;
          if (result.first) {
            result.bytes.assign(startCode.begin(), startCode.end());
            result.bytes.push_back((payload[0] & 0xe0) | (payload[1] & nalTypeMask));
          }
          result.bytes.insert(result.bytes.end(), payload.begin() + 2, payload.end());
          return result;
        }
        if (type == nalStapA) {
          std::size_t nals = 0;
          while (at < payload.size()) {
            if (payload.size() - at < 2 || ++nals > maximumNals)
              return {};
            const auto size = protocol::read16(payload, at);
            at += 2;
            if (!size
                || size > payload.size() - at
                || !(payload[at] & nalTypeMask)
                || (payload[at] & nalTypeMask) >= nalStapA)
              return {};
            result.bytes.insert(result.bytes.end(), startCode.begin(), startCode.end());
            result.bytes.insert(result.bytes.end(), payload.begin() + at, payload.begin() + at + size);
            at += size;
          }
          result.first = true;
          return result;
        }
        return {};
      } else
        return {};
      if (at >= payload.size())
        return {};
      result.bytes.assign(payload.begin() + at, payload.end());
      return result;
    }
    std::vector<std::uint8_t>
    feedbackHeader(std::uint8_t type, std::uint8_t format, std::uint32_t self, std::uint32_t media, std::size_t size) {
      if (!self || !media)
        return {};
      std::vector<std::uint8_t> packet(size);
      packet[0] = 0x80 | format;
      packet[1] = type;
      protocol::write16(packet, 2, static_cast<std::uint16_t>(size / 4 - 1));
      protocol::write32(packet, 4, self);
      protocol::write32(packet, 8, media);
      return packet;
    }
  } // namespace
  std::vector<protocol::Packet> packetizeVideo(
      std::span<const std::uint8_t> frame, std::uint32_t timestamp, VideoRoute route, std::uint16_t& sequence
  ) {
    if (frame.empty() || frame.size() > maximumVideoFrameBytes || !route.ssrc || route.payloadType > 127)
      return {};
    std::vector<protocol::Packet> packets;
    auto append = [&](std::vector<std::uint8_t> bytes) {
      packets.push_back({route.ssrc, timestamp, sequence++, std::move(bytes), route.payloadType, false});
    };
    if (route.codec == MediaCodec::VP8 || route.codec == MediaCodec::VP9) {
      constexpr auto contentBytes = videoPacketPayloadBytes - 1;
      for (std::size_t at = 0; at < frame.size(); at += contentBytes) {
        const auto length = std::min(contentBytes, frame.size() - at);
        std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(
            route.codec == MediaCodec::VP8 ? (at == 0 ? 0x10 : 0)
                                           : ((at == 0 ? 0x08 : 0) | (at + length == frame.size() ? 0x04 : 0))
        )};
        bytes.insert(bytes.end(), frame.begin() + at, frame.begin() + at + length);
        append(std::move(bytes));
      }
    } else if (route.codec == MediaCodec::H264) {
      std::size_t at = 0, nals = 0;
      while (at < frame.size()) {
        if (frame.size() - at < startCode.size() + 1
            || !std::ranges::equal(frame.subspan(at, startCode.size()), startCode)
            || ++nals > maximumNals)
          return {};
        at += startCode.size();
        const auto next = std::search(frame.begin() + at, frame.end(), startCode.begin(), startCode.end());
        const auto end = static_cast<std::size_t>(next - frame.begin());
        const auto nal = frame.subspan(at, end - at);
        if (nal.empty() || !(nal[0] & nalTypeMask) || (nal[0] & nalTypeMask) >= nalStapA)
          return {};
        if (nal.size() <= videoPacketPayloadBytes)
          append({nal.begin(), nal.end()});
        else {
          constexpr auto contentBytes = videoPacketPayloadBytes - 2;
          for (std::size_t part = 1; part < nal.size(); part += contentBytes) {
            const auto count = std::min(contentBytes, nal.size() - part);
            std::vector<std::uint8_t> bytes{
                static_cast<std::uint8_t>((nal[0] & 0xe0) | nalFuA),
                static_cast<std::uint8_t>(
                    (nal[0] & nalTypeMask) | (part == 1 ? 0x80 : 0) | (part + count == nal.size() ? 0x40 : 0)
                )
            };
            bytes.insert(bytes.end(), nal.begin() + part, nal.begin() + part + count);
            append(std::move(bytes));
          }
        }
        at = end;
      }
    }
    if (!packets.empty())
      packets.back().marker = true;
    return packets;
  }
  void VideoPacer::refill(Clock::time_point now) {
    if (!m_initialized) {
      m_initialized = true;
      m_updated = m_burstAt = now;
      m_credit = maximumVideoCredit;
    } else if (now > m_updated) {
      m_credit = std::min(
          maximumVideoCredit, m_credit + std::chrono::duration<double>(now - m_updated).count() * m_bytesPerSecond
      );
      m_updated = now;
    }
    if (now - m_burstAt >= videoBurstInterval) {
      m_burstAt = now;
      m_burstPackets = 0;
    }
  }
  bool VideoPacer::enqueue(
      std::vector<protocol::Packet> packets, int bitrate, Clock::time_point now, std::int64_t capturedAtUs,
      bool retransmission
  ) {
    refill(now);
    m_bytesPerSecond = std::clamp(bitrate, minimumVideoBitrate, maximumVideoBitrate) / 8.0;
    std::size_t bytes = 0;
    bool valid =
        !packets.empty() && packets.size() <= (retransmission ? maximumFeedbackLosses : maximumVideoReceivePackets);
    for (const auto& packet : packets) {
      const auto size = pacedBytes(packet);
      if (packet.payload.empty()
          || packet.payload.size() > videoPacketPayloadBytes + sizeof(std::uint16_t)
          || size > maximumPacedVideoBytes - bytes) {
        valid = false;
        break;
      }
      bytes += size;
    }
    const auto queued = m_packets.size() + m_repairs.size();
    valid = valid
        && bytes <= maximumPacedVideoBytes - m_bytes
        && packets.size() <= maximumVideoReceivePackets - queued
        && std::max(0.0, static_cast<double>(m_bytes + bytes) - m_credit) / m_bytesPerSecond
            <= std::chrono::duration<double>(maximumVideoQueueDelay).count();
    if (!valid) {
      if (!retransmission) {
        clear();
        m_keyframe = true;
      }
      return false;
    }
    auto& queue = retransmission ? m_repairs : m_packets;
    for (auto& packet : packets)
      queue.push_back({{std::move(packet), capturedAtUs, retransmission}, now});
    m_bytes += bytes;
    return true;
  }
  std::vector<PacedVideoPacket> VideoPacer::take(Clock::time_point now) {
    refill(now);
    std::vector<PacedVideoPacket> output;
    while ((!m_packets.empty() || !m_repairs.empty()) && m_burstPackets < maximumVideoBurstPackets) {
      auto& queue = !m_repairs.empty() ? m_repairs : m_packets;
      if (now - queue.front().queued >= maximumVideoQueueDelay) {
        clear();
        m_keyframe = true;
        break;
      }
      const auto bytes = pacedBytes(queue.front().value.packet);
      if (m_credit < bytes)
        break;
      m_credit -= bytes;
      m_bytes -= bytes;
      ++m_burstPackets;
      output.push_back(std::move(queue.front().value));
      queue.pop_front();
    }
    return output;
  }
  VideoPacer::Clock::time_point VideoPacer::deadline() const {
    if (m_packets.empty() && m_repairs.empty())
      return Clock::time_point::max();
    const auto& next = !m_repairs.empty() ? m_repairs.front() : m_packets.front();
    const auto missing = std::max(0.0, static_cast<double>(pacedBytes(next.value.packet)) - m_credit);
    auto due =
        m_updated + std::chrono::ceil<Clock::duration>(std::chrono::duration<double>{missing / m_bytesPerSecond});
    if (m_burstPackets >= maximumVideoBurstPackets)
      due = std::max(due, m_burstAt + videoBurstInterval);
    return std::min(due, next.queued + maximumVideoQueueDelay);
  }
  bool VideoPacer::needsKeyframe() { return std::exchange(m_keyframe, false); }
  void VideoPacer::clear() {
    m_packets.clear();
    m_repairs.clear();
    m_bytes = m_burstPackets = 0;
    m_updated = m_burstAt = {};
    m_credit = m_bytesPerSecond = 0;
    m_initialized = m_keyframe = false;
  }
  VideoAssembler::VideoAssembler(VideoRoute route) : m_route(route) {}
  void VideoAssembler::route(VideoRoute route) {
    m_route = route;
    clear();
  }
  void VideoAssembler::clear() {
    m_packets.clear();
    m_first.reset();
    m_last.reset();
    m_bytes = 0;
    m_began = {};
    m_nacked = {};
    m_completed = false;
  }
  bool VideoAssembler::needsKeyframe() { return std::exchange(m_keyframe, false); }
  std::optional<EncodedVideo> VideoAssembler::receive(protocol::Packet packet, Clock::time_point now) {
    if (packet.ssrc == m_route.rtxSsrc && packet.payloadType == m_route.rtxPayloadType) {
      if (packet.payload.size() <= 2)
        return {};
      packet.sequence = protocol::read16(packet.payload, 0);
      packet.payload.erase(packet.payload.begin(), packet.payload.begin() + 2);
      packet.ssrc = m_route.ssrc;
      packet.payloadType = m_route.payloadType;
    }
    if (packet.ssrc != m_route.ssrc || packet.payloadType != m_route.payloadType)
      return {};
    const auto part = fragment(packet.payload, m_route.codec);
    if (!part) {
      m_keyframe = true;
      return {};
    }
    if (!m_packets.empty() && (packet.timestamp != m_timestamp || now - m_began > videoAssemblyDeadline)) {
      if (static_cast<std::int32_t>(packet.timestamp - m_timestamp) < 0)
        return {};
      const bool incomplete = !m_completed;
      clear();
      m_keyframe |= incomplete;
    }
    if (m_packets.empty()) {
      m_timestamp = packet.timestamp;
      m_began = now;
    }
    if (m_packets.contains(packet.sequence))
      return {};
    if (m_packets.size() == maximumVideoReceivePackets || part->bytes.size() > maximumVideoFrameBytes - m_bytes) {
      clear();
      m_keyframe = true;
      return {};
    }
    m_bytes += part->bytes.size();
    m_packets.emplace(packet.sequence, part->bytes);
    if (part->first && (!m_first || static_cast<std::int16_t>(packet.sequence - *m_first) < 0))
      m_first = packet.sequence;
    if (packet.marker)
      m_last = packet.sequence;
    if (!m_first || !m_last)
      return {};
    const auto count = static_cast<std::uint16_t>(*m_last - *m_first) + 1U;
    if (count > maximumVideoReceivePackets) {
      clear();
      m_keyframe = true;
      return {};
    }
    if (m_packets.size() < count)
      return {};
    EncodedVideo result;
    result.codec = m_route.codec;
    result.timestamp = m_timestamp;
    result.bytes.reserve(m_bytes);
    for (unsigned index = 0; index < count; ++index) {
      const auto it = m_packets.find(static_cast<std::uint16_t>(*m_first + index));
      if (it == m_packets.end())
        return {};
      result.bytes.insert(result.bytes.end(), it->second.begin(), it->second.end());
    }
    m_completed = true;
    return result;
  }
  std::vector<std::uint16_t> VideoAssembler::missing(Clock::time_point now) {
    std::vector<std::uint16_t> result;
    if (m_packets.empty() || m_completed)
      return result;
    if (now - m_began > videoAssemblyDeadline) {
      clear();
      m_keyframe = true;
      return result;
    }
    if (!m_first || !m_last || now - m_began < nackInterval || now - m_nacked < nackInterval)
      return result;
    const auto count = static_cast<std::uint16_t>(*m_last - *m_first) + 1U;
    if (count > maximumVideoReceivePackets)
      return result;
    for (unsigned i = 0; i < count && result.size() < maximumFeedbackLosses; ++i) {
      const auto sequence = static_cast<std::uint16_t>(*m_first + i);
      if (!m_packets.contains(sequence))
        result.push_back(sequence);
    }
    m_nacked = now;
    return result;
  }
  std::optional<VideoFeedback> videoFeedback(std::span<const std::uint8_t> compound, std::uint32_t media) {
    VideoFeedback result;
    if (compound.empty() || compound.size() > protocol::maximumDatagramBytes)
      return {};
    while (!compound.empty()) {
      if (compound.size() < 4 || compound[0] >> 6 != 2)
        return {};
      const std::size_t size = (static_cast<std::size_t>(protocol::read16(compound, 2)) + 1) * 4;
      if (size < 4 || size > compound.size())
        return {};
      auto packet = compound.first(size);
      compound = compound.subspan(size);
      if (packet[0] & 0x20) {
        if (!compound.empty() || !packet.back() || packet.back() > packet.size() - 4)
          return {};
        packet = packet.first(packet.size() - packet.back());
      }
      const auto type = packet[1];
      const auto format = packet[0] & 0x1f;
      if ((type == 205 || type == 206) && packet.size() >= 12 && protocol::read32(packet, 8) == media) {
        if (type == 206 && format == 1) {
          if (packet.size() != 12)
            return {};
          result.keyframe = true;
        }
        if (type == 205 && format == 1) {
          if ((packet.size() - 12) % 4)
            return {};
          for (std::size_t at = 12; at < packet.size(); at += 4) {
            const auto first = protocol::read16(packet, at), bits = protocol::read16(packet, at + 2);
            if (result.lost.size() < maximumFeedbackLosses)
              result.lost.push_back(first);
            for (unsigned bit = 0; bit < 16 && result.lost.size() < maximumFeedbackLosses; ++bit)
              if (bits & (1U << bit))
                result.lost.push_back(static_cast<std::uint16_t>(first + bit + 1));
          }
        }
      }
      if (type == 201 || type == 200) {
        const std::size_t at = type == 201 ? 8 : 28;
        if (packet.size() < at + static_cast<std::size_t>(format) * 24)
          return {};
        for (unsigned block = 0; block < static_cast<unsigned>(format); ++block) {
          const auto offset = at + block * 24;
          if (protocol::read32(packet, offset) == media) {
            result.loss = packet[offset + 4] / 256.0F;
            result.receiverReport = true;
            result.lastSenderReport = protocol::read32(packet, offset + 16);
            result.senderReportDelay = protocol::read32(packet, offset + 20);
          }
        }
      }
      if (type == 206
          && format == 15
          && packet.size() >= 20
          && std::ranges::equal(packet.subspan(12, 4), std::array<std::uint8_t, 4>{'R', 'E', 'M', 'B'})) {
        const auto sources = packet[16];
        if (packet.size() != 20 + static_cast<std::size_t>(sources) * 4)
          return {};
        for (unsigned i = 0; i < sources; ++i)
          if (protocol::read32(packet, 20 + i * 4) == media) {
            const auto exponent = packet[17] >> 2;
            const std::uint64_t mantissa = ((packet[17] & 3) << 16) | (packet[18] << 8) | packet[19];
            result.bitrate = exponent < 46 ? mantissa << exponent : std::numeric_limits<std::uint64_t>::max();
          }
      }
    }
    return result;
  }
  void VideoReception::clear() { *this = VideoReception{}; }
  void VideoReception::packet(std::uint16_t sequence, std::uint32_t timestamp, Clock::time_point now) {
    if (!m_initialized) {
      m_first = m_last = sequence;
      m_seen.set(0);
    } else {
      auto extended = m_last + static_cast<std::int16_t>(sequence - static_cast<std::uint16_t>(m_last));
      if (extended < 0) {
        extended += 1ULL << 16;
        m_first += 1ULL << 16;
        m_last += 1ULL << 16;
      }
      if (extended > m_last) {
        const auto distance = static_cast<std::size_t>(extended - m_last);
        if (distance >= m_seen.size())
          m_seen.reset();
        else
          m_seen <<= distance;
        m_last = extended;
      }
      const auto age = static_cast<std::size_t>(m_last - extended);
      if (age >= m_seen.size() || m_seen.test(age))
        return;
      m_seen.set(age);
      m_first = std::min(m_first, extended);
    }
    const auto arrival = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(std::chrono::duration<double>(now.time_since_epoch()).count() * videoClockRate)
    );
    const auto transit = arrival - timestamp;
    if (m_initialized) {
      const auto difference = std::abs(static_cast<std::int64_t>(static_cast<std::int32_t>(transit - m_transit)));
      m_jitter += (difference - m_jitter) / 16;
    }
    m_transit = transit;
    ++m_received;
    m_initialized = true;
  }
  void VideoReception::synchronization(std::uint64_t ntp, Clock::time_point now) {
    if (ntp && (!m_ntp || static_cast<std::int64_t>(ntp - m_ntp) >= 0)) {
      m_ntp = ntp;
      m_synchronized = now;
    }
  }
  std::vector<std::uint8_t>
  VideoReception::report(std::uint32_t self, std::uint32_t media, Clock::time_point now, std::string_view cname) {
    if (!m_initialized || !self || !media)
      return {};
    auto compound = senderReport({.ssrc = self}, cname);
    if (compound.empty())
      return {};
    const auto expected = static_cast<std::uint64_t>(m_last - m_first + 1);
    const auto expectedInterval = expected - m_expectedPrior, receivedInterval = m_received - m_receivedPrior;
    const auto lost = expected > m_received ? expected - m_received : 0;
    const auto lostInterval = expectedInterval > receivedInterval ? expectedInterval - receivedInterval : 0;
    const auto fraction = expectedInterval ? std::min<std::uint64_t>(255, lostInterval * 256 / expectedInterval) : 0;
    m_expectedPrior = expected;
    m_receivedPrior = m_received;
    std::vector<std::uint8_t> result(receiverReportBytes);
    result[0] = 0x81;
    result[1] = 201;
    protocol::write16(result, 2, receiverReportBytes / 4 - 1);
    protocol::write32(result, 4, self);
    protocol::write32(result, 8, media);
    protocol::write32(result, 12, static_cast<std::uint32_t>(std::min<std::uint64_t>(lost, 0x7fffff)));
    result[12] = fraction;
    protocol::write32(result, 16, static_cast<std::uint32_t>(m_last));
    protocol::write32(
        result, 20,
        static_cast<std::uint32_t>(
            std::clamp(m_jitter, 0.0, static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
        )
    );
    protocol::write32(result, 24, static_cast<std::uint32_t>(m_ntp >> 16));
    if (m_ntp && now >= m_synchronized)
      protocol::write32(
          result, 28,
          static_cast<std::uint32_t>(std::min<double>(
              std::numeric_limits<std::uint32_t>::max(),
              std::chrono::duration<double>(now - m_synchronized).count() * rtcpClockRate
          ))
      );
    result.insert(result.end(), compound.begin() + senderReportBytes, compound.end());
    return result;
  }
  std::vector<std::uint8_t> videoKeyframeRequest(std::uint32_t self, std::uint32_t media) {
    return feedbackHeader(206, 1, self, media, 12);
  }
  std::vector<std::uint8_t> videoNack(std::uint32_t self, std::uint32_t media, std::span<const std::uint16_t> missing) {
    if (missing.empty() || missing.size() > maximumFeedbackLosses)
      return {};
    auto packet = feedbackHeader(205, 1, self, media, 12 + missing.size() * 4);
    if (packet.empty())
      return {};
    for (std::size_t i = 0; i < missing.size(); ++i)
      protocol::write16(packet, 12 + i * 4, missing[i]);
    return packet;
  }
  void VideoRetransmit::sent(const protocol::Packet& packet, Clock::time_point now) {
    while (!m_packets.empty()
           && (now - m_packets.front().at > retransmitLifetime || m_packets.size() >= maximumRetransmitPackets))
      m_packets.pop_front();
    m_packets.push_back({packet, now});
  }
  std::vector<protocol::Packet>
  VideoRetransmit::request(std::span<const std::uint16_t> lost, VideoRoute route, Clock::time_point now) {
    std::vector<protocol::Packet> output;
    if (!route.rtxSsrc || lost.size() > maximumFeedbackLosses)
      return output;
    for (auto sequence : lost) {
      const auto found = std::ranges::find_if(m_packets, [&](const auto& entry) {
        return entry.packet.sequence == sequence && now - entry.at <= retransmitLifetime;
      });
      if (found == m_packets.end())
        continue;
      auto packet = found->packet;
      packet.ssrc = route.rtxSsrc;
      packet.payloadType = route.rtxPayloadType;
      packet.sequence = m_sequence++;
      packet.payload.insert(packet.payload.begin(), 2, 0);
      protocol::write16(packet.payload, 0, sequence);
      output.push_back(std::move(packet));
    }
    return output;
  }
  void VideoRetransmit::clear() { m_packets.clear(); }
} // namespace voice
