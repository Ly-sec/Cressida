#pragma once
#include "api/models.h"
#include "app/message_group.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <span>
#include <string>

namespace app {
  // Session-only content, separate from authoritative history/tombstones.
  // No serialization, file access or network requests belong here.
  class MessageLog {
  public:
    static constexpr std::size_t tombstoneLimit = 4096;
    using Key = std::pair<std::string, std::string>;
    bool known(const std::string& channel, const std::string& id) const {
      return m_dead.contains({channel, id}) || m_records.contains({channel, id});
    }
    bool retained(const std::string& channel, const std::string& id) const { return m_records.contains({channel, id}); }
    std::size_t size() const { return m_records.size(); }
    const std::vector<api::Message>& edits(const std::string& channel, const std::string& id) const {
      static const std::vector<api::Message> empty;
      const auto found = m_edits.find({channel, id});
      return found == m_edits.end() ? empty : found->second;
    }
    static bool stale(const api::Message& before, const api::Message& after) {
      const auto oldTime = messageTime(before.editedTimestamp), newTime = messageTime(after.editedTimestamp);
      return oldTime && newTime && *newTime < *oldTime;
    }
    bool recordEdit(const std::string& channel, const api::Message& before, const api::Message& after) {
      if (known(channel, before.id)
          || before.deleted
          || after.deleted
          || (before.flags & 64)
          || (after.flags & 64)
          || !api::validSnowflake(channel)
          || !api::validSnowflake(before.id)
          || before.id != after.id
          || stale(before, after))
        return false;
      // Embed previews may populate without anyone editing the message.
      const bool sameAttachments =
          std::ranges::equal(before.attachments, after.attachments, [](const auto& a, const auto& b) {
            // Signed CDN URLs and projected image dimensions can refresh without an edit.
            const auto path = [](const auto& url) { return std::string_view(url).substr(0, url.find('?')); };
            return a.id == b.id && a.filename == b.filename && (!a.id.empty() || path(a.url) == path(b.url));
          });
      const bool changed = before.content != after.content
          || !sameAttachments
          || (before.embeds != after.embeds
              && !after.editedTimestamp.empty()
              && before.editedTimestamp != after.editedTimestamp);
      if (!changed)
        return false;
      auto copy = snapshot(before, channel);
      m_edits[{channel, before.id}].push_back(std::move(copy));
      return true;
    }
    void capture(const std::string& channel, const api::Message& message) {
      if (known(channel, message.id)
          || message.deleted
          || (message.flags & 64)
          || !api::validSnowflake(channel)
          || !api::validSnowflake(message.id))
        return;
      auto copy = snapshot(message, channel);
      copy.deleted = true;
      m_records.emplace(Key{channel, message.id}, std::move(copy));
    }
    void removed(const std::string& channel, std::span<const std::string> ids) {
      for (const auto& id : ids) {
        if (!api::validSnowflake(channel) || !api::validSnowflake(id))
          continue;
        const Key key{channel, id};
        if (m_dead.insert(key).second)
          m_deadOrder.push_back(key);
      }
      while (m_dead.size() > tombstoneLimit) {
        m_dead.erase(m_deadOrder.front());
        m_deadOrder.pop_front();
      }
    }
    void filter(const std::string& channel, std::vector<api::Message>& rows) const {
      std::erase_if(rows, [&](auto& row) {
        if (const auto saved = m_records.find({channel, row.id}); saved != m_records.end()) {
          row = saved->second;
          return false;
        }
        return row.deleted || m_dead.contains({channel, row.id});
      });
    }
    void merge(
        const std::string& channel, std::vector<api::Message>& rows, bool oldest, bool latest, std::string first = {},
        std::string last = {}
    ) const {
      filter(channel, rows);
      if (!rows.empty()) {
        first = rows.front().id;
        last = rows.back().id;
      }
      if (rows.empty() && first.empty() && !(oldest && latest))
        return;
      for (auto it = m_records.lower_bound({channel, ""}); it != m_records.end() && it->first.first == channel; ++it) {
        const auto& [key, entry] = *it;
        if ((oldest || !less(key.second, first))
            && (latest || !less(last, key.second))
            && std::ranges::find(rows, key.second, &api::Message::id) == rows.end())
          rows.push_back(entry);
      }
      std::ranges::sort(rows, [](const auto& a, const auto& b) { return less(a.id, b.id); });
    }
    void clearDeleted() { m_records.clear(); }
    void clearEdits() { m_edits.clear(); }
    void clearRecords() {
      clearDeleted();
      clearEdits();
    }
    void clearChannel(const std::string& channel) {
      std::erase_if(m_records, [&](const auto& entry) { return entry.first.first == channel; });
      std::erase_if(m_edits, [&](const auto& entry) { return entry.first.first == channel; });
      std::erase_if(m_dead, [&](const auto& key) { return key.first == channel; });
      std::erase_if(m_deadOrder, [&](const auto& key) { return key.first == channel; });
    }
    void reset() { *this = {}; }
    void clearChannelContent(const std::string& channel) {
      std::erase_if(m_records, [&](const auto& entry) { return entry.first.first == channel; });
      std::erase_if(m_edits, [&](const auto& entry) { return entry.first.first == channel; });
    }
    // Forget one message: its saved snapshot, deletion identity and remembered versions, independently
    void forget(const std::string& channel, const std::string& id, bool deletions, bool edits) {
      const Key key{channel, id};
      if (deletions) {
        m_records.erase(key);
        m_dead.erase(key);
        std::erase(m_deadOrder, key);
      }
      if (edits)
        m_edits.erase(key);
    }

  private:
    static bool less(const std::string& a, const std::string& b) {
      return a.size() != b.size() ? a.size() < b.size() : a < b;
    }
    static api::Message snapshot(const api::Message& message, const std::string& channel) {
      auto copy = message;
      copy.channelId = channel;
      copy.thread.reset();
      copy.hasThread = false;
      copy.buttons.clear();
      return copy;
    }
    std::map<Key, api::Message> m_records;
    std::map<Key, std::vector<api::Message>> m_edits;
    std::deque<Key> m_deadOrder;
    std::set<Key> m_dead;
  };
} // namespace app
