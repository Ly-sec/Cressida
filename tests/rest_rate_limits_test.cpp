#include "api/rest_rate_limits.h"

#include <atomic>
#include <barrier>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
  using Limits = api::RestRateLimits;
  using namespace std::chrono_literals;
  const auto now = Limits::Clock::time_point(1000s);
  const auto wall = std::chrono::sys_days(std::chrono::year(2026) / 10 / 22) + 12h;

  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }

  Limits::Headers headers(std::initializer_list<std::string_view> lines) {
    Limits::Headers result;
    for (const auto line : lines)
      result.add(line);
    return result;
  }

  void finish(
      Limits::Admission& admission, long status, const Limits::Headers& metadata = {},
      const Limits::Fallback& fallback = {}, Limits::Clock::time_point at = now
  ) {
    require(static_cast<bool>(admission.lease), "A response requires a reserved transfer");
    admission.lease.complete(status, metadata, fallback, at, wall);
  }

  void unknownProbes() {
    Limits limits;
    auto first = limits.acquire("GET", "/channels/10/messages?before=20", now);
    require(static_cast<bool>(first.lease), "An unknown route permits one probe");
    const auto blocked = limits.acquire("GET", "/channels/10/messages?after=30", now);
    require(
        !blocked.lease && blocked.retryAt == now + Limits::probeInterval,
        "Query cursors share the in-flight unknown probe"
    );
    require(
        static_cast<bool>(limits.acquire("POST", "/channels/10/messages", now).lease),
        "Unknown routes keep methods independent"
    );
    require(
        static_cast<bool>(limits.acquire("GET", "/channels/11/messages", now).lease),
        "A channel major keeps unrelated probes independent"
    );
    first.lease = {};
    require(
        static_cast<bool>(limits.acquire("GET", "/channels/10/messages", now).lease),
        "Cancelling a probe releases admission immediately"
    );
    auto noHeaders = limits.acquire("GET", "/users/@me", now);
    finish(noHeaders, 200);
    auto one = limits.acquire("GET", "/users/@me", now);
    require(
        static_cast<bool>(one.lease) && !limits.acquire("GET", "/users/@me", now).lease,
        "A successful route without budget headers remains serialized"
    );

    std::barrier start(5), finishProbe(5);
    std::atomic<unsigned> admitted{0};
    std::vector<std::jthread> workers;
    for (unsigned i = 0; i < 4; ++i)
      workers.emplace_back([&] {
        start.arrive_and_wait();
        auto attempt = limits.acquire("GET", "/guilds/20/channels", now);
        if (attempt.lease)
          ++admitted;
        finishProbe.arrive_and_wait();
      });
    start.arrive_and_wait();
    finishProbe.arrive_and_wait();
    workers.clear();
    require(admitted == 1, "Concurrent worker lanes reserve exactly one unknown-route probe");
  }

  void learnedReservations() {
    Limits limits;
    auto initial = limits.acquire("GET", "/channels/10/messages", now);
    finish(
        initial, 200,
        headers(
            {"X-RateLimit-Bucket: messages", "X-RateLimit-Limit: 3", "X-RateLimit-Remaining: 3",
             "X-RateLimit-Reset-After: 2.25"}
        )
    );
    std::vector<Limits::Lease> leases;
    for (unsigned i = 0; i < 3; ++i) {
      auto reserved = limits.acquire("GET", "/channels/10/messages", now);
      require(static_cast<bool>(reserved.lease), "Learned allowance permits its advertised remaining requests");
      leases.push_back(std::move(reserved.lease));
    }
    auto exhausted = limits.acquire("GET", "/channels/10/messages", now);
    require(
        !exhausted.lease && exhausted.retryAt == now + 2250ms + Limits::safetyMargin,
        "Concurrent reservations exhaust allowance before responses arrive"
    );
    leases[1].complete(200, headers({"X-RateLimit-Remaining: 2"}), {}, now, wall);
    leases[0].complete(200, headers({"X-RateLimit-Remaining: 3"}), {}, now, wall);
    leases[2].complete(200, headers({"X-RateLimit-Remaining: 1"}), {}, now, wall);
    require(
        !limits.acquire("GET", "/channels/10/messages", now + 2s).lease,
        "Out-of-order larger remaining headers cannot replenish an exhausted window"
    );
    auto renewed = limits.acquire("GET", "/channels/10/messages", exhausted.retryAt);
    require(static_cast<bool>(renewed.lease), "Allowance refills only after its monotonic reset");
    finish(renewed, 200, headers({"X-RateLimit-Remaining: 2", "X-RateLimit-Reset-After: 1"}), {}, exhausted.retryAt);
  }

  void concurrentAllowance() {
    Limits limits;
    auto initial = limits.acquire("GET", "/channels/10/messages", now);
    finish(initial, 200, headers({"X-RateLimit-Limit: 2", "X-RateLimit-Remaining: 2", "X-RateLimit-Reset-After: 5"}));
    std::barrier start(5), held(5);
    std::atomic<unsigned> admitted{0};
    std::vector<std::jthread> workers;
    for (unsigned i = 0; i < 4; ++i)
      workers.emplace_back([&] {
        start.arrive_and_wait();
        auto attempt = limits.acquire("GET", "/channels/10/messages", now);
        if (attempt.lease)
          ++admitted;
        held.arrive_and_wait();
      });
    start.arrive_and_wait();
    held.arrive_and_wait();
    workers.clear();
    require(admitted == 2, "Concurrent workers reserve at most the known shared allowance");
    require(
        !limits.acquire("GET", "/channels/10/messages", now).lease,
        "Releasing uncertain transfers cannot refund and repeat their spent allowance"
    );
  }

  void bucketAliases() {
    Limits limits;
    auto a = limits.acquire("GET", "/channels/10/messages", now);
    finish(
        a, 200,
        headers(
            {"X-RateLimit-Bucket: shared", "X-RateLimit-Limit: 2", "X-RateLimit-Remaining: 2",
             "X-RateLimit-Reset-After: 5"}
        )
    );
    auto b = limits.acquire("POST", "/channels/10/messages", now);
    finish(
        b, 200,
        headers(
            {"X-RateLimit-Bucket: shared", "X-RateLimit-Limit: 2", "X-RateLimit-Remaining: 1",
             "X-RateLimit-Reset-After: 5"}
        )
    );
    auto final = limits.acquire("GET", "/channels/10/messages", now);
    require(
        static_cast<bool>(final.lease) && !limits.acquire("POST", "/channels/10/messages", now).lease,
        "A learned server hash shares reserved allowance across method aliases"
    );
    auto major = limits.acquire("POST", "/channels/11/messages", now);
    finish(
        major, 200,
        headers(
            {"X-RateLimit-Bucket: shared", "X-RateLimit-Limit: 2", "X-RateLimit-Remaining: 1",
             "X-RateLimit-Reset-After: 5"}
        )
    );
    require(
        static_cast<bool>(limits.acquire("POST", "/channels/11/messages", now).lease),
        "Identical server hashes never merge different channel majors"
    );
    require(
        static_cast<bool>(limits.acquire("DELETE", "/channels/10/messages/55", now).lease),
        "An unaliased method and route remain independent"
    );
    auto webhook = limits.acquire("POST", "/webhooks/30/token-a", now);
    finish(webhook, 429, headers({"Retry-After: 2", "X-RateLimit-Bucket: webhook"}));
    require(
        static_cast<bool>(limits.acquire("POST", "/webhooks/30/token-b", now).lease),
        "Webhook token is part of the major identity"
    );
    require(
        !limits.acquire("POST", "/webhooks/30/token-a?wait=true", now).lease,
        "Webhook queries cannot bypass their token's cooldown"
    );
  }

  void responseHeadersAndFallbacks() {
    Limits limits;
    auto a = limits.acquire("GET", "/channels/10/messages/11/ack", now);
    finish(a, 429, headers({"Retry-After: 0.125", "X-RateLimit-Scope: shared"}));
    const auto same = limits.acquire("GET", "/channels/10/messages/22/ack", now);
    require(
        !same.lease && same.retryAt == now + 125ms + Limits::safetyMargin,
        "Fractional header-only 429s block normalized non-major IDs"
    );
    require(
        static_cast<bool>(limits.acquire("GET", "/channels/11/messages/22/ack", now).lease),
        "A shared-scope 429 is not a global cooldown"
    );

    auto global = limits.acquire("POST", "/channels/11/messages", now);
    finish(global, 429, headers({"Retry-After: 120.75", "X-RateLimit-Global: true"}));
    const auto stopped = limits.acquire("GET", "/users/@me", now);
    require(
        !stopped.lease && stopped.retryAt == now + 120750ms + Limits::safetyMargin,
        "Header-only global limits pause unrelated workers for their full advertised delay"
    );
    limits.clear();
    auto body = limits.acquire("PUT", "/users/@me/settings", now);
    finish(body, 429, headers({"Retry-After: 1"}), {.retryAfter = 2.5, .global = true});
    require(
        limits.acquire("GET", "/users/@me", now).retryAt == now + 2500ms + Limits::safetyMargin,
        "JSON fallback and headers use the longer wait and either global signal"
    );
    limits.clear();
    auto malformed = limits.acquire("GET", "/users/@me", now);
    finish(
        malformed, 429, headers({"Retry-After: nan", "X-RateLimit-Reset-After: -5"}),
        {.retryAfter = std::numeric_limits<double>::infinity()}
    );
    require(
        limits.acquire("GET", "/users/@me", now).retryAt == now + Limits::unknownLimitDelay + Limits::safetyMargin,
        "Malformed or missing 429 timing gets a conservative finite delay"
    );
    limits.clear();
    auto success = limits.acquire("GET", "/users/@me", now);
    finish(success, 200, headers({"X-RateLimit-Remaining: 0", "X-RateLimit-Reset-After: 0.5"}));
    require(
        limits.acquire("GET", "/users/@me", now).retryAt == now + 500ms + Limits::safetyMargin,
        "A successful response with zero remaining pauses the next request proactively"
    );
    limits.clear();
    auto partial = limits.acquire("GET", "/users/@me", now);
    finish(partial, 200, headers({"X-RateLimit-Remaining: 0"}));
    require(
        limits.acquire("GET", "/users/@me", now).retryAt == now + Limits::unknownResetDelay,
        "Missing reset metadata cannot turn zero remaining into immediate traffic"
    );
  }

  void datesAndHeaderBlocks() {
    Limits limits;
    auto dated = limits.acquire("GET", "/users/@me", now);
    finish(dated, 429, headers({"Date: Thu, 22 Oct 2026 12:00:00 GMT", "Retry-After: Thu, 22 Oct 2026 12:00:03 GMT"}));
    require(
        limits.acquire("GET", "/users/@me", now).retryAt == now + 3s + Limits::safetyMargin,
        "HTTP-date Retry-After is converted to a monotonic cooldown"
    );
    limits.clear();
    const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(wall.time_since_epoch()).count();
    auto absolute = limits.acquire("GET", "/users/@me", now);
    finish(
        absolute, 200,
        headers(
            {"X-RateLimit-Remaining: 0", std::string("X-RateLimit-Reset: ") + std::to_string(epoch + 5),
             "Date: Thu, 22 Oct 2026 12:00:00 GMT"}
        )
    );
    require(
        limits.acquire("GET", "/users/@me", now).retryAt == now + 5s + Limits::safetyMargin,
        "Absolute resets use the response date as a clock-skew-safe fallback"
    );
    limits.clear();
    auto preferred = limits.acquire("GET", "/users/@me", now);
    finish(
        preferred, 200,
        headers(
            {"X-RateLimit-Remaining: 0", "X-RateLimit-Reset-After: 0.25",
             std::string("X-RateLimit-Reset: ") + std::to_string(epoch + 1000)}
        )
    );
    require(
        limits.acquire("GET", "/users/@me", now).retryAt == now + 250ms + Limits::safetyMargin,
        "Relative monotonic reset metadata takes priority over absolute wall-clock metadata"
    );

    auto metadata = headers(
        {"HTTP/1.1 200 Connection established\r\n", "Retry-After: 9999", "X-RateLimit-Global: true",
         "HTTP/2 103 Early Hints\r\n", "X-RateLimit-Bucket: early", "HTTP/2 200\r\n", "x-ratelimit-remaining: 5",
         "X-RateLimit-Remaining: 2", "X-RateLimit-Reset-After: 0.25", "X-RateLimit-Reset-After: 0.5",
         "X-RateLimit-Bucket: final", "X-RateLimit-Bucket: conflicting", "X-RateLimit-Bucket: final"}
    );
    require(
        !metadata.global
            && !metadata.retryAfter
            && metadata.bucket.empty()
            && metadata.remaining == 2
            && metadata.resetAfter == 0.5,
        "Only final response headers survive and conflicting duplicates cannot increase allowance or alias routes"
    );
    metadata = headers(
        {"X-RateLimit-Remaining: -1", "X-RateLimit-Remaining: 1000001", "X-RateLimit-Limit: 2.5",
         "X-RateLimit-Reset-After: 1 trailing", "Retry-After: Thu, 32 Oct 2026 12:00:00 GMT",
         "Retry-After: Thu, 22 Oct 9999 12:00:00 GMT", "X-RateLimit-Bucket: contains space"}
    );
    require(
        !metadata.remaining
            && !metadata.limit
            && !metadata.resetAfter
            && !metadata.retryDate
            && metadata.bucket.empty(),
        "Malformed counts, timing, calendar overflow and bucket names do not enter admission state"
    );
    const auto unrelated = std::string("X-Unrelated: ") + std::string(9000, 'x');
    for (unsigned i = 0; i < 8; ++i)
      metadata.add(unrelated);
    metadata.add("Retry-After: 120\r\n");
    metadata.add("X-RateLimit-Global: true\r\n");
    require(
        metadata.retryAfter == 120 && metadata.global,
        "Large unrelated header blocks cannot hide later bounded retry and global metadata"
    );
    metadata.add(std::string(20 * 1024, 'x'));
    metadata.add("X-RateLimit-Remaining: 1\r\n");
    require(metadata.remaining == 1, "An oversized irrelevant header cannot prevent later bounded metadata");
  }

  void lifecycleAndBounds() {
    Limits limits;
    auto stale = limits.acquire("GET", "/users/@me", now);
    limits.clear();
    auto current = limits.acquire("GET", "/users/@me", now);
    finish(stale, 429, headers({"Retry-After: 500", "X-RateLimit-Global: true"}));
    require(
        !limits.acquire("GET", "/users/@me", now).lease,
        "An old response cannot release a new session's unknown-route permit"
    );
    require(
        static_cast<bool>(limits.acquire("GET", "/guilds/20/channels", now).lease),
        "An old response cannot poison the new session with a global cooldown"
    );
    current.lease = {};
    require(
        static_cast<bool>(limits.acquire("GET", "/users/@me", now).lease),
        "Cancellation releases the new generation's own permit"
    );

    std::vector<Limits::Lease> occupied;
    for (std::size_t i = 0; i < Limits::maximumRoutes; ++i) {
      auto admission = limits.acquire("GET", "/channels/" + std::to_string(i + 100) + "/messages", now);
      require(static_cast<bool>(admission.lease), "Each bounded route may retain an active probe");
      occupied.push_back(std::move(admission.lease));
    }
    auto overflow = limits.acquire("GET", "/channels/9999/messages", now);
    require(
        static_cast<bool>(overflow.lease) && !limits.acquire("GET", "/channels/10000/messages", now).lease,
        "At capacity new routes use one conservative overflow probe without evicting active transfers"
    );
    finish(overflow, 429, headers({"Retry-After: 5"}));
    require(
        !limits.acquire("GET", "/channels/10000/messages", now).lease,
        "Overflow routes retain a cooldown rather than forgetting it at capacity"
    );
    require(
        !limits.acquire("GET", "/channels/100/messages", now).lease,
        "Capacity pressure never evicts an existing in-flight route"
    );
    limits.clear();
    occupied.clear();
    auto safe = limits.acquire("GET", "/channels/100/messages", now);
    finish(safe, 200);
    require(
        static_cast<bool>(limits.acquire("GET", "/channels/100/messages", now).lease),
        "Clearing and releasing old capacity cannot strand the new session"
    );

    Limits active;
    const auto exhaustedHeaders = headers(
        {"X-RateLimit-Bucket: occupied", "X-RateLimit-Limit: 1", "X-RateLimit-Remaining: 0",
         "X-RateLimit-Reset-After: 10"}
    );
    for (std::size_t i = 0; i < Limits::maximumRoutes; ++i) {
      auto admission = active.acquire("GET", "/channels/" + std::to_string(i + 100) + "/messages", now);
      finish(admission, 200, exhaustedHeaders);
    }
    auto extra = active.acquire("GET", "/channels/9999/messages", now);
    finish(extra, 429, headers({"Retry-After: 1", "X-RateLimit-Bucket: extra"}));
    const auto retained = active.acquire("GET", "/channels/100/messages", now);
    require(
        !retained.lease && retained.retryAt == now + 10s + Limits::safetyMargin,
        "Full route and bucket maps never evict a learned active cooldown"
    );
    auto clamped = active.acquire("GET", "/channels/9999/messages", now + 2s);
    finish(clamped, 429, headers({"Retry-After: 1e300"}), {}, now + 2s);
    require(
        active.acquire("GET", "/channels/9999/messages", now + 2s).retryAt
            == now + 2s + Limits::maximumDelay + Limits::safetyMargin,
        "Huge finite delays are bounded without floating-point or duration overflow"
    );

    Limits::Lease surviving;
    {
      Limits temporary;
      auto admission = temporary.acquire("GET", "/users/@me", now);
      surviving = std::move(admission.lease);
    }
    surviving = {};
  }
} // namespace

int main() {
  try {
    unknownProbes();
    learnedReservations();
    concurrentAllowance();
    bucketAliases();
    responseHeadersAndFallbacks();
    datesAndHeaderBlocks();
    lifecycleAndBounds();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
