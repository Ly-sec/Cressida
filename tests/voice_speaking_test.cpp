#include "voice/speaking.h"

#include <iostream>
#include <stdexcept>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
} // namespace
int main() {
  using namespace std::chrono_literals;
  using Activity = voice::SpeakingActivity;
  const auto now = Activity::Clock::time_point{};
  try {
    Activity activity("1");
    require(activity.hint("2", true, now) && activity.active("2", now), "Remote speaking hints appear immediately");
    require(!activity.hint("2", true, now + 100ms), "Repeated hints refresh activity without redundant state changes");
    require(
        !activity.expire(now + 399ms) && activity.active("2", now + 399ms),
        "Refreshed remote indicators retain a bounded hold"
    );
    require(
        activity.expire(now + 400ms) && !activity.active("2", now + 400ms),
        "A one-shot speaking hint cannot leave a stuck indicator"
    );
    require(!activity.packet("2", false, false, now), "Unauthenticated audio cannot create a speaking indicator");
    require(
        activity.packet("2", true, false, now), "Authenticated non-silence audio indicates speaking without an op5 hint"
    );
    require(
        !activity.packet("2", false, true, now + 50ms) && activity.active("2", now + 50ms),
        "Unauthenticated silence cannot clear a genuine speaker"
    );
    require(
        activity.packet("2", true, true, now + 100ms) && !activity.active("2", now + 100ms),
        "Authenticated explicit silence clears the speaker immediately"
    );
    activity.hint("2", true, now);
    require(
        activity.hint("2", false, now + 1ms) && !activity.active("2", now + 1ms),
        "An explicit remote stop clears the indicator without waiting"
    );
    require(
        !activity.hint("1", true, now) && !activity.packet("1", true, false, now) && !activity.active("1", now),
        "Self echoes and remote hints never replace local microphone activity"
    );
    require(
        activity.capture(true, false, false, now) && activity.active("1", now),
        "Actual local voice activity starts the local indicator"
    );
    require(
        activity.capture(true, true, false, now + 1ms) && !activity.active("1", now + 1ms),
        "Muting suppresses stale voiced capture immediately"
    );
    require(
        !activity.capture(true, false, true, now + 2ms) && !activity.active("1", now + 2ms),
        "Deafening also suppresses local microphone indications"
    );
    activity.capture(true, false, false, now + 3ms);
    require(
        activity.capture(false, false, false, now + 4ms) && !activity.active("1", now + 4ms),
        "VAD and push-to-talk release clear the local indicator immediately"
    );
    activity.capture(true, false, false, now);
    require(
        activity.expire(now + Activity::hold) && !activity.active("1", now + Activity::hold),
        "A stalled capture worker cannot leave local activity stuck"
    );
    activity.packet("2", true, false, now);
    activity.packet("3", true, false, now + 50ms);
    require(
        activity.remove("2") && !activity.active("2", now + 100ms) && activity.active("3", now + 100ms),
        "Removing a participant preserves unrelated speaker state"
    );
    activity.capture(true, false, false, now);
    require(
        activity.clear() && !activity.active("1", now) && !activity.active("3", now) && !activity.clear(),
        "Rekey and disconnect clear all local and remote activity"
    );
    for (std::size_t i = 2; i <= voice::protocol::maximumParticipants; ++i)
      require(
          activity.hint(std::to_string(i), true, now), "The bounded participant roster can indicate each remote member"
      );
    require(
        !activity.hint(std::to_string(voice::protocol::maximumParticipants + 1), true, now),
        "Unexpected extra speakers cannot grow the tracker beyond the roster bound"
    );
    require(
        !activity.hint("invalid", true, now) && !activity.hint("0", true, now),
        "Invalid participant identities are never tracked"
    );
    std::cout << "Injected-clock local and authenticated remote speaking activity passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
