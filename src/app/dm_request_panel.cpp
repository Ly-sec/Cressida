#include "app/client_application.h"
#include "app/message_time.h"
#include "app/notification_preview.h"
#include "app/theme.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <cmath>

namespace app {
  void ClientApplication::dmButton(Node& parent, Hit hit, const std::string& text, bool primary) {
    using namespace theme;
    const auto position = [&](const Node* node) {
      std::pair<float, float> result{};
      for (; node && node != m_scene.get(); node = node->parent()) {
        result.first += node->x();
        result.second += node->y();
      }
      return result;
    };
    const auto [x, y] = position(&parent);
    Hit absolute = hit;
    absolute.x += x;
    absolute.y += y;
    const bool over = hovered(absolute.x, absolute.y, absolute.width, absolute.height);
    rectangle(
        parent, hit.x, hit.y, hit.width, hit.height,
        primary    ? (over ? lerpColor(accent, foreground, .12F) : accent)
            : over ? lerpColor(raised, foreground, .08F)
                   : raised,
        Style::controlRadius
    );
    const float captionWidth = std::max(1.0F, hit.width - 16);
    const auto metrics =
        m_window.target().renderer().measureText(text, Style::fontSizeDescription, FontWeight::Medium, captionWidth, 1);
    auto caption = std::make_unique<TextNode>();
    caption->setText(text);
    caption->setFontSize(Style::fontSizeDescription);
    caption->setFontWeight(FontWeight::Medium);
    caption->setColor(primary ? ink : foreground);
    caption->setMaxWidth(captionWidth);
    caption->setMaxLines(1);
    // The renderer uses tight glyph surfaces; center the measured text block,
    // rather than relying on alignment within a wider logical text node.
    caption->setPosition(
        hit.x + (hit.width - metrics.width) / 2, hit.y + (hit.height - (metrics.bottom - metrics.top)) / 2 - metrics.top
    );
    caption->setSize(std::max(1.0F, metrics.width), std::max(16.0F, metrics.bottom - metrics.top));
    parent.addChild(std::move(caption));
    // Clip manual hit areas with the same ancestors as the painted button.
    for (const auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      if (!node->clipChildren())
        continue;
      const auto [clipX, clipY] = position(node);
      const float right = std::min(absolute.x + absolute.width, clipX + node->width());
      const float bottom = std::min(absolute.y + absolute.height, clipY + node->height());
      absolute.x = std::max(absolute.x, clipX);
      absolute.y = std::max(absolute.y, clipY);
      absolute.width = std::max(0.0F, right - absolute.x);
      absolute.height = std::max(0.0F, bottom - absolute.y);
    }
    if (absolute.width > 0 && absolute.height > 0)
      m_hits.push_back(std::move(absolute));
  }

