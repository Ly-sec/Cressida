#pragma once
#include "app/message_group.h"

#include <array>
#include <charconv>
#include <string>
#include <string_view>

namespace app {
  struct JoinNotice {
    std::string_view before, after;
  };
  inline JoinNotice joinNotice(const api::Message& message) {
    static constexpr std::array<JoinNotice, 13> variants{
        {{"", " joined the party."},
         {"", " is here."},
         {"Welcome, ", ". We hope you brought pizza."},
         {"A wild ", " appeared."},
         {"", " just landed."},
         {"", " just slid into the server."},
         {"", " just showed up!"},
         {"Welcome ", ". Say hi!"},
         {"", " hopped into the server."},
         {"Everyone welcome ", "!"},
         {"Glad you're here, ", "."},
         {"Good to see you, ", "."},
         {"Yay you made it, ", "!"}}
    };
    std::uint64_t milliseconds = 0;
    if (const auto time = messageTime(message.timestamp); time && time->time_since_epoch().count() >= 0)
      milliseconds = static_cast<std::uint64_t>(time->time_since_epoch().count());
    else {
      std::uint64_t snowflake = 0;
      const auto parsed = std::from_chars(message.id.data(), message.id.data() + message.id.size(), snowflake);
      if (parsed.ec == std::errc{} && parsed.ptr == message.id.data() + message.id.size())
        milliseconds = (snowflake >> 22) + 1420070400000ULL;
    }
    return variants[milliseconds % variants.size()];
  }
  inline std::string joinNoticeText(const api::Message& message, std::string_view author) {
    const auto notice = joinNotice(message);
    return std::string(notice.before) + std::string(author) + std::string(notice.after);
  }
  inline constexpr int pinNoticeType = 6, joinNoticeType = 7;
  // Pins and joins render as compact inline notices instead of author rows.
  inline bool compactSystemNotice(int type) { return type == pinNoticeType || type == joinNoticeType; }
  inline constexpr std::string_view pinNoticeAction = " pinned a message to this channel.",
                                    pinNoticeLink = " See all pinned messages.";
  inline std::string pinNoticeText(std::string_view author) {
    return std::string(author) + std::string(pinNoticeAction) + std::string(pinNoticeLink);
  }
  inline std::string pinNoticeMarkdown(std::string_view authorToken) {
    return std::string(authorToken) + " pinned **a message** to this channel. See all **pinned messages**.";
  }
  inline std::string systemNoticeText(const api::Message& message, std::string_view author) {
    return message.type == pinNoticeType ? pinNoticeText(author) : joinNoticeText(message, author);
  }
} // namespace app
