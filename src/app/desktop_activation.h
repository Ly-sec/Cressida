#pragma once

#include "core/application_identity.h"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace app {
  inline constexpr auto& desktopApplicationId = cressida::applicationId;
  // Desktop-issued activation handles are session-only, single-use and short
  // lived. Do not retain or log them as application preferences.
  class DesktopActivationToken {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t maximumBytes = 4096;
    static constexpr auto lifetime = std::chrono::seconds(5);
    bool assign(std::string_view value, Clock::time_point now = Clock::now()) {
      clear();
      if (value.empty() || value.size() > maximumBytes || value.find('\0') != std::string_view::npos)
        return false;
      m_value = value;
      m_created = now;
      return true;
    }
    bool pending(Clock::time_point now = Clock::now()) const {
      return !m_value.empty() && now >= m_created && now - m_created <= lifetime;
    }
    std::string take(Clock::time_point now = Clock::now()) {
      if (!pending(now))
        clear();
      return std::exchange(m_value, {});
    }
    void clear() { m_value.clear(); }

  private:
    std::string m_value;
    Clock::time_point m_created;
  };
} // namespace app
