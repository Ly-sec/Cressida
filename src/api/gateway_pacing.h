#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>

namespace api::gateway_pacing {
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;
  using namespace std::chrono_literals;

  // Reserve room for handshake and heartbeat traffic independently of UI work.
  inline constexpr std::size_t applicationLimit = 90;
  inline constexpr auto applicationWindow = 60s;
  inline constexpr auto subscriptionSpacing = 500ms;
  inline constexpr auto memberSpacing = 1s;
  inline constexpr auto memberSearchLifetime = 5s;
  inline constexpr auto presenceWindow = 20s;
  inline constexpr std::size_t presenceLimit = 5;
  inline constexpr auto connectionSpacing = 1s;
  inline constexpr auto identifySpacing = 5s;
  inline constexpr auto stableConnection = 60s;
  inline constexpr auto maximumBackoff = 60s;
  inline constexpr auto rateLimitedBackoff = 60s;

  class ApplicationBudget {
  public:
    Time ready(Time now, std::size_t count = 1) {
      while (!m_sent.empty() && now - m_sent.front() >= applicationWindow)
        m_sent.pop_front();
      if (!count || count > applicationLimit)
        return Time::max();
      if (m_sent.size() + count <= applicationLimit)
        return now;
      return m_sent[m_sent.size() + count - applicationLimit - 1] + applicationWindow;
    }
    bool acquire(Time now, std::size_t count = 1) {
      if (ready(now, count) > now)
        return false;
      for (std::size_t index = 0; index < count; ++index)
        m_sent.push_back(now);
      return true;
    }

  private:
    std::deque<Time> m_sent;
  };

  enum class Failure { Transport, InvalidSession, RateLimited, Handoff };

  inline std::chrono::milliseconds invalidSessionDelay(std::uint32_t random) {
    constexpr auto minimum = 1000ms;
    constexpr auto spread = 4000;
    return minimum + std::chrono::milliseconds(random % (spread + 1));
  }

  inline std::chrono::milliseconds heartbeatDelay(std::chrono::milliseconds interval, std::uint32_t random) {
    constexpr auto randomRange = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
    return std::chrono::milliseconds(static_cast<std::uint64_t>(interval.count()) * random / randomRange);
  }

  class ReconnectBudget {
  public:
    Time ready(Time now, bool resumable, bool handoff = false) const {
      auto deadline = now;
      if (m_connect && !handoff)
        deadline = std::max(deadline, *m_connect + connectionSpacing);
      if (!resumable && m_identify)
        deadline = std::max(deadline, *m_identify + identifySpacing);
      return deadline;
    }
    void connecting(Time now) { m_connect = now; }
    void identifying(Time now) { m_identify = now; }
    Time failed(Time now, Failure failure, Clock::duration readyDuration = {}, std::uint32_t random = 0) {
      if (readyDuration >= stableConnection)
        m_backoff = 1s;
      if (failure == Failure::Handoff)
        return now;
      if (failure == Failure::InvalidSession)
        return now + invalidSessionDelay(random);
      const auto delay = failure == Failure::RateLimited ? std::max(m_backoff, rateLimitedBackoff) : m_backoff;
      m_backoff = std::min(m_backoff * 2, maximumBackoff);
      return now + delay;
    }

  private:
    std::optional<Time> m_connect, m_identify;
    std::chrono::seconds m_backoff{1};
  };
} // namespace api::gateway_pacing
