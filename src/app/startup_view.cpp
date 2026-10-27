#include "app/client_application.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    constexpr auto startupLoadingDeadline = std::chrono::seconds(12);
    constexpr std::string_view startupReplayChannel = "101";
    constexpr std::string_view startupReplayDraft = "Keep this draft through startup preparation";
    constexpr std::string_view startupReplayStorage = "Saved sign-in remains available";
  } // namespace

  void ClientApplication::beginStartupLoading() {
    if (!m_session || m_startupLoadingSession == m_session)
      return;
    m_startupLoadingTimer.stop();
    m_startupRevealTimer.stop();
    m_startupRevealStart.reset();
    m_startupLoadingSession = m_session;
    m_startupLoading = true;
    m_controlInput.cancelPointerCapture();
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_fieldFocused = false;
    const auto session = m_session;
    m_startupLoadingTimer.start(startupLoadingDeadline, [this, session] {
      if (!m_startupLoading || m_startupLoadingSession != session || m_session != session)
        return;
      if (m_status.empty())
        m_status = "Your conversation is still loading. You can continue in the app.";
      finishStartupLoading();
    });
    m_window.invalidate();
  }

  void ClientApplication::finishStartupLoading() {
    m_startupLoadingTimer.stop();
    if (!std::exchange(m_startupLoading, false))
      return;
    if (m_connected && m_startupLoadingSession == m_session && !m_preferences.reducedMotion) {
      m_startupRevealStart = std::chrono::steady_clock::time_point{};
    } else
      finishStartupReveal();
    m_window.invalidate();
  }

  void ClientApplication::finishStartupReveal() {
    m_startupRevealTimer.stop();
    m_startupRevealStart.reset();
    if (m_connected && m_startupLoadingSession == m_session)
      focusChatComposer();
    m_window.invalidate();
  }

  float ClientApplication::startupRevealOpacity() const {
    if (!m_startupRevealStart || m_preferences.reducedMotion)
      return 0;
    if (*m_startupRevealStart == std::chrono::steady_clock::time_point{})
      return 1;
    const float elapsed =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - *m_startupRevealStart).count();
    const float progress = std::clamp(elapsed / Style::animSlow, 0.0F, 1.0F);
    return 1 - progress * progress * (3 - 2 * progress);
  }

  void ClientApplication::updateStartupLoading() {
    if (m_startupRevealStart && (!m_connected || m_startupLoadingSession != m_session || m_preferences.reducedMotion))
      finishStartupReveal();
    if (!m_startupLoading)
      return;
    if (!m_connected || m_startupLoadingSession != m_session) {
      finishStartupLoading();
      return;
    }
    if (m_readyBootstrapPending || m_openThreadQuery)
      return;
    if (!m_channelId.empty()) {
      if (!m_dmMode && m_loadingChannels)
        return;
      if (m_history.error().empty()
          && !m_dmMode
          && memberPaneWidth() > 0
          && (!m_live || !m_membersLoaded || m_memberEpoch != m_chatCacheEpoch))
        return;
      if (forumView()) {
        if (m_threadsLoaded.contains(m_channelId) || m_threadErrors.contains(m_channelId))
          finishStartupLoading();
      } else if ((!m_loadingMessages && m_history.initialized()) || !m_history.error().empty())
        finishStartupLoading();
      return;
    }
    if (m_dmMode) {
      if (m_loadingPrivate || m_dmPreviewLoading)
        return;
      if (!m_dmPage.empty() || !m_status.empty())
        finishStartupLoading();
      return;
    }
    if (!m_loadingChannels && (m_guildChannels.contains(m_guildId) || !m_status.empty()))
      finishStartupLoading();
  }

  void ClientApplication::demoStartupStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    require(m_demo, "Startup preparation replay must remain offline");
    const auto step = m_demoInteractionStep++;
    const auto chat = [&] {
      m_preferences.reducedMotion = true;
      finishStartupLoading();
      finishStartupReveal();
      ++m_session;
      m_connected = true;
      m_connecting = m_restoringLogin = false;
      m_dmMode = false;
      m_dmPage.clear();
      m_dmPreviewId.clear();
      m_dmPreviewLoading = false;
      m_guildId = "1";
      m_channelId = startupReplayChannel;
      m_channels = {{std::string(startupReplayChannel), "general"}};
      m_guildChannels[m_guildId] = m_channels;
      m_readyBootstrapPending = false;
      m_openThreadQuery.reset();
      m_loadingChannels = m_loadingPrivate = false;
      m_loadingMessages = true;
      m_live = false;
      m_showMembers = false;
      m_membersLoaded = false;
      m_status.clear();
      m_history.reset();
      m_messages.clear();
      m_liveChanges.clear();
      m_messageLayouts.clear();
    };
    const auto history = [&] {
      const auto query = m_history.begin(api::HistoryDirection::Latest, m_messages);
      require(query.has_value(), "The startup fixture starts a real initial history query");
      m_loadingMessages = true;
      return api::Event{
          .operation = api::Operation::Messages, .session = m_session, .resource = m_channelId, .history = query
      };
    };
    if (step == 0) {
      chat();
      m_drafts[std::string(startupReplayChannel)] = startupReplayDraft;
      m_loginStorageStatus = startupReplayStorage;
      m_readyBootstrapPending = true;
      beginStartupLoading();
      updateStartupLoading();
      require(
          m_startupLoading && m_startupLoadingTimer.active(),
          "Authenticated startup waits behind the initial snapshot fence with a bounded fallback"
      );
    } else if (step == 1) {
      m_messageScroll = Style::spaceGroup;
      m_scrolling.limit(m_messageScroll, Style::spaceGroup * 3);
      m_scrolling.stop(m_messageScroll);
      m_pointerX = chatLeft() + Style::spaceXl;
      m_pointerY = layout::Window::titlebarHeight + Style::spaceGroup;
      m_window.scroll(Style::spaceGroup);
      require(
          m_messageScroll == Style::spaceGroup && m_scrolling.target(m_messageScroll) == Style::spaceGroup,
          "Wheel input over the loading logo cannot scroll the covered conversation"
      );
      require(!sidebarResizeAt(chatLeft(), m_pointerY), "The covered sidebar has no active resize target");
      require(
          std::ranges::none_of(
              m_hits,
              [](const auto& hit) {
                return hit.action == Action::CancelLogin
                    || hit.action == Action::Channel
                    || hit.action == Action::Reply;
              }
          ),
          "The loading screen covers normal conversation controls and has no sign-in cancel button"
      );
      auto ready = history();
      auto stale = ready;
      ++stale.history->request;
      stale.messages = {{"9000", m_channelId, "Maya", "Stale startup history"}};
      applyHistory(stale);
      require(
          !m_history.initialized() && m_loadingMessages && m_messages.empty(),
          "A stale history generation cannot finish preparation or populate the conversation"
      );
      auto unrelated = ready;
      unrelated.resource = "102";
      applyHistory(unrelated);
      require(!m_history.initialized(), "Another channel's history cannot complete the restored conversation");
      ready.messages = {{"9001", m_channelId, "Maya", "The restored conversation is ready"}};
      applyHistory(ready);
      updateStartupLoading();
      require(
          m_startupLoading && m_history.initialized(),
          "History arriving first still waits for the end of the initial snapshot"
      );
    } else if (step == 2) {
      m_readyBootstrapPending = false;
      updateStartupLoading();
      require(
          !m_startupLoading && !m_startupLoadingTimer.active() && m_messages.size() == 1,
          "Accepted initial history and the completed snapshot reveal the restored conversation"
      );
      beginStartupLoading();
      require(!m_startupLoading, "The same authenticated session never starts a second preparation gate");
      chat();
      beginStartupLoading();
      (void)history();
    } else if (step == 3) {
      api::Event empty{
          .operation = api::Operation::Messages,
          .session = m_session,
          .resource = m_channelId,
          .history = m_history.pending()
      };
      applyHistory(empty);
      updateStartupLoading();
      require(
          !m_startupLoading && m_history.initialized() && m_messages.empty(),
          "A successful empty history page is ready instead of an endless loading screen"
      );
      chat();
      beginStartupLoading();
      auto failed = history();
      failed.error = "Offline startup history failure";
      applyHistory(failed);
      updateStartupLoading();
      require(
          !m_startupLoading && !m_history.busy() && m_history.error() == failed.error,
          "History failure reveals the existing retry state without repeating the request"
      );
    } else if (step == 4) {
      require(
          m_inputs.contains("composer") && m_controlInput.focusedArea() == m_inputs.at("composer")->inputArea(),
          "Revealing the initial conversation focuses its newly mounted native composer"
      );
      chat();
      beginStartupLoading();
      const auto armedSession = m_startupLoadingSession;
      ++m_session;
      updateStartupLoading();
      require(
          !m_startupLoading && !m_startupLoadingTimer.active() && m_startupLoadingSession == armedSession,
          "Replacing the account session retires the old preparation gate and timer"
      );
      m_dmMode = true;
      m_guildId.clear();
      m_channelId.clear();
      m_dmPage = "friends";
      m_privateReady = false;
      m_loadingPrivate = false;
      beginStartupLoading();
      updateStartupLoading();
      require(
          !m_startupLoading && !m_privateReady,
          "An empty private-channel REST result can reveal DM home without a live snapshot flag"
      );
    } else if (step == 5) {
      chat();
      m_dmMode = true;
      m_guildId.clear();
      m_channelId.clear();
      m_loadingPrivate = true;
      beginStartupLoading();
      updateStartupLoading();
      require(m_startupLoading, "Private-channel discovery must settle before revealing an unresolved destination");
      m_loadingPrivate = false;
      m_status = "Offline private-channel discovery failure";
      updateStartupLoading();
      require(!m_startupLoading, "A private-channel error reveals recovery instead of a permanent loading screen");
      chat();
      m_channelId.clear();
      m_channels.clear();
      m_guildChannels[m_guildId].clear();
      beginStartupLoading();
      updateStartupLoading();
      require(!m_startupLoading, "A definitive empty channel snapshot is a valid navigation result");
      chat();
      m_channelId = m_threadParent = "105";
      m_channels = {{"105", "Forum", 15}};
      m_threadsLoaded.erase(m_channelId);
      m_threadErrors.erase(m_channelId);
      beginStartupLoading();
      updateStartupLoading();
      require(m_startupLoading, "A selected forum waits for its own post collection");
    } else if (step == 6) {
      m_threadsLoaded.insert(m_channelId);
      updateStartupLoading();
      require(!m_startupLoading, "An accepted empty forum collection completes preparation");
      chat();
      m_history.restoreLatest(m_messages, true);
      m_loadingMessages = false;
      m_openThreadQuery = api::ThreadQuery{api::ThreadList::Single, m_guildId, "201"};
      beginStartupLoading();
      updateStartupLoading();
      require(m_startupLoading, "A cached fallback conversation cannot hide pending saved-thread resolution");
    } else if (step == 7) {
      m_openThreadQuery.reset();
      m_status = "Offline saved-thread resolution failure";
      updateStartupLoading();
      require(
          !m_startupLoading && m_channelId == startupReplayChannel,
          "A resolved thread failure exposes its prepared fallback conversation"
      );
      chat();
      beginStartupLoading();
    } else if (step == 8) {
      const auto session = m_session;
      const auto account = m_account.id;
      onKey({.symbol = XKB_KEY_Escape});
      require(
          !m_startupLoading
              && !m_startupLoadingTimer.active()
              && m_connected
              && m_session == session
              && m_account.id == account
              && m_loginStorageStatus == startupReplayStorage,
          "Escape reveals the authenticated app without logging out or discarding saved sign-in state"
      );
      beginStartupLoading();
      require(!m_startupLoading, "Explicit reveal cannot automatically reopen preparation");
    } else if (step == 9) {
      require(
          !m_startupLoading
              && !m_startupLoadingTimer.active()
              && m_connected
              && m_drafts[std::string(startupReplayChannel)] == startupReplayDraft
              && m_loginStorageStatus == startupReplayStorage,
          "Preparation preserves drafts and credential storage status across every completion path"
      );
      chat();
      m_window.setUiScale(1);
      m_showMembers = true;
      m_members.clear();
      m_history.restoreLatest(m_messages, true);
      m_loadingMessages = false;
      beginStartupLoading();
      updateStartupLoading();
      require(
          memberPaneWidth() > 0 && m_startupLoading,
          "REST history arriving before the live connection cannot expose a connecting member pane"
      );
    } else if (step == 10) {
      m_live = true;
      updateStartupLoading();
      require(m_startupLoading, "A ready live connection still waits for the visible initial member list");
      api::Event unrelated{.operation = api::Operation::Members, .session = m_session, .resource = "102"};
      require(!applyMemberSnapshot(unrelated), "Another channel cannot complete startup member preparation");
      auto stale = unrelated;
      stale.resource = m_channelId;
      --stale.session;
      require(!applyMemberSnapshot(stale), "A stale account cannot populate the initial member pane");
      auto wrongIdentity = unrelated;
      wrongIdentity.resource = m_channelId;
      wrongIdentity.memberIdentity = api::MemberListIdentity{"unrelated-list", "102", false};
      require(!applyMemberSnapshot(wrongIdentity), "An unrelated member subscription cannot release startup");
      auto failed = unrelated;
      failed.resource = m_channelId;
      failed.error = "Offline member failure";
      require(!applyMemberSnapshot(failed), "Failed member reads never become ready empty snapshots");
      updateStartupLoading();
      require(m_startupLoading, "Rejected member results retain the startup cover");
      api::Event pendingRows{.operation = api::Operation::Members, .session = m_session, .resource = m_channelId};
      pendingRows.memberCount = 1;
      require(
          applyMemberSnapshot(pendingRows), "A member count without its initial rows is an accepted partial update"
      );
      updateStartupLoading();
      require(
          m_startupLoading && !m_membersLoaded,
          "A positive member count without rows cannot masquerade as a ready empty list"
      );
      api::Event members{.operation = api::Operation::Members, .session = m_session, .resource = m_channelId};
      members.members = {{0, {"11", "Maya"}, "online"}};
      members.memberCount = 1;
      require(applyMemberSnapshot(members), "The selected channel accepts its actual member snapshot");
      ++m_chatCacheEpoch;
      updateStartupLoading();
      require(m_startupLoading, "A member snapshot from before connection invalidation cannot release startup");
      members.members = {{0, {"11", "Maya"}, "online"}};
      require(applyMemberSnapshot(members), "A fresh member snapshot replaces the invalidated initial list");
      updateStartupLoading();
      require(
          !m_startupLoading && m_membersLoaded && m_members.size() == 1,
          "Ready history, connection and members reveal together"
      );
    } else if (step == 11) {
      chat();
      m_showMembers = true;
      m_live = true;
      m_members.clear();
      m_history.restoreLatest(m_messages, true);
      m_loadingMessages = false;
      beginStartupLoading();
      updateStartupLoading();
      require(m_startupLoading, "An empty uninitialized member pane still waits for its response");
      api::Event empty{.operation = api::Operation::Members, .session = m_session, .resource = m_channelId};
      require(applyMemberSnapshot(empty), "A successful empty member snapshot is accepted");
      updateStartupLoading();
      require(
          !m_startupLoading && m_membersLoaded && m_members.empty(),
          "An accepted empty member list settles preparation without a loading placeholder"
      );
    } else if (step == 12) {
      bool emptyLabel = false, loadingLabel = false;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text) {
          const auto& text = static_cast<const TextNode&>(node).text();
          emptyLabel |= text == "No members to display";
          loadingLabel |= text == "Loading members…" || text == "Connecting…";
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(emptyLabel && !loadingLabel, "The revealed empty member pane renders its settled empty state");
      chat();
      m_history.restoreLatest(m_messages, true);
      m_loadingMessages = false;
      beginStartupLoading();
      updateStartupLoading();
      require(!m_startupLoading, "A hidden member pane does not delay the initial conversation");
      chat();
      m_showMembers = true;
      m_history.restoreLatest(m_messages, true);
      m_loadingMessages = false;
      beginStartupLoading();
      updateStartupLoading();
      require(
          m_startupLoading && m_startupLoadingTimer.active(), "Unavailable members retain the bounded startup deadline"
      );
      finishStartupLoading();
      require(
          !m_startupLoading && !m_startupLoadingTimer.active(),
          "Explicit or deadline reveal remains available while members are missing"
      );
    } else if (step == 13) {
      require(
          m_connected && !startupCovered() && m_drafts[std::string(startupReplayChannel)] == startupReplayDraft,
          "Member preparation and fallback preserve the authenticated account and draft"
      );
      std::clog << "Offline startup preparation replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
