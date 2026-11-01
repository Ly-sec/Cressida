#pragma once

#include "api/models.h"

#include <set>

namespace api {
  enum class ThreadList { Active, Archived, PrivateArchived, Single, Starter };
  struct ThreadQuery {
    ThreadList list = ThreadList::Active;
    std::string guild, parent, before;
    std::uint64_t request = 0, revision = 0;
    std::string search, tag;
    bool newest = false;
    bool operator==(const ThreadQuery&) const = default;
  };
  struct ThreadPage {
    std::vector<Channel> threads;
    bool more = false;
    std::string before;
    std::vector<Message> firstMessages;
  };
  struct ThreadChange {
    enum class Kind { Upsert, Sync, Remove, RemoveParent, RemoveGuild };
    Kind kind = Kind::Upsert;
    std::string guild, id;
    std::optional<std::vector<std::string>> parents;
    std::vector<Channel> threads;
  };
  struct ThreadUpdates {
    bool reset = false;
    std::vector<ThreadChange> changes;
  };
  std::optional<Channel> parseThread(const nlohmann::json& value);
  ThreadUpdates parseThreadUpdates(std::string_view event, const nlohmann::json& value, const std::string& self);
  std::string threadListPath(const ThreadQuery& query);
  ThreadPage parseThreadPage(const nlohmann::json& value, const ThreadQuery& query);

  // UI-owned bounded directory. In-flight reads cannot resurrect deleted threads
  // or overwrite newer names, archive state or membership from the Gateway.
  class ThreadDirectory {
  public:
    void apply(const ThreadUpdates& updates);
    void page(const ThreadQuery& query, const ThreadPage& page);
    const Channel* find(const std::string& guild, const std::string& id) const;
    std::vector<Channel> list(const std::string& guild, const std::string& parent = {}) const;
    std::uint64_t revision() const { return m_revision; }

  private:
    struct Entry {
      std::string guild;
      Channel channel;
      std::uint64_t revision;
    };
    void upsert(const std::string& guild, const Channel& thread, std::uint64_t revision);
    void remove(const std::string& id);
    std::map<std::string, Entry> m_entries;
    std::map<std::string, std::uint64_t> m_removed, m_barriers;
    std::uint64_t m_revision = 0, m_floor = 0;
  };
} // namespace api
