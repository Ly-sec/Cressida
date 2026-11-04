#pragma once

#include "api/models.h"
#include "app/composer_tokens.h"
#include "app/message_preferences.h"
#include "app/system_message.h"

#include <functional>
#include <string>
#include <string_view>

namespace app {
  inline std::string
  desktopMessageTitle(std::string sender, std::string_view conversation = {}, std::string_view server = {}) {
    if (conversation.empty() && server.empty())
      return sender;
    sender += " (";
    sender += conversation;
    if (!conversation.empty() && !server.empty())
      sender += ", ";
    sender += server;
    sender += ')';
    return sender;
  }
  using NotificationName = std::function<std::string(char, const std::string&)>;
  // Notifications carry plain text. Resolve protocol tokens before the desktop
  // worker performs capability-aware escaping; inserted names are never reparsed.
  inline std::string desktopMessagePreview(const api::Message& message, const NotificationName& name) {
    if (compactSystemNotice(message.type))
      return systemNoticeText(message, name('@', message.authorId));
    if (message.type == 1 || message.type == 2) {
      std::string people;
      for (const auto& [id, fallback] : message.mentions) {
        if (!people.empty())
          people += ", ";
        people += name('@', id);
      }
      if (!people.empty())
        return name('@', message.authorId) + (message.type == 1 ? " added " : " removed ") + people + '.';
    }
    if (message.forwarded && message.content.empty())
      return !message.forwarded->stickers.empty()  ? "Forwarded a sticker"
          : message.forwarded->attachments.empty() ? "Forwarded a message"
                                                   : "Forwarded an attachment";
    if (message.content.empty() && !message.stickers.empty())
      return "Sent a sticker: " + message.stickers.front().name;
    if (message.content.empty())
      return message.attachments.empty() && message.embeds.empty() ? "New message" : "Sent an attachment";
    const auto text = notificationMessagePreview(message.content);
    std::string result;
    std::size_t at = 0;
    for (const auto& token : composerTokens(text)) {
      result.append(text, at, token.begin - at);
      if (token.kind == ComposerToken::Emoji) {
        result += ':';
        result += token.name;
        result += ':';
      } else {
        const char kind = token.kind == ComposerToken::Channel ? '#' : token.kind == ComposerToken::Role ? '&' : '@';
        auto resolved = name(kind, std::string(token.id));
        if (resolved.empty())
          resolved = kind == '&' ? "unknown-role" : kind == '#' ? "unknown-channel" : "unknown-user";
        result += kind == '#' ? '#' : '@';
        result += resolved;
      }
      at = token.end;
    }
    result.append(text, at);
    return result;
  }
} // namespace app
