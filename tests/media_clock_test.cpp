#include "voice/media_clock.h"
#include "voice/protocol.h"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
} // namespace
int main() {
  using namespace voice;
  using namespace std::chrono_literals;
  try {
    const auto wall = std::chrono::system_clock::time_point{} + std::chrono::hours{24 * 365 * 56};
    const auto ntp = ntpTimestamp(wall);
    const SenderReport video{1, 0xfffffff0, 100, 12000, ntp}, audio{2, 48000, 50, 6400, ntp};
    const auto encoded = senderReport(video, "private-synthetic-session");
    const auto reports = senderReports(encoded);
    require(
        reports
            && reports->size() == 1
            && reports->front().ssrc == video.ssrc
            && reports->front().timestamp == video.timestamp
            && reports->front().ntp == ntp,
        "Compound sender reports preserve both wall and RTP clocks"
    );
    auto malformed = encoded;
    malformed[3] = 0xff;
    require(!senderReports(malformed), "Truncated compound report lengths are rejected");
    require(senderReport(video, std::string(256, 'a')).empty(), "Report source descriptions have bounded lengths");
    MediaClock clock;
    const auto now = MediaClock::Clock::time_point{} + 10s;
    require(
        clock.due(1, 0, 90000, now) == now + streamPlayoutDelay,
        "Streams have a bounded fallback delay before clock reports arrive"
    );
    require(
        clock.report(video, 90000, now) && clock.report(audio, 48000, now + 10ms),
        "Audio and video reports share one remote clock anchor"
    );
    const auto videoDue = clock.due(1, static_cast<std::uint32_t>(video.timestamp + 9000), 90000, now + 20ms);
    const auto audioDue = clock.due(2, audio.timestamp + 4800, 48000, now + 80ms);
    require(
        videoDue == audioDue && videoDue == now + 100ms + streamPlayoutDelay,
        "Different RTP clock rates and arrival times present the same captured instant together across video timestamp "
        "wrap"
    );
    auto next = video;
    next.ntp = ntpTimestamp(wall + 1s);
    next.timestamp += 90000;
    require(clock.report(next, 90000, now + 1s + 5ms), "Periodic reports refresh the source's RTP mapping");
    require(!clock.report(video, 90000, now + 2s), "Stale clock reports cannot rewind an established stream");
    require(
        clock.due(1, next.timestamp + 90000 * 100, 90000, now + 2s) <= now + 2s + maximumStreamFuture,
        "Forged or discontinuous timestamps cannot schedule unbounded future work"
    );
    require(
        clock.due(1, next.timestamp - 90000 * 100, 90000, now + 2s) >= now + 2s - maximumStreamLateness,
        "Old timestamps have a bounded late presentation window"
    );
    for (std::uint32_t source = 3; source <= 4; ++source) {
      auto report = video;
      report.ssrc = source;
      require(clock.report(report, 90000, now), "The bounded clock directory accepts legitimate media sources");
    }
    auto excessive = video;
    excessive.ssrc = 5;
    require(!clock.report(excessive, 90000, now), "Additional clock domains cannot grow storage beyond its limit");
    clock.clear();
    require(clock.due(1, 0, 90000, now) == now + streamPlayoutDelay, "Call reset removes prior clock mappings");
    std::cout << "Shared media presentation clocks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