  void ClientApplication::drawDmRequestPanel(Node& root, float x, float y, float width, float height) {
    using namespace theme;
    const auto channel = std::ranges::find(m_privateChannels, m_dmPreviewId, &api::Channel::id);
    if (channel == m_privateChannels.end() || !channel->isMessageRequest())
      return;
    const auto user = channel->recipients.empty() ? api::Account{"", channel->name} : channel->recipients.front();
    const auto name = userName(user.id, user.name);
    const bool narrow = x == chatLeft();
    rectangle(root, x, y, width, height, background);
    if (!narrow)
      rectangle(root, x - Style::borderWidth, y, 1, height, raised);
    m_dmPreviewBounds = {x, y + 66, width, std::max(0.0F, height - 66)};
    if (narrow)
      iconButton(
          root,
          {x + Style::spaceSm, y + (Style::spaceLg + Style::borderWidth), 30, 30, Action::DmAction, "close-request"},
          "arrow-left", "Back to requests"
      );
    avatar(root, x + (narrow ? 48 : 16), y + Style::spaceRelaxed, layout::Avatar::dm, user.avatar, name, user.id);
    label(
        root, x + (narrow ? 86 : 56), y + (Style::spaceXl + Style::borderWidth),
        width - (narrow ? 132 : 102) - layout::ChannelHeader::actionStep, name, Style::fontSizeBody, foreground,
        FontWeight::SemiBold
    );
    iconButton(
        root,
        {x + width - 42 - layout::ChannelHeader::actionStep, y + (Style::spaceLg + Style::borderWidth), 30, 30,
         Action::DmAction, "close-request"},
        "x", "Close request"
    );
    if (!user.id.empty())
      m_hits.push_back(
          {x + (narrow ? 48 : 16), y + (Style::spaceLg + Style::borderWidth),
           std::max(0.0F, width - (narrow ? 100 : 68) - layout::ChannelHeader::actionStep), 32, Action::DmAction,
           "profile:" + user.id}
      );
    rectangle(root, x, y + 65, width, 1, raised);

    const float top = y + 82, bottom = y + height - 16, inner = std::max(1.0F, width - 40);
    const auto measure = [&](const std::string& text, float size, float wrap, FontWeight weight = FontWeight::Normal) {
      const auto bounds = m_window.target().renderer().measureText(text, size, weight, wrap, 0);
      return std::max(size + 4, bounds.bottom - bounds.top);
    };
    const auto description = "This is the beginning of your direct message history with " + name + '.';
    const float descriptionHeight = measure(description, 13, inner);
    const float actionY = 158 + descriptionHeight;
    const float feedbackHeight = m_dmStatus.empty() ? 0 : measure(m_dmStatus, 12, inner) + 12;
    const float introHeight = actionY + 50 + feedbackHeight;
    struct Row {
      const api::Message* message;
      std::string text;
      float bodyHeight;
    };
    std::vector<Row> rows;
    float total = introHeight;
    for (const auto& message : m_dmPreview) {
      // A request remains plain text until accepted: no media, embeds, active
      // links, selection actions or read acknowledgements enter this panel.
      auto text = desktopMessagePreview(message, [&](char kind, const std::string& id) -> std::string {
        if (kind != '@')
          return {};
        if (const auto mention = message.mentions.find(id); mention != message.mentions.end())
          return userName(id, mention->second);
        if (id == user.id)
          return name;
        const auto person = lookupUser(id);
        return person.name == "Unknown user" ? "" : userName(id, person.name);
      });
      if (!message.attachments.empty() || !message.embeds.empty()) {
        if (message.content.empty())
          text.clear();
        else
          text += '\n';
        text += "[Attachment hidden until accepted]";
      }
      const float bodyHeight = measure(text, 14, std::max(1.0F, inner - (Style::spaceGroup - Style::spaceXxs)));
      total += 26 + bodyHeight + (Style::spaceLg + Style::spaceXxs);
      rows.push_back({&message, std::move(text), bodyHeight});
    }
    if (rows.empty())
      total += 60;
    const float viewport = std::max(0.0F, bottom - top);
    m_scrolling.limit(m_dmPreviewScroll, std::max(0.0F, total - viewport));
    auto content = std::make_unique<Node>();
    content->setPosition(x + Style::spaceRelaxed, top);
    content->setSize(inner, viewport);
    content->setClipChildren(true);
    content->setTextScrollOffset(-m_dmPreviewScroll);
    float rowY = std::max(0.0F, viewport - total) - m_dmPreviewScroll;
    avatar(*content, 0, rowY, layout::Avatar::request, user.avatar, name, user.id);
    label(*content, 0, rowY + 88, inner, name, Style::fontSizeProfile, foreground, FontWeight::SemiBold);
    label(
        *content, 0, rowY + 119, inner, user.username.empty() ? name : '@' + user.username, Style::fontSizeBody, muted
    );
    label(*content, 0, rowY + 148, inner, description, Style::fontSizeCaption, muted, FontWeight::Normal, 0);
    const auto button = [&](const std::string& caption, const std::string& command, float localX, float localY,
                            float size, bool primary = false) {
      dmButton(*content, {localX, localY, size, 26, Action::DmAction, command}, caption, primary);
    };
    if (m_dmPending.contains(channel->id))
      label(
          *content, 0, rowY + actionY + Style::spaceCompact, inner, "Updating request…", Style::fontSizeDescription,
          muted
      );
    else if (m_dmConfirm == "ignore:" + channel->id) {
      button("Ignore request?", "confirm-ignore:" + channel->id, 0, rowY + actionY, 126);
      button("Cancel", "cancel", 134, rowY + actionY, 70);
    } else {
      button("Accept DM", "accept:" + channel->id, 0, rowY + actionY, 104, true);
      button("Ignore", "ignore:" + channel->id, 112, rowY + actionY, 76);
    }
    if (!m_dmStatus.empty())
      label(
          *content, 0, rowY + actionY + (Style::spaceSection - Style::spaceXxs), inner, m_dmStatus,
          Style::fontSizeDescription, muted, FontWeight::Normal, 0
      );
    rowY += introHeight;
    rectangle(*content, 0, rowY - Style::spaceDense, inner, 1, raised);
    if (rows.empty()) {
      if (m_dmPreviewLoading) {
        rectangle(*content, 0, rowY + Style::spaceXs, 28, 28, lerpColor(background, muted, .17F), 14);
        rectangle(
            *content, 46, rowY + (Style::spaceXs + Style::borderWidth), inner * .4F, 8,
            lerpColor(background, muted, .23F), 4
        );
        rectangle(
            *content, 46, rowY + ((Style::spaceXl - Style::spaceXxs) + Style::borderWidth), inner * .65F, 9,
            lerpColor(background, muted, .13F), 4
        );
      } else if (m_dmStatus.empty())
        label(
            *content, 0, rowY + Style::spaceCompact, inner, "This request has no messages to display.",
            Style::fontSizeCaption, muted, FontWeight::Normal, 2
        );
      else
        button("Retry", "retry-request", 0, rowY + Style::spaceCompact, 76);
    }
    for (const auto& row : rows) {
      const auto& message = *row.message;
      const float rowHeight = 26 + row.bodyHeight + (Style::spaceLg + Style::spaceXxs);
      if (rowY < viewport && rowY + rowHeight > 0) {
        const auto author =
            message.authorId == user.id || message.authorId.empty() ? name : userName(message.authorId, message.author);
        const auto face = message.avatar.empty() ? user.avatar : message.avatar;
        avatar(
            *content, 0, rowY + Style::spaceXxs, layout::Avatar::member, face, author,
            message.authorId.empty() ? user.id : message.authorId
        );
        label(
            *content, 46, rowY + Style::borderWidth, std::max(1.0F, inner - 94), author, Style::fontSizeCaption,
            foreground, FontWeight::SemiBold
        );
        const auto timestamp = formatMessageTime(message.timestamp);
        if (!timestamp.empty())
          label(
              *content, inner - (Style::spaceSection + Style::spaceXxs), rowY + (Style::spaceXxs + Style::borderWidth),
              42, timestamp, Style::fontSizeDetail, muted
          );
        label(
            *content, 46, rowY + (Style::spaceXl + Style::borderWidth),
            std::max(1.0F, inner - (Style::spaceGroup - Style::spaceXxs)), row.text, Style::fontSizeBody, foreground,
            FontWeight::Normal, 0
        );
      }
      rowY += rowHeight;
    }
    root.addChild(std::move(content));
    settingsScrollbar(root, "dm-request-scroll", {x + width - 10, top, 6, viewport}, total, m_dmPreviewScroll);
  }
} // namespace app
