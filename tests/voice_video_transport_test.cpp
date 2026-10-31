#include "voice/video_transport.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }
} // namespace
int main() {
  using namespace voice;
  using namespace std::chrono_literals;
  try {
    const std::array<std::uint8_t, 32> key{};
    const std::array<std::uint8_t, 3> types{120, 101, 102};
    protocol::RtpCipher sender, receiver;
    sender.payloadTypes(types);
    receiver.payloadTypes(types);
    require(sender.setKey(key) && receiver.setKey(key), "Transport keys install");
    auto now = VideoAssembler::Clock::time_point{} + 1s;
    for (const auto codec : {MediaCodec::VP8, MediaCodec::VP9, MediaCodec::H264}) {
      VideoRoute route{codec, 101, 102, 12, 13};
      std::vector<std::uint8_t> frame(10000, 42);
      if (codec == MediaCodec::H264) {
        frame[0] = frame[1] = frame[2] = 0;
        frame[3] = 1;
        frame[4] = 0x65;
      }
      std::uint16_t sequence = 65533;
      auto packets = packetizeVideo(frame, 90000, route, sequence);
      require(
          packets.size() > 3 && packets.back().marker && packets.front().sequence == 65533,
          "Video uses bounded packets and marks complete frames across sequence wrap"
      );
      VideoAssembler assembler(route);
      VideoRetransmit retransmit;
      std::optional<EncodedVideo> complete;
      for (std::size_t i = 0; i < packets.size(); ++i) {
        retransmit.sent(packets[i], now);
        if (i == 1)
          continue;
        const auto encrypted = sender.seal(packets[i]);
        const auto plain = receiver.open(encrypted);
        require(
            plain && plain->payloadType == 101 && plain->marker == packets[i].marker,
            "Transport authenticates the negotiated video type and marker"
        );
        auto result = assembler.receive(*plain, now);
        if (result)
          complete = std::move(result);
      }
      require(!complete, "A missing fragment never becomes a partial frame");
      const auto missing = assembler.missing(now + 50ms);
      require(missing == std::vector<std::uint16_t>{65534}, "Loss detection respects sequence wrap");
      const auto nack = videoNack(20, 12, missing);
      const auto feedback = videoFeedback(nack, 12);
      require(feedback && feedback->lost == missing, "NACK identifies the missing original sequence");
      const auto recovered = retransmit.request(feedback->lost, route, now + 50ms);
      require(
          recovered.size() == 1 && recovered[0].payloadType == 102 && recovered[0].ssrc == 13,
          "RTX uses its own negotiated SSRC and payload type"
      );
      complete = assembler.receive(*receiver.open(sender.seal(recovered[0])), now + 50ms);
      require(complete && complete->bytes == frame, "Recovered fragments reconstruct the entire encrypted frame");
      require(retransmit.request(missing, route, now + 2s).empty(), "Retransmission cache has a finite lifetime");
      now += 3s;
    }
    const auto pli = videoKeyframeRequest(20, 12);
    auto encrypted = sender.sealControl(pli);
    auto corrupt = encrypted;
    corrupt[4] ^= 1;
    require(receiver.openControl(corrupt).empty(), "Control sender tampering fails authentication");
    auto control = receiver.openControl(encrypted);
    require(control == pli && videoFeedback(control, 12)->keyframe, "Authenticated PLI requests a keyframe");
    require(receiver.openControl(encrypted).empty(), "Control packets reject replays");
    require(sender.setKey(key) && receiver.setKey(key), "Repeated key installation succeeds");
    require(receiver.openControl(encrypted).empty(), "Repeated keys preserve transport replay protection");
    require(receiver.openControl(sender.sealControl(pli)) == pli, "Repeated keys preserve outgoing nonce continuity");
    require(
        !videoFeedback(std::array<std::uint8_t, 4>{0x81, 206, 0, 20}, 12), "Malformed compound lengths are rejected"
    );
    VideoAssembler expired({MediaCodec::VP8, 101, 102, 12, 13});
    std::uint16_t sequence = 0;
    auto packets =
        packetizeVideo(std::vector<std::uint8_t>(4000, 42), 0, {MediaCodec::VP8, 101, 102, 12, 13}, sequence);
    expired.receive(packets.front(), now);
    expired.missing(now + 1s);
    require(expired.needsKeyframe(), "Expired incomplete frames request resynchronization");
    VideoPacer pacer;
    const VideoRoute pacedRoute{MediaCodec::VP8, 101, 102, 30, 31};
    constexpr int pacedBitrate = 3000000;
    constexpr std::size_t pacedFrameBytes = 64 * 1024;
    auto pacedPackets = packetizeVideo(std::vector<std::uint8_t>(pacedFrameBytes, 42), 90000, pacedRoute, sequence);
    const auto packetCount = pacedPackets.size();
    require(
        pacer.enqueue(std::move(pacedPackets), pacedBitrate, now, 123456), "A bounded keyframe enters the paced queue"
    );
    const auto firstBurst = pacer.take(now);
    require(
        firstBurst.size() == maximumVideoBurstPackets && pacer.take(now).empty(),
        "One instant never sends more than the bounded packet burst"
    );
    auto repair = firstBurst.front().packet;
    repair.ssrc = pacedRoute.rtxSsrc;
    repair.payloadType = pacedRoute.rtxPayloadType;
    require(pacer.enqueue({repair}, pacedBitrate, now, 0, true), "A bounded repair shares the network pacing budget");
    auto scheduled = pacer.deadline();
    auto repaired = pacer.take(scheduled);
    require(
        !repaired.empty() && repaired.front().retransmission, "Repair packets take priority over unsent frame fragments"
    );
    std::size_t sent = firstBurst.size();
    for (const auto& packet : repaired)
      sent += !packet.retransmission;
    for (std::size_t ticks = 0; pacer.deadline() != VideoPacer::Clock::time_point::max(); ++ticks) {
      require(ticks < packetCount * 4, "A bounded queue drains without a zero-deadline loop");
      scheduled = pacer.deadline();
      const auto burst = pacer.take(scheduled);
      require(burst.size() <= maximumVideoBurstPackets, "Every paced batch respects the packet burst limit");
      for (const auto& packet : burst) {
        require(packet.capturedAtUs == 123456, "Pacing preserves each frame's capture clock");
        sent += !packet.retransmission;
      }
    }
    require(
        sent == packetCount && scheduled > now && scheduled - now <= maximumVideoQueueDelay,
        "A keyframe is spread over time while preserving every packet and the latency bound"
    );
    require(
        !pacer.enqueue(
            packetizeVideo(std::vector<std::uint8_t>(1024 * 1024, 42), 180000, pacedRoute, sequence),
            minimumVideoBitrate, now
        )
            && pacer.needsKeyframe()
            && pacer.deadline() == VideoPacer::Clock::time_point::max(),
        "Oversized network backlogs are discarded with explicit keyframe recovery"
    );
    require(
        pacer.enqueue(
            packetizeVideo(std::vector<std::uint8_t>(pacedFrameBytes, 42), 270000, pacedRoute, sequence), pacedBitrate,
            now
        ),
        "A recovered keyframe can start a fresh paced queue"
    );
    require(
        pacer.take(now + maximumVideoQueueDelay + 1ms).empty() && pacer.needsKeyframe(),
        "A stalled worker never transmits an expired video backlog"
    );
    pacer.clear();
    require(
        pacer.deadline() == VideoPacer::Clock::time_point::max() && !pacer.needsKeyframe(),
        "Pause and revocation can retire every queued datagram"
    );
    constexpr std::size_t largeFrameBytes = 2 * 1024 * 1024;
    const std::vector<std::uint8_t> largeFrame(largeFrameBytes, 42);
    auto largePackets = packetizeVideo(largeFrame, 360000, pacedRoute, sequence);
    require(
        largePackets.size() > 64 && largePackets.size() < protocol::maximumMediaReplayPackets,
        "A high-resolution frame exceeds the ordinary audio replay window"
    );
    std::vector<std::vector<std::uint8_t>> encryptedPackets;
    for (const auto& packet : largePackets)
      encryptedPackets.push_back(sender.seal(packet));
    VideoAssembler largeAssembler(pacedRoute);
    require(bool(receiver.open(encryptedPackets.front())), "The first packet in a large frame authenticates");
    largeAssembler.receive(largePackets.front(), now);
    std::optional<EncodedVideo> reconstructed;
    for (std::size_t i = encryptedPackets.size(); i-- > 1;) {
      const auto packet = receiver.open(encryptedPackets[i]);
      require(bool(packet), "Reordered large-frame packets remain inside the bounded video replay window");
      if (auto frame = largeAssembler.receive(*packet, now))
        reconstructed = std::move(frame);
    }
    require(
        reconstructed && reconstructed->bytes == largeFrame && !receiver.open(encryptedPackets.back()),
        "Large reordered frames reconstruct exactly and still reject authenticated replays"
    );
    VideoReception reception;
    constexpr std::uint64_t senderNtp = 0x123456789abcdef0ULL;
    reception.synchronization(senderNtp, now);
    reception.packet(65534, 90000, now);
    reception.packet(0, 93000, now + 20ms);
    auto report = reception.report(20, 30, now + 1s, "test-viewer");
    const auto reported = videoFeedback(report, 30);
    require(
        report.size() > 32 && report[0] == 0x81 && report[1] == 201 && reported && reported->receiverReport,
        "A receiver emits a standard compound report with a source description"
    );
    require(
        reported->loss == 85 / 256.0F
            && (protocol::read32(report, 12) & 0xffffff) == 1
            && protocol::read32(report, 16) == 65536
            && protocol::read32(report, 20) > 0,
        "Receiver statistics retain loss, jitter and extended sequence continuity across wrap"
    );
    require(
        reported->lastSenderReport == static_cast<std::uint32_t>(senderNtp >> 16)
            && reported->senderReportDelay == 65536,
        "Receiver feedback echoes the sender clock with its actual reporting delay"
    );
    reception.packet(65535, 90000, now + 40ms);
    reception.packet(65535, 90000, now + 40ms);
    report = reception.report(20, 30, now + 2s, "test-viewer");
    require(
        videoFeedback(report, 30)->loss == 0 && (protocol::read32(report, 12) & 0xffffff) == 0,
        "Late repair clears cumulative loss while duplicate packets do not inflate received counts"
    );
    reception.clear();
    require(
        reception.report(20, 30, now + 3s, "test-viewer").empty(),
        "Suspending a subscription clears its reception statistics"
    );
    std::cout << "Video transport and feedback passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
