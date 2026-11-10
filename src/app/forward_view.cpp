#include "app/client_application.h"
#include "app/reply_preview.h"
#include "app/theme.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/scrollbar.h"

#include <algorithm>
#include <glib.h>

namespace app {
  namespace {
    constexpr std::size_t maximumForwardCandidates = 4096;
    std::string folded(std::string_view value) {
      auto* text = g_utf8_casefold(value.data(), static_cast<gssize>(value.size()));
      std::string result = text ? text : "";
      g_free(text);
      return result;
    }
  } // namespace
  bool ClientApplication::forwardSourceAllowed(const api::ForwardReference& source) const {
    if (!api::validForwardReference(source) || m_messageLog.known(source.channel, source.message))
      return false;
    if (source.channel == m_channelId)
      if (const auto live = m_liveChanges.find(source.message); live != m_liveChanges.end() && !live->second)
        return false;
    const auto allowed = source.guild.empty()
        ? std::ranges::any_of(m_privateChannels, [&](const auto& channel) { return channel.id == source.channel; })
        : channelCan(source.guild, source.channel, can::readHistory);
    if (!allowed)
      return false;
    const auto check = [&](const auto& messages) {
      const auto found = std::ranges::find(messages, source.message, &api::Message::id);
      return found == messages.end() || (api::canForwardMessage(*found) && !blockedMessage(*found));
    };
    if (source.channel == m_channelId && !check(m_messages))
      return false;
    const auto cached = m_channelMessages.find(source.channel);
    return cached == m_channelMessages.end() || check(cached->second);
  }
  bool ClientApplication::forwardDestinationAllowed(const ForwardDestination& destination) const {
    if (!api::validSnowflake(destination.channel))
      return false;
    if (destination.guild.empty()) {
      const auto found = std::ranges::find(m_privateChannels, destination.channel, &api::Channel::id);
      return found != m_privateChannels.end()
          && !found->isMessageRequest()
          && (found->type == 1 || found->type == 3)
          && std::ranges::none_of(found->recipients, [&](const auto& user) {
               return m_userDirectory.relationship(user.id) == 2 || m_userDirectory.ignored(user.id);
             });
    }
    const auto* channel = serverChannel(destination.guild, destination.channel);
    if (!channel)
      channel = m_threads.find(destination.guild, destination.channel);
    return channel
        && api::messageWriteAccess(*channel, serverChannelPermissions(destination.guild, destination.channel))
        == api::MessageWriteAccess::Allowed;
  }
  std::vector<ClientApplication::ForwardDestination> ClientApplication::forwardDestinations() const {
    std::vector<ForwardDestination> result;
    std::map<std::string, std::string> latest;
    const auto query = folded(m_forwardEditor ? m_forwardEditor->query : "");
    std::set<std::string> seen;
    const auto add = [&](ForwardDestination destination) {
      if (result.size() >= maximumForwardCandidates
          || seen.contains(destination.channel)
          || !forwardDestinationAllowed(destination)
          || (!query.empty() && folded(destination.name + " " + destination.detail).find(query) == std::string::npos))
        return;
      seen.insert(destination.channel);
      result.push_back(std::move(destination));
    };
    for (const auto& channel : m_privateChannels) {
      latest[channel.id] = channel.lastMessageId;
      std::string name = channel.name, avatar = channel.icon;
      std::string detail = channel.type == 1 ? "Direct message" : "Group message";
      if (channel.type == 1 && !channel.recipients.empty()) {
        const auto& user = channel.recipients.front();
        name = userName(user.id, user.name);
        avatar = user.avatar;
        if (!user.username.empty())
          detail = user.username;
      }
      if (name.empty())
        for (const auto& user : channel.recipients)
          name += (name.empty() ? "" : ", ") + userName(user.id, user.name);
      add({"", channel.id, name.empty() ? "Group conversation" : name, detail, avatar});
    }
    for (const auto& guild : m_guilds) {
      const auto channels = m_guildChannels.find(guild.id);
      if (channels != m_guildChannels.end())
        for (const auto& channel : channels->second) {
          latest[channel.id] = channel.lastMessageId;
          add({guild.id, channel.id, channel.name, guild.name, guild.icon});
        }
      for (const auto& channel : m_threads.list(guild.id)) {
        latest[channel.id] = channel.lastMessageId;
        add({guild.id, channel.id, channel.name, guild.name + " · Thread", guild.icon});
      }
    }
    std::map<std::string, std::size_t> visited;
    std::map<std::string, std::size_t> accountUsage;
    for (std::size_t index = 0; index < m_emojiPreferences.frequentChannels.size(); ++index)
      accountUsage[m_emojiPreferences.frequentChannels[index]] = m_emojiPreferences.frequentChannels.size() - index;
    for (std::size_t index = 0; index < m_channelCacheOrder.size(); ++index)
      visited[m_channelCacheOrder[index]] = index + 1;
    const auto newer = [](const std::string& a, const std::string& b) {
      return a.size() != b.size() ? a.size() > b.size() : a > b;
    };
    for (const auto& destination : result) {
      auto& last = latest[destination.channel];
      if (!api::validSnowflake(last))
        last.clear();
      if (const auto cursor = m_unreadTimeline.cursor(destination.channel); cursor && newer(cursor->latest, last))
        last = cursor->latest;
      if (const auto cache = m_channelMessages.find(destination.channel); cache != m_channelMessages.end())
        for (const auto& message : cache->second)
          if (api::validSnowflake(message.id) && newer(message.id, last))
            last = message.id;
    }
    std::ranges::stable_sort(result, [&](const auto& a, const auto& b) {
      const auto scoreA = accountUsage.contains(a.channel) ? accountUsage.at(a.channel) : 0;
      const auto scoreB = accountUsage.contains(b.channel) ? accountUsage.at(b.channel) : 0;
      if (scoreA != scoreB)
        return scoreA > scoreB;
      const auto visitA = visited.contains(a.channel) ? visited.at(a.channel) : 0;
      const auto visitB = visited.contains(b.channel) ? visited.at(b.channel) : 0;
      if (visitA != visitB)
        return visitA > visitB;
      return newer(latest.at(a.channel), latest.at(b.channel));
    });
    return result;
  }
  void ClientApplication::openForwardMessage(const api::Message& message) {
    const api::ForwardReference source{message.id, message.channelId, m_dmMode ? "" : m_guildId};
    if (!api::canForwardMessage(message) || blockedMessage(message) || !forwardSourceAllowed(source))
      return;
    openForwardEditor(source, api::forwardPreview(message, source));
  }
  void ClientApplication::openForwardEditor(
      api::ForwardReference source, api::ForwardedMessage snapshot, std::string comment,
      std::optional<ForwardDestination> destination
  ) {
    closeContext();
    closeDialog();
    closeCompletion();
    closeGifPicker();
    closeEmojiPicker();
    cancelMessageEdit(false);
    m_forwardEditor.emplace();
    m_forwardEditor->source = std::move(source);
    m_forwardEditor->snapshot = std::move(snapshot);
    m_forwardEditor->comment = std::move(comment);
    if (destination)
      m_forwardEditor->selected.emplace(destination->channel, *destination);
    m_dialogKind = "forward";
    m_dialogError.clear();
    if (!m_demo && !m_emojiPreferencesLoaded && !m_loadingEmojiPreferences && m_emojiError.empty())
      m_loadingEmojiPreferences = m_client.emojiPreferences();
    m_focusedInput = "forward:search";
    m_window.invalidate();
  }
  void ClientApplication::forwardAction(const std::string& action) {
    if (!m_forwardEditor || m_dialogKind != "forward")
      return;
    auto& editor = *m_forwardEditor;
    if (action == "clear") {
      editor.selected.clear();
      m_dialogError.clear();
      m_window.invalidate();
      return;
    }
    if (action == "emoji") {
      if (m_emojiMenu) {
        closeEmojiPicker();
        return;
      }
      m_reactionMessage.clear();
      m_reactionChannel.clear();
      m_stickerTab = false;
      editor.emoji = true;
      m_emojiMenu = true;
      m_window.invalidate();
      return;
    }
    if (action.starts_with("select:")) {
      const auto id = action.substr(std::string_view("select:").size());
      if (editor.selected.erase(id)) {
        m_window.invalidate();
        return;
      }
      const auto candidates = forwardDestinations();
      const auto found = std::ranges::find(candidates, id, &ForwardDestination::channel);
      if (found != candidates.end() && editor.selected.size() < api::maximumForwardDestinations)
        editor.selected.emplace(id, *found);
      else if (editor.selected.size() >= api::maximumForwardDestinations)
        m_dialogError = "Choose up to five destinations.";
      m_window.invalidate();
      return;
    }
    if (action != "send")
      return;
    if (editor.selected.empty() || editor.selected.size() > api::maximumForwardDestinations)
      return;
    if (!forwardSourceAllowed(editor.source)) {
      m_dialogError = "This message is no longer available to forward.";
      return;
    }
    if (editor.comment.size() > api::maximumForwardCommentBytes
        || api::utf8Length(editor.comment) > api::maximumForwardCommentCharacters) {
      m_dialogError = "Keep the optional comment within 2,000 characters.";
      return;
    }
    for (const auto& [_, destination] : editor.selected)
      if (!forwardDestinationAllowed(destination)) {
        m_dialogError = "A selected destination is no longer writable. Clear the selection to choose again.";
        return;
      }
    auto destinations = editor.selected;
    for (const auto& [id, destination] : destinations) {
      if (!queueForward(destination, editor.source, editor.snapshot, editor.comment)) {
        m_dialogError =
            m_status.empty() ? "Could not queue this forward. Your remaining choices are retained." : m_status;
        return;
      }
      editor.selected.erase(id);
    }
    closeDialog();
  }
  void ClientApplication::drawForwardDialog(Node& root, float width, float height) {
    using namespace theme;
    using D = layout::ForwardDialog;
    if (!m_forwardEditor)
      return;
    auto& editor = *m_forwardEditor;
    const float w = std::min(D::maximumWidth, width - D::padding * 2);
    const float h = std::min(D::maximumHeight, height - layout::Window::titlebarHeight - D::padding * 2);
    const float x = (width - w) / 2,
                y = layout::Window::titlebarHeight + (height - layout::Window::titlebarHeight - h) / 2;
    const float inner = w - D::padding * 2;
    rectangle(
        root, 0, layout::Window::titlebarHeight, width, height - layout::Window::titlebarHeight,
        withAlpha(background, Style::modalBackdropOpacity)
    );
    m_hits.push_back(
        {0, layout::Window::titlebarHeight, width, height - layout::Window::titlebarHeight, Action::None, {}}
    );
    rectangle(root, x, y, w, h, sidebar, Style::radiusXl);
    m_dialogBounds = {x, y, w, h, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    label(
        root, x + D::padding, y + D::padding, inner - Style::controlHeightSm, "Forward To", Style::fontSizeHeading,
        foreground, FontWeight::SemiBold
    );
    label(
        root, x + D::padding, y + D::padding + Style::spaceWide, inner - Style::controlHeight * 3,
        "Choose up to five destinations.", Style::fontSizeDescription, muted
    );
    const auto generation = m_dialogGeneration;
    const auto valid = [this, generation] {
      return m_forwardEditor && m_dialogKind == "forward" && generation == m_dialogGeneration;
    };
    pollButton(
        root, "forward:close",
        {x + w - D::padding - Style::controlHeightSm, y + D::padding / 2, Style::controlHeightSm,
         Style::controlHeightSm},
        "×", [this, valid] {
          if (valid())
            closeDialog();
        }
    );
    const float searchY = y + D::padding + D::heading;
    pollButton(
        root, "forward:clear",
        {x + w - D::padding - Style::controlHeight * 3, searchY - Style::controlHeightSm, Style::controlHeight * 3,
         Style::controlHeightSm},
        "Clear selection",
        [this, valid] {
          if (valid())
            forwardAction("clear");
        },
        !editor.selected.empty()
    );
    inputControl(
        root, x + D::padding, searchY, inner, Style::controlHeightLg, "forward:search", editor.query,
        "Search conversations and channels"
    );
    const float top = searchY + Style::controlHeightLg + Style::spaceSm;
    const float footer = y + h - D::footer;
    editor.viewport = {x + D::padding, top, inner, std::max(0.0F, footer - top)};
    const auto candidates = forwardDestinations();
    const float full = candidates.size() * D::rowHeight;
    m_scrolling.limit(editor.scroll, std::max(0.0F, full - editor.viewport.height));
    auto list = std::make_unique<Node>();
    list->setPosition(editor.viewport.x, editor.viewport.y);
    list->setSize(inner, editor.viewport.height);
    list->setClipChildren(true);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      const auto& destination = candidates[i];
      const float row = i * D::rowHeight - editor.scroll;
      if (row + D::rowHeight <= 0 || row >= editor.viewport.height)
        continue;
      const bool selected = editor.selected.contains(destination.channel);
      pollButton(
          *list, "forward:destination:" + destination.channel, {0, row, inner, D::rowHeight}, "",
          [this, valid, id = destination.channel] {
            if (valid())
              forwardAction("select:" + id);
          }
      );
      const auto* rowButton = static_cast<Button*>(m_widgets.at("forward:destination:" + destination.channel));
      const bool over = rowButton->hovered() && !rowButton->pressed();
      const auto textColor = over ? onHover : foreground;
      const auto detailColor = over ? onHover : muted;
      const auto decorationStart = list->children().size();
      if (destination.guild.empty())
        avatar(
            *list, Style::spaceSm, row + (D::rowHeight - D::avatar) / 2, D::avatar, destination.avatar, destination.name
        );
      else {
        const float iconY = row + (D::rowHeight - D::avatar) / 2;
        const auto guild = std::ranges::find(m_guilds, destination.guild, &api::Guild::id);
        avatar(
            *list, Style::spaceSm, iconY, D::avatar, destination.avatar,
            guild == m_guilds.end() ? destination.detail : guild->name
        );
      }
      const float textX = D::avatar + Style::spaceLg;
      label(
          *list, textX, row + Style::spaceSm, inner - textX - Style::controlHeight, destination.name,
          Style::fontSizeBody, textColor, FontWeight::Medium
      );
      label(
          *list, textX, row + Style::spaceSm + Style::spaceXl, inner - textX - Style::controlHeight, destination.detail,
          Style::fontSizeDescription, detailColor
      );
      const auto id = "forward:check:" + destination.channel;
      std::unique_ptr<Node> check;
      if (const auto old = m_detachedInputs.find(id); old != m_detachedInputs.end()) {
        check = std::move(old->second);
        m_detachedInputs.erase(old);
      } else
        check = std::make_unique<Checkbox>();
      auto* checkbox = static_cast<Checkbox*>(check.get());
      m_widgets[id] = checkbox;
      checkbox->setOnChange({});
      checkbox->setChecked(selected);
      checkbox->setPosition(inner - Style::controlHeightSm, row + (D::rowHeight - Style::controlHeightSm) / 2);
      checkbox->setOnChange([this, valid, channel = destination.channel](bool) {
        if (valid())
          forwardAction("select:" + channel);
      });
      list->addChild(std::move(check));
      for (std::size_t child = decorationStart; child < list->children().size(); ++child) {
        list->children()[child]->setHitTestVisible(false);
        list->children()[child]->setExcludeSubtreeFromTabOrder(true);
      }
    }
    if (candidates.empty())
      label(
          *list, Style::spaceSm, Style::spaceSm, inner - Style::spaceLg, "No writable destinations found.",
          Style::fontSizeBody, muted
      );
    root.addChild(std::move(list));
    if (full > editor.viewport.height) {
      const std::string id = "forward:scrollbar";
      std::unique_ptr<Node> saved;
      if (auto old = m_detachedInputs.find(id); old != m_detachedInputs.end()) {
        saved = std::move(old->second);
        m_detachedInputs.erase(old);
      } else
        saved = std::make_unique<Scrollbar>();
      auto* bar = static_cast<Scrollbar*>(saved.get());
      m_widgets[id] = bar;
      bar->setPosition(x + w - Style::scrollbarHoverWidth, top);
      bar->setSize(Style::scrollbarHoverWidth, editor.viewport.height);
      bar->setOnScrollChanged([this, valid](float offset) {
        if (valid()) {
          m_scrolling.stop(m_forwardEditor->scroll);
          m_forwardEditor->scroll = offset;
          m_window.invalidate();
        }
      });
      bar->update(editor.viewport.height, full, editor.scroll);
      root.addChild(std::move(saved));
    }
    const bool readable = forwardSourceAllowed(editor.source);
    rectangle(
        root, x + D::padding, footer + Style::spaceSm, Style::emphasizedBorderWidth, D::previewHeight - Style::spaceMd,
        raised, Style::radiusXs
    );
    auto preview = std::make_unique<Node>();
    preview->setPosition(x + D::padding + Style::spaceLg, footer + Style::spaceSm);
    preview->setSize(inner - Style::spaceLg, D::previewHeight - Style::spaceMd);
    preview->setClipChildren(true);
    auto* previewNode = root.addChild(std::move(preview));
    if (!readable)
      label(*previewNode, 0, 0, previewNode->width(), "Source message no longer available", Style::fontSizeBody, muted);
    else {
      api::Message captured;
      captured.content = editor.snapshot.content;
      captured.attachments = editor.snapshot.attachments;
      captured.embeds = m_preferences.showEmbeds ? editor.snapshot.embeds : std::vector<api::Embed>{};
      captured.stickers = editor.snapshot.stickers;
      captured.mentions = editor.snapshot.mentions;
      if (!m_preferences.showImages)
        for (auto& attachment : captured.attachments)
          attachment.image.url.clear();
      const MessageMeasure measure = [this](std::string_view text, const MessageStyle& style) {
        return m_window.target()
            .renderer()
            .measureText(
                messageMarkup(text, style), style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
                TextEllipsize::End, true
            )
            .width;
      };
      MessageOptions options{
          .emojiImageLinks = m_preferences.showImages, .compact = true, .unicodeEmoji = [this](std::string_view text) {
            return m_emojiCatalog.isUnicode(text);
          }
      };
      auto mini = layoutRichMessage(captured, previewNode->width(), measure, Style::fontSizeDescription, options);
      const auto media = std::ranges::find_if(mini.pictures, [&](const auto& picture) {
        return m_preferences.showImages && !picture.spoiler && !picture.media.url.empty();
      });
      const auto thumbnail = media == mini.pictures.end() ? std::string{} : media->media.url;
      if (!thumbnail.empty()) {
        const float size = previewNode->height();
        picture(*previewNode, previewNode->width() - size, 0, size, size, thumbnail, Style::radiusSm, true);
        mini = layoutRichMessage(
            captured, previewNode->width() - size - Style::spaceSm, measure, Style::fontSizeDescription, options
        );
      }
      for (const auto& run : mini.runs) {
        if (run.y + run.height > previewNode->height() || run.style.spoiler)
          continue;
        if (!run.image.empty())
          emojiImage(*previewNode, run.x, run.y, run.width, run.image);
        else
          label(
              *previewNode, run.x, run.y, run.width + Style::spaceXxs, messageMarkup(run.text, run.style, false),
              run.style.size, run.style.link.empty() ? muted : accent, FontWeight::Normal, 1, true
          );
      }
      for (const auto& area : messageSpoilerAreas(mini))
        rectangle(*previewNode, area.x, area.y, area.width, area.height, raised, Style::radiusSm);
      drawMessageStrikes(*previewNode, mini, 0, 0, muted, false, {}, previewNode->height());
      if (mini.runs.empty() || (!thumbnail.empty() && mini.plainText.empty()))
        label(
            *previewNode, 0, 0, std::max(0.0F, previewNode->width() - previewNode->height() - Style::spaceSm),
            !captured.stickers.empty() ? "Sticker" : "Attachment", Style::fontSizeDescription, muted
        );
    }
    const float commentY = footer + D::previewHeight;
    const float sendWidth = Style::controlHeightLg * 2 + Style::spaceSm;
    const float emojiWidth = Style::controlHeightLg;
    const float commentWidth = inner - sendWidth - emojiWidth - Style::spaceSm * 2;
    inputControl(
        root, x + D::padding, commentY, commentWidth, Style::controlHeightLg, "forward:comment", editor.comment,
        "Add an optional comment"
    );
    editor.emojiAnchor = {x + D::padding + commentWidth, commentY, emojiWidth, Style::controlHeightLg};
    pollButton(root, "forward:emoji", editor.emojiAnchor, "", [this, valid] {
      if (valid())
        forwardAction("emoji");
    });
    static_cast<Button*>(m_widgets.at("forward:emoji"))->setGlyph("mood-smile");
    pollButton(
        root, "forward:send", {x + w - D::padding - sendWidth, commentY, sendWidth, Style::controlHeightLg}, "Send",
        [this, valid] {
          if (valid())
            forwardAction("send");
        },
        readable && !editor.selected.empty(), true
    );
    const auto error = !readable ? "This message is no longer available to forward." : m_dialogError;
    label(
        root, x + D::padding, commentY + Style::controlHeightLg + Style::spaceSm, inner,
        error.empty() ? std::to_string(editor.selected.size()) + " / 5 selected" : error, Style::fontSizeDescription,
        error.empty() ? muted : danger, FontWeight::Normal, 2
    );
  }
} // namespace app
