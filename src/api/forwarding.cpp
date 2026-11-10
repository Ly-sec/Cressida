#include "api/forwarding.h"

#include "api/actions.h"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace api {
  namespace {
    constexpr std::uint64_t privateMessageFlags = (1ULL << 6) | (1ULL << 7);
    constexpr int forwardReferenceType = 1;
    constexpr int chatInputCommandType = 20, contextMenuCommandType = 23;
  } // namespace
  bool validForwardReference(const ForwardReference& source) {
    return validSnowflake(source.message)
        && validSnowflake(source.channel)
        && (source.guild.empty() || validSnowflake(source.guild));
  }
  bool canForwardMessage(const Message& message) {
    return validSnowflake(message.id)
        && validSnowflake(message.channelId)
        && !message.deleted
        && !(message.flags & privateMessageFlags)
        && !message.poll
        && !message.hasCall
        && !message.hasActivity
        && (message.type == defaultMessageType
            || message.type == replyMessageType
            || message.type == chatInputCommandType
            || message.type == contextMenuCommandType)
        && (!message.forwarded || message.forwarded->available);
  }
  ForwardedMessage forwardPreview(const Message& message, const ForwardReference& source) {
    if (message.forwarded) {
      auto snapshot = *message.forwarded;
      snapshot.message = source.message;
      snapshot.channel = source.channel;
      snapshot.guild = source.guild;
      return snapshot;
    }
    return {
        .message = source.message,
        .channel = source.channel,
        .guild = source.guild,
        .timestamp = message.timestamp,
        .content = message.content,
        .attachments = message.attachments,
        .embeds = message.embeds,
        .mentions = message.mentions,
        .available = true,
        .stickers = message.stickers
    };
  }
  bool confirmsForward(const Message& message, const ForwardReference& source) {
    return message.forwarded
        && message.forwarded->message == source.message
        && message.forwarded->channel == source.channel
        && (message.forwarded->guild.empty() || message.forwarded->guild == source.guild);
  }
  nlohmann::json forwardMessageBody(const ForwardReference& source, std::string_view nonce) {
    if (!validForwardReference(source) || !validSnowflake(std::string(nonce)))
      throw std::invalid_argument("Invalid forward reference or nonce");
    auto body = messageBody("", {}, {}, nonce);
    body.erase("content");
    body["allowed_mentions"] = {{"parse", nlohmann::json::array()}, {"replied_user", false}};
    body["message_reference"] = {
        {"type", forwardReferenceType},
        {"message_id", source.message},
        {"channel_id", source.channel},
        {"fail_if_not_exists", true}
    };
    if (!source.guild.empty())
      body["message_reference"]["guild_id"] = source.guild;
    return body;
  }
} // namespace api
