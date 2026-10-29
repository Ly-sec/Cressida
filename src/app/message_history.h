#pragma once

#include "api/history.h"

#include <algorithm>
#include <map>
#include <optional>
#include <span>

namespace app {
  class MessageHistory {
  public:
    static constexpr std::size_t windowLimit = 1000;
    void reset() {
      m_pending.reset();
      m_olderEnd = false;
      m_newerEnd = true;
      m_initialized = false;
      m_error.clear();
      m_failedCursor.clear();
      m_olderCursor.clear();
      m_newerCursor.clear();
      m_patches.clear();
      ++m_serial;
    }
    bool atLatest() const { return m_newerEnd; }
    bool busy() const { return m_pending.has_value(); }
    bool initialized() const { return m_initialized; }
    void restoreLatest(std::span<const api::Message> messages, bool olderEnd) {
      reset();
      m_initialized = true;
      m_olderEnd = olderEnd;
      if (!messages.empty()) {
        m_olderCursor = messages.front().id;
        m_newerCursor = messages.back().id;
      }
    }
    bool olderEnd() const { return m_olderEnd; }
    const auto& pending() const { return m_pending; }
    const std::string& error() const { return m_error; }
    api::HistoryDirection failedDirection() const { return m_failed; }
    void cancelPending() {
      m_pending.reset();
      m_patches.clear();
    }
    std::optional<api::HistoryQuery> begin(
        api::HistoryDirection direction, std::span<const api::Message> messages, bool retry = false,
        bool preload = false
    ) {
      using enum api::HistoryDirection;
      if (preload && direction != Latest)
        return {};
      if (direction == Around)
        return retry ? beginAround(m_failedCursor) : std::nullopt;
      if (direction != Latest
          && (busy()
              || (!retry && !m_error.empty())
              || messages.empty()
              || (direction == Older ? m_olderEnd : m_newerEnd)))
        return {};
      api::HistoryQuery query{direction, {}, ++m_serial, preload};
      if (direction != Latest)
        query.cursor = direction == Older ? (m_olderCursor.empty() ? messages.front().id : m_olderCursor)
                                          : (m_newerCursor.empty() ? messages.back().id : m_newerCursor);
      m_pending = query;
      m_error.clear();
      m_patches.clear();
      return query;
    }
    std::optional<api::HistoryQuery> beginAround(const std::string& message) {
      if (!api::validSnowflake(message))
        return {};
      m_pending = api::HistoryQuery{api::HistoryDirection::Around, message, ++m_serial};
      m_error.clear();
      m_patches.clear();
      return m_pending;
    }
    bool accepts(const api::HistoryQuery& query) const { return m_pending == query; }
    std::optional<api::HistoryQuery> adoptPending(MessageHistory& preload) {
      if (!preload.m_pending
          || !preload.m_pending->preload
          || preload.m_pending->direction != api::HistoryDirection::Latest)
        return {};
      const auto query = begin(api::HistoryDirection::Latest, {});
      m_patches = std::move(preload.m_patches);
      preload.cancelPending();
      return query;
    }
    void fail(const api::HistoryQuery& query, std::string error) {
      if (!accepts(query))
        return;
      m_failed = query.direction;
      m_failedCursor = query.cursor;
      m_pending.reset();
      m_error = std::move(error);
      m_patches.clear();
    }
    void changed(const std::string& id, const api::MessagePatch& patch) {
      if (!m_pending)
        return;
      if (!m_patches.contains(id) && m_patches.size() >= 4096) {
        fail(*m_pending, "Message history changed too much while loading. Try again.");
        return;
      }
      auto& combined = m_patches[id];
      auto merge = [](auto& target, const auto& source) {
        if (source)
          target = source;
      };
      merge(combined.content, patch.content);
      merge(combined.editedTimestamp, patch.editedTimestamp);
      merge(combined.buttons, patch.buttons);
      merge(combined.applicationId, patch.applicationId);
      merge(combined.authorBot, patch.authorBot);
      merge(combined.flags, patch.flags);
      merge(combined.type, patch.type);
      merge(combined.webhookId, patch.webhookId);
      merge(combined.thread, patch.thread);
      merge(combined.hasThread, patch.hasThread);
      merge(combined.attachments, patch.attachments);
      merge(combined.stickers, patch.stickers);
      merge(combined.embeds, patch.embeds);
      merge(combined.mentions, patch.mentions);
      merge(combined.mentionRoles, patch.mentionRoles);
      merge(combined.mentionEveryone, patch.mentionEveryone);
      merge(combined.reply, patch.reply);
      merge(combined.forwarded, patch.forwarded);
      merge(combined.reactions, patch.reactions);
    }
    // Apply only contiguous pages. Existing live patches/tombstones win over
    // REST, including messages changed while the request was in flight.
    bool apply(
        const api::HistoryQuery& query, std::vector<api::Message>& messages, std::vector<api::Message> page,
        const std::map<std::string, std::optional<api::Message>>& live
    ) {
      using enum api::HistoryDirection;
      if (!accepts(query))
        return false;
      const auto rawCount = page.size();
      std::erase_if(page, [&](const auto& message) {
        return !api::validSnowflake(message.id)
            || (query.direction == Older && !less(message.id, query.cursor))
            || (query.direction == Newer && !less(query.cursor, message.id));
      });
      const bool advanced = !page.empty();
      sort(page);
      if (!page.empty()) {
        if (query.direction == Older)
          m_olderCursor = page.front().id;
        else if (query.direction == Newer)
          m_newerCursor = page.back().id;
      }
      if (query.direction != Latest && rawCount && !advanced) {
        fail(query, "Message history did not advance. Try again.");
        return false;
      }
      if (query.direction == Around) {
        messages.clear();
        m_olderEnd = page.empty();
        m_newerEnd = page.empty();
        m_olderCursor = page.empty() ? "" : page.front().id;
        m_newerCursor = page.empty() ? "" : page.back().id;
      } else if (query.direction == Latest) {
        // Keep a contiguous cached prefix only when the latest page overlaps it.
        bool overlaps = false;
        for (const auto& message : page)
          if (std::ranges::find(messages, message.id, &api::Message::id) != messages.end()) {
            overlaps = true;
            break;
          }
        if (!m_initialized || !overlaps) {
          messages.clear();
          m_olderEnd = rawCount < api::messagePageSize;
        }
        m_newerEnd = true;
      } else if (query.direction == Older)
        m_olderEnd = rawCount < api::messagePageSize;
      else
        m_newerEnd = rawCount < api::messagePageSize;
      for (auto& message : page) {
        if (const auto changed = live.find(message.id); changed != live.end()) {
          if (!changed->second)
            continue;
          message = *changed->second;
        }
        const auto at = std::ranges::find(messages, message.id, &api::Message::id);
        if (at == messages.end())
          messages.push_back(std::move(message));
        else
          *at = std::move(message);
      }
      if (query.direction == Latest || (query.direction == Newer && m_newerEnd))
        for (const auto& [id, changed] : live) {
          if (!changed)
            std::erase_if(messages, [&](const auto& message) { return message.id == id; });
          else if (messages.empty() || !less(id, messages.front().id)) {
            const auto at = std::ranges::find(messages, id, &api::Message::id);
            if (at == messages.end())
              messages.push_back(*changed);
            else
              *at = *changed;
          }
        }
      for (auto& message : messages)
        if (const auto patch = m_patches.find(message.id); patch != m_patches.end())
          patch->second.apply(message);
      sort(messages);
      trim(messages, query.direction == Older);
      if (query.direction == Latest && !messages.empty()) {
        m_olderCursor = messages.front().id;
        m_newerCursor = messages.back().id;
      }
      m_pending.reset();
      m_patches.clear();
      m_initialized = true;
      m_error.clear();
      return true;
    }
    void trim(std::vector<api::Message>& messages, bool keepOlder = false) {
      if (messages.size() <= windowLimit)
        return;
      if (keepOlder) {
        messages.resize(windowLimit);
        m_newerEnd = false;
        m_newerCursor = messages.back().id;
      } else {
        messages.erase(messages.begin(), messages.end() - windowLimit);
        m_olderEnd = false;
        m_olderCursor = messages.front().id;
      }
    }
    static bool less(const std::string& a, const std::string& b) {
      return a.size() != b.size() ? a.size() < b.size() : a < b;
    }
    static void sort(std::vector<api::Message>& messages) {
      std::ranges::sort(messages, [](const auto& a, const auto& b) { return less(a.id, b.id); });
    }

  private:
    std::optional<api::HistoryQuery> m_pending;
    std::uint64_t m_serial = 0;
    bool m_olderEnd = false, m_newerEnd = true, m_initialized = false;
    std::string m_error, m_failedCursor;
    std::string m_olderCursor, m_newerCursor;
    std::map<std::string, api::MessagePatch> m_patches;
    api::HistoryDirection m_failed = api::HistoryDirection::Older;
  };
} // namespace app
