#include "app/client_application.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace app {
  bool ClientApplication::canPreloadChat(const std::string& id) const {
    if (!api::validSnowflake(id) || id == m_channelId || m_deletedChannels.contains(id))
      return false;
    if (const auto dm = std::ranges::find(m_privateChannels, id, &api::Channel::id); dm != m_privateChannels.end())
      return (dm->type == 1 || dm->type == 3) && !dm->isMessageRequest();
    if (m_dmMode)
      return false;
    const auto channel = std::ranges::find(m_channels, id, &api::Channel::id);
    if (channel == m_channels.end()
        || (channel->type != 0
            && channel->type != 2
            && channel->type != 5
            && channel->type != 13
            && !channel->isThread())
        || (channel->type == 12 && !channel->joined.value_or(false)))
      return false;
    return channelAllows(m_guildId, id, api::permission::readMessageHistory);
  }

  std::string ClientApplication::hoveredPreloadChat() const {
    if (!m_connected
        || (!m_demo && (!m_live || !m_window.active()))
        || (m_demo && m_demoOverlay != "chat-preload-cycle")
        || settingsVisible()
        || m_inboxOpen
        || !m_dialogKind.empty()
        || m_contextOpen
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || m_selectRequest
        || m_fontPickerOpen
        || m_completionQuery
        || !m_viewImage.empty()
        || m_videoExpanded
        || m_resizingSidebar
        || m_dragDraft
        || m_dragMessage
        || m_dragImage
        || m_loadingMessages
        || m_sending)
      return {};
    // Use the foremost native hit, so tags, pin buttons and covered rows cannot
    // accidentally preload the conversation beneath them.
    for (auto hit = m_hits.rbegin(); hit != m_hits.rend(); ++hit)
      if (hit->contains(m_pointerX, m_pointerY)) {
        if ((hit->action == Action::Channel || hit->action == Action::DirectMessage)
            && canPreloadChat(hit->id)
            && !m_channelMessages.contains(hit->id))
          return hit->id;
        break;
      }
    return {};
  }

  void ClientApplication::updateChatPreload() {
    const auto id = hoveredPreloadChat();
    if (id == m_chatPreloadHover)
      return; // One attempt per stable hover, including after a failed request.
    m_chatPreloadTimer.stop();
    m_chatPreloadHover = id;
    if (id.empty() || !m_chatPreloadChannel.empty())
      return;
    const auto now = std::chrono::steady_clock::now();
    const auto delay = std::max(
        std::chrono::milliseconds(120), std::chrono::ceil<std::chrono::milliseconds>(m_chatPreloadAfter - now)
    );
    m_chatPreloadTimer.start(delay, [this, id] {
      if (hoveredPreloadChat() == id && m_chatPreloadHover == id)
        startChatPreload();
    });
  }

  void ClientApplication::startChatPreload() {
    m_chatPreloadTimer.stop();
    const auto id = hoveredPreloadChat();
    const auto now = std::chrono::steady_clock::now();
    if (id.empty() || id != m_chatPreloadHover || !m_chatPreloadChannel.empty() || now < m_chatPreloadAfter)
      return;
    m_chatPreloadHistory.reset();
    const auto query = m_chatPreloadHistory.begin(api::HistoryDirection::Latest, {}, false, true);
    if (!query)
      return;
    m_chatPreloadLive.clear();
    m_chatPreloadReactions.clear();
    m_chatPreloadEpoch = m_chatCacheEpoch;
    m_chatPreloadAfter = now + std::chrono::seconds(1);
    if (m_demo || m_client.messages(id, *query))
      m_chatPreloadChannel = id;
    else {
      m_chatPreloadHistory.reset();
      m_chatPreloadAfter = now + std::chrono::seconds(10);
    }
  }

  void ClientApplication::clearChatPreload() {
    m_chatPreloadTimer.stop();
    m_chatPreloadHover.clear();
    m_chatPreloadChannel.clear();
    m_chatPreloadHistory.reset();
    m_chatPreloadLive.clear();
    m_chatPreloadReactions.clear();
  }

  bool ClientApplication::adoptChatPreload(const std::string& id) {
    if (m_chatPreloadChannel != id || !m_chatPreloadHistory.pending())
      return false;
    const auto source = *m_chatPreloadHistory.pending();
    if (!m_demo && !m_client.promotePreload(id, source))
      return false;
    const auto visible = m_history.adoptPending(m_chatPreloadHistory);
    if (!visible)
      return false;
    m_liveChanges = std::move(m_chatPreloadLive);
    m_historyEpoch = m_chatPreloadEpoch;
    auto reactions = std::move(m_chatPreloadReactions);
    clearChatPreload();
    m_adoptedChatPreload = AdoptedChatPreload{id, source, *visible, std::move(reactions)};
    m_loadingMessages = true;
    return true;
  }

  bool ClientApplication::chatPreloadEvent(api::Event& event) {
    if (event.operation != api::Operation::Messages || !event.history || !event.history->preload)
      return false;
    if (m_adoptedChatPreload
        && event.session == m_session
        && event.resource == m_adoptedChatPreload->channel
        && *event.history == m_adoptedChatPreload->source) {
      if (event.resource != m_channelId || !m_history.accepts(m_adoptedChatPreload->visible)) {
        m_adoptedChatPreload.reset();
        return true;
      }
      event.history = m_adoptedChatPreload->visible;
      return false; // Continue through the normal visible-history merge/error path.
    }
    // Consume stale speculative replies here, before normal history, request
    // previews or status/error handling can affect the visible conversation.
    if (event.session != m_session
        || event.resource != m_chatPreloadChannel
        || !m_chatPreloadHistory.accepts(*event.history))
      return true;
    const auto id = m_chatPreloadChannel;
    if (event.error.empty() && canPreloadChat(id)) {
      std::vector<api::Message> rows;
      if (m_chatPreloadHistory.apply(*event.history, rows, std::move(event.messages), m_chatPreloadLive)) {
        for (const auto& change : m_chatPreloadReactions)
          for (auto& row : rows)
            if (row.id == change.message)
              api::applyReaction(row, change, m_account.id);
        std::erase_if(rows, [&](const auto& row) {
          return row.channelId != id || (row.flags & 64) || m_messageLog.known(id, row.id);
        });
        const bool trimmed = rows.size() > 200;
        if (trimmed)
          rows.erase(rows.begin(), rows.end() - 200);
        m_channelMessages[id] = std::move(rows);
        m_channelHistoryState[id] = {
            ChatCacheState::Clock::now(), m_chatPreloadEpoch, !trimmed && m_chatPreloadHistory.olderEnd()
        };
        touchChannelCache(id);
      }
    } else if (!event.error.empty())
      m_chatPreloadAfter = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    clearChatPreload();
    m_chatPreloadHover = id; // Require a new hover before retrying this chat.
    return true;
  }

  void ClientApplication::demoChatPreloadStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto hover = [&](const std::string& id) {
      const auto row =
          std::ranges::find_if(m_hits, [&](const auto& hit) { return hit.action == Action::Channel && hit.id == id; });
      require(row != m_hits.end(), "Preload replay requires a visible conversation row");
      m_window.motion(
          row->x + (Style::cardPadding + Style::borderWidth),
          row->y
              + (Style::cardPadding + Style::borderWidth)
              + m_window.titlebarHeight()
              - layout::Window::titlebarHeight
      );
      updateChatPreload();
    };
    const auto result = [&] {
      api::Event event{.operation = api::Operation::Messages, .session = m_session, .resource = m_chatPreloadChannel};
      event.history = m_chatPreloadHistory.pending();
      return event;
    };
    const auto step = m_demoInteractionStep++;
    const auto guildTarget = m_channels.size() > 1 ? m_channels[1].id : std::string{};
    if (step == 0) {
      hover(guildTarget);
      require(m_chatPreloadTimer.active() && m_chatPreloadChannel.empty(), "Hover waits before starting a request");
      m_window.pointerLeft();
      require(!m_chatPreloadTimer.active(), "Passing over a row cancels its dwell timer");
      hover(guildTarget);
    } else if (step == 1) {
      m_chatPreloadAfter = {};
      startChatPreload();
      require(
          m_chatPreloadChannel == guildTarget && m_chatPreloadHistory.pending()->preload,
          "A stable channel hover starts one background history request"
      );
      const auto query = m_chatPreloadHistory.pending();
      startChatPreload();
      require(m_chatPreloadHistory.pending() == query, "Repeated hover cannot duplicate an in-flight request");
    } else if (step == 2) {
      const auto current = m_channelId, draft = m_drafts[current], status = m_status;
      const auto localRead = m_localRead;
      updateMessage(guildTarget, "9801", {.content = "Edited while preloading"});
      deleteMessages(guildTarget, {"9802"});
      m_chatPreloadLive["9803"] = {"9803", guildTarget, "Maya", "Arrived while preloading"};
      auto event = result();
      event.messages = {
          {"9801", guildTarget, "Maya", "Older response"}, {"9802", guildTarget, "Maya", "Deleted while preloading"}
      };
      require(
          chatPreloadEvent(event)
              && m_channelMessages.at(guildTarget).size() == 2
              && m_channelMessages.at(guildTarget).front().content == "Edited while preloading"
              && m_channelMessages.at(guildTarget).back().id == "9803",
          "Preloads merge live creates, partial edits and deletion tombstones"
      );
      require(
          m_channelId == current && m_drafts[current] == draft && m_status == status && m_localRead == localRead,
          "Hover must not navigate, alter drafts/status or acknowledge messages"
      );
      hover(guildTarget);
      require(!m_chatPreloadTimer.active(), "Cached conversations do not request another preload");
    } else if (step == 3) {
      m_drafts[guildTarget] = "A retained per-chat draft";
      selectChannel(guildTarget);
      require(
          m_messages.size() == 2
              && m_messages.front().content == "Edited while preloading"
              && m_drafts[guildTarget] == "A retained per-chat draft",
          "Selecting a warmed channel displays cached messages and preserves its draft"
      );
      m_privateChannels = {
          {.id = "701", .name = "Maya", .type = 1},
          {.id = "702", .name = "Noah", .type = 3},
          {.id = "703", .name = "Spam", .type = 1, .spam = true}
      };
      showDirectMessages(true);
    } else if (step == 4) {
      require(!canPreloadChat("703"), "Message requests and spam must not be preloaded");
      hover("701");
      m_chatPreloadAfter = {};
      startChatPreload();
      auto event = result();
      event.messages = {{"9810", "701", "Maya", "A warmed direct message"}};
      m_unreadDMs.insert("701");
      require(
          chatPreloadEvent(event)
              && m_channelMessages.at("701").size() == 1
              && m_channelId.empty()
              && m_unreadDMs.contains("701"),
          "DM hover fills the cache while preserving the home page and unread badge"
      );
    } else if (step == 5) {
      hover("702");
      m_chatPreloadAfter = {};
      startChatPreload();
      auto event = result();
      ++event.session;
      require(chatPreloadEvent(event) && m_chatPreloadChannel == "702", "Other-session replies cannot fill caches");
      --event.session;
      clearChatPreload();
      require(chatPreloadEvent(event) && !m_channelMessages.contains("702"), "Cancelled preload replies stay ignored");
      hover("702");
      m_chatPreloadAfter = {};
      startChatPreload();
      event = result();
      const auto status = m_status;
      event.error = "Synthetic rate limit";
      require(
          chatPreloadEvent(event) && m_status == status && !m_channelMessages.contains("702"),
          "Speculative failures stay silent and do not cache an empty conversation"
      );
      updateChatPreload();
      startChatPreload();
      require(
          m_chatPreloadChannel.empty() && !m_chatPreloadTimer.active(),
          "A failed stable hover never schedules an automatic retry"
      );
    } else if (step == 6) {
      clearChatPreload();
      for (int i = 0; i < 30; ++i) {
        const auto id = std::to_string(800 + i);
        m_channelMessages[id] = {};
        touchChannelCache(id);
      }
      require(
          m_channelCacheOrder.size() == 24 && m_channelMessages.size() == 24 && !m_channelMessages.contains("800"),
          "Preloading shares the bounded navigation LRU"
      );
      m_channelMessages["701"] = {{"9810", "701", "Maya", "A warmed direct message"}};
      touchChannelCache("701");
      m_drafts["701"] = "DM draft";
      selectChannel("701");
    } else if (step == 7) {
      require(
          m_messages.size() == 1 && m_messages.front().id == "9810" && m_drafts["701"] == "DM draft",
          "Selecting a warmed DM uses the same cache as a warmed guild channel"
      );
      clearChatPreload();
      hover("702");
      m_chatPreloadAfter = {};
      startChatPreload();
      auto event = result();
      event.messages = {{"9820", "702", "Noah", "Before the click"}};
      updateMessage(
          "702", "9820", {.content = "Edited while hovering", .reactions = std::vector<api::Reaction>{{{}, "👍", 2}}}
      );
      const auto reaction = [&](const std::string& message) {
        return api::ReactionUpdate{.channel = "702", .message = message, .user = m_account.id, .emoji = {.name = "👍"}};
      };
      m_chatPreloadReactions.push_back(reaction("9820"));
      api::Message created{"9821", "702", "Noah", "Created while hovering"};
      created.reactions = {{{}, "👍", 4}};
      m_chatPreloadLive[created.id] = std::move(created);
      m_chatPreloadReactions.push_back(reaction("9821"));
      selectChannel("702");
      require(
          m_adoptedChatPreload && m_loadingMessages && m_history.busy() && m_chatPreloadChannel.empty(),
          "A click promotes the existing preload instead of starting a second request"
      );
      updateMessage("702", "9820", {.reactions = std::vector<api::Reaction>{{{}, "👍", 10}}});
      m_adoptedChatPreload->reactions.push_back(reaction("9820"));
      m_adoptedChatPreload->reactions.push_back(reaction("9821"));
      require(
          !chatPreloadEvent(event) && event.history == m_history.pending(),
          "The original preload response is routed to the clicked conversation"
      );
      applyHistory(event);
      require(
          m_messages.size() == 2
              && m_messages.front().content == "Edited while hovering"
              && !m_loadingMessages
              && m_history.initialized(),
          "Adopted history keeps live patches and completes the visible loading state"
      );
      require(
          m_messages.front().reactions.size() == 1
              && m_messages.front().reactions.front().count == 11
              && m_messages.back().id == "9821"
              && m_messages.back().reactions.size() == 1
              && m_messages.back().reactions.front().count == 6,
          "Adopted reactions follow full snapshots and preserve deltas before and after the click"
      );
    } else if (step == 8) {
      selectChannel("701");
      selectChannel("702");
      require(
          m_history.initialized() && !m_history.busy() && !m_loadingMessages && m_messages.front().id == "9820",
          "Revisiting a current resident page opens it without another loading request"
      );
      ++m_chatCacheEpoch;
      require(!restoreCachedHistory("702"), "A reconnect makes resident pages require catch-up");
    } else {
      m_channelMessages.erase("701");
      m_channelHistoryState.erase("701");
      hover("701");
      m_chatPreloadAfter = {};
      startChatPreload();
      auto event = result();
      selectChannel("701");
      event.error = "Synthetic visible failure";
      require(!chatPreloadEvent(event), "An adopted failure reaches the visible history handler");
      applyHistory(event);
      require(
          !m_loadingMessages && !m_history.busy() && m_history.error() == event.error,
          "An adopted failure releases loading and offers the normal history error/retry state"
      );
    }
    m_window.invalidate();
  }
} // namespace app
