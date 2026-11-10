#include "app/client_application.h"
#include "app/discord_timestamp.h"
#include "app/reply_preview.h"
#include "app/theme.h"
#include "ui/controls/scrollbar.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <glib.h>
#include <iostream>
#include <nlohmann/json.hpp>

namespace app {
  using namespace theme;
  namespace {
    std::string folded(const std::string& text) {
      auto* value = g_utf8_casefold(text.c_str(), text.size());
      std::string result = value ? value : text;
      g_free(value);
      return result;
    }
    bool newerId(const std::string& a, const std::string& b) {
      return a.size() != b.size() ? a.size() > b.size() : a > b;
    }
  } // namespace
  bool ClientApplication::forumView() const {
    if (m_dmMode)
      return false;
    const auto channel = std::ranges::find(m_channels, m_channelId, &api::Channel::id);
    return channel != m_channels.end() && channel->isForum();
  }
  void ClientApplication::updateForumSearch() {
    if (!forumView())
      return;
    m_forumSearchTimer.stop();
    m_threadRequests.erase(m_channelId);
    m_threadsLoaded.erase(m_channelId);
    m_threadPages.erase(m_channelId);
    m_threadErrors.erase(m_channelId);
    m_scrolling.stop(m_forumScroll);
    m_forumScroll = 0;
    if (!m_demo) {
      const auto channel = m_channelId, guild = m_guildId;
      m_forumSearchTimer.start(std::chrono::milliseconds(300), [this, channel, guild] {
        if (forumView() && m_channelId == channel && m_guildId == guild && !settingsVisible())
          loadThreads();
      });
    }
    m_window.invalidate();
  }
  void ClientApplication::cacheForumPreview(api::Message message) {
    const auto id = message.channelId;
    if (m_forumDeletedStarters.contains(id))
      return;
    if (const auto prior = m_forumFirstMessages.find(id); prior != m_forumFirstMessages.end()) {
      if (MessageLog::stale(prior->second, message)
          || (!prior->second.editedTimestamp.empty() && message.editedTimestamp.empty())
          || (api::validSnowflake(prior->second.authorId) && !api::validSnowflake(message.authorId)))
        return;
    }
    m_forumFirstMessages[id] = std::move(message);
    std::erase(m_forumPreviewOrder, id);
    m_forumPreviewOrder.push_back(id);
    while (m_forumPreviewOrder.size() > ForumPreviewRead::maximumCached) {
      m_forumFirstMessages.erase(m_forumPreviewOrder.front());
      m_forumPreviewOrder.erase(m_forumPreviewOrder.begin());
    }
    m_forumPreviewErrors.erase(id);
  }
  void ClientApplication::requestForumPreview(const api::Channel& thread) {
    if (m_forumDeletedStarters.contains(thread.id))
      return;
    if (const auto cached = m_forumFirstMessages.find(thread.id); cached != m_forumFirstMessages.end()) {
      std::erase(m_forumPreviewOrder, thread.id);
      m_forumPreviewOrder.push_back(thread.id);
      if (api::validSnowflake(cached->second.authorId))
        return;
    }
    if (m_demo
        || !m_connected
        || !forumView()
        || thread.parentId != m_channelId
        || m_forumPreviewReads.size() >= ForumPreviewRead::maximumPending
        || m_forumPreviewReads.contains(thread.id)
        || m_forumPreviewErrors.contains(thread.id))
      return;
    api::ThreadQuery query{api::ThreadList::Starter, m_guildId, thread.id, {}, ++m_threadSerial, m_threads.revision()};
    if (m_client.threads(query))
      m_forumPreviewReads.emplace(thread.id, ForumPreviewRead{std::move(query)});
    else {
      if (m_forumPreviewErrors.size() >= ForumPreviewRead::maximumCached)
        m_forumPreviewErrors.erase(m_forumPreviewErrors.begin());
      m_forumPreviewErrors[thread.id] = "Post preview could not be loaded.";
    }
  }
  bool ClientApplication::forumPreviewEvent(const api::Event& event) {
    if (!event.threadQuery || event.threadQuery->list != api::ThreadList::Starter)
      return false;
    const auto& query = *event.threadQuery;
    const auto pending = m_forumPreviewReads.find(query.parent);
    if (pending == m_forumPreviewReads.end() || pending->second.query != query)
      return true;
    auto read = std::move(pending->second);
    m_forumPreviewReads.erase(pending);
    const auto thread = m_threads.find(query.guild, query.parent);
    if (query.guild != m_guildId || !forumView() || !thread || thread->parentId != m_channelId)
      return true;
    if (event.error.empty() && event.threadPage && event.threadPage->firstMessages.size() == 1) {
      auto message = event.threadPage->firstMessages.front();
      if (message.id == query.parent && message.channelId == query.parent && api::validSnowflake(message.authorId)) {
        for (const auto& patch : read.patches)
          patch.apply(message);
        for (const auto& reaction : read.reactions)
          api::applyReaction(message, reaction, m_account.id);
        cacheForumPreview(std::move(message));
        m_window.invalidate();
        return true;
      }
    }
    if (m_forumPreviewErrors.size() >= ForumPreviewRead::maximumCached)
      m_forumPreviewErrors.erase(m_forumPreviewErrors.begin());
    m_forumPreviewErrors[query.parent] = "Post preview is unavailable.";
    m_window.invalidate();
    return true;
  }
  void ClientApplication::drawForum(Node& root, float x, float y, float width, float height) {
    const auto parent = std::ranges::find(m_channels, m_channelId, &api::Channel::id);
    if (parent == m_channels.end())
      return;
    const float inset = 22, inner = width - 2 * inset;
    inputControl(
        root, x + inset, y + (Style::spaceLg + Style::spaceXxs), std::max(80.0F, inner - 142), 42, "forum-search",
        m_forumSearch, "Search posts…"
    );
    button(
        root, {x + width - inset - 130, y + (Style::spaceXl - Style::spaceXxs), 130, 34, Action::NewForumPost, {}},
        "New Post", true
    );
    const float sortWidth = 146;
    button(
        root, {x + inset, y + 76, sortWidth, 30, Action::ForumSort, {}},
        m_forumNewest ? "Newest posts" : "Latest activity"
    );
    button(
        root,
        {x + inset + sortWidth + Style::spaceSm, y + 76, 96, 30, Action::ThreadTab,
         m_threadTab == api::ThreadList::Archived ? "forum-active" : "forum-archived"},
        m_threadTab == api::ThreadList::Archived ? "Active posts" : "Archived"
    );
    float tx = sortWidth + 116, ty = 76;
    for (const auto& tag : parent->availableTags) {
      const float tw = std::min(
          inner - sortWidth - Style::spaceMd,
          m_window.target().renderer().measureText(tag.name, Style::fontSizeDescription).width
              + (tag.emojiId.empty() && tag.emojiName.empty() ? 22 : 42)
      );
      if (tx + tw > inner) {
        tx = 0;
        ty += (Style::space2Xl + Style::spaceXs);
      }
      if (ty > 112)
        break;
      const bool selected = m_forumTag == tag.id;
      const float bx = x + inset + tx, by = y + ty;
      rectangle(
          root, bx, by, tw, 30,
          selected                      ? lerpColor(panel, accent, 0.3F)
              : hovered(bx, by, tw, 30) ? raised
                                        : panel,
          15
      );
      float textX = bx + (Style::spaceDense + Style::borderWidth);
      if (!tag.emojiId.empty()) {
        emojiImage(root, textX, by + (Style::spaceCompact + Style::borderWidth), 16, api::emojiPath(tag.emojiId));
        textX += (Style::spaceXl - Style::spaceXxs);
      } else if (!tag.emojiName.empty()) {
        label(root, textX, by + Style::spaceCompact, 22, tag.emojiName, Style::fontSizeTitle, foreground);
        textX += (Style::spaceXl - Style::spaceXxs);
      }
      label(
          root, textX, by + Style::spaceSm, std::max(1.0F, tw - (textX - bx) - Style::spaceSm), tag.name,
          Style::fontSizeDescription, selected ? accent : foreground, FontWeight::Medium
      );
      m_hits.push_back({bx, by, tw, 30, Action::ForumTag, tag.id});
      tx += tw + 7;
    }
    const bool loading = m_threadRequests.contains(m_channelId);
    const auto error = m_threadErrors.find(m_channelId);
    const bool failed = error != m_threadErrors.end() && !error->second.empty();
    const bool more = m_threadPages.contains(m_channelId) && m_threadPages.at(m_channelId).more;
    const float top = y + ty + Style::spaceGroup, footer = failed ? 82 : loading || more ? 48 : 16;
    const float viewport = std::max(20.0F, y + height - top - footer);
    auto threads = m_threads.list(m_guildId, m_channelId);
    const auto query = folded(m_forumSearch);
    std::erase_if(threads, [&](const auto& thread) {
      return thread.archived.value_or(false) != (m_threadTab == api::ThreadList::Archived)
          || (!query.empty() && !folded(thread.name).contains(query))
          || (!m_forumTag.empty()
              && (!thread.appliedTags
                  || std::ranges::find(*thread.appliedTags, m_forumTag) == thread.appliedTags->end()));
    });
    if (!m_forumNewest)
      std::ranges::stable_sort(threads, [](const auto& a, const auto& b) {
        return newerId(
            a.lastMessageId.empty() ? a.id : a.lastMessageId, b.lastMessageId.empty() ? b.id : b.lastMessageId
        );
      });
    constexpr float rowHeight = 148;
    const float fullHeight = threads.size() * rowHeight;
    m_scrolling.limit(m_forumScroll, std::max(0.0F, fullHeight - viewport));
    auto content = std::make_unique<Node>();
    content->setPosition(x + inset, top);
    content->setSize(inner, viewport);
    content->setClipChildren(true);
    content->setTextScrollOffset(-m_forumScroll);
    const auto measure = [&](std::string_view text, const MessageStyle& style) {
      return m_window.target()
          .renderer()
          .measureText(
              messageMarkup(text, style), style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
              TextEllipsize::End, true
          )
          .width;
    };
    for (std::size_t i = 0; i < threads.size(); ++i) {
      const auto& thread = threads[i];
      const float ry = i * rowHeight - m_forumScroll, cardHeight = rowHeight - Style::cardPadding,
                  cardWidth = inner - Style::spaceDense;
      if (ry + cardHeight <= 0 || ry >= viewport)
        continue;
      const float sy = std::max(top, top + ry), end = std::min(top + viewport, top + ry + cardHeight);
      rectangle(
          *content, 0, ry, cardWidth, cardHeight, hovered(x + inset, sy, cardWidth, end - sy) ? raised : panel,
          Style::radiusXl
      );
      m_hits.push_back({x + inset, sy, cardWidth, end - sy, Action::OpenThread, thread.id});
      requestForumPreview(thread);
      const auto first = m_forumFirstMessages.find(thread.id);
      const api::Message* preview = first == m_forumFirstMessages.end() ? nullptr : &first->second;
      std::string thumbnail;
      if (preview) {
        for (const auto& a : preview->attachments)
          if (!a.image.url.empty()) {
            thumbnail = a.image.url;
            break;
          }
        if (thumbnail.empty())
          for (const auto& e : preview->embeds) {
            thumbnail = !e.image.url.empty() ? e.image.url : e.thumbnail.url;
            if (!thumbnail.empty())
              break;
          }
      }
      const float textWidth = std::max(32.0F, cardWidth - Style::space2Xl - (thumbnail.empty() ? 0 : 102));
      float chipX = 14;
      if (thread.appliedTags)
        for (const auto& id : *thread.appliedTags) {
          const auto tag = std::ranges::find(parent->availableTags, id, &api::ForumTag::id);
          if (tag == parent->availableTags.end())
            continue;
          const float tw = m_window.target().renderer().measureText(tag->name, Style::fontSizeDetail).width
              + 20
              + (tag->emojiId.empty() && tag->emojiName.empty() ? 0 : 18);
          if (chipX + tw > textWidth)
            break;
          rectangle(*content, chipX, ry + Style::spaceMd, tw, 23, lerpColor(panel, muted, 0.13F), 11);
          float cx = chipX + (Style::spaceSm + Style::borderWidth);
          if (!tag->emojiId.empty()) {
            emojiImage(*content, cx, ry + Style::spaceLg, 14, api::emojiPath(tag->emojiId));
            cx += (Style::spaceLg + Style::spaceXxs);
          } else if (!tag->emojiName.empty()) {
            label(
                *content, cx, ry + (Style::cardPadding + Style::borderWidth), 18, tag->emojiName,
                Style::fontSizeCaption, foreground
            );
            cx += (Style::spaceLg + Style::spaceXxs);
          }
          label(
              *content, cx, ry + (Style::spaceLg + Style::spaceXxs), tw - (cx - chipX) - Style::spaceXs, tag->name,
              Style::fontSizeDetail, muted
          );
          chipX += tw + 6;
        }
      if (chipX == 14) {
        glyph(*content, 15, ry + Style::spaceLg, 15, thread.locked.value_or(false) ? "lock" : "message", muted);
        if (thread.joined.value_or(false))
          label(
              *content, 37, ry + (Style::spaceLg + Style::spaceXxs), textWidth - (Style::space2Xl - Style::spaceXxs),
              "Following", Style::fontSizeDetail, muted
          );
      }
      label(
          *content, 14, ry + ((Style::spaceSection + Style::spaceXs) + Style::borderWidth), textWidth, thread.name,
          Style::fontSizeSection, foreground, FontWeight::SemiBold
      );
      if (preview && api::validSnowflake(preview->authorId)) {
        const auto owner = thread.ownerId.value_or(preview->authorId);
        const auto name = userName(owner, owner == preview->authorId ? preview->author : "Unknown user");
        const auto plain = oneLineReplyText(name + ": " + (preview->content.empty() ? "Attachment" : preview->content));
        const auto layout = layoutMessage(
            plain, textWidth, measure, Style::fontSizeDescription, {},
            {.compact = true, .unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); }}
        );
        auto previewLine = std::make_unique<Node>();
        previewLine->setPosition(layout::Forum::previewInset, ry + layout::Forum::previewTop);
        previewLine->setSize(textWidth, layout::Forum::previewHeight);
        previewLine->setClipChildren(true);
        auto* line = content->addChild(std::move(previewLine));
        for (const auto& run : layout.runs) {
          if (run.y >= layout::Forum::previewHeight)
            continue;
          if (!run.style.spoiler)
            messageTimestampHover(*line, run, run.x, run.y);
          if (!run.image.empty())
            emojiImage(*line, run.x, run.y, run.width, run.image);
          else
            label(
                *line, run.x, run.y, run.width + Style::spaceXxs, messageMarkup(run.text, run.style, false),
                run.style.size, muted, FontWeight::Normal, 1, true
            );
        }
        drawMessageStrikes(*line, layout, 0, 0, muted);
      } else {
        const bool deleted = m_forumDeletedStarters.contains(thread.id);
        const bool unavailable = m_forumPreviewErrors.contains(thread.id);
        const auto owner = thread.ownerId ? userName(*thread.ownerId, "") : "";
        const auto text = (owner.empty() ? "" : owner + ": ")
            + (deleted           ? "Opening message was deleted."
                   : unavailable ? "Preview unavailable · Retry"
                                 : "Loading post preview…");
        label(
            *content, layout::Forum::previewInset, ry + layout::Forum::previewTop, textWidth, text,
            Style::fontSizeDescription, muted
        );
        if (unavailable && !deleted) {
          const float py = std::max(top, top + ry + layout::Forum::previewTop);
          const float bottom =
              std::min(top + viewport, top + ry + layout::Forum::previewTop + layout::Forum::previewHeight);
          if (bottom > py)
            m_hits.push_back(
                {x + inset + layout::Forum::previewInset, py, textWidth, bottom - py, Action::ForumPreviewRetry,
                 thread.id}
            );
        }
      }
      float detailX = 14;
      if (preview && !preview->reactions.empty()) {
        const auto& reaction = preview->reactions.front();
        rectangle(
            *content, detailX, ry + 102, 62, 24, lerpColor(background, accent, reaction.me ? 0.2F : 0.07F),
            Style::hoverRadius
        );
        if (!reaction.id.empty())
          emojiImage(
              *content, detailX + (Style::spaceCompact + Style::borderWidth), ry + 105, 18, api::emojiPath(reaction.id)
          );
        else
          label(
              *content, detailX + (Style::spaceCompact + Style::borderWidth), ry + 103, 28, reaction.name,
              Style::fontSizeHeading, foreground
          );
        label(
            *content, detailX + (Style::space2Xl + Style::spaceXxs), ry + 108, 24, std::to_string(reaction.count),
            Style::fontSizeMini, muted
        );
        detailX += 76;
      }
      glyph(*content, detailX, ry + 108, Style::glyphSizeSmall, "message", muted);
      const auto count = thread.messageCount ? std::to_string(*thread.messageCount) : "";
      label(
          *content, detailX + (Style::spaceRelaxed + Style::borderWidth), ry + 108, 46, count, Style::fontSizeMini,
          muted
      );
      std::uint64_t snowflake = 0;
      const auto& last = thread.lastMessageId.empty() ? thread.id : thread.lastMessageId;
      const auto parsed = std::from_chars(last.data(), last.data() + last.size(), snowflake);
      if (parsed.ec == std::errc{} && snowflake > (1ULL << 22)) {
        const auto relative = discordTimestamp("t:" + std::to_string((snowflake >> 22) / 1000 + 1420070400ULL) + ":R");
        if (relative)
          label(
              *content, detailX + 68, ry + 108, std::max(1.0F, textWidth - detailX - 70), *relative,
              Style::fontSizeMini, muted
          );
      }
      if (!thumbnail.empty())
        picture(
            *content, cardWidth - 106, ry + (Style::cardPadding + Style::borderWidth), 90, 90, thumbnail,
            Style::radiusLg, true
        );
    }
    if (threads.empty())
      label(
          *content, 14, 22, inner - Style::spaceWide,
          loading                                             ? "Loading posts…"
              : !m_forumSearch.empty() || !m_forumTag.empty() ? "No matching posts."
                                                              : "No posts yet.",
          Style::fontSizeBody, muted
      );
    root.addChild(std::move(content));
    if (fullHeight > viewport) {
      const std::string id = "forum-scrollbar";
      std::unique_ptr<Node> owned;
      if (auto stored = m_detachedInputs.find(id); stored != m_detachedInputs.end()) {
        owned = std::move(stored->second);
        m_detachedInputs.erase(stored);
      } else {
        auto bar = std::make_unique<Scrollbar>();
        bar->setTrackInset(3);
        bar->setOnScrollChanged([this](float offset) {
          m_scrolling.stop(m_forumScroll);
          m_forumScroll = offset;
          m_window.invalidate();
        });
        owned = std::move(bar);
      }
      auto* bar = static_cast<Scrollbar*>(owned.get());
      m_widgets[id] = bar;
      bar->setPosition(x + width - 14, top);
      bar->setSize(bar->reservedThickness(), viewport);
      bar->update(viewport, fullHeight, m_forumScroll);
      root.addChild(std::move(owned));
    }
    const float footerY = top + viewport + 9;
    if (failed) {
      label(
          root, x + inset, footerY, inner - 94, error->second, Style::fontSizeDescription, rgbHex(0xED4245),
          FontWeight::Normal, 3
      );
      button(root, {x + width - inset - 84, footerY + Style::spaceXs, 84, 30, Action::ForumRetry, {}}, "Retry");
    } else if (loading)
      label(root, x + inset, footerY, inner, "Loading posts…", Style::fontSizeDescription, muted);
    else if (more)
      button(root, {x + inset, footerY, 120, 30, Action::ForumMore, {}}, "Load more");
  }
  void ClientApplication::newForumPost() {
    if (!forumView())
      return;
    closeDialog();
    closeContext();
    m_dialogKind = "forum-create";
    m_dialogText.clear();
    m_forumPostTag = m_forumTag;
    m_dialogTitle = "New post";
    m_dialogAction = api::Interaction{
        .kind = api::InteractionKind::CreateForumPost,
        .channel = m_channelId,
        .guild = m_guildId,
        .revision = ++m_dialogGeneration,
        .stateRevision = m_threads.revision()
    };
    m_focusedInput = "dialog";
    m_window.invalidate();
  }
  void ClientApplication::drawForumPostDialog(Node& root, float w, float h) {
    const float width = std::min(540.0F, w - Style::space2Xl), height = std::min(480.0F, h - 64);
    const float x = (w - width) / 2, y = std::max(44.0F, (h - height) / 2);
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, Color{0, 0, 0, 0.55F});
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::None, {}});
    rectangle(root, x, y, width, height, sidebar, 14);
    m_dialogBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    label(
        root, x + Style::spaceXl, y + ((Style::spaceXl - Style::spaceXxs) + Style::borderWidth), width - 80, "New post",
        Style::fontSizeModal, foreground, FontWeight::SemiBold
    );
    iconButton(
        root, {x + width - 46, y + Style::spaceLg, 30, 30, m_dialogPending ? Action::None : Action::CloseDialog, {}},
        "x", "Close"
    );
    inputControl(root, x + Style::spaceXl, y + 70, width - 48, 42, "dialog", m_dialogText, "Post title");
    inputControl(
        root, x + Style::spaceXl, y + 126, width - 48, std::max(60.0F, height - 332), "forum-body",
        m_drafts[m_channelId], "Write your post…", true
    );
    const auto channel = std::ranges::find(m_channels, m_channelId, &api::Channel::id);
    float tx = 24, ty = height - 190;
    if (channel != m_channels.end())
      for (const auto& tag : channel->availableTags) {
        const float tw = std::min(
            width - 48, m_window.target().renderer().measureText(tag.name, Style::fontSizeDescription).width + 26
        );
        if (tx + tw > width - 24) {
          tx = 24;
          ty += Style::space2Xl;
        }
        if (ty > height - 126)
          break;
        button(
            root, {x + tx, y + ty, tw, 27, m_dialogPending ? Action::None : Action::ForumPostTag, tag.id}, tag.name,
            m_forumPostTag == tag.id
        );
        tx += tw + 6;
      }
    label(
        root, x + Style::spaceXl, y + height - 92, width - Style::spaceGroup, m_dialogError, Style::fontSizeDescription,
        rgbHex(0xED4245), FontWeight::Normal, 2
    );
    button(
        root, {x + Style::spaceXl, y + height - 52, 88, 32, m_dialogPending ? Action::None : Action::CloseDialog, {}},
        "Cancel"
    );
    button(
        root, {x + width - 136, y + height - 52, 112, 32, m_dialogPending ? Action::None : Action::SubmitDialog, {}},
        m_dialogPending ? "Posting…" : "Post", true
    );
    m_tooltip.clear();
  }
  void ClientApplication::prepareForumDemo(const std::string& view) {
    m_demoOverlay = view;
    if (view.ends_with("light"))
      m_preferences.theme = "light";
    using Json = nlohmann::json;
    auto forum = api::parseChannels(
                     Json::array(
                         {{{"id", "105"},
                           {"type", 15},
                           {"name", "showcase"},
                           {"flags", 16},
                           {"available_tags",
                            Json::array(
                                {{{"id", "70"}, {"name", "native apps"}, {"emoji_name", "🌙"}},
                                 {{"id", "71"}, {"name", "desktops"}, {"emoji_name", "🐧"}},
                                 {{"id", "72"}, {"name", "art"}, {"emoji_name", "🎨"}}}
                            )}}}
                     )
    ).front();
    m_guildChannels[m_guildId] = m_channels;
    m_guildChannels[m_guildId].push_back(forum);
    rebuildThreadChannels();
    Json threads = Json::array(), messages = Json::array();
    for (int i = 0; i < 12; ++i) {
      const auto id = std::to_string(9000 + i);
      threads.push_back(
          {{"id", id},
           {"type", 11},
           {"guild_id", "1"},
           {"parent_id", "105"},
           {"owner_id", i % 2 ? "11" : "12"},
           {"name",
            i == 0        ? "Weekend project"
                : i == 11 ? "My native desktop setup"
                          : "A little creative project " + std::to_string(i)},
           {"applied_tags", Json::array({i % 2 == 0 ? "70" : "71"})},
           {"message_count", i * 3},
           {"member", Json::object()},
           {"thread_metadata", {{"archived", false}, {"locked", false}}}}
      );
      api::Message message{
          id,
          id,
          i % 2 ? "Maya" : "Noah",
          "A small project, built with care. What do you think?",
          "2026-10-17T09:50:00Z",
          i % 2 ? "11" : "12"
      };
      message.attachments.push_back({id, "preview.png", "", "image/png", {"demo-preview", 640, 360}});
      message.reactions = {{"", "❤️", i + 1}};
      cacheForumPreview(std::move(message));
    }
    api::ThreadQuery query{api::ThreadList::Active, "1", "105", {}, 0, m_threads.revision()};
    m_threads.page(query, api::parseThreadPage(Json{{"threads", threads}}, query));
    rebuildThreadChannels();
    selectChannel("105");
    if (view == "forum-error")
      m_threadErrors["105"] = "You do not have permission to access this channel or perform this action.";
  }
  void ClientApplication::demoForumStep() {
    auto require = [](bool ok, const char* message) {
      if (!ok)
        throw std::runtime_error(message);
    };
    auto click = [&](Action action, const std::string& id = "") {
      auto h =
          std::ranges::find_if(m_hits, [&](const auto& h) { return h.action == action && (id.empty() || h.id == id); });
      require(h != m_hits.end(), "Forum replay control is missing");
      const auto copy = *h;
      activate(copy);
    };
    const auto step = m_demoInteractionStep++;
    const auto starterResult = [&](const std::string& id, const std::string& error = "") {
      api::Event result;
      result.operation = api::Operation::Threads;
      result.threadQuery =
          api::ThreadQuery{api::ThreadList::Starter, m_guildId, id, {}, ++m_threadSerial, m_threads.revision()};
      m_forumPreviewReads[id] = {*result.threadQuery};
      result.error = error;
      if (error.empty()) {
        result.threadPage.emplace();
        const auto thread = m_threads.find(m_guildId, id);
        const auto owner = thread ? thread->ownerId.value_or("12") : "12";
        result.threadPage->firstMessages.push_back(
            {id, id, owner == "11" ? "Maya" : "Noah", "Opening post", "2026-10-17T09:50:00Z", owner}
        );
      }
      return result;
    };
    std::clog << "Forum replay step " << step << '\n';
    if (step == 0) {
      m_unreadTimeline.snapshot({{"105", api::UnreadCursor{.after = "9000", .latest = "9011"}}});
      m_unreadChannels.insert("105");
      require(markForumRead(), "A focused forum view acknowledges its latest post activity");
      require(
          !m_unreadTimeline.cursor("105") && !m_unreadChannels.contains("105") && m_localRead["105"] == "9011",
          "Viewing a forum clears its unread dot"
      );
      require(!markForumRead(), "An already acknowledged forum does not repeat its read");
      m_unreadChannels.insert("105");
      m_dialogKind = "test";
      require(!markForumRead() && m_unreadChannels.contains("105"), "Covered forums are not acknowledged");
      m_dialogKind.clear();
      require(markForumRead() && !m_unreadChannels.contains("105"), "Uncovered forums clear stale local dots");
      auto saved = m_forumFirstMessages;
      auto savedOrder = m_forumPreviewOrder;
      for (std::size_t i = 0; i < ForumPreviewRead::maximumCached; ++i) {
        const auto id = std::to_string(80000 + i);
        cacheForumPreview({id, id, "Noah", "Cached starter", {}, "12"});
      }
      cacheForumPreview({"100", "100", "Noah", "An older archived post", {}, "12"});
      require(
          m_forumFirstMessages.contains("100") && m_forumFirstMessages.size() <= ForumPreviewRead::maximumCached,
          "Reading an older post cannot immediately evict its own preview from a full cache"
      );
      m_forumFirstMessages = std::move(saved);
      m_forumPreviewOrder = std::move(savedOrder);
      m_forumFirstMessages.erase("9011");
      auto starter = starterResult("9011");
      updateMessage("9011", "9011", {.content = "Edited while the preview loaded"});
      threadEvent(starter);
      require(
          m_forumFirstMessages.at("9011").authorId == "11"
              && m_forumFirstMessages.at("9011").content == "Edited while the preview loaded"
              && m_forumPreviewReads.empty(),
          "A missing starter loads with its owner while preserving newer live edits"
      );
      auto deletedStarter = starterResult("9010");
      deleteMessages("9010", {"9010"});
      threadEvent(deletedStarter);
      cacheForumPreview(deletedStarter.threadPage->firstMessages.front());
      require(
          !m_forumFirstMessages.contains("9010") && m_forumDeletedStarters.contains("9010"),
          "Late individual and bundled starter reads cannot restore a deleted opening message"
      );
      require(
          forumView() && m_dialogKind.empty() && m_channelId == "105",
          "Forums must select their own channel in the main content area"
      );
      require(
          (!m_inputs.contains("composer") || !m_inputs.at("composer")->parent())
              && m_widgets.contains("forum-scrollbar"),
          "Forum lists have their own scrollbar and no chat composer"
      );
      click(Action::OpenThread, "9011");
    } else if (step == 1) {
      require(m_channelId == "9011" && !forumView(), "A forum post uses the normal thread timeline");
      click(Action::ThreadParent);
    } else if (step == 2) {
      require(
          forumView() && m_channelId == "105" && m_dialogKind.empty(),
          "Back restores the forum without an overlay or empty channel"
      );
      TextInputEdit edit;
      edit.hasCommitText = true;
      edit.commitText = "Weekend";
      m_inputs.at("forum-search")->textInputApplyEdit(edit);
    } else if (step == 3) {
      require(m_forumSearch == "Weekend", "Forum search uses the retained native editor");
      require(
          std::ranges::any_of(m_hits, [](const auto& h) { return h.action == Action::OpenThread && h.id == "9000"; }),
          "Forum search filters visible post cards"
      );
      click(Action::ForumTag, "70");
      m_forumFirstMessages.erase("9000");
      threadEvent(starterResult("9000", "Synthetic preview failure"));
    } else if (step == 4) {
      require(m_forumPreviewErrors.contains("9000"), "A failed preview stays settled until explicit retry");
      click(Action::ForumPreviewRetry, "9000");
      require(!m_forumPreviewErrors.contains("9000"), "The card's Retry control re-enables its preview read");
      threadEvent(starterResult("9000"));
      require(m_forumFirstMessages.at("9000").content == "Opening post", "Retry restores the original post preview");
      require(m_forumTag == "70", "Forum tag filters are interactive");
      m_forumSearch.clear();
      m_forumTag.clear();
      updateForumSearch();
      click(Action::ForumSort);
    } else if (step == 5) {
      require(m_forumNewest, "Forum sorting changes the post order");
      m_threadErrors["105"] = "You do not have permission to access this channel or perform this action.";
      m_window.invalidate();
    } else if (step == 6) {
      const auto retry = std::ranges::find(m_hits, Action::ForumRetry, &Hit::action);
      require(retry != m_hits.end(), "Forum failures offer Retry");
      for (const auto& h : m_hits)
        if (h.action == Action::OpenThread)
          require(h.y + h.height <= retry->y, "Forum errors and retry controls must not overlap post cards");
      m_threadErrors.erase("105");
      click(Action::NewForumPost);
    } else if (step == 7) {
      require(
          m_dialogKind == "forum-create" && m_inputs.at("forum-body")->parent(),
          "New Post offers native title and message editors"
      );
      m_dialogText = "A new project";
      focusControl("forum-body");
      m_window.key({.symbol = 'a', .text = "a"});
      require(m_drafts["105"] == "a", "The post body accepts keyboard input through the overlay and input dispatcher");
      m_drafts["105"] = "Here is my latest project.";
      submitDialog();
      require(
          !m_dialogPending && m_dialogError == "Choose a tag for this post.",
          "Required forum tags are validated before sending"
      );
      click(Action::ForumPostTag, "70");
    } else if (step == 8) {
      require(m_forumPostTag == "70", "The post composer retains the selected forum tag");
      closeDialog();
    } else if (step == 9) {
      require(
          forumView() && m_dialogKind.empty() && m_drafts["105"] == "Here is my latest project.",
          "Cancelling preserves the unsent post body"
      );
      const auto revision = m_threads.revision();
      api::Event failure;
      failure.operation = api::Operation::Threads;
      failure.threadQuery = api::ThreadQuery{api::ThreadList::Active, "1", "105", {}, ++m_threadSerial, revision};
      m_threadRequests["105"] = *failure.threadQuery;
      failure.error = "A synthetic request failed.";
      m_forumFirstMessages.erase("9000");
      auto latePreview = starterResult("9000");
      selectChannel("101");
      threadEvent(failure);
      threadEvent(latePreview);
      require(
          !m_forumFirstMessages.contains("9000"), "A preview read cannot populate the cache after leaving its forum"
      );
    } else if (step == 10) {
      require(
          m_channelId == "101" && m_dialogKind.empty(),
          "A late forum failure cannot change the selected channel or open a modal"
      );
      selectChannel("105");
    } else if (step == 11) {
      require(forumView() && m_channelId == "105", "Forum navigation survives errors and revisits");
      click(Action::ThreadTab, "forum-archived");
      require(m_threadTab == api::ThreadList::Archived, "Archived forum posts stay in the main forum view");
      std::clog << "Offline forum replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
