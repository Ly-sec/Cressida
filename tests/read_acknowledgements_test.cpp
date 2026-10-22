#include "app/read_acknowledgements.h"
#include "app/unread_timeline.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

int main() {
  const auto require = [](bool value, const char* reason) {
    if (!value)
      throw std::runtime_error(reason);
  };
  try {
    using Acks = app::ReadAcknowledgements;
    using namespace std::chrono_literals;
    const auto start = Acks::TimePoint{};
    Acks acks;
    require(!acks.delay(start) && !acks.take(start), "Idle read state has no timer or request");
    require(!acks.queue("bad", "100") && !acks.queue("10", ""), "Invalid cursors never enter the queue");
    require(acks.queue("10", "99"), "The first viewed cursor is accepted");
    auto sent = acks.take(start);
    require(sent && sent->channel == "10" && sent->message == "99", "The first automatic read sends immediately");
    require(!acks.delay(start), "There is no idle polling timer after a send");
    require(acks.queue("10", "100") && acks.queue("10", "101"), "A live burst coalesces by channel");
    require(acks.queue("10", "98") && acks.pending() == 1, "A stale cursor never rewinds a pending read");
    require(
        acks.delay(start + 1s) == 1s && !acks.take(start + 1999ms), "Automatic traffic observes the shared cooldown"
    );
    sent = acks.take(start + 2s);
    require(sent && sent->message == "101", "Only the newest captured viewed cursor is sent");

    require(
        acks.queue("10", "102") && acks.queue("20", "200"), "Navigation preserves captured reads for both channels"
    );
    sent = acks.take(start + 4s);
    require(sent && sent->channel == "10" && sent->message == "102", "A prior channel dispatches its captured cursor");
    require(!acks.take(start + 4s), "Different channels share the automatic request budget");
    sent = acks.take(start + 6s);
    require(
        sent && sent->channel == "20" && sent->message == "200", "A second channel is dispatched without starvation"
    );
    require(!acks.take(start + 10s), "A failed or unconfirmed write is never implicitly retried");

    require(acks.queue("10", "103"), "Another viewed cursor is queued");
    acks.acknowledged("10", "104", start + 7s);
    require(!acks.delay(start + 7s), "An explicit newer read cancels an obsolete deferred read");
    require(
        acks.queue("10", "105") && !acks.take(start + 8s), "An explicit read also delays the next automatic request"
    );
    sent = acks.take(start + 9s);
    require(sent && sent->message == "105", "Automatic reads resume at the explicit read's cooldown");

    require(acks.queue("10", "106"), "A newer viewed cursor remains pending");
    acks.acknowledged("10", "105", start + 10s);
    require(acks.pending() == 1, "An older explicit read never discards a newer captured cursor");
    acks.cancel("10");
    require(!acks.delay(start + 20s), "Manual unread, deletion or leaving a server cancels its pending cursor");

    acks.reset();
    for (std::size_t i = 0; i < Acks::pendingChannelLimit; ++i)
      require(acks.queue(std::to_string(i + 1), "100"), "The bounded channel backlog accepts available slots");
    require(!acks.queue("999", "100") && acks.queue("1", "101"), "A full backlog still coalesces existing channels");
    acks.cancel("2");
    require(acks.queue("999", "100"), "Cancellation frees the bounded channel slot");
    acks.reset();
    require(!acks.delay(start), "Account reset cancels all pending cursors and timers");
    require(
        acks.queue("30", "300") && acks.take(start).has_value(), "A new account starts with an immediate first read"
    );

    acks.reset();
    int requests = 0;
    std::string last;
    constexpr int liveMessages = 1000;
    constexpr auto arrivalSpacing = 10ms;
    for (int i = 0; i < liveMessages; ++i) {
      const auto now = start + arrivalSpacing * i;
      require(acks.queue("10", std::to_string(1000 + i)), "Busy channels accept newer viewed messages");
      if (const auto cursor = acks.take(now)) {
        ++requests;
        last = cursor->message;
      }
    }
    if (const auto cursor = acks.take(start + arrivalSpacing * liveMessages)) {
      ++requests;
      last = cursor->message;
    }
    require(
        requests == 6 && last == "1999", "A thousand live arrivals send six latest-only acknowledgements in ten seconds"
    );

    app::UnreadTimeline timeline;
    timeline.open("10");
    timeline.snapshot({{"10", {"99", "103"}}});
    timeline.positioned();
    timeline.acknowledged("10", "101");
    acks.reset();
    require(acks.queue("10", "101"), "A captured viewed cursor is accepted before hiding");
    timeline.snapshot({{"10", {"99", "104"}}});
    sent = acks.take(start);
    require(
        sent && sent->message == "101" && timeline.cursor("10")->after == "101",
        "Hidden or newer arrivals cannot advance a captured read"
    );
    timeline.acknowledged("10", "102");
    require(acks.queue("10", "102"), "A newer viewed cursor has an independent rollback boundary");
    timeline.failed("10", "101");
    require(timeline.cursor("10")->after == "102", "An older failed request cannot undo a newer queued local read");
    timeline.failed("10", "102");
    require(timeline.cursor("10")->after == "99", "A failed newest request restores the authoritative unread cursor");
    acks.cancel("10");
    require(!acks.take(start + 10s), "A canceled failed read is not retried");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
