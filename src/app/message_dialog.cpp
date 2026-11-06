#include "app/client_application.h"
#include "app/message_preferences.h"
#include "app/theme.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  bool ClientApplication::beginMessageEdit(const std::string& id) {
    const auto message = std::ranges::find(m_messages, id, &api::Message::id);
    if (message == m_messages.end()
        || message->authorId != m_account.id
        || message->deleted
        || message->forwarded
        || message->poll
        || (message->flags & api::messageVoice)
        || (!message->stickers.empty() && message->content.empty())
        || !message->webhookId.empty()
        || (message->flags & 64)
        || (message->type != 0 && message->type != 19)
        || (m_messageEdit && m_messageEdit->request))
      return false;
    cancelMessageEdit(false);
    m_messageEdit = MessageEditState{m_channelId, id, message->content, {}, {}, ++m_messageEditGeneration};
    closeGifPicker();
    closeEmojiPicker();
    m_controlInput.setFocus(nullptr);
    m_focusedInput = "message-edit";
    m_fieldFocused = false;
    m_dragDraft = m_dragMessage = false;
    m_scrolling.stop(m_messageScroll);
    m_seekMessage = id;
    m_window.invalidate();
    return true;
  }

  void ClientApplication::cancelMessageEdit(bool restoreComposer) {
    if (!m_messageEdit)
      return;
    if (m_completionInput == "message-edit")
      closeCompletion();
    const bool focused = m_focusedInput == "message-edit";
    m_messageEdit.reset();
    ++m_messageEditGeneration;
    if (m_inputs.contains("message-edit"))
      m_inputs.at("message-edit")->setValue("");
    if (focused) {
      m_controlInput.setFocus(nullptr);
      m_focusedInput = restoreComposer ? "composer" : "";
      m_fieldFocused = restoreComposer;
    }
    m_window.invalidate();
  }

  bool ClientApplication::messageEditKey(const Key& key) {
    if (!m_connected
        || settingsVisible()
        || !m_dialogKind.empty()
        || m_contextOpen
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || forumView())
      return false;
    if (m_messageEdit && m_focusedInput == "message-edit" && key.symbol == XKB_KEY_Escape) {
      if (!m_messageEdit->request)
        cancelMessageEdit();
      return true;
    }
    if (key.symbol != XKB_KEY_Up
        || key.control
        || key.shift
        || m_messageEdit
        || m_sending
        || m_loadingMessages
        || (!m_demo && !m_history.initialized())
        || !m_history.atLatest()
        || m_channelId.empty()
        || !m_drafts[m_channelId].empty()
        || !m_attachments[m_channelId].empty()
        || !(m_focusedInput == "composer" || (m_focusedInput.empty() && m_fieldFocused)))
      return false;
    for (auto message = m_messages.rbegin(); message != m_messages.rend(); ++message)
      if (message->authorId == m_account.id && !message->content.empty() && beginMessageEdit(message->id))
        return true;
    return false;
  }

  void ClientApplication::submitMessageEdit() {
    if (!m_messageEdit || m_messageEdit->request || m_messageEdit->channel != m_channelId)
      return;
    const auto message = std::ranges::find(m_messages, m_messageEdit->message, &api::Message::id);
    if (message == m_messages.end() || message->deleted || message->authorId != m_account.id) {
      cancelMessageEdit();
      return;
    }
    if (message->content == m_messageEdit->text) {
      cancelMessageEdit();
      return;
    }
    if (m_messageEdit->text.size() > 8000
        || (m_messageEdit->text.empty() && message->attachments.empty() && message->stickers.empty())) {
      m_messageEdit->error = m_messageEdit->text.empty() ? "Messages cannot be empty." : "This message is too long.";
      m_window.invalidate();
      return;
    }
    api::Interaction action{
        .kind = api::InteractionKind::EditMessage,
        .channel = m_channelId,
        .message = message->id,
        .text = m_emojiCatalog.messageContent(
            m_preferences.convertEmoticons ? convertMessageEmoticons(m_messageEdit->text) : m_messageEdit->text,
            m_dmMode ? "" : m_guildId, m_account.premiumType.value_or(0), m_preferences.emojiImageLinks
        ),
        .revision = ++m_messageEditGeneration
    };
    if (startInteraction(action)) {
      m_messageEdit->request = std::move(action);
      m_messageEdit->error.clear();
    } else {
      m_messageEdit->error = m_status;
      m_status.clear();
    }
    m_window.invalidate();
  }

  float ClientApplication::messageEditHeight(float width) {
    if (!m_messageEdit)
      return 0;
    const TextPresentation presentation(m_messageEdit->text, composerReplacements(m_messageEdit->text));
    const auto measured = m_window.target().renderer().measureText(
        presentation.text().empty() ? " " : presentation.text(), m_preferences.messageSize, FontWeight::Normal,
        std::max(1.0F, width - 24), 0
    );
    const float field = std::clamp(measured.bottom - measured.top + Style::spaceXl, 44.0F, 180.0F);
    return field + 30 + (m_messageEdit->error.empty() ? 0 : 34);
  }

  void ClientApplication::drawMessageEdit(Node& parent, float x, float y, float width) {
    using namespace theme;
    if (!m_messageEdit)
      return;
    const float fieldHeight = messageEditHeight(width) - 30 - (m_messageEdit->error.empty() ? 0 : 34);
    auto& input = inputControl(parent, x, y, width, fieldHeight, "message-edit", m_messageEdit->text, "Message", true);
    input.setFontSize(m_preferences.messageSize);
    input.setMultilineCenterWhenFits(true);
    input.setFrameRadius(6);
    input.setHorizontalPadding(12);
    input.setClearButtonEnabled(false);
    const float footerY = y + fieldHeight + Style::spaceXs;
    if (width > 320)
      label(
          parent, x + Style::spaceXxs, footerY + Style::spaceCompact, width - 150, "Escape to cancel · Enter to save",
          Style::fontSizeDetail, muted
      );
    const auto control = [&](float bx, float bw, const std::string& text, Action action, bool primary) {
      const bool enabled = !m_messageEdit->request;
      Hit hit{bx, footerY, bw, 24, enabled ? action : Action::None, {}};
      // iconButton/button translate nested hits; this small text control does the same.
      float ox = 0, oy = 0;
      for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
        ox += node->x();
        oy += node->y();
      }
      const bool over = enabled && hovered(bx + ox, footerY + oy, bw, 24);
      rectangle(parent, bx, footerY, bw, 24, primary ? accent : over ? raised : panel, 4);
      const auto measured =
          m_window.target().renderer().measureText(text, Style::fontSizeDescription, FontWeight::Medium);
      label(
          parent, bx + (bw - measured.width) / 2, footerY + (24 - (measured.bottom - measured.top)) / 2,
          measured.width + Style::spaceXxs, text, Style::fontSizeDescription,
          primary       ? ink
              : enabled ? foreground
                        : muted,
          FontWeight::Medium
      );
      hit.x += ox;
      hit.y += oy;
      m_hits.push_back(std::move(hit));
    };
    control(x + width - 136, 58, "Cancel", Action::CancelMessageEdit, false);
    control(x + width - 72, 72, m_messageEdit->request ? "Saving…" : "Save", Action::SaveMessageEdit, true);
    if (!m_messageEdit->error.empty())
      label(
          parent, x + Style::spaceXxs, footerY + (Style::spaceWide + Style::borderWidth), width - Style::spaceXs,
          m_messageEdit->error, Style::fontSizeMini, theme::danger, FontWeight::Normal, 2
      );
  }

  void ClientApplication::demoMessageEditStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      onKey({XKB_KEY_Up});
      require(
          m_messageEdit && m_messageEdit->message == "9803" && m_dialogKind.empty(),
          "Up edits the latest ordinary own message, skipping deleted/system rows, without a dialog"
      );
    } else if (step == 1) {
      require(
          m_inputs.at("message-edit")->parent() && m_focusedInput == "message-edit",
          "The inline editor retains native keyboard focus in the message row"
      );
      pasteControl("message-edit", "\nPasted line");
      onKey({.symbol = XKB_KEY_Return, .shift = true});
      require(m_messageEdit->text.ends_with("Pasted line\n"), "Inline paste and Shift+Enter preserve newlines");
      onKey({XKB_KEY_Escape});
      require(
          !m_messageEdit && m_drafts[m_channelId].empty() && m_inputs.at("message-edit")->value().empty(),
          "Escape clears edit input/undo without changing the composer"
      );
    } else if (step == 2) {
      m_drafts[m_channelId] = "An unsent draft";
      m_inputs.at("composer")->setValue(m_drafts[m_channelId]);
      focusControl("composer");
      onKey({XKB_KEY_Up});
      require(
          !m_messageEdit && m_drafts[m_channelId] == "An unsent draft",
          "Up keeps ordinary composer navigation for nonempty drafts"
      );
      m_drafts[m_channelId].clear();
      m_inputs.at("composer")->setValue("");
      m_contextMessage = "9801";
      m_contextChannel = m_channelId;
      contextAction(Action::EditMessage);
      require(
          m_messageEdit && m_messageEdit->message == "9801",
          "Context Edit opens the same inline editor for older messages"
      );
    } else if (step == 3) {
      m_inputs.at("message-edit")->selectAll();
      pasteControl("message-edit", "Changed inline\nwith a second line.");
      onKey({XKB_KEY_Return});
      require(
          m_messageEdit && !m_messageEdit->error.empty() && !m_messageEdit->request,
          "Enter submits through the existing action path; demo remains read-only and preserves the draft"
      );
      api::Event failed;
      failed.operation = api::Operation::Interaction;
      failed.interaction = api::Interaction{
          .kind = api::InteractionKind::EditMessage,
          .channel = m_channelId,
          .message = "9801",
          .text = m_messageEdit->text,
          .revision = ++m_messageEditGeneration
      };
      m_messageEdit->request = failed.interaction;
      failed.error = "Could not save. Try again.";
      interactionResult(failed);
      require(
          m_messageEdit
              && !m_messageEdit->request
              && m_messageEdit->error == failed.error
              && m_messageEdit->text == "Changed inline\nwith a second line.",
          "Failed acknowledgement preserves the editor draft and error for explicit retry"
      );
    } else if (step == 4) {
      updateMessage(
          m_channelId, "9801",
          {.content = "A live edit from another device.", .editedTimestamp = "2026-10-18T12:03:00Z"}
      );
      require(
          m_messageEdit->text == "Changed inline\nwith a second line.", "Live updates cannot replace an unsaved draft"
      );
      api::Event saved;
      saved.operation = api::Operation::Interaction;
      saved.interaction = api::Interaction{
          .kind = api::InteractionKind::EditMessage,
          .channel = m_channelId,
          .message = "9801",
          .text = m_messageEdit->text,
          .revision = ++m_messageEditGeneration
      };
      m_messageEdit->request = saved.interaction;
      auto message = m_messages.front();
      message.content = m_messageEdit->text;
      message.editedTimestamp = "2026-10-18T12:04:00Z";
      saved.messages = {message};
      interactionResult(saved);
      require(
          !m_messageEdit
              && m_messages.front().content == message.content
              && !m_messageLog.edits(m_channelId, "9801").empty(),
          "Successful acknowledgement closes the editor and merges through edit history"
      );
    } else if (step == 5) {
      beginMessageEdit("9803");
      deleteMessages(m_channelId, {"9803"});
      require(
          !m_messageEdit && m_inputs.at("message-edit")->value().empty(),
          "Live deletion clears the inline editor immediately"
      );
      beginMessageEdit("9801");
      const auto staleGeneration = m_messageEdit->generation;
      cancelMessageEdit();
      beginMessageEdit("9801");
      const auto draft = m_messageEdit->text;
      m_pasteInput = "message-edit";
      m_pasteMessageEditGeneration = staleGeneration;
      m_pasteConnected = m_connected;
      m_pasteSession = m_session;
      m_pasteChannel = m_channelId;
      m_window.pasted("Stale clipboard", "text/plain");
      require(m_messageEdit->text == draft, "Clipboard replies cannot enter a reopened edit generation");
    } else if (step == 6) {
      const auto originalChannel = m_channelId;
      auto original = m_messages.front();
      api::Interaction late{
          .kind = api::InteractionKind::EditMessage,
          .channel = originalChannel,
          .message = original.id,
          .text = "A late saved edit.",
          .revision = ++m_messageEditGeneration
      };
      m_messageEdit->request = late;
      selectChannel(m_channels.back().id);
      require(
          !m_messageEdit && m_inputs.at("message-edit")->value().empty(),
          "Navigation clears draft and native edit history"
      );
      auto next = original;
      next.id = "9891";
      next.channelId = m_channelId;
      m_messages = {next};
      m_messageLayouts.clear();
      beginMessageEdit(next.id);
      api::Event event;
      event.operation = api::Operation::Interaction;
      event.interaction = late;
      original.content = late.text;
      original.editedTimestamp = "2026-10-18T12:05:00Z";
      event.messages = {original};
      interactionResult(event);
      require(
          m_messageEdit && m_messageEdit->message == next.id && m_messageEdit->text == next.content,
          "Late save acknowledgements cannot close or replace a different editor"
      );
    } else {
      require(m_messageEdit && m_inputs.at("message-edit")->parent(), "The new channel editor is rendered inline");
      openSettings();
      require(
          !m_messageEdit && m_inputs.at("message-edit")->value().empty(), "Opening settings clears inline edit state"
      );
    }
    m_window.invalidate();
  }

  void ClientApplication::drawMessageDialog(Node& root, float w, float h) {
    using namespace theme;
    if (!m_dialogAction)
      return;
    const auto message = std::ranges::find(m_messages, m_dialogAction->message, &api::Message::id);
    if (message == m_messages.end())
      return;
    using Kind = api::InteractionKind;
    const auto kind = m_dialogAction->kind;
    const bool pin = kind == Kind::PinMessage, unpin = kind == Kind::UnpinMessage;
    std::string destination;
    if (const auto channel = std::ranges::find(m_channels, message->channelId, &api::Channel::id);
        channel != m_channels.end())
      destination = "#" + channel->name;
    else if (const auto* thread = m_threads.find(m_guildId, message->channelId))
      destination = "#" + thread->name;
    else if (
        const auto conversation = std::ranges::find(m_privateChannels, message->channelId, &api::Channel::id);
        conversation != m_privateChannels.end()
    )
      destination = channelName(*conversation);
    if (destination.empty() || destination == "#")
      destination = "this conversation";
    const std::string title = pin || unpin ? m_dialogTitle : "Delete message";
    const std::string description = pin ? "Hey, just double-checking that you want to pin this message to "
            + destination
            + " for posterity and greatness?"
        : unpin ? "You sure you want to remove this pinned message from " + destination + "?"
                : "This message will be deleted for everyone.";
    const std::string confirm = pin ? (m_dialogPending ? "Pinning…" : "Oh yeah. Pin it")
        : unpin                     ? (m_dialogPending ? "Unpinning…" : "Remove it please")
                                    : (m_dialogPending ? "Deleting…" : "Delete");
    const float width = std::min(500.0F, w - Style::space2Xl), inset = Style::spaceRelaxed,
                textWidth = width - inset * 2;
    const float previewText = layout::Avatar::small + Style::spaceMd * 2;
    auto& renderer = m_window.target().renderer();
    const auto headingMetrics =
        renderer.measureText(title, Style::fontSizeHeading, FontWeight::SemiBold, width - 72, 1);
    const auto descriptionMetrics =
        renderer.measureText(description, Style::fontSizeCaption, FontWeight::Normal, textWidth, 3);
    const std::string author = userName(message->authorId, message->author);
    const auto nameMetrics = renderer.measureText(author, Style::fontSizeCaption, FontWeight::SemiBold);
    const float bodyLeft = previewText,
                bodyWidth = std::max(Style::spaceSection, textWidth - previewText - Style::spaceMd);
    const float bodyTop =
        Style::spaceMd + std::max(Style::fontSizeCaption, nameMetrics.bottom - nameMetrics.top) + Style::spaceXs;
    const auto measure = [&](std::string_view value, const MessageStyle& style) {
      return renderer
          .measureText(
              messageMarkup(value, style), style.size, style.bold ? FontWeight::Bold : FontWeight::Normal, 0, 1,
              TextAlign::Start, style.code ? "monospace" : "", TextEllipsize::End, true
          )
          .width;
    };
    auto displayed = *message;
    for (auto& [id, name] : displayed.mentions)
      name = userName(id, name);
    if (!m_preferences.showEmbeds)
      displayed.embeds.clear();
    if (!m_preferences.showImages)
      for (auto& attachment : displayed.attachments)
        attachment.image.url.clear();
    displayed.forwarded.reset();
    displayed.reply.reset();
    if (blockedMessage(*message)) {
      displayed = {};
      displayed.content = "Message from a blocked user.";
    }
    const auto body = layoutRichMessage(
        displayed, bodyWidth, measure, Style::fontSizeCaption,
        {.emojiImageLinks = m_preferences.emojiImageLinks && m_preferences.showImages,
         .compact = true,
         .unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); },
         .channelMention = [this](auto guild, auto channel) { return channelMention(guild, channel); },
         .roleMention =
             [this](auto role) { return m_roleDirectory.roleName(m_dmMode ? "" : m_guildId, std::string(role)); },
         .twelveHourClock = m_preferences.twelveHourClock,
         .showEmbedIcons = m_preferences.showImages}
    );
    const float headingHeight = std::max(Style::fontSizeHeading + 4, headingMetrics.bottom - headingMetrics.top);
    const float descriptionHeight =
        std::max(Style::fontSizeCaption + 4, descriptionMetrics.bottom - descriptionMetrics.top);
    const float footerHeight = Style::controlHeightSm + Style::spaceLg * 2;
    const float errorHeight = m_dialogError.empty() ? 0 : 38;
    const float headingTop = inset + Style::borderWidth, descriptionTop = headingTop + headingHeight + Style::spaceSm,
                previewTop = descriptionTop + descriptionHeight + Style::spaceMd;
    // Keep the dialog inside the window; long messages and tall media are clipped.
    const float chrome = previewTop + inset + errorHeight + footerHeight + bodyTop + Style::spaceMd;
    const float bodyLimit = std::clamp(
        h - layout::Window::titlebarHeight - Style::space2Xl * 2 - chrome, Style::spaceSection, Style::spaceSection * 8
    );
    const float bodyHeight = std::min(std::max(body.height, Style::fontSizeCaption), bodyLimit);
    const float previewHeight =
        std::max(bodyTop + bodyHeight + Style::spaceMd, layout::Avatar::small + Style::spaceMd * 2);
    const float height = previewTop + previewHeight + inset + errorHeight + footerHeight;
    const float x = (w - width) / 2, y = std::max(44.0F, (h - height) / 2);
    const auto red = theme::destructive;
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, Color{0, 0, 0, .65F});
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::None, {}});
    rectangle(root, x, y, width, height, sidebar, Style::radiusXl);
    m_dialogBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    label(root, x + inset, y + headingTop, width - 72, title, Style::fontSizeHeading, foreground, FontWeight::SemiBold);
    iconButton(
        root, {x + width - 42, y + Style::spaceMd, 30, 30, m_dialogPending ? Action::None : Action::CloseDialog, {}},
        "x", "Close"
    );
    {
      label(
          root, x + inset, y + descriptionTop, textWidth, description, Style::fontSizeCaption, foreground,
          FontWeight::Normal, 3
      );
      auto preview = std::make_unique<Node>();
      preview->setPosition(x + inset, y + previewTop);
      preview->setSize(textWidth, previewHeight);
      preview->setClipChildren(true);
      rectangle(*preview, 0, 0, textWidth, previewHeight, background, Style::surfaceRadius);
      avatar(
          *preview, Style::spaceMd, Style::spaceMd, layout::Avatar::small, message->avatar, message->author,
          message->authorId
      );
      label(
          *preview, previewText, Style::spaceMd, textWidth - previewText - Style::spaceMd, author,
          Style::fontSizeCaption, foreground, FontWeight::SemiBold
      );
      auto content = std::make_unique<Node>();
      content->setPosition(bodyLeft, bodyTop);
      content->setSize(bodyWidth, bodyHeight);
      content->setClipChildren(true);
      auto& out = *content;
      for (const auto& box : body.boxes) {
        if (box.y >= bodyHeight)
          continue;
        if (box.divider)
          rectangle(out, box.x, box.y, box.width, box.height, raised);
        else if (!box.link.empty())
          rectangle(out, box.x, box.y, box.width, box.height, box.spoiler ? raised : panel, Style::controlRadius);
        else if (box.inlineCode) {
          rectangle(out, box.x, box.y, box.width, box.height, withAlpha(muted, .2F), Style::radiusSm);
          rectangle(
              out, box.x + Style::borderWidth, box.y + Style::borderWidth, box.width - Style::spaceXxs,
              box.height - Style::spaceXxs, panel, Style::radiusXs
          );
        } else {
          rectangle(
              out, box.x, box.y, box.width, box.height,
              box.quote       ? muted
                  : box.embed ? sidebar
                              : panel,
              box.quote ? 0 : Style::radiusMd
          );
          if (box.embed && box.embedAccent)
            rectangle(out, box.x, box.y, Style::spaceXxs + Style::borderWidth, box.height, rgbHex(box.color), 1);
        }
      }
      for (const auto& area : messageSpoilerAreas(body))
        if (area.y < bodyHeight)
          rectangle(out, area.x, area.y, area.width, area.height, raised, Style::radiusSm);
      for (const auto& run : body.runs) {
        if (run.y >= bodyHeight || run.style.spoiler)
          continue;
        if (!run.image.empty()) {
          if (!emojiImage(out, run.x, run.y, run.width, run.image))
            label(out, run.x, run.y, run.width + Style::spaceXxs, "…", run.style.size, muted);
          continue;
        }
        label(
            out, run.x, run.y, std::max(1.0F, run.width + Style::spaceXxs), messageMarkup(run.text, run.style, false),
            run.style.size,
            run.style.historical || run.style.muted    ? muted
                : run.style.syntax != CodeToken::Plain ? syntaxColor(run.style.syntax)
                : (run.style.link.empty() || run.style.neutralLink) && !run.style.mention ? foreground
                                                                                          : accent,
            FontWeight::Normal, 1, true
        );
      }
      drawMessageStrikes(out, body, 0, 0, foreground, false, {}, bodyHeight);
      if (body.edited && body.edited->y < bodyHeight)
        label(
            out, body.edited->x, body.edited->y, body.edited->width + Style::spaceXxs, "(edited)", body.edited->size,
            muted
        );
      if (m_preferences.showImages)
        for (const auto& icon : body.embedIcons)
          if (icon.frame.y < bodyHeight)
            image(out, icon.frame.x, icon.frame.y, icon.frame.width, icon.url, icon.frame.width / 2);
      for (const auto& pic : body.pictures) {
        if (pic.y >= bodyHeight)
          continue;
        if (pic.spoiler) {
          rectangle(out, pic.x, pic.y, pic.width, pic.height, raised, Style::radiusMd);
          label(
              out, pic.x + Style::spaceMd, pic.y + Style::spaceMd, pic.width - Style::spaceXl, "Spoiler",
              Style::fontSizeDescription, muted
          );
          continue;
        }
        if (pic.sticker) {
          picture(out, pic.x, pic.y, pic.width, pic.height, pic.media.url, 0, false, false, ImageFitMode::Contain);
          continue;
        }
        const bool video = pic.video || !pic.videoSource.empty();
        if (video)
          rectangle(out, pic.x, pic.y, pic.width, pic.height, panel, Style::radiusMd);
        picture(
            out, pic.x, pic.y, pic.width, pic.height, pic.media.url, Style::radiusMd, true, !pic.videoSource.empty()
        );
        if (video) {
          const float badge = Style::controlHeightSm;
          rectangle(
              out, pic.x + (pic.width - badge) / 2, pic.y + (pic.height - badge) / 2, badge, badge, panel, badge / 2
          );
          glyph(
              out, pic.x + (pic.width - Style::glyphSizeMedium) / 2, pic.y + (pic.height - Style::glyphSizeMedium) / 2,
              Style::glyphSizeMedium, "player-play", foreground
          );
        }
      }
      for (const auto& card : body.invites)
        if (card.y < bodyHeight)
          label(out, card.x, card.y, card.width, "Server invite: " + card.code, Style::fontSizeDescription, muted);
      preview->addChild(std::move(content));
      root.addChild(std::move(preview));
    }
    if (!m_dialogError.empty())
      label(
          root, x + inset, y + height - footerHeight - errorHeight, textWidth, m_dialogError,
          Style::fontSizeDescription, red, FontWeight::Normal, 2
      );
    rectangle(root, x, y + height - footerHeight, width, footerHeight, panel, Style::radiusXl);
    rectangle(root, x, y + height - footerHeight, width, Style::radiusXl, panel);
    const float buttonTop = y + height - footerHeight + Style::spaceLg;
    const auto primaryFill = pin || unpin ? accent : red;
    const auto primaryInk = pin || unpin ? ink : rgbHex(0xFFFFFF);
    const auto actionButton = [&](float bx, float bw, const std::string& caption, bool primary, bool enabled) {
      Hit hit{
          bx,
          buttonTop,
          bw,
          Style::controlHeightSm,
          enabled ? (primary ? Action::SubmitDialog : Action::CloseDialog) : Action::None,
          {}
      };
      const bool over = enabled && hit.contains(m_pointerX, m_pointerY);
      const auto fill = primary ? primaryFill : over ? raised : panel;
      rectangle(
          root, hit.x, hit.y, bw, hit.height,
          enabled ? (over && primary ? lerpColor(fill, foreground, .12F) : fill) : lerpColor(panel, fill, .4F),
          Style::radiusMd
      );
      const auto metrics = renderer.measureText(caption, Style::fontSizeCaption, FontWeight::Medium);
      label(
          root, hit.x + (bw - metrics.width) / 2, hit.y + (hit.height - (metrics.bottom - metrics.top)) / 2,
          metrics.width + Style::spaceXxs, caption, Style::fontSizeCaption,
          !enabled       ? muted
              : !primary ? foreground
                         : primaryInk,
          FontWeight::Medium
      );
      m_hits.push_back(hit);
    };
    const auto buttonWidth = [&](const std::string& caption) {
      return renderer.measureText(caption, Style::fontSizeCaption, FontWeight::Medium).width + Style::spaceXl * 2;
    };
    const float confirmWidth = std::max(
        buttonWidth(confirm),
        buttonWidth(
            pin ? "Oh yeah. Pin it"
                : unpin ? "Remove it please"
                        : "Delete"
        )
    );
    const float cancelWidth = buttonWidth("Cancel");
    const float confirmX = x + width - inset - confirmWidth;
    actionButton(confirmX - Style::spaceSm - cancelWidth, cancelWidth, "Cancel", false, !m_dialogPending);
    actionButton(confirmX, confirmWidth, confirm, true, !m_dialogPending);
    m_tooltip.clear();
  }
} // namespace app
