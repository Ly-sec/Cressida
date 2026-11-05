#include "api/assets.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
  constexpr auto timeout = std::chrono::seconds(5);
  constexpr auto unpublishedWindow = std::chrono::milliseconds(50);
  constexpr std::size_t mebibyte = 1024 * 1024;
  constexpr std::size_t pendingByteLimit = 168 * mebibyte;
  constexpr std::size_t pendingResultLimit = 128;

  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }

  std::string path(int value) { return "/media/v1/collectibles-shop/" + std::to_string(value) + "/static"; }

  struct Call {
    std::string path;
    bool videoPoster = false;
    std::stop_token stop;
    bool released = false;
    unsigned char value = 0;
    std::weak_ptr<const media::Animation> animation;
  };

  class LoaderFixture {
  public:
    explicit LoaderFixture(
        bool ignoreCancellation = false, std::size_t encodedBytes = 1, std::size_t animationBytes = 64
    )
        : m_ignoreCancellation(ignoreCancellation), m_encodedBytes(encodedBytes), m_animationBytes(animationBytes) {}

    api::Asset load(const std::string& path, bool videoPoster, std::stop_token stop) {
      auto call = std::make_shared<Call>();
      call->path = path;
      call->videoPoster = videoPoster;
      call->stop = stop;
      std::stop_callback canceled(stop, [&] {
        std::lock_guard lock(m_mutex);
        m_ready.notify_all();
      });
      std::unique_lock lock(m_mutex);
      call->value = m_calls.size() + 1;
      m_calls.push_back(call);
      m_ready.notify_all();
      m_ready.wait(lock, [&] {
        return m_releaseAll || call->released || (!m_ignoreCancellation && stop.stop_requested());
      });
      auto animation = std::make_shared<media::Animation>();
      animation->frames.resize(2);
      animation->frames.front().rgba.resize(m_animationBytes / 2);
      animation->frames.back().rgba.resize(m_animationBytes - m_animationBytes / 2);
      call->animation = animation;
      std::vector<unsigned char> bytes(m_encodedBytes, call->value);
      m_ready.notify_all();
      return {path, std::move(bytes), std::move(animation)};
    }

    std::shared_ptr<Call> wait(const std::string& path, std::size_t occurrence = 1) {
      std::unique_lock lock(m_mutex);
      const auto find = [&]() -> std::shared_ptr<Call> {
        std::size_t count = 0;
        for (const auto& call : m_calls)
          if (call->path == path && ++count == occurrence)
            return call;
        return {};
      };
      require(m_ready.wait_for(lock, timeout, [&] { return bool(find()); }), "The requested worker call starts");
      return find();
    }

    void waitPrepared(const std::shared_ptr<Call>& call) {
      std::unique_lock lock(m_mutex);
      require(
          m_ready.wait_for(lock, timeout, [&] { return !call->animation.expired(); }),
          "The loader prepares its complete decoded result"
      );
    }

    void waitTotal(std::size_t count) {
      std::unique_lock lock(m_mutex);
      require(
          m_ready.wait_for(lock, timeout, [&] { return m_calls.size() >= count; }),
          "The workers reach the expected number of loaded requests"
      );
    }

    std::size_t count(const std::string& path) {
      std::lock_guard lock(m_mutex);
      return std::ranges::count_if(m_calls, [&](const auto& call) { return call->path == path; });
    }

    bool animationAlive(const std::shared_ptr<Call>& call) {
      std::lock_guard lock(m_mutex);
      return !call->animation.expired();
    }

    void release(const std::shared_ptr<Call>& call) {
      std::lock_guard lock(m_mutex);
      call->released = true;
      m_ready.notify_all();
    }

    void payload(std::size_t encodedBytes, std::size_t animationBytes) {
      std::lock_guard lock(m_mutex);
      m_encodedBytes = encodedBytes;
      m_animationBytes = animationBytes;
    }

    void releaseAll() {
      std::lock_guard lock(m_mutex);
      m_releaseAll = true;
      m_ready.notify_all();
    }

  private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::vector<std::shared_ptr<Call>> m_calls;
    bool m_ignoreCancellation = false, m_releaseAll = false;
    std::size_t m_encodedBytes = 1, m_animationBytes = 64;
  };

  struct ReleaseOnExit {
    LoaderFixture& fixture;
    ~ReleaseOnExit() { fixture.releaseAll(); }
  };

  void waitResult(api::Assets& assets) {
    pollfd ready{assets.eventFd(), POLLIN, 0};
    require(
        poll(&ready, 1, std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()) == 1,
        "A worker publishes its completed result"
    );
  }

  void consumeSignal(api::Assets& assets) {
    waitResult(assets);
    unsigned char signal = 0;
    require(
        read(assets.eventFd(), &signal, sizeof(signal)) == sizeof(signal),
        "Consume a completion signal without draining its queued result"
    );
  }

  bool hasSignal(api::Assets& assets) {
    pollfd ready{assets.eventFd(), POLLIN, 0};
    return poll(&ready, 1, unpublishedWindow.count()) == 1;
  }

  std::size_t resultBytes(const std::vector<api::Asset>& results) {
    std::size_t bytes = 0;
    for (const auto& result : results)
      bytes += result.bytes.capacity() + (result.animation ? result.animation->byteSize() : 0);
    return bytes;
  }

  std::shared_ptr<Call> complete(api::Assets& assets, LoaderFixture& fixture, const std::string& resource) {
    require(assets.request(resource), "Request a queue pressure fixture");
    const auto call = fixture.wait(resource);
    fixture.release(call);
    consumeSignal(assets);
    return call;
  }

  void testBytePressurePreservesAnimation() {
    LoaderFixture fixture(false, 2 * mebibyte, 83 * mebibyte);
    api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
    ReleaseOnExit release{fixture};
    complete(assets, fixture, path(1));
    require(assets.request(path(2)), "Load a second animation while the first result occupies the queue");
    const auto waiting = fixture.wait(path(2));
    fixture.release(waiting);
    fixture.waitPrepared(waiting);
    require(
        !hasSignal(assets) && fixture.animationAlive(waiting),
        "A complete animation waits for byte capacity instead of publishing an encoded-only still image"
    );
    auto results = assets.drain();
    require(
        results.size() == 1 && resultBytes(results) <= pendingByteLimit,
        "Queued results remain within the byte limit while a worker waits"
    );
    consumeSignal(assets);
    results = assets.drain();
    require(
        results.size() == 1
            && results.front().animation
            && results.front().animation->frames.size() == 2
            && resultBytes(results) <= pendingByteLimit,
        "Draining room publishes the waiting animation with all frames intact"
    );
  }

  void testCancelWaitingResultAndReenable() {
    LoaderFixture fixture(false, 2 * mebibyte, 83 * mebibyte);
    api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
    ReleaseOnExit release{fixture};
    complete(assets, fixture, path(1));
    require(assets.request(path(2)), "Create a decoded result waiting on byte capacity");
    const auto old = fixture.wait(path(2));
    fixture.release(old);
    fixture.waitPrepared(old);
    require(!hasSignal(assets), "The second result is held by backpressure");
    require(assets.request(path(3)), "Occupy the other worker with a separate loader barrier");
    fixture.wait(path(3));
    assets.cancel(path(2));
    require(old->stop.stop_requested() && assets.request(path(2)), "Cancel a capacity wait and reenable immediately");
    const auto replacement = fixture.wait(path(2), 2);
    require(
        !fixture.animationAlive(old) && !replacement->stop.stop_requested(),
        "Cancel wakes the blocked producer, releases old frames and starts a fresh same-path generation"
    );
    assets.cancel(path(1));
    fixture.release(replacement);
    consumeSignal(assets);
    const auto results = assets.drain();
    require(
        results.size() == 1
            && results.front().path == path(2)
            && results.front().animation
            && results.front().animation->frames.size() == 2,
        "Canceling a completed result frees capacity for the healthy replacement animation"
    );
  }

  void testShutdownWhileWaitingForBytes() {
    LoaderFixture fixture(false, 2 * mebibyte, 83 * mebibyte);
    std::shared_ptr<Call> first, waiting;
    {
      api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
      ReleaseOnExit release{fixture};
      first = complete(assets, fixture, path(1));
      require(assets.request(path(2)), "Start the shutdown byte-pressure fixture");
      waiting = fixture.wait(path(2));
      fixture.release(waiting);
      fixture.waitPrepared(waiting);
      require(!hasSignal(assets), "The shutdown fixture has a producer blocked on byte capacity");
    }
    require(
        waiting->stop.stop_requested() && !fixture.animationAlive(first) && !fixture.animationAlive(waiting),
        "Shutdown interrupts a capacity wait and releases completed and waiting decoded storage"
    );
  }

  void fillResultCapacity(api::Assets& assets, LoaderFixture& fixture) {
    require(assets.request(path(1)) && assets.request(path(2)), "Occupy both workers before filling result capacity");
    fixture.wait(path(1));
    fixture.wait(path(2));
    for (std::size_t index = 0; index < pendingResultLimit; ++index)
      require(assets.request(path(1000 + index)), "Queue work for the bounded completed-result fixture");
    fixture.releaseAll();
    fixture.waitTotal(pendingResultLimit + 2);
    for (std::size_t index = 0; index < pendingResultLimit; ++index)
      consumeSignal(assets);
    require(!hasSignal(assets), "Result count backpressure holds both remaining workers without overfilling");
  }

  void testResultCountPressure() {
    LoaderFixture fixture;
    api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
    ReleaseOnExit release{fixture};
    fillResultCapacity(assets, fixture);
    require(!assets.request(path(5000)), "A full completed-result queue retains its existing admission limit");
    auto results = assets.drain();
    require(
        results.size() == pendingResultLimit && resultBytes(results) < pendingByteLimit,
        "The result count limit applies independently of the byte budget"
    );
    consumeSignal(assets);
    consumeSignal(assets);
    results = assets.drain();
    require(
        results.size() == 2 && results.front().animation && results.back().animation,
        "Both count-blocked producers publish complete animations after drain releases slots"
    );
  }

  void testShutdownWhileBothProducersWait() {
    LoaderFixture fixture;
    std::shared_ptr<Call> last;
    {
      api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
      ReleaseOnExit release{fixture};
      fillResultCapacity(assets, fixture);
      last = fixture.wait(path(1000 + pendingResultLimit - 1));
    }
    require(
        last->stop.stop_requested() && !fixture.animationAlive(last),
        "Shutdown cancels and joins both workers even when neither can publish into the full result queue"
    );
  }

  void testOversizedResultFailsWithoutBlocking() {
    LoaderFixture fixture(false, pendingByteLimit + 1, 64);
    api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
    ReleaseOnExit release{fixture};
    const auto oversized = complete(assets, fixture, path(1));
    const auto results = assets.drain();
    require(
        results.size() == 1
            && results.front().bytes.capacity() == 0
            && !results.front().animation
            && !fixture.animationAlive(oversized),
        "A single impossible-to-admit custom result returns a bounded failure without partial success or deadlock"
    );
    fixture.payload(1, 64);
    complete(assets, fixture, path(2));
    const auto healthy = assets.drain();
    require(
        healthy.size() == 1 && healthy.front().animation && healthy.front().animation->frames.size() == 2,
        "An oversized failure leaves workers and capacity usable for the next healthy animation"
    );
  }

  void testQueuedCancellation() {
    LoaderFixture fixture;
    api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
    ReleaseOnExit release{fixture};
    require(!assets.request("/unapproved/media.png"), "Media validation is retained for injected loaders");
    require(assets.request(path(1)) && assets.request(path(2)), "Both existing workers accept work");
    const auto first = fixture.wait(path(1));
    const auto second = fixture.wait(path(2));
    require(assets.request(path(3)), "Queue a third request while both workers are occupied");
    assets.cancel(path(3));
    assets.cancel(path(3));
    require(assets.request(path(4)), "Cancellation leaves the queue usable");
    fixture.release(first);
    const auto barrier = fixture.wait(path(4));
    require(fixture.count(path(3)) == 0, "Canceled queued work is never passed to the loader");
    waitResult(assets);
    auto results = assets.drain();
    require(results.size() == 1 && results.front().path == first->path, "Only completed uncanceled work drains");
    fixture.release(barrier);
    waitResult(assets);
    results = assets.drain();
    require(results.size() == 1 && results.front().path == barrier->path, "Work after a canceled queue item completes");
    fixture.release(second);
    waitResult(assets);
    assets.drain();
    require(assets.request(path(3), true), "A canceled queued path can be requested again");
    const auto replacement = fixture.wait(path(3));
    require(replacement->videoPoster, "A replacement retains its independently requested decode mode");
    fixture.release(replacement);
    waitResult(assets);
    results = assets.drain();
    require(
        results.size() == 1 && results.front().bytes == std::vector<unsigned char>{replacement->value},
        "Only the replacement request publishes a canceled queued path"
    );
  }

  void testCompletedCancellation() {
    LoaderFixture fixture;
    api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
    ReleaseOnExit release{fixture};
    require(assets.request(path(1)), "Request a completed cancellation fixture");
    const auto completed = fixture.wait(path(1));
    fixture.release(completed);
    waitResult(assets);
    require(fixture.animationAlive(completed), "The completed queue owns its decoded animation");
    assets.cancel(path(1));
    require(
        assets.drain().empty() && !fixture.animationAlive(completed),
        "Cancel removes completed results and releases their decoded storage immediately"
    );
    require(assets.request(path(1)), "A canceled completed path can be requested again");
    const auto replacement = fixture.wait(path(1), 2);
    fixture.release(replacement);
    waitResult(assets);
    const auto results = assets.drain();
    require(
        results.size() == 1 && results.front().bytes == std::vector<unsigned char>{replacement->value},
        "A replacement publishes fresh data after its completed predecessor was discarded"
    );
  }

  void testInFlightGenerationRace() {
    LoaderFixture fixture(true);
    api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
    ReleaseOnExit release{fixture};
    require(assets.request(path(1)), "Request the first generation");
    const auto old = fixture.wait(path(1));
    assets.cancel(path(1));
    require(old->stop.stop_requested(), "Cancel requests stop on the actual in-flight loader token");
    require(assets.request(path(1), true), "Immediately reenable the same path while its old loader is still running");
    const auto replacement = fixture.wait(path(1), 2);
    require(
        !replacement->stop.stop_requested() && replacement->videoPoster,
        "The new generation has independent cancellation state and decode mode"
    );
    fixture.release(old);
    require(assets.request(path(2)), "Queue a barrier while the replacement occupies the other worker");
    const auto barrier = fixture.wait(path(2));
    fixture.release(barrier);
    waitResult(assets);
    auto results = assets.drain();
    require(
        results.size() == 1 && results.front().path == barrier->path,
        "A late old loader cannot publish over or remove the replacement's pending identity"
    );
    require(assets.request(path(1)), "A still-pending replacement remains coalesced after the old loader finishes");
    require(fixture.count(path(1)) == 2, "Old completion cannot accidentally enable a duplicate request");
    fixture.release(replacement);
    waitResult(assets);
    results = assets.drain();
    require(
        results.size() == 1 && results.front().bytes == std::vector<unsigned char>{replacement->value},
        "Only the newest in-flight generation reaches the application"
    );
  }

  void testQueueLimits() {
    LoaderFixture fixture;
    api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
    ReleaseOnExit release{fixture};
    require(assets.request(path(1)) && assets.request(path(2)), "Occupy both workers for bounded queue checks");
    fixture.wait(path(1));
    fixture.wait(path(2));
    for (int index = 0; index < 128; ++index)
      require(assets.request(path(1000 + index)), "Fill the existing bounded request queue");
    require(!assets.request(path(3000)), "Cancellation support retains the existing queue capacity");
    assets.cancel(path(1000));
    require(assets.request(path(3000)), "Canceling queued work immediately releases its queue slot");
  }

  void testShutdownCancellation() {
    LoaderFixture fixture;
    std::shared_ptr<Call> first, second;
    {
      api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
      require(assets.request(path(1)) && assets.request(path(2)), "Start both worker shutdown fixtures");
      first = fixture.wait(path(1));
      second = fixture.wait(path(2));
    }
    require(
        first->stop.stop_requested() && second->stop.stop_requested(),
        "Destruction stops both in-flight requests before joining their workers"
    );
  }
  void testFullAnimationResult() {
    LoaderFixture fixture(false, 40 * mebibyte, media::animationStorageLimit);
    api::Assets assets([&](const auto& path, bool poster, auto stop) { return fixture.load(path, poster, stop); });
    ReleaseOnExit release{fixture};
    complete(assets, fixture, path(1));
    const auto results = assets.drain();
    require(
        results.size() == 1
            && results.front().animation
            && results.front().animation->byteSize() == media::animationStorageLimit
            && resultBytes(results) == pendingByteLimit,
        "A full 128 MiB animation and encoded-buffer capacity fit the pending-result limit exactly"
    );
  }
} // namespace

int main() {
  try {
    testQueuedCancellation();
    testCompletedCancellation();
    testInFlightGenerationRace();
    testQueueLimits();
    testShutdownCancellation();
    testFullAnimationResult();
    testBytePressurePreservesAnimation();
    testCancelWaitingResultAndReenable();
    testShutdownWhileWaitingForBytes();
    testResultCountPressure();
    testShutdownWhileBothProducersWait();
    testOversizedResultFailsWithoutBlocking();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
