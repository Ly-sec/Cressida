#include "app/client_application.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace app {
  void
  ClientApplication::updateMessage(const std::string& channel, const std::string& id, const api::MessagePatch& patch) {
    if (m_dismissedEphemeral.contains(id) || m_messageLog.known(channel, id))
      return;
    const api::Message* prior = nullptr;
    if (channel == m_channelId) {
      const auto found = std::ranges::find(m_messages, id, &api::Message::id);
      if (found != m_messages.end())
        prior = &*found;
    }
    if (!prior)
      if (const auto cache = m_channelMessages.find(channel); cache != m_channelMessages.end()) {
        const auto found = std::ranges::find(cache->second, id, &api::Message::id);
        if (found != cache->second.end())
          prior = &*found;
      }
    if (prior) {
      auto updated = *prior;
      patch.apply(updated);
      if (MessageLog::stale(*prior, updated))
        return;
      logMessageEdit(*prior, updated);
    }
    if (auto preview = m_forumFirstMessages.find(channel);
        preview != m_forumFirstMessages.end() && preview->second.id == id)
      patch.apply(preview->second);
    if (channel == m_channelId) {
      m_history.changed(id, patch);
      if (!m_history.busy())
        m_loadingMessages = false;
    }
    if (channel == m_chatPreloadChannel) {
      if (patch.reactions)
        std::erase_if(m_chatPreloadReactions, [&](const auto& change) { return change.message == id; });
      m_chatPreloadHistory.changed(id, patch);
      if (!m_chatPreloadHistory.busy())
        clearChatPreload();
    }
    if (patch.reactions)
      invalidateReactors(channel, id);
    if (patch.reactions && m_adoptedChatPreload && m_adoptedChatPreload->channel == channel)
      std::erase_if(m_adoptedChatPreload->reactions, [&](const auto& change) { return change.message == id; });
    if (auto cached = m_channelMessages.find(channel); cached != m_channelMessages.end())
      for (auto& message : cached->second)
        if (message.id == id)
          patch.apply(message);
    if (channel != m_channelId)
      return;
    for (auto& message : m_messages)
      if (message.id == id) {
        patch.apply(message);
        m_messageLayouts.erase(message.id);
        m_liveChanges[message.id] = message;
      }
    m_window.invalidate();
  }
  void ClientApplication::logMessageEdit(const api::Message& before, const api::Message& after) {
    if (m_preferences.logEditHistory && m_messageLog.recordEdit(before.channelId, before, after))
      m_messageLayouts.erase(before.id);
  }
  void ClientApplication::deleteMessages(const std::string& channel, const std::vector<std::string>& ids) {
    if (channel.empty() || ids.empty())
      return;
    const auto deleted = [&](const auto& message) { return std::ranges::find(ids, message.id) != ids.end(); };
    if (m_preferences.logDeletedMessages) {
      // Prefer current rows: they may have newer edits than the navigation cache.
      if (channel == m_channelId)
        for (const auto& message : m_messages)
          if (deleted(message))
            m_messageLog.capture(channel, message);
      if (const auto cache = m_channelMessages.find(channel); cache != m_channelMessages.end())
        for (const auto& message : cache->second)
          if (deleted(message))
            m_messageLog.capture(channel, message);
    }
    if (channel == m_chatPreloadChannel) {
      for (const auto& id : ids)
        m_chatPreloadLive[id] = std::nullopt;
      if (m_chatPreloadLive.size() > 4096)
        clearChatPreload();
    }
    m_messageLog.removed(channel, ids);
    const auto invalidateReplies = [&](auto& rows) {
      for (auto& message : rows)
        if (message.reply
            && (message.reply->channel.empty() ? message.channelId : message.reply->channel) == channel
            && std::ranges::find(ids, message.reply->message) != ids.end()) {
          message.reply->deleted = true;
          message.reply->available = true;
          message.reply->content.clear();
        }
    };
    invalidateReplies(m_messages);
    for (auto& [id, rows] : m_channelMessages)
      invalidateReplies(rows);
    if (const auto preview = m_forumFirstMessages.find(channel);
        preview != m_forumFirstMessages.end() && deleted(preview->second))
      m_forumFirstMessages.erase(preview);
    for (const auto& id : ids) {
      invalidateReactors(channel, id);
      if (channel == m_channelId)
        m_liveChanges[id] = std::nullopt;
      std::erase_if(m_componentStates, [&](const auto& item) { return item.first.starts_with(id + ":"); });
      std::erase_if(m_replies, [&](const auto& item) {
        return item.second.channel == channel && item.second.message == id;
      });
      if (m_dialogAction && m_dialogAction->channel == channel && m_dialogAction->message == id)
        closeDialog();
      if (m_messageEdit && m_messageEdit->channel == channel && m_messageEdit->message == id)
        cancelMessageEdit();
      if (m_contextChannel == channel && m_contextMessage == id)
        closeContext();
      if (m_reactionChannel == channel && m_reactionMessage == id) {
        m_reactionMessage.clear();
        m_reactionChannel.clear();
        m_emojiMenu = false;
      }
    }
    // Keep current and navigation-cache copies consistent with saved snapshots.
    for (auto& [id, rows] : m_channelMessages)
      m_messageLog.filter(id, rows);
    m_messageLog.filter(m_channelId, m_messages);
    // Rich layouts and selection strings own content too. Release discarded copies.
    m_messageLayouts.clear();
    m_messageText.clear();
    m_textAreas.clear();
    m_selectedMessage.clear();
    m_selectedText.clear();
    m_chatEdit = {};
    m_window.invalidate();
  }

  void ClientApplication::clearMessageLog(bool deletions, bool edits) {
    if (deletions)
      m_messageLog.clearDeleted();
    if (edits)
      m_messageLog.clearEdits();
    for (auto& [id, rows] : m_channelMessages)
      m_messageLog.filter(id, rows);
    m_messageLog.filter(m_channelId, m_messages);
    m_messageLayouts.clear();
    m_messageText.clear();
    m_textAreas.clear();
    m_selectedMessage.clear();
    m_selectedText.clear();
    m_chatEdit = {};
    closeContext();
    m_window.invalidate();
  }

  void
  ClientApplication::forgetMessageLog(const std::string& channel, const std::string& id, bool deletions, bool edits) {
    m_messageLog.forget(channel, id, deletions, edits);
    if (deletions)
      std::erase_if(m_componentStates, [&](const auto& item) { return item.first.starts_with(id + ":"); });
    if (const auto cache = m_channelMessages.find(channel); cache != m_channelMessages.end())
      m_messageLog.filter(channel, cache->second);
    if (channel == m_channelId) {
      m_messageLog.filter(m_channelId, m_messages);
      m_messageLayouts.erase(id);
      if (m_selectedMessage == id) {
        m_selectedMessage.clear();
        m_selectedText.clear();
      }
      if (m_messageEdit && m_messageEdit->channel == channel && m_messageEdit->message == id)
        cancelMessageEdit();
    }
  }

  void ClientApplication::demoDeletedStep() {
    const auto require = [](bool ok, const char* reason) {
      if (!ok)
        throw std::runtime_error(reason);
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(m_messageLog.size() == 2 && m_messages.size() == 4, "Single/bulk deletion retains loaded rows");
      int labels = 0;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text && static_cast<const TextNode&>(node).text().contains("Deleted"))
          ++labels;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(
          m_messageLog.edits(m_channelId, "9503").size() == 2 && m_messageLayouts.at("9503").layout.historyHeight > 0,
          "Successive edits retain and display both previous versions"
      );
      require(labels == 2, "Every retained message needs an explicit Deleted label");
      require(!markChannelRead() || m_localRead[m_channelId] == "9503", "Deleted rows cannot be ACK targets");
      for (const auto& id : {"9502", "9504"}) {
        m_pointerX = chatLeft() + 140;
        m_pointerY = m_messageTops.at(id) + 10;
        openContext(m_pointerX, m_pointerY);
        require(m_contextOpen, "Deleted message retains its context menu");
        for (const auto& entry : m_contextEntries)
          require(
              entry.id != static_cast<int>(Action::Reply)
                  && entry.id != static_cast<int>(Action::EditMessage)
                  && entry.id != static_cast<int>(Action::DeleteMessage)
                  && entry.id != static_cast<int>(Action::AddReaction)
                  && entry.id != static_cast<int>(Action::CreateThread),
              "Deleted message menu must be read-only"
          );
        closeContext();
      }
      const auto saved = m_messages;
      m_messages.clear();
      m_messageLog.merge(m_channelId, m_messages, true, true);
      require(m_messages.size() == 2 && m_messages.front().deleted, "Saved rows survive a history refresh");
      m_messages = saved;
      m_channelMessages[m_channelId] = saved;
      activate({0, 0, 0, 0, Action::Preference, "log_deleted_messages"});
      require(
          !m_preferences.logDeletedMessages
              && m_preferences.logEditHistory
              && m_messageLog.edits(m_channelId, "9503").size() == 2,
          "Disabling deletion logging preserves edit history and its preference"
      );
      require(
          m_messages.size() == 2 && m_channelMessages[m_channelId].size() == 2 && !m_messageLog.size(),
          "Disabling deletions removes only deleted rows from current/cache/saved content"
      );
    } else if (step == 1) {
      require(
          m_messageLayouts.at("9503").layout.historyHeight > 0,
          "Edit history stays visible while deletion logging is off"
      );
      updateMessage(m_channelId, "9503", {.content = "An edit while only history logging is enabled"});
      require(m_messageLog.edits(m_channelId, "9503").size() == 3, "History-only mode captures new edits");
      activate({0, 0, 0, 0, Action::Preference, "log_deleted_messages"});
      deleteMessages(m_channelId, {"9502"});
      require(!m_messageLog.size(), "Re-enabling cannot recover previously deleted content");
      deleteMessages(m_channelId, {"9501"});
      activate({0, 0, 0, 0, Action::Preference, "log_edit_history"});
      require(
          m_preferences.logDeletedMessages
              && !m_preferences.logEditHistory
              && m_messageLog.size() == 1
              && m_messageLog.edits(m_channelId, "9503").empty(),
          "Disabling edit history preserves deleted snapshots and their preference"
      );
    } else if (step == 2) {
      require(
          m_messageLayouts.at("9503").layout.historyHeight == 0 && m_messages.front().deleted,
          "Deletion-only mode keeps deleted rows visible without previous versions"
      );
      updateMessage(m_channelId, "9503", {.content = "An edit while history logging is disabled"});
      require(m_messageLog.edits(m_channelId, "9503").empty(), "Deletion-only mode does not capture edits");
      activate({0, 0, 0, 0, Action::Preference, "log_edit_history"});
      updateMessage(m_channelId, "9503", {.content = "History resumes from the current version"});
      require(
          m_messageLog.edits(m_channelId, "9503").size() == 1,
          "Re-enabling edit logging starts fresh without restoring cleared versions"
      );
      // Deletions in a cached background channel use the same capture path.
      m_channelMessages["999"] = {{"9601", "999", "Maya", "Cached channel message"}};
      updateMessage("999", "9601", {.content = "An edited background message"});
      require(m_messageLog.edits("999", "9601").size() == 1, "Cached background edits are captured");
      deleteMessages("999", {"9601"});
      require(m_channelMessages["999"].front().deleted, "Background channel deletion is retained");
      m_messageLog.reset();
      clearMessageLog();
      require(m_channelMessages["999"].empty(), "Session reset clears retained background rows");
      std::cout << "Message logger replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
