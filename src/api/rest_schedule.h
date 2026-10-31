#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stop_token>

namespace api {

  enum class RestCancellation { None, Stopped, SessionChanged, ForegroundReady, Superseded };

  struct RestReadIntent {
    std::atomic<std::uint64_t> navigation{0};
    std::atomic<bool> foreground{false}, cancelled{false};

    void promote(std::uint64_t revision) {
      navigation = revision;
      foreground = true;
    }
  };

  // A background GET may yield to accepted user work, but a mutation must only
  // be cancelled by shutdown or an account/session change. Publication preview
  // updates can additionally be revoked when sharing stops or previews are hidden.
  struct RestRequestScope {
    std::stop_token stop;
    const std::atomic<std::uint64_t>* session = nullptr;
    std::uint64_t expectedSession = 0;
    const std::atomic<std::uint64_t>* foregroundRevision = nullptr;
    std::uint64_t expectedRevision = 0;
    const std::atomic<std::uint64_t>* authoritativeSession = nullptr;

    const std::atomic<std::uint64_t>* readRevision = nullptr;
    std::uint64_t expectedReadRevision = 0;
    const std::atomic<std::uint64_t>* credentialRevision = nullptr;
    std::uint64_t expectedCredentialRevision = 0;
    const RestReadIntent* intent = nullptr;
    const std::atomic<std::uint64_t>* promotedNavigation = nullptr;
    const std::atomic_bool* revoked = nullptr;

    RestCancellation cancellation() const noexcept {
      if (stop.stop_requested())
        return RestCancellation::Stopped;
      if (session && session->load() != expectedSession)
        return RestCancellation::SessionChanged;
      if (revoked && revoked->load())
        return RestCancellation::Superseded;
      const bool promoted = intent && intent->foreground.load();
      if (intent
          && (intent->cancelled.load()
              || (promoted && promotedNavigation && promotedNavigation->load() != intent->navigation.load())))
        return RestCancellation::Superseded;
      if ((readRevision && readRevision->load() != expectedReadRevision)
          || (credentialRevision && credentialRevision->load() != expectedCredentialRevision))
        return RestCancellation::Superseded;
      if (authoritativeSession && authoritativeSession->load() == expectedSession)
        return RestCancellation::Superseded;
      if (!promoted && foregroundRevision && foregroundRevision->load() != expectedRevision)
        return RestCancellation::ForegroundReady;
      return RestCancellation::None;
    }
  };

  // Preserve FIFO ordering within each class. In particular, accepted writes
  // never overtake one another when optional bootstrap reads are deferred.
  template <typename Queue, typename Background> auto nextRestWork(Queue& queue, Background background) {
    const auto foreground =
        std::find_if(queue.begin(), queue.end(), [&](const auto& command) { return !background(command); });
    return foreground != queue.end() ? foreground : queue.begin();
  }

  // The account/mutation owner and independent GET readers share one bounded
  // queue, but only the owner may take stateful work. One optional GET may run
  // at once, leaving two of the three read lanes available for visible work.
  template <typename Queue, typename Read, typename Background>
  auto nextRestRead(Queue& queue, Read read, Background background, bool backgroundBusy) {
    const auto visible = std::find_if(queue.begin(), queue.end(), [&](const auto& command) {
      return read(command) && !background(command);
    });
    if (visible != queue.end() || backgroundBusy)
      return visible;
    return std::find_if(queue.begin(), queue.end(), read);
  }
  template <typename Queue, typename Read, typename Background>
  auto nextRestOwned(Queue& queue, Read read, Background background) {
    const auto visible = std::find_if(queue.begin(), queue.end(), [&](const auto& command) {
      return !read(command) && !background(command);
    });
    return visible != queue.end()
        ? visible
        : std::find_if(queue.begin(), queue.end(), [&](const auto& command) { return !read(command); });
  }
} // namespace api
