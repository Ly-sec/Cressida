#include "api/stream_manager.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <stdexcept>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  template <class Predicate> void await(api::StreamManager& manager, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (!predicate(manager.snapshot())) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      require(remaining.count() > 0, "Stream state settles within the private test deadline");
      pollfd descriptor{manager.eventFd(), POLLIN, 0};
      ::poll(&descriptor, 1, remaining.count());
      manager.drainEvents();
    }
  }
} // namespace
int main() {
  try {
    std::mutex mutex;
    std::vector<api::StreamCommand> commands;
    std::atomic<unsigned> previewUploads = 0;
    api::StreamManager manager(
        [&](std::uint64_t generation, api::StreamCommand command) {
          require(generation == 7 || generation == 8, "Stream commands retain their account generation");
          std::lock_guard lock(mutex);
          commands.push_back(command);
          return true;
        },
        [&](api::StreamPreviewQuery) {
          ++previewUploads;
          return true;
        }
    );
    const auto count = [&](api::StreamCommandKind kind) {
      std::lock_guard lock(mutex);
      return std::ranges::count(commands, kind, &api::StreamCommand::kind);
    };
    manager.parent({7, "1", "2", "3", "private-parent-session"});
    await(manager, [](const auto& snapshot) { return snapshot.channel == "2"; });
    manager.roster(7, {.members = {{.guild = "1", .channel = "2", .user = "4", .stream = true}}});
    await(manager, [](const auto& snapshot) { return snapshot.available.size() == 1; });
    auto frame = std::make_shared<voice::VideoFrame>();
    frame->width = 16;
    frame->height = 16;
    frame->stride = frame->width * 4;
    frame->rgba.resize(frame->stride * frame->height, 255);
    manager.preview()->source(frame);
    require(manager.start({}), "Explicit publication queues its intention");
    await(manager, [](const auto& snapshot) { return snapshot.active.size() == 1; });
    require(
        count(api::StreamCommandKind::Create) == 1 && !manager.sharedAudio()->state().running,
        "Publication signaling does not start desktop audio before RTC setup"
    );
    const auto previewDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{650};
    while (std::chrono::steady_clock::now() < previewDeadline) {
      pollfd descriptor{manager.eventFd(), POLLIN, 0};
      ::poll(&descriptor, 1, 25);
      manager.drainEvents();
    }
    require(
        previewUploads == 0,
        "A consented local frame and queued publication never upload a thumbnail before RTC readiness"
    );
    const auto failedRevision = manager.snapshot().active.front().connection.revision;
    manager.event(7, "STREAM_COMMAND_FAILED", {{"stream_key", "guild:1:2:3"}});
    await(manager, [](const auto& snapshot) {
      return snapshot.active.size() == 1 && snapshot.active.front().connection.phase == voice::Phase::Failed;
    });
    require(
        count(api::StreamCommandKind::Create) == 1 && !manager.snapshot().active.front().connection.error.empty(),
        "A failed publication stays explicit and never retries automatically"
    );
    require(manager.start({}), "An explicit action can retry a failed publication");
    await(manager, [&](const auto& snapshot) {
      return snapshot.active.size() == 1
          && snapshot.active.front().connection.phase == voice::Phase::Connecting
          && snapshot.active.front().connection.revision > failedRevision;
    });
    require(count(api::StreamCommandKind::Create) == 2, "Retry submits one new publication intention");
    manager.event(
        7, "STREAM_SERVER_UPDATE",
        {{"stream_key", "guild:1:2:3"}, {"endpoint", "synthetic.discord.media"}, {"token", "stale-reply"}}
    );
    require(manager.watch("guild:1:2:4"), "A second participant's stream can be watched while publishing");
    await(manager, [](const auto& snapshot) { return snapshot.active.size() == 2; });
    require(count(api::StreamCommandKind::Watch) == 1, "The watch request is independent from publication");
    manager.pause(true);
    await(manager, [](const auto& snapshot) {
      return std::ranges::any_of(snapshot.active, [](const auto& view) { return view.publishing && view.paused; });
    });
    require(count(api::StreamCommandKind::Pause) == 1, "An explicit pause sends one signaling intention");
    manager.stop("guild:1:2:4");
    await(manager, [](const auto& snapshot) { return snapshot.active.size() == 1; });
    manager.event(7, "STREAM_DELETE", {{"stream_key", "guild:1:2:4"}});
    await(manager, [](const auto& snapshot) { return snapshot.available.size() == 1; });
    require(
        manager.snapshot().available.front().key.owner == "4",
        "Stopping a watch preserves discovery while the participant is still live"
    );
    manager.roster(7, {.members = {{.guild = "1", .channel = "2", .user = "4", .stream = false}}});
    await(manager, [](const auto& snapshot) { return snapshot.available.empty(); });
    manager.roster(7, {.members = {{.guild = "1", .channel = "2", .user = "3", .stream = true}}});
    await(manager, [](const auto& snapshot) { return snapshot.available.size() == 1; });
    manager.stop();
    await(manager, [](const auto& snapshot) { return snapshot.active.empty() && snapshot.available.empty(); });
    require(
        count(api::StreamCommandKind::Delete) == 3,
        "Stopping watching and publishing sends their separate delete intentions"
    );
    manager.reset(true);
    await(manager, [](const auto& snapshot) { return snapshot.generation == 0 && snapshot.channel.empty(); });
    manager.parent({8, "1", "2", "5", "new-private-parent-session"});
    await(manager, [](const auto& snapshot) { return snapshot.generation == 8; });
    manager.event(
        7, "STREAM_CREATE", {{"stream_key", "guild:1:2:4"}, {"rtc_server_id", "10"}, {"rtc_channel_id", "11"}}
    );
    manager.roster(8, {.members = {{.guild = "1", .channel = "2", .user = "6", .stream = true}}});
    await(manager, [](const auto& snapshot) { return snapshot.available.size() == 1; });
    require(
        manager.snapshot().available.front().key.owner == "6" && manager.snapshot().active.empty(),
        "An old account's setup event cannot repopulate stream discovery or resume capture"
    );
    std::cout << "Stream lifecycle and account isolation passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
