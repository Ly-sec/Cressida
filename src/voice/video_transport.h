#pragma once

#include "voice/media.h"
#include "voice/protocol.h"

#include <deque>
#include <map>

namespace voice {
  inline constexpr std::size_t videoPacketPayloadBytes = 1150;
  inline constexpr std::size_t maximumRetransmitPackets = 1024;
  inline constexpr std::size_t maximumVideoBurstPackets = 8;
  inline constexpr auto maximumVideoQueueDelay = std::chrono::milliseconds{200};
  struct VideoRoute {
    MediaCodec codec = MediaCodec::VP8;
    std::uint8_t payloadType = 101, rtxPayloadType = 102;
    std::uint32_t ssrc = 0, rtxSsrc = 0;
  };
  std::vector<protocol::Packet> packetizeVideo(
      std::span<const std::uint8_t> encryptedFrame, std::uint32_t timestamp, VideoRoute route, std::uint16_t& sequence
  );
  struct PacedVideoPacket {
    protocol::Packet packet;
    std::int64_t capturedAtUs = 0;
    bool retransmission = false;
  };
  class VideoPacer {
  public:
    using Clock = std::chrono::steady_clock;
    bool enqueue(
        std::vector<protocol::Packet> packets, int bitrate, Clock::time_point now, std::int64_t capturedAtUs = 0,
        bool retransmission = false
    );
    std::vector<PacedVideoPacket> take(Clock::time_point now);
    Clock::time_point deadline() const;
    bool needsKeyframe();
    void clear();

  private:
    struct Entry {
      PacedVideoPacket value;
      Clock::time_point queued;
    };
    void refill(Clock::time_point now);
    std::deque<Entry> m_packets, m_repairs;
    Clock::time_point m_updated{}, m_burstAt{};
    double m_credit = 0, m_bytesPerSecond = 0;
    std::size_t m_bytes = 0, m_burstPackets = 0;
    bool m_initialized = false, m_keyframe = false;
  };
  class VideoAssembler {
  public:
    using Clock = std::chrono::steady_clock;
    explicit VideoAssembler(VideoRoute route = {});
    void route(VideoRoute route);
    std::optional<EncodedVideo> receive(protocol::Packet packet, Clock::time_point now);
    std::vector<std::uint16_t> missing(Clock::time_point now);
    bool needsKeyframe();
    void clear();

  private:
    VideoRoute m_route;
    std::map<std::uint16_t, std::vector<std::uint8_t>> m_packets;
    std::optional<std::uint16_t> m_first, m_last;
    std::uint32_t m_timestamp = 0;
    std::size_t m_bytes = 0;
    Clock::time_point m_began{}, m_nacked{};
    bool m_keyframe = false, m_completed = false;
  };
  struct VideoFeedback {
    bool keyframe = false;
    bool receiverReport = false;
    std::vector<std::uint16_t> lost;
    float loss = 0;
    std::uint64_t bitrate = 0;
    std::uint32_t lastSenderReport = 0, senderReportDelay = 0;
  };
  class VideoReception {
  public:
    using Clock = std::chrono::steady_clock;
    void packet(std::uint16_t sequence, std::uint32_t timestamp, Clock::time_point now);
    void synchronization(std::uint64_t ntp, Clock::time_point now);
    std::vector<std::uint8_t>
    report(std::uint32_t self, std::uint32_t media, Clock::time_point now, std::string_view cname);
    void clear();

  private:
    std::bitset<maximumVideoReceivePackets> m_seen;
    std::int64_t m_first = 0, m_last = 0;
    std::uint64_t m_received = 0, m_expectedPrior = 0, m_receivedPrior = 0, m_ntp = 0;
    std::uint32_t m_transit = 0;
    double m_jitter = 0;
    Clock::time_point m_synchronized{};
    bool m_initialized = false;
  };
  std::optional<VideoFeedback> videoFeedback(std::span<const std::uint8_t> compound, std::uint32_t mediaSsrc);
  std::vector<std::uint8_t> videoKeyframeRequest(std::uint32_t self, std::uint32_t media);
  std::vector<std::uint8_t> videoNack(std::uint32_t self, std::uint32_t media, std::span<const std::uint16_t> missing);
  class VideoRetransmit {
  public:
    using Clock = std::chrono::steady_clock;
    void sent(const protocol::Packet& packet, Clock::time_point now);
    std::vector<protocol::Packet> request(std::span<const std::uint16_t> lost, VideoRoute route, Clock::time_point now);
    void clear();

  private:
    struct Entry {
      protocol::Packet packet;
      Clock::time_point at;
    };
    std::deque<Entry> m_packets;
    std::uint16_t m_sequence = 0;
  };
} // namespace voice
