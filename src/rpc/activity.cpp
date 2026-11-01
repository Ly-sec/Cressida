#include "rpc/activity.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>

namespace rpc {
  namespace {
    constexpr std::size_t maximumText = 128, maximumUrl = 2048, maximumSecret = 128, maximumButtonLabel = 32;
    constexpr std::int64_t maximumPartySize = 1000000;
    constexpr std::int64_t maximumTimestamp = 253402300799000LL;
    bool snowflake(const std::string& text) {
      if (text.empty() || text.size() > 20 || text.front() == '0')
        return false;
      std::uint64_t value = 0;
      const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
      return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value;
    }
    bool url(const std::string& text) {
      constexpr auto prefix = std::string_view("https://");
      if (!text.starts_with(prefix))
        return false;
      const auto hostEnd = text.find_first_of("/?#", prefix.size());
      const auto authority =
          text.substr(prefix.size(), hostEnd == std::string::npos ? hostEnd : hostEnd - prefix.size());
      return text.starts_with("https://")
          && text.size() > std::string_view("https://").size()
          && !authority.empty()
          && authority.find('@') == std::string::npos
          && text.size() <= maximumUrl
          && std::ranges::none_of(text, [](unsigned char c) { return c <= ' ' || c == 127 || c == '\\'; });
    }
    void textField(
        nlohmann::json& out, const nlohmann::json& in, const char* key, std::size_t limit = maximumText,
        bool link = false, bool bytes = false
    ) {
      const auto at = in.find(key);
      if (at == in.end() || at->is_null())
        return;
      if (!at->is_string())
        throw std::runtime_error("Activity text fields must contain text.");
      const auto value = at->get<std::string>();
      const auto length = bytes || link
          ? value.size()
          : static_cast<std::size_t>(std::ranges::count_if(value, [](unsigned char c) { return (c & 0xC0) != 0x80; }));
      if (length > limit || value.find('\0') != std::string::npos || (link && !value.empty() && !url(value)))
        throw std::runtime_error(link ? "Activity links must be valid HTTPS URLs." : "An activity field is too long.");
      if (!value.empty())
        out[key] = value;
    }
    void objectField(nlohmann::json& out, const nlohmann::json& in, const char* key, auto parse) {
      const auto at = in.find(key);
      if (at == in.end() || at->is_null())
        return;
      if (!at->is_object())
        throw std::runtime_error("An activity section must be an object.");
      auto result = nlohmann::json::object();
      parse(result, *at);
      if (!result.empty())
        out[key] = std::move(result);
    }
  } // namespace
  static std::expected<Activity, std::string>
  parseActivityValue(const std::string& application, const nlohmann::json& value, bool incoming) {
    if ((!snowflake(application) && (!incoming || !application.empty())) || !value.is_object())
      return std::unexpected("Provide a valid application ID and a bounded activity object.");
    try {
      if (value.dump().size() > maximumActivityBytes)
        return std::unexpected("The activity is too large.");
      Activity result{application};
      auto& out = result.fields;
      int type = 0;
      if (value.contains("type")) {
        if (!value["type"].is_number_integer())
          throw std::runtime_error("Select a supported activity type.");
        type = value["type"].get<int>();
      }
      if (type != 0 && type != 2 && type != 3 && type != 5 && !(incoming && type == 1))
        throw std::runtime_error("Select Playing, Listening, Watching or Competing.");
      out["type"] = type;
      for (const auto key : {"name", "state", "details"})
        textField(out, value, key);
      for (const auto key : {"state_url", "details_url"})
        textField(out, value, key, maximumUrl, true);
      if (incoming && type == 1)
        textField(out, value, "url", maximumUrl, true);
      if (value.contains("status_display_type")) {
        const auto& display = value["status_display_type"];
        if (!display.is_number_integer() || display < 0 || display > 2)
          throw std::runtime_error("Select Name, State or Details for the status display.");
        out["status_display_type"] = display;
      }
      objectField(out, value, "timestamps", [](auto& target, const auto& source) {
        for (const auto key : {"start", "end"})
          if (const auto at = source.find(key); at != source.end() && !at->is_null()) {
            if (!at->is_number_integer() || *at < 0 || *at > maximumTimestamp)
              throw std::runtime_error("Activity times must be valid Unix timestamps.");
            target[key] = *at;
          }
        if (target.contains("start") && target.contains("end") && target["end"] < target["start"])
          throw std::runtime_error("The activity end time must follow its start time.");
      });
      objectField(out, value, "assets", [](auto& target, const auto& source) {
        for (const auto key : {"large_image", "small_image", "invite_cover_image"})
          textField(target, source, key, maximumUrl);
        for (const auto key : {"large_text", "small_text"})
          textField(target, source, key);
        for (const auto key : {"large_url", "small_url"})
          textField(target, source, key, maximumUrl, true);
      });
      objectField(out, value, "party", [](auto& target, const auto& source) {
        textField(target, source, "id");
        if (source.contains("size")) {
          const auto& size = source["size"];
          if (!size.is_array()
              || size.size() != 2
              || !size[0].is_number_integer()
              || !size[1].is_number_integer()
              || size[0] < 0
              || size[1] < 1
              || size[0] > size[1]
              || size[1] > maximumPartySize)
            throw std::runtime_error("Party size must contain a current size and a larger maximum.");
          target["size"] = size;
        }
        if (source.contains("privacy")) {
          if (!source["privacy"].is_number_integer() || source["privacy"] < 0 || source["privacy"] > 1)
            throw std::runtime_error("Select a supported party privacy mode.");
          target["privacy"] = source["privacy"];
        }
      });
      objectField(out, value, "secrets", [](auto& target, const auto& source) {
        for (const auto key : {"join", "spectate", "match"})
          textField(target, source, key, maximumSecret, false, true);
      });
      if (value.contains("instance")) {
        if (!value["instance"].is_boolean())
          throw std::runtime_error("Activity instance must be a boolean.");
        out["instance"] = value["instance"];
      }
      if (value.contains("supported_platforms")) {
        const auto& platforms = value["supported_platforms"];
        if (!platforms.is_array() || platforms.size() > 4)
          throw std::runtime_error("Select supported activity platforms.");
        out["supported_platforms"] = nlohmann::json::array();
        for (const auto& platform : platforms) {
          if (!platform.is_string() || (platform != "desktop" && platform != "android" && platform != "ios"))
            throw std::runtime_error("Select supported activity platforms.");
          out["supported_platforms"].push_back(platform);
        }
      }
      if (value.contains("buttons")) {
        const auto& buttons = value["buttons"];
        if (!buttons.is_array() || buttons.size() > maximumButtons)
          throw std::runtime_error("An activity can have at most two buttons.");
        out["buttons"] = nlohmann::json::array();
        for (const auto& button : buttons) {
          if (!button.is_object())
            throw std::runtime_error("Each activity button needs a label and a link.");
          nlohmann::json target = nlohmann::json::object();
          textField(target, button, "label", maximumButtonLabel);
          textField(target, button, "url", maximumUrl, true);
          if (!target.contains("label") || (!incoming && !target.contains("url")))
            throw std::runtime_error("Each activity button needs a label and a link.");
          out["buttons"].push_back(std::move(target));
        }
      }
      return result;
    } catch (const std::runtime_error& error) {
      return std::unexpected(error.what());
    } catch (...) {
      return std::unexpected("The activity contains an invalid field.");
    }
  }
  std::expected<Activity, std::string> parseActivity(const std::string& application, const nlohmann::json& value) {
    return parseActivityValue(application, value, false);
  }
  std::vector<std::string> externalImages(const Activity& activity) {
    std::vector<std::string> result;
    if (activity.fields.contains("assets"))
      for (const auto key : {"large_image", "small_image", "invite_cover_image"}) {
        const auto image = activity.fields["assets"].value(key, std::string{});
        if (url(image) && std::ranges::find(result, image) == result.end())
          result.push_back(image);
      }
    return result;
  }
  nlohmann::json gatewayActivity(const Activity& activity) {
    auto value = activity.fields;
    value["application_id"] = activity.application;
    if (value.contains("timestamps"))
      for (const auto key : {"start", "end"})
        if (value["timestamps"].contains(key)) {
          constexpr std::int64_t millisecondThreshold = 1000000000000LL, millisecondsPerSecond = 1000;
          auto& timestamp = value["timestamps"][key];
          if (timestamp < millisecondThreshold)
            timestamp = timestamp.get<std::int64_t>() * millisecondsPerSecond;
        }
    if (value.contains("buttons")) {
      // IPC accepts button objects; personal-account presence sends indexed labels and link metadata.
      auto labels = nlohmann::json::array(), links = nlohmann::json::array();
      for (const auto& button : value["buttons"]) {
        labels.push_back(button.at("label"));
        links.push_back(button.at("url"));
      }
      value["buttons"] = std::move(labels);
      value["metadata"] = {{"button_urls", std::move(links)}};
    }
    return value;
  }
  nlohmann::json savedActivity(const Activity& activity) {
    auto value = activity.fields;
    value["application_id"] = activity.application;
    value.erase("secrets");
    return value;
  }
  std::optional<ActivityClock> activityClock(const Activity& activity, std::int64_t nowMilliseconds) {
    const auto at = activity.fields.find("timestamps");
    if (at == activity.fields.end() || !at->is_object())
      return {};
    constexpr std::int64_t millisecondThreshold = 1000000000000LL, millisecondsPerSecond = 1000;
    const auto timestamp = [&](const char* key) -> std::optional<std::int64_t> {
      const auto value = at->find(key);
      if (value == at->end() || !value->is_number_integer())
        return {};
      const auto number = value->get<std::int64_t>();
      if (number < 0 || number > maximumTimestamp)
        return {};
      return number >= millisecondThreshold ? number : number * millisecondsPerSecond;
    };
    const auto start = timestamp("start"), end = timestamp("end");
    if ((!start && !end) || (start && end && *end < *start))
      return {};
    ActivityClock result;
    result.countdown = end.has_value();
    result.progress = start && end && *end > *start;
    result.ticking = !end || nowMilliseconds < *end;
    if (start) {
      const auto elapsed = std::max<std::int64_t>(0, nowMilliseconds - *start);
      result.elapsed = (end ? std::min(elapsed, *end - *start) : elapsed) / millisecondsPerSecond;
    }
    if (end)
      result.remaining = std::max<std::int64_t>(0, *end - nowMilliseconds) / millisecondsPerSecond;
    if (result.progress) {
      result.total = (*end - *start) / millisecondsPerSecond;
      result.fraction =
          std::clamp(static_cast<double>(nowMilliseconds - *start) / static_cast<double>(*end - *start), 0.0, 1.0);
    }
    return result;
  }
  std::string activityTime(std::int64_t seconds) {
    constexpr std::int64_t secondsPerMinute = 60, secondsPerHour = secondsPerMinute * 60;
    seconds = std::max<std::int64_t>(0, seconds);
    const auto padded = [](std::int64_t value) { return (value < 10 ? "0" : "") + std::to_string(value); };
    if (seconds >= secondsPerHour)
      return std::to_string(seconds / secondsPerHour)
          + ":"
          + padded(seconds / secondsPerMinute % secondsPerMinute)
          + ":"
          + padded(seconds % secondsPerMinute);
    return std::to_string(seconds / secondsPerMinute) + ":" + padded(seconds % secondsPerMinute);
  }
  std::vector<Activity> receivedActivities(const nlohmann::json& value) {
    std::vector<Activity> result;
    if (!value.is_array())
      return result;
    for (auto item : value) {
      if (!item.is_object() || !item.contains("type") || !item["type"].is_number_integer() || item["type"] == 4)
        continue;
      const auto applicationId = item.find("application_id");
      if (applicationId != item.end() && !applicationId->is_null() && !applicationId->is_string())
        continue;
      const auto application =
          applicationId != item.end() && applicationId->is_string() ? applicationId->get<std::string>() : std::string{};
      const auto removeNulls = [](auto&& self, nlohmann::json& object) -> void {
        if (!object.is_object())
          return;
        for (auto at = object.begin(); at != object.end();)
          if (at->is_null())
            at = object.erase(at);
          else {
            self(self, *at);
            ++at;
          }
      };
      removeNulls(removeNulls, item);
      if (item.contains("party") && item["party"].is_object() && item["party"].contains("size")) {
        const auto& size = item["party"]["size"];
        if (size.is_array() && size.size() == 2 && size[0] == 0 && size[1] == 0)
          item["party"].erase("size");
      }
      if (item.contains("buttons") && item["buttons"].is_array()) {
        const auto labels = item["buttons"];
        auto buttons = nlohmann::json::array();
        if (item.contains("metadata")
            && item["metadata"].is_object()
            && item["metadata"].contains("button_urls")
            && item["metadata"]["button_urls"].is_array()) {
          const auto& urls = item["metadata"]["button_urls"];
          for (std::size_t i = 0; i < std::min(labels.size(), maximumButtons); ++i)
            if (labels[i].is_string()) {
              nlohmann::json button{{"label", labels[i]}};
              if (i < urls.size() && urls[i].is_string() && url(urls[i].get<std::string>()))
                button["url"] = urls[i];
              buttons.push_back(std::move(button));
            }
        } else
          for (const auto& button : labels)
            if (button.is_object())
              buttons.push_back(button);
            else if (button.is_string())
              buttons.push_back({{"label", button}});
        item["buttons"] = std::move(buttons);
      }
      item.erase("secrets");
      if (auto activity = parseActivityValue(application, item, true)) {
        if (const auto session = item.find("session_id"); session != item.end() && session->is_string()) {
          const auto text = session->get<std::string>();
          constexpr std::size_t maximumSessionBytes = 128;
          if (text.size() <= maximumSessionBytes && std::ranges::all_of(text, [](unsigned char c) {
                return (c >= 'a' && c <= 'z')
                    || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9')
                    || c == '_'
                    || c == '-';
              }))
            activity->session = text;
        }
        result.push_back(std::move(*activity));
        if (result.size() == maximumActivities)
          break;
      }
    }
    return result;
  }
  std::optional<std::string> activityMetadataPath(const std::string& user, const Activity& activity) {
    if (!snowflake(user)
        || (!activity.application.empty() && !snowflake(activity.application))
        || activity.session.empty())
      return {};
    constexpr std::size_t maximumSessionBytes = 128;
    if (activity.session.size() > maximumSessionBytes || !std::ranges::all_of(activity.session, [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }))
      return {};
    return "/users/"
        + user
        + "/sessions/"
        + activity.session
        + "/activities/"
        + (activity.application.empty() ? "0" : activity.application)
        + "/metadata";
  }
  std::expected<std::vector<std::string>, std::string> activityButtonUrls(const nlohmann::json& value) {
    if (!value.is_object()
        || !value.contains("button_urls")
        || !value["button_urls"].is_array()
        || value["button_urls"].size() > maximumButtons)
      return std::unexpected("The activity link response was invalid.");
    std::vector<std::string> result;
    for (const auto& entry : value["button_urls"]) {
      if (!entry.is_string() || !url(entry.get<std::string>()))
        return std::unexpected("The activity link response was invalid.");
      result.push_back(entry.get<std::string>());
    }
    return result;
  }
} // namespace rpc
