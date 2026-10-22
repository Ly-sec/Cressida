#pragma once

#include "api/models.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>

namespace app {
  // Only cursors captured while viewing messages belong here. Dispatch must not
  // look up a newer message when the user navigates away or hides the window.
  class ReadAcknowledgements {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr auto interval = std::chrono::seconds(2);
    static constexpr std::size_t pendingChannelLimit = 32;
    struct Cursor {
      std::string channel, message;
    };

    bool queue(const std::string& channel, const std::string& message) {
      if (!api::validSnowflake(channel) || !api::validSnowflake(message))
        return false;
      if (auto at = m_pending.find(channel); at != m_pending.end()) {
        if (newer(message, at->second))
          at->second = message;
        return true;
      }
      if (m_pending.size() >= pendingChannelLimit)
        return false;
      m_order.push_back(channel);
      m_pending.emplace(channel, message);
      return true;
    }
    std::optional<Cursor> take(TimePoint now) {
      if (m_order.empty() || (m_after && now < *m_after))
        return {};
      auto channel = std::move(m_order.front());
      m_order.pop_front();
      auto at = m_pending.find(channel);
      Cursor result{std::move(channel), std::move(at->second)};
      m_pending.erase(at);
      // Failed or uncertain writes are not automatically retried.
      m_after = now + interval;
      return result;
    }
    std::optional<std::chrono::milliseconds> delay(TimePoint now) const {
      if (m_order.empty())
        return {};
      if (!m_after || *m_after <= now)
        return std::chrono::milliseconds::zero();
      return std::chrono::ceil<std::chrono::milliseconds>(*m_after - now);
    }
    void acknowledged(const std::string& channel, const std::string& message, TimePoint now) {
      if (const auto at = m_pending.find(channel); at != m_pending.end() && !newer(at->second, message))
        cancel(channel);
      // Explicit actions remain immediate and also quiet subsequent automatic reads.
      m_after = std::max(m_after.value_or(now), now + interval);
    }
    void cancel(const std::string& channel) {
      m_pending.erase(channel);
      std::erase(m_order, channel);
    }
    void reset() { *this = {}; }
    std::size_t pending() const { return m_pending.size(); }

  private:
    static bool newer(const std::string& a, const std::string& b) {
      return a.size() != b.size() ? a.size() > b.size() : a > b;
    }
    std::map<std::string, std::string> m_pending;
    std::deque<std::string> m_order;
    std::optional<TimePoint> m_after;
  };
} // namespace app
