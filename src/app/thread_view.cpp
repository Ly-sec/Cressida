#include "app/client_application.h"
#include "app/theme.h"
#include "ui/controls/scrollbar.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>

namespace app {
  using namespace theme;
  void ClientApplication::clearThreads() {
    clearChannelFeatures();
    m_threads = {};
    m_forumSearchTimer.stop();
    m_forumFirstMessages.clear();
    m_threadPages.clear();
    m_forumSearch.clear();
    m_forumTag.clear();
    m_forumPostTag.clear();
    m_scrolling.stop(m_forumScroll);
    m_forumScroll = 0;
    m_threadRequests.clear();
    m_threadsLoaded.clear();
    m_threadErrors.clear();
    m_threadPending.clear();
    m_archiveQuery.reset();
    m_openThreadQuery.reset();
    m_threadParent.clear();
    m_threadBefore.clear();
    m_threadError.clear();
    m_threadScroll = 0;
    m_threadMore = false;
  }
  void ClientApplication::loadThreads(bool retry, bool more) {
    const auto parent = forumView() ? m_channelId : m_threadParent;
    if (m_demo
        || !m_connected
        || m_dmMode
        || !api::validSnowflake(parent)
        || m_threadRequests.contains(parent)
        || (!retry && !more && (m_threadsLoaded.contains(parent) || m_threadErrors.contains(parent))))
      return;
    api::ThreadQuery query{
        forumView() && m_threadTab == api::ThreadList::Archived ? api::ThreadList::Archived : api::ThreadList::Active,
        m_guildId,
        parent,
        more ? m_threadPages[parent].before : "",
        ++m_threadSerial,
        m_threads.revision()
    };
    if (forumView()) {
      query.search = m_forumSearch;
      query.tag = m_forumTag;
      query.newest = m_forumNewest;
    }
    m_threadErrors.erase(parent);
    if (m_client.threads(query))
      m_threadRequests[parent] = query;
    else
      m_threadErrors[parent] = "Could not load posts. Try again.";
  }
  void ClientApplication::rebuildThreadChannels() {
    if (m_dmMode || m_guildId.empty())
      return;
    auto base = m_guildChannels.find(m_guildId);
    if (base == m_guildChannels.end())
      return;
    std::vector<api::Channel> result;
    const auto threads = m_threads.list(m_guildId);
    for (const auto& channel : base->second) {
      result.push_back(channel);
      if (channel.type == 4)
        continue;
      for (const auto& thread : threads)
        if (thread.parentId == channel.id
            && ((!thread.archived.value_or(false) && thread.joined.value_or(false))
                || thread.id == m_channelId
                || thread.id == m_restoreChannel))
          result.push_back(thread);
    }
    m_channels = std::move(result);
  }
  void ClientApplication::threadEvent(const api::Event& event) {
    m_messageLayouts.clear();
    m_completionDirty = true;
    const auto selected = m_threads.find(m_guildId, m_channelId);
    const auto oldParent = selected ? selected->parentId : "";
    if (event.operation == api::Operation::ThreadUpdate) {
      m_threads.apply(event.threadUpdates);
      if (event.threadUpdates.reset) {
        m_threadRequests.clear();
        m_threadsLoaded.clear();
        m_threadErrors.clear();
        m_openThreadQuery.reset();
        m_archiveQuery.reset();
        loadThreads();
        if (m_dialogKind == "threads" && m_threadTab != api::ThreadList::Active)
          loadArchivedThreads();
      }
    } else if (event.threadQuery) {
      const auto& query = *event.threadQuery;
      const bool active = m_threadRequests.contains(query.parent) && m_threadRequests.at(query.parent) == query;
      const bool archive = m_archiveQuery == query;
      const bool opening = m_openThreadQuery == query;
      if (!active && !archive && !opening)
        return;
      if (active) {
        m_threadRequests.erase(query.parent);
        if (event.error.empty())
          m_threadsLoaded.insert(query.parent);
        else
          m_threadErrors[query.parent] = event.error;
      }
      if (archive) {
        m_archiveQuery.reset();
        m_threadError = event.error;
        if (event.threadPage) {
          m_threadMore = event.threadPage->more;
          m_threadBefore = event.threadPage->before;
        }
      }
      if (event.error.empty() && event.threadPage) {
        m_threads.page(query, *event.threadPage);
        if (active)
          m_threadPages[query.parent] = {{}, event.threadPage->more, event.threadPage->before};
        for (const auto& message : event.threadPage->firstMessages) {
          m_forumFirstMessages[message.channelId] = message;
          while (m_forumFirstMessages.size() > 256)
            m_forumFirstMessages.erase(m_forumFirstMessages.begin());
        }
      }
      if (opening) {
        m_openThreadQuery.reset();
        if (event.error.empty() && query.guild == m_guildId && m_threads.find(query.guild, query.parent)) {
          const auto jump = m_pendingMessageJump;
          m_pendingMessageJump.reset();
          openThread(query.parent);
          if (jump && jump->first == query.parent)
            jumpToMessage(jump->first, jump->second);
        } else
          m_status = event.error.empty() ? "This thread is no longer available." : event.error;
      }
    }
    rebuildThreadChannels();
    // Deletion/access loss leaves a useful parent conversation, never a stale
    // composer pointing at a thread which is no longer accessible.
    if (!oldParent.empty() && !m_threads.find(m_guildId, m_channelId)) {
      const auto removedId = m_channelId;
      m_channelMessages.erase(removedId);
      m_channelMembers.erase(removedId);
      auto parent = std::ranges::find(m_channels, oldParent, &api::Channel::id);
      if (parent != m_channels.end() && !parent->isForum())
        selectChannel(oldParent);
      else {
        const bool showParent = parent != m_channels.end();
        selectChannel("");
        if (showParent)
          selectChannel(oldParent);
      }
      m_channelMessages.erase(removedId);
      m_channelMembers.erase(removedId);
      std::erase(m_channelCacheOrder, removedId);
    }
    refreshMemberSubscription();
    m_window.invalidate();
  }
  void ClientApplication::showThreads(const std::string& parent) {
    if (m_dmMode || m_guildId.empty() || !api::validSnowflake(parent))
      return;
    const auto channel = std::ranges::find(m_channels, parent, &api::Channel::id);
    if (channel != m_channels.end() && channel->isForum()) {
      selectChannel(parent);
      return;
    }
    closeVideo();
    closeGifPicker();
    closeContext();
    closeDialog();
    m_emojiMenu = m_presenceMenu = false;
    m_scrolling.stopAll();
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_threadParent = parent;
    m_threadTab = api::ThreadList::Active;
    m_threadScroll = 0;
    m_threadBefore.clear();
    m_threadError.clear();
    m_threadMore = false;
    m_dialogKind = "threads";
    loadThreads();
    m_window.invalidate();
  }
  void ClientApplication::loadArchivedThreads(bool more) {
    if (m_threadTab == api::ThreadList::Active || m_archiveQuery || m_demo)
      return;
    api::ThreadQuery query{m_threadTab,      m_guildId,           m_threadParent, more ? m_threadBefore : "",
                           ++m_threadSerial, m_threads.revision()};
    m_threadError.clear();
    if (m_client.threads(query))
      m_archiveQuery = query;
    else
      m_threadError = "Could not load archived threads. Try again.";
  }
  void ClientApplication::drawThreads(Node& root, float w, float h) {
    const float width = std::min(620.0F, w - Style::space2Xl), height = std::min(584.0F, h - 72);
    const float x = (w - width) / 2, y = std::max(44.0F, (h - height) / 2);
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, Color{0, 0, 0, 0.5F});
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::None, {}});
    rectangle(root, x, y, width, height, sidebar, 14);
    m_dialogBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    glyph(
        root, x + (Style::spaceXl - Style::spaceXxs), y + Style::spaceXl, Style::glyphSizeFeature, "messages", accent
    );
    label(
        root, x + 58, y + ((Style::spaceXl - Style::spaceXxs) + Style::borderWidth), width - 120, "Threads",
        Style::fontSizeModal, foreground, FontWeight::SemiBold
    );
    iconButton(root, {x + width - 46, y + Style::spaceLg, 30, 30, Action::CloseDialog, {}}, "x", "Close threads");
    const auto parent = std::ranges::find(m_channels, m_threadParent, &api::Channel::id);
    label(
        root, x + Style::spaceXl, y + 59, width - Style::spaceGroup,
        parent == m_channels.end() ? "Channel threads" : "# " + parent->name, Style::fontSizeCaption, muted
    );
    float tx = x + (Style::spaceXl - Style::spaceXxs);
    for (const auto& [id, title] :
         {std::pair{"active", "Active"}, {"archived", "Archived"}, {"private", "Private · joined"}}) {
      const bool chosen = m_threadTab
          == (std::string_view(id) == "active"         ? api::ThreadList::Active
                  : std::string_view(id) == "archived" ? api::ThreadList::Archived
                                                       : api::ThreadList::PrivateArchived);
      const float bw =
          m_window.target().renderer().measureText(title, Style::fontSizeCaption, FontWeight::Medium).width + 28;
      button(root, {tx, y + 90, bw, 32, Action::ThreadTab, id}, title, chosen);
      tx += bw + 8;
    }
    const float viewport = std::max(30.0F, height - 242), top = y + 138;
    auto threads = m_threads.list(m_guildId, m_threadParent);
    std::erase_if(threads, [&](const auto& thread) {
      if (m_threadTab == api::ThreadList::Active)
        return thread.archived.value_or(false);
      return !thread.archived.value_or(false)
          || (m_threadTab == api::ThreadList::Archived ? thread.type == 12
                                                       : thread.type != 12 || !thread.joined.value_or(false));
    });
    const float fullHeight = threads.size() * 68.0F;
    m_scrolling.limit(m_threadScroll, std::max(0.0F, fullHeight - viewport));
    auto content = std::make_unique<Node>();
    content->setPosition(x + Style::spaceRelaxed, top);
    content->setSize(width - Style::spaceSection, viewport);
    content->setClipChildren(true);
    content->setTextScrollOffset(-m_threadScroll);
    for (std::size_t i = 0; i < threads.size(); ++i) {
      const auto& thread = threads[i];
      const float ry = i * 68 - m_threadScroll;
      if (ry + 62 < 0 || ry > viewport)
        continue;
      const float sy = std::max(top, top + ry), end = std::min(top + viewport, top + ry + 62);
      const bool over = hovered(x + Style::spaceRelaxed, sy, width - Style::spaceGroup, end - sy);
      rectangle(*content, 0, ry, width - Style::spaceGroup, 60, over ? raised : panel, Style::radiusLg);
      glyph(
          *content, 14, ry + (Style::spaceLg + Style::spaceXxs), 21,
          thread.locked.value_or(false) || thread.type == 12 ? "lock" : "message", accent
      );
      label(
          *content, 48, ry + (Style::spaceDense + Style::borderWidth), width - 120, thread.name, Style::fontSizeBody,
          foreground, FontWeight::SemiBold
      );
      std::string detail = thread.archived.value_or(false) ? "Archived" : "Active";
      if (thread.messageCount)
        detail += " · " + std::to_string(*thread.messageCount) + " messages";
      if (thread.joined.value_or(false))
        detail += " · Joined";
      label(
          *content, 48, ry + ((Style::space2Xl + Style::spaceXxs) + Style::borderWidth), width - 120, detail,
          Style::fontSizeMini, muted
      );
      m_hits.push_back(
          {x + Style::spaceRelaxed, sy, width - 48, std::max(0.0F, end - sy), Action::OpenThread, thread.id}
      );
    }
    root.addChild(std::move(content));
    if (fullHeight > viewport) {
      const std::string id = "thread-scrollbar";
      std::unique_ptr<Node> owned;
      if (auto stored = m_detachedInputs.find(id); stored != m_detachedInputs.end()) {
        owned = std::move(stored->second);
        m_detachedInputs.erase(stored);
      } else {
        auto bar = std::make_unique<Scrollbar>();
        bar->setTrackInset(4);
        bar->setOnScrollChanged([this](float offset) {
          m_scrolling.stop(m_threadScroll);
          m_threadScroll = offset;
          m_window.invalidate();
        });
        owned = std::move(bar);
      }
      auto* bar = static_cast<Scrollbar*>(owned.get());
      m_widgets[id] = bar;
      bar->setPosition(x + width - Style::spaceLg, top);
      bar->setSize(bar->reservedThickness(), viewport);
      bar->update(viewport, fullHeight, m_threadScroll);
      root.addChild(std::move(owned));
    }
    const bool loading =
        m_threadTab == api::ThreadList::Active ? m_threadRequests.contains(m_threadParent) : m_archiveQuery.has_value();
    const auto error = m_threadTab == api::ThreadList::Active ? m_threadErrors[m_threadParent] : m_threadError;
    if (threads.empty())
      label(
          root, x + (Style::space2Xl - Style::spaceXxs), top + Style::spaceWide, width - 60,
          loading              ? "Loading threads…"
              : !error.empty() ? error
                               : "No threads here yet.",
          Style::fontSizeBody, muted, FontWeight::Normal, 3
      );
    else if (!error.empty())
      label(
          root, x + (Style::spaceXl - Style::spaceXxs), y + height - 68, width - 44, error, Style::fontSizeMini,
          rgbHex(0xED4245), FontWeight::Normal, 2
      );
    if (!error.empty())
      button(root, {x + (Style::spaceXl - Style::spaceXxs), y + height - 48, 80, 30, Action::ThreadRetry, {}}, "Retry");
    else if (loading)
      label(
          root, x + Style::spaceXl, y + height - Style::spaceSection, width - Style::spaceGroup, "Loading…",
          Style::fontSizeDescription, muted
      );
    else if (
        m_threadTab == api::ThreadList::Active
            ? m_threadPages.contains(m_threadParent) && m_threadPages.at(m_threadParent).more
            : m_threadMore
    )
      button(
          root, {x + (Style::spaceXl - Style::spaceXxs), y + height - 48, 120, 30, Action::ThreadMore, {}}, "Load more"
      );
    if (parent != m_channels.end() && (parent->type == 0 || parent->type == 5))
      button(
          root, {x + width - 164, y + height - 48, 142, 30, Action::CreateThread, m_threadParent}, "Create Thread", true
      );
    m_tooltip.clear();
  }
  void ClientApplication::openThread(const std::string& id) {
    m_pendingDiscordLink.reset();
    if (m_dmMode || !api::validSnowflake(id))
      return;
    if (!m_threads.find(m_guildId, id)) {
      for (const auto& message : m_messages)
        if (message.thread && message.thread->id == id)
          m_threads.page({api::ThreadList::Single, m_guildId, message.thread->id, {}, 0, 0}, {{*message.thread}});
    }
    if (const auto* thread = m_threads.find(m_guildId, id)) {
      const auto copy = *thread;
      const auto parent = std::ranges::find(m_channels, copy.parentId, &api::Channel::id);
      if (parent != m_channels.end())
        m_collapsedCategories.erase(parent->parentId);
      if (std::ranges::find(m_channels, id, &api::Channel::id) == m_channels.end())
        m_channels.push_back(copy);
      selectChannel(id);
    } else if (!m_demo) {
      closeContext();
      closeDialog();
      api::ThreadQuery query{api::ThreadList::Single, m_guildId, id, {}, ++m_threadSerial, m_threads.revision()};
      if (m_client.threads(query)) {
        m_openThreadQuery = query;
        m_status = "Opening thread…";
      } else
        m_status = "Could not open thread. Try again.";
    }
    m_window.invalidate();
  }
  void ClientApplication::createThread(const std::string& parent, const std::string& message) {
    const auto c = std::ranges::find(m_channels, parent, &api::Channel::id);
    if (m_dmMode || c == m_channels.end() || (c->type != 0 && c->type != 5))
      return;
    const auto parentId = parent, messageId = message;
    closeDialog();
    closeContext();
    m_dialogKind = "thread-create";
    m_dialogTitle = "Create thread";
    m_dialogText.clear();
    m_dialogAction = api::Interaction{
        .kind = api::InteractionKind::CreateThread,
        .channel = parentId,
        .message = messageId,
        .guild = m_guildId,
        .revision = ++m_dialogGeneration,
        .stateRevision = m_threads.revision()
    };
    m_focusedInput = "dialog";
    m_window.invalidate();
  }
  bool ClientApplication::threadAction(const Hit& hit) {
    switch (hit.action) {
    case Action::ForumSort:
      m_forumNewest = !m_forumNewest;
      updateForumSearch();
      break;
    case Action::ForumTag:
      m_forumTag = m_forumTag == hit.id ? "" : hit.id;
      updateForumSearch();
      break;
    case Action::ForumPostTag:
      m_forumPostTag = m_forumPostTag == hit.id ? "" : hit.id;
      break;
    case Action::ForumRetry:
      loadThreads(true);
      break;
    case Action::ForumMore:
      loadThreads(false, true);
      break;
    case Action::NewForumPost:
      newForumPost();
      break;
    case Action::BrowseThreads:
      showThreads(hit.id);
      break;
    case Action::OpenThread:
      openThread(hit.id);
      break;
    case Action::ThreadParent: {
      const auto c = std::ranges::find(m_channels, hit.id, &api::Channel::id);
      if (c != m_channels.end() && c->isForum())
        selectChannel(hit.id);
      else
        selectChannel(hit.id);
      break;
    }
    case Action::CreateThread:
      createThread(hit.id);
      break;
    case Action::ThreadTab:
      if (forumView() && hit.id.starts_with("forum-")) {
        m_threadTab = hit.id == "forum-archived" ? api::ThreadList::Archived : api::ThreadList::Active;
        updateForumSearch();
        break;
      }
      m_threadTab = hit.id == "active" ? api::ThreadList::Active
          : hit.id == "archived"       ? api::ThreadList::Archived
                                       : api::ThreadList::PrivateArchived;
      m_archiveQuery.reset();
      m_threadBefore.clear();
      m_threadError.clear();
      m_threadMore = false;
      m_scrolling.stop(m_threadScroll);
      m_threadScroll = 0;
      if (m_threadTab != api::ThreadList::Active)
        loadArchivedThreads();
      break;
    case Action::ThreadMore:
      if (m_threadTab == api::ThreadList::Active)
        loadThreads(false, true);
      else
        loadArchivedThreads(true);
      break;
    case Action::ThreadRetry:
      if (m_threadTab == api::ThreadList::Active)
        loadThreads(true);
      else
        loadArchivedThreads(!m_threadBefore.empty());
      break;
    case Action::JoinThread:
    case Action::LeaveThread:
    case Action::ReopenThread: {
      if (!m_threads.find(m_guildId, hit.id) || m_threadPending.contains(hit.id))
        break;
      const auto kind = hit.action == Action::JoinThread ? api::InteractionKind::JoinThread
          : hit.action == Action::LeaveThread            ? api::InteractionKind::LeaveThread
                                                         : api::InteractionKind::UnarchiveThread;
      if (startInteraction(
              {.kind = kind, .channel = hit.id, .guild = m_guildId, .stateRevision = m_threads.revision()}
          ))
        m_threadPending.insert(hit.id);
      break;
    }
    default:
      return false;
    }
    m_window.invalidate();
    return true;
  }
  bool ClientApplication::threadInteraction(const api::Event& event, bool dialog) {
    const auto& a = *event.interaction;
    using Kind = api::InteractionKind;
    if (a.kind != Kind::CreateThread
        && a.kind != Kind::CreateForumPost
        && a.kind != Kind::JoinThread
        && a.kind != Kind::LeaveThread
        && a.kind != Kind::UnarchiveThread)
      return false;
    api::Channel patch;
    std::string previousParent;
    if (const auto* thread = m_threads.find(a.guild, a.channel))
      previousParent = thread->parentId;
    patch.id = a.channel;
    patch.type = 11;
    if (a.kind == Kind::JoinThread || a.kind == Kind::LeaveThread)
      patch.joined = a.kind == Kind::JoinThread;
    else if (!event.channels.empty())
      patch = event.channels.front();
    m_threads.page({api::ThreadList::Single, a.guild, patch.id, {}, 0, a.stateRevision}, {{patch}});
    rebuildThreadChannels();
    if ((a.kind == Kind::CreateThread || a.kind == Kind::CreateForumPost) && dialog) {
      if (a.kind == Kind::CreateForumPost
          && m_emojiCatalog.messageContent(
                 m_drafts[a.channel], a.guild, m_account.premiumType.value_or(0), m_preferences.emojiImageLinks
             ) == a.content)
        m_drafts[a.channel].clear();
      closeDialog();
      if (a.guild == m_guildId && !event.channels.empty())
        openThread(patch.id);
    } else if (
        a.kind == Kind::LeaveThread
        && a.guild == m_guildId
        && a.channel == m_channelId
        && !previousParent.empty()
        && !m_threads.find(a.guild, a.channel)
    ) {
      selectChannel(previousParent);
      m_channelMessages.erase(a.channel);
      m_channelMembers.erase(a.channel);
      std::erase(m_channelCacheOrder, a.channel);
    }
    m_status.clear();
    m_window.invalidate();
    return true;
  }
  bool ClientApplication::canSendToThread() {
    const auto* thread = m_threads.find(m_guildId, m_channelId);
    if (forumView())
      return false;
    if (!m_dmMode && thread && thread->archived.value_or(false)) {
      m_status = "Reopen this thread before sending a message.";
      m_window.invalidate();
      return false;
    }
    return true;
  }
  float ClientApplication::threadStrip(Node* root, const api::Message& message, float x, float y, float width) {
    const auto* thread = m_threads.find(m_guildId, message.id);
    if (!thread && message.thread)
      thread = &*message.thread;
    if (message.deleted
        || m_dmMode
        || (!thread && !message.hasThread)
        || (thread ? thread->id : message.id) == m_channelId)
      return 0;
    if (!root)
      return 48;
    const float cardWidth = std::min(width, 390.0F);
    rectangle(*root, x, y, cardWidth, 40, panel, Style::surfaceRadius);
    glyph(
        *root, x + Style::spaceDense, y + (Style::spaceDense + Style::borderWidth), Style::glyphSizeMedium, "messages",
        accent
    );
    label(
        *root, x + (Style::spaceSection - Style::spaceXxs), y + (Style::spaceXs + Style::borderWidth), cardWidth - 72,
        thread && !thread->name.empty() ? thread->name : "Open thread", Style::fontSizeDescription, foreground,
        FontWeight::Medium
    );
    const std::string detail =
        thread && thread->messageCount ? std::to_string(*thread->messageCount) + " messages" : "View conversation";
    label(
        *root, x + (Style::spaceSection - Style::spaceXxs),
        y + ((Style::spaceXl - Style::spaceXxs) + Style::borderWidth), cardWidth - 72, detail, Style::fontSizeDetail,
        muted
    );
    glyph(
        *root, x + cardWidth - Style::spaceXl, y + (Style::spaceMd + Style::borderWidth), Style::glyphSizeSmall,
        "chevron-right", muted
    );
    m_hits.push_back(
        {conversationLeft() + Style::spaceXl + x, m_messageViewportTop + y, cardWidth, 40, Action::OpenThread,
         thread ? thread->id : message.id}
    );
    return 48;
  }
  void ClientApplication::prepareThreadDemo(const std::string& view) {
    m_demoOverlay = view;
    m_guildChannels[m_guildId] = m_channels;
    using Json = nlohmann::json;
    Json threads = Json::array();
    for (int i = 0; i < 12; ++i)
      threads.push_back(
          {{"id", std::to_string(800 + i)},
           {"type", i == 2 ? 12 : 11},
           {"parent_id", "101"},
           {"name",
            i == 0       ? "A little space for ideas"
                : i == 1 ? "Weekend plans"
                         : "Project discussion " + std::to_string(i)},
           {"message_count", 8 + i * 3},
           {"thread_metadata",
            {{"archived", i >= 10}, {"locked", false}, {"archive_timestamp", "2026-10-17T09:50:00Z"}}}}
      );
    api::ThreadQuery query{api::ThreadList::Active, m_guildId, {}, {}, 0, m_threads.revision()};
    auto page = api::parseThreadPage(Json{{"threads", threads}, {"members", Json::array({{{"id", "809"}}})}}, query);
    m_threads.page(query, page);
    rebuildThreadChannels();
    if (view == "thread")
      openThread("809");
    else
      showThreads("101");
  }
  void ClientApplication::demoThreadStep() {
    auto require = [](bool ok, const char* message) {
      if (!ok)
        throw std::runtime_error(message);
    };
    auto click = [&](Action action, const std::string& id = "") {
      auto hit =
          std::ranges::find_if(m_hits, [&](const auto& h) { return h.action == action && (id.empty() || h.id == id); });
      require(hit != m_hits.end(), "Thread replay action is missing");
      const auto copy = *hit;
      activate(copy);
    };
    const auto step = m_demoInteractionStep++;
    std::clog << "Thread replay step " << step << "\n";
    if (step == 0) {
      require(m_dialogKind == "threads" && m_widgets.contains("thread-scrollbar"), "Thread browser needs scrolling");
      click(Action::OpenThread, "809");
    } else if (step == 1) {
      require(
          m_channelId == "809" && m_dialogKind.empty() && !m_messages.empty(),
          "Opening threads uses the regular conversation flow"
      );
      require(m_lastChannels.at(m_guildId) == "809", "Remember the selected thread");
      click(Action::ThreadParent);
    } else if (step == 2) {
      require(m_channelId == "101", "Back returns to the parent channel");
      m_messages.back().thread = *m_threads.find(m_guildId, "809");
      m_window.invalidate();
    } else if (step == 3) {
      click(Action::OpenThread, "809");
    } else if (step == 4) {
      api::Event event;
      event.operation = api::Operation::ThreadUpdate;
      event.threadUpdates = api::parseThreadUpdates(
          "THREAD_UPDATE", nlohmann::json{{"id", "809"}, {"guild_id", "1"}, {"thread_metadata", {{"archived", true}}}},
          "1"
      );
      threadEvent(event);
    } else if (step == 5) {
      require(
          m_threads.find("1", "809")->archived == true && !canSendToThread(),
          "Archived threads retain history and require reopening"
      );
      require(
          std::ranges::any_of(m_hits, [](const auto& h) { return h.action == Action::ReopenThread; }),
          "Archived threads offer reopening"
      );
      api::Event event;
      event.operation = api::Operation::ThreadUpdate;
      event.threadUpdates =
          api::parseThreadUpdates("THREAD_DELETE", nlohmann::json{{"id", "809"}, {"guild_id", "1"}}, "1");
      threadEvent(event);
    } else if (step == 6) {
      require(m_channelId == "101" && !m_threads.find("1", "809"), "Deleted threads return to their parent");
      click(Action::BrowseThreads);
    } else if (step == 7) {
      click(Action::ThreadTab, "archived");
    } else if (step == 8) {
      require(
          std::ranges::any_of(m_hits, [](const auto& h) { return h.action == Action::OpenThread && h.id == "810"; }),
          "Archived browser lists archived conversations"
      );
      click(Action::CreateThread);
    } else if (step == 9) {
      require(
          m_dialogKind == "thread-create" && m_inputs.contains("dialog"), "Thread creation uses a native name editor"
      );
      closeDialog();
      std::clog << "Offline thread replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
