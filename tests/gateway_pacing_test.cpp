#include "api/gateway_pacing.h"

#include <iostream>
#include <limits>

int main() {
  using namespace api::gateway_pacing;
  const Time start{100s};
  bool valid = true;
  const auto require = [&](bool condition, const char* message) {
    if (!condition) {
      std::cerr << message << '\n';
      valid = false;
    }
  };

  ApplicationBudget application;
  require(!application.acquire(start, 0), "Empty reservations are invalid");
  require(!application.acquire(start, applicationLimit + 1), "Oversized reservations are invalid");
  require(application.acquire(start, applicationLimit - 2), "Initial application traffic is available");
  require(application.acquire(start + 1s), "All application types share the remaining allowance");
  require(
      application.ready(start + 2s, 2) == start + applicationWindow,
      "A two-frame subscription waits for sufficient shared allowance"
  );
  require(!application.acquire(start + 2s, 2), "An incomplete subscription reservation cannot overspend");
  require(application.acquire(start + 2s), "A rejected two-frame reservation does not consume a remaining slot");
  require(!application.acquire(start + applicationWindow - 1ms), "A reconnect cannot erase the rolling budget");
  require(application.ready(start + 2s) == start + applicationWindow, "The next wake uses the oldest expiry");
  require(
      application.acquire(start + applicationWindow, applicationLimit - 2),
      "Expired entries release exactly their allowance"
  );
  require(
      application.ready(start + applicationWindow, 2) == start + applicationWindow + 2s,
      "Two-frame capacity honors both later entries instead of resetting the whole window"
  );
  require(
      application.acquire(start + applicationWindow + 2s, 2), "A delayed subscription eventually receives both slots"
  );

  ReconnectBudget reconnect;
  require(reconnect.ready(start, false) == start, "The first connection is immediate");
  reconnect.connecting(start);
  reconnect.identifying(start + 100ms);
  require(
      reconnect.ready(start + 200ms, true) == start + connectionSpacing,
      "Ordinary resumable connections retain independent connection spacing"
  );
  require(
      reconnect.ready(start + 200ms, false) == start + 100ms + identifySpacing,
      "Fresh sessions retain IDENTIFY spacing despite a quick READY"
  );
  require(reconnect.ready(start + 200ms, true, true) == start + 200ms, "Explicit handoffs can resume immediately");
  require(
      reconnect.ready(start + 200ms, false, true) == start + 100ms + identifySpacing,
      "Explicit handoffs cannot bypass fresh IDENTIFY spacing"
  );
  require(reconnect.failed(start, Failure::Transport) == start + 1s, "The initial transport backoff is bounded");
  require(
      reconnect.failed(start, Failure::Transport, 1s) == start + 2s,
      "Brief READY sessions do not reset repeated-failure backoff"
  );
  require(reconnect.failed(start, Failure::Handoff) == start, "Server handoffs do not incur transport backoff");
  require(
      reconnect.failed(start, Failure::Transport, 1s) == start + 4s,
      "A handoff does not clear accumulated failure history"
  );
  require(
      reconnect.failed(start, Failure::Transport, stableConnection) == start + 1s,
      "A stable live session restores the initial retry delay"
  );
  require(
      reconnect.failed(start, Failure::RateLimited) >= start + rateLimitedBackoff,
      "Rate-limit closes wait a meaningful full cooldown"
  );
  for (int attempt = 0; attempt < 10; ++attempt)
    require(
        reconnect.failed(start, Failure::Transport) <= start + maximumBackoff,
        "Repeated failures retain a finite maximum delay"
    );
  require(
      reconnect.failed(start, Failure::InvalidSession, {}, 0) == start + 1s,
      "Invalid-session retries respect the minimum jitter"
  );
  require(
      reconnect.failed(start, Failure::InvalidSession, {}, 4000) == start + 5s,
      "Invalid-session retries respect the maximum jitter"
  );
  for (const auto random : {0u, 1u, 3999u, 4000u, 4001u, std::numeric_limits<std::uint32_t>::max()}) {
    const auto delay = invalidSessionDelay(random);
    require(delay >= 1s && delay <= 5s, "Every entropy input yields valid invalid-session jitter");
    const auto heartbeat = heartbeatDelay(30000ms, random);
    require(heartbeat >= 0ms && heartbeat < 30000ms, "The first heartbeat is jittered inside its announced interval");
  }
  require(
      heartbeatDelay(120000ms, std::numeric_limits<std::uint32_t>::max()) == 119999ms,
      "Heartbeat jitter avoids integer overflow at the longest interval"
  );
  return valid ? 0 : 1;
}
