#include "app/automod_message.h"
#include "app/client_application.h"
#include "app/message_time.h"
#include "app/system_message.h"
#include "app/theme.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace app {
  void ClientApplication::drawMessageStrikes(
      Node& parent, const MessageLayout& message, float x, float y, Color color, bool showSpoilers,
      std::optional<float> baseline, std::optional<float> height
  ) {
    for (const auto& area : messageStrikeAreas(message)) {
      const auto& style = message.runs[area.first].style;
      if (height && area.y + area.height > *height)
        continue;
      if (style.spoiler && !showSpoilers)
        continue;
      const auto metrics = m_window.target().renderer().measureText(
          messageMarkup("x", style, false), style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
          TextEllipsize::End, true
      );
      const float thickness = layout::Markdown::strikeThickness(style.size);
      const float center = (metrics.inkTop + metrics.inkBottom) / 2;
      const float top = y + baseline.value_or(area.y - metrics.top) + center - thickness / 2;
      rectangle(parent, x + area.x, top, area.width, thickness, style.muted ? theme::muted : color);
      parent.children().back()->setHitTestVisible(false);
    }
  }
  void ClientApplication::drawSystemNotice(
      Node& parent, const api::Message& message, const MessageLayout& layout, float y, float width
  ) {
    using namespace theme;
    const float absoluteX = chatLeft() + 74, absoluteY = m_messageViewportTop + y;
    const bool over = hovered(chatLeft() + Style::spaceXl, absoluteY, width + 50, layout.height);
    const auto& automod = layout.autoModeration;
    glyph(
        parent, Style::spaceSm, y + Style::spaceXxs, Style::glyphSizeLarge, automod ? "shield-check" : "arrow-right",
        automod ? accent : success
    );
    if (automod) {
      avatar(
          parent, layout::Chat::authorGutter, y + automod->authorTop, layout::AutoModeration::avatarSize,
          message.avatar, userName(message.authorId, message.author), message.authorId
      );
    }
    m_messageText[message.id] = layout.plainText;
    m_hits.push_back({absoluteX, absoluteY, width, layout.height, Action::SelectMessage, message.id});
    if (automod) {
      Hit hit{
          absoluteX,
          absoluteY + automod->authorTop,
          layout::AutoModeration::avatarSize,
          layout::AutoModeration::avatarSize,
          Action::UserProfile,
          message.authorId
      };
      m_hits.push_back(hit);
      hit.action = Action::UserMenu;
      m_contextTargets.push_back(std::move(hit));
    }
    for (const auto& run : layout.runs) {
      const float x = 50 + run.x, top = y + run.y;
      if (automod
          && !automod->matched.empty()
          && run.offset < automod->contentEnd
          && run.offset + run.text.size() > automod->contentStart) {
        for (auto at = layout.plainText.find(automod->matched, automod->contentStart);
             at != std::string::npos && at + automod->matched.size() <= automod->contentEnd;
             at = layout.plainText.find(automod->matched, at + automod->matched.size())) {
          const auto from = std::max(at, run.offset),
                     to = std::min(at + automod->matched.size(), run.offset + run.text.size());
          if (to <= from || !run.image.empty())
            continue;
          const auto measure = [&](std::string_view text) {
            return m_window.target()
                .renderer()
                .measureText(
                    messageMarkup(text, run.style), run.style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
                    TextEllipsize::End, true
                )
                .width;
          };
          rectangle(
              parent, x + measure(std::string_view(run.text).substr(0, from - run.offset)), top,
              measure(std::string_view(run.text).substr(from - run.offset, to - from)), run.height,
              withAlpha(accent, Style::textHighlightAlpha), Style::radiusXs
          );
        }
      }
      if (run.channelChip) {
        drawChannelLink(parent, run, message.id, x, top, absoluteX + run.x, absoluteY + run.y);
        continue;
      }
      if (m_selectedMessage == message.id && m_selectedText == layout.plainText && m_chatEdit.selected()) {
        const auto from = std::max(std::min(m_chatEdit.caret, m_chatEdit.anchor), run.offset);
        const auto to = std::min(std::max(m_chatEdit.caret, m_chatEdit.anchor), run.offset + run.text.size());
        if (to > from) {
          const auto measure = [&](std::string_view text) {
            return m_window.target()
                .renderer()
                .measureText(
                    messageMarkup(text, run.style), run.style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
                    TextEllipsize::End, true
                )
                .width;
          };
          const float offset = run.image.empty() ? measure(std::string_view(run.text).substr(0, from - run.offset)) : 0;
          const float length =
              run.image.empty() ? measure(std::string_view(run.text).substr(from - run.offset, to - from)) : run.width;
          rectangle(
              parent, x + offset, top, length, run.height, withAlpha(accent, Style::textHighlightAlpha), Style::radiusXs
          );
        }
      }
      if (!run.image.empty()) {
        if (!emojiImage(parent, x, top, run.width, run.image))
          label(parent, x, top, run.width, "…", run.style.size, foreground);
      } else if ((!automod && run.style.mention) || !run.style.userMention.empty())
        roleLabel(
            parent, x, top, run.width + Style::spaceXxs, run.text, run.style.size, foreground,
            run.style.bold ? FontWeight::Bold : FontWeight::Normal, automod ? run.style.userMention : message.authorId,
            "system:" + message.id, over,
            automod && run.style.userMention != message.authorId ? std::nullopt : message.memberRoles
        );
      else
        label(
            parent, x, top, run.width + Style::spaceXxs, messageMarkup(run.text, run.style, false), run.style.size,
            automod && !run.style.muted ? foreground : muted, FontWeight::Normal, 1, true
        );
      m_textAreas.push_back({run, message.id, absoluteX + run.x, absoluteY + run.y});
      const auto& user = automod ? run.style.userMention : message.authorId;
      if ((run.style.mention || !run.style.userMention.empty()) && !user.empty()) {
        Hit hit{absoluteX + run.x, absoluteY + run.y, run.width, run.height, Action::UserProfile, user};
        const float bottom = std::min(m_messageViewportBottom, hit.y + hit.height);
        hit.y = std::max(m_messageViewportTop, hit.y);
        hit.height = std::max(0.0F, bottom - hit.y);
        m_hits.push_back(hit);
        hit.action = Action::UserMenu;
        m_contextTargets.push_back(std::move(hit));
      }
    }
    drawMessageStrikes(parent, layout, layout::Chat::authorGutter, y, automod ? foreground : muted);
    const auto timestamp = formatMessageClock(message.timestamp, m_preferences.twelveHourClock);
    if (!layout.runs.empty() && !timestamp.empty()) {
      const auto headerEnd = automod
          ? std::ranges::find_if(layout.runs, [&](const auto& run) { return run.y >= automod->authorTop; })
          : layout.runs.end();
      const auto& last = *(headerEnd == layout.runs.begin() ? headerEnd : std::prev(headerEnd));
      const auto caption = timestamp + (message.deleted ? " · Deleted" : "");
      const auto timestampWidth = m_window.target().renderer().measureText(caption, Style::fontSizeDetail).width;
      label(
          parent, 50 + last.x + last.width + Style::spaceCompact, y + last.y + Style::spaceXs,
          timestampWidth + Style::spaceXxs, caption, Style::fontSizeDetail, message.deleted ? theme::danger : muted
      );
    }
    if (automod) {
      const std::array<std::pair<std::string_view, Action>, 3> actions{
          {{"Actions", Action::AutoModActions},
           {"Jump to Surrounding", Action::AutoModJump},
           {"Report Issues", Action::AutoModReport}}
      };
      for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto& frame = automod->actions.controls[i];
        const auto& [caption, action] = actions[i];
        bool enabled = !message.deleted;
        if (action == Action::AutoModJump) {
          const auto target = autoModerationJump(message);
          enabled &= target && !channelMention(m_guildId, target->first).link.empty();
        } else if (action == Action::AutoModReport)
          enabled &= !autoModerationField(autoModerationEmbed(message), "decision_id").empty();
        const float localX = layout::Chat::authorGutter + frame.x;
        const float localY = y + automod->actionsTop + frame.y;
        const Hit hit{absoluteX + frame.x, absoluteY + automod->actionsTop + frame.y, frame.width,
                      frame.height,        enabled ? action : Action::None,           message.id};
        const bool hovered = enabled && hit.contains(m_pointerX, m_pointerY);
        if (hovered)
          rectangle(parent, localX, localY, frame.width, frame.height, raised, Style::controlRadius);
        const auto color = enabled ? accent : muted;
        float captionX = localX + layout::AutoModeration::actionPadding;
        if (i == 0) {
          glyph(
              parent, captionX, localY + (frame.height - layout::AutoModeration::actionGlyph) / 2,
              layout::AutoModeration::actionGlyph, "shield-check", color
          );
          captionX += layout::AutoModeration::actionGlyph + Style::spaceXs;
        }
        const auto metrics =
            m_window.target().renderer().measureText(std::string(caption), layout::AutoModeration::actionSize);
        label(
            parent, captionX, localY + (frame.height - (metrics.bottom - metrics.top)) / 2,
            frame.width - (captionX - localX) - layout::AutoModeration::actionPadding,
            messageMarkup(caption, {.underline = hovered}), layout::AutoModeration::actionSize, color,
            FontWeight::Normal, 1, true
        );
        m_hits.push_back(hit);
      }
    }
    reactionStrip(&parent, message, 50, y + std::max(22.0F, layout.height) + 6, width, true);
  }

  void ClientApplication::demoJoinStep() {
    const auto require = [](bool ok, const char* reason) {
      if (!ok)
        throw std::runtime_error(reason);
    };
    const auto step = m_demoInteractionStep++;
    int joinArrows = 0;
    const auto inspect = [&](auto&& self, const Node& node) -> void {
      if (node.type() == NodeType::Text) {
        const auto& text = static_cast<const TextNode&>(node);
        require(text.text() != "[System message]", "Join notices must not show the generic placeholder");
      }
      for (const auto& child : node.children())
        self(self, *child);
    };
    inspect(inspect, *m_scene);
    for (const auto& message : m_messages) {
      require(message.type == 7 && message.content.empty(), "Join presentation must preserve empty protocol content");
      require(
          m_messageText.at(message.id) == joinNoticeText(message, userName(message.authorId, message.author)),
          "Selection/copy text must contain the generated notice and current display name"
      );
      const auto text = m_messageLayouts.at(message.id).layout.plainText;
      require(text.find("@") == std::string::npos, "Join names must not gain a mention prefix");
      if (m_messageTops.at(message.id) >= m_messageViewportTop)
        ++joinArrows;
    }
    require(joinArrows > 0, "Join notices must be visible");
    const auto reaction =
        std::ranges::find_if(m_hits, [](const auto& hit) { return hit.action == Action::ToggleReaction; });
    require(step == 1 || reaction != m_hits.end(), "Join notices retain their normal reaction targets");
    const auto add = std::ranges::find_if(m_hits, [](const auto& hit) { return hit.action == Action::AddReaction; });
    require(step == 1 || add != m_hits.end(), "Join reaction rows expose the native add-reaction control");
    if (step == 0) {
      m_window.click(
          add->x + Style::spaceSm, add->y + Style::spaceSm + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          272
      );
      m_window.released();
      require(m_emojiMenu, "Join reaction buttons must open the real emoji picker");
    } else if (step == 1) {
      require(m_emojiMenu, "Join reaction picker survives the next frame");
      closeEmojiPicker();
      api::UserUpdates names;
      names.users["11"].nickname = "Local nickname <b>literal</b>";
      m_userDirectory.apply(names);
      m_messageLayouts.clear();
    } else if (step == 2) {
      require(
          m_messageText.at(m_messages.front().id).find("Local nickname <b>literal</b>") != std::string::npos,
          "Live personal nicknames update system notices without markup injection"
      );
      m_preferences.compact = true;
      m_preferences.channelSidebarWidth = 420;
      m_messageLayouts.clear();
    } else {
      require(
          m_messageText.at(m_messages.front().id).find("Local nickname") != std::string::npos,
          "Join notices survive compact mode and sidebar resizing"
      );
      std::clog << "Offline server join replay passed\n";
    }
    m_window.invalidate();
  }
  void ClientApplication::autoModerationAction(const Hit& hit) {
    const auto message = std::ranges::find(m_messages, hit.id, &api::Message::id);
    if (message == m_messages.end() || message->deleted || message->type != autoModerationActionType)
      return;
    if (hit.action == Action::AutoModActions) {
      auto target = hit;
      target.action = Action::UserMenu;
      target.id = message->authorId;
      m_contextTargets.push_back(target);
      openContext(target.x + target.width / 2, target.y + target.height / 2);
    } else if (hit.action == Action::AutoModJump) {
      const auto target = autoModerationJump(*message);
      if (target && !channelMention(m_guildId, target->first).link.empty())
        jumpToMessage(target->first, target->second);
    } else if (hit.action == Action::AutoModReport) {
      if (autoModerationField(autoModerationEmbed(*message), "decision_id").empty())
        return;
      closeContext();
      m_dialogKind = "confirm";
      m_dialogTitle = "Report an AutoMod issue?";
      m_dialogText.clear();
      m_dialogError.clear();
      m_dialogPending = false;
      m_dialogAction = api::Interaction{
          .kind = api::InteractionKind::AutoModerationFeedback,
          .channel = m_channelId,
          .message = message->id,
          .guild = m_guildId,
          .revision = ++m_dialogGeneration,
          .content = autoModerationEmbed(*message)->description,
          .customId = std::string(autoModerationField(autoModerationEmbed(*message), "decision_id")),
          .markAutoModerationFeedback = (selectedChannelPermissions().value_or(0) & api::manageMessages) != 0
      };
    }
    m_window.invalidate();
  }
  void ClientApplication::demoMarkdownStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto step = m_demoInteractionStep++;
    const auto& message = m_messageLayouts.at("9401").layout;
    const auto links = messageLinkAreas(message);
    const auto spoilers = messageSpoilerAreas(message);
    const auto strikes = messageStrikeAreas(message);
    require(!links.empty() && !spoilers.empty() && !strikes.empty(), "The Markdown fixture contains all span types");
    const auto textArea = [&](std::size_t index) {
      const auto found = std::ranges::find_if(m_textAreas, [&](const auto& area) {
        return area.message == "9401" && area.run.offset == message.runs[index].offset;
      });
      require(found != m_textAreas.end(), "The formatted text is mounted");
      return *found;
    };
    const auto inspect = [&](auto&& self, const Node& node, float x, float y, auto&& predicate) -> bool {
      x += node.x();
      y += node.y();
      if (predicate(node, x, y))
        return true;
      return std::ranges::any_of(node.children(), [&](const auto& child) {
        return self(self, *child, x, y, predicate);
      });
    };
    for (std::size_t i = 0; i < message.runs.size(); ++i) {
      const auto& run = message.runs[i];
      if (run.style.spoiler && !spoilersVisible("9401"))
        continue;
      auto style = run.style;
      style.linkHovered = step == 1 && style.link == "https://example.com/markdown";
      const auto markup = messageMarkup(run.text, style, false);
      require(
          inspect(
              inspect, *m_scene, 0, 0,
              [&](const Node& node, float, float) {
                return node.type() == NodeType::Text
                    && static_cast<const TextNode&>(node).text() == markup
                    && static_cast<const TextNode&>(node).fontSize() == run.style.size;
              }
          ),
          "Rendered Markdown retains its measured style and hover-only link underline"
      );
      if (run.style.muted)
        require(run.style.size < m_preferences.messageSize, "Wrapped subtext stays smaller than body text");
    }
    for (const auto& area : strikes) {
      const auto first = textArea(area.first);
      require(
          inspect(
              inspect, *m_scene, 0, 0,
              [&](const Node& node, float x, float y) {
                return node.type() == NodeType::Rect
                    && x == first.x
                    && node.width() == area.width
                    && node.height() == layout::Markdown::strikeThickness(first.run.style.size)
                    && y >= first.y
                    && y < first.y + first.run.height;
              }
          ),
          "The scene paints one uninterrupted strike line across each span, including spaces and nested emphasis"
      );
    }
    const auto reveal = std::ranges::find_if(m_hits, [](const auto& hit) {
      return hit.action == Action::RevealSpoiler && hit.id == "9401";
    });
    if (step == 0) {
      require(
          std::ranges::count_if(
              m_hits, [](const auto& hit) { return hit.action == Action::RevealSpoiler && hit.id == "9401"; }
          ) == std::ssize(spoilers),
          "Spoiler clicks cover complete line masks"
      );
      const auto gap = std::ranges::find_if(message.runs, [](const auto& run) {
        return run.text == " " && run.style.link == "https://example.com/markdown";
      });
      require(gap != message.runs.end(), "The link contains a styled whitespace run");
      const auto target = textArea(gap - message.runs.begin());
      m_window.motion(
          target.x + target.run.width / 2,
          target.y + target.run.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
    } else if (step == 1) {
      require(reveal != m_hits.end(), "The spoiler exposes a continuous click target");
      const float x = reveal->x + reveal->width / 2;
      const float y = reveal->y + reveal->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 1);
      m_window.released();
      m_window.motion(0, 0);
    } else if (step == 2) {
      require(
          spoilersVisible("9401") && reveal == m_hits.end(), "Clicking the spoiler reveals its whole formatted span"
      );
      m_preferences.messageSize = Style::fontSizeHeader;
      m_preferences.channelSidebarWidth = layout::Sidebar::maximumWidth - Style::spaceSection * 2;
      m_messageLayouts.clear();
      m_window.setUiScale(1.5F);
    } else if (step == markdownReplayFrames - 1)
      std::clog << "Offline Markdown hover, continuous spans and subtext replay passed\n";
    m_window.invalidate();
  }
  void ClientApplication::demoEmbedStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto step = m_demoInteractionStep++;
    std::clog << "Offline embed replay step " << step << '\n';
    if (step == embedReplayFrames - 1) {
      require(m_drafts[m_channelId] == "Retained embed removal draft", "Embed removal preserves the composer draft");
      std::clog << "Offline AutoMod, embed removal and continuous link hover replay passed\n";
      return;
    }
    const auto click = [&](Action action) {
      const auto found = std::ranges::find(m_hits, action, &Hit::action);
      require(found != m_hits.end() && found->height > 0, "The actual message action control is visible");
      const float x = found->x + found->width / 2;
      const float y = found->y + found->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 1);
      m_window.released();
    };
    if (step >= 11) {
      constexpr std::uint64_t retainedFlag = 1ULL << 12;
      constexpr std::uint64_t concurrentFlag = 1ULL << 14;
      if (step == 11) {
        require(m_channelId == "102", "Jump to Surrounding navigates to the cached source channel");
        m_window.setUiScale(m_demoOverlay.ends_with("narrow") ? 1.5F : 1.0F);
        selectChannel("101");
        api::Message message{
            .id = "9301",
            .channelId = m_channelId,
            .author = m_account.name,
            .content = "https://example.com/preview",
            .timestamp = "2026-10-20T10:45:00Z",
            .authorId = m_account.id
        };
        message.flags = retainedFlag;
        message.embeds = {
            {.title = "A removable link preview", .url = "https://example.com/preview"}, {.title = "A second preview"}
        };
        message.attachments.push_back({.id = "9302", .filename = "keep.txt", .url = "https://example.com/keep.txt"});
        m_messages = {message};
        m_channelMessages[m_channelId] = m_messages;
        m_messageLayouts.clear();
        m_messageScroll = 0;
        m_preferences.compact = false;
        m_preferences.logDeletedMessages = false;
        m_drafts[m_channelId] = "Retained embed removal draft";
      } else if (step == 12) {
        const auto target = std::ranges::find_if(m_contextTargets, [](const auto& hit) {
          return hit.action == Action::MessageMenu && hit.id == "9301";
        });
        require(target != m_contextTargets.end(), "The removable message has a native context target");
        m_window.motion(
            target->x + Style::spaceXl,
            target->y + Style::spaceSm + m_window.titlebarHeight() - layout::Window::titlebarHeight
        );
      } else if (step == 13) {
        require(
            std::ranges::count(m_hits, Action::RemoveEmbeds, &Hit::action) == 2,
            "Each embed exposes its own close control on message hover"
        );
        click(Action::RemoveEmbeds);
        require(
            m_dialogKind == "confirm" && m_dialogAction && m_dialogAction->kind == api::InteractionKind::RemoveEmbeds,
            "The close control opens embed removal confirmation"
        );
        require(
            !(m_messages.front().flags & api::messageSuppressEmbeds), "Opening confirmation does not remove embeds"
        );
      } else if (step == 14) {
        bool title = false, explanation = false, hint = false, remove = false;
        const auto inspect = [&](auto&& self, const Node& node) -> void {
          if (node.type() == NodeType::Text) {
            const auto& text = static_cast<const TextNode&>(node);
            title |= text.text() == "Are you sure?";
            explanation |= text.text() == "This will remove all embeds on this message for everyone.";
            hint |= text.text().contains("Hold Shift");
            remove |= text.text() == "Remove All Embeds";
          }
          for (const auto& child : node.children())
            self(self, *child);
        };
        inspect(inspect, *m_scene);
        require(
            title && explanation && hint && remove,
            "The confirmation paints the warning, Shift hint and explicit action"
        );
        require(
            m_dialogBounds.x >= 0 && m_dialogBounds.x + m_dialogBounds.width <= m_window.width(),
            "Embed confirmation stays inside the window"
        );
        click(Action::CloseDialog);
        require(
            m_dialogKind.empty() && !(m_messages.front().flags & api::messageSuppressEmbeds),
            "Cancel keeps all message embeds"
        );
        const auto target = std::ranges::find_if(m_contextTargets, [](const auto& hit) {
          return hit.action == Action::MessageMenu && hit.id == "9301";
        });
        require(target != m_contextTargets.end(), "The message remains available after cancel");
        openContext(target->x + target->width / 2, target->y + target->height / 2);
        require(
            std::ranges::any_of(
                m_contextEntries, [](const auto& entry) { return entry.id == static_cast<int>(Action::RemoveEmbeds); }
            ),
            "The message menu also offers embed removal"
        );
      } else if (step == 15) {
        contextAction(Action::RemoveEmbeds);
        require(m_dialogKind == "confirm", "The context menu uses the same confirmation");
      } else if (step == 16) {
        click(Action::SubmitDialog);
        require(m_status == "Demo mode is read-only.", "The offline confirmation never submits account requests");
        m_dialogPending = true;
        interactionResult(
            {.operation = api::Operation::Interaction,
             .error = "Offline embed removal failure",
             .interaction = *m_dialogAction}
        );
        require(
            !m_dialogPending && !m_dialogError.empty() && !(m_messages.front().flags & api::messageSuppressEmbeds),
            "A failed removal keeps the confirmation, error and original embeds"
        );
      } else if (step == 17) {
        updateMessage(
            m_channelId, "9301",
            api::MessagePatch{.content = "Newer message text", .flags = retainedFlag | concurrentFlag}
        );
        auto request = *m_dialogAction;
        const auto patch = api::removedEmbedsPatch(
            request,
            {{"id", "9301"},
             {"channel_id", m_channelId},
             {"flags", retainedFlag | api::messageSuppressEmbeds},
             {"content", "Old response text"},
             {"embeds", nlohmann::json::array()}}
        );
        require(patch.has_value(), "The synthetic response confirms the selected message");
        interactionResult({.operation = api::Operation::Interaction, .patch = *patch, .interaction = request});
        require(
            m_dialogKind.empty()
                && m_messages.front().content == "Newer message text"
                && m_messages.front().attachments.size() == 1
                && m_messages.front().flags == (retainedFlag | concurrentFlag | api::messageSuppressEmbeds),
            "Confirmed removal preserves concurrent edits, attachments and unrelated flags"
        );
      } else if (step == 18) {
        require(m_messageLayouts.at("9301").layout.embeds.empty(), "Confirmed removal hides every embed");
        require(
            std::ranges::none_of(m_hits, [](const auto& hit) { return hit.action == Action::RemoveEmbeds; }),
            "Suppressed previews no longer expose removal controls"
        );
        updateMessage(m_channelId, "9301", api::MessagePatch{.flags = retainedFlag});
        m_messages.front().authorId = "11";
        m_roleDirectory.apply({false, {{.guild = m_guildId, .owner = "12"}}});
      } else if (step == 19) {
        require(
            std::ranges::none_of(m_hits, [](const auto& hit) { return hit.action == Action::RemoveEmbeds; }),
            "Another author's embeds have no close control without permission"
        );
        removeMessageEmbeds("9301", false);
        require(m_dialogKind.empty(), "Unknown permissions cannot open a removal confirmation");
        m_messages.front().authorId = m_account.id;
        removeMessageEmbeds("9301", true);
        require(
            m_dialogKind.empty()
                && m_status == "Demo mode is read-only."
                && !(m_messages.front().flags & api::messageSuppressEmbeds),
            "The Shift path skips confirmation and still uses the normal request gate"
        );
        removeMessageEmbeds("9301", false);
      } else if (step == 20) {
        m_messages.front().authorId = "11";
        click(Action::SubmitDialog);
        require(
            !m_dialogPending && m_dialogError.contains("cannot remove"),
            "Submission rechecks author and permissions after the popup opens"
        );
      } else if (step == 21) {
        const auto request = *m_dialogAction;
        deleteMessages(m_channelId, {"9301"});
        require(m_dialogKind.empty(), "Live deletion closes the removal confirmation");
        interactionResult(
            {.operation = api::Operation::Interaction,
             .patch = api::MessagePatch{.flags = retainedFlag | api::messageSuppressEmbeds},
             .interaction = request}
        );
        require(m_messages.empty(), "A late removal response cannot recreate a deleted message");
      }
      m_window.invalidate();
      return;
    }
    const auto& alert = m_messageLayouts.at("9201").layout;
    require(
        alert.autoModeration
            && alert.plainText.starts_with("AutoMod")
            && !alert.plainText.contains("decision_id")
            && !alert.plainText.contains("opaque-decision"),
        "The parsed AutoMod fixture paints its dedicated notice instead of raw embed metadata"
    );
    if (step == 9) {
      require(
          alert.plainText.contains("Flagged content unavailable"),
          "Hiding embeds retains an explicit AutoMod summary without raw fields"
      );
      m_preferences.showEmbeds = true;
      m_messageLayouts.clear();
      m_window.setUiScale(1);
      m_messageScroll = 0;
      m_window.invalidate();
      return;
    }
    if (step == 10) {
      click(Action::AutoModJump);
      m_window.invalidate();
      return;
    }
    if (step == 3) {
      require(m_contextOpen && m_contextUser == "11", "Actions opens the affected user's native context menu");
      closeContext();
      m_window.invalidate();
      return;
    }
    if (step == 5 || step == 6) {
      require(
          m_dialogKind == "confirm"
              && m_dialogAction
              && m_dialogAction->kind == api::InteractionKind::AutoModerationFeedback,
          "Report Issues opens a native confirmation tied to the actual alert"
      );
      if (step == 5)
        click(Action::SubmitDialog);
      const auto request = *m_dialogAction;
      m_dialogPending = true;
      api::Event event{.operation = api::Operation::Interaction, .interaction = request};
      if (step == 5)
        event.error = "Offline simulated feedback failure";
      interactionResult(event);
      if (step == 5)
        require(
            !m_dialogKind.empty() && m_dialogError == event.error && !m_dialogPending,
            "Failed reports retain the confirmation and show a retryable error"
        );
      else {
        require(m_dialogKind.empty(), "Only a successful report acknowledgement closes the confirmation");
        m_window.motion(0, 0);
        m_window.setUiScale(1.5F);
      }
      m_window.invalidate();
      return;
    }
    const auto& preview = m_messageLayouts.at("9203").layout;
    const auto areas = messageLinkAreas(preview);
    require(!areas.empty(), "The preview has a linked title");
    require(
        std::ranges::count_if(
            m_hits,
            [](const auto& hit) { return hit.action == Action::OpenUrl && hit.id == "https://example.com/preview"; }
        ) == std::ssize(areas),
        "Actual embed hit targets span complete rendered lines instead of individual words"
    );
    const auto hasText = [&](std::string_view expected, float size) {
      bool found = false;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text) {
          const auto& text = static_cast<const TextNode&>(node);
          found |= text.text() == expected && text.fontSize() == size;
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      return found;
    };
    for (const auto& run : preview.runs) {
      require(run.style.size <= layout::Embed::titleSize, "Chat font changes do not enlarge embed text");
      if (run.style.link.empty())
        continue;
      auto style = run.style;
      style.linkHovered = step == 1;
      require(
          hasText(messageMarkup(run.text, style), run.style.size),
          "Rendered linked title glyphs underline together only while their logical link is hovered"
      );
    }
    if (step == 0) {
      const auto region = std::ranges::find_if(areas, [](const auto& area) { return area.last > area.first; });
      require(region != areas.end(), "The fixture has multiple linked words on one line");
      const auto& first = preview.runs[region->first];
      const auto& second = preview.runs[region->first + 1];
      const auto target = std::ranges::find_if(m_textAreas, [&](const auto& area) {
        return area.message == "9203" && area.run.offset == first.offset;
      });
      require(target != m_textAreas.end(), "The native preview text is mounted");
      const float gapX = target->x + (first.width + second.x - first.x) / 2;
      const float gapY = target->y + first.height / 2;
      require(
          std::ranges::any_of(
              m_hits,
              [&](const auto& hit) {
                return hit.action == Action::OpenUrl && hit.id == region->url && hit.contains(gapX, gapY);
              }
          ),
          "Whitespace between linked words remains inside the actual click target"
      );
      m_window.motion(gapX, gapY + m_window.titlebarHeight() - layout::Window::titlebarHeight);
    } else if (step == 1) {
      m_window.motion(0, 0);
      m_preferences.messageSize = Style::fontSizePage;
      m_messageLayouts.clear();
    } else if (step == 2) {
      click(Action::AutoModActions);
    } else if (step == 4) {
      click(Action::AutoModReport);
    } else if (step == 7) {
      m_preferences.compact = true;
      m_messageLayouts.clear();
    } else if (step == 8) {
      m_preferences.showEmbeds = false;
      m_messageLayouts.clear();
    }
    m_window.invalidate();
  }
} // namespace app
