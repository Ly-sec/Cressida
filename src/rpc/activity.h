#pragma once

#include <cstdint>
#include <expected>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace rpc {
  inline constexpr std::size_t maximumActivities = 4;
  inline constexpr std::size_t maximumActivityBytes = 16384;
  inline constexpr std::size_t maximumButtons = 2;
  struct Activity {
    std::string application;
    nlohmann::json fields = nlohmann::json::object();
    std::string session;
    bool operator==(const Activity&) const = default;
  };
  std::expected<Activity, std::string> parseActivity(const std::string& application, const nlohmann::json& value);
  std::vector<std::string> externalImages(const Activity& activity);
  nlohmann::json gatewayActivity(const Activity& activity);
  nlohmann::json savedActivity(const Activity& activity);
  std::vector<Activity> receivedActivities(const nlohmann::json& value);
  std::optional<std::string> activityMetadataPath(const std::string& user, const Activity& activity);
  std::expected<std::vector<std::string>, std::string> activityButtonUrls(const nlohmann::json& value);
  struct ActivityClock {
    std::int64_t elapsed = 0, total = 0, remaining = 0;
    double fraction = 0;
    bool progress = false, countdown = false, ticking = false;
  };
  std::optional<ActivityClock> activityClock(const Activity& activity, std::int64_t nowMilliseconds);
  std::string activityTime(std::int64_t seconds);
} // namespace rpc
