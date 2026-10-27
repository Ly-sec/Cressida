#pragma once

#include "api/models.h"
#include "app/layout_metrics.h"
#include "app/syntax_highlight.h"
#include "ui/style.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace app {
  struct MessageStyle {
    bool bold = false, italic = false, underline = false, strike = false, code = false, spoiler = false,
         mention = false, emoji = false;
    bool historical = false;
    bool linkHovered = false, muted = false;
    float size = Style::fontSizeBody;
    std::string link;
    CodeToken syntax = CodeToken::Plain;
    std::string userMention;
    std::size_t linkId = 0;
    std::size_t spoilerId = 0;
    std::size_t strikeId = 0;
  };
  struct MessageRun {
    std::string text, image;
    float x = 0, y = 0, width = 0;
    MessageStyle style;
    std::size_t offset = 0;
    float height = 22;
    struct ChannelChip {
      struct Part {
        enum Kind { Text, Glyph, GuildIcon } kind;
        std::string value;
        float x, width;
        bool locked = false;
      };
      std::vector<Part> parts;
    };
    std::optional<ChannelChip> channelChip;
  };
  struct MessageBox {
    float x, y, width, height;
    bool quote = false;
    std::string code;
    bool embed = false;
    std::uint32_t color = 0;
    bool inlineCode = false;
    bool embedAccent = true, divider = false, disabled = false, spoiler = false;
    std::string link;
  };
  struct MessagePicture {
    api::Media media;
    float x, y, width, height;
    std::string link;
    bool video = false;
    bool animated = false;
    std::string videoSource;
    std::optional<api::Sticker> sticker;
    bool spoiler = false;
  };
  struct MessageLayout {
    struct AutoModeration {
      float authorTop;
      std::size_t contentStart, contentEnd;
      std::string matched;
      float actionsTop;
      layout::AutoModerationActions actions;
    };
    std::optional<AutoModeration> autoModeration;
    struct EditedMarker {
      float x, y, width, height, size;
    };
    struct ForwardedCard {
      float y, height;
      std::string guild, channel, message, timestamp;
    };
    std::vector<ForwardedCard> forwards;
    std::vector<MessageRun> runs;
    std::vector<MessageBox> boxes;
    std::vector<MessagePicture> pictures;
    std::vector<layout::Frame> embeds;
    std::string plainText;
    float height = 0, historyHeight = 0;
    std::optional<EditedMarker> edited;
  };
  struct MessageOptions {
    bool emojiImageLinks = false, compact = false;
    std::function<bool(std::string_view)> unicodeEmoji;
    bool discordTimestamps = false;
    bool mentionPrefix = true;
    struct ChannelMention {
      std::string name, link;
      std::string guildName, guildIcon, channelName, parentName;
      std::string icon = "hash", parentIcon = "hash";
      bool locked = false;
    };
    std::function<ChannelMention(std::string_view, std::string_view)> channelMention;
    std::function<std::string(std::string_view)> roleMention;
  };
  MessageLayout layoutRichMessage(
      const api::Message& message, float width,
      const std::function<float(std::string_view, const MessageStyle&)>& measure, float fontSize = Style::fontSizeBody,
      const MessageOptions& options = {}
  );
  using MessageMeasure = std::function<float(std::string_view, const MessageStyle&)>;
  struct MessageLinkArea {
    float x, y, width, height;
    std::size_t first, last, group;
    std::string url;
  };
  std::vector<MessageLinkArea> messageLinkAreas(const MessageLayout& layout);
  bool messageLinkHovered(const std::vector<MessageLinkArea>& areas, std::size_t run, float x, float y);
  struct MessageSpanArea {
    float x, y, width, height;
    std::size_t first, last;
  };
  std::vector<MessageSpanArea> messageSpoilerAreas(const MessageLayout& layout);
  std::vector<MessageSpanArea> messageStrikeAreas(const MessageLayout& layout);
  MessageLayout layoutReplyPreview(
      const api::MessageReference& reply, std::string_view author, float width, const MessageMeasure& measure,
      const MessageOptions& options = {}
  );
  std::string messageMarkup(std::string_view text, const MessageStyle& style, bool inlineStrike = true);
  MessageLayout layoutMessage(
      std::string_view content, float width, const MessageMeasure& measure, float fontSize = Style::fontSizeBody,
      const std::map<std::string, std::string>& mentions = {}, const MessageOptions& options = {}
  );
  MessageLayout
  layoutMessage(std::string_view content, float width, const std::function<float(std::string_view)>& measure);
} // namespace app
