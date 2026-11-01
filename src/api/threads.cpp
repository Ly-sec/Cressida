#include "api/threads.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace api {
  namespace {
    using Json = nlohmann::json;
    constexpr std::size_t limit = 4096;
    std::string string(const Json& v, const char* key) {
      auto i = v.find(key);
      return i != v.end() && i->is_string() ? i->get<std::string>() : "";
    }
    bool completeStarter(const Json& value) {
      const auto author = value.find("author"), content = value.find("content");
      return value.is_object()
          && author != value.end()
          && author->is_object()
          && validSnowflake(string(*author, "id"))
          && (!string(*author, "username").empty() || !string(*author, "global_name").empty())
          && content != value.end()
          && content->is_string();
    }
    bool newer(const std::string& a, const std::string& b) {
      return a.size() != b.size() ? a.size() > b.size() : a > b;
    }
    void memberFlags(std::vector<Channel>& threads, const Json& members) {
      if (!members.is_array())
        return;
      std::set<std::string> joined;
      for (const auto& m : members)
        if (joined.size() < limit && validSnowflake(string(m, "id")))
          joined.insert(string(m, "id"));
      for (auto& t : threads)
        t.joined = joined.contains(t.id);
    }
    std::vector<Channel> parseList(const Json& list, const std::string& guild) {
      std::vector<Channel> result;
      if (!list.is_array())
        return result;
      for (const auto& item : list) {
        if (result.size() == limit)
          break;
        if (!string(item, "guild_id").empty() && string(item, "guild_id") != guild)
          continue;
        if (auto thread = parseThread(item))
          result.push_back(std::move(*thread));
      }
      return result;
    }
  } // namespace
  std::optional<Channel> parseThread(const Json& value) {
    if (!value.is_object() || !validSnowflake(string(value, "id")))
      return {};
    Channel result;
    result.id = string(value, "id");
    if (value.contains("type") && value["type"].is_number_integer())
      result.type = value["type"].get<int>();
    else
      result.type = 11; // Partial membership updates omit the channel type.
    if (!result.isThread())
      return {};
    result.name = string(value, "name").substr(0, 400);
    result.parentId = string(value, "parent_id");
    if (const auto owner = string(value, "owner_id"); validSnowflake(owner))
      result.ownerId = owner;
    if (!result.parentId.empty() && !validSnowflake(result.parentId))
      return {};
    if (value.contains("applied_tags") && value["applied_tags"].is_array()) {
      result.appliedTags.emplace();
      for (const auto& tag : value["applied_tags"])
        if (tag.is_string() && validSnowflake(tag.get<std::string>()) && result.appliedTags->size() < 5)
          result.appliedTags->push_back(tag.get<std::string>());
    }
    result.lastMessageId = string(value, "last_message_id");
    if (!validSnowflake(result.lastMessageId))
      result.lastMessageId.clear();
    auto metadata = value.find("thread_metadata");
    if (metadata != value.end() && metadata->is_object()) {
      for (const auto& [key, dest] : {std::pair{"archived", &result.archived}, {"locked", &result.locked}})
        if (metadata->contains(key) && (*metadata)[key].is_boolean())
          *dest = (*metadata)[key].get<bool>();
      result.archiveTimestamp = string(*metadata, "archive_timestamp").substr(0, 40);
    }
    if (value.contains("member") && value["member"].is_object())
      result.joined = true;
    for (const auto& [key, dest] :
         {std::pair{"message_count", &result.messageCount}, {"member_count", &result.threadMemberCount}})
      if (value.contains(key) && value[key].is_number_integer())
        *dest = static_cast<int>(std::clamp<std::int64_t>(value[key].get<std::int64_t>(), 0, 1000000));
    return result;
  }
  ThreadUpdates parseThreadUpdates(std::string_view event, const Json& value, const std::string& self) {
    ThreadUpdates result;
    if (!value.is_object())
      return result;
    auto snapshot = [&](const Json& guild) {
      const auto id = string(guild, "id");
      if (validSnowflake(id) && guild.contains("threads") && guild["threads"].is_array())
        result.changes.push_back({ThreadChange::Kind::Sync, id, {}, {}, parseList(guild["threads"], id)});
    };
    if (event == "READY") {
      result.reset = true;
      if (value.contains("guilds") && value["guilds"].is_array())
        for (const auto& g : value["guilds"])
          snapshot(g);
      return result;
    }
    if (event == "GUILD_CREATE") {
      snapshot(value);
      return result;
    }
    const auto guild = event == "GUILD_DELETE" ? string(value, "id") : string(value, "guild_id");
    if (!validSnowflake(guild))
      return result;
    ThreadChange change;
    change.guild = guild;
    change.id = string(value, "id");
    if (event == "THREAD_LIST_SYNC") {
      if (!value.contains("threads") || !value["threads"].is_array())
        return result;
      change.kind = ThreadChange::Kind::Sync;
      if (value.contains("channel_ids")) {
        if (!value["channel_ids"].is_array())
          return result;
        change.parents.emplace();
        for (const auto& p : value["channel_ids"])
          if (p.is_string() && validSnowflake(p.get<std::string>()) && change.parents->size() < limit)
            change.parents->push_back(p.get<std::string>());
      }
      change.threads = parseList(value["threads"], guild);
      if (change.parents)
        std::erase_if(change.threads, [&](const auto& t) {
          return std::ranges::find(*change.parents, t.parentId) == change.parents->end();
        });
      if (value.contains("members"))
        memberFlags(change.threads, value["members"]);
    } else if (event == "THREAD_CREATE" || event == "THREAD_UPDATE") {
      if (auto thread = parseThread(value))
        change.threads.push_back(*thread);
    } else if (event == "MESSAGE_CREATE" || event == "MESSAGE_UPDATE") {
      if (value.contains("thread"))
        if (auto t = parseThread(value["thread"]))
          change.threads.push_back(*t);
      if (event == "MESSAGE_CREATE" && validSnowflake(string(value, "channel_id"))) {
        Channel patch;
        patch.id = string(value, "channel_id");
        patch.type = 11;
        patch.lastMessageId = string(value, "id");
        if (validSnowflake(patch.lastMessageId))
          change.threads.push_back(std::move(patch));
      }
    } else if (event == "THREAD_MEMBER_UPDATE" || event == "THREAD_MEMBERS_UPDATE") {
      if (auto thread = parseThread(value)) {
        if (event == "THREAD_MEMBER_UPDATE" && (string(value, "user_id").empty() || string(value, "user_id") == self))
          thread->joined = true;
        if (value.contains("added_members") && value["added_members"].is_array())
          for (const auto& m : value["added_members"])
            if (string(m, "user_id") == self)
              thread->joined = true;
        if (value.contains("removed_member_ids") && value["removed_member_ids"].is_array())
          for (const auto& id : value["removed_member_ids"])
            if (id.is_string() && id.get<std::string>() == self)
              thread->joined = false;
        change.threads.push_back(*thread);
      }
    } else if (event == "THREAD_DELETE")
      change.kind = ThreadChange::Kind::Remove;
    else if (event == "CHANNEL_DELETE")
      change.kind = ThreadChange::Kind::RemoveParent;
    else if (event == "GUILD_DELETE" && !value.value("unavailable", false))
      change.kind = ThreadChange::Kind::RemoveGuild;
    else
      return result;
    result.changes.push_back(std::move(change));
    return result;
  }
  std::string threadListPath(const ThreadQuery& q) {
    if (!validSnowflake(q.guild))
      return {};
    if (!validSnowflake(q.parent))
      return {};
    if (q.list == ThreadList::Starter)
      return q.before.empty() && q.search.empty() && q.tag.empty() && !q.newest
          ? "/channels/" + q.parent + "/messages/" + q.parent
          : "";
    if (q.list == ThreadList::Active) {
      unsigned offset = 0;
      if (!q.before.empty()) {
        if (q.before.size() > 4 || !std::ranges::all_of(q.before, [](unsigned char c) { return c >= '0' && c <= '9'; }))
          return {};
        offset = static_cast<unsigned>(std::stoul(q.before));
        if (offset > 9975)
          return {};
      }
      if (q.search.size() > 400
          || utf8Length(q.search) > 100
          || std::ranges::any_of(q.search, [](unsigned char c) { return c < 32 || c == 127; })
          || (!q.tag.empty() && !validSnowflake(q.tag)))
        return {};
      std::string path = "/channels/"
          + q.parent
          + "/threads/search?archived=false&sort_by="
          + (q.newest ? "creation_time" : "last_message_time")
          + "&sort_order=desc&limit=25&offset="
          + std::to_string(offset);
      auto encode = [](std::string_view value) {
        constexpr char hex[] = "0123456789ABCDEF";
        std::string result;
        for (unsigned char c : value) {
          if ((c >= 'a' && c <= 'z')
              || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9')
              || c == '-'
              || c == '_'
              || c == '.'
              || c == '~')
            result += c;
          else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
          }
        }
        return result;
      };
      if (!q.search.empty())
        path += "&name=" + encode(q.search);
      if (!q.tag.empty())
        path += "&tag=" + q.tag + "&tag_setting=match_some";
      return path;
    }
    if (q.list == ThreadList::Single)
      return q.before.empty() ? "/channels/" + q.parent : "";
    std::string path = "/channels/"
        + q.parent
        + (q.list == ThreadList::Archived ? "/threads/archived/public?limit=50"
                                          : "/users/@me/threads/archived/private?limit=50");
    if (!q.before.empty()) {
      if (q.list == ThreadList::PrivateArchived) {
        if (!validSnowflake(q.before))
          return {};
        path += "&before=" + q.before;
      } else {
        if (q.before.size() < 20 || q.before.size() > 40 || !std::ranges::all_of(q.before, [](unsigned char c) {
              return (c >= '0' && c <= '9') || c == '-' || c == ':' || c == '.' || c == '+' || c == 'T' || c == 'Z';
            }))
          return {};
        path += "&before=";
        for (char c : q.before)
          path += c == ':' ? "%3A" : c == '+' ? "%2B" : std::string(1, c);
      }
    }
    return path;
  }
  ThreadPage parseThreadPage(const Json& value, const ThreadQuery& q) {
    if (!value.is_object())
      throw std::runtime_error("Expected thread response");
    ThreadPage result;
    if (q.list == ThreadList::Starter) {
      if (!completeStarter(value))
        throw std::runtime_error("Incomplete post preview");
      auto messages = parseMessages(Json::array({value}));
      if (messages.size() != 1
          || (!string(value, "guild_id").empty() && string(value, "guild_id") != q.guild)
          || messages.front().id != q.parent
          || messages.front().channelId != q.parent
          || !validSnowflake(messages.front().authorId))
        throw std::runtime_error("Unexpected post preview identity");
      result.firstMessages = std::move(messages);
      return result;
    }
    if (q.list == ThreadList::Single) {
      auto thread = parseThread(value);
      if (!thread
          || thread->id != q.parent
          || (!string(value, "guild_id").empty() && string(value, "guild_id") != q.guild))
        throw std::runtime_error("Unexpected thread identity");
      result.threads.push_back(*thread);
      return result;
    }
    if (!value.contains("threads") || !value["threads"].is_array() || value["threads"].size() > limit)
      throw std::runtime_error("Invalid thread list");
    result.threads = parseList(value["threads"], q.guild);
    if (!q.parent.empty())
      std::erase_if(result.threads, [&](const auto& t) { return t.parentId != q.parent; });
    if (value.contains("members"))
      memberFlags(result.threads, value["members"]);
    if (q.list == ThreadList::PrivateArchived)
      for (auto& t : result.threads)
        t.joined = true;
    result.more = value.contains("has_more") && value["has_more"] == true;
    if (result.more && value["threads"].empty())
      throw std::runtime_error("Thread page did not advance");
    if (value.contains("first_messages")
        && value["first_messages"].is_array()
        && value["first_messages"].size() <= 100) {
      for (const auto& item : value["first_messages"]) {
        if (!completeStarter(item))
          continue;
        for (auto& message : parseMessages(Json::array({item})))
          if (message.id == message.channelId
              && std::ranges::any_of(result.threads, [&](const auto& t) { return message.channelId == t.id; }))
            result.firstMessages.push_back(std::move(message));
      }
    }
    if (q.list == ThreadList::Active) {
      const auto offset = q.before.empty() ? 0U : static_cast<unsigned>(std::stoul(q.before));
      result.before = std::to_string(offset + value["threads"].size());
      if (offset + value["threads"].size() > 9975)
        result.more = false;
    } else if (!result.threads.empty())
      result.before =
          q.list == ThreadList::PrivateArchived ? result.threads.back().id : result.threads.back().archiveTimestamp;
    if (result.more
        && (result.before.empty()
            || result.before == q.before
            || threadListPath(ThreadQuery{q.list, q.guild, q.parent, result.before}).empty()))
      throw std::runtime_error("Thread page did not advance");
    return result;
  }
  void ThreadDirectory::upsert(const std::string& guild, const Channel& t, std::uint64_t revision) {
    if (!validSnowflake(guild) || !validSnowflake(t.id) || !t.isThread())
      return;
    auto found = m_entries.find(t.id);
    if (found == m_entries.end()) {
      if (m_entries.size() >= limit || t.name.empty() || !validSnowflake(t.parentId))
        return;
      m_entries.emplace(t.id, Entry{guild, t, revision});
    } else if (found->second.guild == guild) {
      if (found->second.channel.type == 12 && t.joined == false && t.name.empty() && t.parentId.empty()) {
        remove(t.id);
        return;
      }
      auto& c = found->second.channel;
      if (!t.name.empty())
        c.name = t.name;
      if (!t.parentId.empty()) {
        c.parentId = t.parentId;
        c.type = t.type;
      }
      if (!t.lastMessageId.empty() && newer(t.lastMessageId, c.lastMessageId)) {
        if (!t.messageCount && c.messageCount && *c.messageCount < 1000000)
          ++*c.messageCount;
        c.lastMessageId = t.lastMessageId;
      }
      if (t.archived)
        c.archived = t.archived;
      if (t.locked)
        c.locked = t.locked;
      if (t.joined)
        c.joined = t.joined;
      if (t.appliedTags)
        c.appliedTags = t.appliedTags;
      if (t.ownerId)
        c.ownerId = t.ownerId;
      if (t.messageCount)
        c.messageCount = t.messageCount;
      if (t.threadMemberCount)
        c.threadMemberCount = t.threadMemberCount;
      if (!t.archiveTimestamp.empty())
        c.archiveTimestamp = t.archiveTimestamp;
      found->second.revision = revision;
    }
    m_removed.erase(t.id);
  }
  void ThreadDirectory::remove(const std::string& id) {
    m_entries.erase(id);
    if (m_removed.size() >= limit * 2) {
      m_removed.clear();
      m_floor = m_revision;
    }
    m_removed[id] = m_revision;
  }
  void ThreadDirectory::apply(const ThreadUpdates& updates) {
    ++m_revision;
    if (updates.reset) {
      m_entries.clear();
      m_removed.clear();
      m_barriers.clear();
      m_floor = m_revision;
    }
    for (const auto& change : updates.changes) {
      if (change.kind == ThreadChange::Kind::Remove) {
        if (const auto existing = m_entries.find(change.id);
            existing != m_entries.end() && existing->second.guild != change.guild)
          continue;
        remove(change.id);
        continue;
      }
      if (m_barriers.size() >= limit * 2) {
        m_barriers.clear();
        m_floor = m_revision;
      }
      if (change.kind == ThreadChange::Kind::RemoveGuild
          || (change.kind == ThreadChange::Kind::Sync && !change.parents))
        m_barriers["g:" + change.guild] = m_revision;
      if (change.kind == ThreadChange::Kind::RemoveParent)
        m_barriers["p:" + change.guild + ":" + change.id] = m_revision;
      if (change.kind == ThreadChange::Kind::Sync && change.parents)
        for (const auto& parent : *change.parents)
          m_barriers["p:" + change.guild + ":" + parent] = m_revision;
      std::vector<std::string> removed;
      for (const auto& [id, e] : m_entries) {
        if (e.guild != change.guild)
          continue;
        const bool scope =
            !change.parents || std::ranges::find(*change.parents, e.channel.parentId) != change.parents->end();
        if (change.kind == ThreadChange::Kind::RemoveGuild
            || (change.kind == ThreadChange::Kind::RemoveParent && e.channel.parentId == change.id)
            || (change.kind == ThreadChange::Kind::Sync
                && scope
                && !e.channel.archived.value_or(false)
                && std::ranges::none_of(change.threads, [&](const auto& t) { return t.id == id; })))
          removed.push_back(id);
      }
      for (const auto& id : removed)
        remove(id);
      for (const auto& thread : change.threads)
        upsert(change.guild, thread, m_revision);
    }
  }
  void ThreadDirectory::page(const ThreadQuery& query, const ThreadPage& page) {
    if (query.revision < m_floor)
      return;
    for (const auto& thread : page.threads) {
      const auto guildBarrier = m_barriers.find("g:" + query.guild),
                 parentBarrier = m_barriers.find("p:" + query.guild + ":" + thread.parentId);
      if ((guildBarrier != m_barriers.end() && guildBarrier->second > query.revision)
          || (parentBarrier != m_barriers.end() && parentBarrier->second > query.revision))
        continue;
      if (auto i = m_entries.find(thread.id); i != m_entries.end() && i->second.revision > query.revision) {
        if (i->second.guild == query.guild
            && i->second.channel.parentId == thread.parentId
            && !i->second.channel.ownerId
            && thread.ownerId)
          i->second.channel.ownerId = thread.ownerId;
        continue;
      }
      if (auto i = m_removed.find(thread.id); i != m_removed.end() && i->second > query.revision)
        continue;
      upsert(query.guild, thread, query.revision);
    }
  }
  const Channel* ThreadDirectory::find(const std::string& guild, const std::string& id) const {
    auto i = m_entries.find(id);
    return i == m_entries.end() || i->second.guild != guild ? nullptr : &i->second.channel;
  }
  std::vector<Channel> ThreadDirectory::list(const std::string& guild, const std::string& parent) const {
    std::vector<Channel> result;
    for (const auto& [id, e] : m_entries)
      if (e.guild == guild && (parent.empty() || parent == e.channel.parentId))
        result.push_back(e.channel);
    std::ranges::sort(result, [](const auto& a, const auto& b) { return newer(a.id, b.id); });
    return result;
  }
} // namespace api
