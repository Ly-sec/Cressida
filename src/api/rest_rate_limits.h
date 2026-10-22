#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace api {
  // Shared admission state. No mutex remains held during a transfer or wait.
  class RestRateLimits {
  public:
    using Clock = std::chrono::steady_clock;
    using WallClock = std::chrono::system_clock;
    static constexpr auto probeInterval = std::chrono::milliseconds(50);
    static constexpr auto safetyMargin = std::chrono::milliseconds(100);
    static constexpr auto unknownLimitDelay = std::chrono::seconds(60);
    static constexpr auto unknownResetDelay = std::chrono::seconds(60);
    static constexpr auto maximumDelay = std::chrono::hours(24 * 365);
    static constexpr std::size_t maximumRoutes = 512;
    static constexpr std::size_t maximumBuckets = 512;
    static constexpr std::size_t maximumPathBytes = 4096;

    // Only bounded, recognized metadata is retained. A new HTTP status line
    // discards proxy and informational response headers before the final block.
    struct Headers {
      std::optional<unsigned> limit, remaining;
      std::optional<double> resetAfter, resetEpoch, retryAfter;
      std::optional<WallClock::time_point> retryDate, responseDate;
      std::string bucket;
      bool global = false;
      bool shared = false;

      void add(std::string_view line) {
        if (line.starts_with("HTTP/")) {
          *this = {};
          return;
        }
        constexpr std::size_t maximumHeaderBytes = 16 * 1024;
        if (line.size() > maximumHeaderBytes)
          return;
        // Ignore unrelated fields without suppressing later rate-limit metadata.
        // The retained fields have independent fixed bounds; no raw block is kept.
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon > maximumNameBytes)
          return;
        const auto name = lower(trim(line.substr(0, colon)));
        const auto value = trim(line.substr(colon + 1));
        if (name == "x-ratelimit-limit")
          minimum(limit, count(value));
        else if (name == "x-ratelimit-remaining")
          minimum(remaining, count(value));
        else if (name == "x-ratelimit-reset-after")
          maximum(resetAfter, number(value));
        else if (name == "x-ratelimit-reset")
          maximum(resetEpoch, number(value));
        else if (name == "retry-after") {
          if (const auto seconds = number(value))
            maximum(retryAfter, seconds);
          else if (const auto date = httpDate(value))
            maximum(retryDate, date);
        } else if (name == "date") {
          if (const auto date = httpDate(value))
            minimum(responseDate, date);
        } else if (name == "x-ratelimit-bucket") {
          constexpr std::size_t maximumBucketBytes = 128;
          if (!value.empty() && value.size() <= maximumBucketBytes && std::ranges::all_of(value, [](char c) {
                return c > ' ' && c < '\x7f';
              })) {
            if (!m_bucketConflict) {
              if (bucket.empty())
                bucket = value;
              else if (bucket != value) {
                bucket.clear();
                m_bucketConflict = true;
              }
            }
          }
        } else if (name == "x-ratelimit-global")
          global |= lower(value) == "true";
        else if (name == "x-ratelimit-scope") {
          const auto scope = lower(value);
          global |= scope == "global";
          shared |= scope == "shared";
        }
      }

    private:
      static constexpr std::size_t maximumNameBytes = 64;
      bool m_bucketConflict = false;

      static std::string_view trim(std::string_view value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
          value.remove_prefix(1);
        while (!value.empty()
               && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n'))
          value.remove_suffix(1);
        return value;
      }
      static std::string lower(std::string_view value) {
        std::string result(value);
        for (auto& c : result)
          if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        return result;
      }
      template <typename T> static void maximum(std::optional<T>& value, std::optional<T> other) {
        if (other)
          value = value ? std::max(*value, *other) : *other;
      }
      template <typename T> static void minimum(std::optional<T>& value, std::optional<T> other) {
        if (other)
          value = value ? std::min(*value, *other) : *other;
      }
      static std::optional<double> number(std::string_view value) {
        double parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(parsed) || parsed < 0)
          return {};
        return parsed;
      }
      static std::optional<unsigned> count(std::string_view value) {
        constexpr unsigned maximumAllowance = 1'000'000;
        unsigned parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size() || parsed > maximumAllowance)
          return {};
        return parsed;
      }
      static std::optional<WallClock::time_point> httpDate(std::string_view value) {
        constexpr std::array<std::string_view, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        if (value.size() != 29
            || value.substr(3, 2) != ", "
            || value[7] != ' '
            || value[11] != ' '
            || value[16] != ' '
            || value[19] != ':'
            || value[22] != ':'
            || value.substr(25) != " GMT")
          return {};
        const auto integer = [&](std::size_t start, std::size_t length) -> std::optional<int> {
          int parsed = 0;
          const auto field = value.substr(start, length);
          const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), parsed);
          if (error != std::errc{} || end != field.data() + field.size() || parsed < 0)
            return {};
          return parsed;
        };
        const auto day = integer(5, 2), year = integer(12, 4), hour = integer(17, 2), minute = integer(20, 2),
                   second = integer(23, 2);
        const auto month = std::ranges::find(months, value.substr(8, 3));
        if (!day
            || !year
            || !hour
            || !minute
            || !second
            || month == months.end()
            || *year < 1970
            || *year > 2261
            || *hour > 23
            || *minute > 59
            || *second > 59)
          return {};
        const std::chrono::year_month_day date{
            std::chrono::year(*year), std::chrono::month(static_cast<unsigned>(month - months.begin() + 1)),
            std::chrono::day(static_cast<unsigned>(*day))
        };
        if (!date.ok())
          return {};
        return std::chrono::time_point_cast<WallClock::duration>(
            std::chrono::sys_days(date)
            + std::chrono::hours(*hour)
            + std::chrono::minutes(*minute)
            + std::chrono::seconds(*second)
        );
      }
    };

    struct Fallback {
      std::optional<double> retryAfter;
      bool global = false;
    };

  private:
    struct Bucket {
      std::shared_ptr<Bucket> parent;
      std::optional<unsigned> limit;
      unsigned remaining = 0;
      unsigned inFlight = 0;
      bool known = false;
      Clock::time_point reset{}, blocked{};
    };
    struct Route {
      std::shared_ptr<Bucket> bucket;
    };
    struct State {
      std::mutex mutex;
      std::uint64_t generation = 1;
      Clock::time_point global{};
      std::map<std::string, Route, std::less<>> routes;
      std::map<std::string, std::shared_ptr<Bucket>, std::less<>> buckets;
      std::shared_ptr<Bucket> overflow = std::make_shared<Bucket>();
    };

    static std::shared_ptr<Bucket> root(std::shared_ptr<Bucket> bucket) {
      auto result = bucket;
      while (result->parent)
        result = result->parent;
      while (bucket->parent && bucket->parent != result) {
        auto next = bucket->parent;
        bucket->parent = result;
        bucket = std::move(next);
      }
      return result;
    }
    static std::shared_ptr<Bucket> merge(std::shared_ptr<Bucket> from, std::shared_ptr<Bucket> into) {
      from = root(std::move(from));
      into = root(std::move(into));
      if (from == into)
        return into;
      into->inFlight += from->inFlight;
      into->blocked = std::max(into->blocked, from->blocked);
      into->reset = std::max(into->reset, from->reset);
      if (from->known) {
        if (into->known)
          into->remaining = std::min(into->remaining, from->remaining);
        else {
          into->known = true;
          into->remaining = from->remaining;
        }
      }
      if (from->limit)
        into->limit = into->limit ? std::min(*into->limit, *from->limit) : *from->limit;
      from->parent = into;
      from->inFlight = 0;
      return into;
    }
    static void refresh(Bucket& bucket, Clock::time_point now) {
      if (bucket.reset != Clock::time_point{} && bucket.reset <= now && bucket.inFlight == 0) {
        bucket.known = bucket.limit.has_value();
        bucket.remaining = bucket.limit.value_or(0);
        bucket.reset = {};
      }
    }
    static bool disposable(const std::shared_ptr<Bucket>& value, Clock::time_point now) {
      const auto bucket = root(value);
      return bucket->inFlight == 0 && bucket->blocked <= now && bucket->reset <= now;
    }
    static void prune(State& state, Clock::time_point now) {
      // Only finished windows can be forgotten. Active limits and transfers
      // retain their state even when a busy account fills the bounded maps.
      std::erase_if(state.routes, [&](auto& entry) {
        entry.second.bucket = root(entry.second.bucket);
        return disposable(entry.second.bucket, now);
      });
      std::erase_if(state.buckets, [&](auto& entry) {
        entry.second = root(entry.second);
        return disposable(entry.second, now);
      });
    }
    struct RouteKey {
      std::string route, major;
    };
    static RouteKey key(std::string_view method, std::string_view path) {
      if (path.size() > maximumPathBytes || method.size() > maximumMethodBytes)
        return {"overflow", {}};
      path = path.substr(0, path.find('?'));
      RouteKey result;
      result.route = method;
      for (auto& c : result.route)
        if (c >= 'a' && c <= 'z')
          c -= 'a' - 'A';
      result.route += ' ';
      std::string_view group;
      std::size_t part = 0;
      while (!path.empty()) {
        if (path.front() == '/') {
          result.route += '/';
          path.remove_prefix(1);
          continue;
        }
        const auto end = path.find('/');
        const auto segment = path.substr(0, end);
        if (part == 0)
          group = segment;
        const bool major = (part == 1 && (group == "channels" || group == "guilds" || group == "webhooks"))
            || (part == 2 && group == "webhooks");
        const bool numeric =
            !segment.empty() && std::ranges::all_of(segment, [](char c) { return c >= '0' && c <= '9'; });
        result.route += numeric && !major ? ":id" : std::string(segment);
        if (major) {
          result.major += '/';
          if (part == 1) {
            result.major += group;
            result.major += '/';
          }
          result.major += segment;
        }
        ++part;
        if (end == std::string_view::npos)
          break;
        path.remove_prefix(end);
      }
      return result;
    }
    static std::chrono::milliseconds duration(double seconds) {
      if (!std::isfinite(seconds) || seconds < 0)
        return std::chrono::duration_cast<std::chrono::milliseconds>(unknownLimitDelay);
      const auto bounded = std::min(seconds, std::chrono::duration<double>(maximumDelay).count());
      return std::chrono::ceil<std::chrono::milliseconds>(std::chrono::duration<double>(bounded));
    }
    static std::optional<std::chrono::milliseconds>
    retryDelay(const Headers& headers, const Fallback& fallback, WallClock::time_point wallNow) {
      std::optional<std::chrono::milliseconds> result;
      const auto update = [&](std::chrono::milliseconds value) { result = result ? std::max(*result, value) : value; };
      if (headers.retryAfter)
        update(duration(*headers.retryAfter));
      if (fallback.retryAfter)
        update(duration(*fallback.retryAfter));
      if (headers.retryDate) {
        const auto reference = headers.responseDate.value_or(wallNow);
        update(duration(std::max(0.0, std::chrono::duration<double>(*headers.retryDate - reference).count())));
      }
      return result;
    }
    static std::optional<std::chrono::milliseconds> resetDelay(const Headers& headers, WallClock::time_point wallNow) {
      if (headers.resetAfter)
        return duration(*headers.resetAfter);
      if (headers.resetEpoch) {
        const auto reference = headers.responseDate.value_or(wallNow);
        const auto epoch = std::chrono::duration<double>(reference.time_since_epoch()).count();
        return duration(std::max(0.0, *headers.resetEpoch - epoch));
      }
      return {};
    }

  public:
    class Lease {
    public:
      Lease() = default;
      Lease(const Lease&) = delete;
      Lease& operator=(const Lease&) = delete;
      Lease(Lease&& other) noexcept { swap(other); }
      Lease& operator=(Lease&& other) noexcept {
        if (this != &other) {
          release();
          swap(other);
        }
        return *this;
      }
      ~Lease() { release(); }
      explicit operator bool() const { return static_cast<bool>(m_bucket); }

      Clock::time_point complete(
          long status, const Headers& headers, const Fallback& fallback, Clock::time_point now = Clock::now(),
          WallClock::time_point wallNow = WallClock::now()
      ) {
        if (!m_bucket)
          return {};
        std::lock_guard lock(m_state->mutex);
        auto bucket = root(m_bucket);
        if (m_generation != m_state->generation) {
          m_bucket.reset();
          return {};
        }
        if (!headers.bucket.empty() && !m_overflow) {
          const auto identity = m_major + '\n' + headers.bucket;
          if (!m_state->buckets.contains(identity) && m_state->buckets.size() >= maximumBuckets)
            prune(*m_state, now);
          auto found = m_state->buckets.find(identity);
          if (found == m_state->buckets.end() && m_state->buckets.size() < maximumBuckets)
            found = m_state->buckets.emplace(identity, bucket).first;
          if (found != m_state->buckets.end())
            bucket = merge(bucket, found->second);
          else
            bucket = merge(bucket, m_state->overflow);
          if (const auto route = m_state->routes.find(m_route); route != m_state->routes.end())
            route->second.bucket = bucket;
        }
        if (bucket->inFlight > 0)
          --bucket->inFlight;
        const auto reset = resetDelay(headers, wallNow);
        if (headers.limit && *headers.limit > 0)
          bucket->limit = headers.limit;
        // Response allowances can arrive out of order. Subtract transfers still
        // in flight and never replenish an already reserved window.
        if (headers.remaining) {
          const unsigned reserved = bucket->inFlight;
          const unsigned allowance = bucket->limit ? std::min(*headers.remaining, *bucket->limit) : *headers.remaining;
          const unsigned remaining = allowance > reserved ? allowance - reserved : 0;
          bucket->remaining = bucket->known ? std::min(bucket->remaining, remaining) : remaining;
          bucket->known = true;
        }
        if (reset)
          bucket->reset = std::max(bucket->reset, now + *reset + safetyMargin);
        if (status != 429 && bucket->known && bucket->remaining == 0 && bucket->reset <= now)
          bucket->reset = now + unknownResetDelay;
        if (status == 429) {
          auto delay = retryDelay(headers, fallback, wallNow);
          if (reset)
            delay = delay ? std::max(*delay, *reset) : *reset;
          if (!delay)
            delay = std::chrono::duration_cast<std::chrono::milliseconds>(unknownLimitDelay);
          const auto until = now + *delay + safetyMargin;
          bucket->remaining = 0;
          bucket->blocked = std::max(bucket->blocked, until);
          bucket->reset = std::max(bucket->reset, until);
          if (headers.global || fallback.global)
            m_state->global = std::max(m_state->global, until);
        }
        const auto result = std::max(
            m_state->global,
            std::max(bucket->blocked, bucket->known && !bucket->remaining ? bucket->reset : Clock::time_point{})
        );
        m_bucket.reset();
        return result;
      }

    private:
      friend class RestRateLimits;
      Lease(
          std::shared_ptr<State> state, std::shared_ptr<Bucket> bucket, std::uint64_t generation, RouteKey route,
          bool overflow
      )
          : m_state(std::move(state)), m_bucket(std::move(bucket)), m_generation(generation),
            m_route(std::move(route.route)), m_major(std::move(route.major)), m_overflow(overflow) {}

      void swap(Lease& other) noexcept {
        using std::swap;
        swap(m_state, other.m_state);
        swap(m_bucket, other.m_bucket);
        swap(m_generation, other.m_generation);
        swap(m_route, other.m_route);
        swap(m_major, other.m_major);
        swap(m_overflow, other.m_overflow);
      }
      void release() {
        if (!m_bucket)
          return;
        std::lock_guard lock(m_state->mutex);
        if (m_generation == m_state->generation) {
          const auto bucket = root(m_bucket);
          if (bucket->inFlight > 0)
            --bucket->inFlight;
        }
        m_bucket.reset();
      }
      std::shared_ptr<State> m_state;
      std::shared_ptr<Bucket> m_bucket;
      std::uint64_t m_generation = 0;
      std::string m_route, m_major;
      bool m_overflow = false;
    };

    struct Admission {
      Lease lease;
      Clock::time_point retryAt{};
    };

    Admission acquire(std::string_view method, std::string_view path, Clock::time_point now = Clock::now()) {
      auto route = key(method, path);
      std::lock_guard lock(m_state->mutex);
      if (m_state->global > now)
        return {{}, m_state->global};
      auto found = m_state->routes.find(route.route);
      if (found == m_state->routes.end() && m_state->routes.size() >= maximumRoutes)
        prune(*m_state, now);
      found = m_state->routes.find(route.route);
      bool overflow = route.route == "overflow";
      if (found == m_state->routes.end() && !overflow && m_state->routes.size() < maximumRoutes)
        found = m_state->routes.emplace(route.route, Route{std::make_shared<Bucket>()}).first;
      overflow |= found == m_state->routes.end();
      auto bucket = root(overflow ? m_state->overflow : found->second.bucket);
      refresh(*bucket, now);
      const auto until = std::max(m_state->global, bucket->blocked);
      if (until > now)
        return {{}, until};
      if (!bucket->known && bucket->inFlight)
        return {{}, now + probeInterval};
      if (bucket->known && bucket->remaining == 0) {
        if (bucket->reset == Clock::time_point{})
          bucket->reset = now + unknownResetDelay;
        return {{}, bucket->reset > now ? bucket->reset : now + probeInterval};
      }
      if (bucket->known)
        --bucket->remaining;
      ++bucket->inFlight;
      return {Lease(m_state, std::move(bucket), m_state->generation, std::move(route), overflow), {}};
    }

    // Legacy callers use GET routing. New transfers reserve a Lease instead.
    Clock::time_point deadline(std::string_view path) const {
      std::lock_guard lock(m_state->mutex);
      const auto found = m_state->routes.find(key("GET", path).route);
      if (found == m_state->routes.end())
        return m_state->global;
      const auto bucket = root(found->second.bucket);
      return std::max(
          m_state->global,
          std::max(bucket->blocked, bucket->known && !bucket->remaining ? bucket->reset : Clock::time_point{})
      );
    }
    void
    limited(std::string_view path, std::chrono::milliseconds wait, bool global, Clock::time_point now = Clock::now()) {
      const auto until = now
          + std::clamp(wait, std::chrono::milliseconds(0),
                       std::chrono::duration_cast<std::chrono::milliseconds>(maximumDelay));
      std::lock_guard lock(m_state->mutex);
      if (global) {
        m_state->global = std::max(m_state->global, until);
        return;
      }
      const auto route = key("GET", path);
      if (!m_state->routes.contains(route.route) && m_state->routes.size() >= maximumRoutes)
        prune(*m_state, now);
      if (!m_state->routes.contains(route.route) && m_state->routes.size() >= maximumRoutes) {
        m_state->global = std::max(m_state->global, until);
        return;
      }
      auto [found, inserted] = m_state->routes.try_emplace(route.route, Route{});
      if (inserted)
        found->second.bucket = std::make_shared<Bucket>();
      const auto bucket = root(found->second.bucket);
      bucket->blocked = std::max(bucket->blocked, until);
    }
    void clear() {
      std::lock_guard lock(m_state->mutex);
      ++m_state->generation;
      m_state->global = {};
      m_state->routes.clear();
      m_state->buckets.clear();
      m_state->overflow = std::make_shared<Bucket>();
    }

  private:
    static constexpr std::size_t maximumMethodBytes = 16;
    std::shared_ptr<State> m_state = std::make_shared<State>();
  };
} // namespace api
