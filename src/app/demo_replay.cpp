#include "api/uploads.h"
#include "app/automod_message.h"
#include "app/client_application.h"
#include "app/composer_format.h"
#include "app/member_list_layout.h"
#include "app/message_group.h"
#include "app/message_layout.h"
#include "app/message_log_layout.h"
#include "app/message_mentions.h"
#include "app/message_preferences.h"
#include "app/reply_preview.h"
#include "app/sidebar_activity.h"
#include "app/single_instance.h"
#include "app/stream_popout.h"
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
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/scrollbar.h"
#include "ui/palette.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;

  void ClientApplication::demoFrame(std::chrono::steady_clock::time_point drawStarted) {
    if (m_demo && m_demoOverlay.starts_with("sound-cycle") && m_demoInteractionStep < soundReplayFrames)
      demoSoundStep();
    if (m_demo && m_demoOverlay.starts_with("login-cycle") && m_demoInteractionStep < loginReplayFrames)
      demoLoginStep();
    if (m_demo
        && m_demoOverlay.starts_with("startup-transition")
        && m_demoInteractionStep < startupTransitionReplayFrames)
      demoStartupTransitionStep();
    if (m_demo && m_demoOverlay.starts_with("startup-cycle") && m_demoInteractionStep < startupReplayFrames)
      demoStartupStep();
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
    if (m_demo && m_demoOverlay.starts_with("syntax-"))
      demoSyntaxCheck();
    if (m_demo && m_demoOverlay.starts_with("startup-loading") && !m_startupLoading)
      throw std::runtime_error("The standalone startup preview must render its loading gate");
    if (m_demo && m_demoOverlay.starts_with("login-themed")) {
      const bool light = m_demoOverlay == "login-themed-light" || m_demoOverlay == "login-themed-system-light";
      const auto& palette =
          light ? m_themeSnapshot.selected("gruvbox").light : m_themeSnapshot.selected("gruvbox").dark;
      const auto* surface = dynamic_cast<const RectNode*>(m_scene->children().front().get());
      if (m_connected
          || !surface
          || surface->style().fill != themeColor(palette.background)
          || theme::foreground != themeColor(palette.foreground)
          || theme::accent != themeColor(palette.accent))
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
        throw std::runtime_error("Forwarded messages must show the snapshot and every image with explicit attribution");
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
        // Header selection targets also belong to this message; inspect its body row.
        const auto selected = std::ranges::find_if(m_hits, [&](const auto& hit) {
          return hit.action == Action::SelectMessage
              && hit.id == message.id
              && hit.x == chatLeft() + layout::Chat::inset + layout::Chat::authorGutter;
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
        require(hasHighlight, "Hover paints the continuation background");
        require(hasInk, "Hover paints the continuation text");
        require(hasTime, "Hover paints the continuation timestamp");
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
    if (m_demo && m_demoOverlay.starts_with("inline-edit-cycle") && m_demoInteractionStep < 9)
      demoMessageEditStep();
    if (m_demo && m_demoOverlay == "autocomplete-cycle" && m_demoInteractionStep < 9)
      demoCompletionStep();
    if (m_demo && m_demoOverlay.starts_with("slash-commands-cycle") && m_demoInteractionStep < slashCommandReplayFrames)
      demoSlashCommandStep();
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
    if (m_demo && m_demoOverlay.starts_with("sharing-cycle") && m_demoInteractionStep < sharingReplayFrames)
      demoSharingStep();
    if (m_demo && m_demoOverlay.starts_with("voice-cycle") && m_demoInteractionStep < voiceReplayFrames)
      demoVoiceStep();
    if (m_demo && m_demoOverlay.starts_with("rpc-cycle") && m_demoInteractionStep < rpcReplayFrames)
      demoRpcStep();
    if (m_demo && m_demoOverlay.starts_with("ime-cycle") && m_demoInteractionStep < imeReplayFrames)
      replayImeStep();
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
    if (m_demo && m_demoOverlay == "themes-cycle" && m_demoInteractionStep < themeReplayFrames)
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
    if (m_demo && m_demoOverlay.starts_with("bot-messages-cycle") && m_demoInteractionStep < botMessagesReplayFrames)
      demoBotMessagesStep();
    if (m_demo && m_demoOverlay.starts_with("timestamps-cycle") && m_demoInteractionStep < timestampReplayFrames)
      demoTimestampStep();
    if (m_demo
        && m_demoOverlay.starts_with("reaction-browser-cycle")
        && m_demoInteractionStep < reactionBrowserReplayFrames)
      demoReactionBrowserStep();
    if (m_demo
        && m_demoOverlay.starts_with("sidebar-activity-cycle")
        && m_demoInteractionStep < sidebarActivityReplayFrames)
      demoSidebarActivityStep();
    if (m_demo && m_demoOverlay.starts_with("composer-edit-cycle") && m_demoInteractionStep < composerEditReplayFrames)
      demoComposerEditStep();
    if (m_demo && m_demoOverlay.starts_with("markdown-cycle") && m_demoInteractionStep < markdownReplayFrames)
      demoMarkdownStep();
    if (m_demo && m_demoOverlay.starts_with("media-gallery-cycle") && m_demoInteractionStep < mediaGalleryReplayFrames)
      demoMediaGalleryStep();
    if (m_demo
        && m_demoOverlay.starts_with("image-menu")
        && m_demoInteractionStep < (m_demoOverlay.starts_with("image-menu-cycle") ? imageMenuReplayFrames : 2))
      demoImageMenuStep();
    if (m_demo && m_demoOverlay.starts_with("inbox-cycle") && m_demoInteractionStep < inboxReplayFrames)
      demoInboxStep();
    if (m_demo && m_demoOverlay.starts_with("search-cycle") && m_demoInteractionStep < searchReplayFrames)
      demoSearchStep();
    if (m_demo && m_demoOverlay.starts_with("permissions-cycle") && m_demoInteractionStep < permissionsReplayFrames)
      demoPermissionsStep();
    if (m_demo && m_demoOverlay.starts_with("polls-cycle") && m_demoInteractionStep < pollReplayFrames)
      demoPollStep();
    if (m_demo && m_demoOverlay.starts_with("voice-messages-cycle") && m_demoInteractionStep < voiceMessageReplaySteps)
      demoVoiceMessageStep();
    if (m_demo && m_demoOverlay.starts_with("outgoing-cycle") && m_demoInteractionStep < outgoingReplayFrames)
      demoOutgoingStep();
    if (m_demo && m_demoOverlay.starts_with("server-access-cycle") && m_demoInteractionStep < serverAccessReplayFrames)
      demoServerAccessStep();
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
                == formatMessageTimestamp(m_messages.front().timestamp, m_preferences.twelveHourClock)
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
        const auto a = std::ranges::find_if(m_textAreas.rbegin(), m_textAreas.rend(), [](const auto& area) {
          return area.message == "9101" && !area.header;
        });
        const auto b =
            std::ranges::find_if(m_textAreas, [](const auto& area) { return area.message == "9102" && !area.header; });
        const auto c =
            std::ranges::find_if(m_textAreas, [](const auto& area) { return area.message == "9103" && !area.header; });
        const auto aY =
            m_messageTops.at("9101") + layout::Chat::headerHeight + m_messageLayouts.at("9101").layout.runs.back().y;
        const auto bY = m_messageTops.at("9102") + m_messageLayouts.at("9102").layout.runs.front().y;
        const auto cY = m_messageTops.at("9103") + m_messageLayouts.at("9103").layout.runs.front().y;
        if (a == m_textAreas.rend()
            || b == m_textAreas.end()
            || c == m_textAreas.end()
            || std::abs((bY - aY) - (cY - bY)) > .1F
            || std::abs((cY - bY) - a->run.height) > .1F)
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
    if (m_demo && m_demoOverlay.starts_with("forum-cycle"))
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
    if (m_demo && m_demoOverlay.starts_with("unread-badge-cycle")) {
      const auto step = m_demoInteractionStep++;
      const auto checkRow = [&](const std::string& channel, bool unread) {
        const auto row = std::ranges::find_if(m_hits, [&](const auto& hit) {
          return hit.action == Action::Channel && hit.id == channel;
        });
        if (row == m_hits.end())
          throw std::runtime_error("The unread mute replay requires a visible channel row");
        const auto entry = std::ranges::find(m_channels, channel, &api::Channel::id);
        bool name = false, marker = false;
        const auto inspect = [&](auto&& self, const Node& node, float x, float y) -> void {
          x += node.x();
          y += node.y();
          if (x >= layout::Sidebar::railWidth && x < chatLeft() && node.type() == NodeType::Text) {
            const auto& text = static_cast<const TextNode&>(node);
            if (text.text() == entry->name) {
              if (text.fontWeight() != FontWeight::Normal || text.color() != (unread ? foreground : muted))
                throw std::runtime_error("Channel unread styling uses color independently of server mute");
              name = true;
            }
          }
          if (node.type() == NodeType::Rect
              && x == layout::Sidebar::unreadX
              && node.width() == layout::Sidebar::unreadWidth
              && node.height() == layout::Sidebar::unreadHeight
              && y >= row->y
              && y < row->y + row->height)
            marker = true;
          for (const auto& child : node.children())
            self(self, *child, x, y);
        };
        inspect(inspect, *m_scene, 0, 0);
        if (!name || marker != unread)
          throw std::runtime_error("Only channel mute suppresses that channel's ordinary unread marker");
      };
      checkRow(step == 1 ? "1200" : "103", step != 3);
      const auto at = std::ranges::find(m_hits, Action::JumpChannelMention, &Hit::action);
      if (step == 0 || step == 2) {
        if (at == m_hits.end() || at->id != "1200")
          throw std::runtime_error("Offscreen mentioned channel needs a floating badge");
        const auto hit = *at;
        if (step == 0)
          activate(hit);
        else {
          api::UserUpdates muted;
          muted.mutedChannels["103"] = true;
          m_userDirectory.apply(muted);
          m_unreadTimeline.acknowledged("1200", "9000");
        }
      } else if (step == 1) {
        if ((at != m_hits.end() && at->id == "1200") || m_unreadTimeline.mentions("1200") != 2 || m_channelId != "101")
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
    if (m_demo && m_demoOverlay.starts_with("expression-cycle") && m_demoInteractionStep < expressionReplayFrames)
      demoExpressionStep();
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
  }

  int ClientApplication::demoFrameLimit(const char* view) const {
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
    return (
        m_demo && view && std::string_view(view).starts_with("voice-messages-cycle")        ? 512
            : m_demo && view && std::string_view(view).starts_with("polls-cycle")           ? pollReplayFrames
            : m_demo && view && std::string_view(view).starts_with("inbox-cycle")           ? inboxReplayFrames
            : m_demo && view && std::string_view(view).starts_with("search-cycle")          ? searchReplayFrames
            : m_demo && view && std::string_view(view).starts_with("server-access-cycle")   ? serverAccessReplayFrames
            : m_demo && view && std::string_view(view).starts_with("permissions-cycle")     ? permissionsReplayFrames
            : m_demo && view && std::string_view(view).starts_with("outgoing-cycle")        ? outgoingReplayFrames
            : m_demo && view && std::string_view(view).starts_with("sound-cycle")           ? soundReplayFrames
            : m_demo && view && std::string_view(view).starts_with("chat-links-cycle")      ? chatLinkReplayFrames
            : m_demo && view && std::string_view(view) == "attachment-cycle"                ? attachmentReplayFrames
            : m_demo && view && std::string_view(view).starts_with("window-controls-cycle") ? windowControlsReplayFrames
            : m_demo && view && std::string_view(view) == "tray-cycle"                      ? trayReplayFrames
            : m_demo && view && std::string_view(view).starts_with("expression-cycle")      ? expressionReplayFrames
            : m_demo && view && std::string_view(view) == "sticker-cycle"                   ? 8
            : m_demo && view && std::string_view(view) == "dm-compose-cycle"                ? 16
            : m_demo && view && std::string_view(view).starts_with("dm-home-cycle")         ? dmHomeReplayFrames
            : m_demo && view && std::string_view(view).starts_with("reply-cycle")           ? composerReplyReplayFrames
            : m_demo && view && std::string_view(view).starts_with("embed-cycle")           ? embedReplayFrames
            : m_demo && view && std::string_view(view).starts_with("bot-messages-cycle")    ? botMessagesReplayFrames
            : m_demo && view && std::string_view(view).starts_with("composer-edit-cycle")   ? composerEditReplayFrames
            : m_demo && view && std::string_view(view).starts_with("reaction-browser-cycle")
            ? reactionBrowserReplayFrames
            : m_demo && view && std::string_view(view).starts_with("timestamps-cycle")   ? timestampReplayFrames
            : m_demo && view && std::string_view(view).starts_with("login-cycle")        ? loginReplayFrames
            : m_demo && view && std::string_view(view).starts_with("startup-transition") ? startupTransitionReplayFrames
            : m_demo && view && std::string_view(view).starts_with("startup-cycle")      ? startupReplayFrames
            : m_demo && view && std::string_view(view).starts_with("markdown-cycle")     ? markdownReplayFrames
            : m_demo && view && std::string_view(view).starts_with("reply-grouping")     ? replyGroupingReplayFrames
            : m_demo && view && std::string_view(view).starts_with("media-gallery-cycle")  ? mediaGalleryReplayFrames
            : m_demo && view && std::string_view(view).starts_with("image-menu-cycle")     ? imageMenuReplayFrames
            : m_demo && view && std::string_view(view) == "chat-preload-cycle"             ? 10
            : m_demo && view && std::string_view(view) == "join-cycle"                     ? 4
            : m_demo && view && std::string_view(view) == "deleted-cycle"                  ? 3
            : m_demo && view && std::string_view(view).starts_with("inline-edit-cycle")    ? 9
            : m_demo && view && std::string_view(view) == "autocomplete-cycle"             ? 9
            : m_demo && view && std::string_view(view).starts_with("slash-commands-cycle") ? slashCommandReplayFrames
            : m_demo && view && std::string_view(view).starts_with("decorations-cycle")    ? 4
            : m_demo && view && std::string_view(view).starts_with("reduced-motion-cycle") ? reducedMotionReplayFrames
            : m_demo && view && std::string_view(view).starts_with("sharing-cycle")        ? sharingReplayFrames
            : m_demo && view && std::string_view(view).starts_with("voice-cycle")          ? voiceReplayFrames
            : m_demo && view && std::string_view(view).starts_with("rpc-cycle")            ? rpcReplayFrames
            : m_demo && view && std::string_view(view).starts_with("ime-cycle")            ? imeReplayFrames
            : m_demo && view && std::string_view(view) == "user-preferences-cycle"         ? 12
            : m_demo && view && std::string_view(view) == "user-account-cycle"             ? 16
            : m_demo && view && std::string_view(view) == "user-settings-lists-cycle"      ? 30
            : m_demo && view && std::string_view(view) == "settings-font-cycle"            ? 12
            : m_demo && view && std::string_view(view) == "server-hover-cycle"             ? 7
            : m_demo && view && std::string_view(view) == "themes-cycle"                   ? themeReplayFrames
            : m_demo && view && std::string_view(view).starts_with("message-hover-cycle")  ? 10
            : m_demo && view && std::string_view(view) == "sidebar-cycle"                  ? 10
            : m_demo && view && std::string_view(view) == "role-emoji-cycle"               ? 3
            : m_demo && view && std::string_view(view) == "role-members-cycle"             ? 2
            : m_demo && view && std::string_view(view) == "mentions-cycle"                 ? 4
            : m_demo && view && std::string_view(view) == "channel-menu-scroll-cycle"      ? 3
            : m_demo && view && std::string_view(view) == "channel-create-cycle"           ? 12
            : m_demo && view && std::string_view(view) == "channel-invite-cycle"           ? 18
            : m_demo && view && std::string_view(view) == "server-settings-commands-cycle" ? 12
            : m_demo && view && std::string_view(view) == "server-settings-expressions-cycle" ? 26
            : m_demo && view && std::string_view(view) == "server-settings-management-cycle"  ? 12
            : m_demo && view && std::string_view(view) == "server-settings-cycle"             ? 42
            : m_demo && view && std::string_view(view).starts_with("floating-cycle")          ? floatingReplayFrames
            : m_demo && view && std::string_view(view).starts_with("typing-cycle")            ? typingReplayFrames
            : m_demo && view && std::string_view(view).starts_with("rail-drag")               ? 10
            : m_demo && view && std::string_view(view).starts_with("folder-animation")        ? 9
            : m_demo && view && std::string_view(view) == "folder-menu-cycle"                 ? 13
            : m_demo && view && std::string_view(view) == "server-menu-cycle"                 ? 20
            : m_demo && view && std::string_view(view) == "channel-menu-cycle"                ? 31
            : m_demo && view && std::string_view(view) == "channel-cycle"                     ? 18
            : m_demo && view && std::string_view(view).starts_with("forum-cycle")             ? 12
            : m_demo && view && std::string_view(view) == "thread-cycle"                      ? 10
            : m_demo && view && std::string_view(view) == "guild-tag-cycle"                   ? 10
            : m_demo && view && std::string_view(view).starts_with("cosmetics-cycle")         ? cosmeticsReplayFrames
            : m_demo && view && std::string_view(view).starts_with("dm-profile-cycle")        ? dmProfileReplayFrames
            : m_demo && view && std::string_view(view) == "profile-card-cycle"                ? 8
            : m_demo && view && std::string_view(view) == "role-style-cycle"                  ? 7
            : m_demo && view && std::string_view(view).starts_with("sidebar-activity-cycle")
            ? sidebarActivityReplayFrames
            : m_demo && view && std::string_view(view).starts_with("unread-badge-cycle") ? 4
            : m_demo && view && std::string_view(view) == "unread-cycle"                 ? 10
            : m_demo && view && std::string_view(view) == "jump-latest-cycle"            ? 8
            : m_demo && view && std::string_view(view) == "connection-cycle"             ? 8
            : interactionCycle ? (std::string_view(view) == "video-cycle" ? 512 : 64)
                               : 1
    );
  }

  void ClientApplication::demoVerifyCompletion() const {
    if (m_demoOverlay.starts_with("sound-cycle") && m_demoInteractionStep != soundReplayFrames)
      throw std::runtime_error("Sound settings replay did not finish");
    if (m_demoOverlay.starts_with("voice-messages-cycle") && m_demoInteractionStep != voiceMessageReplaySteps)
      throw std::runtime_error("Voice message replay did not finish");
    if (m_demoOverlay.starts_with("polls-cycle") && m_demoInteractionStep != pollReplayFrames)
      throw std::runtime_error("Poll replay did not finish");
    if (m_demoOverlay.starts_with("login-cycle") && m_demoInteractionStep != loginReplayFrames)
      throw std::runtime_error("Sign-in replay did not finish");
    if (m_demoOverlay.starts_with("startup-transition") && m_demoInteractionStep != startupTransitionReplayFrames)
      throw std::runtime_error("Startup transition replay did not finish");
    if (m_demoOverlay.starts_with("startup-cycle") && m_demoInteractionStep != startupReplayFrames)
      throw std::runtime_error("Startup preparation replay did not finish");
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
    if (m_demoOverlay.starts_with("inline-edit-cycle") && m_demoInteractionStep != 9)
      throw std::runtime_error("Inline message editing replay did not finish");
    if (m_demoOverlay == "autocomplete-cycle" && m_demoInteractionStep != 9)
      throw std::runtime_error("Autocomplete replay did not finish");
    if (m_demoOverlay.starts_with("slash-commands-cycle") && m_demoInteractionStep != slashCommandReplayFrames)
      throw std::runtime_error("Slash command replay did not finish");
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
    if (m_demoOverlay == "themes-cycle" && m_demoInteractionStep != themeReplayFrames)
      throw std::runtime_error("Theme settings replay did not finish");
    if (m_demoOverlay.starts_with("message-hover-cycle") && m_demoInteractionStep != 10)
      throw std::runtime_error("Message hover replay did not finish");
    if (m_demoOverlay == "join-cycle" && m_demoInteractionStep != 4)
      throw std::runtime_error("Server join replay did not complete");
    if (m_demoOverlay.starts_with("expression-cycle") && m_demoInteractionStep != expressionReplayFrames)
      throw std::runtime_error("Expression information replay did not finish");
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
    if (m_demoOverlay.starts_with("voice-cycle") && m_demoInteractionStep != voiceReplayFrames)
      throw std::runtime_error("The voice replay did not complete");
    if (m_demoOverlay.starts_with("rpc-cycle") && m_demoInteractionStep != rpcReplayFrames)
      throw std::runtime_error("The RPC replay did not complete");
    if (m_demoOverlay.starts_with("sharing-cycle") && m_demoInteractionStep != sharingReplayFrames)
      throw std::runtime_error("Sharing replay did not finish");
    if (m_demoOverlay.starts_with("ime-cycle") && m_demoInteractionStep != imeReplayFrames)
      throw std::runtime_error("The input-method replay did not complete");
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
    if (m_demoOverlay.starts_with("bot-messages-cycle") && m_demoInteractionStep != botMessagesReplayFrames)
      throw std::runtime_error("Bot identity and message deletion replay did not complete");
    if (m_demoOverlay.starts_with("timestamps-cycle") && m_demoInteractionStep != timestampReplayFrames)
      throw std::runtime_error("Message timestamp replay did not complete");
    if (m_demoOverlay.starts_with("composer-edit-cycle") && m_demoInteractionStep != composerEditReplayFrames)
      throw std::runtime_error("Composer editing replay did not complete");
    if (m_demoOverlay.starts_with("sidebar-activity-cycle") && m_demoInteractionStep != sidebarActivityReplayFrames)
      throw std::runtime_error("Sidebar activity replay did not complete");
    if (m_demoOverlay.starts_with("reaction-browser-cycle") && m_demoInteractionStep != reactionBrowserReplayFrames)
      throw std::runtime_error("Reaction browser replay did not complete");
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
    if (m_demoOverlay.starts_with("permissions-cycle") && m_demoInteractionStep != permissionsReplayFrames)
      throw std::runtime_error("Channel permission replay did not complete");
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
    if (m_demoOverlay.starts_with("forum-cycle") && m_demoInteractionStep != 12)
      throw std::runtime_error("Forum replay did not complete");
    if (m_demoOverlay.starts_with("cosmetics-cycle") && m_demoInteractionStep != cosmeticsReplayFrames)
      throw std::runtime_error("Cosmetic replay did not complete");
    if (m_demoOverlay.starts_with("dm-profile-cycle") && m_demoInteractionStep != dmProfileReplayFrames)
      throw std::runtime_error("DM profile replay did not finish");
    if (m_demoOverlay == "thread-cycle" && m_demoInteractionStep != 10)
      throw std::runtime_error("Thread replay did not complete");
    if (m_demoOverlay == "video-cycle" && m_demoInteractionStep < 11)
      throw std::runtime_error("Native video replay did not complete");
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
} // namespace app
