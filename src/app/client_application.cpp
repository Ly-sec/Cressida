#include "app/client_application.h"

#include "api/uploads.h"
#include "app/automod_message.h"
#include "app/member_list_layout.h"
#include "app/message_group.h"
#include "app/message_layout.h"
#include "app/message_log_layout.h"
#include "app/message_mentions.h"
#include "app/message_preferences.h"
#include "app/reply_preview.h"
#include "app/single_instance.h"
#include "app/system_message.h"
#include "app/theme.h"
#include "core/deferred_call.h"
#include "core/environment.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/timer_manager.h"
#include "render/core/texture_manager.h"
#include "render/gl_shared_context.h"
#include "render/scene/image_node.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "render/scene/text_node.h"
#include "ui/controls/scrollbar.h"
#include "ui/palette.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;
  namespace {
    struct ChannelListRow {
      const api::Channel* channel;
      float top, height, advance;
    };
    template <class Muted, class MentionCount, class Visible>
    std::vector<ChannelListRow> channelListRows(
        const std::vector<api::Channel>& channels, const std::set<std::string>& collapsed, const std::string& selected,
        const ChannelListPreferences& preferences, Muted muted, MentionCount mentions, Visible visible
    ) {
      std::vector<ChannelListRow> rows;
      float top = 0;
      std::vector<const api::Channel*> order;
      for (const auto& id : preferences.pinned)
        if (const auto at = std::ranges::find(channels, id, &api::Channel::id); at != channels.end() && at->type != 4)
          order.push_back(&*at);
      for (const auto& channel : channels)
        if (std::ranges::find(preferences.pinned, channel.id) == preferences.pinned.end() || channel.type == 4)
          order.push_back(&channel);
      for (const auto* value : order) {
        const auto& channel = *value;
        if (!visible(channel))
          continue;
        const bool pinned = std::ranges::find(preferences.pinned, channel.id) != preferences.pinned.end();
        const auto parent = std::ranges::find(channels, channel.parentId, &api::Channel::id);
        const auto category = channel.isThread() && parent != channels.end() ? parent->parentId : channel.parentId;
        if (channel.type != 4
            && preferences.hideMuted
            && (muted(channel.id) || muted(category))
            && channel.id != selected
            && mentions(channel.id) == 0)
          continue;
        if (channel.type != 4
            && !pinned
            && collapsed.contains(category)
            && channel.id != selected
            && mentions(channel.id) == 0)
          continue;
        const bool direct =
            channel.kind() == api::ChannelType::DirectMessage || channel.kind() == api::ChannelType::GroupDirectMessage;
        const float height = channel.type == 4 ? layout::Sidebar::categoryHeight
            : direct                           ? layout::DmSidebar::conversationHeight
                                               : layout::Sidebar::rowHeight;
        const float advance = channel.type == 4 ? layout::Sidebar::categoryHeight
            : direct                            ? layout::DmSidebar::conversationStep
                                                : layout::Sidebar::rowStep;
        rows.push_back({&channel, top, height, advance});
        top += advance;
      }
      return rows;
    }

  } // namespace

  ClientApplication::ClientApplication(bool demoMode, std::string token, SingleInstance* instance)
      : m_instance(instance), m_token(std::move(token)) {
    const auto* trace = std::getenv("CRESSIDA_TRACE_SCROLL");
    m_traceScroll = trace && std::string_view(trace) == "1";
    const auto* animation = std::getenv("CRESSIDA_SCROLL_ANIMATION");
    m_animateWheel = !animation || std::string_view(animation) != "0";
    m_emojiCatalog.load(paths::assetPath("emoji.json"), paths::assetPath("emoji-aliases.json"));
    m_edit.end(m_token);
    m_showTokenEntry = !m_token.empty();
    m_fieldFocused = m_showTokenEntry;
    m_window.draw = [this] {
      const auto drawStarted = std::chrono::steady_clock::now();
      prepareDemo();
      draw();
      if (m_demo)
        verifySettingsScrollbarGutters();
      if (m_demo && m_demoOverlay == "chat-preload-cycle")
        demoChatPreloadStep();
      if (m_demo && m_demoOverlay == "tray-cycle" && m_demoInteractionStep < trayReplayFrames)
        demoTrayStep();
      if (m_demo && m_demoOverlay.starts_with("tray-startup-") && m_demoInteractionStep == 0)
        demoTrayStartup(true);
      if (m_demo && m_demoOverlay.starts_with("chat-links-cycle") && m_demoInteractionStep < chatLinkReplayFrames)
        demoChatLinksStep();
      if (m_demo && m_demoOverlay == "attachment-cycle" && m_demoInteractionStep < attachmentReplayFrames)
        demoAttachmentsStep();
      if (m_demo && m_demoOverlay.starts_with("autocomplete-tokens")) {
        const auto& input = *m_inputs.at("composer");
        const auto& display = input.displayValue();
        if (display.find("@Maya") == std::string::npos
            || display.find("@Moonwarden") == std::string::npos
            || display.find("<@") != std::string::npos
            || display.find("800000000") != std::string::npos)
          throw std::runtime_error("Composer tokens must display resolved names without wire IDs");
        std::size_t images = 0;
        const auto countImages = [&](auto&& self, const Node& node) -> void {
          if (const auto* image = dynamic_cast<const ImageNode*>(&node);
              image && image->visible() && image->width() > 0 && image->height() > 0)
            ++images;
          for (const auto& child : node.children())
            self(self, *child);
        };
        countImages(countImages, input);
        if (images != 2 || !input.value().contains("<@11>") || !input.value().contains("<a:umbriel:"))
          throw std::runtime_error("Composer emoji previews must retain original mention and emoji syntax");
        std::clog << "Offline composer presentation check passed\n";
      }
      if (m_demo && m_demoOverlay.starts_with("syntax-nix-flat-rail")) {
        const auto& boxes = m_messageLayouts.at(m_messages.front().id).layout.boxes;
        const auto code = std::ranges::find_if(boxes, [](const auto& box) { return !box.code.empty(); });
        if (code == boxes.end() || theme::panel == theme::background || theme::railBackground != theme::background)
          throw std::runtime_error("Code surface fixture requires a fence and matching rail/chat colors");
        int surfaces = 0;
        const auto inspect = [&](auto&& self, const Node& node) -> void {
          if (node.type() == NodeType::Rect
              && node.width() == code->width
              && node.height() == code->height
              && static_cast<const RectNode&>(node).style().fill == theme::panel)
            ++surfaces;
          for (const auto& child : node.children())
            self(self, *child);
        };
        inspect(inspect, *m_scene);
        if (surfaces != 1)
          throw std::runtime_error("Fenced code must retain its surface when rail and chat colors match");
        std::clog << "Offline code surface check passed\n";
      }
      if (m_demo && m_demoOverlay.starts_with("login-themed")) {
        const bool light = m_demoOverlay == "login-themed-light" || m_demoOverlay == "login-themed-system-light";
        const auto& palette =
            light ? m_themeSnapshot.selected("gruvbox").light : m_themeSnapshot.selected("gruvbox").dark;
        const auto* surface = dynamic_cast<const RectNode*>(m_scene->children().front().get());
        if (m_connected
            || !surface
            || surface->style().fill != rgbHex(palette.background)
            || theme::foreground != rgbHex(palette.foreground)
            || theme::accent != rgbHex(palette.accent))
          throw std::runtime_error("Startup and login must render with the selected palette and mode");
        std::clog << "Offline themed login check passed\n";
      }
      if (m_demo && m_demoOverlay.starts_with("channel-empty")) {
        int welcome = 0, generic = 0;
        const auto inspect = [&](auto&& self, const Node& node) -> void {
          if (node.type() == NodeType::Text) {
            const auto& text = static_cast<const TextNode&>(node).text();
            welcome += text.starts_with("Welcome to #");
            generic += text == "The conversation starts here." || text == "Send the first message.";
          }
          for (const auto& child : node.children())
            self(self, *child);
        };
        inspect(inspect, *m_scene);
        if (welcome != 1 || generic != 0)
          throw std::runtime_error("Empty channels must show one welcome view without a duplicate placeholder");
        std::clog << "Offline empty-channel welcome check passed\n";
      }
      if (m_demo && m_demoOverlay.starts_with("forwarded")) {
        const auto& message = m_messages.front();
        const auto& layout = m_messageLayouts.at(message.id).layout;
        const std::size_t expected = m_demoOverlay.find("text") != std::string::npos ? 0
            : m_demoOverlay.find("single") != std::string::npos                      ? 1
                                                                                     : 4;
        if (message.reply
            || message.content == "[System message]"
            || layout.forwards.size() != 1
            || layout.pictures.size() != expected)
          throw std::runtime_error(
              "Forwarded messages must show the snapshot and every image with explicit attribution"
          );
        const bool sourceLink = std::ranges::any_of(m_hits, [&](const auto& hit) {
          return hit.action == Action::OpenUrl && hit.id.ends_with("/800");
        });
        if (sourceLink == (m_demoOverlay.find("private") != std::string::npos))
          throw std::runtime_error("Forward source links must require known channel visibility");
        std::clog << "Offline forwarded snapshot, media gallery and source permission check passed\n";
      }
      if (m_demo && m_demoOverlay.starts_with("embed-components")) {
        const auto& layout = m_messageLayouts.at(m_messages.front().id).layout;
        const bool missing = m_demoOverlay.ends_with("missing"), spoiler = m_demoOverlay.ends_with("spoiler");
        if (missing) {
          if (!layout.boxes.empty() || !layout.pictures.empty())
            throw std::runtime_error("Unknown previews must not render an empty colored card");
        } else {
          if (!layout.plainText.contains("D (programming language)")
              || layout.pictures.size() != 1
              || layout.boxes.empty()
              || layout.boxes.front().height < 300
              || layout.boxes.front().embedAccent)
            throw std::runtime_error(
                "Modern article previews must include their text, full-size image and measured container"
            );
          if (spoiler && std::ranges::any_of(m_hits, [](const auto& hit) { return hit.action == Action::ViewImage; }))
            throw std::runtime_error("Spoiler preview media must remain hidden until explicitly revealed");
        }
        std::clog << "Offline component embed article and media check passed\n";
      }
      if (m_demo && m_demoOverlay.starts_with("reaction-spacing")) {
        for (std::size_t i = 0; i + 1 < m_messages.size(); ++i) {
          const auto& message = m_messages[i];
          const auto& next = m_messages[i + 1];
          const auto following = std::ranges::find_if(m_hits, [&](const auto& hit) {
            return hit.action == Action::SelectMessage && hit.id == next.id;
          });
          if (!continuesMessageGroup(message, next) || following == m_hits.end())
            throw std::runtime_error("Reaction spacing must cover visible consecutive messages from the same author");
          if (m_preferences.showReactions) {
            float chipBottom = 0;
            for (const auto& hit : m_hits)
              if (hit.action == Action::ToggleReaction && hit.id.starts_with(message.id + "\n"))
                chipBottom = std::max(chipBottom, hit.y + hit.height);
            if (chipBottom == 0 || std::abs(following->y - chipBottom - Style::spaceSm) > .5F)
              throw std::runtime_error("Reaction chips must leave eight pixels before the next message's text");
          } else {
            const float end =
                m_messageTops.at(message.id) + (i == 0 ? 18 : 0) + m_messageLayouts.at(message.id).layout.height;
            if (std::abs(following->y - end) > .5F)
              throw std::runtime_error("Hidden reactions must not leave an empty footer gap");
          }
        }
        std::clog << "Offline image, wrapped-reaction and continuation spacing check passed\n";
      }
      if (m_demo && m_demoOverlay.starts_with("chat-spacing")) {
        const auto& last = m_messages.back();
        const float messageEnd =
            m_messageTops.at(last.id) + 18 + std::max(22.0F, m_messageLayouts.at(last.id).layout.height);
        if (std::abs(m_messageViewportBottom - messageEnd - layout::Chat::bottomPadding) > .5F)
          throw std::runtime_error(
              "The final message retains its gutter above the reserved typing row without another group's margin"
          );
        std::clog << "Offline final-message composer spacing check passed\n";
      }
      if (m_demo && m_demoOverlay.starts_with("reply-grouping") && m_demoInteractionStep < replyGroupingReplayFrames) {
        const auto require = [](bool value, const char* reason) {
          if (!value)
            throw std::runtime_error(reason);
        };
        const auto avatars = std::ranges::count_if(m_hits, [&](const auto& hit) {
          return hit.action == Action::UserProfile
              && hit.id == "11"
              && hit.x == chatLeft() + layout::Chat::inset
              && hit.width == layout::Chat::avatarSize;
        });
        require(avatars == 2, "Only the two reply-started groups paint an avatar and author header");
        require(
            std::ranges::count_if(m_hits, [](const auto& hit) { return hit.action == Action::JumpReply; }) == 2,
            "Both replies retain their independent reference previews and jump targets"
        );
        for (const auto index : {std::size_t{0}, std::size_t{3}}) {
          const auto& reply = m_messages[index];
          const auto& next = m_messages[index + 1];
          const float expected =
              layout::Reply::step + layout::Chat::headerHeight + m_messageLayouts.at(reply.id).layout.height;
          require(
              std::abs(m_messageTops.at(next.id) - m_messageTops.at(reply.id) - expected) < Style::borderWidth,
              "An ordinary follow-up uses continuation spacing directly beneath its reply"
          );
        }
        require(
            std::abs(
                m_messageTops.at(m_messages[2].id)
                - m_messageTops.at(m_messages[1].id)
                - m_messageLayouts.at(m_messages[1].id).layout.height
            ) < Style::borderWidth,
            "Later follow-ups keep normal intra-group spacing"
        );
        for (const auto& message : m_messages)
          require(
              std::ranges::any_of(
                  m_hits, [&](const auto& hit) { return hit.action == Action::SelectMessage && hit.id == message.id; }
              ),
              "Grouped replies and follow-ups retain separate message selection targets"
          );
        const auto step = m_demoInteractionStep++;
        if (step > 0) {
          const auto& message = m_messages[step == 1 ? 1 : 2];
          const auto& formatted = m_messageLayouts.at(message.id).layout;
          const auto selected = std::ranges::find_if(m_hits, [&](const auto& hit) {
            return hit.action == Action::SelectMessage && hit.id == message.id;
          });
          require(selected != m_hits.end(), "The continuation retains its complete row hit region");
          float highlightTop = 0, highlightBottom = 0, inkTop = 0, inkBottom = 0, timeCenter = 0;
          bool hasInk = false, hasTime = false, hasHighlight = false;
          const auto inspect = [&](auto&& self, const Node& node, float x, float y) -> void {
            x += node.x();
            y += node.y();
            if (node.type() == NodeType::Rect
                && x == chatLeft() + Style::spaceDense
                && y <= selected->y + selected->height / 2
                && y + node.height() >= selected->y + selected->height / 2) {
              highlightTop = y;
              highlightBottom = y + node.height();
              hasHighlight = true;
            }
            if (node.type() == NodeType::Text) {
              const auto& text = static_cast<const TextNode&>(node);
              const auto metrics = m_window.target().renderer().measureText(
                  text.text(), text.fontSize(), text.fontWeight(), 0, 1, TextAlign::Start, {}, TextEllipsize::End, true
              );
              const float first = y + metrics.inkTop, last = y + metrics.inkBottom;
              for (const auto& run : formatted.runs)
                if (text.text() == messageMarkup(run.text, run.style, false)
                    && text.fontSize() == run.style.size
                    && x == selected->x + run.x) {
                  inkTop = hasInk ? std::min(inkTop, first) : first;
                  inkBottom = hasInk ? std::max(inkBottom, last) : last;
                  hasInk = true;
                }
              if (text.text() == formatMessageClock(message.timestamp, m_preferences.twelveHourClock)
                  && text.fontSize() == Style::fontSizeFine) {
                timeCenter = (first + last) / 2;
                hasTime = true;
              }
            }
            for (const auto& child : node.children())
              self(self, *child, x, y);
          };
          inspect(inspect, *m_scene, 0, 0);
          require(
              hasHighlight && hasInk && hasTime, "Hover paints the actual continuation background, text and timestamp"
          );
          const float center = (highlightTop + highlightBottom) / 2;
          const float tolerance = Style::borderWidth / Style::spaceXs;
          require(
              std::abs((inkTop + inkBottom) / 2 - center) < tolerance,
              "Grouped text ink is vertically centered in its hover background"
          );
          require(
              std::abs(timeCenter - center) < tolerance, "The continuation timestamp shares the text's visible center"
          );
        }
        if (step + 1 < replyGroupingReplayFrames) {
          const auto& next = m_messages[step == 0 ? 1 : 2];
          const auto target = std::ranges::find_if(m_hits, [&](const auto& hit) {
            return hit.action == Action::SelectMessage && hit.id == next.id;
          });
          require(target != m_hits.end(), "The next grouped hover row is mounted");
          m_window.motion(
              target->x + Style::spaceMd,
              target->y + target->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
          );
        }
        std::clog << "Offline reply grouping, centered text and timestamp check passed\n";
      }
      if (m_demo && m_demoOverlay.starts_with("autocomplete-frequent")) {
        if (m_completionItems.size() < 3
            || m_completionItems[0].id != "800000000000000005"
            || m_completionItems[1].id != "800000000000000101"
            || m_completionItems[2].id != "800000000000000001")
          throw std::runtime_error(
              "Emoji suggestions must rank usage across different names and same-name server duplicates"
          );
        chooseCompletion(1);
        if (m_recentEmojis.front() != "800000000000000101")
          throw std::runtime_error("Choosing an emoji suggestion must feed subsequent usage ranking");
        std::clog << "Offline emoji usage ranking and insertion check passed\n";
      }
      if (m_demo && m_demoOverlay == "deleted-cycle" && m_demoInteractionStep < 3)
        demoDeletedStep();
      if (m_demo && m_demoOverlay == "dm-compose-cycle" && m_demoInteractionStep < 16)
        demoDmComposeStep();
      if (m_demo && m_demoOverlay.starts_with("dm-home-cycle") && m_demoInteractionStep < dmHomeReplayFrames)
        demoDmHomeStep();
      if (m_demo && m_demoOverlay == "inline-edit-cycle" && m_demoInteractionStep < 8)
        demoMessageEditStep();
      if (m_demo && m_demoOverlay == "autocomplete-cycle" && m_demoInteractionStep < 9)
        demoCompletionStep();
      if (m_demo
          && m_demoOverlay.starts_with("window-controls-cycle")
          && m_demoInteractionStep < windowControlsReplayFrames)
        demoWindowControlsStep();
      if (m_demo && m_demoOverlay.starts_with("decorations-cycle") && m_demoInteractionStep < 4)
        demoDecorationsStep();
      if (m_demo && m_demoOverlay.starts_with("floating-cycle") && m_demoInteractionStep < floatingReplayFrames)
        demoFloatingStep();
      if (m_demo && m_demoOverlay.starts_with("reply-cycle") && m_demoInteractionStep < composerReplyReplayFrames)
        demoComposerReplyStep();
      if (m_demo && m_demoOverlay == "user-preferences-cycle" && m_demoInteractionStep < 12)
        demoUserPreferencesStep();
      if (m_demo
          && m_demoOverlay.starts_with("reduced-motion-cycle")
          && m_demoInteractionStep < reducedMotionReplayFrames)
        demoReducedMotionStep();
      if (m_demo && m_demoOverlay == "user-account-cycle" && m_demoInteractionStep < 16)
        demoUserAccountStep();
      if (m_demo && m_demoOverlay == "user-settings-lists-cycle" && m_demoInteractionStep < 30)
        demoUserSettingsListsStep();
      if (m_demo && m_demoOverlay == "settings-font-cycle" && m_demoInteractionStep < 12)
        demoFontStep();
      if (m_demo && m_demoOverlay == "server-settings-commands-cycle")
        demoServerCommandsStep();
      if (m_demo && m_demoOverlay == "server-settings-expressions-cycle")
        demoServerExpressionsStep();
      if (m_demo && m_demoOverlay == "server-settings-management-cycle")
        demoServerManagementStep();
      if (m_demo && m_demoOverlay == "server-settings-cycle")
        demoServerSettingsStep();
      if (m_demo && m_demoOverlay == "channel-create-cycle")
        demoChannelCreateStep();
      if (m_demo && m_demoOverlay == "channel-invite-cycle")
        demoChannelInviteStep();
      if (m_demo && (m_demoOverlay == "channel-menu-cycle" || m_demoOverlay == "channel-menu-scroll-cycle"))
        demoChannelMenuStep();
      if (m_demo && m_demoOverlay.starts_with("typing-cycle"))
        demoTypingStep();
      if (m_demo && m_demoOverlay.starts_with("rail-drag") && m_demoInteractionStep < 10)
        demoRailDragStep();
      if (m_demo && m_demoOverlay.starts_with("folder-animation") && m_demoInteractionStep < 9)
        demoFolderAnimationStep();
      if (m_demo && m_demoOverlay == "folder-menu-cycle")
        demoFolderMenuStep();
      if (m_demo && m_demoOverlay == "server-menu-cycle")
        demoServerMenuStep();
      if (m_demo && m_demoOverlay == "server-hover-cycle")
        demoGuildHoverStep();
      if (m_demo && m_demoOverlay == "themes-cycle" && m_demoInteractionStep < 11)
        demoThemeStep();
      if (m_demo && m_demoOverlay.starts_with("message-hover-cycle") && m_demoInteractionStep < 10) {
        const auto step = m_demoInteractionStep++;
        if (step > 0 && step < 5) {
          const bool expectActions = step == 1 || step == 3;
          int hoverBands = 0;
          const auto hoverColor = lerpColor(theme::background, theme::foreground, theme::lightTheme ? .035F : .04F);
          for (const auto& node : m_scene->children())
            if (node->type() == NodeType::Rect
                && static_cast<const RectNode&>(*node).style().fill == hoverColor
                && node->x() >= chatLeft())
              ++hoverBands;
          if ((hoverBands > 0) != expectActions)
            throw std::runtime_error("Message hover backgrounds must follow the same clipped viewport as actions");
          for (const auto action : {Action::Reply, Action::CopyCode, Action::CopyMessage, Action::MessageMenu})
            if (std::ranges::any_of(m_hits, [&](const auto& hit) { return hit.action == action; }) != expectActions)
              throw std::runtime_error("Message hover actions must stay inside the conversation viewport");
          if (expectActions) {
            const auto copy = std::ranges::find(m_hits, Action::CopyCode, &Hit::action);
            if (copy == m_hits.end()
                || copy->width != 22
                || copy->height != 22
                || copy->id != "Fenced code with a Copy action.\n")
              throw std::runtime_error("Code Copy uses a compact glyph target with unchanged source text");
          }
        }
        if (step >= 6) {
          const RectNode* band = nullptr;
          const auto hoverColor = lerpColor(theme::background, theme::foreground, theme::lightTheme ? .035F : .04F);
          for (const auto& node : m_scene->children())
            if (node->type() == NodeType::Rect
                && static_cast<const RectNode&>(*node).style().fill == hoverColor
                && node->x() >= chatLeft()) {
              if (band)
                throw std::runtime_error("Only the pointed message may have a hover background");
              band = static_cast<const RectNode*>(node.get());
            }
          if (step == 8) {
            if (band || std::ranges::any_of(m_hits, [](const auto& hit) { return hit.action == Action::Reply; }))
              throw std::runtime_error("The gap between message groups must not activate hover styling or actions");
          } else {
            const std::size_t index = step == 6 ? 0 : step == 7 ? 1 : 2;
            const float messageTop = m_messageTops.at(m_messages[index].id);
            const auto reply = std::ranges::find(m_hits, Action::Reply, &Hit::action);
            if (!band
                || reply == m_hits.end()
                || reply->id != m_messages[index].id
                || band->y() < m_messageViewportTop
                || band->y() + band->height() > m_messageViewportBottom)
              throw std::runtime_error("Hover backgrounds and actions must follow one clipped message");
            if (index != 1 && band->y() > messageTop - (Style::spaceXxs + Style::borderWidth))
              throw std::runtime_error("Author hover backgrounds must enclose the guild tag above the header");
            if (index == 0 && band->y() + band->height() > m_messageTops.at(m_messages[1].id))
              throw std::runtime_error("The first message hover must stop before its continuation");
            if (index == 1
                && (band->y() != messageTop || band->y() + band->height() > m_messageTops.at(m_messages[2].id) - 6))
              throw std::runtime_error("Continuation hover must leave the next author's spacing clear");
            if (index == 2
                && band->y() + band->height() < messageTop
                        + (Style::spaceLg + Style::spaceXxs)
                        + std::max(22.0F, m_messageLayouts.at(m_messages[index].id).layout.height)
                        + 3.5F)
              throw std::runtime_error("Standalone message hover must leave padding below its last text line");
          }
        }
        if (step == 2)
          m_channelPanel = "pins";
        if (step < 4) {
          m_pointerX = step == 0 || step == 2 ? chatLeft() + 100 : m_window.width() - Style::spaceRelaxed;
          m_pointerY = m_messageTops.at(m_messages.front().id) + 24;
        } else {
          m_channelPanel.clear();
          m_pointerX = chatLeft() + 100;
          if (step == 4)
            m_pointerY = 0; // Let the layout settle after closing the pins panel.
          else if (step == 5)
            m_pointerY = m_messageTops.at(m_messages[0].id) - 3;
          else if (step == 6)
            m_pointerY = m_messageTops.at(m_messages[1].id) + 4;
          else if (step == 7)
            m_pointerY = m_messageTops.at(m_messages[2].id) - 6;
          else
            m_pointerY = m_messageTops.at(m_messages[2].id) - 3;
        }
        m_window.invalidate();
      }
      if (m_demo && m_demoOverlay == "join-cycle")
        demoJoinStep();
      if (m_demo && m_demoOverlay.starts_with("embed-cycle") && m_demoInteractionStep < embedReplayFrames)
        demoEmbedStep();
      if (m_demo && m_demoOverlay.starts_with("markdown-cycle") && m_demoInteractionStep < markdownReplayFrames)
        demoMarkdownStep();
      if (m_demo
          && m_demoOverlay.starts_with("media-gallery-cycle")
          && m_demoInteractionStep < mediaGalleryReplayFrames)
        demoMediaGalleryStep();
      if (m_demo
          && m_demoOverlay.starts_with("image-menu")
          && m_demoInteractionStep < (m_demoOverlay.starts_with("image-menu-cycle") ? imageMenuReplayFrames : 2))
        demoImageMenuStep();
      if (m_demo && m_demoOverlay.starts_with("inbox-cycle") && m_demoInteractionStep < inboxReplayFrames)
        demoInboxStep();
      if (m_demo && m_demoOverlay.starts_with("search-cycle") && m_demoInteractionStep < searchReplayFrames)
        demoSearchStep();
      if (m_demo && m_demoOverlay.starts_with("outgoing-cycle") && m_demoInteractionStep < outgoingReplayFrames)
        demoOutgoingStep();
      if (m_demo && m_demoOverlay == "sidebar-cycle")
        sidebarDemoFrame();
      if (m_demo && m_demoOverlay == "mentions-cycle") {
        int highlights = 0;
        const auto inspect = [&](auto&& self, const Node& node) -> void {
          if (node.type() == NodeType::Rect) {
            const auto& rect = static_cast<const RectNode&>(node);
            if (node.width() == 3 && rect.style().fill == theme::accent) {
              if (node.x() + node.width() >= chatLeft() + Style::spaceXl)
                throw std::runtime_error("Mention bars must sit clear of the avatar column");
              ++highlights;
            }
          }
          if (node.type() == NodeType::Text
              && static_cast<const TextNode&>(node).text()
                  == formatMessageClock(m_messages.front().timestamp, m_preferences.twelveHourClock)
              && node.x() > 300)
            throw std::runtime_error("Message timestamps must follow the author header instead of the right edge");
          for (const auto& child : node.children())
            self(self, *child);
        };
        inspect(inspect, *m_scene);
        const auto step = m_demoInteractionStep++;
        if (highlights != 3 - static_cast<int>(step))
          throw std::runtime_error("Mention highlights must follow direct, role and everyone updates independently");
        if (step == 0) {
          if (m_unreadTimeline.unread())
            throw std::runtime_error("Mention previews must remain highlighted after reading");
          const auto a = std::ranges::find(m_textAreas.rbegin(), m_textAreas.rend(), "9101", &TextArea::message);
          const auto b = std::ranges::find(m_textAreas, "9102", &TextArea::message);
          const auto c = std::ranges::find(m_textAreas, "9103", &TextArea::message);
          if (a == m_textAreas.rend()
              || b == m_textAreas.end()
              || c == m_textAreas.end()
              || std::abs((b->y - a->y) - (c->y - b->y)) > .1F
              || std::abs((c->y - b->y) - a->run.height) > .1F)
            throw std::runtime_error("Wrapped lines and consecutive messages must use the same line spacing");
          api::parseMessagePatch({{"mentions", nlohmann::json::array()}, {"edited_timestamp", "2026-10-17T09:42:00Z"}})
              .apply(m_messages[1]);
          m_messageLayouts.erase(m_messages[1].id);
        } else if (step == 1) {
          if (!m_messageLayouts.at("9102").layout.edited)
            throw std::runtime_error("Live edits must show an edited marker on the next frame");
          api::GuildRoleUpdate roles;
          roles.guild = m_guildId;
          roles.members[m_account.id] = {};
          m_roleDirectory.apply({false, {roles}});
        } else if (step == 2) {
          api::parseMessagePatch({{"mention_everyone", false}}).apply(m_messages[3]);
          api::parseMessagePatch({{"edited_timestamp", nullptr}}).apply(m_messages[4]);
          m_messageLayouts.erase(m_messages[4].id);
        } else {
          if (m_messageLayouts.at("9105").layout.edited)
            throw std::runtime_error("An explicit cleared edit timestamp removes the marker");
          std::clog << "Offline mention highlighting and grouping spacing replay passed\n";
        }
        m_window.invalidate();
      }
      if (m_demo && m_demoOverlay == "video-cycle")
        demoVideoStep();
      if (m_demo && m_demoOverlay == "history-cycle")
        demoHistoryStep();
      if (m_demo && m_demoOverlay == "connection-cycle")
        demoConnectionStep();
      if (m_demo && m_demoOverlay == "jump-latest-cycle") {
        const auto step = m_demoInteractionStep++;
        const auto jump = std::ranges::find(m_hits, Action::JumpLatest, &Hit::action);
        auto require = [](bool value, const char* error) {
          if (!value)
            throw std::runtime_error(error);
        };
        if (step == 0) {
          require(jump == m_hits.end(), "Latest-message view must hide the jump control");
          m_drafts[m_channelId] = "Unsent draft";
          m_messageScroll = 140;
          m_scrolling.stop(m_messageScroll);
          m_timelineAnchor.reset();
          m_demoHistoryQuery = m_history.begin(api::HistoryDirection::Older, m_messages);
        } else if (step == 1 || step == 3 || step == 5) {
          require(jump != m_hits.end(), "Scroll-back and historical windows must offer Jump to latest");
          const float right = m_window.width() - (m_showMembers ? 240 : 0);
          require(
              std::abs(jump->x + jump->width - (right - Style::spaceXl)) < 1
                  && jump->y + jump->height < m_messageViewportBottom,
              "Jump to latest must sit at the bottom right of the conversation above the composer"
          );
          const float x = jump->x + jump->width / 2;
          const float y = jump->y + jump->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
          m_window.motion(x, y);
          m_window.click(x, y, 272);
          m_window.released();
        } else if (step == 2) {
          require(
              jump == m_hits.end()
                  && m_messageScroll == 0
                  && m_scrolling.target(m_messageScroll) == 0
                  && m_messages.size() == 100
                  && m_messages.back().id == "3099"
                  && !m_history.busy(),
              "Jumping within the latest window must retain messages and avoid a history request"
          );
          api::Event event;
          event.resource = m_channelId;
          event.history = std::exchange(m_demoHistoryQuery, {});
          event.messages.push_back({"2900", m_channelId, "Maya", "Late older page.", "2026-10-17T09:41:00Z"});
          applyHistory(event);
          require(m_messages.size() == 100 && m_messages.front().id == "3000", "Jump must reject late older pages");
          m_demoHistoryQuery = m_history.beginAround("3001");
          m_loadingMessages = true;
          m_seekMessage = "3001";
          m_messageScroll = 140;
          m_scrolling.stop(m_messageScroll);
          m_timelineAnchor.reset();
        } else if (step == 4) {
          require(
              !m_loadingMessages && !m_history.busy() && m_seekMessage.empty() && m_messageScroll == 0,
              "A local jump must cancel a pending message search without retaining its loading or seek state"
          );
          api::Event event;
          event.resource = m_channelId;
          event.history = std::exchange(m_demoHistoryQuery, {});
          event.messages.push_back({"2900", m_channelId, "Maya", "Late search page.", "2026-10-17T09:41:00Z"});
          applyHistory(event);
          require(m_messages.size() == 100 && m_messages.back().id == "3099", "Jump must reject late search pages");
          event.history = m_history.beginAround("3001");
          event.messages = m_messages;
          applyHistory(event);
          m_timelineAnchor.reset();
        } else if (step == 6) {
          require(
              m_loadingMessages && m_demoHistoryQuery && m_demoHistoryQuery->direction == api::HistoryDirection::Latest,
              "Jumping from an older window must request the current tail"
          );
          api::Event event;
          event.resource = m_channelId;
          event.history = std::exchange(m_demoHistoryQuery, {});
          event.messages.push_back({"5000", m_channelId, "Maya", "The latest message.", "2026-10-17T09:41:00Z"});
          applyHistory(event);
        } else if (step == 7) {
          require(
              jump == m_hits.end()
                  && m_history.atLatest()
                  && !m_loadingMessages
                  && m_messageScroll == 0
                  && m_messages.size() == 1
                  && m_messages.back().id == "5000",
              "Latest history must replace the older window and hide the jump control"
          );
          std::clog << "Offline Jump to latest replay passed\n";
        }
        require(m_drafts[m_channelId] == "Unsent draft", "Jump to latest must preserve the composer draft");
        m_window.invalidate();
      }
      if (m_demo
          && (m_demoOverlay == "role-style-cycle"
              || m_demoOverlay == "role-members-cycle"
              || m_demoOverlay == "role-emoji-cycle"))
        demoRoleStep();
      if (m_demo && m_demoOverlay.starts_with("dm-profile-cycle") && m_demoInteractionStep < dmProfileReplayFrames)
        demoDmProfileStep();
      if (m_demo && m_demoOverlay.starts_with("cosmetics-cycle") && m_demoInteractionStep < cosmeticsReplayFrames)
        demoCosmeticsStep();
      if (m_demo && m_demoOverlay == "profile-card-cycle")
        demoProfileStep();
      if (m_demo && m_demoOverlay == "channel-cycle")
        demoChannelStep();
      if (m_demo && m_demoOverlay == "forum-cycle")
        demoForumStep();
      if (m_demo && m_demoOverlay == "thread-cycle")
        demoThreadStep();
      if (m_demo && m_demoOverlay == "guild-tag-cycle")
        demoGuildTagStep();
      if (m_demo && m_demoOverlay == "emoji-fit") {
        int images = 0;
        auto inspect = [&](auto&& self, const Node& node) -> void {
          if (node.type() == NodeType::Image) {
            const auto& picture = static_cast<const ImageNode&>(node);
            const auto wide = m_images.at(api::emojiPath("1407464763293831269", false)).texture.id;
            const auto tall = m_images.at(api::emojiPath("123", true)).texture.id;
            if (picture.textureId() == wide || picture.textureId() == tall) {
              if (picture.fitMode() != ImageFitMode::Contain || picture.textureWidth() == picture.textureHeight())
                throw std::runtime_error("Non-square custom emoji must retain full image bounds");
              ++images;
            }
          }
          for (const auto& child : node.children())
            self(self, *child);
        };
        inspect(inspect, *m_scene);
        if (images < 9)
          throw std::runtime_error("Emoji-fit preview must cover replies, inline/jumbo messages and reactions");
        std::clog << "Offline emoji-fit preview passed\n";
      }
      if (m_demo && m_demoOverlay == "unread-badge-cycle") {
        const auto step = m_demoInteractionStep++;
        const auto at = std::ranges::find(m_hits, Action::JumpChannelMention, &Hit::action);
        if (step == 0 || step == 2) {
          if (at == m_hits.end() || at->id != "1200")
            throw std::runtime_error("Offscreen mentioned channel needs a floating badge");
          const auto hit = *at;
          if (step == 0)
            activate(hit);
          else
            m_unreadTimeline.acknowledged("1200", "9000");
        } else if (step == 1) {
          if (at != m_hits.end() || m_unreadTimeline.mentions("1200") != 2 || m_channelId != "101")
            throw std::runtime_error("Revealing a channel hides its floating badge without reading/selecting it");
          m_scrolling.stop(m_channelScroll);
          m_channelScroll = 0;
        } else if (step == 3) {
          if (at != m_hits.end())
            throw std::runtime_error("Read mentions must not keep a floating badge");
          std::clog << "Offline unread badge replay passed\n";
        }
        m_window.invalidate();
      }
      if (m_demo && m_demoOverlay == "interaction-cycle")
        demoInteractionStep();
      if (m_demo && m_demoOverlay == "sticker-cycle")
        demoStickerStep();
      if (m_demo && m_demoOverlay == "gif-cycle")
        demoGifStep();
      if (m_demo && m_demoOverlay == "unread-cycle") {
        const auto step = m_demoInteractionStep++;
        const auto first = m_unreadTimeline.first(m_messages);
        const bool banner = std::ranges::any_of(m_hits, [](const auto& hit) { return hit.action == Action::MarkRead; });
        const float chrome = m_window.titlebarHeight() - layout::Window::titlebarHeight;
        auto clickAction = [&](Action action) {
          const auto at = std::ranges::find(m_hits, action, &Hit::action);
          if (at == m_hits.end())
            throw std::runtime_error("Unread banner action is missing");
          const float x = at->x + at->width / 2, y = at->y + at->height / 2 + chrome;
          m_window.motion(x, y);
          m_window.click(x, y, 272);
          m_window.released();
        };
        auto newMessage = [&](const std::string& id) {
          const auto previous = m_messages.back().id;
          m_messages.push_back({id, m_channelId, "Maya", "Another new message.", "2026-10-17T11:19:00Z"});
          m_unreadTimeline.snapshot({{m_channelId, {previous, id}}});
          m_scrolling.stop(m_messageScroll);
          m_messageScroll = m_messageMaxScroll / 2;
        };
        if (step == 0) {
          if (!first
              || !banner
              || m_messageTops.at(m_messages[*first].id) < m_messageViewportTop
              || m_messageTops.at(m_messages[*first].id) > m_messageViewportTop + 80)
            throw std::runtime_error("Opening unread history must show its boundary and top banner");
          const auto* bar = static_cast<Scrollbar*>(m_widgets.at("chat-scrollbar"));
          const auto* thumb = bar->children()[1].get();
          if (!bar->visible())
            throw std::runtime_error("Overflowing chat must have a native scrollbar");
          const float x = bar->x() + bar->width() / 2;
          const float y = bar->y() + thumb->y() + thumb->height() / 2 + chrome;
          m_window.motion(x, y);
          m_window.click(x, y, 272);
          m_window.motion(x, y + 100);
        } else if (step == 1) {
          if (!banner || m_messageScroll >= m_messageMaxScroll - 300)
            throw std::runtime_error("Dragging the scrollbar must move the chat");
          // Continue the same pointer capture after the scene has been rebuilt.
          m_window.motion(m_pointerX, m_window.height() * 3);
          m_window.released();
        } else if (step == 2) {
          if (first || banner || m_messageScroll > 1)
            throw std::runtime_error("Reaching the newest message must clear both unread indicators");
          m_window.motion(450, 300 + chrome);
          m_window.scroll(-120);
        } else if (step == 3) {
          if (first || banner)
            throw std::runtime_error("Scrolling back up must not resurrect read indicators");
          newMessage("1030");
        } else if (step == 4) {
          if (!first || *first != m_messages.size() - 1 || !banner)
            throw std::runtime_error("A later unread batch must start at its own boundary");
          clickAction(Action::MarkRead);
        } else if (step == 5) {
          if (first || banner)
            throw std::runtime_error("Mark as Read must clear the divider and banner while scrolled up");
          m_unreadTimeline.snapshot({{m_channelId, {"1029", "1030"}}});
          if (m_unreadTimeline.unread())
            throw std::runtime_error("A delayed Gateway snapshot must not undo a local read");
          newMessage("1031");
        } else if (step == 6) {
          if (!first || !banner)
            throw std::runtime_error("New arrivals after an acknowledgement must remain visible");
          clickAction(Action::JumpUnread);
        } else if (step == 7) {
          if (first || banner)
            throw std::runtime_error("Jumping to the newest unread message must catch up");
          m_unreadTimeline.snapshot({{m_channelId, {"1001", "1031"}}}, m_channelId);
          cancelAutomaticRead(m_channelId);
          m_localRead.erase(m_channelId);
          m_messageScroll = m_messageMaxScroll / 2;
        } else if (step == 8) {
          if (!first || !banner)
            throw std::runtime_error("An explicit manual rewind must restore unread indicators");
          m_unreadTimeline.snapshot({});
        } else if (first || banner)
          throw std::runtime_error("Another device reading the channel must clear both indicators");
        std::clog << "Offline unread replay step " << step + 1 << " passed\n";
        m_window.invalidate();
      }
      if (m_demo
          && (m_demoOverlay == "scroll-cycle"
              || m_demoOverlay == "emoji-scroll-cycle"
              || m_demoOverlay == "gif-scroll-cycle")) {
        const auto step = m_demoInteractionStep++ % 16;
        const bool emoji = m_demoOverlay == "emoji-scroll-cycle", gif = m_demoOverlay == "gif-scroll-cycle";
        auto& offset = emoji ? m_emojiScroll : gif ? m_gifScroll : m_messageScroll;
        if (step == 0 || step == 4 || step == 8 || step == 12) {
          const float x = emoji ? m_emojiBounds.x + 100 : gif ? m_gifBounds.x + 100 : 450;
          const float y = emoji ? m_emojiBounds.y + 200 : gif ? m_gifBounds.y + 200 : 200;
          m_window.motion(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight);
          const float amount = step == 8 ? -48 : 120;
          m_window.scroll(emoji || gif ? amount : -amount);
          if (step == 8 && m_scrolling.target(offset) >= offset)
            throw std::runtime_error("Scroll reversal must change direction immediately");
        }
        if (emoji) {
          const float top = m_emojiBounds.y + 104, bottom = m_emojiBounds.y + m_emojiBounds.height - 54;
          for (const auto& hit : m_hits)
            if ((hit.action == Action::PickEmoji || hit.action == Action::FavoriteEmoji)
                && (hit.y < top || hit.y + hit.height > bottom + 0.01F))
              throw std::runtime_error("Emoji hit regions must be clipped to the scrolling grid");
        }
        std::clog
            << "Offline "
            << m_demoOverlay
            << " frame "
            << m_demoInteractionStep
            << " offset "
            << offset
            << " draw_ms "
            << std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - drawStarted).count()
            << '\n';
        m_window.invalidate();
      }
    };
    m_controlInput.setCursorShapeCallback([this](std::uint32_t, std::uint32_t shape) { m_controlCursor = shape; });
    m_window.motion = [this](float x, float y) {
      m_pointerX = x;
      y += layout::Window::titlebarHeight - m_window.titlebarHeight();
      m_pointerY = y;
      if (railDragMotion()) {
        updateCursor();
        m_window.invalidate();
        return;
      }
      if (m_resizingSidebar) {
        if (!sidebarResizeAvailable())
          finishSidebarResize();
        else {
          m_preferences.channelSidebarWidth =
              layout::sidebarWidth(x - layout::Sidebar::railWidth - m_sidebarDragOffset, m_window.width());
          updateCursor();
          m_window.invalidate();
          return;
        }
      }
      videoMotion(x, y);
      const auto inboxBar = m_widgets.find("inbox-scrollbar");
      const bool inboxScrollbar = inboxBar != m_widgets.end()
          && inboxBar->second->parent()
          && Hit{inboxBar->second->x(), inboxBar->second->y(), inboxBar->second->width(), inboxBar->second->height()}
                 .contains(x, y);
      const bool inboxCovered =
          m_inboxOpen && m_inboxBounds.contains(x, y) && !m_controlInput.pointerCaptured() && !inboxScrollbar;
      if (inboxCovered
          || ((!m_viewImage.empty() || m_videoExpanded) && (!m_contextOpen || !m_contextBounds.contains(x, y))))
        m_controlInput.pointerLeave();
      else
        m_controlInput.pointerMotion(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 0);
      contextMotion(x, y);
      if (m_dragImage) {
        m_viewPanX += x - m_imageDragX;
        m_viewPanY += y - m_imageDragY;
        m_imageDragX = x;
        m_imageDragY = y;
      }
      updateCursor();
      m_window.invalidate();
      if (m_dragDraft)
        positionCaret(x, y, true);
      if (m_dragMessage)
        positionChatSelection(x, y);
    };
    m_window.released = [this] {
      finishRailDrag();
      finishSidebarResize();
      videoRelease();
      m_controlInput.pointerButton(
          m_pointerX, m_pointerY + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, false, 0, 0, false
      );
      if ((!m_viewImage.empty() || m_videoExpanded) && !m_contextOpen)
        m_controlInput.pointerLeave();
      m_dragDraft = m_dragMessage = m_dragImage = false;
      updateCursor();
    };
    m_window.pointerLeft = [this] {
      finishRailDrag(true);
      finishSidebarResize();
      m_controlInput.pointerLeave();
      m_pointerX = m_pointerY = -1000;
      updateChatPreload();
      updateCursor();
      m_window.invalidate();
    };
    m_window.click = [this](float x, float y, std::uint32_t) {
      y += layout::Window::titlebarHeight - m_window.titlebarHeight();
      m_pointerX = x;
      m_pointerY = y;
      if (!(m_inboxOpen && m_inboxBounds.contains(x, y)) && sidebarResizeAt(x, y)) {
        m_controlInput.cancelPointerCapture();
        m_dragDraft = m_dragMessage = m_dragImage = false;
        m_scrolling.stopAll();
        m_resizingSidebar = true;
        m_sidebarDragStart = m_preferences.channelSidebarWidth;
        m_sidebarDragOffset = x - chatLeft();
        updateCursor();
        m_window.invalidate();
        return;
      }
      if (selectDropdownClick(x, y))
        return;
      if (userAccountDialogClick(x, y))
        return;
      if (overlayClick(x, y)) {
        return;
      }
      if (inboxClick(x, y))
        return;
      if (searchMenuClick(x, y))
        return;
      if (completionClick(x, y))
        return;
      if (fontPickerClick(x, y))
        return;
      if (m_emojiMenu
          && m_emojiBounds.contains(x, y)
          && y >= m_emojiBounds.y + 52
          && y < m_emojiBounds.y + 90
          && m_controlInput.pointerButton(
              x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
          )) {
        m_window.invalidate();
        return;
      }
      if (m_gifMenu
          && m_gifBounds.contains(x, y)
          && y >= m_gifBounds.y + 52
          && y < m_gifBounds.y + 90
          && m_controlInput.pointerButton(
              x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
          )) {
        m_window.invalidate();
        return;
      }
      if (m_viewImage.empty()
          && !m_videoExpanded
          && !m_presenceMenu
          && !m_emojiMenu
          && !m_gifMenu
          && m_controlInput.pointerButton(
              x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
          )) {
        m_window.invalidate();
        return;
      }
      m_controlInput.setFocus(nullptr);
      m_focusedInput.clear();
      for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it) {
        if (it->contains(x, y)) {
          const auto hit = *it;
          if (beginRailDrag(hit))
            return;
          activate(hit);
          return;
        }
      }
      m_fieldFocused = false;
      m_selectAll = false;
      m_window.invalidate();
    };
    m_window.contextClick = [this](float x, float y) {
      finishRailDrag(true);
      if (m_videoExpanded)
        return;
      y += layout::Window::titlebarHeight - m_window.titlebarHeight();
      m_pointerX = x;
      m_pointerY = y;
      finishSidebarResize();
      m_dragDraft = m_dragMessage = false;
      if (m_inboxOpen && m_inboxBounds.contains(x, y))
        return;
      closeInbox();
      openContext(x, y);
    };
    m_window.key = [this](const Key& key) { onKey(key); };
    m_window.focusChanged = [this](bool focused) { windowFocusChanged(focused); };
    m_window.animate = [this] {
      const bool active = m_scrolling.tick();
      if (m_preferences.reducedMotion || settingsVisible() || (!m_demo && !m_window.active()))
        m_folderMotion.finish();
      const bool railDrag = tickRailDrag();
      const bool folders = m_folderMotion.tick(folderMotionTime());
      if (m_showMembers && !forumView() && !m_dmMode && !m_demo && !m_guildId.empty() && !m_channelId.empty()) {
        const int page = layout::MemberListLayout(m_members, m_memberCount).indexAt(m_memberScroll) / 100 * 100;
        if (page != m_memberPage) {
          m_memberPage = page;
          m_client.subscribe(m_guildId, m_channelId, page);
        }
      }
      return active
          || railDrag
          || folders
          || (m_window.active() && (m_roleMotion.active() || (m_typingVisible && !m_preferences.reducedMotion)));
    };
    m_window.zoom = [this](float delta) {
      finishRailDrag(true);
      closeSelectDropdown();
      finishSidebarResize();
      if (!m_viewImage.empty()) {
        m_viewZoom = std::clamp(m_viewZoom * (delta < 0 ? 1.1F : 1 / 1.1F), 0.25F, 4.0F);
        m_window.invalidate();
        return;
      }
      m_window.setUiScale(m_window.uiScale() * (delta < 0 ? 1.1F : 1 / 1.1F));
      saveSelection();
    };
    m_window.pasted = [this](std::string bytes, std::string mime) {
      if (m_pasteServerScope != serverSettingsScope())
        return;
      if (m_pasteConnected != m_connected || m_pasteSession != m_session || m_pasteChannel != m_channelId)
        return;
      if (!m_pasteInput.empty()) {
        auto id = std::exchange(m_pasteInput, {});
        if (id == "message-edit"
            && (!m_messageEdit || m_messageEdit->generation != m_pasteMessageEditGeneration || m_messageEdit->request))
          return;
        if ((id == "dialog"
             || id == "forum-body"
             || id == "channel-name"
             || id == "channel-topic"
             || id == "channel-access-filter"
             || id.starts_with("application-field:")
             || id.starts_with("user-account:"))
            && m_pasteDialogGeneration != m_dialogGeneration)
          return;
        if (id == m_focusedInput && mime.starts_with("text/plain")) {
          pasteControl(id, std::move(bytes));
          return;
        }
        if (id != "composer")
          return;
      }
      if (mime == "error") {
        m_status = "Clipboard transfer failed or exceeded 20 MiB.";
        m_window.invalidate();
        return;
      }
      if (mime.starts_with("text/plain")) {
        insert(std::move(bytes));
        return;
      }
      if (!m_connected || m_demo || m_channelId.empty() || !m_fieldFocused)
        return;
      api::Upload upload{
          "pasted-image-"
              + std::to_string(++m_attachmentSerial)
              + (mime == "image/png"        ? ".png"
                     : mime == "image/jpeg" ? ".jpg"
                                            : ".webp"),
          mime,
          {bytes.begin(), bytes.end()}
      };
      std::vector<api::Upload> uploads;
      for (const auto& image : m_attachments[m_channelId])
        uploads.push_back(image.upload);
      uploads.push_back(upload);
      if (!api::validUploads(uploads)) {
        m_status = "Attachments exceed the 20 MiB draft limit.";
        m_window.invalidate();
        return;
      }
      auto texture = m_window.context().textureManager().loadFromEncodedBytes(upload.bytes.data(), upload.bytes.size());
      if (!texture.valid()) {
        m_status = "Could not decode the pasted image.";
        m_window.invalidate();
        return;
      }
      m_attachments[m_channelId].push_back({std::move(upload), texture, std::to_string(m_attachmentSerial)});
      m_attachmentScroll = std::numeric_limits<float>::max();
      m_window.invalidate();
    };
    m_window.scroll = [this](float delta) {
      if (m_videoExpanded)
        return;
      auto scroll = [&](float& offset, float amount) {
        const bool precise = m_window.preciseScroll();
        m_scrolling.scroll(
            offset, ScrollMotion::distance(amount, precise), precise || !m_animateWheel || m_preferences.reducedMotion
        );
        if (m_traceScroll)
          std::clog
              << "scroll input surface="
              << (&offset == &m_messageScroll     ? "chat"
                      : &offset == &m_gifScroll   ? "gif"
                      : &offset == &m_emojiScroll ? "emoji"
                                                  : "other")
              << " delta="
              << amount
              << " offset="
              << offset
              << " target="
              << m_scrolling.target(offset)
              << " precise="
              << precise
              << " animation="
              << m_animateWheel
              << '\n';
      };

      if (m_inboxOpen && m_inboxBounds.contains(m_pointerX, m_pointerY) && m_dialogKind.empty() && !settingsVisible()) {
        scroll(m_inboxScroll, delta);
        m_window.invalidate();
        return;
      }
      if (!m_searchInput.empty() && m_searchBounds.contains(m_pointerX, m_pointerY)) {
        scroll(m_searchScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_selectRequest) {
        if (m_selectBounds.contains(m_pointerX, m_pointerY)) {
          scroll(m_selectScroll, delta);
          m_window.invalidate();
          return;
        }
        closeSelectDropdown();
      }
      if (userAccountDialogOpen()) {
        userAccountDialogScroll(delta);
        return;
      }
      if (m_serverSettings.open) {
        scroll(m_pointerX < m_serverSettings.navRight ? m_serverSettings.navScroll : m_serverSettings.scroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_completionQuery && m_completionBounds.contains(m_pointerX, m_pointerY)) {
        scroll(m_completionScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_emojiMenu) {
        if (m_emojiBounds.contains(m_pointerX, m_pointerY))
          scroll(m_pointerX < m_emojiBounds.x + 56 ? m_emojiRailScroll : m_emojiScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_gifMenu) {
        if (m_gifBounds.contains(m_pointerX, m_pointerY))
          scroll(m_gifScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_contextOpen) {
        if ((!m_reactionSubmenu || !m_reactionSubmenuBounds.contains(m_pointerX, m_pointerY))
            && m_contextBounds.contains(m_pointerX, m_pointerY)
            && m_contextFullHeight > m_contextBounds.height) {
          m_reactionSubmenu = false;
          scroll(m_contextScroll, delta);
          m_window.invalidate();
        } else
          scrollContext(delta);
        return;
      }
      if (m_dialogKind == "dm-create") {
        if (m_dmCreateBounds.contains(m_pointerX, m_pointerY))
          scroll(m_dmCreateScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind.starts_with("server-")) {
        if (m_dialogBounds.contains(m_pointerX, m_pointerY))
          scroll(m_serverDialogScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "threads") {
        if (m_dialogBounds.contains(m_pointerX, m_pointerY))
          scroll(m_threadScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "sidebar-create"
          && m_dialogAction
          && m_dialogAction->kind == api::InteractionKind::CreateChannel) {
        if (m_dialogBounds.contains(m_pointerX, m_pointerY))
          scroll(m_channelCreateScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "sidebar-invite") {
        if (m_channelInvite.editing && m_channelInvite.formBounds.contains(m_pointerX, m_pointerY))
          scroll(m_channelInvite.formScroll, delta);
        else if (!m_channelInvite.editing && m_channelInvite.listBounds.contains(m_pointerX, m_pointerY))
          scroll(m_channelInvite.scroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "channel-info"
          || m_dialogKind == "channel-access"
          || m_dialogKind == "channel-edit"
          || m_dialogKind == "application-modal") {
        if (m_dialogBounds.contains(m_pointerX, m_pointerY))
          scroll(m_channelDialogScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "profile") {
        if (m_dialogBounds.contains(m_pointerX, m_pointerY))
          scroll(m_profileScroll, delta);
        m_window.invalidate();
        return;
      }
      if (!m_dialogKind.empty())
        return;
      if (m_settingsOpen) {
        if (m_fontPickerOpen) {
          if (m_fontPickerBounds.contains(m_pointerX, m_pointerY))
            scroll(m_fontPickerScroll, delta);
          m_window.invalidate();
          return;
        }
        scroll(m_pointerX < m_settingsNavRight ? m_settingsNavScroll : m_settingsScroll, delta);
        m_window.invalidate();
        return;
      }
      if (!m_viewImage.empty()) {
        m_viewZoom = std::clamp(m_viewZoom * (delta < 0 ? 1.15F : 1 / 1.15F), 0.25F, 4.0F);
        m_window.invalidate();
        return;
      }
      if (!m_connected)
        return;
      if (!m_attachments[m_channelId].empty()
          && m_pointerX >= m_attachmentX
          && m_pointerX < m_attachmentX + m_attachmentWidth
          && m_pointerY >= m_attachmentY
          && m_pointerY < m_attachmentY + layout::DraftAttachment::height + Style::spaceXs)
        scroll(m_attachmentScroll, delta);
      else if (m_pointerX < layout::Sidebar::railWidth)
        scroll(m_guildScroll, delta);
      else if (m_pointerX < chatLeft())
        scroll(m_channelScroll, delta);
      else if (m_dmMode && !m_dmPage.empty())
        scroll(m_dmPreviewBounds.contains(m_pointerX, m_pointerY) ? m_dmPreviewScroll : m_dmHomeScroll, delta);
      else if (!m_channelPanel.empty() && m_pointerX >= m_window.width() - channelPanelWidth())
        scroll(m_channelPanelScroll, delta);
      else if (forumView())
        scroll(m_forumScroll, delta);
      else if (memberPaneWidth() > 0 && m_pointerX >= m_window.width() - memberPaneWidth()) {
        scroll(dmRecipient() ? m_dmProfile.scroll : m_memberScroll, delta);
      } else
        scroll(m_messageScroll, -delta);
      m_window.invalidate();
    };
    if (demoMode)
      demo();
  }

  int ClientApplication::run(bool smokeTest, bool startMinimized) {
    struct ReplayInstance {
      SingleInstance*& target;
      std::filesystem::path runtime;
      std::unique_ptr<SingleInstance> server;
      ~ReplayInstance() {
        if (server)
          target = nullptr;
        server.reset();
        if (!runtime.empty()) {
          std::error_code ignored;
          std::filesystem::remove_all(runtime, ignored);
        }
      }
    } replayInstance{m_instance};
    if (ContextMenuControl::compiledEntrySize() != sizeof(ContextMenuControlEntry))
      throw std::runtime_error(
          "The renderer library is stale: its menu layout differs from the client. Run just rebuild."
      );
    if (!m_demo)
      m_preferences = m_sessionStore.startupPreferences();
    m_themeSnapshot =
        m_themes.start(paths::assetPath("themes"), m_demo ? std::filesystem::path{} : userThemeDirectory(), !m_demo);
    m_window.initialize();
    if (!m_demo) {
      m_window.closeRequested = [this] { closeToTray(); };
      m_tray.start(paths::assetPath("cressida-tray.svg"));
    }
    const auto* view = cressida::environment("CRESSIDA_DEMO_VIEW", "DISCORD_CPP_DEMO_VIEW");
    if (m_demo && view && std::string_view(view) == "tray-cycle") {
      char pattern[] = "/tmp/cressida-instance-replay-XXXXXX";
      const char* runtime = ::mkdtemp(pattern);
      if (!runtime)
        throw std::runtime_error("Could not create the private instance replay directory.");
      replayInstance.runtime = runtime;
      replayInstance.server = std::make_unique<SingleInstance>(replayInstance.runtime);
      if (replayInstance.server->claim() != SingleInstance::Launch::Owner)
        throw std::runtime_error("The private instance replay must own its isolated socket.");
      m_instance = replayInstance.server.get();
    }
    const bool trayStartupDemo = m_demo && view && std::string_view(view).starts_with("tray-startup-");
    if ((!m_demo && startMinimized) || trayStartupDemo) {
      beginTrayStartup();
      if (trayStartupDemo) {
        m_demoOverlay = view;
        demoTrayStartup(false);
      }
    }
    if (auto snapshot = m_themes.drain())
      m_themeSnapshot = std::move(*snapshot);
    if (!m_demo) {
      if (!m_token.empty())
        connect();
      else
        restoreLogin();
    }
    const bool interactionCycle = m_demo
        && view
        && (std::string_view(view) == "interaction-cycle"
            || std::string_view(view) == "animation-cycle"
            || std::string_view(view) == "gif-cycle"
            || std::string_view(view) == "sticker-cycle"
            || std::string_view(view) == "scroll-cycle"
            || std::string_view(view) == "emoji-scroll-cycle"
            || std::string_view(view) == "gif-scroll-cycle"
            || std::string_view(view) == "history-cycle"
            || std::string_view(view) == "video-cycle");
    if (!m_demo)
      m_desktopNotifications.start();
    m_window.run(
        m_client.eventFd(), [this] { tick(); },
        smokeTest
            ? (m_demo && view && std::string_view(view).starts_with("inbox-cycle")            ? inboxReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("search-cycle")     ? searchReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("outgoing-cycle")   ? outgoingReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("chat-links-cycle") ? chatLinkReplayFrames
                   : m_demo && view && std::string_view(view) == "attachment-cycle"           ? attachmentReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("window-controls-cycle")
                   ? windowControlsReplayFrames
                   : m_demo && view && std::string_view(view) == "tray-cycle"               ? trayReplayFrames
                   : m_demo && view && std::string_view(view) == "sticker-cycle"            ? 8
                   : m_demo && view && std::string_view(view) == "dm-compose-cycle"         ? 16
                   : m_demo && view && std::string_view(view).starts_with("dm-home-cycle")  ? dmHomeReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("reply-cycle")    ? composerReplyReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("embed-cycle")    ? embedReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("markdown-cycle") ? markdownReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("reply-grouping") ? replyGroupingReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("media-gallery-cycle")
                   ? mediaGalleryReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("image-menu-cycle")  ? imageMenuReplayFrames
                   : m_demo && view && std::string_view(view) == "chat-preload-cycle"          ? 10
                   : m_demo && view && std::string_view(view) == "join-cycle"                  ? 4
                   : m_demo && view && std::string_view(view) == "deleted-cycle"               ? 3
                   : m_demo && view && std::string_view(view) == "inline-edit-cycle"           ? 8
                   : m_demo && view && std::string_view(view) == "autocomplete-cycle"          ? 9
                   : m_demo && view && std::string_view(view).starts_with("decorations-cycle") ? 4
                   : m_demo && view && std::string_view(view).starts_with("reduced-motion-cycle")
                   ? reducedMotionReplayFrames
                   : m_demo && view && std::string_view(view) == "user-preferences-cycle"            ? 12
                   : m_demo && view && std::string_view(view) == "user-account-cycle"                ? 16
                   : m_demo && view && std::string_view(view) == "user-settings-lists-cycle"         ? 30
                   : m_demo && view && std::string_view(view) == "settings-font-cycle"               ? 12
                   : m_demo && view && std::string_view(view) == "server-hover-cycle"                ? 7
                   : m_demo && view && std::string_view(view) == "themes-cycle"                      ? 11
                   : m_demo && view && std::string_view(view).starts_with("message-hover-cycle")     ? 10
                   : m_demo && view && std::string_view(view) == "sidebar-cycle"                     ? 10
                   : m_demo && view && std::string_view(view) == "role-emoji-cycle"                  ? 3
                   : m_demo && view && std::string_view(view) == "role-members-cycle"                ? 2
                   : m_demo && view && std::string_view(view) == "mentions-cycle"                    ? 4
                   : m_demo && view && std::string_view(view) == "channel-menu-scroll-cycle"         ? 3
                   : m_demo && view && std::string_view(view) == "channel-create-cycle"              ? 12
                   : m_demo && view && std::string_view(view) == "channel-invite-cycle"              ? 18
                   : m_demo && view && std::string_view(view) == "server-settings-commands-cycle"    ? 12
                   : m_demo && view && std::string_view(view) == "server-settings-expressions-cycle" ? 26
                   : m_demo && view && std::string_view(view) == "server-settings-management-cycle"  ? 12
                   : m_demo && view && std::string_view(view) == "server-settings-cycle"             ? 42
                   : m_demo && view && std::string_view(view).starts_with("floating-cycle")   ? floatingReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("typing-cycle")     ? typingReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("rail-drag")        ? 10
                   : m_demo && view && std::string_view(view).starts_with("folder-animation") ? 9
                   : m_demo && view && std::string_view(view) == "folder-menu-cycle"          ? 13
                   : m_demo && view && std::string_view(view) == "server-menu-cycle"          ? 20
                   : m_demo && view && std::string_view(view) == "channel-menu-cycle"         ? 31
                   : m_demo && view && std::string_view(view) == "channel-cycle"              ? 18
                   : m_demo && view && std::string_view(view) == "forum-cycle"                ? 12
                   : m_demo && view && std::string_view(view) == "thread-cycle"               ? 10
                   : m_demo && view && std::string_view(view) == "guild-tag-cycle"            ? 10
                   : m_demo && view && std::string_view(view).starts_with("cosmetics-cycle")  ? cosmeticsReplayFrames
                   : m_demo && view && std::string_view(view).starts_with("dm-profile-cycle") ? dmProfileReplayFrames
                   : m_demo && view && std::string_view(view) == "profile-card-cycle"         ? 8
                   : m_demo && view && std::string_view(view) == "role-style-cycle"           ? 7
                   : m_demo && view && std::string_view(view) == "unread-badge-cycle"         ? 4
                   : m_demo && view && std::string_view(view) == "unread-cycle"               ? 10
                   : m_demo && view && std::string_view(view) == "jump-latest-cycle"          ? 8
                   : m_demo && view && std::string_view(view) == "connection-cycle"           ? 8
                   : interactionCycle ? (std::string_view(view) == "video-cycle" ? 512 : 64)
                                      : 1)
            : 0,
        m_assets.eventFd(), m_video.eventFd(), m_themes.eventFd(), m_filePicker.eventFd(), m_soundPreview.eventFd(),
        m_desktopNotifications.eventFd(), m_tray.eventFd(), m_windowIcons.eventFd(),
        m_instance ? m_instance->eventFd() : -1
    );
    if (m_demoOverlay == "server-settings-commands-cycle" && m_demoInteractionStep != 12)
      throw std::runtime_error("Server command permissions replay did not finish");
    if (m_demoOverlay == "server-settings-expressions-cycle" && m_demoInteractionStep != 26)
      throw std::runtime_error("Server expressions replay did not finish");
    if (m_demoOverlay == "server-settings-management-cycle" && m_demoInteractionStep != 12)
      throw std::runtime_error("Server management replay did not finish");
    if (m_demoOverlay == "server-settings-cycle" && m_demoInteractionStep != 42)
      throw std::runtime_error("Server settings replay did not finish");
    if (m_demoOverlay == "deleted-cycle" && m_demoInteractionStep != 3)
      throw std::runtime_error("Deleted-message replay did not finish");
    if (m_demoOverlay == "inline-edit-cycle" && m_demoInteractionStep != 8)
      throw std::runtime_error("Inline message editing replay did not finish");
    if (m_demoOverlay == "autocomplete-cycle" && m_demoInteractionStep != 9)
      throw std::runtime_error("Autocomplete replay did not finish");
    if (m_demoOverlay == "settings-font-cycle" && m_demoInteractionStep != 12)
      throw std::runtime_error("Font settings replay did not finish");
    if (m_demoOverlay == "tray-cycle" && m_demoInteractionStep != trayReplayFrames)
      throw std::runtime_error("Tray lifecycle replay did not complete");
    if (m_demoOverlay.starts_with("tray-startup-") && m_demoInteractionStep != 1)
      throw std::runtime_error("Tray startup replay did not finish");
    if (m_demoOverlay.starts_with("chat-links-cycle") && m_demoInteractionStep != chatLinkReplayFrames)
      throw std::runtime_error("Chat link replay did not finish");
    if (m_demoOverlay == "attachment-cycle" && m_demoInteractionStep != attachmentReplayFrames)
      throw std::runtime_error("Draft attachment replay did not finish");
    if (m_demoOverlay == "chat-preload-cycle" && m_demoInteractionStep != 10)
      throw std::runtime_error("Chat preload replay did not finish");
    if (m_demoOverlay == "server-hover-cycle" && m_demoInteractionStep != 7)
      throw std::runtime_error("Server hover replay did not finish");
    if (m_demoOverlay == "themes-cycle" && m_demoInteractionStep != 11)
      throw std::runtime_error("Theme settings replay did not finish");
    if (m_demoOverlay.starts_with("message-hover-cycle") && m_demoInteractionStep != 10)
      throw std::runtime_error("Message hover replay did not finish");
    if (m_demoOverlay == "join-cycle" && m_demoInteractionStep != 4)
      throw std::runtime_error("Server join replay did not complete");
    if (m_demoOverlay == "sticker-cycle" && m_demoInteractionStep != 8)
      throw std::runtime_error("Sticker replay did not complete");
    if (m_demoOverlay == "role-emoji-cycle" && m_demoInteractionStep != 3)
      throw std::runtime_error("Role emoji replay did not complete");
    if (m_demoOverlay == "role-members-cycle" && m_demoInteractionStep != 2)
      throw std::runtime_error("Member role replay did not complete");
    if (m_demoOverlay == "sidebar-cycle" && m_demoInteractionStep != 10)
      throw std::runtime_error("Cursor/sidebar replay did not complete");
    if (m_demoOverlay == "jump-latest-cycle" && m_demoInteractionStep != 8)
      throw std::runtime_error("Jump to latest replay did not complete");
    if (m_demoOverlay.starts_with("window-controls-cycle") && m_demoInteractionStep != windowControlsReplayFrames)
      throw std::runtime_error("Window controls replay did not finish");
    if (m_demoOverlay.starts_with("decorations-cycle") && m_demoInteractionStep != 4)
      throw std::runtime_error("Desktop decorations replay did not finish");
    if (m_demoOverlay.starts_with("reduced-motion-cycle") && m_demoInteractionStep != reducedMotionReplayFrames)
      throw std::runtime_error("Reduced motion media replay did not finish");
    if (m_demoOverlay == "user-preferences-cycle" && m_demoInteractionStep != 12)
      throw std::runtime_error("Client preferences replay did not finish");
    if (m_demoOverlay == "dm-compose-cycle" && m_demoInteractionStep != 16)
      throw std::runtime_error("DM compose replay did not finish");
    if (m_demoOverlay.starts_with("dm-home-cycle") && m_demoInteractionStep != dmHomeReplayFrames)
      throw std::runtime_error("Direct message home replay did not finish");
    if (m_demoOverlay.starts_with("reply-cycle") && m_demoInteractionStep != composerReplyReplayFrames)
      throw std::runtime_error("Reply composer replay did not finish");
    if (m_demoOverlay == "user-account-cycle" && m_demoInteractionStep != 16)
      throw std::runtime_error("User account replay did not finish");
    if (m_demoOverlay == "user-settings-lists-cycle" && m_demoInteractionStep != 30)
      throw std::runtime_error("User settings lists replay did not finish");
    if (m_demoOverlay == "connection-cycle" && m_demoInteractionStep != 8)
      throw std::runtime_error("Staged connection replay did not complete");
    if (m_demoOverlay == "channel-menu-scroll-cycle" && m_demoInteractionStep != 3)
      throw std::runtime_error("Compact channel menu replay did not finish");
    if (m_demoOverlay == "channel-create-cycle" && m_demoInteractionStep != 12)
      throw std::runtime_error("Channel creation replay did not finish");
    if (m_demoOverlay == "channel-invite-cycle" && m_demoInteractionStep != 18)
      throw std::runtime_error("Channel invite replay did not finish");
    if (m_demoOverlay.starts_with("floating-cycle") && m_demoInteractionStep != floatingReplayFrames)
      throw std::runtime_error("Floating layout and radius replay did not complete");
    if (m_demoOverlay.starts_with("embed-cycle") && m_demoInteractionStep != embedReplayFrames)
      throw std::runtime_error("AutoMod and embed link replay did not complete");
    if (m_demoOverlay.starts_with("markdown-cycle") && m_demoInteractionStep != markdownReplayFrames)
      throw std::runtime_error("Markdown replay did not complete");
    if (m_demoOverlay.starts_with("reply-grouping") && m_demoInteractionStep != replyGroupingReplayFrames)
      throw std::runtime_error("Grouped hover alignment replay did not complete");
    if (m_demoOverlay.starts_with("media-gallery-cycle") && m_demoInteractionStep != mediaGalleryReplayFrames)
      throw std::runtime_error("Media gallery replay did not complete");
    if (m_demoOverlay.starts_with("image-menu-cycle") && m_demoInteractionStep != imageMenuReplayFrames)
      throw std::runtime_error("Image viewer menu replay did not complete");
    if (m_demoOverlay.starts_with("inbox-cycle") && m_demoInteractionStep != inboxReplayFrames)
      throw std::runtime_error("Inbox replay did not complete");
    if (m_demoOverlay.starts_with("search-cycle") && m_demoInteractionStep != searchReplayFrames)
      throw std::runtime_error("Search menu replay did not complete");
    if (m_demoOverlay.starts_with("outgoing-cycle") && m_demoInteractionStep != outgoingReplayFrames)
      throw std::runtime_error("Outgoing message replay did not complete");
    if (m_demoOverlay.starts_with("typing-cycle") && m_demoInteractionStep != typingReplayFrames)
      throw std::runtime_error("Typing indicator replay did not finish");
    if (m_demoOverlay.starts_with("rail-drag") && m_demoInteractionStep != 10)
      throw std::runtime_error("Rail drag replay did not finish");
    if (m_demoOverlay.starts_with("folder-animation") && m_demoInteractionStep != 9)
      throw std::runtime_error("Folder animation replay did not finish");
    if (m_demoOverlay == "folder-menu-cycle" && m_demoInteractionStep != 13)
      throw std::runtime_error("Folder menu replay did not finish");
    if (m_demoOverlay == "server-menu-cycle" && m_demoInteractionStep != 20)
      throw std::runtime_error("Server menu replay did not finish");
    if (m_demoOverlay == "channel-menu-cycle" && m_demoInteractionStep != 31)
      throw std::runtime_error("Channel menu replay did not finish");
    if (m_demoOverlay == "channel-cycle" && m_demoInteractionStep != 18)
      throw std::runtime_error("Channel feature replay did not complete");
    if (m_demoOverlay == "mentions-cycle" && m_demoInteractionStep != 4)
      throw std::runtime_error("Mention highlighting replay did not complete");
    if (m_demoOverlay == "forum-cycle" && m_demoInteractionStep != 12)
      throw std::runtime_error("Forum replay did not complete");
    if (m_demoOverlay.starts_with("cosmetics-cycle") && m_demoInteractionStep != cosmeticsReplayFrames)
      throw std::runtime_error("Cosmetic replay did not complete");
    if (m_demoOverlay.starts_with("dm-profile-cycle") && m_demoInteractionStep != dmProfileReplayFrames)
      throw std::runtime_error("DM profile replay did not finish");
    if (m_demoOverlay == "thread-cycle" && m_demoInteractionStep != 10)
      throw std::runtime_error("Thread replay did not complete");
    if (m_demoOverlay == "video-cycle" && m_demoInteractionStep < 11)
      throw std::runtime_error("Native video replay did not complete");
    m_client.disconnect();
    return 0;
  }

  void ClientApplication::prepareDemo() {
    if (!m_demo || m_demoPrepared)
      return;
    m_demoPrepared = true;
    if (m_demo && m_window.context().makeCurrent(m_window.target())) {
      // Procedural offline fixture, so media previews never need a CDN in demo.
      std::vector<unsigned char> pixels(640 * 360 * 4);
      for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x) {
          const auto offset = (y * 640 + x) * 4;
          pixels[offset] = 40 + 180 * x / 640;
          pixels[offset + 1] = 50 + 150 * y / 360;
          pixels[offset + 2] = 180;
          pixels[offset + 3] = 255;
        }
      m_images["demo-preview"].texture = m_window.context().textureManager().loadFromRgba(pixels.data(), 640, 360);
      api::Message preview{"6", m_channelId, "Alex", "An image and a video preview", "2026-10-17T09:50:00Z"};
      preview.attachments.push_back(
          {"7", "preview.png", "https://example.com/preview.png", "image/png", {"demo-preview", 1280, 720}}
      );
      api::Embed video;
      video.type = "video";
      video.provider = "YouTube";
      video.title = "Offline video preview";
      video.url = "https://www.youtube.com";
      video.thumbnail = {"demo-preview", 1280, 720};
      preview.embeds.push_back(std::move(video));
      m_messages.push_back(std::move(preview));
      if (const auto* option = cressida::environment("CRESSIDA_DEMO_VIEW", "DISCORD_CPP_DEMO_VIEW")) {
        const std::string view(option);

        if (view.starts_with("inbox-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
        }
        if (view.starts_with("search-cycle")) {
          m_demoOverlay = view;
          m_channelPanel = "search";
          m_focusedInput = "channel-search";
          m_channelSearchText.clear();
          m_searchHistory.clear();
          m_messages = {{"90001", m_channelId, "Maya", "Search fixture", "2026-10-21T10:00:00Z", "11"}};
          m_channels.push_back({.id = "202", .name = "design", .type = 0});
          m_guildChannels[m_guildId] = m_channels;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.find("narrow") != std::string::npos) {
            m_window.setUiScale(1.5F);
            m_focusedInput = "channel-search-panel";
          }
        } else if (view.starts_with("outgoing-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages.clear();
          m_drafts[m_channelId].clear();
          m_showMembers = false;
          m_history.restoreLatest(m_messages, true);
        } else if (view == "tray-cycle") {
          m_demoOverlay = view;
          m_drafts[m_channelId].clear();
        } else if (view.starts_with("image-menu")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          prepareMediaGalleryDemo(2);
        } else if (view.starts_with("media-gallery")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          const int count = view.find("cycle") != std::string::npos || view.find("pair") != std::string::npos ? 2
              : view.find("ten") != std::string::npos                                                         ? 10
              : view.find("three") != std::string::npos || view.find("portrait") != std::string::npos         ? 3
                                                                                                              : 5;
          prepareMediaGalleryDemo(count, view.find("portrait") != std::string::npos);
        } else if (view.starts_with("forwarded")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view.find("compact") != std::string::npos;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          nlohmann::json attachments = nlohmann::json::array();
          const int count = view.find("single") != std::string::npos ? 1 : 4;
          for (int i = 0; i < count; ++i) {
            const auto url = "https://cdn.discordapp.com/attachments/100/" + std::to_string(700 + i) + "/image.png";
            attachments.push_back(
                {{"id", std::to_string(700 + i)},
                 {"filename", "image.png"},
                 {"url", url},
                 {"content_type", "image/png"},
                 {"width", 640},
                 {"height", 360}}
            );
            m_images[api::mediaUrl(url)].texture =
                m_window.context().textureManager().loadFromRgba(pixels.data(), 640, 360);
          }
          auto source =
              nlohmann::json{{"type", 1}, {"guild_id", m_guildId}, {"channel_id", m_channelId}, {"message_id", "800"}};
          m_messages = api::parseMessages(
              nlohmann::json::array(
                  {{{"id", "900"},
                    {"channel_id", m_channelId},
                    {"author", {{"id", "1"}, {"username", "Alex"}}},
                    {"timestamp", "2026-10-18T18:50:00Z"},
                    {"content", ""},
                    {"message_reference", source},
                    {"message_snapshots",
                     nlohmann::json::array(
                         {{{"message",
                            {{"timestamp", "2026-10-18T18:49:00Z"},
                             {"content", view.find("text") != std::string::npos ? "Saved **formatted** text." : ""},
                             {"attachments",
                              view.find("text") != std::string::npos ? nlohmann::json::array() : attachments}}}}}
                     )}}}
              )
          );
          m_guildChannels[m_guildId] = m_channels;
          api::GuildRoleUpdate owner{.guild = m_guildId};
          owner.owner = view.find("private") == std::string::npos ? m_account.id : "999";
          m_roleDirectory.apply({false, {owner}});
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messageLayouts.clear();
        } else if (view.starts_with("server-hover")) {
          m_demoOverlay = view;
          m_openFolders.clear();
          m_folders = {{{}, {}, 0, {"1", "2", "3"}}};
          m_guilds[0].features = {"COMMUNITY"};
          m_guilds[0].members = 6870;
          m_guilds[0].online = 1191;
          m_guilds[1].features = {"COMMUNITY", "PARTNERED"};
          m_guilds[1].members = 12000;
          m_guilds[1].online = 0;
          m_guilds[2].features = {"COMMUNITY", "VERIFIED"};
          m_guilds[2].members = 431;
          m_guilds[2].online = 123;
          if (view == "server-hover-cycle") {
            for (int i = 0; i < 3; ++i) {
              auto animation = std::make_shared<media::Animation>();
              animation->width = animation->height = 32;
              animation->duration = std::chrono::milliseconds(200);
              for (int n = 0; n < 2; ++n) {
                media::Frame frame;
                frame.duration = std::chrono::milliseconds(100);
                frame.rgba.resize(32 * 32 * 4);
                for (std::size_t pixel = 0; pixel < frame.rgba.size(); pixel += 4) {
                  frame.rgba[pixel] = 80 + i * 40;
                  frame.rgba[pixel + 1] = 110 + n * 70;
                  frame.rgba[pixel + 2] = 160;
                  frame.rgba[pixel + 3] = 255;
                }
                animation->frames.push_back(std::move(frame));
              }
              const auto stem = "/icons/" + m_guilds[i].id + "/a_hover";
              m_guilds[i].icon = stem + ".png?size=64";
              auto& still = m_images[m_guilds[i].icon];
              still.texture =
                  m_window.context().textureManager().loadFromRgba(animation->frames.front().rgba.data(), 32, 32);
              auto& moving = m_images[stem + ".gif?size=64"];
              moving.animation = animation;
              moving.texture =
                  m_window.context().textureManager().loadFromRgba(animation->frames.front().rgba.data(), 32, 32);
            }
          }
          m_guildChannels[m_guildId] = m_channels;
          m_unreadGuilds[m_channels.front().id] = m_guildId;
          m_unreadChannels.insert(m_channels.front().id);
          m_pointerX = layout::Sidebar::railWidth / 2;
          m_pointerY = layout::ServerRail::top
              + (view == "server-hover-partner"        ? 1
                     : view == "server-hover-verified" ? 2
                                                       : 0)
                  * layout::ServerRail::step
              + layout::Avatar::rail / 2;
          if (view == "server-hover-light")
            m_preferences.theme = "light";
          if (view == "server-hover-plain") {
            m_guilds.front().features.clear();
            m_guilds.front().members.reset();
            m_guilds.front().online.reset();
            m_unreadChannels.clear();
          }
          if (view == "server-hover-bottom") {
            for (int n = 4; n < 12; ++n) {
              m_guilds.push_back(
                  {std::to_string(n),
                   "A long server name that should stay inside the window",
                   {},
                   {"VERIFIED"},
                   1000000,
                   999999}
              );
              m_folders.front().guildIds.push_back(std::to_string(n));
            }
            m_guildScroll = 500;
            m_pointerY = 617;
          }
        }
        if (view == "sidebar-cycle")
          m_demoOverlay = view;
        if (view == "sidebar-wide")
          m_preferences.channelSidebarWidth = 420;
        if (view.starts_with("autocomplete")) {
          m_demoOverlay = view;
          m_showMembers = false;
          if (view == "autocomplete-light")
            m_preferences.theme = "light";
          m_preferences.emojiImageLinks = true;
          prepareEmojiPicker();
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9901","author":{"id":"1","username":"Alex"},"timestamp":"2026-10-18T12:00:00Z","content":"Fix @ma"},
            {"id":"9902","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-18T12:01:00Z","content":"Mention people and roles, or search emoji by name."}
          ])JSON"));
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          m_members = {{0, {"11", "Maya", {}, "maya"}}, {1, {"12", "Noah", {}, "noah"}}};
          api::GuildRoleUpdate roles{.guild = m_guildId};
          roles.roles = {{"50", 2, {0xC6B5F5}, "Moonwarden"}, {"51", 1, {0x82AAFF}, "Helpers"}};
          roles.members["11"] = {"50"};
          m_roleDirectory.apply({false, {roles}});
          if (view.starts_with("autocomplete-frequent")) {
            m_recentEmojis = {"800000000000000005"};
            m_emojiPreferences.frequent = {"800000000000000101", "800000000000000001"};
            if (view.ends_with("light"))
              m_preferences.theme = "light";
          }
          m_drafts[m_channelId] = view.starts_with("autocomplete-frequent") ? ":pro"
              : view == "autocomplete-emoji"                                ? ":umb"
              : view == "autocomplete-roles"                                ? "@moon"
                                                                            : "Hello @ma";
          if (view.starts_with("autocomplete-tokens")) {
            m_drafts[m_channelId] = "Hello <@11> <@&50> <:project_1:800000000000000001>\n"
                                    "<a:umbriel:800000000000000000> 🙂";
            if (view.ends_with("light"))
              m_preferences.theme = "light";
          }
          m_focusedInput = "composer";
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          if (view == "autocomplete-edit")
            beginMessageEdit("9901");
        }
        if (view == "inline-edit"
            || view == "inline-edit-light"
            || view == "inline-edit-compact"
            || view == "inline-edit-short"
            || view == "inline-edit-cycle") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view == "inline-edit-compact";
          m_preferences.logEditHistory = true;
          if (view == "inline-edit-light")
            m_preferences.theme = "light";
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9801","author":{"id":"1","username":"Alex"},"timestamp":"2026-10-18T12:00:00Z","content":"An earlier message from me."},
            {"id":"9802","author":{"id":"12","username":"Maya"},"timestamp":"2026-10-18T12:01:00Z","content":"You can edit that message in the conversation."},
            {"id":"9803","author":{"id":"1","username":"Alex"},"timestamp":"2026-10-18T12:02:00Z","content":"The latest message from me.\nThis second line remains editable."}
          ])JSON"));
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          if (view == "inline-edit-short")
            m_messages.back().content = "Two";
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          if (view == "inline-edit-cycle") {
            auto system = m_messages.back();
            system.id = "9804";
            system.type = 7;
            system.content.clear();
            m_messages.push_back(system);
            system.id = "9805";
            system.type = 0;
            system.deleted = true;
            system.content = "Deleted messages cannot be edited.";
            m_messages.push_back(std::move(system));
            m_focusedInput = "composer";
          } else
            beginMessageEdit("9803");
        }
        if (view == "syntax"
            || view == "syntax-light"
            || view == "syntax-compact"
            || view == "syntax-nix"
            || view == "syntax-nix-light"
            || view == "syntax-nix-compact"
            || view.starts_with("syntax-nix-flat-rail")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view.ends_with("-compact");
          if (view.ends_with("-light"))
            m_preferences.theme = "light";
          if (view.starts_with("syntax-nix-flat-rail")) {
            auto& palette = m_themeSnapshot.themes.front();
            palette.dark.rail = palette.dark.background;
            palette.light.rail = palette.light.background;
          }
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9701","author":{"id":"11","username":"Lysec"},"timestamp":"2026-10-18T11:26:00Z","content":"```cpp\n// Keep the original source when copying.\nconstexpr int count = 42;\nif (count > 0) {\n  print(\"Hello, world!\");\n}\n```"},
            {"id":"9702","author":{"id":"12","username":"Maya"},"timestamp":"2026-10-18T11:27:00Z","content":"```python\ndef greet(name):\n    # Comments and strings stay separate.\n    return \"Hello, \" + name\n\nprint(greet(\"Cressida\"))\n```"},
            {"id":"9703","author":{"id":"12","username":"Maya"},"timestamp":"2026-10-18T11:28:00Z","content":"```json\n{\n  \"enabled\": true, \"count\": 12,\n  \"label\": \"Syntax highlighting\"\n}\n```"}
          ])JSON"));
          if (view.starts_with("syntax-nix")) {
            m_messages.resize(1);
            m_messages.front().content = R"NIX(```nix
{
  description = "My NixOS config";
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    nur.url = "github:nix-community/NUR";
    home-manager = {
      url = "github:nix-community/home-manager";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };
  # An indented string with escaped and real interpolation.
  script = ''
    echo ''${PATH}
    echo ${if enabled then "yes" else "no"}
  '';
}
```)NIX";
          }
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          m_pointerX = chatLeft() + 100;
          m_pointerY = 150;
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
        }
        if (view == "inline-code"
            || view == "inline-code-light"
            || view == "inline-code-compact"
            || view.starts_with("message-hover-cycle")) {
          m_demoOverlay = view;
          m_showMembers = view.starts_with("message-hover-cycle");
          m_preferences.compact = view.ends_with("compact");
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9601","author":{"id":"11","username":"Lysec"},"timestamp":"2026-10-18T09:26:00Z","content":"`~/.cache/noctalia/noctalia.log`"},
            {"id":"9602","author":{"id":"11","username":"Lysec"},"timestamp":"2026-10-18T09:26:10Z","content":"Check `a < b` and `two words` in this line. 🙂 `aligned code`","edited_timestamp":"2026-10-18T09:27:00Z"},
            {"id":"9603","author":{"id":"12","username":"Maya"},"timestamp":"2026-10-18T09:28:00Z","content":"A long path: `~/.cache/noctalia/a-long-directory-name/another-directory/noctalia.log`\n```text\nFenced code keeps its own layout.\n```"}
          ])JSON"));
          if (view.starts_with("message-hover-cycle")) {
            m_messages.front().content = "Inline `two words`.\n```text\nFenced code with a Copy action.\n```";
            m_messages[1].content = "A continuation before the next author.";
            m_messages[2].content = "A different author starts a new group.";
            for (auto& message : m_messages)
              message.authorTag = api::GuildTag{m_guildId, "NOCT", {}};
          }
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
        }
        if (view == "joins" || view == "joins-light" || view == "joins-compact" || view == "join-cycle") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view == "joins-compact";
          if (view == "joins-light")
            m_preferences.theme = "light";
          m_messages.clear();
          const std::vector<std::string> people = {"NIRIXD", "ae", "lowvan", "nikusya", "Kyoka", "Telepuzik"};
          for (std::size_t i = 0; i < people.size(); ++i) {
            auto messages = api::parseMessages(
                nlohmann::json::array(
                    {{{"id", std::to_string(9400 + i)},
                      {"channel_id", m_channelId},
                      {"type", 7},
                      {"content", ""},
                      {"timestamp", "2026-10-17T15:22:00.00" + std::to_string(i) + "Z"},
                      {"author", {{"id", std::to_string(11 + i)}, {"username", people[i]}}},
                      {"reactions", nlohmann::json::array({{{"count", 1}, {"me", true}, {"emoji", {{"name", "❤️"}}}}})}}}
                )
            );
            m_messages.push_back(std::move(messages.front()));
          }
          api::GuildRoleUpdate roles{.guild = m_guildId};
          roles.roles = {{"50", 1, {0x5798D0}, "Nightling"}};
          for (const auto& message : m_messages)
            roles.members[message.authorId] = {"50"};
          m_roleDirectory.apply({false, {roles}});
          m_messageLayouts.clear();
        }
        if (view == "categories") {
          for (auto& channel : m_channels)
            channel.parentId = "100";
          m_channels.insert(m_channels.begin(), {"100", "PRIVATE", 4});
          m_channels.push_back({"200", "COMMUNITY", 4});
          api::Channel channel{"201", "showcase"};
          channel.parentId = "200";
          m_channels.push_back(std::move(channel));
          m_guildChannels[m_guildId] = m_channels;
        }
        if (view == "mentions" || view == "mentions-light" || view == "mentions-compact" || view == "mentions-cycle") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view == "mentions-compact";
          if (view == "mentions-light")
            m_preferences.theme = "light";
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9101","channel_id":"101","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-17T09:41:00Z","content":"A normal message starts the group.\nThis second line belongs to that same message."},
            {"id":"9102","channel_id":"101","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-17T09:41:10Z","content":"<@1> could you take a look?","mentions":[{"id":"1","username":"Alex"}]},
            {"id":"9103","channel_id":"101","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-17T09:41:20Z","content":"The Moonwarden role was mentioned.","mention_roles":["50"]},
            {"id":"9104","channel_id":"101","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-17T09:41:30Z","content":"@everyone the update is ready!","mention_everyone":true},
            {"id":"9105","channel_id":"101","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-17T09:41:40Z","edited_timestamp":"2026-10-17T09:42:00Z","content":"Literal code: `<@1> @everyone`"}
          ])JSON"));
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          api::GuildRoleUpdate roles;
          roles.guild = m_guildId;
          roles.members[m_account.id] = {"50"};
          m_roleDirectory.apply({false, {roles}});
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
        }
        if (view == "deleted"
            || view == "deleted-light"
            || view == "deleted-compact"
            || view == "deleted-cycle"
            || view == "message-edit"
            || view == "message-delete") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view == "deleted-compact";
          m_preferences.logDeletedMessages = m_preferences.logEditHistory = true;
          if (view == "deleted-light")
            m_preferences.theme = "light";
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9501","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-18T09:41:00Z","content":"This message is still here."},
            {"id":"9502","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-18T09:42:00Z","edited_timestamp":"2026-10-18T09:43:00Z","content":"This message was deleted. Its last known text stays readable.\n**Formatting** and `code` still work.","reactions":[{"emoji":{"name":"❤️"},"count":2,"me":true}]},
            {"id":"9503","author":{"id":"12","username":"Jun"},"timestamp":"2026-10-18T09:44:00Z","content":"Another normal message."},
            {"id":"9504","author":{"id":"12","username":"Jun"},"timestamp":"2026-10-18T09:45:00Z","content":"A second deleted message, removed in a bulk delete."}
          ])JSON"));
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          deleteMessages(m_channelId, {"9502"});
          deleteMessages(m_channelId, {"9504", "9599"});
          updateMessage(
              m_channelId, "9503",
              {.content = "We should refresh the workspace when a display reconnects.",
               .editedTimestamp = "2026-10-18T09:45:00Z"}
          );
          updateMessage(
              m_channelId, "9503",
              {.content =
                   "We should refresh the workspace and preserve the window positions when a display reconnects.",
               .editedTimestamp = "2026-10-18T09:46:00Z"}
          );
          if (view == "message-edit" || view == "message-delete") {
            m_messages[2].authorId = m_account.id;
            m_messages[2].author = m_account.name;
            m_contextMessage = "9503";
            m_contextChannel = m_channelId;
            m_contextUser = m_account.id;
            contextAction(view == "message-edit" ? Action::EditMessage : Action::DeleteMessage);
            if (view == "message-edit" && m_messageEdit)
              m_messageEdit->text += " Keep the animation smooth.";
          }
        }
        if (view.starts_with("chat-links")) {
          prepareChatLinksDemo();
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view == "chat-links-narrow")
            m_window.setUiScale(2);
        }
        if (view == "channel"
            || view.starts_with("channel-empty")
            || view == "channel-cycle"
            || view == "channel-search"
            || view.starts_with("channel-edit")
            || view == "channel-ephemeral"
            || view == "channel-modal")
          prepareChannelDemo(view);
        if (view.starts_with("channel-menu")
            || view.starts_with("channel-list-menu")
            || view.starts_with("channel-category-menu")
            || view.starts_with("channel-create")
            || view == "channel-duplicate"
            || view == "channel-delete"
            || view == "channel-move-confirm")
          prepareChannelMenuDemo(view);
        if (view.starts_with("rail-drag")
            || view.starts_with("folder-animation")
            || view == "folders-open"
            || view == "folders-closed") {
          m_folderMotion.clear();
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view == "folders-closed")
            m_openFolders.clear();
          else
            m_openFolders = {"10"};
          if (view.starts_with("rail-drag") || view.starts_with("folder-animation")) {
            m_demoOverlay = view;
            m_demoInteractionStep = 0;
          }
        }
        if (view.starts_with("floating-cycle")) {
          prepareChannelMenuDemo("channel-menu");
          closeContext();
          for (auto& channel : m_channels)
            if (channel.id == m_channelId)
              channel.name = "☕-offtopic";
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
          m_showMembers = true;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
        }
        if (view.starts_with("typing-")) {
          prepareChannelMenuDemo("channel-menu");
          closeContext();
          m_demoOverlay = view;
          m_showMembers = true;
          m_demoTypingLayout.reset();
          const std::vector<api::Account> typists{{"7", "Maya"}, {"8", "Noah"}, {"10", "Alex"}, {"11", "Dani"}};
          m_members.clear();
          for (std::size_t i = 0; i < typists.size(); ++i)
            m_members.push_back({static_cast<int>(i), typists[i], i ? "idle" : "online"});
          m_memberCount = static_cast<int>(m_members.size());
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (!view.starts_with("typing-cycle")) {
            const auto timestamp =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
            for (const auto& user : typists) {
              api::Event event{.operation = api::Operation::TypingStart, .session = m_session, .resource = m_channelId};
              event.typing = api::TypingStart{m_channelId, m_guildId, user.id, user.name, timestamp};
              typingEvent(event);
              if (view == "typing-one")
                break;
            }
          }
        }
        if (view.starts_with("server-menu") || view == "folder-menu-cycle") {
          prepareChannelMenuDemo("channel-menu");
          m_demoOverlay = view;
          closeContext();
        }
        if (view.starts_with("channel-invite"))
          prepareChannelInviteDemo(view);
        if (view.starts_with("server-settings")) {
          m_demoOverlay = view;
          prepareServerSettingsDemo();
        }
        if (view.starts_with("channel-create")
            || view == "channel-duplicate"
            || view == "channel-delete"
            || view == "channel-move-confirm") {
          const auto guild = std::ranges::find(m_guilds, m_guildId, &api::Guild::id);
          if (guild != m_guilds.end())
            guild->features.push_back("COMMUNITY");
          m_dialogKind = "sidebar-create";
          m_dialogTitle = "Create Channel";
          m_dialogAction = api::Interaction{.kind = api::InteractionKind::CreateChannel, .guild = m_guildId};
          m_dialogText = "";
          m_focusedInput = "dialog";
          if (view.find("voice") != std::string::npos)
            m_dialogAction->channelType = 2;
          if (view.find("forum") != std::string::npos)
            m_dialogAction->channelType = 15;
          if (view.find("category") != std::string::npos)
            m_dialogAction->channelType = 4;
          if (view.find("private") != std::string::npos || view.find("access") != std::string::npos) {
            m_dialogAction->privateChannel = true;
            m_dialogText = "moderators";
          }
          if (view.find("access") != std::string::npos)
            m_channelCreateAccessStep = true;
          if (view.find("light") != std::string::npos)
            m_preferences.theme = "light";
          if (view.find("narrow") != std::string::npos)
            m_window.setUiScale(1.8F);
          if (view == "channel-duplicate") {
            m_dialogAction->kind = api::InteractionKind::DuplicateChannel;
            m_dialogAction->channel = "103";
            m_dialogText = "other-channel-copy";
            m_dialogTitle = "Duplicate Channel";
          } else if (view == "channel-delete") {
            m_dialogKind = "sidebar-delete";
            m_dialogAction->kind = api::InteractionKind::DeleteChannel;
            m_dialogAction->channel = "103";
            m_dialogTitle = "Delete Channel";
          } else if (view == "channel-move-confirm") {
            m_dialogKind = "sidebar-move";
            m_dialogAction->kind = api::InteractionKind::MoveChannel;
            m_dialogAction->channel = "103";
            m_dialogAction->parentId = "201";
            m_dialogTitle = "Sync permissions?";
          }
        }
        if (view == "forum" || view == "forum-error" || view == "forum-cycle")
          prepareForumDemo(view);
        if (view == "threads" || view == "thread" || view == "thread-cycle")
          prepareThreadDemo(view);
        if (view == "role-styles"
            || view == "role-styles-light"
            || view == "role-style-cycle"
            || view == "role-members-cycle"
            || view == "role-emoji"
            || view == "role-emoji-cycle"
            || view == "role-style-settings") {
          m_demoOverlay = view;
          if (view == "role-styles-light")
            m_preferences.theme = "light";
          const std::vector<std::string> names = view.starts_with("role-emoji")
              ? std::vector<
                    std::
                        string>{"Kitsune 🦊 Cat 🏳️‍🌈", "Amber 🇩🇪", "👩🏽‍💻 Ada 1️⃣✨", "Default member"}
              : std::vector<std::string>{"⪻𝚂ҟ₳𝘕ↁ✧亗⪼", "Prismatic member", "Amber member", "Default member"};
          api::GuildRoleUpdate roles;
          roles.guild = "1";
          roles.replaceRoles = true;
          roles.enhanced = true;
          roles.roles = {
              {"50", 2, {0xF38BA8, 0x89DCEB, {}}},
              {"51", 3, {11127295, 16759788, 16761760}},
              {"52", 1, {0xF5B76B, {}, {}}}
          };
          m_messages.clear();
          m_members.clear();
          m_activeMemberIdentity.clear();
          for (std::size_t i = 0; i < names.size(); ++i) {
            const auto user = std::to_string(11 + i);
            const auto id = std::to_string(8200 + i);
            m_messages.push_back(
                {id, m_channelId, names[i], "A message from " + names[i] + ".", "2026-10-17T09:50:00Z", user}
            );
            api::MemberRow member;
            member.index = static_cast<int>(i);
            member.user = {user, names[i]};
            member.status = "online";
            m_members.push_back(std::move(member));
            roles.members[user] = i < 3 ? std::vector<std::string>{std::to_string(50 + i)} : std::vector<std::string>{};
          }
          if (view == "role-members-cycle")
            roles.members.clear();
          m_roleDirectory.apply({true, {std::move(roles)}});
          if (view.starts_with("role-emoji")) {
            api::GuildRoleUpdate headings{.guild = "1"};
            headings.roles = {
                {"50", 2, {0xF38BA8, 0x89DCEB, {}}, "Moonwarden", {}, "🌙"},
                {"51", 3, {11127295, 16759788, 16761760}, "Prismatic", {}, "🔮"},
                {"52", 1, {0xF5B76B, {}, {}}, "Amber"}
            };
            m_roleDirectory.apply({false, {headings}});
            const auto people = m_members;
            m_members.clear();
            m_activeMemberIdentity.clear();
            for (std::size_t i = 0; i < people.size(); ++i) {
              api::MemberRow header;
              header.index = static_cast<int>(i * 2);
              header.groupId = i < 3 ? std::to_string(50 + i) : "online";
              header.group = i < 3 ? headings.roles[i].name : "Online";
              header.count = 1;
              m_members.push_back(std::move(header));
              auto person = people[i];
              person.index = static_cast<int>(i * 2 + 1);
              m_members.push_back(std::move(person));
            }
          }
          m_memberCount = static_cast<int>(m_members.size());
          m_showMembers = true;
          if (view == "role-style-settings") {
            openSettings();
            selectSettingsPage("role-styles");
          }
        }
        if (view == "unicode-names") {
          m_messages.clear();
          const std::vector<std::string> names = {"Alex", "⪻𝚂ҟ₳𝘕ↁ✧亗⪼", "⪻𝚂ҟ₳𝘕ↁ✧亗⪼ [KURU]", "𝓢𝓮𝓻𝓮𝓲𝓷", "𓆩Serein𓆪"};
          m_members.clear();
          m_activeMemberIdentity.clear();
          for (std::size_t i = 0; i < names.size(); ++i) {
            const auto id = std::to_string(8100 + i);
            m_messages.push_back({id, m_channelId, names[i], "what is serein?", "2026-10-17T09:50:00Z", id});
            api::MemberRow member;
            member.index = static_cast<int>(i);
            member.user = {id, names[i]};
            member.status = "online";
            m_members.push_back(std::move(member));
          }
          m_memberCount = static_cast<int>(m_members.size());
          m_showMembers = true;
        }
        if (view == "video" || view == "video-cycle") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_messages.clear();
          api::Message clip{
              "7000", m_channelId, "Alex", "Original-resolution native video playback", "2026-10-17T09:50:00Z"
          };
          clip.attachments.push_back({"7001", "demo.mp4", "demo-video", "video/mp4", {"demo-video", 1280, 720}});
          m_messages.push_back(std::move(clip));
          m_images["demo-video"].texture = m_window.context().textureManager().loadFromRgba(pixels.data(), 640, 360);
        }
        if (view == "message-menu"
            || view == "interaction-cycle"
            || view == "reaction-submenu"
            || view == "reaction-people"
            || view == "reaction-loading"
            || view == "user-menu"
            || view == "reply"
            || view.starts_with("reply-cycle")
            || view == "edit-message"
            || view == "block-user"
            || view == "user-profile"
            || view.starts_with("profile-card")
            || view.starts_with("guild-tag")
            || view == "guild-profile"
            || view == "reactions"
            || view == "tooltip-long"
            || view == "blocked") {
          m_demoOverlay = view;
          if (view == "profile-card-light")
            m_preferences.theme = "light";
          // Keep interaction previews focused on the conversation, rather than
          // scrolling it above the large media fixture used by the regular demo.
          m_messages.resize(5);
          m_messages.back().embeds.clear();
          const std::vector<std::string> authors{"11", "12", "1", "11", "12", "1"};
          for (std::size_t i = 0; i < m_messages.size() && i < authors.size(); ++i)
            m_messages[i].authorId = authors[i];
          if (view == "edit-message")
            beginMessageEdit("3");
          m_messages[2].reactions = {{"", "👍", 3, true}, {"", "🎉", 2}, {"", "❤️", 5}};
          m_messages[1].reply = api::MessageReference{"1", m_channelId, "11", "Maya", m_messages.front().content};
          api::UserUpdates users;
          users.users["11"].relationship = view == "blocked" ? 2 : 1;
          m_userDirectory.apply(users);
          if (view.starts_with("guild-tag") || view == "guild-profile")
            prepareGuildTagDemo();
          if (view == "reply" || view.starts_with("reply-cycle"))
            m_replies[m_channelId] = {"1", m_channelId, "11", "Maya", m_messages.front().content};
          if (view.starts_with("reply-cycle")) {
            m_drafts[m_channelId] = "Reply draft";
            m_showMembers = false;
            if (view.ends_with("light"))
              m_preferences.theme = "light";
          }
        }
        if (view == "attachment-cycle" || view == "attachments" || view == "attachments-light") {
          if (view == "attachments-light")
            m_preferences.theme = "light";
          prepareAttachmentsDemo();
          if (view == "attachment-cycle")
            m_demoOverlay = view;
        } else if (view == "login-restoring" || view == "login-retry" || view.starts_with("login-themed")) {
          m_connected = false;
          m_connecting = m_restoringLogin = view == "login-restoring" || view == "login-themed-restoring";
          m_retrySavedLogin = view == "login-retry";
          if (view.starts_with("login-themed")) {
            m_demoOverlay = view;
            m_preferences.colorTheme = "gruvbox";
            m_preferences.theme = view == "login-themed-light" ? "light" : "dark";
            if (view == "login-themed-system-light") {
              m_preferences.theme = "system";
              m_themeSnapshot.systemLight = true;
            }
            m_showTokenEntry = view == "login-themed-token";
            m_connecting = m_restoringLogin || view == "login-themed-connecting";
          }
          m_status = m_restoringLogin ? "Your desktop keyring may ask you to unlock it."
                                      : "Could not reach Discord. Your saved sign-in is still available.";
        } else if (
            view == "gif-picker"
            || view == "gif-favorites"
            || view == "animated-media"
            || view == "animation-cycle"
            || view == "gif-cycle"
            || view == "gif-scroll-cycle"
            || view == "animated-image"
        ) {
          for (int i = 0; i < 8; ++i) {
            const auto path = "https://static.klipy.com/ii/cressida-demo-" + std::to_string(i) + ".gif";
            auto animation = std::make_shared<media::Animation>();
            animation->width = 96;
            animation->height = 64 + i * 8;
            animation->duration = std::chrono::milliseconds(240);
            for (int f = 0; f < 3; ++f) {
              media::Frame frame;
              frame.duration = std::chrono::milliseconds(80);
              frame.rgba.resize(animation->width * animation->height * 4);
              for (int py = 0; py < animation->height; ++py)
                for (int px = 0; px < animation->width; ++px) {
                  const auto index = (py * animation->width + px) * 4;
                  frame.rgba[index] = 60 + (px + i * 17 + f * 24) % 160;
                  frame.rgba[index + 1] = 60 + (py + i * 12) % 150;
                  frame.rgba[index + 2] = 180 + f * 20;
                  frame.rgba[index + 3] = 255;
                }
              animation->frames.push_back(std::move(frame));
            }
            auto& cached = m_images[path];
            cached.animation = animation;
            cached.texture = m_window.context().textureManager().loadFromRgba(
                animation->frames[0].rgba.data(), animation->width, animation->height
            );
            if (i % 2 == 0)
              for (auto& frame : animation->frames)
                frame.pack();
            api::Gif gif;
            gif.url = "https://klipy.com/gifs/cressida-demo-" + std::to_string(i);
            gif.src = path;
            gif.width = animation->width;
            gif.height = animation->height;
            gif.title = "Offline animation " + std::to_string(i + 1);
            m_gifs.push_back(gif);
            if (i % 2 == 0)
              m_gifFavorites.push_back(gif);
          }
          m_gifFavoritesLoaded = true;
          if (view == "gif-cycle" || view == "gif-scroll-cycle")
            m_demoOverlay = view;
          if (view == "animated-media" || view == "animation-cycle" || view == "animated-image") {
            m_messages.resize(1);
            m_messages[0].content = "Native looping media";
            api::Embed gif;
            gif.type = "gifv";
            gif.url = m_gifs[0].url;
            gif.video = {m_gifs[0].src, 480, 320};
            m_messages[0].embeds = {gif};
            m_messageLayouts.clear();
            if (view == "animated-image") {
              m_viewImage = m_gifs[0].src;
              m_viewUrl = m_gifs[0].url;
            }
          } else {
            m_gifMenu = true;
            m_gifFavoritesTab = view == "gif-favorites";
            m_focusedInput = "gif-search";
          }
        } else if (
            view == "unread-divider"
            || view == "unread-divider-light"
            || view == "unread-jump"
            || view == "unread-cycle"
        ) {
          if (view == "unread-divider-light")
            m_preferences.theme = "light";
          m_messages.clear();
          const bool longHistory = view == "unread-jump" || view == "unread-cycle";
          for (int i = 0; i < (longHistory ? 30 : 6); ++i)
            m_messages.push_back(
                {std::to_string(1000 + i), m_channelId, i % 2 ? "Maya" : "Alex",
                 i < 2 ? "A message from before your last visit."
                       : "Something new in the conversation.\nA little room to catch up with everyone.",
                 "2026-10-17T09:41:00Z"}
            );
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({{m_channelId, {"1001", m_messages.back().id}}});
          m_unreadTimeline.open(m_channelId);
          if (view == "unread-jump")
            m_unreadTimeline.positioned();
          if (view == "unread-cycle")
            m_demoOverlay = view;
        } else if (
            view == "images-loading"
            || view == "images-loading-light"
            || view == "images-failed"
            || view == "image-loading"
            || view == "image-failed"
            || view == "images-ready"
        ) {
          m_messages = {m_messages.back()};
          m_messageLayouts.clear();
          if (view == "images-loading-light")
            m_preferences.theme = "light";
          if (view != "images-ready")
            m_images["demo-preview"].texture = {};
          if (view == "images-failed" || view == "image-failed")
            m_images["demo-preview"].retry = std::chrono::steady_clock::now() + std::chrono::seconds(60);
          if (view == "image-loading" || view == "image-failed") {
            m_viewImage = "demo-preview";
            m_viewUrl = "https://example.com/preview.png";
            m_viewerImages = {{m_viewImage, m_viewUrl, 1280, 720}};
          }
        } else if (
            view == "chat-loading"
            || view == "chat-loading-light"
            || view == "chat-loading-compact"
            || view == "chat-loading-cached"
            || view == "chat-empty"
            || view == "channels-loading"
        ) {
          m_preferences.compact = view == "chat-loading-compact";
          if (view == "chat-loading-light")
            m_preferences.theme = "light";
          m_loadingMessages = view != "chat-empty" && view != "channels-loading";
          if (view != "chat-loading-cached") {
            m_messages.clear();
            m_messageLayouts.clear();
          }
          if (view == "channels-loading") {
            m_channelId.clear();
            m_channels.clear();
            m_loadingChannels = true;
          }
        } else if (
            view == "reply-emojis"
            || view == "reply-emojis-light"
            || view == "reply-emojis-compact"
            || view == "emoji-fit"
        ) {
          m_preferences.compact = view == "reply-emojis-compact";
          if (view == "reply-emojis-light")
            m_preferences.theme = "light";
          if (view == "emoji-fit")
            m_demoOverlay = view;
          m_messages.clear();
          const std::string replyAvatar = "/avatars/11/reply-preview.png";
          const std::vector<std::string> replies = {
              "my beautiful pink role <:woe:1407464763293831269>",
              "animated <a:dance:123> beside <:woe:1407464763293831269> 🙂",
              "`<:woe:1407464763293831269>` stays code; <:woe:1407464763293831269> is an emoji",
              "A long reply\nwith paragraph breaks and <:woe:1407464763293831269> near its end, "
                  + std::string(180, 'x')
          };
          for (std::size_t i = 0; i < replies.size(); ++i) {
            api::Message message{
                std::to_string(8400 + i), m_channelId, "Lysec", "they're back xD", "2026-10-17T09:41:00Z"
            };
            message.reply = api::MessageReference{"1", m_channelId, "11", "Yocraft", replies[i]};
            message.reply->avatar = replyAvatar;
            m_messages.push_back(std::move(message));
          }
          if (view == "emoji-fit") {
            m_messages[0].content = "Inline wide <:woe:1407464763293831269> and tall <a:dance:123> emoji.";
            m_messages[1].content = "<:woe:1407464763293831269>";
            m_messages[2].reactions = {{"1407464763293831269", "wide", 1}};
          }
          for (const bool animated : {false, true}) {
            const auto path = api::emojiPath(animated ? "123" : "1407464763293831269", animated);
            auto animation = std::make_shared<media::Animation>();
            animation->width = animated ? 32 : 64;
            animation->height = animated ? 64 : 32;
            animation->duration = std::chrono::milliseconds(400);
            for (int f = 0; f < 2; ++f) {
              media::Frame frame;
              frame.duration = std::chrono::milliseconds(200);
              frame.rgba.resize(animation->width * animation->height * 4);
              for (int py = 0; py < animation->height; ++py)
                for (int px = 0; px < animation->width; ++px) {
                  const auto offset = (py * animation->width + px) * 4;
                  const int cx = px - animation->width / 2, cy = py - animation->height / 2;
                  const bool face = cx * cx + cy * cy < 196;
                  const bool eye = cy >= -7 && cy <= -3 && ((cx >= -7 && cx <= -5) || (cx >= 5 && cx <= 7));
                  const bool mouth = cy >= 4 + f && cy <= 7 + f && cx >= -6 && cx <= 6;
                  const bool firstEdge = animated ? py < 6 : px < 6;
                  const bool lastEdge = animated ? py >= animation->height - 6 : px >= animation->width - 6;
                  frame.rgba[offset] = firstEdge ? 255 : lastEdge ? 40 : eye || mouth ? 50 : 245;
                  frame.rgba[offset + 1] = firstEdge ? 40
                      : lastEdge                     ? 240
                      : eye || mouth                 ? 40
                      : animated                     ? 130 + f * 70
                                                     : 180;
                  frame.rgba[offset + 2] = firstEdge || lastEdge ? 200 : eye || mouth ? 60 : 80;
                  frame.rgba[offset + 3] = face || firstEdge || lastEdge ? 255 : 0;
                }
              animation->frames.push_back(std::move(frame));
            }
            auto& cached = m_images[path];
            cached.texture = m_window.context().textureManager().loadFromRgba(
                animation->frames[0].rgba.data(), animation->width, animation->height
            );
            if (animated)
              cached.animation = std::move(animation);
          }
          const int avatarSize = static_cast<int>(Style::space2Xl);
          std::vector<std::uint8_t> avatarPixels(avatarSize * avatarSize * 4);
          const auto avatarFill = rgbHex(theme::palette.accent);
          const std::array avatarColor{avatarFill.r, avatarFill.g, avatarFill.b, 1.0F};
          for (std::size_t pixel = 0; pixel < avatarPixels.size(); ++pixel)
            avatarPixels[pixel] = static_cast<std::uint8_t>(
                std::lround(avatarColor[pixel % avatarColor.size()] * std::numeric_limits<std::uint8_t>::max())
            );
          m_images[replyAvatar].texture =
              m_window.context().textureManager().loadFromRgba(avatarPixels.data(), avatarSize, avatarSize);
          m_messageLayouts.clear();
        } else if (view == "reply-overlap") {
          m_messages.clear();
          for (int i = 0; i < 6; ++i) {
            api::Message message{
                std::to_string(4000 + i), m_channelId, "Maya", "This reply body stays below its preview.",
                "2026-10-17T09:41:00Z"
            };
            message.reply = api::MessageReference{
                "1", m_channelId, "11", "Alex",
                "The Linux kernel got open-sourced btw\nLike, the entire thing is just open to download on GitHub "
                "now\nhttps://github.com/torvalds/linux"
            };
            m_messages.push_back(std::move(message));
          }
          m_messageLayouts.clear();
        } else if (view == "connection-cycle") {
          m_demoOverlay = view;
          m_guilds.clear();
          m_guildUpdates.clear();
          resetGuildBootstrap();
          m_folders.clear();
          m_guildId = m_startupGuild = "2";
          m_channelId.clear();
          m_channels.clear();
          m_messages.clear();
          m_members.clear();
          m_activeMemberIdentity.clear();
          m_memberCount = 0;
          m_loadingChannels = true;
        } else if (view == "history-cycle") {
          m_demoOverlay = view;
          m_messages.clear();
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          loadHistory(api::HistoryDirection::Latest);
        } else if (
            view == "scroll-cycle"
            || view == "jump-latest-cycle"
            || view == "jump-latest"
            || view == "jump-latest-light"
        ) {
          m_demoOverlay = view;
          if (view == "jump-latest-light")
            m_preferences.theme = "light";
          m_messages.clear();
          for (int i = 0; i < 100; ++i)
            m_messages.push_back(
                {std::to_string(3000 + i), m_channelId, "Maya",
                 "Smooth native scrolling through a conversation.\nA second line keeps "
                 "the timeline long enough to scroll.",
                 "2026-10-17T09:41:00Z"}
            );
          m_messageLayouts.clear();
          if (view == "jump-latest" || view == "jump-latest-light")
            m_messageScroll = 140;
        } else if (view == "chat-preload-cycle") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_live = true;
          m_channelMessages.clear();
          m_channelHistoryState.clear();
          m_channelCacheOrder.clear();
          m_roleDirectory.apply({.guilds = {{.guild = m_guildId, .owner = m_account.id}}});
        } else if (view.starts_with("markdown")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.messageSize = Style::fontSizeBody;
          m_preferences.spoilerMode = 0;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9401","channel_id":"101","timestamp":"2026-10-21T10:45:00Z",
             "author":{"id":"11","username":"Maya"},
             "content":"[A linked phrase with spaces](https://example.com/markdown)\n||A hidden phrase **with bold words**||\n~~strike **through** several words~~\n__underlined text with spaces__\n-# A smaller line with **emphasis** and [a small link](https://example.com/small).\nBody size again."}
          ])JSON"));
          m_revealedSpoilers.clear();
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
        } else if (view.starts_with("embed-cycle") || view == "embeds" || view == "embeds-light") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_guildChannels[m_guildId] = m_channels;
          m_roleDirectory.apply({false, {{.guild = m_guildId, .owner = m_account.id}}});
          m_preferences.messageSize = Style::fontSizeHeader;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9201","channel_id":"101","type":24,"timestamp":"2026-10-20T06:54:00Z",
             "author":{"id":"11","username":"Maya"},
             "embeds":[{"type":"auto_moderation_message","description":"A blocked phrase in context",
               "fields":[{"name":"rule_name","value":"Block flagged words"},
                 {"name":"channel_id","value":"102"},{"name":"decision_id","value":"opaque-decision"},
                 {"name":"keyword","value":"blocked"},{"name":"keyword_matched_content","value":"blocked"},
                 {"name":"decision_outcome","value":"blocked"}]}]},
            {"id":"9202","channel_id":"101","timestamp":"2026-10-20T10:44:00Z",
             "author":{"id":"12","username":"Noah","bot":true},
             "embeds":[{"type":"rich","title":"Message Deleted","color":16733525,
               "description":"**Author:** Maya\n**Channel:** <#102>\n**Content:** An ordinary message.",
               "footer":{"text":"Today at 10:44"}}]},
            {"id":"9203","channel_id":"101","timestamp":"2026-10-20T10:45:00Z",
             "author":{"id":"11","username":"Maya"},
             "embeds":[{"type":"article","provider":{"name":"GitHub"},
               "title":"GitHub - repository preview with a continuous wrapped link title",
               "url":"https://example.com/preview","description":"A compact description below the linked title."}]}
          ])JSON"));
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
        } else if (view.starts_with("embed-components")) {
          m_demoOverlay = view;
          m_showMembers = false;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          const std::string proxy = "https://media.discordapp.net/external/demo/component-preview.png";
          auto embed = nlohmann::json::parse(
              R"EMBED({"type":"components","url":"https://en.wikipedia.org/wiki/D_(programming_language)","components":[{"type":17,"components":[
      {"type":10,"content":"## [D (programming language)](https://en.wikipedia.org/wiki/D_(programming_language))"},
      {"type":10,"content":"D, also known as dlang, is a multi-paradigm system programming language."},
      {"type":12,"items":[{"media":{"url":"https://upload.wikimedia.org/D.png","proxy_url":"https://media.discordapp.net/external/demo/component-preview.png","width":640,"height":480,"content_type":"image/png"}}]}
    ]}]})EMBED"
          );
          if (view.ends_with("missing"))
            embed["components"] = nlohmann::json::array({{{"type", 255}}});
          if (view.ends_with("spoiler"))
            embed["components"][0]["spoiler"] = true;
          m_messages = {
              {"9200", m_channelId, "Maya", "https://en.wikipedia.org/wiki/D_(programming_language) 😂",
               "2026-10-19T12:00:00Z", "11"}
          };
          api::parseMessagePatch(nlohmann::json{{"embeds", nlohmann::json::array({embed})}}).apply(m_messages[0]);
          std::vector<unsigned char> sample(160 * 120 * 4, 180);
          for (std::size_t i = 3; i < sample.size(); i += 4)
            sample[i] = 255;
          m_images[proxy].texture = m_window.context().textureManager().loadFromRgba(sample.data(), 160, 120);
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messageLayouts.clear();
        } else if (view.starts_with("reaction-spacing")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view.find("compact") != std::string::npos;
          m_preferences.showReactions = view.find("hidden") == std::string::npos;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages.clear();
          for (int i = 0; i < 3; ++i)
            m_messages.push_back(
                {std::to_string(9300 + i), m_channelId, "Maya",
                 i == 0       ? ""
                     : i == 1 ? "Text with wrapped reactions."
                              : "The following message has room to breathe.",
                 "2026-10-18T12:00:00Z", "11"}
            );
          m_messages[0].attachments.push_back(
              {"9400", "preview.png", "https://example.com/preview.png", "image/png", {"demo-preview", 640, 360}}
          );
          m_messages[0].reactions = {{"", "👍", 1, true}};
          for (int i = 0; i < 30; ++i)
            m_messages[1].reactions.push_back({"", "👍", i + 1});
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messageLayouts.clear();
        } else if (view.starts_with("reply-grouping")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.twelveHourClock = view.contains("12h");
          m_preferences.compact = view.find("compact") != std::string::npos;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9300","channel_id":"101","type":19,"author":{"id":"11","username":"Maya"},
             "content":"w","timestamp":"2026-10-20T12:35:00Z",
             "message_reference":{"message_id":"800","channel_id":"101"},
             "referenced_message":{"id":"800","channel_id":"101","author":{"id":"12","username":"Noah"},
               "content":"","attachments":[{"id":"801","filename":"gd6jv78.png","url":"https://example.com/gd6jv78.png"}]}},
            {"id":"9301","channel_id":"101","author":{"id":"11","username":"Maya"},
             "content":"a","timestamp":"2026-10-20T12:35:01Z"},
            {"id":"9302","channel_id":"101","author":{"id":"11","username":"Maya"},
             "content":"Another follow-up","timestamp":"2026-10-20T12:35:02Z"},
            {"id":"9303","channel_id":"101","type":19,"author":{"id":"11","username":"Maya"},
             "content":"A new reply starts a group","timestamp":"2026-10-20T12:35:03Z",
             "message_reference":{"message_id":"800","channel_id":"101"},
             "referenced_message":{"id":"800","channel_id":"101","author":{"id":"12","username":"Noah"},
               "content":"Original text"}},
            {"id":"9304","channel_id":"101","author":{"id":"11","username":"Maya"},
             "content":"This continues the new reply","timestamp":"2026-10-20T12:35:04Z"}
          ])JSON"));
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messageLayouts.clear();
        } else if (view.starts_with("chat-spacing")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view.find("compact") != std::string::npos;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages.clear();
          const int count = view.find("short") != std::string::npos ? 1 : 60;
          for (int i = 0; i < count; ++i) {
            api::Message message{
                std::to_string(3000 + i), m_channelId, "Maya", "An ordinary message in the conversation.",
                "2026-10-18T12:00:00Z",   "11"
            };
            if (i + 1 == count) {
              message.authorId = "12";
              message.author = "Noah";
              message.content = view.find("emoji") != std::string::npos ? "😭" : "The latest message.";
            }
            m_messages.push_back(std::move(message));
          }
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messageLayouts.clear();
          m_pointerX = chatLeft() + 140;
          m_pointerY = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight() - 130;
        } else if (view == "emoji-sizing" || view == "emoji-sizing-compact") {
          m_preferences.compact = view == "emoji-sizing-compact";
          prepareEmojiPicker();
          m_showMembers = false;
          m_messages.clear();
          const auto* emoji = m_emojiCatalog.find("800000000000000000");
          const auto custom = emoji ? emoji->token() : "<:wave:123>";
          const std::vector<std::string> samples = {
              "Inline emoji: 🙂 ❤️ 👩🏽‍💻 " + custom + " with normal text.", "🙂 ❤️ " + custom,
              "👨‍👩‍👧‍👦 🇩🇪 1️⃣ 👩🏽‍💻", "`🙂 " + custom + "` stays literal in code."
          };
          for (std::size_t i = 0; i < samples.size(); ++i)
            m_messages.push_back({std::to_string(8000 + i), m_channelId, "Maya", samples[i], "2026-10-17T09:41:00Z"});
          m_messageLayouts.clear();
        } else if (view == "media-only") {
          prepareEmojiPicker();
          const auto* emoji = m_emojiCatalog.find("800000000000000000");
          if (emoji) {
            const auto url = "https://cdn.discordapp.com" + api::emojiPath(emoji->id, emoji->animated);
            m_messages.resize(1);
            m_messages[0].content = "[woe](" + url + ")";
            api::Embed preview;
            preview.type = "image";
            preview.url = url;
            preview.thumbnail = {api::emojiPath(emoji->id, emoji->animated), 160, 160};
            m_messages[0].embeds = {preview};
            m_messageLayouts.clear();
          }
        } else if (
            view == "stickers"
            || view == "stickers-light"
            || view == "sticker-picker"
            || view == "sticker-picker-light"
            || view == "sticker-cycle"
        ) {
          prepareStickers();
          m_preferences.emojiImageLinks = false;
          if (view.ends_with("-light"))
            m_preferences.theme = "light";
          m_messages.resize(4);
          const auto& fixtures = m_stickers.at(m_guilds.front().id);
          for (std::size_t i = 0; i < m_messages.size(); ++i) {
            m_messages[i].content = i == 1 ? "A sticker with a caption" : "";
            m_messages[i].stickers = {fixtures[i]};
            m_messages[i].attachments.clear();
            m_messages[i].embeds.clear();
          }
          m_messageLayouts.clear();
          if (view.starts_with("sticker-picker") || view == "sticker-cycle") {
            m_emojiMenu = m_stickerTab = true;
            m_focusedInput = "emoji-search";
          }
          if (view == "sticker-cycle")
            m_demoOverlay = view;
        } else if (view == "composer") {
          m_drafts[m_channelId] = "A centered message";
        } else if (view == "emoji-picker" || view == "emoji-scrolled" || view == "emoji-scroll-cycle") {
          m_emojiMenu = true;
          if (view == "emoji-scrolled")
            m_emojiScroll = 59;
          if (view == "emoji-scroll-cycle")
            m_demoOverlay = view;
        } else if (
            view == "unread" || view == "unread-badges" || view == "unread-badges-light" || view == "unread-badge-cycle"
        ) {
          if (view == "unread-badges-light")
            m_preferences.theme = "light";
          m_privateChannels = {{"900", "Maya", 1}, {"901", "Studio", 3}};
          m_unreadTimeline.snapshot(
              {{"102", {"8999", "9000", 1}},
               {"104", {"8999", "9000", 12}},
               {"201", {"8999", "9000", 99}},
               {"301", {"8999", "9000", 4}},
               {"900", {"8999", "9000", 3}},
               {"901", {"8999", "9000", 120}}},
              "reset"
          );
          m_unreadGuilds = {{"102", "1"}, {"104", "1"}, {"201", "2"}, {"301", "3"}, {"900", ""}, {"901", ""}};
          for (const auto& [channel, guild] : m_unreadGuilds) {
            m_unreadChannels.insert(channel);
            if (guild.empty())
              m_unreadDMs.insert(channel);
          }
          // A collapsed folder exercises aggregation without counting icons twice.
          m_folders = {{"", "", 0, {"1"}}, {"50", "Creative spaces", 0x8A76C7, {"2", "3"}}};
          m_messages.back().content = "Hi <@11>, check <#102>. `literal <@11>`";
          m_messages.back().mentions = {{"11", "Maya"}};
          if (view == "unread-badge-cycle") {
            m_demoOverlay = view;
            for (int i = 0; i < 40; ++i)
              m_channels.push_back({std::to_string(1100 + i), "quiet-channel-" + std::to_string(i + 1)});
            m_channels.push_back({"1200", "mentioned-channel"});
            auto cursors = std::map<std::string, api::UnreadCursor>{
                {"102", {"8999", "9000", 1}}, {"104", {"8999", "9000", 12}}, {"1200", {"8999", "9000", 2}}
            };
            m_unreadTimeline.snapshot(std::move(cursors), "reset");
            m_unreadGuilds["1200"] = "1";
            m_unreadChannels.insert("1200");
          }
        } else if (
            view.starts_with("dm-home")
            || view.starts_with("dm-requests")
            || view.starts_with("dm-friends")
            || view.starts_with("dm-compose")
            || view.starts_with("dm-pins")
        ) {
          prepareDmHomeDemo(view);
        } else if (view.starts_with("cosmetics")) {
          prepareCosmeticsDemo(view);
        } else if (view.starts_with("dm-profile")) {
          prepareDmProfileDemo(view);
        } else if (view == "dm-presence") {
          m_privateChannels.clear();
          for (const auto& [id, name] : std::vector<std::pair<std::string, std::string>>{
                   {"11", "Maya"}, {"12", "Noah"}, {"13", "Robin"}, {"14", "Sam"}
               }) {
            api::Channel channel;
            channel.id = "90" + id;
            channel.type = 1;
            channel.name = name;
            channel.recipients.push_back({id, name});
            m_privateChannels.push_back(std::move(channel));
          }
          api::UserUpdates users;
          users.users["11"] = {"My friend 🌿", "online"};
          users.users["12"] = {{}, "idle"};
          users.users["13"] = {{}, "dnd"};
          users.users["14"] = {{}, "offline"};
          m_userDirectory.apply(users);
          showDirectMessages();
          selectChannel("9011");
          updateDmMembers();
          m_messages.front().authorId = "11";
          m_messages.front().content = "Hi <@11>! This uses your personal nickname.";
          m_messages.front().mentions = {{"11", "Maya"}};
          m_messageLayouts.clear();
        } else if (view == "image") {
          m_viewImage = "demo-preview";
          m_viewUrl = "https://example.com/preview.png";
          m_viewerImages = {{m_viewImage, m_viewUrl}};
        } else if (view == "status-menu")
          m_presenceMenu = true;
        else if (view == "hover") {
          m_pointerX = 160;
          m_pointerY = 185;
        } else if (
            view.starts_with("user-account-dialog:")
            || view.starts_with("user-account-dialog-light:")
            || view.starts_with("user-account-dialog-narrow:")
        ) {
          if (view.starts_with("user-account-dialog-light:"))
            m_preferences.theme = "light";
          if (view.starts_with("user-account-dialog-narrow:"))
            m_window.setUiScale(1.8F);
          openSettings();
          selectSettingsPage("account");
          prepareUserAccountDemo();
          userAccountAction(view.substr(view.find(':') + 1));
        } else if (
            view == "window-controls"
            || view == "window-controls-custom"
            || view == "window-controls-left"
            || view == "window-controls-compact"
        ) {
          m_preferences.desktopDecorations = false;
          if (view == "window-controls-left") {
            m_preferences.windowControls.left = true;
            m_preferences.windowControls.order = 2;
            m_preferences.windowControls.style = static_cast<int>(WindowIconStyle::Circular);
          }
          if (view == "window-controls-compact") {
            m_preferences.windowControls.buttonWidth = static_cast<int>(Style::windowButtonMinimumWidth);
            m_preferences.windowControls.hoverHighlight = false;
          }
          if (view == "window-controls-custom")
            m_preferences.windowControls.style = static_cast<int>(WindowIconStyle::Custom);
          openSettings();
          selectSettingsPage("advanced");
          if (view == "window-controls-custom")
            m_settingsScroll = layout::Titlebar::height * windowIconNames.size() * 3;
        } else if (view.starts_with("window-controls-cycle")) {
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_preferences.desktopDecorations = false;
          openSettings();
          selectSettingsPage("advanced");
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (view.starts_with("decorations-cycle")) {
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          openSettings();
          selectSettingsPage("advanced");
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (view.starts_with("reduced-motion-cycle")) {
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          openSettings();
          selectSettingsPage("accessibility");
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (view == "user-preferences-cycle") {
          openSettings();
          selectSettingsPage("keybinds");
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (view == "user-account-cycle" || view == "user-settings-lists-cycle") {
          openSettings();
          selectSettingsPage(view == "user-account-cycle" ? "account" : "relationships");
          if (view == "user-account-cycle")
            prepareUserAccountDemo();
          else
            prepareUserSettingsListsDemo();
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (view == "appearance-light") {
          m_preferences.theme = "light";
          openSettings();
          m_settingsPage = "appearance";
        } else if (
            view.starts_with("settings:")
            || view.starts_with("themes")
            || view.starts_with("settings-font")
            || view.starts_with("settings-narrow")
            || view == "settings-feedback"
            || view == "cressida-settings-light"
            || view == "account"
            || view == "profile"
            || view == "appearance"
            || view == "text"
            || view == "expressions"
            || view == "cressida-settings"
            || view == "cressida-settings-search"
            || view == "presence"
            || view == "privacy"
            || view == "notifications"
            || view == "activity"
            || view == "friends"
            || view == "keybinds"
            || view == "accessibility"
            || view == "search"
        ) {
          openSettings();
          if (view.starts_with("themes")) {
            if (view == "themes-cycle")
              m_demoOverlay = view;
            if (view.contains("catppuccin"))
              m_preferences.colorTheme = "catppuccin";
            if (view.contains("gruvbox"))
              m_preferences.colorTheme = "gruvbox";
            if (view == "themes-narrow")
              m_window.setUiScale(1.8F);
            if (view.contains("rose-pine"))
              m_preferences.colorTheme = "rose-pine";
            if (view.ends_with("-light"))
              m_preferences.theme = "light";
            if (view == "themes-system-light") {
              m_preferences.theme = "system";
              m_themeSnapshot.systemLight = true;
            }
          }
          if (view == "settings-font-cycle")
            m_demoOverlay = view;
          if (view.starts_with("settings-narrow"))
            m_window.setUiScale(1.8F);
          if (view == "cressida-settings-light" || view == "settings-fonts-light")
            m_preferences.theme = "light";
          if (view == "settings:gifts-preview" || view == "settings:invoice")
            m_demoOverlay = view;
          selectSettingsPage(
              view == "settings:gifts-preview"                                                   ? "gifts"
                  : view == "settings:invoice"                                                   ? "payments"
                  : view.starts_with("settings:")                                                ? view.substr(9)
                  : view.starts_with("themes")                                                   ? "themes"
                  : view.starts_with("settings-font")                                            ? "appearance"
                  : view == "settings-narrow-appearance"                                         ? "appearance"
                  : view.starts_with("settings-narrow") || view.starts_with("cressida-settings") ? "cressida"
                  : view == "search" || view == "settings-feedback"                              ? "privacy"
                                                                                                 : view
          );
          if (view.starts_with("settings-fonts")) {
            fontPreference("font-picker");
            if (view == "settings-fonts-filtered")
              m_settingsFields["font_search"] = "mono";
            if (view == "settings-fonts-selected" && !m_installedFonts.empty())
              fontPreference("font-family:" + m_installedFonts.front());
          }
          if (view == "settings-feedback") {
            m_settingsDraft["restrict_new_guild_dms"] ^= 1;
            m_settingsStatus = "Could not save changes. Try again.";
          }
          if (view == "cressida-settings-search")
            m_settingsFields["search"] = "role";
          if (view == "search")
            m_settingsFields["search"] = "privacy";
        }
      }
    }
  }

  void ClientApplication::rectangle(Node& parent, float x, float y, float w, float h, Color fill, float radius) {
    auto node = std::make_unique<RectNode>();
    node->setPosition(x, y);
    node->setSize(std::max(w, 0.0F), std::max(h, 0.0F));
    node->setStyle(RoundedRectStyle{.fill = fill, .radius = Style::scaledShapeRadius(radius, w, h)});
    parent.addChild(std::move(node));
  }

  void ClientApplication::floatingPanel(Node& parent, const layout::Frame& frame, Color fill) {
    m_floatingFrames.push_back(frame);
    auto node = std::make_unique<RectNode>();
    node->setHitTestVisible(false);
    node->setPosition(frame.x, frame.y);
    node->setSize(frame.width, frame.height);
    node->setStyle(
        {.fill = fill, .border = theme::raised, .radius = Style::scaledRadiusXl(), .borderWidth = Style::borderWidth}
    );
    parent.addChild(std::move(node));
  }

  float ClientApplication::label(
      Node& parent, float x, float y, float width, std::string text, float size, Color color, FontWeight weight,
      int lines, bool markup
  ) {
    auto node = std::make_unique<TextNode>();
    auto metrics = m_window.target().renderer().measureText(
        text, size, weight, markup ? 0 : width, lines, TextAlign::Start, {}, TextEllipsize::End, markup
    );
    node->setText(std::move(text));
    node->setFontSize(size);
    node->setUseMarkup(markup);
    node->setFontWeight(weight);
    node->setColor(color);
    // Rich fragments are already wrapped and measured by message_layout.
    // Keep their Pango attributes intact rather than applying a second width
    // constraint (the renderer replaces markup attributes when constraining).
    node->setMaxWidth(markup ? 0 : width);
    node->setMaxLines(lines);
    // TextNode's origin is the baseline; this helper accepts the logical top edge.
    node->setPosition(x, y - metrics.top);
    // TextMetrics exposes top/bottom rather than an assumed line-height.
    const float height = std::max(size + 4, metrics.bottom - metrics.top);
    node->setSize(width, height);
    parent.addChild(std::move(node));
    return height;
  }

  float ClientApplication::countBadge(Node& parent, float right, float y, int count, bool rail, std::string_view noun) {
    if (count <= 0)
      return 0;
    using B = layout::CountBadge;
    const auto text = count > 99 ? "99+" : std::to_string(count);
    const float height = rail ? B::railHeight : B::height;
    const float size = rail ? B::railFontSize : B::fontSize;
    const auto weight = rail ? FontWeight::Medium : FontWeight::Bold;
    const auto metrics = m_window.target().renderer().measureText(text, size, weight);
    const float width = std::max(height, std::ceil(metrics.width) + (rail ? B::railPadding : B::padding));
    const float x = right - width;
    if (rail)
      rectangle(
          parent, x - B::ring, y - B::ring, width + B::ring * 2, height + B::ring * 2, theme::railBackground,
          (height + B::ring * 2) / 2
      );
    rectangle(parent, x, y, width, height, destructive, height / 2);
    label(
        parent, x + (width - metrics.width) / 2, y + (height - (metrics.bottom - metrics.top)) / 2, width, text, size,
        rail ? readableTextColorForBackground(destructive) : rgbHex(0xFFFFFF), weight
    );
    float ox = 0, oy = 0;
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      ox += node->x();
      oy += node->y();
    }
    bool inside = true;
    float nx = ox, ny = oy;
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      if (node->clipChildren())
        inside &=
            m_pointerX >= nx && m_pointerX < nx + node->width() && m_pointerY >= ny && m_pointerY < ny + node->height();
      nx -= node->x();
      ny -= node->y();
    }
    if (inside && hovered(x + ox, y + oy, width, height))
      m_tooltip = std::to_string(count) + ' ' + std::string(noun) + (count == 1 ? "" : "s");
    return width;
  }

  void ClientApplication::button(
      Node& parent, Hit hit, std::string text, bool primary, bool selected, bool fill, bool centered
  ) {
    float offsetX = 0, offsetY = 0;
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      offsetX += node->x();
      offsetY += node->y();
    }
    const auto natural = m_window.target().renderer().measureText(text, Style::fontSizeBody, FontWeight::Medium, 0, 1);
    if (!fill)
      hit.width = std::min(hit.width, std::ceil(natural.width) + 24);
    if (!fill)
      hit.height = std::min(hit.height, std::ceil(natural.bottom - natural.top) + 14);
    const bool over = hit.contains(m_pointerX - offsetX, m_pointerY - offsetY);
    rectangle(
        parent, hit.x, hit.y, hit.width, hit.height,
        primary                ? (over ? lerpColor(accent, foreground, .12F) : accent)
            : selected || over ? raised
                               : panel,
        10
    );
    const auto metrics =
        m_window.target().renderer().measureText(text, Style::fontSizeBody, FontWeight::Medium, hit.width - 24, 1);
    const float textHeight = metrics.bottom - metrics.top;
    const float captionWidth = std::max(1.0F, std::min(natural.width, hit.width - 24));
    label(
        parent, hit.x + (centered ? (hit.width - captionWidth) / 2 : 14), hit.y + (hit.height - textHeight) / 2,
        centered ? std::ceil(captionWidth) + 1 : hit.width - Style::spaceXl, std::move(text), Style::fontSizeBody,
        primary ? ink : foreground, FontWeight::Medium
    );
    hit.x += offsetX;
    hit.y += offsetY;
    m_hits.push_back(std::move(hit));
  }

  void ClientApplication::discardImage(const std::string& path) {
    const auto at = m_images.find(path);
    if (at == m_images.end())
      return;
    auto texture = at->second.texture;
    m_images.erase(at);
    if (!texture.valid())
      return;
    if (m_window.context().makeCurrent(m_window.target()))
      m_window.context().textureManager().unload(texture);
    else
      m_retiredImages.push_back(texture);
  }

  void ClientApplication::discardImagesWithPrefix(std::string_view prefix) {
    for (auto image = m_images.begin(); image != m_images.end();) {
      if (!image->first.starts_with(prefix)) {
        ++image;
        continue;
      }
      const auto key = image->first;
      ++image;
      discardImage(key);
    }
  }

  bool ClientApplication::image(Node& parent, float x, float y, float size, const std::string& path, float radius) {
    auto asset = path;
    if (asset.find("/avatars/") != std::string::npos || asset.find("/icons/") != std::string::npos) {
      if (const auto slash = asset.rfind('/'); slash != std::string::npos && asset.substr(slash + 1).starts_with("a_"))
        if (const auto png = asset.find(".png", slash); png != std::string::npos)
          asset.replace(png, 4, ".gif");
    }
    return picture(parent, x, y, size, size, asset, radius);
  }

  bool ClientApplication::emojiImage(Node& parent, float x, float y, float size, const std::string& path) {
    return picture(parent, x, y, size, size, path, 0, false, false, ImageFitMode::Contain);
  }

  bool ClientApplication::picture(
      Node& parent, float x, float y, float width, float height, const std::string& path, float radius,
      bool placeholder, bool videoPoster, ImageFitMode fit, std::optional<std::chrono::milliseconds> animationTime,
      ImageUse use
  ) {
    const auto* localImage = draftImage(path);
    if (path.empty() || (m_demo && !localImage && !m_images.contains(path)))
      return false;
    auto global = [&](const Node* node) {
      std::pair<float, float> pos{};
      for (; node && node != m_scene.get(); node = node->parent()) {
        pos.first += node->x();
        pos.second += node->y();
      }
      return pos;
    };
    const auto [ox, oy] = global(&parent);
    const float gx = x + ox, gy = y + oy;
    float clipLeft = 0, clipTop = 36, clipRight = m_window.width(),
          clipBottom = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent())
      if (node->clipChildren()) {
        const auto [nx, ny] = global(node);
        clipLeft = std::max(clipLeft, nx);
        clipTop = std::max(clipTop, ny);
        clipRight = std::min(clipRight, nx + node->width());
        clipBottom = std::min(clipBottom, ny + node->height());
      }
    if (gx + width <= clipLeft || gx >= clipRight || gy + height <= clipTop || gy >= clipBottom)
      return false;
    if (localImage) {
      if (!localImage->texture.valid())
        return false;
      auto node = std::make_unique<ImageNode>();
      node->setPosition(x, y);
      node->setSize(width, height);
      node->setTextureId(localImage->texture.id);
      node->setTextureSize(localImage->texture.width, localImage->texture.height);
      node->setFitMode(fit);
      node->setRadius(Style::scaledShapeRadius(radius, width, height));
      parent.addChild(std::move(node));
      return true;
    }
    std::string stillIcon;
    if (path.find("/icons/") != std::string::npos) {
      if (const auto gif = path.find(".gif", path.rfind('/')); gif != std::string::npos) {
        stillIcon = path;
        stillIcon.replace(gif, 4, ".png");
        const float left = std::max(gx, clipLeft), top = std::max(gy, clipTop);
        const bool over =
            hovered(left, top, std::min(gx + width, clipRight) - left, std::min(gy + height, clipBottom) - top);
        const bool play = over
            && (m_demo || m_window.active())
            && m_preferences.animateAvatars
            && !m_preferences.reducedMotion
            && !m_contextOpen
            && (gx >= layout::Sidebar::railWidth || (sidebarResizeAvailable() && !m_resizingSidebar));
        // A separate still URL keeps duplicate icons (for example in a folder)
        // independent of the shared animated texture, and avoids idle GIF loads.
        if (!play)
          return picture(
              parent, x, y, width, height, stillIcon, radius, placeholder, videoPoster, fit, animationTime, use
          );
      }
    }
    auto& cached = m_images[path];
    cached.uses |= imageUseMask(use);
    if (auto pending = m_pendingImages.find(path); pending != m_pendingImages.end())
      pending->second |= cached.uses;
    cached.touched = ++m_imageSerial;
    cached.visibleDraw = m_animationDraw;
    if (!cached.texture.valid()) {
      if (!m_demo
          && !m_pendingImages.contains(path)
          && std::chrono::steady_clock::now() >= cached.retry
          && m_assets.request(path, videoPoster))
        m_pendingImages[path] = cached.uses;
      if (!stillIcon.empty())
        return picture(
            parent, x, y, width, height, stillIcon, radius, placeholder, videoPoster, fit, animationTime, use
        );
      if (placeholder) {
        const bool failed = cached.retry != std::chrono::steady_clock::time_point{} && !m_pendingImages.contains(path);
        rectangle(parent, x, y, width, height, lerpColor(background, muted, 0.13F), radius);
        if (width >= 64 && height >= 64) {
          const float size = std::min(32.0F, std::min(width, height) * 0.25F);
          const bool caption = failed && width >= 160 && height >= 100;
          const float iconY = y + (height - size) / 2;
          glyph(
              parent, x + (width - size) / 2, iconY, size, failed ? "photo-off" : "photo",
              lerpColor(background, muted, 0.38F)
          );
          if (caption) {
            const auto textWidth =
                m_window.target().renderer().measureText("Image unavailable", Style::fontSizeDescription).width;
            label(
                parent, x + (width - textWidth) / 2, iconY + size + Style::spaceDense, textWidth + Style::spaceXxs,
                "Image unavailable", Style::fontSizeDescription, muted
            );
          }
        }
      }
      return false;
    }
    if (!videoPoster && cached.animation && cached.animation->frames.size() > 1) {
      const auto now = std::chrono::steady_clock::now();
      if (cached.visibleFrame + 1 < m_animationDraw || cached.animationStart == std::chrono::steady_clock::time_point{})
        cached.animationStart = now;
      cached.visibleFrame = m_animationDraw;
      const bool emojiAnimation = path.find("/emojis/") != std::string::npos;
      const bool avatarAnimation =
          path.find("/avatars/") != std::string::npos || path.find("/icons/") != std::string::npos;
      const bool cosmeticAnimation = path.contains("/avatar-decoration-presets/")
          || path.contains("/collectibles-shop/")
          || path.contains("/assets/profile_effects/")
          || path.contains("/assets/content/");
      const bool autoplay = cosmeticAnimation ? m_preferences.animateAvatars
          : emojiAnimation                    ? m_preferences.animateEmoji
          : avatarAnimation                   ? m_preferences.animateAvatars
                                              : m_preferences.autoplayGifs;
      const bool messageMedia =
          !avatarAnimation && !cosmeticAnimation && use == ImageUse::Content && !path.contains("/banners/");
      const bool motionAllowed = !m_preferences.reducedMotion
          || (emojiAnimation ? m_preferences.emojiWithReducedMotion
                             : messageMedia && m_preferences.gifsWithReducedMotion);
      const bool playing = !path.contains("passthrough=false")
          && (m_demo || m_window.active())
          && motionAllowed
          && (autoplay || (!emojiAnimation && !avatarAnimation && !cosmeticAnimation && hovered(gx, gy, width, height)))
          && (m_viewImage.empty() || path == m_viewImage);
      if (playing) {
        if (cached.playingDraw + 1 < m_animationDraw)
          cached.animationStart = now;
        cached.playingDraw = m_animationDraw;
      }
      const auto elapsed =
          animationTime.value_or(std::chrono::duration_cast<std::chrono::milliseconds>(now - cached.animationStart));
      const auto& animation = *cached.animation;
      const auto frame = playing || cached.playingDraw == m_animationDraw ? animation.frameAt(elapsed) : 0;
      if (frame != cached.animationFrame) {
        const auto pixels = animation.pixels(frame, m_animationPixels);
        if (!pixels.empty()) {
          m_window.context().textureManager().updateSubImage(
              cached.texture, pixels.data(), 0, 0, animation.width, animation.height, TextureDataFormat::Rgba
          );
          cached.animationFrame = frame;
        }
      }
      if (playing) {
        auto offset = elapsed % animation.duration;
        for (std::size_t i = 0; i < frame; ++i)
          offset -= animation.frames[i].duration;
        m_animationDelay = std::min(
            m_animationDelay, std::max(std::chrono::milliseconds(10), animation.frames[frame].duration - offset)
        );
      }
    }
    auto node = std::make_unique<ImageNode>();
    node->setPosition(x, y);
    node->setSize(width, height);
    node->setTextureId(cached.texture.id);
    node->setTextureSize(cached.texture.width, cached.texture.height);
    node->setFitMode(fit);
    node->setRadius(Style::scaledShapeRadius(radius, width, height));
    parent.addChild(std::move(node));
    return true;
  }

  void ClientApplication::avatar(
      Node& parent, float x, float y, float size, const std::string& path, const std::string& name,
      const std::string& requestedUser, const std::string& decoration, std::optional<layout::Frame> hoverRegion
  ) {
    if (!image(parent, x, y, size, path, size / 2)) {
      rectangle(parent, x, y, size, size, raised, size / 2);
      label(
          parent, x + size * 0.28F, y + size * 0.17F, size * 0.7F, initial(name), size * 0.45F, accent,
          FontWeight::SemiBold
      );
    }
    auto user = requestedUser;
    if (user.empty()) {
      const auto marker = path.find("/users/");
      const auto avatar = path.find("/avatars/");
      const auto start = marker != std::string::npos ? marker + std::string_view("/users/").size()
          : avatar != std::string::npos              ? avatar + std::string_view("/avatars/").size()
                                                     : std::string::npos;
      if (start != std::string::npos)
        user = path.substr(start, path.find('/', start) - start);
    }
    const auto cosmetics = m_userDirectory.cosmetics(user, m_dmMode ? "" : m_guildId);
    drawAvatarDecoration(
        parent, x, y, size, decoration.empty() ? cosmetics.decoration.value_or("") : decoration,
        hoverRegion.value_or(layout::Frame{x, y, size, size})
    );
  }

  void ClientApplication::draw() {
    m_floatingFrames.clear();
    m_typingVisible = false;
    m_roleMotion.begin();
    m_videoVisible = false;
    if (!m_videoExpanded)
      m_videoPosters.clear();
    ++m_animationDraw;
    m_animationDelay = std::chrono::milliseconds::max();
    const bool lightMode = resolveLightMode(m_preferences.theme, m_themeSnapshot.systemLight);
    const auto& colorTheme = m_themeSnapshot.selected(m_preferences.colorTheme);
    theme::apply(lightMode ? colorTheme.light : colorTheme.dark, lightMode, m_preferences.saturation / 100.0F);
    Style::setCornerRadiusScale(static_cast<float>(m_preferences.cornerRadius) / Style::cornerRadiusDefaultPercent);
    m_window.setDesktopDecorations(m_preferences.desktopDecorations);
    m_window.setWindowControls(m_preferences.windowControls);
    updateWindowIcons();
    m_window.context().setTextFontFamily(m_connected ? m_preferences.fontFamily : std::string{});
    const float w = m_window.width(),
                h = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
    m_tooltip.clear();
    if (!m_emojiMenu && m_emojiOpened)
      closeEmojiPicker();
    m_gifTargets.clear();
    m_stickerTargets.clear();
    m_reactionHoveredThisFrame = false;
    m_contextTargets.clear();
    m_messageTops.clear();
    m_textAreas.clear();
    m_messageText.clear();
    if (m_viewImage.empty())
      m_viewerImages.clear();
    beginControls();
    if (!m_scene)
      m_scene = std::make_unique<Node>();
    while (!m_scene->children().empty()) {
      auto removed = m_scene->removeChild(m_scene->children().back().get());
    }
    m_scene->setPopupContext(this);
    m_scene->setSize(w, h);
    m_scene->setPosition(0, m_window.titlebarHeight() - layout::Window::titlebarHeight);
    auto& root = *m_scene;
    m_hits.clear();
    rectangle(
        root, 0, 0, std::ceil(m_window.target().renderWidth()),
        std::ceil(m_window.target().renderHeight()) + 36 - m_window.titlebarHeight(), background
    );
    if (m_window.titlebarHeight() > 0)
      drawTitlebar(root, w);

    const auto pointer = std::pair{m_pointerX, m_pointerY};
    const bool mediaViewer = !m_viewImage.empty() || m_videoExpanded;
    if (mediaViewer) {
      // Covered surfaces still paint, but only the media overlay sees the pointer.
      m_pointerX = -w;
      m_pointerY = -h;
    }

    if (!m_connected && m_restoringLogin) {
      const float center = h / 2;
      const std::string title = "Opening Cressida…";
      const std::string subtitle = "Restoring your saved sign-in";
      auto& renderer = m_window.target().renderer();
      const auto titleWidth = renderer.measureText(title, Style::fontSizeHero, FontWeight::SemiBold).width;
      const auto subtitleWidth = renderer.measureText(subtitle, Style::fontSizeCaption, FontWeight::Normal).width;
      glyph(root, (w - Style::spaceWide) / 2, center - 80, Style::glyphSizeHero, "loader", accent);
      label(
          root, (w - titleWidth) / 2, center - Style::spaceXl, titleWidth + Style::spaceXxs, title, Style::fontSizeHero,
          foreground, FontWeight::SemiBold
      );
      label(
          root, (w - subtitleWidth) / 2, center + 18, subtitleWidth + Style::spaceXxs, subtitle, Style::fontSizeCaption,
          muted
      );
      button(root, {(w - 96) / 2, center + 62, 96, 32, Action::CancelLogin, {}}, "Cancel");
    } else if (!m_connected) {
      const float extra = m_retrySavedLogin && !m_connecting ? 42 : 0;
      const float x = (w - 460) / 2, y = std::max(62.0F, (h - 360 - extra) / 2);
      rectangle(root, x, y, 460, 360 + extra, sidebar, 22);
      label(
          root, x + Style::space2Xl, y + Style::spaceWide, 390, "A LITTLE CLOSER TO YOUR PEOPLE", Style::fontSizeDetail,
          accent, FontWeight::SemiBold
      );
      label(root, x + Style::space2Xl, y + 58, 390, "Welcome back.", 32, foreground, FontWeight::Bold);
      label(
          root, x + Style::space2Xl, y + 106, 390, "Connect your Discord account and settle in.", Style::fontSizeBody,
          muted
      );
      if (m_connecting) {
        label(root, x + Style::space2Xl, y + 163, 396, "Connecting to Discord…", Style::fontSizeTitle, foreground);
        button(root, {x + Style::space2Xl, y + 222, 190, 38, Action::CancelLogin, {}}, "Cancel");
        button(root, {x + 238, y + 222, 190, 38, Action::Demo, {}}, "Explore demo");
        label(
            root, x + Style::space2Xl, y + 291, 396, m_status, Style::fontSizeDescription, muted, FontWeight::Normal, 3
        );
      } else if (!m_showTokenEntry) {
        label(
            root, x + Style::space2Xl, y + 151, 390, "Sign in using Discord in your default browser.",
            Style::fontSizeCaption, muted, FontWeight::Normal, 2
        );
        button(
            root, {x + Style::space2Xl, y + 200, 396, 46, Action::DiscordLogin, {}},
            m_login.active() ? "Waiting for your browser…" : "Sign in with Discord", true
        );
        if (m_retrySavedLogin)
          button(root, {x + Style::space2Xl, y + 252, 396, 32, Action::RetryLogin, {}}, "Retry saved sign-in");
        button(root, {x + Style::space2Xl, y + 260 + extra, 190, 38, Action::Demo, {}}, "Explore demo");
        button(
            root, {x + 238, y + 260 + extra, 190, 38, m_login.active() ? Action::CancelLogin : Action::ToggleToken, {}},
            m_login.active() ? "Cancel sign-in" : "Use a token instead"
        );
        label(
            root, x + Style::space2Xl, y + 316 + extra, 396,
            m_status.empty() ? "Sign in on Discord's own page. Your password stays there." : m_status,
            Style::fontSizeMini, muted, FontWeight::Normal, 2
        );
      } else {
        label(
            root, x + Style::space2Xl, y + 151, 390, "Account token", Style::fontSizeDescription, foreground,
            FontWeight::Medium
        );
        inputControl(
            root, x + Style::space2Xl, y + 177, 396, 48, "token", m_token, "Paste your account token", false, true
        );
        button(
            root, {x + Style::space2Xl, y + 241, 234, 44, Action::Connect, {}},
            m_connecting ? "Connecting…" : "Connect account", true
        );
        button(root, {x + 278, y + 241, 150, 44, Action::ToggleToken, {}}, "Back to sign-in");
        label(
            root, x + Style::space2Xl, y + 306, 396,
            m_status.empty() ? "Sign-in is remembered in your desktop keyring." : m_status, Style::fontSizeDescription,
            m_status.empty() ? muted : danger, FontWeight::Normal, 2
        );
      }
    } else if (!settingsVisible()) {
      rectangle(root, 0, layout::Window::titlebarHeight, w, h - layout::Window::titlebarHeight, panel);
      const auto account = layout::accountFrame(h, chatLeft());
      const auto railFrame = layout::floatingFrame(
          0, layout::Window::titlebarHeight, layout::Sidebar::railWidth,
          account.y - layout::Floating::inset - layout::Window::titlebarHeight
      );
      const float sidebarBottom = railFrame.y + railFrame.height;
      floatingPanel(root, railFrame, theme::railBackground);
      const float sidebarWidth = channelSidebarWidth();
      floatingPanel(
          root,
          layout::floatingFrame(
              layout::Sidebar::railWidth, layout::Window::titlebarHeight, sidebarWidth,
              account.y - layout::Floating::inset - layout::Window::titlebarHeight
          ),
          background
      );
      std::string guildName = m_dmMode ? "Direct messages" : "Your servers";
      for (const auto& guild : m_guilds)
        if (guild.id == m_guildId)
          guildName = guild.name;
      float guildBadgeWidth = 0;
      if (!m_dmMode) {
        if (const auto at = std::ranges::find(m_guilds, m_guildId, &api::Guild::id); at != m_guilds.end())
          guildBadgeWidth = drawGuildBadge(root, 98, 59, api::guildBadge(*at));
      }
      if (m_dmMode) {
        auto& search = inputControl(
            root, layout::Sidebar::railWidth + layout::DmSidebar::inset, layout::DmSidebar::searchTop,
            sidebarWidth - layout::DmSidebar::inset * 2, layout::DmSidebar::searchHeight, "dm-search", m_dmSearch,
            "Find or start a conversation"
        );
        search.setFontSize(Style::fontSizeCaption);
        search.setHorizontalPadding(Style::spaceMd);
        search.setFrameRadius(Style::controlRadius);
      } else
        label(
            root, 98 + guildBadgeWidth, 57, sidebarWidth - 60 - guildBadgeWidth, guildName, Style::fontSizeSection,
            foreground, FontWeight::SemiBold
        );
      if (!m_dmMode && !m_guildId.empty()) {
        glyph(root, chatLeft() - Style::space2Xl, 61, Style::baseGlyphSize, "chevron-down", muted);
        Hit header{
            88,
            layout::Sidebar::headerTop,
            sidebarWidth - Style::spaceRelaxed,
            layout::Sidebar::headerHeight,
            Action::GuildMenu,
            m_guildId
        };
        m_hits.push_back(header);
        m_contextTargets.push_back(header);
      }
      if (m_dmMode) {
        for (const auto& [page, title, icon, y] : std::vector<std::tuple<std::string, std::string, std::string, float>>{
                 {"friends", "Friends", "users", layout::DmSidebar::friendsTop},
                 {"requests", "Message Requests", "mail", layout::DmSidebar::requestsTop}
             }) {
          const bool selected = m_dmPage == page;
          if (selected || hovered(90, y, sidebarWidth - Style::spaceXl, layout::DmSidebar::rowHeight))
            rectangle(
                root, 90, y, sidebarWidth - Style::spaceXl, layout::DmSidebar::rowHeight, selected ? raised : panel,
                Style::radiusMd
            );
          glyph(root, 102, y + Style::spaceSm, Style::glyphSizeLarge, icon, selected ? foreground : muted);
          label(
              root, 136, y + Style::spaceDense, sidebarWidth - 94, title, Style::fontSizeCaption,
              selected ? foreground : muted, FontWeight::Medium
          );
          if (page == "requests")
            countBadge(
                root, chatLeft() - (Style::spaceXl - Style::spaceXxs), y + (Style::spaceSm + Style::borderWidth),
                messageRequestCount(), false, "pending message request"
            );
          m_hits.push_back({90, y, sidebarWidth - Style::spaceXl, layout::DmSidebar::rowHeight, Action::DmPage, page});
        }
      }
      constexpr float dmHeadingTop = layout::DmSidebar::headingTop, dmHeadingHeight = layout::DmSidebar::headingHeight;
      const auto dmHeadingMetrics =
          m_window.target().renderer().measureText("DIRECT MESSAGES", Style::fontSizeDetail, FontWeight::SemiBold);
      const float dmHeadingY = dmHeadingTop + (dmHeadingHeight - (dmHeadingMetrics.bottom - dmHeadingMetrics.top)) / 2;
      if (m_dmMode)
        label(
            root, 98, dmHeadingY, sidebarWidth - Style::spaceGroup, "DIRECT MESSAGES", Style::fontSizeDetail, muted,
            FontWeight::SemiBold
        );
      drawDirectMessagesButton(root);
      countBadge(
          root, layout::ServerRail::badgeRight,
          layout::ServerRail::homeTop + layout::ServerRail::badgeTop(layout::ServerRail::rowHeight),
          messageRequestCount(), true, "pending message request"
      );

      drawServerRail(root, sidebarBottom);
      auto listedChannels = m_channels;
      if (m_dmMode && !m_dmSearch.empty())
        std::erase_if(listedChannels, [&](const auto& channel) { return !dmMatches(channelName(channel)); });
      const auto visibleChannels = channelListRows(
          listedChannels, m_collapsedCategories, m_channelId, m_channelListPreferences,
          [&](const auto& id) { return channelMuted(id); },
          [&](const auto& id) { return m_unreadTimeline.mentions(id); },
          [&](const auto& channel) { return m_dmMode || serverChannelVisible(channel); }
      );
      if (m_dmMode)
        iconButton(
            root,
            {chatLeft() - (Style::spaceSection - Style::spaceXxs), dmHeadingTop, 26, dmHeadingHeight, Action::DmAction,
             "create"},
            "plus", "New message"
        );
      const float channelTop = m_dmMode ? layout::DmSidebar::channelsTop : layout::Sidebar::channelsTop;
      const float accountTop = account.y;
      const float channelListHeight =
          visibleChannels.empty() ? 0 : visibleChannels.back().top + visibleChannels.back().advance;
      m_scrolling.limit(
          m_channelScroll, std::max(0.0F, channelListHeight - (accountTop - channelTop - Style::cardPadding))
      );
      if (!m_dmMode)
        m_contextTargets.push_back(
            {layout::Sidebar::railWidth,
             layout::Sidebar::channelsTop - Style::spaceXs,
             sidebarWidth,
             std::max(0.0F, sidebarBottom - layout::Sidebar::channelsTop + Style::spaceXs),
             Action::ChannelListMenu,
             {}}
        );
      int mentionsBelow = 0;
      std::string nextMention;
      for (const auto& row : visibleChannels) {
        const float y = channelTop + row.top - m_channelScroll;
        const auto& channel = *row.channel;
        if (channel.type != 4 && y + row.height > sidebarBottom && m_unreadTimeline.mentions(channel.id) > 0) {
          mentionsBelow = std::min(1000000, mentionsBelow + m_unreadTimeline.mentions(channel.id));
          if (nextMention.empty())
            nextMention = channel.id;
        }
        if (y < channelTop - Style::spaceXs || y + row.height > sidebarBottom)
          continue;
        const auto rowContent = [&](bool dim) -> Node& {
          if (!dim)
            return root;
          auto content = std::make_unique<Node>();
          content->setSize(w, h);
          content->setHitTestVisible(false);
          content->setOpacity(Style::mutedChannelOpacity);
          return *root.addChild(std::move(content));
        };
        if (channel.type == 4) {
          const float categoryX = layout::Sidebar::railWidth + Style::spaceMd;
          const float categoryWidth = chatLeft() - Style::spaceSm - categoryX - Style::spaceXs;
          const float labelX = categoryX + Style::baseGlyphSize + Style::spaceXxs;
          auto& content = rowContent(channelMuted(channel.id) && !hovered(categoryX, y, categoryWidth, row.height));
          if (hovered(categoryX, y, categoryWidth, row.height))
            rectangle(root, categoryX, y, categoryWidth, row.height, panel, Style::radiusMd);
          glyph(
              content, categoryX, y + (row.height - Style::baseGlyphSize) / 2, Style::baseGlyphSize,
              m_collapsedCategories.contains(channel.id) ? "chevron-right" : "chevron-down", muted
          );
          label(
              content, labelX, y + (Style::spaceXs + Style::borderWidth),
              chatLeft() - labelX - layout::Sidebar::rowInset, channel.name, Style::fontSizeCaption, muted,
              FontWeight::Bold
          );
          m_hits.push_back({categoryX, y, categoryWidth, row.height, Action::Category, channel.id});
          m_contextTargets.push_back({categoryX, y, categoryWidth, row.height, Action::ChannelMenu, channel.id});
          continue;
        }
        const bool selected = channel.id == m_channelId;
        const int mentions = m_unreadTimeline.mentions(channel.id);
        const bool unread = (m_unreadChannels.contains(channel.id) || m_unreadDMs.contains(channel.id))
            && (((m_dmMode || !m_userDirectory.muted(m_guildId)) && !channelMuted(channel.id)) || mentions > 0);
        const float rowX = layout::Sidebar::railWidth + layout::Sidebar::rowInset;
        const float rowWidth = sidebarWidth - layout::Sidebar::rowInset * 2;
        const bool over =
            hovered(rowX, y, rowWidth, row.height) && (m_dmMode || (sidebarResizeAvailable() && !m_resizingSidebar));
        const float hoverShift = !m_dmMode && over ? Style::spaceXs : 0;
        auto& content = rowContent(!m_dmMode && channelMuted(channel.id) && !selected && !over && mentions == 0);
        constexpr float channelHoverTint = .06F;
        const auto rowSurface = m_dmMode ? (selected   ? raised
                                                : over ? panel
                                                       : background)
            : over                       ? lerpColor(selected ? raised : background, foreground, channelHoverTint)
            : selected                   ? raised
                                         : background;
        if (selected || over)
          rectangle(root, rowX, y, rowWidth, row.height, rowSurface, Style::surfaceRadius);
        if (unread && !selected)
          rectangle(
              root, layout::Sidebar::unreadX, y + (row.height - layout::Sidebar::unreadHeight) / 2,
              layout::Sidebar::unreadWidth, layout::Sidebar::unreadHeight, foreground, Style::radiusXs
          );
        if (m_dmMode) {
          if (channel.type == 1 && !channel.recipients.empty())
            drawNameplate(
                root, {rowX, y, rowWidth, row.height}, channel.recipients.front().id, "",
                NameplatePlacement::Conversation, selected
            );
          avatar(
              root, 99, y + (row.height - layout::Avatar::dm) / 2, layout::Avatar::dm, channel.icon,
              channelName(channel),
              channel.type == 1 && !channel.recipients.empty() ? channel.recipients.front().id : "", "",
              layout::Frame{rowX, y, rowWidth, row.height}
          );
          if (channel.type == 1 && !channel.recipients.empty())
            statusBadge(
                root, 119, y + (row.height - layout::Avatar::dm) / 2 + Style::spaceRelaxed - Style::spaceXxs,
                userStatus(channel.recipients.front().id),
                selected   ? raised
                    : over ? panel
                           : background,
                channel.recipients.front().id, channel.id
            );
        } else {
          const float size = channel.isThread() ? layout::Sidebar::threadIconSize : layout::Sidebar::iconSize;
          const float x = layout::Sidebar::railWidth
              + layout::Sidebar::rowInset * 2
              + (channel.isThread() ? layout::Sidebar::threadIconSize : 0);
          if (channel.isThread()) {
            const float branchX = x - layout::Sidebar::threadIconSize;
            rectangle(
                content, branchX, y, layout::Sidebar::threadBranchStroke, layout::Sidebar::threadBranchHeight, muted
            );
            rectangle(
                content, branchX, y + layout::Sidebar::threadBranchHeight - layout::Sidebar::threadBranchStroke,
                layout::Sidebar::threadBranchWidth, layout::Sidebar::threadBranchStroke, muted
            );
          }
          channelIcon(
              content, x + hoverShift, y + (row.height - size) / 2, size, channel, selected ? accent : muted, rowSurface
          );
        }
        m_hits.push_back({rowX, y, rowWidth, row.height, Action::Channel, channel.id});
        const bool pinned = m_dmMode && m_userDirectory.pinnedDm(channel.id);
        const bool showPin = m_dmMode && (pinned || over);
        const float badgeWidth = countBadge(
            root, chatLeft() - (Style::spaceXl - Style::spaceXxs) - (showPin ? 26 : 0),
            y + (row.height - layout::CountBadge::height) / 2, mentions
        );
        if (m_dmMode) {
          const float right =
              chatLeft() - Style::spaceRelaxed - (showPin ? 28 : 0) - (badgeWidth ? badgeWidth + Style::spaceSm : 0);
          std::optional<api::GuildTag> tag;
          if (channel.type == 1 && !channel.recipients.empty()) {
            const auto& user = channel.recipients.front();
            tag = m_userDirectory.guildTag(user.id, user.guildTag);
          }
          const float tagWidth = tag ? std::min(
                                           104.0F,
                                           std::ceil(m_window.target()
                                                         .renderer()
                                                         .measureText(tag->tag, Style::fontSizeMini, FontWeight::Medium)
                                                         .width)
                                               + 32
                                       )
                                     : 0;
          const auto weight = selected || unread ? FontWeight::SemiBold : FontWeight::Normal;
          const float available = std::max(1.0F, right - 136);
          const bool tagFits = tag && available >= tagWidth + (Style::spaceSection - Style::spaceXxs);
          const float nameWidth = std::max(1.0F, available - (tagFits ? tagWidth + Style::spaceCompact : 0));
          const auto name = channelName(channel);
          const auto custom = channel.kind() == api::ChannelType::DirectMessage && !channel.recipients.empty()
              ? m_userDirectory.customStatus(channel.recipients.front().id)
              : api::UserCustomStatus{};
          const float identityHeight = custom.empty() ? row.height : layout::DmSidebar::identityHeight;
          const float identityY = y + (custom.empty() ? 0 : layout::DmSidebar::contentInset);
          const auto metrics =
              m_window.target().renderer().measureText(name, Style::fontSizeBody, weight, nameWidth, 1);
          label(
              root, 136, identityY + (identityHeight - (metrics.bottom - metrics.top)) / 2, nameWidth, name,
              Style::fontSizeBody, selected || over || unread ? foreground : muted, weight
          );
          if (!custom.empty())
            drawCustomStatus(
                root, {136, identityY + identityHeight, available, layout::DmSidebar::statusHeight}, custom,
                Style::fontSizeMini
            );
          if (tagFits) {
            const float actualName = std::min(
                nameWidth, std::ceil(m_window.target().renderer().measureText(name, Style::fontSizeBody, weight).width)
            );
            drawGuildTag(
                root, 136 + actualName + Style::spaceCompact, identityY + (identityHeight - Style::spaceRelaxed) / 2,
                tagWidth, tag
            );
          }
          if (showPin)
            iconButton(
                root,
                {chatLeft() - ((Style::spaceSection + Style::spaceXxs) + Style::borderWidth),
                 y + (row.height - layout::DmSidebar::pinSize) / 2, layout::DmSidebar::pinSize,
                 layout::DmSidebar::pinSize, m_dmPinRequests.contains(channel.id) ? Action::None : Action::DmAction,
                 std::string(pinned ? "unpin:" : "pin:") + channel.id},
                pinned
                        && hovered(
                            chatLeft() - ((Style::spaceSection + Style::spaceXxs) + Style::borderWidth),
                            y + (row.height - layout::DmSidebar::pinSize) / 2, layout::DmSidebar::pinSize,
                            layout::DmSidebar::pinSize
                        )
                    ? "pinned-off"
                    : pinned ? "pinned-filled"
                             : "pin",
                m_dmPinRequests.contains(channel.id) ? "Saving pin…"
                    : pinned                         ? "Unpin conversation"
                                                     : "Pin to top"
            );
        } else {
          const float labelX = layout::Sidebar::railWidth
              + layout::Sidebar::rowInset * 2
              + (channel.isThread() ? layout::Sidebar::threadIconSize * 2 + Style::spaceSm
                                    : layout::Sidebar::iconSize + Style::spaceCompact)
              + hoverShift;
          const float labelRight = rowX + rowWidth - layout::Sidebar::rowInset;
          const float labelWidth = std::max(0.0F, labelRight - labelX - (badgeWidth ? badgeWidth + Style::spaceSm : 0));
          const auto weight = selected || unread ? FontWeight::SemiBold : FontWeight::Normal;
          const auto metrics =
              m_window.target().renderer().measureText(channel.name, Style::fontSizeBody, weight, labelWidth, 1);
          label(
              content, labelX, y + (row.height - (metrics.bottom - metrics.top)) / 2, labelWidth, channel.name,
              Style::fontSizeBody, selected || over || unread ? foreground : muted, weight
          );
        }
        m_contextTargets.push_back(
            {rowX, y, rowWidth, row.height, m_dmMode ? Action::ConversationMenu : Action::ChannelMenu, channel.id}
        );
      }
      if (mentionsBelow > 0) {
        const auto text = (mentionsBelow > 99 ? "99+" : std::to_string(mentionsBelow))
            + std::string(mentionsBelow == 1 ? " NEW MENTION" : " NEW MENTIONS");
        const auto metrics = m_window.target().renderer().measureText(text, Style::fontSizeMini, FontWeight::Bold);
        const float width = std::ceil(metrics.width) + 24,
                    x = layout::Sidebar::railWidth + sidebarWidth / 2 - width / 2,
                    y = accountTop - (Style::spaceSection - Style::spaceXxs);
        rectangle(
            root, x - (Style::spaceXxs + Style::borderWidth), y - (Style::spaceXxs + Style::borderWidth),
            width + Style::spaceCompact, 34, sidebar, 17
        );
        rectangle(
            root, x, y, width, 28, hovered(x, y, width, 28) ? lerpColor(destructive, background, .12F) : destructive, 14
        );
        label(
            root, x + Style::spaceMd, y + (28 - (metrics.bottom - metrics.top)) / 2, width - Style::spaceXl, text,
            Style::fontSizeMini, rgbHex(0xFFFFFF), FontWeight::Bold
        );
        m_hits.push_back({x, y, width, 28, Action::JumpChannelMention, nextMention});
      }
      if (m_channels.empty())
        label(
            root, 100, channelTop + (Style::spaceSm + Style::borderWidth), sidebarWidth - Style::spaceGroup,
            (m_dmMode ? m_loadingPrivate : m_loadingChannels) ? "Loading…"
                : m_dmMode                                    ? "No conversations yet."
                                                              : "No text channels to show.",
            Style::fontSizeCaption, muted, FontWeight::Normal, 2
        );
      floatingPanel(
          root, {account.x, account.y, account.width, account.height},
          hovered(account.x, account.y, account.width, account.height) ? panel : background
      );
      drawNameplate(
          root, {account.x, account.y, account.width, account.height}, m_account.id, "", NameplatePlacement::Account
      );
      avatar(
          root, account.avatarX, account.avatarY, layout::Avatar::account, m_account.avatar, m_account.name,
          m_account.id, "", layout::Frame{account.x, account.y, account.width, account.height}
      );
      rectangle(
          root, account.avatarX + layout::Avatar::account - layout::Sidebar::accountStatusSize + Style::borderWidth,
          account.avatarY + layout::Avatar::account - layout::Sidebar::accountStatusSize + Style::borderWidth,
          layout::Sidebar::accountStatusSize, layout::Sidebar::accountStatusSize, presenceColor(m_presence.status),
          Style::radiusMd
      );
      const auto accountDetail = !m_live && !m_demo ? m_gatewayStatus
          : !m_presence.text.empty()                ? m_presence.text
                                                    : std::string(presenceName(m_presence.status));
      const auto accountNameMetrics =
          m_window.target().renderer().measureText(m_account.name, Style::fontSizeCaption, FontWeight::Medium);
      const auto accountDetailMetrics = m_window.target().renderer().measureText(accountDetail, Style::fontSizeDetail);
      const float accountNameHeight =
          std::max(Style::fontSizeCaption + Style::spaceXs, accountNameMetrics.bottom - accountNameMetrics.top);
      const float accountDetailHeight =
          std::max(Style::fontSizeDetail + Style::spaceXs, accountDetailMetrics.bottom - accountDetailMetrics.top);
      const float accountTextTop =
          accountTop + layout::Sidebar::accountOffset(accountNameHeight + accountDetailHeight + Style::spaceXxs);
      label(
          root, account.nameX, accountTextTop, account.nameWidth, m_account.name, Style::fontSizeCaption, foreground,
          FontWeight::Medium
      );
      label(
          root, account.nameX, accountTextTop + accountNameHeight + Style::spaceXxs, account.nameWidth, accountDetail,
          Style::fontSizeDetail, muted
      );
      m_hits.push_back({account.x, account.y, account.presenceWidth, account.height, Action::TogglePresence, {}});
      iconButton(
          root,
          {account.settingsX,
           account.settingsY,
           layout::Sidebar::accountSettingsSize,
           layout::Sidebar::accountSettingsSize,
           Action::Settings,
           {}},
          "settings", "User settings"
      );

      const bool forum = forumView();
      const float panelWidth = m_channelPanel.empty() ? 0 : channelPanelWidth();
      const float memberWidth = memberPaneWidth();
      const float left = chatLeft(), bodyWidth = w - left - memberWidth - panelWidth;
      if (m_messageEdit
          && (m_messageEdit->channel != m_channelId
              || std::ranges::find(m_messages, m_messageEdit->message, &api::Message::id) == m_messages.end()))
        cancelMessageEdit(false);
      if (!m_dmMode || m_dmPage.empty())
        drawChannelHeader(root, w);
      else
        floatingPanel(
            root, layout::floatingFrame(left, layout::Window::titlebarHeight, w - left, layout::ChannelHeader::height),
            background
        );
      if ((m_dmMode && !m_dmPage.empty()) || forum)
        floatingPanel(
            root,
            layout::floatingFrame(
                left, layout::Window::titlebarHeight + layout::ChannelHeader::height, bodyWidth,
                h - layout::Window::titlebarHeight - layout::ChannelHeader::height
            ),
            background
        );
      if (m_dmMode && !m_dmPage.empty())
        drawDmHome(root, w, h);
      else if (forum)
        drawForum(root, left, 102, bodyWidth, h - 102);
      else {
        const auto& draft = m_drafts[m_channelId];
        m_edit.clamp(draft);
        m_draftWidth = std::max(32.0F, bodyWidth - 222);
        m_draftOffsets = TextEdit::stops(draft);
        const TextPresentation draftPresentation(draft, composerReplacements(draft));
        std::erase_if(m_draftOffsets, [&](const auto byte) {
          const auto* replacement = draftPresentation.replacementAt(byte);
          return replacement && byte != replacement->begin;
        });
        auto displayOffsets = m_draftOffsets;
        for (auto& byte : displayOffsets)
          byte = draftPresentation.displayOffset(byte);
        m_window.target().renderer().measureTextCursorStopsWrapped(
            draftPresentation.text(), Style::fontSizeBody, displayOffsets, m_draftWidth, m_draftStops
        );
        const float fullDraftHeight = m_draftStops.empty()
            ? layout::Composer::minimumTextHeight
            : m_draftStops.back().y + std::max(layout::Composer::minimumTextHeight, m_draftStops.back().height);
        m_draftHeight = std::clamp(fullDraftHeight, layout::Composer::minimumTextHeight, std::min(160.0F, h * 0.3F));
        const float uploadHeight = m_attachments[m_channelId].empty() ? 0 : layout::DraftAttachment::composerReserve;
        const float stickerHeight = m_draftStickers[m_channelId].empty() ? 0 : 100;
        const float attachmentHeight = uploadHeight + stickerHeight;
        const float replyHeight = m_replies.contains(m_channelId) ? layout::ComposerReply::height : 0;
        const float composerHeight = m_draftHeight + layout::Composer::verticalPadding + attachmentHeight + replyHeight;
        const auto composer = layout::composerFrame(h, composerHeight);
        const float composerY = composer.top;
        float top = m_unreadTimeline.unread() ? layout::Chat::unreadTimelineTop : layout::Chat::timelineTop;
        constexpr float timelineTopPadding = layout::Chat::topPadding,
                        timelineBottomPadding = layout::Chat::bottomPadding;
        const auto typing = typingText();
        // Reserve typing space inside the conversation panel, keeping its bottom
        // aligned with the sidebar even when nobody is typing.
        const float bottom = composer.viewportBottom, textWidth = std::max(32.0F, bodyWidth - 104);
        const float panelTop = layout::Window::titlebarHeight + layout::ChannelHeader::height + layout::Floating::inset;
        floatingPanel(
            root,
            {left + layout::Floating::inset, panelTop, std::max(0.0F, bodyWidth - layout::Floating::gap),
             std::max(0.0F, composer.panelBottom - panelTop)},
            background
        );
        const MessageMeasure measure = [&](std::string_view text, const MessageStyle& style) {
          return m_window.target()
              .renderer()
              .measureText(
                  messageMarkup(text, style), style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
                  TextEllipsize::End, true
              )
              .width;
        };
        auto content = std::make_unique<Node>();
        content->setPosition(left + Style::spaceXl, top);
        content->setSize(bodyWidth - Style::spaceGroup, std::max(0.0F, bottom - top));
        content->setClipChildren(true);
        std::vector<api::Message> outgoingTimeline;
        const auto* rows = &m_messages;
        for (const auto& [_, pending] : m_pendingSends)
          if (pending.preview.channelId == m_channelId && pending.confirmed.empty() && m_history.atLatest()) {
            if (rows == &m_messages) {
              outgoingTimeline = m_messages;
              rows = &outgoingTimeline;
            }
            outgoingTimeline.push_back(pending.preview);
          }
        const auto& timeline = *rows;
        std::vector<float> heights;
        std::vector<bool> continuations(timeline.size(), false);
        std::vector<const MessageLayout*> layouts;
        const auto ownRoles =
            m_dmMode ? std::optional<std::vector<std::string>>{} : m_roleDirectory.memberships(m_guildId, m_account.id);
        auto unreadIndex = m_unreadTimeline.first(m_messages);
        constexpr float unreadHeight = 36;
        const float introHeight = channelIntro(nullptr, 0, 0, textWidth);
        const auto continues = [&](std::size_t i) {
          return i > 0
              && !blockedMessage(timeline[i - 1])
              && !blockedMessage(timeline[i])
              && continuesMessageGroup(
                     timeline[i - 1], timeline[i],
                     timeline[i].id.starts_with("pending:") && m_pendingSends.contains(timeline[i].nonce)
              );
        };
        for (std::size_t i = 0; i < timeline.size(); ++i)
          continuations[i] = (!unreadIndex || i != *unreadIndex) && continues(i);
        std::map<std::string_view, const api::Message*> replySources;
        for (const auto& message : timeline)
          replySources.emplace(message.id, &message);
        for (std::size_t i = 0; i < timeline.size(); ++i) {
          const auto& message = timeline[i];
          auto& cached = m_messageLayouts[message.id];
          const auto& renderer = m_window.target().renderer();
          auto reply = message.reply;
          if (reply) {
            const auto source = replySources.find(reply->message);
            const auto& channel = reply->channel.empty() ? message.channelId : reply->channel;
            *reply = resolvedReply(
                *reply, source == replySources.end() ? nullptr : source->second,
                m_messageLog.known(channel, reply->message)
            );
          }
          if (cached.width != textWidth
              || cached.scale != renderer.renderScale()
              || cached.fonts != renderer.textMetricsGeneration()
              || cached.replySource != reply) {
            auto displayed = message;
            if (message.type == autoModerationActionType)
              displayed.author = userName(message.authorId, message.author);
            for (const auto& member : m_members)
              if (!member.user.id.empty())
                displayed.mentions.try_emplace(member.user.id, member.user.name);
            displayed.mentions.try_emplace(m_account.id, m_account.name);
            for (auto& [id, name] : displayed.mentions)
              name = userName(id, name);
            if (!m_preferences.showEmbeds)
              displayed.embeds.clear();
            if (!m_preferences.showImages)
              for (auto& attachment : displayed.attachments)
                attachment.image.url.clear();
            if (displayed.forwarded) {
              if (!m_preferences.showEmbeds)
                displayed.forwarded->embeds.clear();
              if (!m_preferences.showImages)
                for (auto& attachment : displayed.forwarded->attachments)
                  attachment.image.url.clear();
              for (auto& [id, name] : displayed.forwarded->mentions)
                name = userName(id, name);
            }
            if (message.type == 7) {
              const auto notice = joinNotice(message);
              const auto authorId = api::validSnowflake(message.authorId) ? message.authorId : "1";
              const auto text = std::string(notice.before) + "<@" + authorId + ">" + std::string(notice.after);
              cached.layout = layoutMessage(
                  text, std::max(32.0F, textWidth - Style::spaceSection), measure, m_preferences.messageSize,
                  {{authorId, userName(message.authorId, message.author)}},
                  {.compact = true,
                   .unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); },
                   .mentionPrefix = false}
              );
              if (cached.layout.plainText.ends_with('\n'))
                cached.layout.plainText.pop_back();
            } else {
              const auto& versions = m_messageLog.edits(m_channelId, message.id);
              if (!versions.empty() && displayed.editedTimestamp.empty())
                displayed.editedTimestamp = "retained";
              cached.layout = layoutRichMessage(
                  displayed, textWidth, measure, m_preferences.messageSize,
                  {.emojiImageLinks = m_preferences.emojiImageLinks && m_preferences.showImages,
                   .compact = m_preferences.compact,
                   .unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); },
                   .channelMention = [this](auto guild, auto channel) { return channelMention(guild, channel); },
                   .roleMention = [this](
                                      auto role
                                  ) { return m_roleDirectory.roleName(m_dmMode ? "" : m_guildId, std::string(role)); }}
              );
              cached.layout = withEditHistory(
                  std::move(cached.layout), versions, textWidth, measure, m_preferences.messageSize,
                  {.compact = m_preferences.compact,
                   .unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); },
                   .channelMention = [this](auto guild, auto channel) { return channelMention(guild, channel); },
                   .roleMention = [this](
                                      auto role
                                  ) { return m_roleDirectory.roleName(m_dmMode ? "" : m_guildId, std::string(role)); }}
              );
            }
            cached.reply = reply
                ? layoutReplyPreview(
                      *reply, userName(reply->authorId, reply->author.empty() ? "message" : reply->author), textWidth,
                      measure,
                      {.emojiImageLinks = m_preferences.emojiImageLinks,
                       .compact = true,
                       .unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); },
                       .roleMention =
                           [this](auto role) {
                             return m_roleDirectory.roleName(m_dmMode ? "" : m_guildId, std::string(role));
                           }}
                  )
                : MessageLayout{};
            cached.replySource = std::move(reply);
            cached.width = textWidth;
            cached.scale = renderer.renderScale();
            cached.fonts = renderer.textMetricsGeneration();
          }
          const float bodyHeight = m_messageEdit && m_messageEdit->message == message.id
              ? messageEditHeight(textWidth)
              : std::max(22.0F, cached.layout.height);
          const float height = blockedMessage(message)                        ? 36
              : message.type == 7 || message.type == autoModerationActionType ? std::max(22.0F, cached.layout.height)
                  + (i + 1 == timeline.size()    ? 0
                         : m_preferences.compact ? std::min(8, m_preferences.messageGroupSpacing)
                                                 : m_preferences.messageGroupSpacing)
                  + (!m_preferences.showReactions || message.reactions.empty()
                         ? 0
                         : 6 + reactionStrip(nullptr, message, 0, 0, textWidth, true))
                                                                              : bodyHeight
                  + messageRowPadding(continuations[i], i + 1 < continuations.size() && continuations[i + 1],
                                      m_preferences.compact, m_preferences.messageGroupSpacing,
                                      i + 1 == timeline.size())
                  + (message.reply ? layout::Reply::step : 0)
                  + messageFooterHeight(messageButtons(nullptr, message, 0, 0, textWidth),
                                        reactionStrip(nullptr, message, 0, 0, textWidth),
                                        threadStrip(nullptr, message, 0, 0, textWidth));
          layouts.push_back(&cached.layout);
          const auto pending = m_pendingSends.find(message.nonce);
          const bool failed = pending != m_pendingSends.end() && !pending->second.error.empty();
          heights.push_back(height + (failed ? layout::Chat::pendingStatusHeight : 0));
        }
        float totalHeight = timelineTopPadding + timelineBottomPadding + introHeight;
        for (const auto height : heights)
          totalHeight += height;
        if (unreadIndex)
          totalHeight += unreadHeight;
        m_messageMaxScroll = std::max(0.0F, totalHeight - (bottom - top));
        if ((m_messageScroll > 1
             || m_scrolling.target(m_messageScroll) > 1
             || !m_history.atLatest()
             || m_keepHistoryAnchor)
            && !m_unreadTimeline.positionPending()) {
          std::vector<TimelineAnchor::Row> positions;
          float rowY = (bottom - totalHeight) + m_messageScroll + timelineTopPadding + introHeight;
          for (std::size_t i = 0; i < timeline.size(); ++i) {
            if (unreadIndex && i == *unreadIndex)
              rowY += unreadHeight;
            positions.push_back({timeline[i].id, rowY});
            rowY += heights[i];
          }
          if (const auto correction = m_timelineAnchor.correction(positions, m_messageScroll))
            m_scrolling.rebase(m_messageScroll, *correction);
        }
        m_scrolling.limit(m_messageScroll, m_messageMaxScroll);
        m_keepHistoryAnchor = false;
        if (!m_seekMessage.empty() && !m_loadingMessages) {
          bool foundTarget = false;
          float preceding = timelineTopPadding + introHeight;
          for (std::size_t i = 0; i < timeline.size(); ++i) {
            if (unreadIndex && i == *unreadIndex)
              preceding += unreadHeight;
            if (timeline[i].id == m_seekMessage) {
              foundTarget = true;
              m_scrolling.stop(m_messageScroll);
              m_messageScroll = std::clamp(
                  totalHeight - (bottom - top) - preceding + (bottom - top) * 0.35F, 0.0F, m_messageMaxScroll
              );
              if (m_jumpHighlightMessage == m_seekMessage && !m_jumpHighlightTimer.active())
                m_jumpHighlightTimer.start(layout::Chat::jumpHighlightDuration, [this] {
                  m_jumpHighlightMessage.clear();
                  m_window.invalidate();
                });
              break;
            }
            preceding += heights[i];
          }
          if (!foundTarget && m_jumpHighlightMessage == m_seekMessage)
            m_jumpHighlightMessage.clear();
          m_seekMessage.clear();
          m_unreadTimeline.positioned();
          m_timelineAnchor.reset();
        }
        if (m_unreadTimeline.positionPending() && !m_unreadTimeline.waiting() && !m_loadingMessages) {
          if (unreadIndex) {
            float preceding = timelineTopPadding + introHeight;
            for (std::size_t i = 0; i < *unreadIndex; ++i)
              preceding += heights[i];
            m_scrolling.stop(m_messageScroll);
            m_messageScroll = std::clamp(totalHeight - (bottom - top) - preceding, 0.0F, m_messageMaxScroll);
          }
          m_unreadTimeline.positioned();
        }
        // Clear the banner and divider in this frame, after opening unread history
        // at its boundary. Do not acknowledge while a wheel gesture is leaving bottom.
        if ((!m_demo || m_demoOverlay == "unread-cycle")
            && !m_unreadTimeline.waiting()
            && !m_unreadTimeline.positionPending()
            && !m_loadingMessages
            && readingChannel(m_channelId)
            && m_scrolling.target(m_messageScroll) <= 1
            && markChannelRead()) {
          if (unreadIndex && !m_unreadTimeline.unread()) {
            const auto i = *unreadIndex;
            if (continues(i) && !continuations[i]) {
              const float savedGap = m_preferences.compact ? std::min(8, m_preferences.messageGroupSpacing)
                                                           : m_preferences.messageGroupSpacing;
              continuations[i] = true;
              heights[i] -= 18;
              heights[i - 1] -= savedGap;
            }
            unreadIndex.reset();
            totalHeight = timelineTopPadding + timelineBottomPadding + introHeight;
            for (const auto height : heights)
              totalHeight += height;
          }
          if (!m_unreadTimeline.unread())
            top = layout::Chat::timelineTop;
          m_messageMaxScroll = std::max(0.0F, totalHeight - (bottom - top));
          m_scrolling.limit(m_messageScroll, m_messageMaxScroll);
        }
        m_messageViewportTop = top;
        m_messageViewportBottom = bottom;
        // Keep fractional content geometry while aligning final glyph positions
        // to physical pixels in both moving and settled frames.
        content->setTextScrollOffset(m_messageScroll);
        if (m_traceScroll)
          std::clog
              << "scroll frame surface=chat offset="
              << m_messageScroll
              << " target="
              << m_scrolling.target(m_messageScroll)
              << " maximum="
              << m_messageMaxScroll
              << " active="
              << m_scrolling.active(m_messageScroll)
              << '\n';
        content->setPosition(left + Style::spaceXl, top);
        content->setSize(bodyWidth - Style::spaceGroup, std::max(0.0F, bottom - top));
        std::vector<std::string> missingAuthors;
        float y = (bottom - top - totalHeight) + m_messageScroll + timelineTopPadding;
        if (introHeight > 0) {
          channelIntro(content.get(), 50, y, textWidth);
          y += introHeight;
        }
        for (std::size_t i = 0; i < timeline.size(); ++i) {
          const auto& message = timeline[i];
          const auto pending = m_pendingSends.find(message.nonce);
          const bool local = message.id.starts_with("pending:") && pending != m_pendingSends.end();
          const bool failed = local && !pending->second.error.empty();
          const bool editing = m_messageEdit && m_messageEdit->message == message.id;
          if (unreadIndex && i == *unreadIndex) {
            if (y + unreadHeight >= 0 && y < bottom - top) {
              const float width = bodyWidth - Style::spaceGroup;
              const float caption = m_window.target()
                                        .renderer()
                                        .measureText("New messages", Style::fontSizeDescription, FontWeight::SemiBold)
                                        .width;
              const float chipWidth = caption + 44, chipX = (width - chipWidth) / 2;
              const float sideWidth = std::max(0.0F, chipX - Style::spaceMd);
              for (int segment = 0; segment < 8; ++segment) {
                const float length = sideWidth / 8;
                const auto color = lerpColor(background, accent, 0.12F + segment * 0.065F);
                rectangle(
                    *content, segment * length, y + (Style::spaceLg + Style::borderWidth), length + 0.5F, 1, color
                );
                rectangle(
                    *content, width - (segment + 1) * length, y + (Style::spaceLg + Style::borderWidth), length + 0.5F,
                    1, color
                );
              }
              rectangle(
                  *content, chipX, y + (Style::spaceXs + Style::borderWidth), chipWidth, 25,
                  lerpColor(background, accent, 0.28F), 12.5F
              );
              rectangle(
                  *content, chipX + Style::borderWidth, y + Style::spaceCompact, chipWidth - Style::spaceXxs, 23,
                  lerpColor(background, accent, 0.08F), 11.5F
              );
              glyph(
                  *content, chipX + (Style::spaceDense + Style::borderWidth), y + Style::spaceDense,
                  Style::glyphSizeSmall, "sparkles", accent
              );
              label(
                  *content, chipX + Style::space2Xl, y + Style::spaceDense, caption + Style::spaceXxs, "New messages",
                  Style::fontSizeDescription, accent, FontWeight::SemiBold
              );
            }
            y += unreadHeight;
          }
          const float originalY = y;
          m_messageTops[message.id] = top + y;
          if (y + heights[i] >= 0 && y <= bottom - top) {
            auto rowNode = std::make_unique<Node>();
            auto* messageContent = rowNode.get();
            rowNode->setSize(content->width(), content->height());
            if (local)
              rowNode->setOpacity(Style::mutedChannelOpacity);
            content->addChild(std::move(rowNode));
            const auto firstHit = m_hits.size();
            const auto rowTop = std::max(top, top + y), rowBottom = std::min(bottom, top + y + heights[i]);
            if (!local)
              m_contextTargets.push_back(
                  {left + Style::spaceXl, rowTop, bodyWidth - Style::spaceGroup, std::max(0.0F, rowBottom - rowTop),
                   Action::MessageMenu, message.id}
              );
            if (blockedMessage(message)) {
              label(
                  *messageContent, 50, y + (Style::spaceCompact + Style::borderWidth), textWidth,
                  m_userDirectory.ignored(message.authorId) ? "Ignored message — click to show"
                                                            : "Blocked message — click to show",
                  Style::fontSizeDescription, muted
              );
              m_hits.push_back(
                  {left + Style::spaceXl, rowTop, bodyWidth - Style::spaceGroup, std::max(0.0F, rowBottom - rowTop),
                   Action::RevealBlocked, message.id}
              );
              y += heights[i];
              continue;
            }
            const float bottomGap = messageRowPadding(
                true, i + 1 < continuations.size() && continuations[i + 1], m_preferences.compact,
                m_preferences.messageGroupSpacing, i + 1 == timeline.size()
            );
            // Tags sit above the header baseline. The following group's margin
            // belongs to neither message's hover background or action target.
            const float hoverTop = std::max(top, top + y - (continuations[i] || message.type == 7 ? 0 : 4));
            // Even zero group spacing must leave the next header's upper edge
            // to that message, so a pointer can never highlight both rows.
            const bool nextHeader = i + 1 < timeline.size()
                && !continuations[i + 1]
                && timeline[i + 1].type != 7
                && !blockedMessage(timeline[i + 1]);
            const bool nextContinuation = i + 1 < continuations.size() && continuations[i + 1];
            // Give the last line a little breathing room without covering a
            // continuation or closing the gap before the next author header.
            const float hoverPadding = continuations[i] || nextContinuation ? 0.0F
                : i + 1 == timeline.size()                                  ? 4.0F
                                           : std::min(4.0F, std::max(0.0F, bottomGap - (nextHeader ? 6.0F : 2.0F)));
            const float hoverRowBottom =
                y + heights[i] - std::max(bottomGap, nextHeader ? Style::spaceXs : 0.0F) + hoverPadding;
            const float hoverBottom = std::min(bottom, top + hoverRowBottom);
            const bool messageHovered = !local
                && !editing
                && !m_inboxOpen
                && m_dialogKind.empty()
                && !m_contextOpen
                && !m_emojiMenu
                && !m_gifMenu
                && !m_presenceMenu
                && m_viewImage.empty()
                && !m_videoExpanded
                && (!m_completionQuery || !m_completionBounds.contains(m_pointerX, m_pointerY))
                && hovered(left, hoverTop, bodyWidth, std::max(0.0F, hoverBottom - hoverTop));
            if (messageHovered)
              rectangle(
                  root, left + Style::spaceDense, hoverTop, bodyWidth - (Style::space2Xl + Style::spaceXxs),
                  hoverBottom - hoverTop, lerpColor(background, foreground, theme::lightTheme ? .035F : .04F)
              );
            if (!m_dmMode
                && message.webhookId.empty()
                && !message.memberRoles
                && !m_roleDirectory.memberships(m_guildId, message.authorId)
                && api::validSnowflake(message.authorId)
                && missingAuthors.size() < 100
                && std::ranges::find(missingAuthors, message.authorId) == missingAuthors.end())
              missingAuthors.push_back(message.authorId);
            if (message.deleted) {
              const auto red = theme::danger;
              const float gap = messageRowPadding(
                  true, false, m_preferences.compact, m_preferences.messageGroupSpacing, i + 1 == timeline.size()
              );
              const float bandTop = std::max(top, top + y - Style::spaceXxs);
              const float bandBottom = std::min(bottom, top + y + heights[i] - gap + 2);
              if (bandBottom > bandTop) {
                rectangle(
                    root, left + Style::spaceDense, bandTop, bodyWidth - (Style::space2Xl + Style::spaceXxs),
                    bandBottom - bandTop, lerpColor(background, red, .07F)
                );
                rectangle(root, left + Style::spaceDense, bandTop, 3, bandBottom - bandTop, red);
              }
            }
            const auto drawJumpHighlight = [&] {
              if (m_jumpHighlightMessage != message.id || !m_jumpHighlightTimer.active())
                return;
              auto color = accent;
              color.a = layout::Chat::jumpHighlightOpacity;
              rectangle(
                  root, left + Style::spaceDense, hoverTop, bodyWidth - (Style::space2Xl + Style::spaceXxs),
                  std::max(0.0F, hoverBottom - hoverTop), color
              );
            };
            if (message.type == 7 || message.type == autoModerationActionType) {
              drawJumpHighlight();
              drawSystemNotice(*messageContent, message, *layouts[i], y, textWidth);
            } else {
              if (message.flags & 64) {
                rectangle(
                    *messageContent, 0, y - (Style::spaceXxs + Style::borderWidth), bodyWidth - Style::spaceGroup,
                    heights[i] - Style::spaceXs, lerpColor(background, accent, .06F), Style::radiusMd
                );
                rectangle(
                    *messageContent, 0, y - (Style::spaceXxs + Style::borderWidth), 3, heights[i] - Style::spaceXs,
                    lerpColor(background, accent, .6F), Style::radiusXs
                );
              }
              if (messageMentions(
                      message, m_account.id,
                      ownRoles ? std::span<const std::string>{*ownRoles} : std::span<const std::string>{}
                  )) {
                const bool over = messageHovered;
                const float tint = (theme::lightTheme ? .10F : .07F) + (over ? .03F : 0.0F);
                const float bottomGap = messageRowPadding(
                    true, i + 1 < continuations.size() && continuations[i + 1], m_preferences.compact,
                    m_preferences.messageGroupSpacing, i + 1 == timeline.size()
                );
                const float inkHeight = std::max(continuations[i] ? 0.0F : 41.0F, heights[i] - bottomGap);
                const float bandTop = std::max(top, top + y - Style::spaceXxs);
                const float bandBottom =
                    std::min({bottom, top + y + inkHeight + Style::spaceXxs, top + y + heights[i]});
                if (bandBottom > bandTop) {
                  rectangle(
                      root, left + Style::spaceDense, bandTop, bodyWidth - (Style::space2Xl + Style::spaceXxs),
                      bandBottom - bandTop, lerpColor(background, accent, tint)
                  );
                  rectangle(root, left + Style::spaceDense, bandTop, 3, bandBottom - bandTop, accent);
                }
              }
              drawJumpHighlight();
              if (message.reply) {
                const auto& reply = *message.reply;
                auto preview = std::make_unique<Node>();
                preview->setPosition(50, y);
                preview->setSize(textWidth, layout::Reply::height);
                preview->setClipChildren(true);
                auto* previewNode = preview.get();
                content->addChild(std::move(preview));
                for (const auto& image : m_messageLayouts.at(message.id).reply.pictures)
                  avatar(*previewNode, image.x, image.y, image.width, image.media.url, reply.author);
                const float mediaTop = (layout::Reply::height - layout::Reply::avatarSize) / 2;
                const auto& previewRuns = m_messageLayouts.at(message.id).reply.runs;
                std::string textMarkup;
                for (const auto& run : previewRuns)
                  if (run.image.empty() && !run.style.emoji && (!run.style.spoiler || spoilersVisible(reply.message)))
                    textMarkup += messageMarkup(run.text, run.style, false);
                const auto textMetrics = m_window.target().renderer().measureText(
                    textMarkup, Style::fontSizeDescription, FontWeight::Normal, 0, 1, TextAlign::Start, {},
                    TextEllipsize::End, true
                );
                const auto inkCenter = [](const TextMetrics& metrics) {
                  return metrics.inkBottom > metrics.inkTop ? (metrics.inkTop + metrics.inkBottom) / 2
                                                            : (metrics.top + metrics.bottom) / 2;
                };
                const float textCenter = inkCenter(textMetrics);
                if (!spoilersVisible(reply.message))
                  for (const auto& area : messageSpoilerAreas(m_messageLayouts.at(message.id).reply))
                    rectangle(
                        *previewNode, area.x, mediaTop, area.width, layout::Reply::avatarSize, raised, Style::radiusSm
                    );
                for (const auto& run : previewRuns) {
                  if (run.style.spoiler && !spoilersVisible(reply.message))
                    continue;
                  if (!run.image.empty()) {
                    if (!emojiImage(*previewNode, run.x, mediaTop, layout::Reply::avatarSize, run.image))
                      label(
                          *previewNode, run.x, mediaTop + Style::borderWidth, layout::Reply::avatarSize, "…",
                          Style::fontSizeDescription, muted
                      );
                  } else {
                    const auto markup = messageMarkup(run.text, run.style, false);
                    const auto metrics = m_window.target().renderer().measureText(
                        markup, run.style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {}, TextEllipsize::End, true
                    );
                    label(
                        *previewNode, run.x,
                        layout::Reply::height / 2 + metrics.top - (run.style.emoji ? inkCenter(metrics) : textCenter),
                        run.width + Style::spaceXxs, markup, run.style.size, muted, FontWeight::Normal, 1, true
                    );
                  }
                }
                drawMessageStrikes(
                    *previewNode, m_messageLayouts.at(message.id).reply, 0, 0, muted, spoilersVisible(reply.message),
                    layout::Reply::height / 2 - textCenter
                );
                const float previewTop = std::max(top, top + y);
                const float previewBottom = std::min(bottom, top + y + layout::Reply::height);
                if (previewBottom > previewTop)
                  m_hits.push_back(
                      {left + layout::Chat::inset + layout::Chat::authorGutter, previewTop, textWidth,
                       previewBottom - previewTop, Action::JumpReply, reply.message}
                  );
                y += layout::Reply::step;
              }
              const bool continuation = continuations[i];
              const auto& layout = *layouts[i];
              float continuationTextOffset = 0;
              float continuationLineCenter = (m_preferences.messageSize + Style::spaceSm) / 2;
              if (continuation) {
                const bool textOnly = !editing
                    && layout.pictures.empty()
                    && std::ranges::all_of(layout.boxes, [](const auto& box) { return box.inlineCode; })
                    && std::ranges::all_of(
                        layout.runs,
                        [](const auto& run) { return run.image.empty() && !run.style.emoji && !run.channelChip; }
                    )
                    && std::abs(heights[i] - bottomGap - std::max(layout::Chat::minimumBodyHeight, layout.height))
                        <= std::numeric_limits<float>::epsilon() * heights[i];
                bool hasInk = false, hasFirstLineInk = false;
                float inkTop = 0, inkBottom = 0, firstTop = 0, firstBottom = 0;
                const float firstLineBottom =
                    layout.runs.empty() ? 0 : layout.runs.front().y + layout.runs.front().height;
                for (const auto& run : layout.runs) {
                  if (!run.image.empty()
                      || run.style.emoji
                      || run.channelChip
                      || (run.style.spoiler && !spoilersVisible(message.id)))
                    continue;
                  auto inkStyle = run.style;
                  inkStyle.underline = false;
                  inkStyle.linkHovered = false;
                  const auto metrics = m_window.target().renderer().measureText(
                      messageMarkup(run.text, inkStyle, false), run.style.size, FontWeight::Normal, 0, 1,
                      TextAlign::Start, {}, TextEllipsize::End, true
                  );
                  if (metrics.inkBottom <= metrics.inkTop)
                    continue;
                  const float first = run.y + metrics.inkTop - metrics.top;
                  const float last = run.y + metrics.inkBottom - metrics.top;
                  inkTop = hasInk ? std::min(inkTop, first) : first;
                  inkBottom = hasInk ? std::max(inkBottom, last) : last;
                  hasInk = true;
                  if (run.y < firstLineBottom) {
                    firstTop = hasFirstLineInk ? std::min(firstTop, first) : first;
                    firstBottom = hasFirstLineInk ? std::max(firstBottom, last) : last;
                    hasFirstLineInk = true;
                  }
                }
                if (textOnly && hasInk)
                  continuationTextOffset =
                      layout::Chat::centeredTextTop((hoverRowBottom - y) / 2, 0, inkTop, inkBottom);
                if (hasFirstLineInk)
                  continuationLineCenter = (firstTop + firstBottom) / 2 + continuationTextOffset;
              }
              if (!continuation) {
                const auto author = userName(message.authorId, message.author);
                const auto authorTag = m_userDirectory.guildTag(message.authorId, message.authorTag);
                auto timestamp = formatMessageClock(message.timestamp, m_preferences.twelveHourClock);
                if (message.deleted)
                  timestamp += timestamp.empty() ? "Deleted" : " · Deleted";
                const float timestampWidth =
                    m_window.target().renderer().measureText(timestamp, Style::fontSizeDetail).width;
                const float headerWidth =
                    std::max(24.0F, textWidth - (timestamp.empty() ? 0 : timestampWidth + Style::spaceDense));
                const float roleSpace = m_preferences.showRoleIcons
                        && !m_dmMode
                        && m_roleDirectory.icon(m_guildId, message.authorId, message.memberRoles)
                    ? 22
                    : 0;
                const float tagSpace = m_preferences.showGuildTags && authorTag
                    ? std::min(
                          104.0F,
                          std::ceil(m_window.target()
                                        .renderer()
                                        .measureText(authorTag->tag, Style::fontSizeMini, FontWeight::Medium)
                                        .width)
                              + 32
                      ) + 6
                    : 0;
                const bool tagFits = headerWidth >= tagSpace + roleSpace + 38;
                const float nameMax = std::max(24.0F, headerWidth - (tagFits ? tagSpace : 0) - roleSpace - 6);
                const float authorWidth = std::min(
                    nameMax,
                    m_window.target().renderer().measureText(author, Style::fontSizeCaption, FontWeight::SemiBold).width
                        + 4
                        + (m_preferences.roleColorMode == 1
                                   && !m_dmMode
                                   && m_roleDirectory.colors(m_guildId, message.authorId, message.memberRoles)
                               ? 12
                               : 0)
                );
                float groupBottom = originalY + heights[i];
                for (std::size_t next = i + 1; next < timeline.size() && continuations[next]; ++next)
                  groupBottom += heights[next];
                avatar(
                    *messageContent, 0, y + Style::borderWidth, layout::Avatar::chat, message.avatar, author,
                    message.authorId, "",
                    layout::Frame{0, y, bodyWidth - Style::spaceGroup, std::max(0.0F, groupBottom - y)}
                );
                roleLabel(
                    *messageContent, 50, y, nameMax, author, Style::fontSizeCaption, foreground, FontWeight::SemiBold,
                    message.authorId, "message:" + message.id,
                    y + (Style::spaceLg + Style::spaceXxs) >= 0
                        && y < bottom - top
                        && hovered(
                            left + Style::spaceXl, rowTop, bodyWidth - Style::spaceGroup,
                            std::max(0.0F, std::min(bottom, top + groupBottom) - rowTop)
                        ),
                    message.memberRoles
                );
                const float tagUsed = m_preferences.showGuildTags && tagFits
                    ? drawGuildTag(
                          *messageContent, 50 + authorWidth + Style::spaceXs, y - Style::spaceXxs,
                          headerWidth - authorWidth - roleSpace - 4, authorTag
                      )
                    : 0;
                const float roleUsed =
                    m_preferences.showRoleIcons && !m_dmMode && headerWidth - authorWidth - tagUsed >= 22
                    ? drawRoleIcon(
                          *messageContent, 50 + authorWidth + tagUsed + 4, y, m_guildId, message.authorId,
                          message.memberRoles
                      )
                    : 0;
                if (!message.authorId.empty()) {
                  const auto avatarY = std::max(top, top + y);
                  const auto avatarH =
                      std::max(0.0F, std::min(bottom, top + y + (Style::spaceSection + Style::borderWidth)) - avatarY);
                  const auto nameWidth = authorWidth;
                  if (!local)
                    m_contextTargets.push_back(
                        {left + Style::spaceXl, avatarY, 40, avatarH, Action::UserMenu, message.authorId}
                    );
                  if (!local)
                    m_contextTargets.push_back(
                        {left + 74, avatarY, nameWidth, std::min(18.0F, avatarH), Action::UserMenu, message.authorId}
                    );
                  m_hits.push_back(
                      {left + Style::spaceXl, avatarY, 40, avatarH, Action::UserProfile, message.authorId}
                  );
                  m_hits.push_back(
                      {left + 74, avatarY, nameWidth, std::min(18.0F, avatarH), Action::UserProfile, message.authorId}
                  );
                }
                const float timestampX = 50 + authorWidth + tagUsed + roleUsed + 6;
                const float timestampAvailable = 50 + textWidth - timestampX;
                if (!timestamp.empty() && timestampAvailable > 0)
                  label(
                      *messageContent, timestampX, y + (Style::spaceXxs + Style::borderWidth),
                      std::min(timestampWidth + Style::spaceXxs, timestampAvailable), timestamp, Style::fontSizeDetail,
                      message.deleted ? theme::danger : muted
                  );
              } else if (messageHovered) {
                const auto timestamp = formatMessageClock(message.timestamp, m_preferences.twelveHourClock);
                const auto metrics = m_window.target().renderer().measureText(timestamp, Style::fontSizeFine);
                label(
                    *messageContent, 0,
                    y
                        + layout::Chat::centeredTextTop(
                            continuationLineCenter, metrics.top, metrics.inkTop, metrics.inkBottom
                        ),
                    m_preferences.twelveHourClock ? layout::Chat::authorGutter - Style::spaceXxs
                                                  : layout::Chat::continuationTimestampWidth,
                    timestamp, Style::fontSizeFine, muted
                );
              }
              const float bodyY = y + (continuation ? 0 : 18);
              const float bodyHeight = editing ? messageEditHeight(textWidth) : layout.height;
              const float componentHeight =
                  messageButtons(messageContent, message, 50, bodyY + bodyHeight + Style::spaceCompact, textWidth);
              const float reactionHeight = reactionStrip(
                  messageContent, message, 50, bodyY + bodyHeight + Style::spaceCompact + componentHeight, textWidth
              );
              threadStrip(
                  messageContent, message, 50,
                  bodyY + bodyHeight + Style::spaceCompact + componentHeight + reactionHeight, textWidth
              );
              if (editing)
                drawMessageEdit(*messageContent, 50, bodyY, textWidth);
              else {
                m_messageText[message.id] = layout.plainText;
                m_hits.push_back({left + 74, top + bodyY, textWidth, layout.height, Action::SelectMessage, message.id});
                for (const auto& card : layout.forwards) {
                  const float cardY = bodyY + card.y;
                  rectangle(
                      *messageContent, 50, cardY + (Style::spaceXl - Style::spaceXxs), 3, card.height - 22,
                      lerpColor(background, muted, .4F), 1.5F
                  );
                  glyph(*messageContent, 50, cardY + Style::spaceXxs, Style::glyphSizeSmall, "corner-up-right", muted);
                  label(
                      *messageContent, 70, cardY + Style::spaceXxs, textWidth - Style::spaceRelaxed, "Forwarded",
                      Style::fontSizeDescription, muted
                  );
                  const float footerY = cardY + card.height - 19;
                  std::string source;
                  const auto guild = std::ranges::find(m_guilds, card.guild, &api::Guild::id);
                  const auto channels = m_guildChannels.find(card.guild);
                  const api::Channel* origin = nullptr;
                  if (guild != m_guilds.end() && channels != m_guildChannels.end()) {
                    const auto at = std::ranges::find(channels->second, card.channel, &api::Channel::id);
                    if (at != channels->second.end()) {
                      const auto permissions = api::channelPermissions(
                          card.guild, m_account.id, m_roleDirectory.owner(card.guild),
                          m_roleDirectory.allRoles(card.guild), m_roleDirectory.memberships(card.guild, m_account.id),
                          *at
                      );
                      if (permissions && (*permissions & api::viewChannel))
                        origin = &*at;
                    }
                  }
                  if (origin)
                    source = guild->name + " · #" + origin->name;
                  const auto time = formatMessageClock(card.timestamp, m_preferences.twelveHourClock);
                  if (!time.empty())
                    source += (source.empty() ? "" : " · ") + time;
                  if (!source.empty()) {
                    const auto footerHeight = label(
                        *messageContent, 62, footerY, textWidth - Style::spaceMd, source, Style::fontSizeMini, muted
                    );
                    const float linkTop = std::max(top, top + footerY),
                                linkBottom = std::min(bottom, top + footerY + footerHeight);
                    if (origin && linkBottom > linkTop)
                      m_hits.push_back(
                          {left + 86, linkTop, textWidth - Style::spaceMd, linkBottom - linkTop, Action::OpenUrl,
                           "https://discord.com/channels/" + card.guild + "/" + card.channel + "/" + card.message}
                      );
                  }
                }
                // Card backgrounds precede nested code/quote decoration.
                for (const auto& box : layout.boxes) {
                  if (box.divider) {
                    rectangle(*messageContent, 50 + box.x, bodyY + box.y, box.width, box.height, raised);
                    continue;
                  }
                  if (!box.link.empty()) {
                    Hit hit{left + 74 + box.x,
                            top + bodyY + box.y,
                            box.width,
                            box.height,
                            box.disabled ? Action::None : Action::OpenUrl,
                            box.link};
                    const bool hidden = box.spoiler && !spoilersVisible(message.id);
                    rectangle(
                        *messageContent, 50 + box.x, bodyY + box.y, box.width, box.height,
                        hidden                                                      ? raised
                            : !box.disabled && hit.contains(m_pointerX, m_pointerY) ? raised
                                                                                    : panel,
                        Style::controlRadius
                    );
                    if (hidden) {
                      hit.action = Action::RevealSpoiler;
                      hit.id = message.id;
                    }
                    m_hits.push_back(std::move(hit));
                    continue;
                  }
                  if (box.inlineCode) {
                    rectangle(
                        *messageContent, 50 + box.x, bodyY + box.y + continuationTextOffset, box.width, box.height,
                        withAlpha(muted, .2F), Style::radiusSm
                    );
                    rectangle(
                        *messageContent, 51 + box.x, bodyY + box.y + continuationTextOffset + Style::borderWidth,
                        box.width - Style::spaceXxs, box.height - Style::spaceXxs, panel, Style::radiusXs
                    );
                    continue;
                  }
                  rectangle(
                      *messageContent, 50 + box.x, bodyY + box.y, box.width, box.height,
                      box.quote       ? muted
                          : box.embed ? sidebar
                                      : panel,
                      box.quote ? 0 : Style::radiusMd
                  );
                  if (box.embed && box.embedAccent)
                    rectangle(*messageContent, 50 + box.x, bodyY + box.y, 3, box.height, rgbHex(box.color), 1);
                  if (!box.code.empty()) {
                    if (messageHovered) {
                      iconButton(
                          *messageContent,
                          {50 + box.x + box.width - 26, bodyY + box.y + Style::spaceXs, 22, 22, Action::CopyCode,
                           box.code},
                          "copy", "Copy code"
                      );
                    }
                  }
                }
                const auto linkAreas = messageLinkAreas(layout);
                for (const auto& area : linkAreas) {
                  const auto& run = layout.runs[area.first];
                  if (run.style.spoiler && !spoilersVisible(message.id))
                    continue;
                  const Hit hit{
                      left + layout::Chat::inset + layout::Chat::authorGutter + area.x,
                      top + bodyY + area.y,
                      area.width,
                      area.height,
                      Action::OpenUrl,
                      area.url
                  };
                  m_hits.push_back(hit);
                }
                if (!spoilersVisible(message.id))
                  for (const auto& area : messageSpoilerAreas(layout)) {
                    rectangle(
                        *messageContent, layout::Chat::authorGutter + area.x, bodyY + area.y, area.width, area.height,
                        raised, Style::radiusSm
                    );
                    m_hits.push_back(
                        {left + layout::Chat::inset + layout::Chat::authorGutter + area.x, top + bodyY + area.y,
                         area.width, area.height, Action::RevealSpoiler, message.id}
                    );
                  }
                std::size_t runIndex = 0;
                for (const auto& run : layout.runs) {
                  auto style = run.style;
                  style.linkHovered = messageLinkHovered(
                      linkAreas, runIndex++, m_pointerX - left - layout::Chat::inset - layout::Chat::authorGutter,
                      m_pointerY - top - bodyY
                  );
                  const float rx = 50 + run.x, ry = bodyY + run.y;
                  const bool hidden = run.style.spoiler && !spoilersVisible(message.id);
                  if (hidden)
                    continue;
                  if (run.channelChip) {
                    drawChannelLink(
                        *messageContent, run, message.id, rx, ry, left + layout::Chat::inset + rx, top + ry
                    );
                    continue;
                  }
                  if (m_selectedMessage == message.id && m_selectedText == layout.plainText && m_chatEdit.selected()) {
                    const auto from = std::max(std::min(m_chatEdit.caret, m_chatEdit.anchor), run.offset);
                    const auto to =
                        std::min(std::max(m_chatEdit.caret, m_chatEdit.anchor), run.offset + run.text.size());
                    if (to > from) {
                      const auto offset = run.image.empty()
                          ? measure(std::string_view(run.text).substr(0, from - run.offset), run.style)
                          : 0;
                      const auto length = run.image.empty()
                          ? measure(std::string_view(run.text).substr(from - run.offset, to - from), run.style)
                          : run.width;
                      rectangle(
                          *messageContent, rx + offset, ry, length, run.height, withAlpha(accent, .3F), Style::radiusXs
                      );
                    }
                  }
                  if (run.image.empty())
                    label(
                        *messageContent, rx, ry + continuationTextOffset, std::max(1.0F, run.width + Style::spaceXxs),
                        messageMarkup(run.text, style, false), run.style.size,
                        run.style.historical || run.style.muted            ? muted
                            : run.style.syntax != CodeToken::Plain         ? syntaxColor(run.style.syntax)
                            : run.style.link.empty() && !run.style.mention ? foreground
                                                                           : accent,
                        FontWeight::Normal, 1, true
                    );
                  else if (!emojiImage(*messageContent, rx, ry, run.width, run.image))
                    label(*messageContent, rx, ry, 22, "…", Style::fontSizeBody, muted);
                  m_textAreas.push_back({run, message.id, left + Style::spaceXl + rx, top + ry});
                  if (!run.style.userMention.empty())
                    m_hits.push_back(
                        {left + Style::spaceXl + rx, top + ry, run.width, run.height, Action::UserProfile,
                         run.style.userMention}
                    );
                  else if (!run.style.link.empty() && run.channelChip)
                    m_hits.push_back(
                        {left + Style::spaceXl + rx, top + ry, run.width, run.height, Action::OpenUrl, run.style.link}
                    );
                }
                drawMessageStrikes(
                    *messageContent, layout, layout::Chat::authorGutter, bodyY + continuationTextOffset, foreground,
                    spoilersVisible(message.id)
                );
                if (layout.edited) {
                  const auto& marker = *layout.edited;
                  label(
                      *messageContent, 50 + marker.x, bodyY + marker.y + continuationTextOffset,
                      marker.width + Style::spaceXxs, "(edited)", marker.size, muted
                  );
                }
                for (const auto& pic : layout.pictures) {
                  if (pic.spoiler && !spoilersVisible(message.id)) {
                    rectangle(
                        *messageContent, 50 + pic.x, bodyY + pic.y, pic.width, pic.height, raised, Style::radiusMd
                    );
                    label(
                        *messageContent, 50 + pic.x + Style::spaceMd, bodyY + pic.y + Style::spaceMd,
                        pic.width - Style::spaceXl, "Spoiler", Style::fontSizeDescription, muted
                    );
                    m_hits.push_back(
                        {left + 74 + pic.x, top + bodyY + pic.y, pic.width, pic.height, Action::RevealSpoiler,
                         message.id}
                    );
                    continue;
                  }
                  if (pic.sticker) {
                    const float sx = left + 74 + pic.x, sy = top + bodyY + pic.y;
                    const bool loaded = picture(
                        *messageContent, 50 + pic.x, bodyY + pic.y, pic.width, pic.height, pic.media.url, 0, false,
                        false, ImageFitMode::Contain
                    );
                    if (!loaded) {
                      glyph(
                          *messageContent, 50 + pic.x + (pic.width - Style::space2Xl) / 2,
                          bodyY + pic.y + Style::spaceXl, Style::glyphSizeDisplay, "sticker", muted
                      );
                      label(
                          *messageContent, 50 + pic.x + Style::spaceXs, bodyY + pic.y + 64, pic.width - Style::spaceSm,
                          pic.sticker->name, Style::fontSizeDescription, muted, FontWeight::Normal, 2
                      );
                    }
                    m_stickerTargets[pic.sticker->id] = *pic.sticker;
                    m_hits.push_back({sx, sy, pic.width, pic.height, Action::StickerInfo, pic.sticker->id});
                    continue;
                  }
                  if (!pic.videoSource.empty()) {
                    m_videoPosters[pic.videoSource] = pic.media.url;
                    drawVideo(
                        *messageContent, 50 + pic.x, bodyY + pic.y, pic.width, pic.height, pic.videoSource,
                        pic.media.url
                    );
                    continue;
                  }
                  if (pic.video)
                    rectangle(
                        *messageContent, 50 + pic.x, bodyY + pic.y, pic.width, pic.height, panel, Style::radiusMd
                    );
                  picture(
                      *messageContent, 50 + pic.x, bodyY + pic.y, pic.width, pic.height, pic.media.url, Style::radiusMd,
                      true
                  );
                  const float px = left + 74 + pic.x, py = top + bodyY + pic.y;
                  if (pic.video) {
                    rectangle(
                        *messageContent, 50 + pic.x + pic.width / 2 - 25,
                        bodyY + pic.y + pic.height / 2 - Style::spaceRelaxed, 50, 40,
                        hovered(px, py, pic.width, pic.height) ? accent : panel, 10
                    );
                    label(
                        *messageContent, 50 + pic.x + pic.width / 2 - 7, bodyY + pic.y + pic.height / 2 - 11, 24, "▶",
                        Style::fontSizeHeader, foreground
                    );
                    m_hits.push_back({px, py, pic.width, pic.height, Action::OpenUrl, pic.link});
                  } else {
                    if (m_viewImage.empty())
                      m_viewerImages.push_back(
                          {pic.media.url, pic.link.empty() ? pic.media.url : pic.link,
                           static_cast<float>(pic.media.width), static_cast<float>(pic.media.height)}
                      );
                    m_hits.push_back({px, py, pic.width, pic.height, Action::ViewImage, pic.media.url});
                    const auto cachedImage = m_images.find(pic.media.url);
                    const bool animated = pic.animated
                        || pic.media.url.contains(".gif")
                        || (cachedImage != m_images.end()
                            && cachedImage->second.animation
                            && cachedImage->second.animation->frames.size() > 1);
                    if (animated
                        && hovered(px, py, pic.width, pic.height)
                        && py >= top
                        && py + (Style::space2Xl + Style::spaceXs) <= bottom) {
                      if (!m_demo && !m_gifFavoritesLoaded && !m_loadingGifFavorites)
                        m_loadingGifFavorites = m_client.gifFavorites();
                      api::Gif gif;
                      gif.url = pic.link.empty() ? api::mediaUrl(pic.media.url) : pic.link;
                      gif.src = api::mediaUrl(pic.media.url);
                      gif.width = pic.media.width;
                      gif.height = pic.media.height;
                      gif.format = pic.media.url.contains(".mp4") || pic.media.url.contains(".webm") ? 2 : 1;
                      const bool favorite =
                          std::ranges::find(m_gifFavorites, gif.url, &api::Gif::url) != m_gifFavorites.end();
                      m_gifTargets[gif.url] = gif;
                      rectangle(
                          *messageContent, 50 + pic.x + pic.width - 36, bodyY + pic.y + Style::spaceXs, 30, 30, sidebar,
                          Style::hoverRadius
                      );
                      iconButton(
                          *messageContent,
                          {50 + pic.x + pic.width - 36, bodyY + pic.y + Style::spaceXs, 30, 30,
                           m_pendingGifFavorites.contains(gif.url) ? Action::None : Action::FavoriteGif, gif.url},
                          favorite ? "star-filled" : "star", favorite ? "Remove GIF from favorites" : "Favorite GIF",
                          favorite
                      );
                    }
                  }
                }
              }
            }
            if (messageHovered) {
              const float toolbarWidth = message.deleted ? 64 : message.type == 7 ? 94 : 124;
              rectangle(
                  *messageContent, bodyWidth - 66 - toolbarWidth, y - Style::spaceSm, toolbarWidth, 34, panel,
                  Style::hoverRadius
              );
              iconButton(
                  *messageContent,
                  {bodyWidth - 62 - toolbarWidth, y - Style::spaceXs, 28, 26, Action::CopyMessage, message.id}, "copy",
                  "Copy text"
              );
              if (!message.deleted && message.type != 7 && message.type != autoModerationActionType)
                iconButton(
                    *messageContent, {bodyWidth - 156, y - Style::spaceXs, 28, 26, Action::Reply, message.id},
                    "corner-up-left", "Reply"
                );
              if (!message.deleted)
                iconButton(
                    *messageContent, {bodyWidth - 126, y - Style::spaceXs, 28, 26, Action::AddReaction, message.id},
                    "mood-smile", "Add reaction"
                );
              iconButton(
                  *messageContent, {bodyWidth - 96, y - Style::spaceXs, 28, 26, Action::MessageMenu, message.id},
                  "dots", "More actions"
              );
            }
            if (failed) {
              const float statusY = originalY + heights[i] - layout::Chat::pendingStatusHeight;
              const float restoreWidth = Style::spaceSection * 3;
              label(
                  *content, layout::Chat::authorGutter, statusY + Style::spaceSm,
                  std::max(0.0F, textWidth - restoreWidth - Style::spaceMd), "Not confirmed",
                  Style::fontSizeDescription, muted
              );
              button(
                  *content,
                  {layout::Chat::authorGutter + std::max(0.0F, textWidth - restoreWidth), statusY, restoreWidth,
                   Style::controlHeightSm, Action::RestoreOutgoing, message.nonce},
                  "Restore"
              );
              if (hovered(
                      left + Style::spaceXl, top + statusY, bodyWidth - Style::spaceGroup,
                      layout::Chat::pendingStatusHeight
                  )) {
                m_tooltip = pending->second.error;
                m_tooltipX = m_pointerX;
                m_tooltipY = top + statusY;
              }
            }
            for (auto index = firstHit; index < m_hits.size(); ++index) {
              auto& hit = m_hits[index];
              const float end = std::min(bottom, hit.y + hit.height);
              hit.y = std::max(top, hit.y);
              hit.height = std::max(0.0F, end - hit.y);
            }
          }
          y = originalY + heights[i];
        }
        const bool loadingConversation =
            m_loadingMessages || (m_channelId.empty() && (m_dmMode ? m_loadingPrivate : m_loadingChannels));
        if (timeline.empty() && loadingConversation) {
          // Match the timeline's geometry without presenting fake text or actions.
          // Stable widths avoid reshuffling the skeleton on unrelated repaints.
          const auto nameColor = lerpColor(background, muted, 0.23F);
          const auto textColor = lerpColor(background, muted, 0.13F);
          const auto avatarColor = lerpColor(background, muted, 0.17F);
          const float viewport = std::max(0.0F, bottom - top);
          const float lineWidth = std::min(textWidth, 620.0F);
          constexpr int lines[] = {2, 1, 3, 2, 1, 2, 4};
          float rowY = 12;
          for (int row = 0; row < 24 && rowY < viewport; ++row) {
            if (!m_preferences.compact)
              rectangle(*content, 0, rowY + Style::borderWidth, 40, 40, avatarColor, 20);
            rectangle(
                *content, 50, rowY + (Style::spaceXxs + Style::borderWidth),
                std::min(lineWidth, 68.0F + (row * 23 % 69)), 9, nameColor, 4.5F
            );
            const int count = lines[row % 7];
            for (int line = 0; line < count; ++line) {
              const float lineY = rowY + (Style::spaceXl + Style::borderWidth) + line * 20;
              if (lineY >= viewport)
                break;
              const float target = lineWidth * (0.42F + ((row * 7 + line * 3) % 10) * 0.055F);
              float wordX = 0;
              for (int word = 0; wordX < target; ++word) {
                const float length = std::min(24.0F + ((row * 19 + line * 13 + word * 23) % 65), target - wordX);
                rectangle(*content, 50 + wordX, lineY, length, 10, textColor, Style::controlRadius);
                wordX += length + 6;
              }
            }
            rowY += (Style::spaceXl + Style::borderWidth) + count * 20 + (m_preferences.compact ? 12 : 22);
          }
        } else if (timeline.empty() && introHeight <= 0) {
          label(
              *content, 18, 36, bodyWidth - 88,
              m_channelId.empty() ? "Make yourself at home." : "The conversation starts here.", Style::fontSizeFeature,
              foreground, FontWeight::Bold
          );
          label(
              *content, 18, 80, bodyWidth - 88,
              m_channelId.empty() ? "Choose a server and a text channel to get started." : "Send the first message.",
              Style::fontSizeBody, muted, FontWeight::Normal, 2
          );
        }
        std::vector<TimelineAnchor::Row> visibleAnchors;
        float rowY = (bottom - totalHeight) + m_messageScroll + timelineTopPadding + introHeight;
        for (std::size_t i = 0; i < timeline.size(); ++i) {
          if (unreadIndex && i == *unreadIndex)
            rowY += unreadHeight;
          if (rowY + heights[i] > top && rowY < bottom)
            visibleAnchors.push_back({timeline[i].id, rowY});
          rowY += heights[i];
        }
        m_timelineAnchor.remember(std::move(visibleAnchors), m_messageScroll);
        if (!m_demo && !m_dmMode)
          m_client.requestMembers(m_guildId, missingAuthors);
        root.addChild(std::move(content));
        if ((!m_demo || m_demoOverlay == "history-cycle")
            && !m_loadingMessages
            && m_history.initialized()
            && !m_history.busy()
            && !settingsVisible()
            && m_viewImage.empty()
            && m_dialogKind.empty()) {
          const float threshold = std::max(layout::Chat::minimumHistoryFetchDistance, (bottom - top) * 0.6F);
          if (!m_history.olderEnd() && m_messageMaxScroll - m_messageScroll < threshold)
            loadHistory(api::HistoryDirection::Older);
          else if (!m_history.atLatest() && m_messageScroll < threshold)
            loadHistory(api::HistoryDirection::Newer);
        }
        const bool jumpLatestVisible = !m_history.atLatest() || m_messageScroll > 48;
        const auto pendingHistory = m_history.pending();
        if ((pendingHistory && pendingHistory->direction != api::HistoryDirection::Latest)
            || !m_history.error().empty()) {
          const auto direction = pendingHistory ? pendingHistory->direction : m_history.failedDirection();
          const bool older = direction == api::HistoryDirection::Older;
          const float statusY = older ? top + Style::spaceXs : bottom - (jumpLatestVisible ? 74 : 32);
          const float width = std::min(360.0F, bodyWidth - Style::spaceGroup), statusX = left + (bodyWidth - width) / 2;
          rectangle(root, statusX, statusY, width, 28, sidebar, Style::surfaceRadius);
          label(
              root, statusX + Style::spaceMd, statusY + (Style::spaceCompact + Style::borderWidth), width - 92,
              m_history.error().empty() ? (older ? "Loading earlier messages…" : "Loading newer messages…")
                                        : "Could not load message history.",
              Style::fontSizeMini, muted
          );
          if (!m_history.error().empty()) {
            button(root, {statusX + width - 76, statusY, 76, 28, Action::RetryHistory, {}}, "Retry");
            if (hovered(statusX, statusY, width, 28)) {
              m_tooltip = m_history.error();
              m_tooltipX = statusX + width / 2;
              m_tooltipY = statusY;
            }
          }
        }
        if (jumpLatestVisible) {
          constexpr float size = 32;
          const float x = left + bodyWidth - size - 24, y = bottom - size - 8;
          const bool over = hovered(x, y, size, size);
          rectangle(root, x, y, size, size, lerpColor(panel, accent, over ? .4F : .2F), size / 2);
          rectangle(
              root, x + Style::borderWidth, y + Style::borderWidth, size - Style::spaceXxs, size - Style::spaceXxs,
              over ? raised : panel, size / 2 - 1
          );
          glyph(
              root, x + Style::spaceSm, y + Style::spaceSm, Style::baseGlyphSize, "arrow-down",
              over ? foreground : muted
          );
          m_hits.push_back({x, y, size, size, Action::JumpLatest, {}});
          if (over) {
            m_tooltip = "Jump to latest";
            m_tooltipX = x + size / 2;
            m_tooltipY = y;
          }
        }
        if (m_unreadTimeline.unread()) {
          std::string caption = "New messages";
          if (unreadIndex) {
            const auto count =
                std::count_if(m_messages.begin() + *unreadIndex, m_messages.end(), [](const auto& message) {
                  return !message.deleted && !(message.flags & 64);
                });
            caption = std::to_string(count)
                + ((!m_history.atLatest() || m_unreadTimeline.estimated(m_messages)) ? "+" : "")
                + (count == 1 ? " new message" : " new messages");
            const auto timestamp =
                formatMessageClock(m_messages[*unreadIndex].timestamp, m_preferences.twelveHourClock);
            if (!timestamp.empty())
              caption += " since " + timestamp;
          }
          const float x = left + Style::spaceLg, y = 110, width = bodyWidth - Style::space2Xl;
          const float readWidth = 144;
          const bool over = hovered(x, y, width - readWidth, 36);
          rectangle(root, x, y, width, 36, lerpColor(background, accent, 0.3F), Style::surfaceRadius);
          if (over)
            rectangle(root, x, y, width - readWidth, 36, lerpColor(background, accent, 0.4F), Style::surfaceRadius);
          glyph(root, x + Style::spaceMd, y + Style::spaceDense, Style::baseGlyphSize, "arrow-down", foreground);
          label(
              root, x + (Style::space2Xl + Style::spaceXs), y + Style::spaceDense,
              std::max(1.0F, width - readWidth - Style::spaceGroup), caption, Style::fontSizeDescription, foreground,
              FontWeight::Medium
          );
          m_hits.push_back({x, y, width - readWidth, 36, Action::JumpUnread, {}});
          const float readX = x + width - readWidth;
          if (hovered(readX, y, readWidth, 36))
            rectangle(root, readX, y, readWidth, 36, lerpColor(background, accent, 0.5F), Style::surfaceRadius);
          label(
              root, readX + Style::spaceMd, y + Style::spaceDense, readWidth - Style::spaceSection, "Mark as Read",
              Style::fontSizeDescription, foreground, FontWeight::SemiBold
          );
          glyph(root, x + width - Style::spaceWide, y + Style::spaceDense, Style::baseGlyphSize, "checks", foreground);
          m_hits.push_back({readX, y, readWidth, 36, Action::MarkRead, {}});
        }
        if (!settingsVisible() && m_viewImage.empty()) {
          const std::string id = "chat-scrollbar";
          std::unique_ptr<Node> owned;
          if (auto stored = m_detachedInputs.find(id); stored != m_detachedInputs.end()) {
            owned = std::move(stored->second);
            m_detachedInputs.erase(stored);
          } else {
            auto bar = std::make_unique<Scrollbar>();
            bar->setTrackInset(4);
            bar->setOnScrollChanged([this](float offset) {
              m_scrolling.stop(m_messageScroll);
              m_messageScroll = std::clamp(m_messageMaxScroll - offset, 0.0F, m_messageMaxScroll);
              m_window.invalidate();
            });
            owned = std::move(bar);
          }
          auto* bar = static_cast<Scrollbar*>(owned.get());
          m_widgets[id] = bar;
          bar->setPosition(left + bodyWidth - layout::Floating::inset - Style::spaceXs - bar->reservedThickness(), top);
          bar->setSize(bar->reservedThickness(), std::max(0.0F, bottom - top));
          bar->update(std::max(0.0F, bottom - top), totalHeight, m_messageMaxScroll - m_messageScroll);
          root.addChild(std::move(owned));
        }
        floatingPanel(
            root, {left + layout::Floating::inset, composerY, bodyWidth - layout::Floating::gap, composerHeight},
            background
        );
        if (!typing.empty())
          drawTyping(root, left + layout::Chat::inset, composer.typingTop, bodyWidth - layout::Chat::inset * 2, typing);
        if (replyHeight) {
          const auto& reply = m_replies.at(m_channelId);
          auto& renderer = m_window.target().renderer();
          const std::string caption = "Replying to " + userName(reply.authorId, reply.author);
          const auto captionMetrics = renderer.measureText(caption, layout::ComposerReply::captionSize);
          const auto offMetrics = renderer.measureText("@ OFF", layout::ComposerReply::captionSize);
          const auto onMetrics = renderer.measureText("@ ON", layout::ComposerReply::captionSize);
          const auto frame = layout::composerReplyFrame(
              left + layout::Floating::inset, composerY, bodyWidth - layout::Floating::gap,
              std::ceil(std::max(offMetrics.width, onMetrics.width)), captionMetrics.bottom - captionMetrics.top
          );
          label(
              root, frame.label.x, frame.label.y, frame.label.width, caption, layout::ComposerReply::captionSize,
              foreground
          );
          const Hit mention{frame.mention.x,
                            frame.mention.y,
                            frame.mention.width,
                            frame.mention.height,
                            Action::ToggleReplyMention,
                            {}};
          const bool over = mention.contains(m_pointerX, m_pointerY);
          rectangle(
              root, mention.x, mention.y, mention.width, mention.height, over ? raised : panel, Style::controlRadius
          );
          const auto& metrics = reply.mention ? onMetrics : offMetrics;
          label(
              root, mention.x + (mention.width - metrics.width) / 2,
              mention.y + (mention.height - (metrics.bottom - metrics.top)) / 2, metrics.width + Style::borderWidth,
              reply.mention ? "@ ON" : "@ OFF", layout::ComposerReply::captionSize, reply.mention ? accent : foreground
          );
          m_hits.push_back(mention);
          iconButton(
              root, {frame.close.x, frame.close.y, frame.close.width, frame.close.height, Action::CancelReply, {}}, "x",
              "Cancel reply"
          );
        }
        m_attachmentX = left + (Style::space2Xl + Style::spaceXs);
        m_attachmentY = composerY + replyHeight + Style::spaceDense;
        m_attachmentWidth = bodyWidth - 72;
        m_scrolling.limit(
            m_attachmentScroll,
            std::max(0.0F, m_attachments[m_channelId].size() * layout::DraftAttachment::step - m_attachmentWidth)
        );
        auto previews = std::make_unique<Node>();
        previews->setPosition(m_attachmentX, m_attachmentY);
        previews->setSize(m_attachmentWidth, layout::DraftAttachment::height);
        previews->setClipChildren(true);
        float previewX = -m_attachmentScroll;
        for (const auto& attachment : m_attachments[m_channelId]) {
          auto preview = std::make_unique<ImageNode>();
          preview->setPosition(previewX, layout::DraftAttachment::topInset);
          preview->setSize(layout::DraftAttachment::size, layout::DraftAttachment::size);
          preview->setTextureId(attachment.texture.id);
          preview->setTextureSize(attachment.texture.width, attachment.texture.height);
          preview->setFitMode(ImageFitMode::Contain);
          previews->addChild(std::move(preview));
          const float hitLeft = std::max(m_attachmentX, m_attachmentX + previewX);
          const float hitRight =
              std::min(m_attachmentX + m_attachmentWidth, m_attachmentX + previewX + layout::DraftAttachment::size);
          if (hitRight > hitLeft)
            m_hits.push_back(
                {hitLeft, m_attachmentY + layout::DraftAttachment::topInset, hitRight - hitLeft,
                 layout::DraftAttachment::size, Action::ViewAttachment, attachment.key}
            );
          iconButton(
              *previews,
              {previewX + layout::DraftAttachment::removeLeft, 0, layout::DraftAttachment::removeSize,
               layout::DraftAttachment::removeSize, Action::RemoveAttachment, attachment.key},
              "x", "Remove attachment"
          );
          auto& hit = m_hits.back();
          const auto right = std::min(hit.x + hit.width, m_attachmentX + m_attachmentWidth);
          hit.x = std::max(hit.x, m_attachmentX);
          hit.width = std::max(0.0F, right - hit.x);
          previewX += layout::DraftAttachment::step;
        }
        root.addChild(std::move(previews));
        const float stickerSize = std::min(80.0F, std::max(32.0F, (bodyWidth - 96) / 3));
        float stickerX = left + (Style::spaceSection - Style::spaceXxs);
        const float stickerY = composerY + replyHeight + uploadHeight + Style::spaceSm;
        for (const auto& sticker : m_draftStickers[m_channelId]) {
          picture(
              root, stickerX, stickerY, stickerSize, stickerSize, api::stickerAsset(sticker), 0, false, false,
              ImageFitMode::Contain
          );
          iconButton(
              root,
              {stickerX + stickerSize - (Style::spaceXl - Style::spaceXxs), stickerY, 24, 24, Action::RemoveSticker,
               sticker.id},
              "x", "Remove sticker"
          );
          stickerX += stickerSize + Style::spaceMd;
        }
        m_draftX = left + 74;
        m_composerTop = composerY;
        m_draftY = composerY + replyHeight + attachmentHeight + Style::spaceXs;
        inputControl(
            root, left + 68, m_draftY, m_draftWidth + Style::spaceMd,
            composerHeight - attachmentHeight - replyHeight - Style::spaceSm, "composer", draft,
            m_demo ? "Demo mode is read-only" : "Message this channel…", true
        );
        const auto actionY =
            composerY + composerHeight - ((Style::spaceSection - Style::spaceXxs) + Style::borderWidth);
        iconButton(
            root, {left + (Style::space2Xl - Style::spaceXxs), actionY, 32, 32, Action::PasteClipboard, {}},
            "clipboard-plus", "Paste from clipboard"
        );
        iconButton(
            root, {left + bodyWidth - 62, actionY, 32, 32, Action::ToggleEmoji, {}}, "mood-smile", "Emoji",
            m_emojiMenu && !m_stickerTab
        );
        iconButton(root, {left + bodyWidth - 98, actionY, 32, 32, Action::ToggleGif, {}}, "gif", "GIFs", m_gifMenu);
        iconButton(
            root, {left + bodyWidth - 134, actionY, 32, 32, Action::ToggleStickers, {}}, "sticker", "Stickers",
            m_emojiMenu && m_stickerTab
        );
        if (!m_status.empty())
          label(root, left + Style::spaceWide, 105, bodyWidth - 56, m_status, Style::fontSizeMini, muted);
        if (memberWidth > 0 && dmRecipient()) {
          drawDmProfile(
              root,
              layout::floatingFrame(
                  w - memberWidth, layout::Window::titlebarHeight + layout::ChannelHeader::height, memberWidth,
                  h - layout::Window::titlebarHeight - layout::ChannelHeader::height
              )
          );
        } else if (memberWidth > 0) {
          const float memberX = w - memberWidth;
          const auto memberFrame = layout::floatingFrame(
              memberX, layout::Window::titlebarHeight + layout::ChannelHeader::height, memberWidth,
              h - layout::Window::titlebarHeight - layout::ChannelHeader::height
          );
          const float memberBottom = memberFrame.y + memberFrame.height - Style::spaceXs;
          const float memberTop = layout::Members::top;
          const float memberHeight = layout::Members::viewportHeight(h);
          const layout::MemberListLayout memberLayout(m_members, m_memberCount);
          m_scrolling.limit(m_memberScroll, memberLayout.maximumScroll(memberHeight));
          floatingPanel(root, memberFrame, background);
          auto members = std::make_unique<Node>();
          members->setPosition(memberX, memberTop);
          members->setSize(memberWidth, memberHeight);
          members->setClipChildren(true);
          const auto guildRoles = m_dmMode ? std::vector<api::ColoredRole>{} : m_roleDirectory.allRoles(m_guildId);
          for (const auto& member : m_members) {
            const float rowY = memberLayout.top(member.index) - m_memberScroll;
            const float rowHeight = member.group.empty() ? layout::Members::rowHeight : layout::Members::groupHeight;
            if (rowY + rowHeight <= 0 || rowY >= memberHeight)
              continue;
            if (!member.group.empty()) {
              // Compact group headings without changing remote subscription indices.
              const auto role = std::ranges::find(guildRoles, member.groupId, &api::ColoredRole::id);
              const float iconWidth = role != guildRoles.end() && (!role->icon.empty() || !role->unicodeEmoji.empty())
                  ? drawRoleBadge(
                        *members, Style::spaceLg + Style::spaceXxs, rowY + layout::Members::groupIconTop, *role
                    )
                  : 0;
              label(
                  *members, Style::spaceLg + Style::spaceXxs + iconWidth, rowY + layout::Members::groupTextTop,
                  memberWidth - layout::Window::titlebarHeight - iconWidth,
                  member.group + (member.count >= 0 ? " — " + std::to_string(member.count) : ""), Style::fontSizeMini,
                  muted, FontWeight::SemiBold
              );
            } else if (!member.user.id.empty()) {
              const bool memberOver = hovered(
                  memberX + Style::spaceMd, memberTop + rowY, memberWidth - Style::spaceXl,
                  layout::Members::rowHitHeight
              );
              if (memberOver)
                rectangle(
                    *members, Style::spaceMd, rowY, memberWidth - Style::spaceXl, layout::Members::rowHitHeight, panel,
                    Style::radiusMd
                );
              drawNameplate(
                  *members, {Style::spaceMd, rowY, memberWidth - Style::spaceXl, layout::Members::rowHitHeight},
                  member.user.id, m_dmMode ? "" : m_guildId, NameplatePlacement::Member
              );
              const auto name = userName(member.user.id, member.user.name);
              const auto status = m_dmMode ? userStatus(member.user.id) : member.status;
              avatar(
                  *members, 16, rowY + (Style::spaceXs + Style::borderWidth), layout::Avatar::small, member.user.avatar,
                  name, member.user.id, "",
                  layout::Frame{Style::spaceMd, rowY, memberWidth - Style::spaceXl, layout::Members::rowHitHeight}
              );
              statusBadge(
                  *members, 38, rowY + ((Style::spaceXl + Style::spaceXxs) + Style::borderWidth), status,
                  memberOver ? panel : background, member.user.id, m_channelId
              );
              roleLabel(
                  *members, 60, rowY + Style::spaceMd, memberWidth - 78, name, Style::fontSizeCaption,
                  status == "offline" ? muted : foreground, FontWeight::Normal, member.user.id,
                  "member:" + member.user.id, memberOver
              );
              const auto rowTop = std::max(memberTop, memberTop + rowY),
                         rowBottom = std::min(memberBottom, memberTop + rowY + layout::Members::rowHitHeight);
              m_contextTargets.push_back(
                  {memberX + Style::spaceMd, rowTop, memberWidth - Style::spaceXl, std::max(0.0F, rowBottom - rowTop),
                   Action::UserMenu, member.user.id}
              );
              m_hits.push_back(
                  {memberX + Style::spaceMd, rowTop, memberWidth - Style::spaceXl, std::max(0.0F, rowBottom - rowTop),
                   Action::UserProfile, member.user.id}
              );
            }
          }
          if (m_members.empty())
            label(
                *members, 18, 8, memberWidth - layout::Window::titlebarHeight,
                m_live ? "Loading members…" : "Connecting…", Style::fontSizeCaption, muted
            );
          root.addChild(std::move(members));
        }
      }
    }
    if (m_resizingSidebar && !sidebarResizeAvailable())
      finishSidebarResize();
    if (m_connected && !m_channelPanel.empty() && !settingsVisible()) {
      const float panel = channelPanelWidth();
      drawChannelPanel(root, w - panel, 102, panel, h - 102);
    }
    // Reuse inverse rounded rectangles to mask pane corners without changing
    // content transforms, native controls or the renderer's clipping model.
    for (const auto& frame : m_floatingFrames) {
      auto mask = std::make_unique<RectNode>();
      mask->setHitTestVisible(false);
      mask->setPosition(frame.x, frame.y);
      mask->setSize(frame.width, frame.height);
      mask->setStyle({.fill = panel, .radius = Style::scaledRadiusXl(), .invertFill = true});
      root.addChild(std::move(mask));
      auto edge = std::make_unique<RectNode>();
      edge->setHitTestVisible(false);
      edge->setPosition(frame.x, frame.y);
      edge->setSize(frame.width, frame.height);
      edge->setStyle(
          {.fill = withAlpha(background, 0),
           .border = raised,
           .radius = Style::scaledRadiusXl(),
           .borderWidth = Style::borderWidth}
      );
      root.addChild(std::move(edge));
    }
    if (m_serverSettings.open)
      drawServerSettings(root, w, h);
    else if (m_settingsOpen)
      drawSettings(root, w, h);
    else if (m_presenceMenu)
      drawPresenceMenu(root, h);
    if (m_settingsOpen && userAccountDialogOpen())
      drawUserAccountDialog(root, w, h);
    if (m_emojiMenu && !settingsVisible() && m_viewImage.empty())
      drawEmojiPicker(root, w, h);
    if (m_gifMenu && !settingsVisible() && m_viewImage.empty())
      drawGifPicker(root, w, h);
    if (mediaViewer) {
      m_pointerX = pointer.first;
      m_pointerY = pointer.second;
      m_tooltip.clear();
      m_contextTargets.clear();
      m_textAreas.clear();
    }
    if (!m_viewImage.empty())
      drawImageViewer(root, w, h);
    if (m_videoExpanded)
      drawVideoViewer(root, w, h);
    if (m_demo && !m_demoOverlayApplied && !m_demoOverlay.empty()) {
      m_demoOverlayApplied = true;
      if (m_demoOverlay.starts_with("channel-menu")
          || m_demoOverlay.starts_with("channel-list-menu")
          || m_demoOverlay.starts_with("channel-category-menu")) {
        const auto id = m_demoOverlay == "channel-menu-readonly" ? "102"
            : m_demoOverlay.starts_with("channel-category-menu") ? "200"
                                                                 : "103";
        const bool empty = m_demoOverlay.starts_with("channel-list-menu");
        const auto target = std::ranges::find_if(m_contextTargets, [&](const auto& hit) {
          return empty ? hit.action == Action::ChannelListMenu : hit.action == Action::ChannelMenu && hit.id == id;
        });
        if (target != m_contextTargets.end())
          openContext(target->x + Style::spaceDense, empty ? 590 : target->y + Style::spaceDense);
        if (m_demoOverlay == "channel-menu-mute"
            || m_demoOverlay == "channel-menu-notifications"
            || m_demoOverlay == "channel-menu-move") {
          const auto kind = m_demoOverlay == "channel-menu-mute" ? ChannelMenuKind::MuteMenu
              : m_demoOverlay == "channel-menu-move"             ? ChannelMenuKind::MoveMenu
                                                                 : ChannelMenuKind::NotifyMenu;
          for (const auto& entry : m_contextEntries)
            if (entry.id >= channelMenuBase && m_channelMenuCommands[entry.id - channelMenuBase].kind == kind) {
              openChannelSubmenu(entry.id, 140);
              break;
            }
        }
      } else if (
          m_demoOverlay == "message-menu" || m_demoOverlay == "reaction-submenu" || m_demoOverlay == "user-menu"
      ) {
        const auto target = std::ranges::find_if(m_contextTargets, [&](const auto& hit) {
          return m_demoOverlay != "user-menu" ? hit.action == Action::MessageMenu && hit.id == "3"
                                              : hit.action == Action::UserMenu && hit.id == "11";
        });
        if (target != m_contextTargets.end()) {
          openContext(target->x + (m_demoOverlay == "user-menu" ? 8 : 110), target->y + Style::spaceSm);
          if (m_demoOverlay == "reaction-submenu")
            openReactionSubmenu(22);
        }
      } else if (m_demoOverlay == "block-user") {
        m_contextMessage = "3";
        m_contextUser = "11";
        m_contextChannel = m_channelId;
        contextAction(Action::BlockUser);
      } else if (m_demoOverlay == "user-profile" || m_demoOverlay.starts_with("profile-card"))
        showProfile(m_demoOverlay == "profile-card-self" ? m_account.id : "11");
      else if (m_demoOverlay == "guild-profile")
        showGuildProfile("99");
    }
    if (m_demoOverlay == "tooltip-long") {
      m_tooltip = "A tooltip sizes itself to its text, wraps long descriptions, and stays inside the window.\nA second "
                  "line is measured too.";
      m_tooltipX = w - Style::spaceSection;
      m_tooltipY = 58;
    }
    if (m_demoOverlay == "reaction-people" || m_demoOverlay == "reaction-loading") {
      const auto chip = std::ranges::find_if(m_hits, [](const auto& hit) {
        return hit.action == Action::ToggleReaction && hit.id.starts_with("3\n");
      });
      if (chip != m_hits.end()) {
        m_pointerX = chip->x + Style::spaceDense;
        m_pointerY = chip->y + Style::spaceMd;
        auto message = std::ranges::find(m_messages, "3", &api::Message::id);
        if (message != m_messages.end()) {
          if (m_demoOverlay == "reaction-people") {
            // Synthetic users only; demo never contacts the network.
            api::Interaction request{
                .kind = api::InteractionKind::ListReactions,
                .channel = m_channelId,
                .message = "3",
                .emoji = "👍",
                .revision = 1
            };
            const auto key = std::to_string(static_cast<int>(request.kind)) + ":" + m_channelId + ":3::👍:normal";
            auto& page = m_reactorPages[key];
            page.revision = 1;
            api::Event result;
            result.interaction = request;
            result.reactors = {{"1", "Alex"}, {"11", "Maya"}, {"12", "Noah"}};
            interactionResult(result);
            api::UserUpdates alias;
            alias.users["11"].nickname = "May";
            m_userDirectory.apply(alias);
          }
          hoverReaction(*message, message->reactions.front(), *chip);
        }
      }
    }
    if (!m_reactionHoveredThisFrame) {
      m_hoverReactionKey.clear();
      m_reactorHoverTimer.stop();
    }
    drawCompletion(root, w, h);
    drawSearchMenu(root, w, h);
    drawInboxButton(root, w);
    drawInbox(root, w, h);
    drawDialog(root, w, h);
    drawContext(root, w, h);
    drawGuildTooltip(root, w, h);
    drawTooltip(root, w, h);
    drawSelectDropdown(root, w, h);
    finishControls();
    m_roleMotion.end();
    if (m_roleMotion.active() || (m_typingVisible && m_window.active() && !m_preferences.reducedMotion))
      m_window.invalidate();
    updateChatPreload();
    updateCursor();
    m_window.context().renderScene(m_window.target(), m_scene.get());
    if (!m_videoVisible || settingsVisible() || !m_viewImage.empty()) {
      m_videoPlayRequested = false;
      if (m_video.state().playing)
        m_video.pause(true);
    }
    if (m_videoContext != EGL_NO_CONTEXT && m_videoVisible) {
      eglMakeCurrent(m_window.graphics().display(), EGL_NO_SURFACE, EGL_NO_SURFACE, m_videoContext);
      m_video.presented();
      m_window.context().makeCurrent(m_window.target());
    }
    if (m_animationDelay != std::chrono::milliseconds::max())
      m_animationTimer.start(m_animationDelay, [this] { m_window.invalidate(); });
    else
      m_animationTimer.stop();
  }

  std::string ClientApplication::userName(const std::string& id, const std::string& fallback) const {
    return m_userDirectory.name(id, fallback);
  }

  std::string ClientApplication::userStatus(const std::string& id, const std::string& fallback) const {
    if (id == m_account.id)
      return m_presence.status == "invisible" ? "offline" : m_presence.status;
    return m_userDirectory.status(id, fallback);
  }

  std::string ClientApplication::channelName(const api::Channel& channel) const {
    if ((channel.type != 1 && channel.type != 3) || (channel.type == 3 && channel.named) || channel.recipients.empty())
      return channel.name;
    std::string name;
    for (const auto& recipient : channel.recipients) {
      if (!name.empty())
        name += ", ";
      name += userName(recipient.id, recipient.name);
    }
    return name;
  }

  void ClientApplication::updateDmMembers() {
    if (!m_dmMode)
      return;
    m_members.clear();
    m_activeMemberIdentity.clear();
    const auto channel = std::ranges::find(m_channels, m_channelId, &api::Channel::id);
    if (channel != m_channels.end()) {
      int index = 0;
      for (const auto& user : channel->recipients)
        if (user.id != m_account.id)
          m_members.push_back({index++, user, userStatus(user.id)});
      if (!m_account.id.empty())
        m_members.push_back({index, m_account, userStatus(m_account.id)});
    }
    m_memberCount = m_members.size();
    refreshDmProfile();
  }

  void ClientApplication::statusBadge(
      Node& parent, float x, float y, const std::string& status, Color backdrop, const std::string& user,
      const std::string& channel
  ) {
    if (userTyping(user, channel)) {
      auto badge = std::make_unique<Node>();
      badge->setPosition(x + layout::TypingBadge::height - layout::TypingBadge::width, y);
      badge->setSize(layout::TypingBadge::width, layout::TypingBadge::height);
      rectangle(*badge, 0, 0, badge->width(), badge->height(), backdrop, badge->height() / 2);
      rectangle(
          *badge, layout::TypingBadge::inset, layout::TypingBadge::inset,
          badge->width() - layout::TypingBadge::inset * 2, badge->height() - layout::TypingBadge::inset * 2,
          theme::muted, (badge->height() - layout::TypingBadge::inset * 2) / 2
      );
      drawTypingDots(
          *badge, (badge->width() - layout::TypingBadge::dotsWidth) / 2,
          (badge->height() - layout::TypingBadge::dotSize) / 2, layout::TypingBadge::dotSize,
          layout::TypingBadge::dotStep, backdrop
      );
      parent.addChild(std::move(badge));
      return;
    }
    rectangle(parent, x, y, 12, 12, backdrop, Style::radiusMd);
    rectangle(parent, x + Style::spaceXxs, y + Style::spaceXxs, 8, 8, presenceColor(status), 4);
    if (status == "offline" || status == "unknown" || status == "invisible")
      rectangle(parent, x + Style::spaceXs, y + Style::spaceXs, 4, 4, backdrop, Style::radiusXs);
    else if (status == "dnd")
      rectangle(
          parent, x + (Style::spaceXxs + Style::borderWidth), y + (Style::spaceXs + Style::borderWidth), 6, 2, backdrop,
          1
      );
    else if (status == "idle")
      rectangle(parent, x + Style::borderWidth, y + Style::borderWidth, 6, 6, backdrop, Style::radiusSm);
  }

  void ClientApplication::connect() {
    if (m_connecting)
      return;
    if (m_token.empty()) {
      m_status = "Paste your account token to connect.";
      m_window.invalidate();
      return;
    }
    m_status.clear();
    m_restoringLogin = m_retrySavedLogin = false;
    m_connecting = true;
    m_guildUpdates.clear();
    resetGuildBootstrap();
    resetReadAcknowledgements();
    m_session = m_client.connect(std::move(m_token));
    m_token.clear();
    if (m_inputs.contains("token"))
      m_inputs["token"]->setValue("");
    m_selectAll = false;
    m_window.invalidate();
  }

  void ClientApplication::restoreLogin() {
    if (m_demo || m_connecting)
      return;
    m_login.cancel();
    m_showTokenEntry = false;
    m_fieldFocused = false;
    m_connecting = m_restoringLogin = true;
    m_retrySavedLogin = false;
    m_status = "Your desktop keyring may ask you to unlock it.";
    m_guildUpdates.clear();
    resetGuildBootstrap();
    resetReadAcknowledgements();
    m_session = m_client.restoreLogin();
    m_window.invalidate();
  }

  void ClientApplication::beginLogin() {
    if (m_connecting || m_login.active())
      return;
    m_showTokenEntry = false;
    m_fieldFocused = false;
    m_status = m_login.start() ? "Complete sign-in in your browser. Allow local access if prompted."
                               : "Could not open your browser. Check your default browser settings.";
    m_window.invalidate();
  }

  void ClientApplication::demo() {
    finishSidebarResize();
    closeVideo();
    clearEmojiState();
    m_scrolling.stopAll();
    clearGifs();
    clearInteractions();
    m_login.cancel();
    resetReadAcknowledgements();
    m_client.disconnect();
    clearThreads();
    m_demo = true;
    m_connecting = m_restoringLogin = m_retrySavedLogin = false;
    m_loginStorageStatus.clear();
    m_userDirectory = {};
    m_roleDirectory = {};
    m_messageLog.reset();
    m_roleMotion.clear();
    m_preferences = {};
    m_folderMotion.clear();
    m_demoPrepared = false;
    m_demoOverlay.clear();
    m_demoOverlayApplied = false;
    m_connected = true;
    m_connecting = false;
    m_token.clear();
    m_status.clear();
    m_account = {"1", "Alex", {}, "alex", "Alex", "Building a native Discord client.", "they/them"};
    m_presence = {"online"};
    m_guilds = {{"1", "The Common Room"}, {"2", "Creative Corner"}, {"3", "After Hours"}};
    m_folders = {{"10", "Hangouts", 0x8F80D4, {"1", "2"}}, {{}, {}, 0, {"3"}}};
    m_openFolders = {"10"};
    selectGuild("1");
  }

  bool ClientApplication::readingChannel(const std::string& channel) const {
    return m_connected
        && channel == m_channelId
        && !m_inboxOpen
        && !forumView()
        && (m_window.active() || (m_demo && m_demoOverlay == "unread-cycle"))
        && !settingsVisible()
        && m_dialogKind.empty()
        && m_viewImage.empty()
        && m_history.atLatest()
        && (m_demo || m_history.initialized())
        && !m_loadingMessages
        && m_messageScroll <= 1;
  }
  bool ClientApplication::markChannelRead(bool explicitAction) {
    if (m_channelId.empty() || m_messages.empty() || !m_history.atLatest() || (!m_demo && !m_history.initialized()))
      return false;
    const auto publicMessage = std::ranges::find_if(m_messages.rbegin(), m_messages.rend(), [](const auto& message) {
      return !message.deleted && !(message.flags & 64);
    });
    if (publicMessage == m_messages.rend())
      return false;
    const auto& latest = publicMessage->id;
    if (!explicitAction && !MessageHistory::less(m_localRead[m_channelId], latest))
      return false;
    return acknowledgeChannelRead(m_channelId, latest, !explicitAction);
  }
  bool
  ClientApplication::acknowledgeChannelRead(const std::string& channel, const std::string& message, bool automatic) {
    if (!api::validSnowflake(channel) || !api::validSnowflake(message) || m_deletedChannels.contains(channel))
      return false;
    if (automatic) {
      if (!m_readAcknowledgements.queue(channel, message))
        return false;
    } else {
      if (!m_demo && !m_client.acknowledge(channel, message))
        return false;
      m_readAcknowledgements.acknowledged(channel, message, ReadAcknowledgements::Clock::now());
    }
    auto& local = m_localRead[channel];
    if (MessageHistory::less(local, message))
      local = message;
    m_unreadTimeline.acknowledged(channel, message);
    m_unreadChannels.erase(channel);
    m_unreadDMs.erase(channel);
    dispatchReadAcknowledgements();
    return true;
  }
  void ClientApplication::dispatchReadAcknowledgements() {
    m_readAcknowledgementTimer.stop();
    if (!m_connected) {
      m_readAcknowledgements.reset();
      return;
    }
    const auto now = ReadAcknowledgements::Clock::now();
    if (const auto pending = m_readAcknowledgements.take(now)) {
      const auto local = m_localRead.find(pending->channel);
      if (!m_deletedChannels.contains(pending->channel)
          && m_unreadGuilds.contains(pending->channel)
          && local != m_localRead.end()
          && local->second == pending->message
          && !m_demo
          && !m_client.acknowledge(pending->channel, pending->message)) {
        failedReadAcknowledgement(pending->channel, pending->message);
        m_status = "Could not mark this channel as read. Try again.";
        m_window.invalidate();
      }
    }
    if (const auto delay = m_readAcknowledgements.delay(now)) {
      const auto session = m_session;
      const auto account = m_account.id;
      m_readAcknowledgementTimer.start(*delay, [this, session, account] {
        if (session != m_session || account != m_account.id)
          return;
        dispatchReadAcknowledgements();
      });
    }
  }
  void ClientApplication::cancelAutomaticRead(const std::string& channel) {
    m_readAcknowledgements.cancel(channel);
    if (!m_readAcknowledgements.pending())
      m_readAcknowledgementTimer.stop();
  }
  void ClientApplication::resetReadAcknowledgements() {
    m_readAcknowledgementTimer.stop();
    m_readAcknowledgements.reset();
  }
  void ClientApplication::failedReadAcknowledgement(const std::string& channel, const std::string& message) {
    m_unreadTimeline.failed(channel, message);
    if (m_unreadTimeline.cursor(channel)) {
      m_unreadChannels.insert(channel);
      if (std::ranges::find(m_privateChannels, channel, &api::Channel::id) != m_privateChannels.end())
        m_unreadDMs.insert(channel);
    }
  }
  void ClientApplication::cacheChannel() {
    if (m_channelId.empty() || m_deletedChannels.contains(m_channelId))
      return;
    if (m_history.atLatest() && (m_history.initialized() || !m_messages.empty())) {
      const auto first = m_messages.size() > 200 ? m_messages.size() - 200 : 0;
      m_channelMessages[m_channelId] = {m_messages.begin() + first, m_messages.end()};
      if (first)
        m_channelHistoryState[m_channelId].olderEnd = false;
    } else {
      m_channelMessages.erase(m_channelId);
      m_channelHistoryState.erase(m_channelId);
    }
    if (!m_dmMode && !m_members.empty())
      if (const auto identity = channelMemberIdentity(m_channelId))
        if (identity->signature == m_activeMemberIdentity)
          m_channelMembers[m_channelId] = {m_members, m_memberCount, identity->signature, m_memberEpoch};
    touchChannelCache(m_channelId);
  }
  void ClientApplication::touchChannelCache(const std::string& id) {
    std::erase(m_channelCacheOrder, id);
    m_channelCacheOrder.push_back(id);
    while (m_channelCacheOrder.size() > 24) {
      m_channelMessages.erase(m_channelCacheOrder.front());
      m_channelHistoryState.erase(m_channelCacheOrder.front());
      m_channelMembers.erase(m_channelCacheOrder.front());
      m_channelCacheOrder.erase(m_channelCacheOrder.begin());
    }
  }
  bool ClientApplication::restoreCachedHistory(const std::string& id) {
    const auto cached = m_channelMessages.find(id);
    if (cached == m_channelMessages.end())
      return false;
    m_messages = cached->second;
    const auto state = m_channelHistoryState.find(id);
    const auto channel = selectedChannel();
    if (state == m_channelHistoryState.end() || !channel || m_deletedChannels.contains(id))
      return false;
    if (!m_dmMode) {
      const auto permissions = channelPermissionsFor(channel->isThread() ? channel->parentId : id);
      if (!permissions || !(*permissions & api::viewChannel) || !(*permissions & (1ULL << 16)))
        return false;
    }
    auto latest = channel->lastMessageId;
    if (const auto unread = m_unreadTimeline.cursor(id); unread && MessageHistory::less(latest, unread->latest))
      latest = unread->latest;
    if (!state->second.fresh(m_live, m_chatCacheEpoch, latest, m_messages))
      return false;
    m_history.restoreLatest(m_messages, state->second.olderEnd);
    m_historyEpoch = state->second.epoch;
    touchChannelCache(id);
    return true;
  }
  std::optional<api::MemberListIdentity> ClientApplication::channelMemberIdentity(const std::string& id) const {
    if (m_dmMode)
      return {};
    const auto channel = std::ranges::find(m_channels, id, &api::Channel::id);
    if (channel == m_channels.end())
      return {};
    if (channel->isThread()) {
      if (channelMention(m_guildId, id).link.empty())
        return {};
      return api::MemberListIdentity{m_guildId + "/thread/" + id, id, true};
    }
    if (!(channelPermissionsFor(id).value_or(0) & api::viewChannel))
      return {};
    for (const auto& role : m_roleDirectory.allRoles(m_guildId))
      if (role.id == m_guildId && role.permissions)
        return api::memberListIdentity(m_guildId, *role.permissions, *channel);
    return {};
  }
  void ClientApplication::refreshMemberSubscription() {
    if (m_demo || m_dmMode || m_channelId.empty())
      return;
    const auto identity = channelMemberIdentity(m_channelId);
    if (!m_activeMemberIdentity.empty() && (!identity || identity->signature != m_activeMemberIdentity)) {
      m_members.clear();
      m_memberCount = 0;
      m_activeMemberIdentity.clear();
      m_channelMembers.erase(m_channelId);
    }
    m_client.subscribe(m_guildId, m_channelId, m_memberPage, identity);
  }
  void ClientApplication::selectGuild(const std::string& id) {
    if (m_dmMode || m_guildId != id || m_channels.empty())
      clearTyping();
    m_pendingDiscordLink.reset();
    m_jumpHighlightTimer.stop();
    m_jumpHighlightMessage.clear();
    clearChatPreload();
    m_startupGuild.clear();
    closeCompletion();
    cancelMessageEdit(false);
    finishSidebarResize();
    closeChannelPanel();
    m_roleMotion.clear();
    closeVideo();
    m_scrolling.stopAll();
    if (!m_dmMode && m_guildId == id && !m_channels.empty())
      return;
    cacheChannel();
    m_restoreChannel = m_lastChannels.contains(id) ? m_lastChannels[id]
        : m_demo                                   ? ""
                                                   : m_sessionStore.channelForGuild(m_account.id, id);
    m_dmMode = false;
    m_guildId = id;
    m_channelListPreferences = m_demo ? ChannelListPreferences{} : m_sessionStore.channelList(m_account.id, id);
    if (const auto hidden = m_userDirectory.guildPreferences(id).hideMutedChannels)
      m_channelListPreferences.hideMuted = *hidden;
    m_forumSearchTimer.stop();
    m_threadParent.clear();
    m_openThreadQuery.reset();
    closeDialog();
    m_channelId.clear();
    m_unreadTimeline.open({});
    m_channels.clear();
    m_messages.clear();
    m_messageLayouts.clear();
    m_members.clear();
    m_activeMemberIdentity.clear();
    m_memberCount = 0;
    m_memberScroll = 0;
    m_memberPage = 0;
    m_liveChanges.clear();
    m_channelScroll = 0;
    m_messageScroll = 0;
    m_history.reset();
    m_adoptedChatPreload.reset();
    m_timelineAnchor.reset();
    m_loadingMessages = false;
    m_status.clear();
    if (m_demo) {
      m_channels = {{"101", "general"}, {"102", "introductions"}, {"103", "projects"}, {"104", "off-topic"}};
      selectChannel("101");
    } else {
      if (auto cached = m_guildChannels.find(id); cached != m_guildChannels.end()) {
        m_channels = cached->second;
        rebuildThreadChannels();
        auto selected = std::ranges::find(m_channels, m_restoreChannel, &api::Channel::id);
        const auto restore = m_restoreChannel;
        if (selected == m_channels.end())
          selected = std::ranges::find_if(m_channels, [](const auto& c) { return c.type != 4 && !c.isForum(); });
        if (selected != m_channels.end())
          selectChannel(selected->id);
        m_restoreChannel.clear();
        if (!restore.empty() && std::ranges::find(m_channels, restore, &api::Channel::id) == m_channels.end())
          openThread(restore);
        m_loadingChannels = false;
        m_window.invalidate();
        return;
      }
      m_loadingChannels = m_readyBootstrapPending || m_client.channels(id);
      if (!m_loadingChannels)
        m_status = "Could not load channels. Try selecting the server again.";
    }
    m_window.invalidate();
  }

  void ClientApplication::showDirectMessages(bool home) {
    clearTyping();
    m_pendingDiscordLink.reset();
    m_jumpHighlightTimer.stop();
    m_jumpHighlightMessage.clear();
    clearChatPreload();
    m_startupGuild.clear();
    closeCompletion();
    cancelMessageEdit(false);
    finishSidebarResize();
    closeChannelPanel();
    m_roleMotion.clear();
    closeVideo();
    m_scrolling.stopAll();
    cacheChannel();
    if (m_restoreChannel.empty())
      m_restoreChannel = m_lastChannels.contains("@me") ? m_lastChannels["@me"]
          : m_demo                                      ? ""
                                                        : m_sessionStore.channelForGuild(m_account.id, "");
    m_dmMode = true;
    m_dmPage = home ? "friends" : "";
    m_dmPreviewId.clear();
    m_dmPreview.clear();
    m_dmPreviewLoading = false;
    m_dmPreviewBounds = {};
    m_scrolling.stop(m_dmPreviewScroll);
    m_dmPreviewScroll = 0;
    m_dmConfirm.clear();
    m_channelListPreferences = {};
    m_openThreadQuery.reset();
    m_forumSearchTimer.stop();
    closeDialog();
    m_guildId.clear();
    m_channelId.clear();
    m_unreadTimeline.open({});
    m_channels = dmConversations();
    m_messages.clear();
    m_messageLayouts.clear();
    m_members.clear();
    m_activeMemberIdentity.clear();
    m_memberCount = 0;
    m_channelScroll = 0;
    m_liveChanges.clear();
    if (!m_demo && !m_readyBootstrapPending && !m_privateReady && m_privateChannels.empty() && !m_loadingPrivate)
      m_loadingPrivate = m_client.privateChannels();
    if (!home && !m_channels.empty()) {
      auto selected = std::ranges::find(m_channels, m_restoreChannel, &api::Channel::id);
      selectChannel(selected == m_channels.end() ? m_channels.front().id : selected->id);
      m_restoreChannel.clear();
    } else if (!home && m_channels.empty() && m_privateReady) {
      m_dmPage = "friends";
      m_dmFilter = "online";
      m_restoreChannel.clear();
      if (!m_demo)
        m_client.subscribe({}, {});
    }
    m_window.invalidate();
  }

  void ClientApplication::selectChannel(std::string id) {
    closeSearchMenu();
    if (id != m_channelId)
      m_adoptedChatPreload.reset();
    if (id != m_chatPreloadChannel)
      clearChatPreload();
    else
      m_chatPreloadTimer.stop();
    if (m_dmMode) {
      const auto request = std::ranges::find(m_privateChannels, id, &api::Channel::id);
      if (request != m_privateChannels.end() && request->isMessageRequest()) {
        openDmPage("requests");
        dmAction("open-request:" + id);
        return;
      }
      m_dmPage.clear();
      m_dmPreviewId.clear();
      m_dmPreview.clear();
    }
    closeCompletion();
    cancelMessageEdit(false);
    finishSidebarResize();
    closeChannelPanel();
    m_seekMessage.clear();
    m_jumpHighlightTimer.stop();
    m_jumpHighlightMessage.clear();
    m_pendingMessageJump.reset();
    const auto selected = std::ranges::find(m_channels, id, &api::Channel::id);
    const bool forum = selected != m_channels.end() && selected->isForum();
    m_forumSearchTimer.stop();
    m_threadParent.clear();
    m_openThreadQuery.reset();
    m_roleMotion.clear();
    closeVideo();
    m_scrolling.stopAll();
    closeGifPicker();
    closeContext();
    closeDialog();
    m_reactionMessage.clear();
    m_reactionChannel.clear();
    m_emojiMenu = false;
    if (m_channelId == id)
      return;
    if (id != m_channelId)
      cacheChannel();
    m_controlInput.setFocus(nullptr);
    m_focusedInput = "composer";
    m_channelId = id;
    clearTyping();
    if (!m_dmMode)
      rebuildThreadChannels();
    m_unreadTimeline.open(id);
    m_lastChannels[m_guildId.empty() ? "@me" : m_guildId] = id;
    m_edit.end(m_drafts[id]);
    m_attachmentScroll = 0;
    m_revealedSpoilers.clear();
    m_selectedMessage.clear();
    m_selectedText.clear();
    m_dragDraft = m_dragMessage = false;
    m_messages.clear();
    m_messageLayouts.clear();
    m_members.clear();
    m_activeMemberIdentity.clear();
    m_memberCount = 0;
    m_memberScroll = 0;
    m_memberPage = 0;
    m_liveChanges.clear();
    m_messageScroll = 0;
    m_history.reset();
    m_adoptedChatPreload.reset();
    m_timelineAnchor.reset();
    m_selectAll = false;
    m_fieldFocused = true;
    m_status.clear();
    m_loadingMessages = false;
    if (readingChannel(id)) {
      m_unreadDMs.erase(id);
      m_unreadChannels.erase(id);
    }
    for (const auto& row : channelListRows(
             m_channels, m_collapsedCategories, m_channelId, m_channelListPreferences,
             [&](const auto& id) { return channelMuted(id); },
             [&](const auto& channel) { return m_unreadTimeline.mentions(channel); },
             [&](const auto& channel) { return m_dmMode || serverChannelVisible(channel); }
         )) {
      if (row.channel->id == id) {
        const float sceneHeight = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
        const float listTop = m_dmMode ? layout::DmSidebar::channelsTop : layout::Sidebar::channelsTop;
        const auto view =
            std::max(row.advance, layout::accountFrame(sceneHeight, chatLeft()).y - layout::Floating::gap - listTop);
        m_channelScroll = std::clamp(m_channelScroll, std::max(0.0F, row.top - view + row.advance), row.top);
        break;
      }
    }
    updateDmMembers();
    if (id.empty()) {
      m_window.invalidate();
      return;
    }
    const auto loadDetails = [&] {
      if (!m_demo && !m_dmMode) {
        auto channel = selectedChannel();
        if (channel && channel->isThread())
          channel = serverChannel(m_guildId, channel->parentId);
        if (channel
            && !m_channelDetailsLoaded.contains(channel->id)
            && (!channel->topic || !channel->overwrites)
            && !m_channelDetailRequests.contains(channel->id)
            && m_client.channelDetails(channel->id))
          m_channelDetailRequests.insert(channel->id);
      }
    };
    if (forum) {
      m_threadParent = id;
      m_threadTab = api::ThreadList::Active;
      m_forumSearch.clear();
      m_forumTag.clear();
      m_scrolling.stop(m_forumScroll);
      m_forumScroll = 0;
      m_threadRequests.erase(id);
      m_threadsLoaded.erase(id);
      m_threadErrors.erase(id);
      m_threadPages.erase(id);
      if (!m_demo) {
        m_client.subscribe(m_guildId, id);
        loadThreads();
        loadDetails();
        saveSelection();
      }
      m_window.invalidate();
      return;
    }
    const bool cachedHistory = restoreCachedHistory(id);
    const auto memberIdentity = channelMemberIdentity(id);
    if (!m_demo)
      m_client.subscribe(m_guildId, id, 0, memberIdentity);
    const bool adoptedPreload = !cachedHistory && adoptChatPreload(id);
    clearChatPreload();
    if (m_demo && m_demoOverlay != "chat-preload-cycle") {
      m_messages = {
          {"1", id, "Maya", "Made a little space for our ideas. Welcome in! 🌿 ✨", "2026-10-17T09:41:00Z"},
          {"2", id, "Noah", "This already feels like home. What is everyone working on?", "2026-10-17T09:43:00Z"},
          {"3", id, "Alex",
           "A **native desktop client**, with *formatted messages*.\n```cpp\nint main() {\n  return 0;\n}\n```",
           "2026-10-17T09:45:00Z"},
          {"4", id, "Maya",
           "A little __underline__, ~~a correction~~, and `inline code`.\n> A good conversation deserves a good place "
           "to live.",
           "2026-10-17T09:47:00Z"},
          {"5", id, "Noah", "The basics first: our servers, our channels, and the people in them.",
           "2026-10-17T09:49:00Z"},
      };
      api::Embed preview;
      preview.provider = "Project notes";
      preview.title = "Native message previews";
      preview.description = "Embeds support **formatted descriptions**, fields, and images.";
      preview.fields = {{"Rendering", "Native Wayland / GLES"}, {"Language", "C++23"}};
      preview.footer = "An offline example";
      m_messages.back().embeds.push_back(std::move(preview));
      m_members = {
          {0, {}, "", "Online", 3},
          {1, {"11", "Maya"}, "online"},
          {2, {"12", "Noah"}, "idle"},
          {3, {"1", "Alex"}, "online"}
      };
      m_memberCount = 4;
    } else if (!m_demo) {
      if (!m_dmMode && m_live && memberIdentity)
        for (const auto& [channel, cached] : m_channelMembers)
          if (cached.epoch == m_chatCacheEpoch && cached.identity == memberIdentity->signature) {
            m_members = cached.rows;
            m_memberCount = cached.count;
            m_activeMemberIdentity = cached.identity;
            m_memberEpoch = cached.epoch;
            break;
          }
      if (!cachedHistory && !adoptedPreload)
        refresh();
      loadDetails();
      saveSelection();
    }
    m_window.invalidate();
  }

  void ClientApplication::refresh() {
    if (forumView() || m_demo || !m_connected || m_channelId.empty() || m_loadingMessages || !m_history.atLatest())
      return;
    loadHistory(api::HistoryDirection::Latest);
  }

  void ClientApplication::loadHistory(api::HistoryDirection direction, bool retry) {
    if (forumView()
        || (m_demo && m_demoOverlay != "history-cycle" && m_demoOverlay != "jump-latest-cycle")
        || !m_connected
        || m_channelId.empty())
      return;
    const auto query = m_history.begin(direction, m_messages, retry);
    if (!query)
      return;
    if (m_demo || m_client.messages(m_channelId, *query)) {
      m_historyEpoch = m_chatCacheEpoch;
      if (m_demo)
        m_demoHistoryQuery = *query;
      m_liveChanges.clear();
      if (direction == api::HistoryDirection::Latest || direction == api::HistoryDirection::Around)
        m_loadingMessages = true;
      if (direction == api::HistoryDirection::Around)
        m_seekMessage = query->cursor;
    } else
      m_history.fail(*query, "Could not load message history. Try again.");
    m_window.invalidate();
  }

  void ClientApplication::applyHistory(api::Event& event) {
    if (!event.history || event.resource != m_channelId || !m_history.accepts(*event.history))
      return;
    if (event.history->direction == api::HistoryDirection::Latest
        || event.history->direction == api::HistoryDirection::Around)
      m_loadingMessages = false;
    if (!event.error.empty()) {
      m_history.fail(*event.history, event.error);
      m_adoptedChatPreload.reset();
      if (m_live && m_history.atLatest() && m_historyEpoch != m_chatCacheEpoch)
        refresh();
      return;
    }
    const auto prior = m_preferences.logEditHistory ? m_messages : std::vector<api::Message>{};
    std::string first, last;
    for (const auto& message : event.messages) {
      m_messageLayouts.erase(message.id);
      if (first.empty() || MessageHistory::less(message.id, first))
        first = message.id;
      if (last.empty() || MessageHistory::less(last, message.id))
        last = message.id;
    }
    if (m_history.apply(*event.history, m_messages, std::move(event.messages), m_liveChanges)) {
      if (m_adoptedChatPreload && m_adoptedChatPreload->visible == *event.history) {
        for (const auto& change : m_adoptedChatPreload->reactions)
          for (auto& message : m_messages)
            if (message.id == change.message)
              api::applyReaction(message, change, m_account.id);
        m_adoptedChatPreload.reset();
      }
      if (m_history.atLatest())
        m_channelHistoryState[m_channelId] = {
            ChatCacheState::Clock::now(), m_historyEpoch, m_history.olderEnd() && m_messages.size() <= 200
        };
      for (auto& message : m_messages) {
        const auto before = std::ranges::find(prior, message.id, &api::Message::id);
        if (before != prior.end()) {
          if (MessageLog::stale(*before, message))
            message = *before;
          else
            logMessageEdit(*before, message);
        }
      }
      m_messageLog.merge(m_channelId, m_messages, m_history.olderEnd(), m_history.atLatest(), first, last);
      m_history.trim(m_messages, event.history->direction == api::HistoryDirection::Older);
      m_keepHistoryAnchor = event.history->direction == api::HistoryDirection::Newer;
      std::set<std::string> retained;
      for (const auto& message : m_messages)
        retained.insert(message.id);
      std::erase_if(m_messageLayouts, [&](const auto& item) { return !retained.contains(item.first); });
    }
    if (m_live && m_history.atLatest() && m_historyEpoch != m_chatCacheEpoch)
      refresh();
  }

  void ClientApplication::demoHistoryStep() {
    const auto step = ++m_demoInteractionStep;
    auto require = [](bool value, const char* error) {
      if (!value)
        throw std::runtime_error(error);
    };
    require(
        std::ranges::any_of(
            m_scene->children(),
            [&](const auto& node) {
              return node->clipChildren()
                  && node->x() == chatLeft() + Style::spaceXl
                  && node->y() == m_messageViewportTop
                  && node->textScrollOffset() == m_messageScroll;
            }
        ),
        "Scrolling panes preserve their logical displacement while text uses final pixel alignment"
    );
    auto visibleRow = [&]() -> TimelineAnchor::Row {
      for (const auto& message : m_messages) {
        const float y = m_messageTops.at(message.id);
        if (y >= m_messageViewportTop && y < m_messageViewportTop + 120)
          return {message.id, y};
      }
      throw std::runtime_error("History replay needs a visible message anchor");
    };
    if (m_demoHistoryAnchor) {
      const auto at = m_messageTops.find(m_demoHistoryAnchor->id);
      require(
          at != m_messageTops.end() && std::abs(at->second - m_demoHistoryAnchor->y) < 0.1F,
          "A page or live message arrival moved the visible message"
      );
      m_demoHistoryAnchor.reset();
    }
    if (m_demoHistoryQuery) {
      const auto query = *std::exchange(m_demoHistoryQuery, {});
      if (query.direction != api::HistoryDirection::Latest)
        m_demoHistoryAnchor = visibleRow();
      const int cursor = query.cursor.empty() ? 12000 : std::stoi(query.cursor);
      const int first = query.direction == api::HistoryDirection::Newer ? cursor + 1 : std::max(2000, cursor - 100);
      const int count = query.direction == api::HistoryDirection::Newer ? std::clamp(12000 - first, 0, 100)
          : cursor <= 2000                                              ? 0
                                                                        : cursor - first;
      api::Event event;
      event.operation = api::Operation::Messages;
      event.resource = m_channelId;
      event.history = query;
      for (int i = 0; i < count; ++i)
        event.messages.push_back(
            {std::to_string(first + i), m_channelId, "Maya",
             "A paginated conversation.\nVisible messages keep their place as history arrives.", "2026-10-17T09:41:00Z"}
        );
      applyHistory(event);
      if (query.direction == api::HistoryDirection::Older)
        ++m_demoHistoryPages;
      require(m_messages.size() <= MessageHistory::windowLimit, "History memory window must remain bounded");
    } else if (m_demoHistoryPhase < 2) {
      if (m_demoHistoryPages >= 12)
        m_demoHistoryPhase = 1;
      if (m_demoHistoryPhase == 1 && m_history.atLatest())
        m_demoHistoryPhase = 2;
      if (m_demoHistoryPhase < 2) {
        const auto* bar = static_cast<Scrollbar*>(m_widgets.at("chat-scrollbar"));
        require(bar->visible(), "History replay needs the native chat scrollbar");
        const auto* thumb = bar->children()[1].get();
        const float x = bar->x() + bar->width() / 2;
        const float chrome = m_window.titlebarHeight() - layout::Window::titlebarHeight;
        const float y = bar->y() + thumb->y() + thumb->height() / 2 + chrome;
        m_window.motion(x, y);
        m_window.click(x, y, 272);
        m_window.motion(x, m_demoHistoryPhase == 0 ? bar->y() - 100 + chrome : m_window.height() * 2);
        m_window.released();
      } else {
        m_scrolling.stop(m_messageScroll);
        m_messageScroll = m_messageMaxScroll / 2;
        m_timelineAnchor.reset();
      }
    } else if (m_demoHistoryPhase == 2) {
      m_demoHistoryAnchor = visibleRow();
      upsertMessage({"12000", m_channelId, "Alex", "A live message while reading history.", "2026-10-17T09:42:00Z"});
      m_demoHistoryPhase = 3;
    } else if (m_demoHistoryPhase == 3) {
      // Exercise both directions through the actual wheel callback, checking
      // a stable reference row rather than an offset that reflow can rebase.
      const bool down = m_demoHistoryMotion >= 8;
      if (m_demoHistoryMotion % 8 == 0) {
        m_demoMotionAnchor = visibleRow();
        m_window.motion(450, m_messageViewportTop + 100 + m_window.titlebarHeight() - layout::Window::titlebarHeight);
        m_window.scroll(down ? 120 : -120);
      } else if (m_demoMotionAnchor) {
        const float y = m_messageTops.at(m_demoMotionAnchor->id);
        require(
            down ? y <= m_demoMotionAnchor->y + 0.05F : y >= m_demoMotionAnchor->y - 0.05F,
            "Visible chat rows moved against the requested wheel direction"
        );
        m_demoMotionAnchor->y = y;
      }
      ++m_demoHistoryMotion;
      if (m_demoHistoryMotion >= 16)
        m_demoHistoryPhase = 4;
    }
    if (step == 64)
      require(
          m_demoHistoryPhase == 4 && m_demoHistoryPages >= 12,
          "History replay did not finish older/newer paging and wheel motion"
      );
    std::clog
        << "Offline history frame "
        << step
        << " older_pages "
        << m_demoHistoryPages
        << " rows "
        << m_messages.size()
        << " phase "
        << m_demoHistoryPhase
        << '\n';
    m_window.invalidate();
  }

  void ClientApplication::send() {
    if (!canSendToThread())
      return;
    if ((m_demo && !m_demoOverlay.starts_with("outgoing-cycle")) || !m_connected || m_channelId.empty())
      return;
    const auto& draft = m_drafts[m_channelId];
    std::vector<api::Upload> uploads;
    for (const auto& attachment : m_attachments[m_channelId])
      uploads.push_back(attachment.upload);
    if (draft.empty() && uploads.empty() && m_draftStickers[m_channelId].empty())
      return;
    if (!api::randomizeMediaFilenames(uploads, m_preferences.randomizeMediaFilenames)) {
      m_status = "Could not generate attachment filenames. Please try again.";
      m_window.invalidate();
      return;
    }
    auto content = m_emojiCatalog.messageContent(
        m_preferences.convertEmoticons ? convertMessageEmoticons(draft) : draft, m_dmMode ? "" : m_guildId,
        m_account.premiumType.value_or(0), m_preferences.emojiImageLinks
    );
    std::vector<std::string> stickerIds;
    for (const auto& selected : m_draftStickers[m_channelId]) {
      auto sticker = selected;
      for (const auto& [_, values] : m_stickers)
        if (const auto found = std::ranges::find(values, selected.id, &api::Sticker::id); found != values.end()) {
          sticker = *found;
          break;
        }
      if (const auto reason = stickerRestriction(sticker); !reason.empty()) {
        m_status = reason;
        m_window.invalidate();
        return;
      }
      const bool external = m_dmMode || (selectedChannelPermissions().value_or(0) & (1ULL << 37));
      if (api::stickerUnavailable(sticker, m_dmMode ? "" : m_guildId, m_account.premiumType.value_or(0), external)
              .empty())
        stickerIds.push_back(sticker.id);
      else {
        if (!content.empty())
          content += '\n';
        content += api::stickerImageLink(sticker);
      }
    }
    queueOutgoing(std::move(content), std::move(uploads), std::move(stickerIds), m_draftStickers[m_channelId]);
    m_window.invalidate();
  }

  void ClientApplication::tick() {
    if (m_instance)
      if (auto activation = m_instance->poll())
        trayEvent({.command = TrayCommand::Open, .activationToken = std::move(*activation)});
    while (auto event = m_tray.poll())
      trayEvent(std::move(*event));
    if (m_demo && m_demoOverlay.starts_with("tray-") && !m_window.visible())
      ++m_trayHiddenTicks;
    drainDesktopNotifications();
    drainServerSettingsUploads();
    if (m_soundPreview.drain())
      m_window.invalidate();
    if (!m_soundPreviewSource.empty()
        && (!m_serverSettings.open || m_serverSettings.page != "soundboard" || (!m_demo && !m_window.active())))
      stopServerSound();
    if (m_soundPlayRequested
        && !m_soundPreview.state().loading
        && m_soundPreview.state().error.empty()
        && m_soundPreview.state().hasAudio) {
      m_soundPlayRequested = false;
      if (m_soundPreview.state().duration > 6) {
        stopServerSound();
        m_serverSettings.error = "Sound previews must be no longer than 5.2 seconds.";
      } else
        m_soundPreview.pause(false);
      m_window.invalidate();
    }
    if (auto snapshot = m_themes.drain()) {
      m_themeSnapshot = std::move(*snapshot);
      m_window.invalidate();
    }
    // Advanced-control requests must be serviced before video metadata arrives,
    // including while paused or offscreen. Waiting for dimensions deadlocks load.
    if (m_videoContext != EGL_NO_CONTEXT) {
      eglMakeCurrent(m_window.graphics().display(), EGL_NO_SURFACE, EGL_NO_SURFACE, m_videoContext);
      m_video.update();
      m_window.context().makeCurrent(m_window.target());
    }
    if (m_video.drain())
      m_window.invalidate();
    if (m_videoPlayRequested
        && !m_video.state().loading
        && m_video.state().width > 0
        && m_video.state().error.empty()) {
      m_videoPlayRequested = false;
      m_video.pause(false);
      m_window.invalidate();
    }
    DeferredCall::drainWakeFd();
    for (auto& callback : DeferredCall::takePending())
      callback();
    TimerManager::instance().tick();
    drainWindowIcons();
    bool imagesChanged = false;
    if (!m_retiredImages.empty() && m_window.context().makeCurrent(m_window.target())) {
      for (auto& texture : m_retiredImages)
        m_window.context().textureManager().unload(texture);
      m_retiredImages.clear();
    }
    for (auto& asset : m_assets.drain()) {
      m_desktopNotifications.avatarLoaded(asset.path, asset.bytes);
      const auto pending = m_pendingImages.extract(asset.path);
      auto& cached = m_images[asset.path];
      cached.uses |= pending.empty() ? imageUseMask(ImageUse::Content) : pending.mapped();
      if (!asset.bytes.empty() && m_window.context().makeCurrent(m_window.target())) {
        auto animation = std::move(asset.animation);
        const bool mipmap = asset.path.starts_with("/media/v1/collectibles-shop/")
            && asset.path.ends_with("/static")
            && (!animation || animation->frames.size() == 1);
        const auto pixels = animation ? animation->pixels(0, m_animationPixels) : std::span<const std::uint8_t>{};
        auto& textures = m_window.context().textureManager();
        auto next = !pixels.empty() ? textures.loadFromRgba(pixels.data(), animation->width, animation->height, mipmap)
                                    : textures.loadFromEncodedBytes(asset.bytes.data(), asset.bytes.size(), mipmap);
        if (replaceCachedImageTexture(cached, textures, next)) {
          cached.animation = std::move(animation);
          cached.animationFrame = 0;
          cached.animationStart = {};
        }
      }
      cached.bytes = std::move(asset.bytes);
      cached.retry = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      if (asset.path == m_imageActionSource) {
        const auto action = std::exchange(m_pendingImageAction, Action::None);
        m_imageActionSource.clear();
        const bool current = std::exchange(m_imageActionViewer, {}) == m_viewImage && !m_viewImage.empty();
        if (current && !cached.bytes.empty())
          imageViewerAction(action);
        else if (current)
          m_imageStatus = "Could not load the original image. Try again.";
      }
      imagesChanged = true;
    }
    for (auto& [path, cached] : m_images)
      cached.reloadable = !api::mediaUrl(path).empty() || path.starts_with("builtin:");
    const auto usage = imageCacheUsage(m_images);
    if (usage.animationBytes > imageAnimationBudget
        || usage.encodedBytes > imageEncodedBudget
        || usage.textureBytes > imageTextureBudget
        || usage.entries > imageEntryBudget) {
      auto* textures =
          m_window.context().makeCurrent(m_window.target()) ? &m_window.context().textureManager() : nullptr;
      const auto& original = m_imageActionSource.empty() ? m_viewImage : m_imageActionSource;
      imagesChanged |= trimImageCache(m_images, textures, m_window.visible() ? m_animationDraw : 0, {}, original);
    }
    if (imagesChanged)
      m_window.invalidate();
    if (auto login = m_login.poll()) {
      if (!login->handoffToken.empty() && !m_connected && !m_demo) {
        m_connecting = true;
        m_restoringLogin = m_retrySavedLogin = false;
        m_token.clear();
        m_status = "Connecting to your account…";
        m_guildUpdates.clear();
        resetGuildBootstrap();
        resetReadAcknowledgements();
        m_session = m_client.connectHandoff(std::move(login->key), std::move(login->handoffToken));
        m_window.invalidate();
      } else if (!login->error.empty()) {
        m_status = std::move(login->error);
        m_window.invalidate();
      }
    }
    for (auto& event : m_client.drain()) {
      if (event.session != m_session || m_demo)
        continue;
      if (event.operation == api::Operation::TypingStart) {
        typingEvent(event);
        continue;
      }
      m_completionDirty = true;
      if (event.operation == api::Operation::GuildSettings && !event.unauthorized) {
        if (!event.live || !serverManagementResult(event))
          serverSettingsResult(event);
        continue;
      }

      if (!event.unauthorized
          && (event.operation == api::Operation::MessageCreate
              || event.operation == api::Operation::MessageUpdate
              || event.operation == api::Operation::MessageDelete))
        inboxEvent(event);
      filterDeletedChannels(event);
      if (!event.unauthorized && chatPreloadEvent(event)) {
        m_window.invalidate();
        continue;
      }
      if (!event.unauthorized && dmHomeEvent(event)) {
        m_window.invalidate();
        continue;
      }
      if (!event.unauthorized
          && event.operation == api::Operation::ChannelDetails
          && m_deletedChannels.contains(event.resource))
        continue;
      if (!event.unauthorized && event.operation == api::Operation::Channels && event.resource != m_guildId) {
        if (event.error.empty() && std::ranges::find(m_guilds, event.resource, &api::Guild::id) != m_guilds.end())
          m_guildChannels[event.resource] = std::move(event.channels);
        resumeServerInvite(event);
        continue;
      }
      if (!event.unauthorized && event.operation == api::Operation::Messages && event.resource != m_channelId)
        continue;
      if (event.unauthorized) {
        resetReadAcknowledgements();
        clearTyping();
        closeVideo();
        clearEmojiState();
        m_scrolling.stopAll();
        clearGifs();
        clearInteractions();
        m_userDirectory = {};
        m_roleDirectory = {};
        m_messageLog.reset();
        m_roleMotion.clear();
        clearAccountSettings();
        m_emojiMenu = false;
        closeServerSettings(true);
        m_settingsOpen = m_presenceMenu = m_savingProfile = m_loadingProfile = false;
        m_desiredPresence.clear();
        m_settingsFields.clear();
        m_settingsStatus.clear();
        closeImageViewer();
        m_client.disconnect();
        m_live = false;
        m_connected = false;
        m_connecting = false;
        m_guildChannels.clear();
        clearThreads();
        clearChatPreload();
        m_channelCacheOrder.clear();
        m_channelMessages.clear();
        m_channelHistoryState.clear();
        m_channelMembers.clear();
        m_lastChannels.clear();
        m_unreadDMs.clear();
        m_unreadChannels.clear();
        m_unreadGuilds.clear();
        m_localRead.clear();
        m_unreadTimeline.reset();
        m_history.reset();
        m_adoptedChatPreload.reset();
        m_timelineAnchor.reset();
        m_guilds.clear();
        m_guildUpdates.clear();
        resetGuildBootstrap();
        m_channels.clear();
        m_messages.clear();
        m_messageLayouts.clear();
        for (auto& [_, images] : m_attachments)
          for (auto& image : images)
            m_window.context().textureManager().unload(image.texture);
        m_attachments.clear();
        m_edit = {};
        m_selectedText.clear();
        m_selectedMessage.clear();
        m_drafts.clear();
        m_draftMentionNames.clear();
        m_fieldFocused = true;
      }
      if (event.operation == api::Operation::Connect) {
        m_connecting = m_restoringLogin = false;
        m_retrySavedLogin = event.restoredLogin && !event.noSavedLogin && !event.unauthorized && !event.error.empty();
        if (event.noSavedLogin)
          m_status.clear();
        if (event.error.empty() && !event.noSavedLogin) {
          m_status.clear();
          m_loginStorageStatus = event.restoredLogin ? "Sign-in restored from your desktop keyring."
                                                     : "Saving sign-in to your desktop keyring…";
          m_account = std::move(event.account);
          m_savingProfile = m_loadingProfile = false;
          m_desiredPresence.clear();
          m_preferences = m_sessionStore.preferences(m_account.id);
          releaseDisabledCosmetics();
          m_sessionStore.saveAppearance(m_preferences);
          m_frequentReactions = m_sessionStore.frequentReactions(m_account.id);
          m_recentEmojis = m_sessionStore.frequentEmojis(m_account.id);
          m_recentStickers = m_sessionStore.frequentStickers(m_account.id);
          clearTyping();
          const auto expanded = m_sessionStore.expandedFolders(m_account.id);
          m_openFolders = expanded.value_or(std::set<std::string>{});
          m_foldersRestored = expanded.has_value();
          m_presence = event.presence.value_or(api::Presence{});
          resetGuildBootstrap();
          m_readyBootstrapPending = event.live;
          m_guilds = std::move(event.guilds);
          m_guildLayout = std::move(event.layout);
          for (auto& guild : m_guilds)
            if (const auto at = m_guildUpdates.find(guild.id); at != m_guildUpdates.end())
              at->second.apply(guild);
          m_folders = api::arrangeGuilds(m_guilds, m_guildLayout);
          m_connected = true;
          if (!m_inboxBootstrap) {
            m_inboxBootstrap = true;
            const auto tab = m_inboxTab;
            m_inboxTab = 3;
            requestInbox();
            m_inboxTab = tab;
          }
          if (!m_live)
            m_gatewayStatus = "Connecting…";
          m_loadingPrivate = m_privateChannels.empty() && (m_readyBootstrapPending || m_client.privateChannels());
          const auto last = m_sessionStore.load(m_account.id);
          m_restoreChannel = last ? last->channel : "";
          if (last)
            m_window.setUiScale(last->scale);
          if (last && api::validSnowflake(last->channel))
            m_lastChannels[last->guild.empty() ? "@me" : last->guild] = last->channel;
          if (last && api::validSnowflake(last->guild)) {
            for (const auto& folder : m_folders)
              if (!m_foldersRestored
                  && !folder.id.empty()
                  && std::ranges::find(folder.guildIds, last->guild) != folder.guildIds.end())
                m_openFolders.insert(folder.id);
            selectGuild(last->guild);
            m_revealGuild = true;
            m_startupGuild = last->guild;
          } else
            showDirectMessages();
        }
      } else if (
          event.operation == api::Operation::GifSearch
          || event.operation == api::Operation::GifFavorites
          || event.operation == api::Operation::SaveGifFavorite
      ) {
        gifEvent(event);
      } else if (
          event.operation == api::Operation::GuildStickers
          || event.operation == api::Operation::StickerPacks
          || event.operation == api::Operation::StickerDetails
      ) {
        stickerEvent(event);
      } else if (
          event.operation == api::Operation::GuildEmojis
          || event.operation == api::Operation::EmojiPreferences
          || event.operation == api::Operation::SaveEmojiFavorite
      ) {
        emojiEvent(event);
      } else if (event.operation == api::Operation::Threads || event.operation == api::Operation::ThreadUpdate) {
        threadEvent(event);
        continue; // Scoped browsing errors belong to their forum/browser, never another channel.

      } else if (
          event.operation == api::Operation::InboxRead
          || event.operation == api::Operation::InboxWrite
          || event.operation == api::Operation::InboxUpdate
      ) {
        inboxEvent(event);
        continue;
      } else if (event.operation == api::Operation::ChannelDetails || event.operation == api::Operation::ChannelPage) {
        channelFeatureEvent(event);
        continue;
      } else if (event.operation == api::Operation::ComponentResult) {
        componentResult(event);
        continue;
      } else if (event.operation == api::Operation::Credentials) {
        m_loginStorageStatus = !event.error.empty()                        ? event.error
            : event.credentialOperation == auth::CredentialOperation::Save ? "Sign-in is saved in your desktop keyring."
                                                                           : "Saved sign-in has been removed.";
      } else if (event.operation == api::Operation::Interaction) {
        interactionResult(event);
      } else if (event.operation == api::Operation::Reaction && event.reaction) {
        const auto& change = *event.reaction;
        if (m_messageLog.known(change.channel, change.message))
          continue;
        if (auto preview = m_forumFirstMessages.find(change.channel);
            preview != m_forumFirstMessages.end() && preview->second.id == change.message)
          api::applyReaction(preview->second, change, m_account.id);
        if (change.channel == m_chatPreloadChannel) {
          if (m_chatPreloadReactions.size() < 512)
            m_chatPreloadReactions.push_back(change);
          else
            clearChatPreload();
        }
        if (m_adoptedChatPreload
            && change.channel == m_adoptedChatPreload->channel
            && std::ranges::none_of(m_messages, [&](const auto& message) { return message.id == change.message; })) {
          if (m_adoptedChatPreload->reactions.size() < 512)
            m_adoptedChatPreload->reactions.push_back(change);
          else {
            m_history.fail(m_adoptedChatPreload->visible, "Message history changed too much while loading. Try again.");
            m_loadingMessages = false;
            m_adoptedChatPreload.reset();
          }
        }
        invalidateReactors(change.channel, change.message);
        if (const auto cached = m_channelMessages.find(change.channel); cached != m_channelMessages.end())
          for (auto& message : cached->second)
            api::applyReaction(message, change, m_account.id);
        if (change.channel == m_channelId)
          for (auto& message : m_messages)
            if (message.id == change.message) {
              api::applyReaction(message, change, m_account.id);
              m_liveChanges[message.id] = message;
              m_history.changed(message.id, {.reactions = message.reactions});
            }
      } else if (event.operation == api::Operation::RoleStyles) {
        serverSettingsRoleUpdate(event.roleUpdates);
        m_roleDirectory.apply(event.roleUpdates);
        m_messageLayouts.clear();
        m_completionDirty = true;
        if (event.roleUpdates.reset)
          m_roleMotion.clear();
        refreshMemberSubscription();
      } else if (event.operation == api::Operation::UserState) {
        m_userDirectory.apply(event.users);
        if (!m_dmMode)
          if (const auto hidden = m_userDirectory.guildPreferences(m_guildId).hideMutedChannels)
            m_channelListPreferences.hideMuted = *hidden;
        if (m_dmMode)
          m_channels = dmConversations();
        scheduleMuteExpiry();
        if (event.users.reset || std::ranges::any_of(event.users.users, [](const auto& item) {
              return item.second.nickname.has_value();
            }))
          m_messageLayouts.clear();
      } else if (event.operation == api::Operation::Guilds) {
        guildListEvent(event);
      } else if (event.operation == api::Operation::ChannelSnapshot) {
        channelSnapshotEvent(event);
        resumeDiscordLink();
      } else if (event.operation == api::Operation::InvalidateChannels) {
        if (!event.removed.empty()) {
          removeGuildChannels(event);
          continue;
        }
        m_guildChannels.erase(event.resource);
        if (!m_dmMode && event.resource == m_guildId) {
          m_restoreChannel = m_channelId;
          m_loadingChannels = m_client.channels(m_guildId);
        }
      } else if (event.operation == api::Operation::ReadState) {
        m_unreadTimeline.snapshot(std::move(event.unreadCursors), event.resource);
        if (event.resource == "reset") {
          resetReadAcknowledgements();
          m_localRead.clear();
        } else if (!event.resource.empty()) {
          cancelAutomaticRead(event.resource);
          m_localRead.erase(event.resource);
        }
        m_unreadChannels.clear();
        m_unreadDMs.clear();
        m_unreadGuilds = std::move(event.unread);
        for (const auto& [id, guild] : m_unreadGuilds) {
          if (!m_unreadTimeline.cursor(id))
            continue;
          m_unreadChannels.insert(id);
          if (guild.empty() && std::ranges::find(m_privateChannels, id, &api::Channel::id) != m_privateChannels.end())
            m_unreadDMs.insert(id);
        }
      } else if (
          event.operation == api::Operation::AccountSettings || event.operation == api::Operation::SaveAccountSettings
      ) {
        if (event.resource == "changed") {
          if (m_settingsOpen && !m_loadingSettings && !m_savingSettings && m_settingsDraft == m_accountSettings)
            m_loadingSettings = m_client.accountSettings();
        } else {
          m_loadingSettings = false;
          const bool saved = event.operation == api::Operation::SaveAccountSettings;
          if (saved)
            m_savingSettings = false;
          if (event.error.empty() && event.settings) {
            const bool dirty = m_settingsDraft != m_accountSettings;
            m_accountSettings = *event.settings;
            if (!dirty || saved || !m_settingsLoaded)
              m_settingsDraft = m_accountSettings;
            m_settingsLoaded = true;
            if (saved)
              m_settingsStatus = "Account settings saved.";
          } else if (!event.error.empty())
            m_settingsStatus = event.error;
        }
      } else if (event.operation == api::Operation::UserSettings) {
        if (event.userSettingsSessionEnded && event.error.empty()) {
          activate({0, 0, 0, 0, Action::SignOut, {}});
        } else if (!userSettingsListsResult(event))
          userAccountResult(event);
      } else if (event.operation == api::Operation::SettingsList) {
        m_loadingSettingsLists.erase(event.resource);
        if (event.error.empty()) {
          m_settingsLists[event.resource] = std::move(event.settingsEntries);
          m_settingsListErrors.erase(event.resource);
        } else
          m_settingsListErrors[event.resource] = event.error;
      } else if (
          event.operation == api::Operation::Profile
          || event.operation == api::Operation::SaveProfile
          || event.operation == api::Operation::AccountUpdate
      ) {
        if (event.operation == api::Operation::Profile)
          m_loadingProfile = false;
        if (event.operation == api::Operation::SaveProfile)
          m_savingProfile = false;
        if (event.error.empty() && event.account.id == m_account.id) {
          const bool editing = m_settingsOpen && m_profileDirty;
          if (!event.account.premiumType)
            event.account.premiumType = m_account.premiumType;
          m_account = std::move(event.account);
          if (event.operation == api::Operation::SaveProfile || !editing)
            resetProfile();
          if (event.operation == api::Operation::SaveProfile)
            m_settingsStatus = "Profile saved.";
        } else if (!event.error.empty())
          m_settingsStatus = event.error;
      } else if (
          event.operation == api::Operation::PresenceChanged || event.operation == api::Operation::SavePresence
      ) {
        if (event.presence && (m_desiredPresence.empty() || event.presence->status == m_desiredPresence))
          m_presence = *event.presence;
        if (event.operation == api::Operation::SavePresence && event.resource == m_desiredPresence) {
          m_desiredPresence.clear();
          if (event.error.empty())
            m_settingsStatus = "Status saved.";
          else
            m_settingsStatus = "Status changed for this session; could not save it to your account. " + event.error;
        }
      } else if (event.operation == api::Operation::Channels && event.resource == m_guildId) {
        m_loadingChannels = false;
        channelSnapshotEvent(event);
        if (event.error.empty())
          resumeDiscordLink();
        else if (m_pendingDiscordLink) {
          m_pendingDiscordLink.reset();
          m_status = "Could not open this channel. " + event.error;
        }
        resumeServerInvite(event);
      } else if (event.operation == api::Operation::Messages && event.resource == m_channelId) {
        applyHistory(event);
      } else if (
          event.operation == api::Operation::PrivateChannels || event.operation == api::Operation::PrivateChannelUpsert
      ) {
        m_loadingPrivate = false;
        if (event.error.empty()) {
          if (event.operation == api::Operation::PrivateChannels && !event.live && m_privateReady)
            continue;
          if (event.operation == api::Operation::PrivateChannels) {
            if (event.live)
              m_privateReady = true;
            m_privateChannels = std::move(event.channels);
          } else
            for (auto& channel : event.channels) {
              auto existing = std::ranges::find(m_privateChannels, channel.id, &api::Channel::id);
              if (existing == m_privateChannels.end())
                m_privateChannels.insert(m_privateChannels.begin(), std::move(channel));
              else
                api::mergePrivateChannel(*existing, std::move(channel));
            }
          if (!m_dmPreviewId.empty()) {
            const auto preview = std::ranges::find(m_privateChannels, m_dmPreviewId, &api::Channel::id);
            if (preview == m_privateChannels.end() || !preview->isMessageRequest()) {
              m_dmPreviewId.clear();
              m_dmPreview.clear();
              m_dmPreviewLoading = false;
            }
          }
          for (const auto& channel : m_privateChannels)
            if (m_unreadGuilds.contains(channel.id) && !readingChannel(channel.id))
              m_unreadDMs.insert(channel.id);
          if (m_dmMode) {
            m_channels = dmConversations();
            if (m_dmPage.empty() && m_channelId.empty() && !m_channels.empty()) {
              auto selected = std::ranges::find(m_channels, m_restoreChannel, &api::Channel::id);
              selectChannel(selected == m_channels.end() ? m_channels.front().id : selected->id);
              m_restoreChannel.clear();
            } else if (m_dmPage.empty() && m_channelId.empty() && m_channels.empty()) {
              m_dmPage = "friends";
              m_dmFilter = "online";
              m_restoreChannel.clear();
              m_loadingMessages = false;
              if (!m_demo)
                m_client.subscribe({}, {});
            }
            updateDmMembers();
          }
        }
      } else if (event.operation == api::Operation::PrivateChannelDelete) {
        if (event.resource == m_chatPreloadChannel || event.resource == m_chatPreloadHover)
          clearChatPreload();
        cancelAutomaticRead(event.resource);
        m_unreadDMs.erase(event.resource);
        m_unreadChannels.erase(event.resource);
        if (m_dmPreviewId == event.resource) {
          m_dmPreviewId.clear();
          m_dmPreview.clear();
          m_dmPreviewLoading = false;
        }
        m_messageLog.clearChannel(event.resource);
        m_channelMessages.erase(event.resource);
        m_channelHistoryState.erase(event.resource);
        std::erase_if(m_privateChannels, [&](const auto& channel) { return channel.id == event.resource; });
        if (m_dmMode) {
          m_channels = dmConversations();
          if (m_channelId == event.resource) {
            clearTyping();
            m_channelId.clear();
            m_messages.clear();
            m_messageLayouts.clear();
          }
        }
      } else if (event.operation == api::Operation::GuildMetadata) {
        m_messageLayouts.clear();
        m_completionDirty = true;
        for (const auto& update : event.guildUpdates) {
          auto at = m_guildUpdates.find(update.id);
          if (at == m_guildUpdates.end() && m_guildUpdates.size() < 10000)
            at = m_guildUpdates.emplace(update.id, api::GuildUpdate{.id = update.id}).first;
          if (at != m_guildUpdates.end())
            at->second.merge(update);
          if (const auto guild = std::ranges::find(m_guilds, update.id, &api::Guild::id); guild != m_guilds.end())
            update.apply(*guild);
        }
      } else if (event.operation == api::Operation::Layout) {
        guildLayoutEvent(event);
      } else if (event.operation == api::Operation::Members && event.resource == m_channelId) {
        const auto identity = channelMemberIdentity(m_channelId);
        if (event.memberIdentity && event.memberIdentity != identity)
          continue;
        m_members = std::move(event.members);
        m_memberCount = event.memberCount;
        m_memberEpoch = m_chatCacheEpoch;
        m_activeMemberIdentity.clear();
        if (identity && event.memberIdentity) {
          m_activeMemberIdentity = identity->signature;
          const MemberSnapshot snapshot{m_members, m_memberCount, identity->signature, m_chatCacheEpoch};
          for (auto& [channel, cached] : m_channelMembers)
            if (cached.epoch == m_chatCacheEpoch && cached.identity == identity->signature)
              cached = snapshot;
        }
        m_messageLayouts.clear();
        m_scrolling.limit(
            m_memberScroll,
            layout::MemberListLayout(m_members, m_memberCount)
                .maximumScroll(
                    layout::Members::viewportHeight(
                        m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight()
                    )
                )
        );
      } else if (event.operation == api::Operation::Gateway) {
        if (!event.live) {
          clearTyping();
          ++m_chatCacheEpoch;
          clearChatPreload();
        }
        m_live = event.live;
        m_gatewayStatus = m_live ? "Connected" : "Reconnecting…";
        if (m_live) {
          if (std::exchange(m_readyBootstrapPending, false)) {
            if (!m_privateReady && m_privateChannels.empty())
              m_loadingPrivate = m_client.privateChannels();
            if (!m_dmMode && !m_guildId.empty() && !m_guildChannels.contains(m_guildId))
              m_loadingChannels = m_client.channels(m_guildId);
          }
          m_status.clear();
          if (!m_history.initialized() || m_historyEpoch != m_chatCacheEpoch)
            refresh();
        }
      } else if (event.operation == api::Operation::MessageCreate) {
        typingEvent(event);
        for (auto& message : event.messages)
          confirmOutgoing(message);
        std::erase_if(event.messages, [&](const auto& message) {
          return m_dismissedEphemeral.contains(message.id) || m_messageLog.known(event.resource, message.id);
        });
        if (event.messages.empty())
          continue;
        if (m_adoptedChatPreload && event.resource == m_adoptedChatPreload->channel)
          std::erase_if(m_adoptedChatPreload->reactions, [&](const auto& change) {
            return std::ranges::any_of(event.messages, [&](const auto& message) {
              return message.id == change.message;
            });
          });
        if (event.resource == m_chatPreloadChannel) {
          for (const auto& message : event.messages) {
            const auto known = m_chatPreloadLive.find(message.id);
            if ((message.flags & 64) || (known != m_chatPreloadLive.end() && !known->second))
              continue;
            m_chatPreloadLive[message.id] = message;
            std::erase_if(m_chatPreloadReactions, [&](const auto& change) { return change.message == message.id; });
          }
          if (m_chatPreloadLive.size() > 4096)
            clearChatPreload();
        }
        for (const auto& message : event.messages)
          notifyMessage(message, event.resource);
        const bool ephemeral =
            std::ranges::all_of(event.messages, [](const auto& message) { return (message.flags & 64) != 0; });
        auto dm = std::ranges::find(m_privateChannels, event.resource, &api::Channel::id);
        if (dm != m_privateChannels.end() && !ephemeral) {
          auto channel = std::move(*dm);
          m_privateChannels.erase(dm);
          if (!event.messages.empty())
            channel.lastMessageId = event.messages.back().id;
          m_privateChannels.insert(m_privateChannels.begin(), std::move(channel));
          if (m_dmMode)
            m_channels = dmConversations();
          if (!readingChannel(event.resource)
              && !event.messages.empty()
              && event.messages.back().authorId != m_account.id)
            m_unreadDMs.insert(event.resource);
        }
        const bool fromOther = !ephemeral && !event.messages.empty() && event.messages.back().authorId != m_account.id;
        if (!readingChannel(event.resource) && fromOther)
          m_unreadChannels.insert(event.resource);
        if (event.resource != m_channelId) {
          if (auto cached = m_channelMessages.find(event.resource); cached != m_channelMessages.end()) {
            for (const auto& message : event.messages) {
              std::erase_if(cached->second, [&](const auto& old) { return old.id == message.id; });
              cached->second.push_back(message);
            }
            if (cached->second.size() > 200) {
              cached->second.erase(cached->second.begin(), cached->second.end() - 200);
              m_channelHistoryState[event.resource].olderEnd = false;
            }
          }
        }
        if (event.resource == m_channelId)
          for (auto& message : event.messages) {
            m_liveChanges[message.id] = message;
            if (m_history.atLatest())
              upsertMessage(std::move(message));
          }
      } else if (event.operation == api::Operation::MemberSearch) {
        completionEvent(event);
      } else if (event.operation == api::Operation::MessageUpdate) {
        updateMessage(event.resource, event.messageId, event.patch);
      } else if (event.operation == api::Operation::MessageDelete) {
        deleteMessages(event.resource, event.removed);
      } else if (event.operation == api::Operation::Send) {
        if (event.unauthorized)
          m_status = event.error;
        outgoingResult(event);
        m_window.invalidate();
        continue;
      }
      if (event.operation == api::Operation::Acknowledge && !event.error.empty())
        failedReadAcknowledgement(event.resource, event.messageId);
      if (!event.error.empty())
        m_status = event.error;
      while (m_liveChanges.size() > (m_history.busy() ? 4096U : 256U))
        m_liveChanges.erase(m_liveChanges.begin());
      m_window.invalidate();
    }
  }

  void ClientApplication::upsertMessage(api::Message message) {
    if (m_messageLog.known(message.channelId, message.id))
      return;
    m_messageLayouts.erase(message.id);
    auto existing = std::ranges::find(m_messages, message.id, &api::Message::id);
    if (existing == m_messages.end())
      m_messages.push_back(std::move(message));
    else
      *existing = std::move(message);
    std::ranges::sort(m_messages, [](const auto& a, const auto& b) {
      return a.id.size() != b.id.size() ? a.id.size() < b.id.size() : a.id < b.id;
    });
    m_history.trim(m_messages);
    std::set<std::string> retained;
    for (const auto& item : m_messages)
      retained.insert(item.id);
    std::erase_if(m_messageLayouts, [&](const auto& item) { return !retained.contains(item.first); });
  }

  void ClientApplication::saveSelection() {
    if (!m_demo && m_connected && !m_channelId.empty())
      m_sessionStore.save(m_account.id, {m_guildId, m_channelId, m_window.uiScale()});
  }

  void ClientApplication::positionCaret(float x, float y, bool extend) {
    if (m_draftStops.empty())
      return;
    float nearest = std::numeric_limits<float>::max();
    std::size_t position = 0;
    for (std::size_t i = 0; i < m_draftStops.size(); ++i) {
      const auto& stop = m_draftStops[i];
      const auto dx = x - m_draftX - stop.x;
      const auto dy = std::abs(y - m_draftY - (stop.y + stop.height / 2));
      const auto distance = dx * dx + std::pow(std::max(0.0F, dy - stop.height / 2), 2) * 10000;
      if (distance < nearest) {
        nearest = distance;
        position = m_draftOffsets[i];
      }
    }
    m_edit.caret = position;
    if (!extend)
      m_edit.anchor = position;
    m_window.invalidate();
  }

  void ClientApplication::positionChatSelection(float x, float y) {
    float nearest = std::numeric_limits<float>::max();
    const TextArea* area = nullptr;
    for (const auto& item : m_textAreas)
      if (item.message == m_selectedMessage) {
        const auto dy = std::max(0.0F, std::abs(y - item.y - item.run.height / 2) - item.run.height / 2);
        const auto dx = x < item.x ? item.x - x : std::max(0.0F, x - item.x - item.run.width);
        const auto distance = dx * dx + dy * dy * 10000;
        if (distance < nearest) {
          nearest = distance;
          area = &item;
        }
      }
    if (!area)
      return;
    const auto& run = area->run;
    auto positions = TextEdit::stops(run.text);
    float distance = std::numeric_limits<float>::max();
    for (auto offset : positions) {
      const float width = run.image.empty()
          ? m_window.target()
                .renderer()
                .measureText(
                    messageMarkup(std::string_view(run.text).substr(0, offset), run.style), run.style.size,
                    FontWeight::Normal, 0, 1, TextAlign::Start, {}, TextEllipsize::End, true
                )
                .width
          : (offset ? run.width : 0);
      const float delta = std::abs(x - area->x - width);
      if (delta < distance) {
        distance = delta;
        m_chatEdit.caret = run.offset + offset;
      }
    }
    m_window.invalidate();
  }

  void ClientApplication::insert(std::string text) {
    if (!m_connected && !m_showTokenEntry)
      return;
    if (!m_fieldFocused || m_connecting || (m_connected && (m_demo || m_channelId.empty())))
      return;
    if (!g_utf8_validate(text.data(), text.size(), nullptr))
      return;
    // Preserve pasted line breaks and indentation in messages; login stays single-line.
    std::string clean;
    for (std::size_t i = 0; i < text.size(); ++i) {
      const auto ch = static_cast<unsigned char>(text[i]);
      if (ch == '\r' && m_connected) {
        clean += '\n';
        if (i + 1 < text.size() && text[i + 1] == '\n')
          ++i;
      } else if (ch >= 32 && ch != 127)
        clean += text[i];
      else if (m_connected && (ch == '\n' || ch == '\t'))
        clean += text[i];
    }
    auto& field = m_connected ? m_drafts[m_channelId] : m_token;
    m_edit.clamp(field);
    if (field.size() - (std::max(m_edit.caret, m_edit.anchor) - std::min(m_edit.caret, m_edit.anchor)) + clean.size()
        > (m_connected ? 8000U : 4096U))
      return;
    m_edit.replace(field, clean);
    m_selectAll = false;
    m_window.invalidate();
  }

  void ClientApplication::onKey(const Key& key) {
    if (m_railDrag && key.symbol == XKB_KEY_Escape) {
      finishRailDrag(true);
      return;
    }
    finishSidebarResize();
    if (!m_shortcutCapture.empty() && shortcutKey(key))
      return;
    if (selectDropdownKey(key))
      return;
    if (userAccountDialogKey(key))
      return;
    if (inboxKey(key))
      return;
    if (searchMenuKey(key))
      return;
    if (shortcutKey(key))
      return;
    if (m_serverSettings.open && key.symbol == XKB_KEY_Escape) {
      closeServerSettings();
      return;
    }
    if (key.symbol == XKB_KEY_Escape && completionKey(key))
      return;
    if (key.symbol == XKB_KEY_Escape
        && m_dmMode
        && m_dmPage == "requests"
        && !m_dmPreviewId.empty()
        && !settingsVisible()
        && m_dialogKind.empty()
        && !m_contextOpen
        && m_viewImage.empty()) {
      dmAction("close-request");
      return;
    }
    if (key.symbol == XKB_KEY_Escape
        && m_dialogKind.empty()
        && !m_channelPanel.empty()
        && m_focusedInput != "message-edit") {
      closeChannelPanel();
      return;
    }
    if (videoKey(key))
      return;
    if (key.symbol == XKB_KEY_Escape && m_gifMenu) {
      closeGifPicker();
      return;
    }
    if (overlayKey(key))
      return;
    if (!m_viewImage.empty()) {
      if (key.control && (key.symbol == XKB_KEY_q || key.symbol == XKB_KEY_Q)) {
        m_window.close();
        return;
      }
      if (key.control && (key.symbol == XKB_KEY_c || key.symbol == XKB_KEY_C)) {
        imageViewerAction(Action::CopyImage);
      } else if (key.symbol == XKB_KEY_plus || key.symbol == XKB_KEY_equal || key.symbol == XKB_KEY_KP_Add)
        m_viewZoom = std::min(4.0F, m_viewZoom * 1.2F);
      else if (key.symbol == XKB_KEY_minus || key.symbol == XKB_KEY_KP_Subtract)
        m_viewZoom = std::max(0.25F, m_viewZoom / 1.2F);
      else if (key.symbol == XKB_KEY_Escape)
        closeImageViewer();
      else if (key.symbol == XKB_KEY_Left)
        changeImage(-1);
      else if (key.symbol == XKB_KEY_Right)
        changeImage(1);
      else if (key.symbol == XKB_KEY_0)
        m_viewZoom = 1;
      m_window.invalidate();
      return;
    }
    if (key.symbol != XKB_KEY_Escape && completionKey(key))
      return;
    if (KeySymbol::isTab(key.symbol) && m_viewImage.empty()) {
      (void)m_controlInput.cycleTabFocus(key.shift);
      m_window.invalidate();
      return;
    }
    if (messageEditKey(key))
      return;
    if (!m_controlInput.focusedArea()
        && !key.control
        && !key.alt
        && !key.super
        && !KeySymbol::isModifier(key.symbol)
        && key.symbol != XKB_KEY_Escape)
      focusChatComposer();
    if (key.symbol != XKB_KEY_Escape && m_controlInput.focusedArea()) {
      const auto unicode = key.text.empty() ? 0U : static_cast<std::uint32_t>(g_utf8_get_char(key.text.c_str()));
      m_controlInput.keyEvent(
          key.symbol, unicode, (key.control ? KeyMod::Ctrl : 0) | (key.shift ? KeyMod::Shift : 0), true
      );
      m_window.invalidate();
      return;
    }
    if (m_serverSettings.open)
      return;
    if (m_settingsOpen) {
      settingsKey(key);
      return;
    }
    if (key.symbol == XKB_KEY_Escape && m_emojiMenu) {
      closeEmojiPicker();
      m_reactionMessage.clear();
      m_reactionChannel.clear();
      m_window.invalidate();
      return;
    }
    if (key.symbol == XKB_KEY_Escape && m_presenceMenu) {
      m_presenceMenu = false;
      m_window.invalidate();
      return;
    }
    if (key.symbol == XKB_KEY_Escape && m_replies.contains(m_channelId)) {
      activate({.action = Action::CancelReply});
      return;
    }
    if (key.control && (key.symbol == XKB_KEY_r || key.symbol == XKB_KEY_R)) {
      refresh();
      return;
    }
    if (key.control
        && (key.symbol == XKB_KEY_c || key.symbol == XKB_KEY_C || key.symbol == XKB_KEY_x || key.symbol == XKB_KEY_X)) {
      if (m_connected && m_fieldFocused) {
        auto& field = m_drafts[m_channelId];
        m_edit.clamp(field);
        if (m_edit.selected()) {
          m_window.copy(m_edit.selection(field));
          if ((key.symbol == XKB_KEY_x || key.symbol == XKB_KEY_X))
            m_edit.replace(field, {});
        }
      } else if (!m_fieldFocused && m_chatEdit.selected() && !m_selectedText.empty())
        m_window.copy(m_chatEdit.selection(m_selectedText));
      m_window.invalidate();
      return;
    }
    if (key.control && (key.symbol == XKB_KEY_v || key.symbol == XKB_KEY_V)) {
      m_pasteConnected = m_connected;
      m_pasteSession = m_session;
      m_pasteChannel = m_channelId;
      if (m_fieldFocused)
        m_window.paste(m_connected && !key.shift);
      return;
    }
    if (key.symbol == XKB_KEY_Tab) {
      if (m_connected && m_fieldFocused)
        insert("    ");
      else
        m_fieldFocused = true;
      m_window.invalidate();
      return;
    }
    if (key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter) {
      if (m_connected) {
        if (key.shift)
          insert("\n");
        else
          send();
      } else if (m_showTokenEntry)
        connect();
      else
        beginLogin();
      return;
    }
    if (!m_fieldFocused || m_connecting || (m_connected && (m_demo)))
      return;
    auto& field = m_connected ? m_drafts[m_channelId] : m_token;
    m_edit.clamp(field);
    if (key.control && (key.symbol == XKB_KEY_a || key.symbol == XKB_KEY_A)) {
      m_edit.anchor = 0;
      m_edit.caret = field.size();
      m_selectAll = true;
    } else if (key.symbol == XKB_KEY_Escape) {
      m_edit.anchor = m_edit.caret;
      m_selectAll = false;
    } else if (key.symbol == XKB_KEY_BackSpace || key.symbol == XKB_KEY_Delete)
      m_edit.erase(field, key.symbol == XKB_KEY_Delete);
    else if (key.symbol == XKB_KEY_Left || key.symbol == XKB_KEY_Right)
      m_edit.move(field, key.symbol == XKB_KEY_Right, key.shift);
    else if (key.symbol == XKB_KEY_Home || key.symbol == XKB_KEY_End) {
      if (key.symbol == XKB_KEY_Home) {
        const auto at = field.rfind('\n', m_edit.caret ? m_edit.caret - 1 : 0);
        m_edit.caret = key.control || at == std::string::npos ? 0 : at + 1;
      } else {
        const auto at = field.find('\n', m_edit.caret);
        m_edit.caret = key.control || at == std::string::npos ? field.size() : at;
      }
      if (!key.shift)
        m_edit.anchor = m_edit.caret;
    } else if ((key.symbol == XKB_KEY_Up || key.symbol == XKB_KEY_Down) && m_connected && !m_draftStops.empty()) {
      const auto at = std::ranges::find(m_draftOffsets, m_edit.caret);
      if (at != m_draftOffsets.end()) {
        const auto& stop = m_draftStops[at - m_draftOffsets.begin()];
        positionCaret(
            m_draftX + stop.x, m_draftY + stop.y + stop.height * (key.symbol == XKB_KEY_Up ? -0.5F : 1.5F), key.shift
        );
      }
    } else if (!key.control)
      insert(key.text);
    m_window.invalidate();
  }

  void ClientApplication::activate(const Hit& hit) {
    if (serverSettingsAction(hit))
      return;
    if (channelMenuAction(hit))
      return;
    if (videoAction(hit))
      return;
    if (hit.action == Action::PickEmoji) {
      pickEmoji(hit.id);
      m_window.invalidate();
      return;
    }
    if (interactionAction(hit))
      return;
    switch (hit.action) {
    case Action::ServerMenuDialogAction:
      serverMenuDialogAction(hit.id);
      break;
    case Action::GuildMenu:
      openServerContext(hit, m_pointerX, m_pointerY);
      break;
    case Action::Settings:
      openSettings();
      break;
    case Action::CloseSettings:
      if (m_userAccount.busy || m_userSettingsLists.busy)
        break;
      if (!m_userAccount.draft.empty() || m_userAccount.crop) {
        m_userAccount.fields["next_page"] = "close";
        m_userAccount.dialog = "discard-profile";
        ++m_dialogGeneration;
        m_window.invalidate();
        break;
      }
      resetUserAccountSettings();
      resetUserSettingsLists();
      m_shortcutCapture.clear();
      m_settingsOpen = false;
      m_settingsField.clear();
      break;
    case Action::SettingsPage:
      if (!settingsVisible())
        openSettings();
      selectSettingsPage(hit.id);
      break;
    case Action::SaveProfile: {
      if (m_demo) {
        m_settingsStatus = "Demo mode is read-only.";
        break;
      }
      if (m_savingProfile || m_loadingProfile)
        break;
      auto edited = m_account;
      edited.displayName = m_settingsFields["display_name"];
      edited.pronouns = m_settingsFields["pronouns"];
      edited.bio = m_settingsFields["bio"];
      m_savingProfile = m_client.saveProfile(edited);
      m_settingsStatus = m_savingProfile ? "Saving profile…" : "Check the field lengths and your connection.";
      break;
    }
    case Action::ResetProfile:
      if (!m_savingProfile)
        resetProfile();
      break;
    case Action::SetPresence:
      setPresence(hit.id);
      break;
    case Action::SaveCustomStatus:
      setPresence(api::validStatus(m_presence.status) ? m_presence.status : "online", true);
      break;
    case Action::TogglePresence:
      m_presenceMenu = !m_presenceMenu;
      m_fieldFocused = false;
      break;
    case Action::AccountPreference: {
      if (!m_settingsLoaded || m_loadingSettings || m_savingSettings)
        break;
      auto key = hit.id;
      const auto colon = key.find(':');
      const auto field = key.substr(0, colon);
      if (!m_settingsDraft.contains(field))
        break;
      auto value = m_settingsDraft[field];
      if (colon == std::string::npos)
        value = !value;
      else {
        const auto number = std::stoi(key.substr(colon + 1));
        if (field == "friend_sources") {
          if (number == 8)
            value = value & 8 ? value & ~8 : 14;
          else {
            value ^= number;
            if ((value & 6) != 6)
              value &= ~8;
          }
        } else
          value = number;
      }
      if (api::validSettingChanges({{field, value}}))
        m_settingsDraft[field] = value;
      break;
    }
    case Action::SaveAccountSettings: {
      if (m_savingSettings || m_loadingSettings || !m_settingsLoaded)
        break;
      if (m_demo) {
        m_settingsStatus = "Demo mode is read-only.";
        break;
      }
      api::SettingValues changes;
      for (const auto& [key, value] : m_settingsDraft)
        if (!m_accountSettings.contains(key) || m_accountSettings.at(key) != value)
          changes[key] = value;
      m_savingSettings = m_client.saveAccountSettings(std::move(changes));
      m_settingsStatus =
          m_savingSettings ? "Saving account settings…" : "No changes to save, or the connection is unavailable.";
      break;
    }
    case Action::ResetAccountSettings:
      if (!m_savingSettings) {
        m_settingsDraft = m_accountSettings;
        m_settingsStatus.clear();
      }
      break;
    case Action::JumpUnread:
      if (const auto first = m_unreadTimeline.first(m_messages)) {
        const auto at = m_messageTops.find(m_messages[*first].id);
        if (at != m_messageTops.end()) {
          m_scrolling.stop(m_messageScroll);
          m_messageScroll = std::clamp(
              m_messageScroll + m_messageViewportTop + (Style::space2Xl + Style::spaceXs) - at->second, 0.0F,
              m_messageMaxScroll
          );
        }
      }
      break;
    case Action::RetryHistory:
      loadHistory(m_history.failedDirection(), true);
      break;
    case Action::JumpLatest:
      m_scrolling.stop(m_messageScroll);
      m_messageScroll = 0;
      m_timelineAnchor.reset();
      m_keepHistoryAnchor = false;
      m_seekMessage.clear();
      m_jumpHighlightTimer.stop();
      m_jumpHighlightMessage.clear();
      m_unreadTimeline.positioned();
      if (m_history.atLatest()) {
        if (const auto& pending = m_history.pending(); pending && pending->direction != api::HistoryDirection::Latest) {
          m_history.cancelPending();
          m_loadingMessages = false;
        }
      } else {
        m_history.reset();
        m_adoptedChatPreload.reset();
        m_messages.clear();
        m_messageLayouts.clear();
        loadHistory(api::HistoryDirection::Latest);
      }
      break;
    case Action::MarkRead:
      markChannelRead(true);
      break;
    case Action::UserAccountAction:
      if (!userSettingsListsAction(hit.id))
        userAccountAction(hit.id);
      break;
    case Action::Preference:
      if (extendedPreference(hit.id))
        break;
      if (fontPreference(hit.id))
        break;
      if (hit.id == "theme-template") {
        m_window.copy(themeTemplate());
        break;
      }
      if (hit.id.starts_with("palette:")) {
        const auto id = hit.id.substr(8);
        if (std::ranges::find(m_themeSnapshot.themes, id, &ColorTheme::id) == m_themeSnapshot.themes.end())
          break;
        m_preferences.colorTheme = id;
      } else if (hit.id.starts_with("theme:")) {
        const auto mode = hit.id.substr(6);
        if (!validColorMode(mode))
          break;
        m_preferences.theme = mode;
      } else if (hit.id == "font+")
        m_preferences.messageSize = std::min(20.0F, m_preferences.messageSize + Style::borderWidth);
      else if (hit.id == "font-")
        m_preferences.messageSize = std::max(12.0F, m_preferences.messageSize - Style::borderWidth);
      else if (hit.id == "compact")
        m_preferences.compact = !m_preferences.compact;
      else if (hit.id == "embeds")
        m_preferences.showEmbeds = !m_preferences.showEmbeds;
      else if (hit.id == "images")
        m_preferences.showImages = !m_preferences.showImages;
      else if (hit.id == "autoplay_gifs")
        m_preferences.autoplayGifs = !m_preferences.autoplayGifs;
      else if (hit.id == "log_deleted_messages") {
        m_preferences.logDeletedMessages = !m_preferences.logDeletedMessages;
        if (!m_preferences.logDeletedMessages)
          clearMessageLog(true, false);
      } else if (hit.id == "log_edit_history") {
        m_preferences.logEditHistory = !m_preferences.logEditHistory;
        if (!m_preferences.logEditHistory)
          clearMessageLog(false, true);
      } else if (hit.id == "emoji_image_links")
        m_preferences.emojiImageLinks = !m_preferences.emojiImageLinks;
      else if (hit.id == "role_colors") {
        m_preferences.roleColors = !m_preferences.roleColors;
        m_preferences.roleColorMode = m_preferences.roleColors ? 0 : 2;
      } else if (hit.id == "animate_role_colors")
        m_preferences.animateRoleColors = !m_preferences.animateRoleColors;
      else if (hit.id == "guild_tags")
        m_preferences.showGuildTags = !m_preferences.showGuildTags;
      else if (hit.id.starts_with("zoom")) {
        m_window.setUiScale(hit.id == "zoom0" ? 1 : m_window.uiScale() * (hit.id == "zoom+" ? 1.1F : 1 / 1.1F));
        saveSelection();
      }
      savePreferences();
      break;
    case Action::None:
      break;
    case Action::ImagePan:
      m_dragImage = true;
      m_imageDragX = m_pointerX;
      m_imageDragY = m_pointerY;
      break;
    case Action::ImageZoom:
      m_viewZoom = hit.id == "fit" ? 1 : std::clamp(m_viewZoom * (hit.id == "in" ? 1.2F : 1 / 1.2F), 0.25F, 4.0F);
      break;
    case Action::Minimize:
      m_window.minimize();
      break;
    case Action::Maximize:
      m_window.toggleMaximized();
      break;
    case Action::ViewImage:
      m_scrolling.stopAll();
      closeImageViewer();
      m_viewImage = hit.id;
      enterMediaViewer();
      m_viewUrl.clear();
      m_viewZoom = 1;
      m_viewPanX = m_viewPanY = 0;
      for (const auto& image : m_viewerImages)
        if (image.path == hit.id)
          m_viewUrl = image.url;
      m_fieldFocused = false;
      m_dragDraft = m_dragMessage = false;
      break;
    case Action::CloseImage:
      closeImageViewer();
      break;
    case Action::ImageNext:
      changeImage(1);
      break;
    case Action::ImagePrevious:
      changeImage(-1);
      break;
    case Action::Close:
      m_window.requestClose();
      break;
    case Action::Connect:
      connect();
      break;
    case Action::DiscordLogin:
      beginLogin();
      break;
    case Action::ToggleToken:
      m_login.cancel();
      m_showTokenEntry = !m_showTokenEntry;
      m_fieldFocused = m_showTokenEntry;
      m_status.clear();
      break;
    case Action::CancelLogin:
      m_login.cancel();
      if (m_connecting)
        m_session = m_client.disconnect();
      m_connecting = m_restoringLogin = m_retrySavedLogin = false;
      m_status.clear();
      break;
    case Action::RetryLogin:
      restoreLogin();
      break;
    case Action::Demo:
      demo();
      break;
    case Action::Guild:
      m_restoreChannel.clear();
      selectGuild(hit.id);
      break;
    case Action::DirectMessage: {
      m_restoreChannel = hit.id;
      showDirectMessages();
      if (m_channelId != hit.id && std::ranges::find(m_channels, hit.id, &api::Channel::id) != m_channels.end())
        selectChannel(hit.id);
      break;
    }
    case Action::Home:
      m_dmSearch.clear();
      if (m_dmMode && m_dmPage.empty() && !m_channelId.empty())
        break;
      m_restoreChannel.clear();
      showDirectMessages();
      break;
    case Action::DmPage:
      openDmPage(hit.id);
      break;
    case Action::DmAction:
      dmAction(hit.id);
      break;
    case Action::Category:
      if (!m_collapsedCategories.erase(hit.id))
        m_collapsedCategories.insert(hit.id);
      break;
    case Action::Channel:
      m_pendingDiscordLink.reset();
      selectChannel(hit.id);
      break;
    case Action::JumpChannelMention: {
      for (const auto& row : channelListRows(
               m_channels, m_collapsedCategories, m_channelId, m_channelListPreferences,
               [&](const auto& id) { return channelMuted(id); },
               [&](const auto& channel) { return m_unreadTimeline.mentions(channel); },
               [&](const auto& channel) { return m_dmMode || serverChannelVisible(channel); }
           )) {
        if (row.channel->id == hit.id) {
          m_scrolling.stop(m_channelScroll);
          // Reveal the row above the floating badge, without selecting or ACKing it.
          const float h = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
          const float listTop = m_dmMode ? layout::DmSidebar::channelsTop : layout::Sidebar::channelsTop;
          m_channelScroll = std::max(0.0F, listTop + row.top - (h - 160));
          break;
        }
      }
      break;
    }
    case Action::Folder:
      if (!m_openFolders.erase(hit.id))
        m_openFolders.insert(hit.id);
      saveExpandedFolders();
      break;
    case Action::DmProfileRetry:
      refreshDmProfile(true);
      break;
    case Action::ToggleMembers:
      if (!m_channelPanel.empty()) {
        closeChannelPanel();
        m_showMembers = true;
      } else
        m_showMembers = !m_showMembers;
      if (m_showMembers)
        refreshDmProfile();
      break;
    case Action::PasteClipboard:
      if (!m_demo && !m_channelId.empty()) {
        focusControl("composer");
        m_pasteInput = "composer";
        m_pasteServerScope = serverSettingsScope();
        m_pasteConnected = m_connected;
        m_pasteSession = m_session;
        m_pasteChannel = m_channelId;
        m_window.paste(true);
      }
      break;
    case Action::ToggleStickers:
    case Action::StickerSection:
    case Action::SendSticker:
    case Action::RemoveSticker:
    case Action::StickerInfo:
    case Action::StickerRetry:
      stickerAction(hit);
      break;
    case Action::ToggleEmoji:
      if (m_stickerTab) {
        closeEmojiPicker();
        m_emojiQuery.clear();
        m_emojiScroll = m_emojiRailScroll = 0;
      }
      closeGifPicker();
      m_scrolling.stopAll();
      if (m_emojiMenu)
        closeEmojiPicker();
      else
        m_emojiMenu = true;
      break;
    case Action::CloseEmoji:
      closeEmojiPicker();
      m_reactionMessage.clear();
      m_reactionChannel.clear();
      break;
    case Action::PickEmoji:
      break;
    case Action::FavoriteEmoji:
      favoriteEmoji(hit.id);
      break;
    case Action::EmojiSection:
      m_scrolling.stop(m_emojiScroll);
      m_emojiQuery.clear();
      m_emojiSection = hit.id;
      if (hit.id == "all") {
        m_emojiScroll = 0;
        m_emojiJump.clear();
      } else
        m_emojiJump = hit.id;
      prepareEmojiPicker();
      break;
    case Action::EmojiRetry:
      m_emojiError.clear();
      if (!hit.id.empty())
        m_emojiGuildErrors.erase(hit.id);
      prepareEmojiPicker();
      break;
    case Action::ToggleGif:
      m_scrolling.stopAll();
      if (m_gifMenu)
        closeGifPicker();
      else {
        closeContext();
        closeEmojiPicker();
        m_presenceMenu = false;
        m_gifMenu = true;
        m_gifScroll = 0;
        m_focusedInput = "gif-search";
        if (!m_demo && !m_loadingGifFavorites)
          m_loadingGifFavorites = m_client.gifFavorites();
        if (!m_gifFavoritesTab)
          searchGifs();
      }
      break;
    case Action::CloseGif:
      closeGifPicker();
      break;
    case Action::GifTab:
      m_scrolling.stop(m_gifScroll);
      m_gifFavoritesTab = hit.id == "favorites";
      m_gifScroll = 0;
      m_gifSearchTimer.stop();
      if (!m_gifFavoritesTab)
        searchGifs();
      else if (!m_demo && !m_gifFavoritesLoaded && !m_loadingGifFavorites)
        m_loadingGifFavorites = m_client.gifFavorites();
      break;
    case Action::GifRetry:
      if (m_gifFavoritesTab) {
        m_gifFavoriteError.clear();
        if (!m_demo)
          m_loadingGifFavorites = m_client.gifFavorites();
      } else
        searchGifs();
      break;
    case Action::FavoriteGif:
      if (const auto gif = m_gifTargets.find(hit.id); gif != m_gifTargets.end())
        toggleGifFavorite(gif->second);
      break;
    case Action::SendGif:
      if (!canSendToThread())
        break;
      if (const auto gif = m_gifTargets.find(hit.id); gif != m_gifTargets.end() && !m_demo && !m_channelId.empty()) {
        if (queueOutgoing(gif->second.url, {}, {}, {}, false, gif->second))
          closeGifPicker();
        else
          m_gifError = "Could not send this GIF. Check the connection.";
      }
      break;
    case Action::InsertEmoji:
      m_emojiMenu = false;
      focusControl("composer");
      if (!m_demo)
        pasteControl("composer", hit.id);
      break;
    case Action::RestoreOutgoing:
      restoreOutgoing(hit.id);
      break;
    case Action::Send:
      send();
      break;
    case Action::Token:
      m_fieldFocused = true;
      m_selectAll = false;
      m_edit.end(m_token);
      break;
    case Action::Composer:
      m_fieldFocused = true;
      m_selectAll = false;
      m_dragDraft = true;
      positionCaret(m_pointerX, m_pointerY, false);
      break;
    case Action::SelectMessage:
      m_fieldFocused = false;
      m_selectedMessage = hit.id;
      m_selectedText = m_messageText[hit.id];
      m_dragMessage = true;
      positionChatSelection(m_pointerX, m_pointerY);
      m_chatEdit.anchor = m_chatEdit.caret;
      break;
    case Action::CopyMessage:
      m_window.copy(m_messageText[hit.id]);
      break;
    case Action::SaveMessageEdit:
      submitMessageEdit();
      break;
    case Action::CompleteText:
      chooseCompletion(std::stoul(hit.id));
      break;
    case Action::CancelMessageEdit:
      if (m_messageEdit && !m_messageEdit->request)
        cancelMessageEdit();
      break;
    case Action::CopyCode:
      m_window.copy(hit.id);
      break;
    case Action::OpenUrl:
      if (!openDiscordLink(hit.id))
        auth::openExternalUrl(hit.id);
      break;
    case Action::RevealSpoiler:
      m_revealedSpoilers.insert(hit.id);
      break;
    case Action::ViewAttachment:
      openDraftAttachment(hit.id);
      break;
    case Action::RemoveAttachment:
      std::erase_if(m_attachments[m_channelId], [&](auto& image) {
        if (image.key != hit.id)
          return false;
        m_window.context().textureManager().unload(image.texture);
        return true;
      });
      break;
    case Action::SignOut:
      resetReadAcknowledgements();
      m_desktopNotifications.clear();
      m_notifiedMessages.clear();
      closeVideo();
      clearEmojiState();
      m_scrolling.stopAll();
      clearGifs();
      clearInteractions();
      clearAccountSettings();
      m_controlInput.setFocus(nullptr);
      m_focusedInput.clear();
      m_emojiMenu = false;
      closeServerSettings(true);
      m_settingsOpen = m_presenceMenu = m_savingProfile = m_loadingProfile = false;
      m_desiredPresence.clear();
      m_settingsStatus.clear();
      m_settingsField.clear();
      m_settingsFields.clear();
      closeImageViewer();
      m_presence = {};
      m_login.cancel();
      m_session = m_demo ? m_client.disconnect() : m_client.logout();
      m_restoringLogin = m_retrySavedLogin = false;
      m_loginStorageStatus.clear();
      m_connected = false;
      m_demo = false;
      m_connecting = false;
      m_live = false;
      m_dmMode = false;
      m_dmPage.clear();
      m_dmSearch.clear();
      m_dmFriendName.clear();
      m_dmStatus.clear();
      m_dmPending.clear();
      m_dmPreview.clear();
      m_dmPreviewId.clear();
      m_dmFriendRequest = m_dmPreviewRequest = 0;
      m_privateChannels.clear();
      m_userDirectory = {};
      m_roleDirectory = {};
      m_messageLog.reset();
      m_roleMotion.clear();
      m_unreadDMs.clear();
      m_unreadChannels.clear();
      m_unreadGuilds.clear();
      m_localRead.clear();
      m_unreadTimeline.reset();
      m_history.reset();
      m_adoptedChatPreload.reset();
      m_timelineAnchor.reset();
      m_guildChannels.clear();
      clearThreads();
      m_channelCacheOrder.clear();
      clearChatPreload();
      m_channelMessages.clear();
      m_channelHistoryState.clear();
      m_channelMembers.clear();
      m_lastChannels.clear();
      m_collapsedCategories.clear();
      m_token.clear();
      m_account = {};
      clearTyping();
      m_guilds.clear();
      m_guildUpdates.clear();
      resetGuildBootstrap();
      m_channels.clear();
      m_messages.clear();
      m_messageLayouts.clear();
      m_members.clear();
      m_activeMemberIdentity.clear();
      m_folders.clear();
      m_folderMotion.clear();
      m_openFolders.clear();
      m_liveChanges.clear();
      for (auto& [_, images] : m_attachments)
        for (auto& image : images)
          m_window.context().textureManager().unload(image.texture);
      m_attachments.clear();
      m_edit = {};
      m_selectedText.clear();
      m_selectedMessage.clear();
      m_drafts.clear();
      m_draftMentionNames.clear();
      m_guildId.clear();
      m_channelId.clear();
      m_status.clear();
      m_fieldFocused = true;
      m_showTokenEntry = false;
      break;
    default:
      // Interaction actions are handled before this switch.
      break;
    }
    m_window.invalidate();
  }
} // namespace app
