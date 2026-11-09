#include "app/notification_sounds.h"

#include "core/files/resource_paths.h"
#include "core/log.h"
#include "core/process/wake_event.h"
#include "media/video_player.h"

#include <chrono>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace app {
  namespace {
    constexpr Logger logger{"sounds"};
    constexpr std::size_t maximumSoundBytes = 128 * 1024, maximumQueuedSounds = 4;
    constexpr auto maximumSoundAge = std::chrono::seconds{1};
    constexpr auto playbackDeadline = std::chrono::seconds{3};
    constexpr auto repeatInterval = std::chrono::milliseconds{150};
    using Clock = std::chrono::steady_clock;
    std::vector<std::uint8_t> readSound(const std::string& path) {
      const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
      if (fd < 0)
        return {};
      struct stat status{};
      std::vector<std::uint8_t> bytes;
      if (::fstat(fd, &status) == 0
          && S_ISREG(status.st_mode)
          && status.st_size > 0
          && status.st_size <= static_cast<off_t>(maximumSoundBytes)) {
        bytes.resize(status.st_size);
        std::size_t offset = 0;
        while (offset < bytes.size()) {
          const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
          if (count < 0 && errno == EINTR)
            continue;
          if (count <= 0) {
            bytes.clear();
            break;
          }
          offset += count;
        }
      }
      ::close(fd);
      return bytes;
    }
  } // namespace
  struct NotificationSounds::Impl {
    struct Request {
      SoundEvent event;
      std::string pack;
      int volume;
      Clock::time_point at;
      std::uint64_t generation;
    };
    bool silent;
    process::WakeEvent wake, errors;
    std::mutex mutex;
    std::deque<Request> requests;
    std::optional<Result> result;
    std::uint64_t generation = 0;
    std::optional<SoundEvent> last;
    Clock::time_point lastAt{};
    std::jthread worker;
    explicit Impl(bool silent) : silent(silent) {}
    ~Impl() {
      if (worker.joinable()) {
        worker.request_stop();
        (void)wake.signal();
        worker.join();
      }
    }
    void fail(SoundEvent event, std::optional<std::uint64_t> expected = {}) {
      std::lock_guard lock(mutex);
      if (expected && *expected != generation)
        return;
      logger.warn("Interface sound unavailable");
      result = {event, "An interface sound could not be played. Check the installed sound assets and audio output."};
      (void)errors.signal();
    }
    void run(std::stop_token stop) {
      media::VideoPlayer player;
      std::map<std::string, std::vector<std::uint8_t>> cache;
      std::optional<Request> active;
      Clock::time_point deadline{};
      bool started = false;
      while (!stop.stop_requested()) {
        wake.drain();
        bool cancelled;
        {
          std::lock_guard lock(mutex);
          cancelled = active && active->generation != generation;
        }
        if (cancelled) {
          player.close();
          active.reset();
        }
        if (active) {
          player.drain();
          const auto& state = player.state();
          if (!state.error.empty() || Clock::now() >= deadline) {
            fail(active->event, active->generation);
            player.close();
            active.reset();
          } else if (state.ended) {
            {
              std::lock_guard lock(mutex);
              if (active->generation == generation && (!result || result->error.empty())) {
                result = {active->event, {}};
                (void)errors.signal();
              }
            }
            player.close();
            active.reset();
          } else if (!started && !state.loading && state.hasAudio) {
            started = true;
            player.pause(false);
          }
        }
        if (!active) {
          std::optional<Request> next;
          {
            std::lock_guard lock(mutex);
            while (!requests.empty()) {
              auto request = std::move(requests.front());
              requests.pop_front();
              if (request.generation == generation && Clock::now() - request.at <= maximumSoundAge) {
                next = std::move(request);
                break;
              }
            }
          }
          if (next) {
            const auto key = next->pack + "/" + std::string(soundEvents[static_cast<std::size_t>(next->event)].id);
            const auto found = cache.find(key);
            auto bytes =
                found == cache.end() ? readSound(paths::assetPath("sounds/" + key + ".ogg").string()) : found->second;
            if (bytes.empty()) {
              fail(next->event, next->generation);
            } else {
              if (found == cache.end())
                cache.emplace(key, bytes);
              bool current;
              {
                std::lock_guard lock(mutex);
                current = next->generation == generation;
              }
              if (current && !stop.stop_requested()) {
                if (!player.openSound(media::memoryVideoInput(bytes), next->volume, silent))
                  fail(next->event, next->generation);
                else {
                  active = std::move(next);
                  started = false;
                  deadline = Clock::now() + playbackDeadline;
                }
              }
            }
            continue;
          }
        }
        pollfd descriptors[]{{wake.fd(), POLLIN, 0}, {active ? player.eventFd() : -1, POLLIN, 0}};
        const int timeout = active
            ? static_cast<int>(std::max<std::int64_t>(
                  0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()
              ))
            : -1;
        if (::poll(descriptors, 2, timeout) < 0 && errno != EINTR) {
          fail(active ? active->event : SoundEvent::Message);
          return;
        }
      }
    }
  };
  NotificationSounds::NotificationSounds(bool silent) : m(std::make_unique<Impl>(silent)) {}
  NotificationSounds::~NotificationSounds() = default;
  void NotificationSounds::play(SoundEvent event, std::string pack, int volume) {
    if (static_cast<std::size_t>(event) >= soundEvents.size()
        || !validSoundPack(pack)
        || volume < 0
        || volume > maximumSoundVolume) {
      m->fail(event);
      return;
    }
    std::lock_guard lock(m->mutex);
    if (!m->wake.valid() || !m->errors.valid()) {
      logger.error("Interface sound wake pipe unavailable");
      m->result = {event, "Interface sounds are unavailable because their worker could not start."};
      return;
    }
    const auto now = Clock::now();
    if (m->last == event && now - m->lastAt < repeatInterval)
      return;
    m->last = event;
    m->lastAt = now;
    if (m->requests.size() == maximumQueuedSounds) {
      m->requests.pop_front();
      logger.debug("Coalescing interface sound burst");
    }
    m->requests.push_back({event, std::move(pack), volume, now, m->generation});
    if (!m->worker.joinable())
      m->worker = std::jthread([impl = m.get()](std::stop_token stop) { impl->run(stop); });
    (void)m->wake.signal();
  }
  void NotificationSounds::clear() {
    std::lock_guard lock(m->mutex);
    ++m->generation;
    m->requests.clear();
    m->last.reset();
    m->result.reset();
    m->errors.drain();
    (void)m->wake.signal();
  }
  int NotificationSounds::eventFd() const { return m->errors.fd(); }
  std::optional<NotificationSounds::Result> NotificationSounds::pollResult() {
    std::lock_guard lock(m->mutex);
    m->errors.drain();
    return std::exchange(m->result, {});
  }
} // namespace app
