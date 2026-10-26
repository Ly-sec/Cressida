#pragma once

#include "voice/protocol.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace voice {
  class SpeakingActivity {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto hold = std::chrono::milliseconds{300};
    explicit SpeakingActivity(std::string self) : m_self(std::move(self)) {}
    bool hint(std::string_view user, bool speaking, Clock::time_point now) {
      if (user == m_self || !protocol::validId(user))
        return false;
      const bool before = active(user, now);
      if (!speaking) {
        m_remote.erase(std::string(user));
        return before;
      }
      const auto at = m_remote.find(user);
      if (at == m_remote.end() && m_remote.size() >= protocol::maximumParticipants - 1)
        return false;
      m_remote[std::string(user)] = now + hold;
      return !before;
    }
    bool packet(std::string_view user, bool authenticated, bool silence, Clock::time_point now) {
      return authenticated && hint(user, !silence, now);
    }
    bool capture(bool voiced, bool muted, bool deafened, Clock::time_point now) {
      const bool before = active(m_self, now);
      if (voiced && !muted && !deafened)
        m_local = now + hold;
      else
        m_local.reset();
      return before != active(m_self, now);
    }
    bool active(std::string_view user, Clock::time_point now) const {
      if (user == m_self)
        return m_local && now < *m_local;
      const auto at = m_remote.find(user);
      return at != m_remote.end() && now < at->second;
    }
    bool expire(Clock::time_point now) {
      bool changed = false;
      if (m_local && now >= *m_local) {
        m_local.reset();
        changed = true;
      }
      return std::erase_if(m_remote, [&](const auto& item) { return now >= item.second; }) != 0 || changed;
    }
    bool remove(std::string_view user) {
      if (user == m_self)
        return std::exchange(m_local, {}).has_value();
      return m_remote.erase(std::string(user)) != 0;
    }
    bool clear() {
      const bool changed = m_local.has_value() || !m_remote.empty();
      m_local.reset();
      m_remote.clear();
      return changed;
    }

  private:
    std::string m_self;
    std::optional<Clock::time_point> m_local;
    std::map<std::string, Clock::time_point, std::less<>> m_remote;
  };
} // namespace voice
