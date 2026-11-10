#include "app/client_application.h"
#include "app/theme.h"
#include "render/core/image_encoder.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/text_node.h"

#include <GLES3/gl3.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    constexpr std::size_t maximumPendingMessages = 32;
    constexpr std::size_t maximumPendingUploadBytes = 64 * 1024 * 1024;
    constexpr std::size_t maximumPendingTextureBytes = 64 * 1024 * 1024;
    std::string outgoingNonce() {
      static std::uint64_t last = 0;
      const auto now =
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();
      last = std::max(last + 1, static_cast<std::uint64_t>(now));
      return std::to_string(last);
    }
    std::string outgoingTimestamp() {
      const auto now = std::chrono::system_clock::now();
      const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
      const auto time = std::chrono::system_clock::to_time_t(seconds);
      std::tm utc{};
      gmtime_r(&time, &utc);
      char timestamp[32]{};
      std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc);
      const auto fraction =
          std::to_string(1000 + std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count());
      return std::string(timestamp) + "." + fraction.substr(1) + "Z";
    }
  } // namespace

  bool ClientApplication::queueOutgoing(
      std::string content, std::vector<api::Upload> uploads, std::vector<std::string> stickers,
      std::vector<api::Sticker> previews, bool consumeDraft, std::optional<api::Gif> gif,
      std::optional<api::PollDraft> poll
  ) {
    const bool voice = uploads.size() == 1 && uploads.front().voice.has_value();
    if (!canSendToThread())
      return false;
    if ((!m_connected && !m_demo)
        || m_channelId.empty()
        || (m_demo
            && !m_demoOverlay.starts_with("outgoing-cycle")
            && !m_demoOverlay.starts_with("polls-cycle")
            && !m_demoOverlay.starts_with("voice-messages-cycle")))
      return false;
    if (voice && !canRecordVoiceMessage()) {
      m_status = "You can no longer send voice messages in this channel.";
      return false;
    }
    if (!voice && !uploads.empty() && !messageActionAllowed(can::attachFiles)) {
      m_status = "You do not have permission to attach files in this channel.";
      return false;
    }
    if (poll && (!canCreatePoll() || !api::pollDraftError(*poll).empty()))
      return false;
    std::size_t retained = 0, retainedTextures = 0, submittedTextures = 0;
    std::size_t slots = m_pendingSends.size();
    for (const auto& [_, pending] : m_pendingSends) {
      slots += !pending.forwardComment.empty();
      if (pending.voice)
        retained += pending.voice->bytes.size();
      for (const auto& image : pending.images) {
        retained += image.upload.bytes.size();
        retainedTextures += image.texture.allocationBytes;
      }
    }
    if (consumeDraft)
      for (const auto& image : m_attachments[m_channelId])
        submittedTextures += image.texture.allocationBytes;
    for (const auto& upload : uploads)
      retained += upload.bytes.size();
    if (slots >= maximumPendingMessages
        || retained > maximumPendingUploadBytes
        || retainedTextures + submittedTextures > std::max(maximumPendingTextureBytes, submittedTextures)) {
      m_status = "Finish or restore pending messages before sending more.";
      return false;
    }
    const auto nonce = outgoingNonce();
    const auto reply =
        !poll && !voice && m_replies.contains(m_channelId) ? std::optional(m_replies.at(m_channelId)) : std::nullopt;
    const auto revoked = std::make_shared<std::atomic_bool>(false);
    if (!m_demo && !m_client.send(m_channelId, content, uploads, reply, stickers, nonce, revoked, poll)) {
      m_status = "Could not queue this message. Check its length and attachments.";
      return false;
    }
    PendingSend pending;
    pending.guild = m_dmMode ? "" : m_guildId;
    pending.revoked = revoked;
    pending.poll = poll;
    if (voice)
      pending.voice = uploads.front();
    pending.composer = consumeDraft;
    pending.preview = {
        .id = "pending:" + nonce,
        .channelId = m_channelId,
        .author = m_account.name,
        .content = std::move(content),
        .timestamp = outgoingTimestamp(),
        .authorId = m_account.id,
        .avatar = m_account.avatar
    };
    if (poll) {
      api::Poll preview{.question = poll->question, .multiple = poll->multiple, .resultsKnown = true};
      for (std::size_t i = 0; i < poll->answers.size(); ++i)
        preview.answers.push_back({.id = static_cast<int>(i + 1), .text = poll->answers[i]});
      pending.preview.poll = std::move(preview);
    }
    if (voice) {
      pending.preview.flags |= api::messageVoice;
      pending.preview.attachments.push_back(
          {.filename = uploads.front().filename, .contentType = uploads.front().mime, .voice = uploads.front().voice}
      );
    }
    pending.preview.nonce = nonce;
    pending.preview.authorTag = m_account.guildTag;
    pending.preview.reply = reply;
    if (reply)
      pending.preview.type = api::replyMessageType;
    pending.preview.mentions = m_draftMentionNames[m_channelId];
    pending.stickers = std::move(previews);
    for (const auto& sticker : pending.stickers)
      if (std::ranges::find(stickers, sticker.id) != stickers.end())
        pending.preview.stickers.push_back(sticker);
    if (gif)
      pending.preview.embeds.push_back({.image = {gif->src, gif->width, gif->height}});
    if (consumeDraft) {
      pending.draft = std::move(m_drafts[m_channelId]);
      m_drafts[m_channelId].clear();
      pending.mentions = std::move(m_draftMentionNames[m_channelId]);
      m_draftMentionNames.erase(m_channelId);
      pending.images = std::move(m_attachments[m_channelId]);
      m_attachments.erase(m_channelId);
      m_draftStickers.erase(m_channelId);
      for (std::size_t i = 0; i < pending.images.size(); ++i) {
        const auto& image = pending.images[i];
        const auto path = "pending-attachment:" + nonce + ":" + image.key;
        pending.preview.attachments.push_back(
            {image.key, uploads[i].filename, {}, image.upload.mime, {path, image.texture.width, image.texture.height}}
        );
      }
      m_edit = {};
      if (const auto input = m_inputs.find("composer"); input != m_inputs.end())
        input->second->setValue("");
      closeCompletion();
    }
    if (!poll && !voice)
      m_replies.erase(m_channelId);
    m_pendingSends.emplace(nonce, std::move(pending));
    m_sending = true;
    m_status.clear();
    m_scrolling.stop(m_messageScroll);
    m_messageScroll = 0;
    m_timelineAnchor.reset();
    if (!m_history.atLatest()) {
      m_history.reset();
      m_adoptedChatPreload.reset();
      m_messages.clear();
      m_messageLayouts.clear();
      if (!m_demo)
        loadHistory(api::HistoryDirection::Latest);
    }
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::queueForward(
      const ForwardDestination& destination, const api::ForwardReference& source, const api::ForwardedMessage& snapshot,
      std::string comment
  ) {
    if (!m_connected
        || (m_demo && !m_demoOverlay.starts_with("forward-cycle"))
        || !forwardSourceAllowed(source)
        || !forwardDestinationAllowed(destination)
        || comment.size() > api::maximumForwardCommentBytes
        || api::utf8Length(comment) > api::maximumForwardCommentCharacters)
      return false;
    std::size_t slots = m_pendingSends.size() + 1 + !comment.empty();
    for (const auto& [_, pending] : m_pendingSends)
      slots += !pending.forwardComment.empty();
    if (slots > maximumPendingMessages) {
      m_status = "Finish or restore pending messages before forwarding more.";
      return false;
    }
    const auto nonce = outgoingNonce();
    const auto revoked = std::make_shared<std::atomic_bool>(false);
    if (!m_demo && !m_client.send(destination.channel, "", {}, {}, {}, nonce, revoked, {}, source)) {
      m_status = "Could not queue this forward. Your choices are retained.";
      return false;
    }
    PendingSend pending;
    pending.guild = destination.guild;
    pending.revoked = revoked;
    pending.forward = source;
    pending.forwardComment = std::move(comment);
    pending.preview = {
        .id = "pending:" + nonce,
        .channelId = destination.channel,
        .author = m_account.name,
        .timestamp = outgoingTimestamp(),
        .authorId = m_account.id,
        .avatar = m_account.avatar
    };
    pending.preview.forwarded = snapshot;
    pending.preview.nonce = nonce;
    pending.preview.authorTag = m_account.guildTag;
    m_pendingSends.emplace(nonce, std::move(pending));
    m_sending = true;
    m_status.clear();
    m_window.invalidate();
    return true;
  }
  void ClientApplication::queueForwardComment(std::string guild, std::string channel, std::string comment) {
    if (comment.empty())
      return;
    const auto nonce = outgoingNonce();
    PendingSend pending;
    pending.guild = std::move(guild);
    pending.revoked = std::make_shared<std::atomic_bool>(false);
    pending.preview = {
        .id = "pending:" + nonce,
        .channelId = std::move(channel),
        .author = m_account.name,
        .content = std::move(comment),
        .timestamp = outgoingTimestamp(),
        .authorId = m_account.id,
        .avatar = m_account.avatar
    };
    pending.preview.nonce = nonce;
    pending.preview.authorTag = m_account.guildTag;
    if (!forwardDestinationAllowed({pending.guild, pending.preview.channelId})
        || (!m_demo
            && !m_client.send(
                pending.preview.channelId, pending.preview.content, {}, {}, {}, nonce, pending.revoked
            ))) {
      pending.revoked->store(true);
      pending.completed = true;
      pending.error =
          "The forward was delivered, but its comment could not be queued. Restore the comment to send it explicitly.";
    }
    m_pendingSends.emplace(nonce, std::move(pending));
    m_sending = std::ranges::any_of(m_pendingSends, [](const auto& entry) {
      return !entry.second.completed && entry.second.confirmed.empty();
    });
  }

  void ClientApplication::releaseOutgoing(PendingSend& pending) {
    if (pending.revoked)
      pending.revoked->store(true);
    for (auto& image : pending.images) {
      if (!image.texture.valid())
        continue;
      if (m_window.context().makeCurrent(m_window.target()))
        m_window.context().textureManager().unload(image.texture);
      else
        m_retiredImages.push_back(image.texture);
    }
    pending.images.clear();
    pending.voice.reset();
    m_messageLayouts.erase(pending.preview.id);
  }

  void ClientApplication::clearOutgoing() {
    for (auto& [_, pending] : m_pendingSends)
      releaseOutgoing(pending);
    m_pendingSends.clear();
    m_sending = false;
  }
  void ClientApplication::revokeUnavailableOutgoing() {
    for (auto& [_, pending] : m_pendingSends) {
      if (pending.completed || !pending.revoked || pending.revoked->load())
        continue;
      if (pending.guild.empty() && !pending.forward)
        continue;
      const auto* channel = serverChannel(pending.guild, pending.preview.channelId);
      if (!channel)
        channel = m_threads.find(pending.guild, pending.preview.channelId);
      const auto permissions = serverChannelPermissions(pending.guild, pending.preview.channelId);
      if ((!pending.forward || forwardSourceAllowed(*pending.forward))
          && (!pending.forward || forwardDestinationAllowed({pending.guild, pending.preview.channelId}))
          && (pending.guild.empty()
              || (channel
                  && api::messageWriteAccess(*channel, permissions) == api::MessageWriteAccess::Allowed
                  && (!pending.poll || (permissions && (*permissions & api::permission::sendPolls)))
                  && (!pending.voice || channelCan(pending.guild, pending.preview.channelId, can::sendVoiceMessage)))))
        continue;
      pending.revoked->store(true);
      pending.completed = true;
      pending.error = "Message delivery could not be confirmed after channel access changed. Check the conversation "
                      "before trying again.";
    }
    m_sending = std::ranges::any_of(m_pendingSends, [](const auto& entry) {
      return !entry.second.completed && entry.second.confirmed.empty();
    });
  }

  void ClientApplication::confirmOutgoing(api::Message& message) {
    const auto found = m_pendingSends.find(message.nonce);
    if (found == m_pendingSends.end()
        || found->second.preview.channelId != message.channelId
        || message.authorId != m_account.id
        || !api::validSnowflake(message.id))
      return;
    auto& pending = found->second;
    if (pending.poll && !message.poll)
      return;
    if (pending.voice && !(message.flags & api::messageVoice))
      return;
    if (pending.forward && !api::confirmsForward(message, *pending.forward))
      return;
    if (message.reply
        && !message.reply->available
        && pending.preview.reply
        && message.reply->message == pending.preview.reply->message) {
      message.reply = pending.preview.reply;
      message.reply->available = true;
    }
    if (pending.confirmed.empty()) {
      pending.confirmed = message.id;
      auto comment = std::exchange(pending.forwardComment, {});
      if (!comment.empty()) {
        if (pending.revoked && pending.revoked->load()) {
          const auto nonce = outgoingNonce();
          PendingSend note;
          note.guild = pending.guild;
          note.completed = true;
          note.revoked = std::make_shared<std::atomic_bool>(true);
          note.error = "The forward was delivered after cancellation. Its comment was kept without sending. Restore it "
                       "to send explicitly.";
          note.preview = {
              .id = "pending:" + nonce,
              .channelId = pending.preview.channelId,
              .author = m_account.name,
              .content = std::move(comment),
              .timestamp = outgoingTimestamp(),
              .authorId = m_account.id,
              .avatar = m_account.avatar
          };
          note.preview.nonce = nonce;
          note.preview.authorTag = m_account.guildTag;
          m_pendingSends.emplace(nonce, std::move(note));
        } else
          queueForwardComment(pending.guild, pending.preview.channelId, std::move(comment));
      }
      if (!pending.error.empty()
          && m_status == "Message delivery was not confirmed. Restore the pending message to edit it.")
        m_status.clear();
      releaseOutgoing(pending);
      if (!m_demo) {
        for (const auto& sticker : pending.stickers)
          m_sessionStore.recordSticker(m_account.id, sticker.id);
        if (!pending.stickers.empty()) {
          m_recentStickers = m_sessionStore.frequentStickers(m_account.id);
          ++m_stickerRevision;
        }
      }
      pending.stickers.clear();
      pending.draft.clear();
      pending.mentions.clear();
    }
    if (pending.completed)
      m_pendingSends.erase(found);
    m_sending = std::ranges::any_of(m_pendingSends, [](const auto& entry) {
      return !entry.second.completed && entry.second.confirmed.empty();
    });
  }

  void ClientApplication::outgoingResult(api::Event& event) {
    const auto found = m_pendingSends.find(event.nonce);
    if (found == m_pendingSends.end() || found->second.preview.channelId != event.resource)
      return;
    auto& pending = found->second;
    std::erase_if(event.messages, [&](const auto& message) {
      return message.channelId != event.resource
          || message.authorId != m_account.id
          || !api::validSnowflake(message.id)
          || (pending.poll && !message.poll)
          || (pending.voice && !(message.flags & api::messageVoice))
          || (pending.forward && !api::confirmsForward(message, *pending.forward))
          || (!message.nonce.empty() && message.nonce != event.nonce);
    });
    pending.completed = true;
    // A live confirmation takes precedence over a later transport error.
    if (!pending.confirmed.empty()) {
      m_pendingSends.erase(found);
    } else if (!event.error.empty() || event.messages.empty()) {
      if (!pending.revoked || !pending.revoked->load())
        pending.error = event.error.empty() ? "No delivery confirmation received." : event.error;
      pending.securityChallenge = event.securityChallenge;
      m_status = "Message delivery was not confirmed. Restore the pending message to edit it.";
    }
    for (auto& message : event.messages) {
      message.nonce = event.nonce;
      confirmOutgoing(message);
      if (message.channelId == m_channelId) {
        m_liveChanges[message.id] = message;
        if (m_history.atLatest())
          upsertMessage(message);
      } else if (auto cached = m_channelMessages.find(message.channelId); cached != m_channelMessages.end()) {
        std::erase_if(cached->second, [&](const auto& old) { return old.id == message.id; });
        cached->second.push_back(message);
        if (cached->second.size() > 200) {
          cached->second.erase(cached->second.begin());
          m_channelHistoryState[message.channelId].olderEnd = false;
        }
      }
    }
    m_sending = std::ranges::any_of(m_pendingSends, [](const auto& entry) {
      return !entry.second.completed && entry.second.confirmed.empty();
    });
    m_window.invalidate();
  }

  void ClientApplication::restoreOutgoing(const std::string& nonce) {
    const auto found = m_pendingSends.find(nonce);
    if (found == m_pendingSends.end()
        || !found->second.completed
        || !found->second.confirmed.empty()
        || found->second.preview.channelId != m_channelId)
      return;
    if (found->second.voice) {
      if (!m_voiceMessageChannel.empty() || !m_dialogKind.empty() || !canRecordVoiceMessage()) {
        m_status = "Discard the current recording or restore channel access before restoring this recording.";
        return;
      }
      openVoiceMessage(false);
      m_voiceMessageDraft = std::move(found->second.voice);
      releaseOutgoing(found->second);
      m_pendingSends.erase(found);
      return;
    }
    if (found->second.forward) {
      if (!m_dialogKind.empty()) {
        m_status = "Close the current dialog before restoring this forward.";
        return;
      }
      const auto& pending = found->second;
      ForwardDestination destination{pending.guild, pending.preview.channelId};
      openForwardEditor(*pending.forward, *pending.preview.forwarded, pending.forwardComment, destination);
      releaseOutgoing(found->second);
      m_pendingSends.erase(found);
      return;
    }
    if (found->second.poll) {
      if (m_pollEditor || !canCreatePoll()) {
        m_status = "Close the current poll or restore channel access before restoring this poll.";
        return;
      }
      auto draft = *found->second.poll;
      openPoll();
      if (m_pollEditor) {
        m_pollEditor->draft = std::move(draft);
        releaseOutgoing(found->second);
        m_pendingSends.erase(found);
      }
      return;
    }
    if (!m_drafts[m_channelId].empty()
        || !m_attachments[m_channelId].empty()
        || !m_draftStickers[m_channelId].empty()
        || m_replies.contains(m_channelId)) {
      m_status = "Clear the current draft before restoring this message.";
      return;
    }
    auto& pending = found->second;
    // Restore exact composer drafts and sticker choices; GIF picker URLs become editable text.
    m_drafts[m_channelId] = pending.composer || !pending.stickers.empty() ? pending.draft : pending.preview.content;
    m_draftMentionNames[m_channelId] = std::move(pending.mentions);
    m_attachments[m_channelId] = std::move(pending.images);
    m_draftStickers[m_channelId] = std::move(pending.stickers);
    if (pending.preview.reply)
      m_replies[m_channelId] = *pending.preview.reply;
    m_messageLayouts.erase(pending.preview.id);
    m_pendingSends.erase(found);
    m_status.clear();
    m_edit.clamp(m_drafts[m_channelId]);
    focusControl("composer");
    m_window.invalidate();
  }
  void ClientApplication::messageSecurityAction(const std::string& action) {
    if (action == "review") {
      const auto channel = m_channelId;
      if (!api::validSnowflake(channel))
        return;
      closeContext();
      closeDialog();
      closeCompletion();
      m_scrolling.stopAll();
      m_dialogKind = "message-security";
      m_dialogText = channel;
      m_fieldFocused = false;
    } else if (m_dialogKind == "message-security" && action == "open") {
      const auto guild = m_unreadGuilds.find(m_dialogText);
      const auto destination = guild == m_unreadGuilds.end() || guild->second.empty() ? "@me" : guild->second;
      if (!m_demo && api::validSnowflake(m_dialogText))
        auth::openExternalUrl("https://discord.com/channels/" + destination + "/" + m_dialogText);
    } else if (m_dialogKind == "message-security" && action == "resume") {
      const auto channel = m_dialogText;
      if (!m_demo && !m_client.resumeMessageChallenge(channel)) {
        m_status = "There is no local security-check pause for this conversation.";
      } else {
        for (auto& [_, pending] : m_pendingSends)
          if (pending.preview.channelId == channel && pending.securityChallenge) {
            pending.securityChallenge = false;
            pending.error = "Local security-check pause cleared. Check the conversation before manually resending.";
          }
        m_status = "Local pause cleared. Restore a failed message to try sending it manually.";
      }
      closeDialog();
    }
    m_window.invalidate();
  }
  void ClientApplication::drawMessageSecurity(Node& root, float w, float h) {
    const auto frame = layout::messageSecurityLayout(w, h);
    rectangle(
        root, 0, layout::Window::titlebarHeight, w, h - layout::Window::titlebarHeight,
        Color{0, 0, 0, Style::modalBackdropOpacity}
    );
    m_hits.push_back({0, layout::Window::titlebarHeight, w, h - layout::Window::titlebarHeight, Action::None, {}});
    m_dialogBounds = {frame.dialog.x, frame.dialog.y, frame.dialog.width, frame.dialog.height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    rectangle(
        root, frame.dialog.x, frame.dialog.y, frame.dialog.width, frame.dialog.height, theme::sidebar, Style::radiusXl
    );
    label(
        root, frame.title.x, frame.title.y, frame.title.width, "Security check required", Style::fontSizeDialog,
        theme::foreground, FontWeight::SemiBold
    );
    iconButton(
        root, {frame.close.x, frame.close.y, frame.close.width, frame.close.height, Action::CloseDialog, {}}, "x",
        "Close"
    );
    const std::string text =
        "The service requested a CAPTCHA. Cressida cannot display it here. Open the official website to complete "
        "any check it offers. If no check appears, complete the action in that client.\n\n"
        "After completing the check, Resume clears only this conversation's local pause. It does not remove "
        "account restrictions or resend failed messages.";
    const auto measured = m_window.target().renderer().measureText(
        text, Style::fontSizeBody, FontWeight::Normal, frame.description.width, 0
    );
    m_scrolling.limit(m_profileScroll, std::max(0.0F, measured.bottom - measured.top - frame.description.height));
    auto description = std::make_unique<Node>();
    description->setPosition(frame.description.x, frame.description.y);
    description->setSize(frame.description.width, frame.description.height);
    description->setClipChildren(true);
    label(
        *description, 0, -m_profileScroll, frame.description.width, text, Style::fontSizeBody, theme::foreground,
        FontWeight::Normal, 0
    );
    root.addChild(std::move(description));
    button(
        root, {frame.open.x, frame.open.y, frame.open.width, frame.open.height, Action::MessageSecurity, "open"},
        "Open official website", false, false, true, true
    );
    button(
        root,
        {frame.resume.x, frame.resume.y, frame.resume.width, frame.resume.height, Action::MessageSecurity, "resume"},
        "Resume sending", true, false, true, true
    );
    m_tooltip.clear();
  }
} // namespace app

namespace app {
  void ClientApplication::demoOutgoingStep() {
    const auto require = [](bool condition, const char* message) {
      if (!condition)
        throw std::runtime_error(message);
    };
    const auto stageImage = [&] {
      constexpr int width = 64, height = 32;
      const std::vector<std::uint8_t> pixels(width * height * 4, 255);
      auto texture = m_window.context().textureManager().loadFromRgba(pixels.data(), width, height);
      require(texture.valid(), "Outgoing replay requires a real local image texture");
      m_demoOutgoingTexture = texture.id.value();
      m_attachments[m_channelId].push_back(
          {{"local.png", "image/png", encodePng(pixels.data(), width, height)},
           texture,
           std::to_string(++m_attachmentSerial)}
      );
    };
    const auto lastNonce = [&] { return m_pendingSends.rbegin()->first; };
    const auto confirmed = [&](const std::string& nonce, const std::string& id) {
      auto message = m_pendingSends.at(nonce).preview;
      message.id = id;
      message.attachments.clear();
      return message;
    };
    const auto sceneContains = [&](std::string_view text, bool dimmed) {
      const auto inspect = [&](auto&& self, const Node& node, bool dim) -> bool {
        dim = dim || node.opacity() == Style::mutedChannelOpacity;
        if (node.type() == NodeType::Text
            && static_cast<const TextNode&>(node).text().find(text) != std::string::npos
            && dim == dimmed)
          return true;
        for (const auto& child : node.children())
          if (self(self, *child, dim))
            return true;
        return false;
      };
      return inspect(inspect, *m_scene, false);
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      m_drafts[m_channelId] = "Optimistic hello";
      m_draftMentionNames[m_channelId] = {{"7", "Friend"}};
      m_replies[m_channelId] = {
          .message = "11",
          .channel = m_channelId,
          .authorId = "7",
          .author = "Friend",
          .content = "Original reply",
          .available = true
      };
      stageImage();
      m_inputs.at("composer")->setValue(m_drafts[m_channelId]);
      focusControl("composer");
      onKey({XKB_KEY_Return});
      require(m_inputs.at("composer")->value().empty(), "Submitting clears the native input before another key event");
      onKey({XKB_KEY_a, "a"});
      require(m_drafts[m_channelId] == "a", "Typing before the next frame cannot resurrect the submitted text");
      onKey({XKB_KEY_z, {}, true});
      require(
          m_pendingSends.size() == 1
              && m_drafts[m_channelId].empty()
              && m_attachments[m_channelId].empty()
              && !m_replies.contains(m_channelId)
              && m_messages.empty(),
          "Submission clears only the composer and keeps server history clean"
      );
      m_demoOutgoingNonces.push_back(lastNonce());
      require(
          m_pendingSends.begin()->second.preview.reply->content == "Original reply"
              && m_pendingSends.begin()->second.preview.mentions.at("7") == "Friend",
          "Pending replies and mention names use the submitted snapshot"
      );
    } else if (step == 1) {
      require(sceneContains("Optimistic", true), "A pending message is rendered dimmed before confirmation");
      const auto inspect = [&](auto&& self, const Node& node, bool dim) -> bool {
        dim = dim || node.opacity() == Style::mutedChannelOpacity;
        if (node.type() == NodeType::Image
            && static_cast<const ImageNode&>(node).textureId() == m_demoOutgoingTexture
            && dim)
          return true;
        for (const auto& child : node.children())
          if (self(self, *child, dim))
            return true;
        return false;
      };
      require(inspect(inspect, *m_scene, false), "Pending upload previews borrow the local texture immediately");
      focusControl("composer");
      onKey({XKB_KEY_a, "a"});
      require(m_drafts[m_channelId] == "a", "The native composer accepts typing during an upload");
      onKey({XKB_KEY_z, {}, true});
      require(m_drafts[m_channelId].empty(), "Native composer undo works while a send is pending");
      m_drafts[m_channelId] = "Optimistic hello";
      send();
      require(m_pendingSends.size() == 2, "A second identical message can queue while the first is sending");
      m_demoOutgoingNonces.push_back(lastNonce());
      require(m_demoOutgoingNonces[0] != m_demoOutgoingNonces[1], "Identical sends have independent request IDs");
      auto message = confirmed(m_demoOutgoingNonces[0], "90001");
      message.reply->available = false;
      message.author = "Server nickname";
      message.avatar = "guild-avatar";
      confirmOutgoing(message);
      require(message.reply->available, "Live confirmation preserves the reply preview snapshot");
      upsertMessage(message);
      require(glIsTexture(m_demoOutgoingTexture) == GL_FALSE, "First confirmation releases the upload texture");
      api::Event lateError{.operation = api::Operation::Send, .resource = m_channelId};
      lateError.nonce = m_demoOutgoingNonces[0];
      lateError.error = "Late transport failure";
      outgoingResult(lateError);
      require(
          !m_pendingSends.contains(lateError.nonce) && m_status != lateError.error,
          "A later HTTP error cannot undo live confirmation"
      );
    } else if (step == 2) {
      require(
          sceneContains("Optimistic", false) && sceneContains("Optimistic", true),
          "Confirmed and pending messages independently use normal and dimmed colors"
      );
      const Node* pendingRow = nullptr;
      const auto findRow = [&](auto&& self, const Node& node, const Node* row) -> void {
        if (node.opacity() == Style::mutedChannelOpacity)
          row = &node;
        if (row && node.type() == NodeType::Text && static_cast<const TextNode&>(node).text().starts_with("Optimistic"))
          pendingRow = row;
        for (const auto& child : node.children())
          self(self, *child, row);
      };
      findRow(findRow, *m_scene, nullptr);
      require(pendingRow, "The grouped pending row is present in the rendered scene");
      const auto hasHeader = [&](auto&& self, const Node& node) -> bool {
        if (node.type() == NodeType::Text && static_cast<const TextNode&>(node).text() == m_account.name)
          return true;
        if (node.width() == layout::Avatar::chat && node.height() == layout::Avatar::chat)
          return true;
        return std::ranges::any_of(node.children(), [&](const auto& child) { return self(self, *child); });
      };
      require(
          !hasHeader(hasHeader, *pendingRow),
          "A pending follow-up has no extra avatar or header after a server profile message"
      );
      api::Event success{.operation = api::Operation::Send, .resource = m_channelId};
      success.nonce = m_demoOutgoingNonces[1];
      success.messages = {confirmed(success.nonce, "90002")};
      outgoingResult(success);
      require(m_pendingSends.empty() && m_messages.size() == 2, "HTTP-first delivery replaces its pending row once");
      stageImage();
      m_drafts[m_channelId] = "Keep my failed upload";
      send();
      m_demoOutgoingNonces.push_back(lastNonce());
    } else if (step == 3) {
      api::Event error{.operation = api::Operation::Send, .resource = m_channelId};
      error.nonce = m_demoOutgoingNonces[2];
      error.error = "Synthetic upload timeout";
      outgoingResult(error);
      require(
          m_pendingSends.at(error.nonce).images.size() == 1 && glIsTexture(m_demoOutgoingTexture),
          "Uncertain delivery keeps the upload available without retrying"
      );
      m_drafts[m_channelId] = "A newer draft";
    } else if (step == 4) {
      require(sceneContains("Not confirmed", false), "Failed delivery has a visible status outside dimmed content");
      const auto restore = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == Action::RestoreOutgoing && hit.id == m_demoOutgoingNonces[2];
      });
      require(restore != m_hits.end(), "Failed messages expose an explicit restore action");
      activate(*restore);
      require(
          m_drafts[m_channelId] == "A newer draft" && m_pendingSends.size() == 1,
          "Restore cannot overwrite a newer draft"
      );
    } else if (step == 5) {
      m_drafts[m_channelId].clear();
      restoreOutgoing(m_demoOutgoingNonces[2]);
      require(
          m_pendingSends.empty()
              && m_drafts[m_channelId] == "Keep my failed upload"
              && m_attachments[m_channelId].size() == 1
              && glIsTexture(m_demoOutgoingTexture),
          "Explicit restore transfers the failed draft and texture back to the composer"
      );
    } else if (step == 6) {
      send();
      const auto nonce = lastNonce();
      api::Event success{.operation = api::Operation::Send, .resource = m_channelId};
      success.nonce = nonce;
      success.messages = {confirmed(nonce, "90003")};
      outgoingResult(success);
      auto echoed = success.messages.front();
      confirmOutgoing(echoed);
      upsertMessage(echoed);
      require(
          m_messages.size() == 3 && m_pendingSends.empty() && glIsTexture(m_demoOutgoingTexture) == GL_FALSE,
          "HTTP-first then live echo deduplicates and releases restored upload resources"
      );
      m_drafts[m_channelId] = "Keep typing";
      require(
          queueOutgoing("Navigation pending", {}, {}, {}, false), "Standalone sends queue without consuming drafts"
      );
      m_demoOutgoingNonces.push_back(lastNonce());
      require(m_drafts[m_channelId] == "Keep typing", "A standalone picker send preserves the current draft");
      m_channelId = "99";
    } else if (step == 7) {
      require(!sceneContains("Navigation", true), "Pending messages do not leak into other channels");
      auto impostor = confirmed(m_demoOutgoingNonces[3], "90004");
      impostor.authorId = "999";
      confirmOutgoing(impostor);
      require(m_pendingSends.at(impostor.nonce).confirmed.empty(), "Another author cannot confirm a pending send");
      impostor.authorId = m_account.id;
      impostor.channelId = "99";
      confirmOutgoing(impostor);
      require(m_pendingSends.at(impostor.nonce).confirmed.empty(), "Another channel cannot confirm a pending send");
      m_channelId = m_pendingSends.at(impostor.nonce).preview.channelId;
    } else if (step == 8) {
      require(sceneContains("Navigation", true), "Pending messages reappear when returning to their channel");
      api::Event error{.operation = api::Operation::Send, .resource = m_channelId};
      error.nonce = m_demoOutgoingNonces[3];
      error.error = "Synthetic uncertain response";
      outgoingResult(error);
      auto delayed = confirmed(error.nonce, "90004");
      confirmOutgoing(delayed);
      upsertMessage(delayed);
      require(
          m_pendingSends.empty() && m_messages.size() == 4,
          "A delayed live confirmation resolves an earlier uncertain failure"
      );
    } else if (step == 9) {
      m_drafts[m_channelId].clear();
      for (std::size_t i = 0; i < maximumPendingMessages; ++i)
        require(queueOutgoing("Queued message"), "The bounded queue accepts its documented capacity");
      m_drafts[m_channelId] = "Keep overflow draft";
      require(
          !queueOutgoing("Overflow")
              && m_drafts[m_channelId] == "Keep overflow draft"
              && m_pendingSends.size() == maximumPendingMessages,
          "Queue capacity rejection preserves the current draft"
      );
      clearOutgoing();
    } else if (step == 10) {
      stageImage();
      m_drafts[m_channelId] = "Release on reset";
      send();
      clearOutgoing();
      require(
          m_pendingSends.empty() && glIsTexture(m_demoOutgoingTexture) == GL_FALSE,
          "Account and shutdown cleanup release pending upload ownership"
      );
      m_status.clear();
    } else if (step == 11) {
      require(!m_sending && m_pendingSends.empty(), "Outgoing replay leaves no pending resources");
      for (const auto& message : m_messages)
        require(api::validSnowflake(message.id), "Confirmed history never contains a local pending ID");
      require(
          queueOutgoing("Security-check fixture", {}, {}, {}, false),
          "A synthetic challenged send enters the normal pipeline"
      );
      m_demoOutgoingNonces.push_back(lastNonce());
    } else if (step == 12) {
      api::Event error{.operation = api::Operation::Send, .resource = m_channelId};
      error.nonce = m_demoOutgoingNonces.back();
      error.error = "Synthetic security check";
      error.securityChallenge = true;
      outgoingResult(error);
    } else if (step == 13) {
      const auto hit =
          std::ranges::find_if(m_hits, [](const auto& value) { return value.action == Action::MessageSecurity; });
      require(hit != m_hits.end(), "A challenged send exposes visible recovery instead of a tooltip-only error");
      activate(*hit);
      require(m_dialogKind == "message-security", "Review opens the bounded security-check explanation");
    } else if (step == 14) {
      require(sceneContains("CAPTCHA", false), "The recovery explains that this client cannot display the challenge");
      activate({0, 0, 0, 0, Action::MessageSecurity, "resume"});
      require(
          m_pendingSends.size() == 1
              && !m_pendingSends.begin()->second.securityChallenge
              && m_pendingSends.begin()->second.completed
              && !m_sending,
          "Explicit recovery never resubmits a failed message"
      );
    } else if (step == 15) {
      require(
          std::ranges::any_of(m_hits, [](const auto& hit) { return hit.action == Action::RestoreOutgoing; }),
          "After recovery the user can restore the retained message manually"
      );
      clearOutgoing();
      std::clog << "Offline outgoing messages, confirmation ordering, recovery and cleanup passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
