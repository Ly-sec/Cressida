#pragma once
#include "app/message_layout.h"

#include <algorithm>
#include <span>

namespace app {
  inline MessageLayout withEditHistory(
      MessageLayout current, std::span<const api::Message> versions, float width, const MessageMeasure& measure,
      float fontSize, const MessageOptions& options = {}
  ) {
    if (versions.empty())
      return current;
    MessageLayout result;
    const auto append = [&](MessageLayout part, bool historical) {
      const auto offset = result.plainText.size();
      const auto y = result.height;
      for (auto run : part.runs) {
        run.offset += offset;
        run.y += y;
        run.style.historical = historical;
        result.runs.push_back(std::move(run));
      }
      for (auto box : part.boxes) {
        box.y += y;
        result.boxes.push_back(std::move(box));
      }
      for (auto card : part.forwards) {
        card.y += y;
        result.forwards.push_back(std::move(card));
      }
      for (auto picture : part.pictures) {
        picture.y += y;
        result.pictures.push_back(std::move(picture));
      }
      for (auto icon : part.embedIcons) {
        icon.frame.y += y;
        result.embedIcons.push_back(std::move(icon));
      }
      if (part.edited) {
        part.edited->y += y;
        result.edited = part.edited;
      }
      result.plainText += part.plainText;
      if (!result.plainText.ends_with('\n'))
        result.plainText += '\n';
      result.height += std::max(22.0F, part.height);
    };
    for (auto version : versions) {
      // Keep old media as filename links; a long history must not repeat full images.
      version.editedTimestamp.clear();
      version.embeds.clear();
      version.stickers.clear();
      for (auto& attachment : version.attachments)
        attachment.image.url.clear();
      if (version.content.empty() && version.attachments.empty())
        version.content = "[Empty message]";
      append(layoutRichMessage(version, width, measure, fontSize, options), true);
    }
    result.historyHeight = result.height;
    append(std::move(current), false);
    return result;
  }
} // namespace app
