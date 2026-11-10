#pragma once

#include "api/models.h"

#include <nlohmann/json_fwd.hpp>
#include <string>

namespace api {
  inline constexpr std::size_t maximumForwardDestinations = 5;
  inline constexpr std::size_t maximumForwardCommentCharacters = 2000;
  inline constexpr std::size_t maximumForwardCommentBytes = maximumForwardCommentCharacters * 4;
  inline constexpr std::size_t maximumForwardSearchBytes = 256;
  struct ForwardReference {
    std::string message, channel, guild;
    bool operator==(const ForwardReference&) const = default;
  };
  bool validForwardReference(const ForwardReference& source);
  bool canForwardMessage(const Message& message);
  ForwardedMessage forwardPreview(const Message& message, const ForwardReference& source);
  bool confirmsForward(const Message& message, const ForwardReference& source);
  nlohmann::json forwardMessageBody(const ForwardReference& source, std::string_view nonce);
} // namespace api
