#include "api/actions.h"
#include "api/read_state.h"
#include "api/threads.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

using nlohmann::json;
using namespace api;
namespace {
  void require(bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  }
  json thread(std::string id, std::string parent = "10", bool archived = false, int type = 11) {
    return {
        {"id", id},
        {"guild_id", "1"},
        {"type", type},
        {"parent_id", parent},
        {"name", "Discussion " + id},
        {"last_message_id", "999"},
        {"message_count", 12},
        {"thread_metadata",
         {{"archived", archived}, {"locked", false}, {"archive_timestamp", "2026-10-17T08:00:00+00:00"}}}
    };
  }
  ThreadUpdates update(const char* type, const json& data) { return parseThreadUpdates(type, data, "7"); }
} // namespace
int main() {
  try {
    auto parsed = parseThread(thread("100"));
    require(
        parsed
            && parsed->isThread()
            && parsed->parentId == "10"
            && parsed->archived == false
            && parsed->messageCount == 12
            && !parsed->joined,
        "Project thread metadata without inventing membership"
    );
    require(
        !parseThread({{"id", "100"}, {"type", 0}}) && !parseThread({{"id", "../x"}, {"type", 11}}),
        "Reject malformed IDs and non-thread channels"
    );
    auto base = parseChannels(
        json::array(
            {{{"id", "10"}, {"type", 0}}, {{"id", "11"}, {"type", 15}}, {{"id", "12"}, {"type", 16}}, thread("100")}
        )
    );
    require(
        base.size() == 3 && base[1].isForum() && base[2].isForum(),
        "Forum parents remain navigable; threads have a separate lifecycle"
    );
    ThreadQuery active{ThreadList::Active, "1", "10"};
    require(
        threadListPath(active)
            == "/channels/10/threads/search?archived=false&sort_by=last_message_time&sort_order=desc&limit=25&offset=0",
        "Personal-account thread search route"
    );
    require(threadListPath({ThreadList::Active, "1"}).empty(), "Never use bot-only guild active-thread endpoints");
    const ThreadQuery starter{ThreadList::Starter, "1", "200"};
    require(
        threadListPath(starter) == "/channels/200/messages/200",
        "Forum starter hydration reads the specific original message without changing chat history"
    );
    require(
        threadListPath({ThreadList::Starter, "1", "../200"}).empty()
            && threadListPath({ThreadList::Starter, "1", "200", "cursor"}).empty(),
        "Starter reads reject malformed identities and unrelated pagination"
    );
    auto starterData = json{
        {"id", "200"},
        {"channel_id", "200"},
        {"content", "Original post"},
        {"author", {{"id", "7"}, {"username", "Maya"}}}
    };
    const auto starterPage = parseThreadPage(starterData, starter);
    require(
        starterPage.threads.empty()
            && starterPage.firstMessages.size() == 1
            && starterPage.firstMessages.front().authorId == "7"
            && starterPage.firstMessages.front().content == "Original post",
        "Starter hydration retains the author and full original message"
    );
    for (const auto field : {"id", "channel_id", "guild_id"}) {
      auto mismatched = starterData;
      mismatched[field] = "201";
      bool rejected = false;
      try {
        (void)parseThreadPage(mismatched, starter);
      } catch (const std::exception&) {
        rejected = true;
      }
      require(rejected, "Starter responses cannot populate another thread's preview");
    }
    ThreadDirectory owners;
    auto owned = thread("200");
    owned["owner_id"] = "7";
    owners.apply(update("THREAD_CREATE", owned));
    owners.apply(update("THREAD_UPDATE", {{"id", "200"}, {"guild_id", "1"}, {"name", "Updated title"}}));
    require(owners.find("1", "200")->ownerId == "7", "Partial updates retain a known thread owner");
    ThreadDirectory delayedOwner;
    delayedOwner.apply(update("THREAD_CREATE", thread("200")));
    const ThreadQuery delayedQuery{ThreadList::Active, "1", "10", {}, 0, delayedOwner.revision()};
    delayedOwner.apply(update("THREAD_UPDATE", {{"id", "200"}, {"guild_id", "1"}, {"name", "Live title"}}));
    delayedOwner.page(delayedQuery, parseThreadPage({{"threads", json::array({owned})}}, delayedQuery));
    require(
        delayedOwner.find("1", "200")->ownerId == "7" && delayedOwner.find("1", "200")->name == "Live title",
        "A delayed page can fill missing owner metadata without replacing a newer live title"
    );
    auto search = active;
    search.search = "hello & 🌙";
    search.tag = "70";
    search.before = "25";
    search.newest = true;
    const auto searchPath = threadListPath(search);
    require(
        searchPath.contains("offset=25")
            && searchPath.contains("sort_by=creation_time")
            && searchPath.contains("name=hello%20%26%20%F0%9F%8C%99")
            && searchPath.contains("tag=70"),
        "Encode query text and validated tags in bounded personal-account searches"
    );
    search.before = "9976";
    require(threadListPath(search).empty(), "Bound search pagination offsets");
    ThreadQuery archived{ThreadList::Archived, "1", "10", "2026-10-17T08:00:00+00:00"};
    require(
        threadListPath(archived)
            == "/channels/10/threads/archived/public?limit=50&before=2026-10-17T08%3A00%3A00%2B00%3A00",
        "Timestamp archive cursor must be URL encoded"
    );
    ThreadQuery privateList{ThreadList::PrivateArchived, "1", "10", "123"};
    require(
        threadListPath(privateList) == "/channels/10/users/@me/threads/archived/private?limit=50&before=123",
        "Joined private archives use snowflake cursors and the user route"
    );
    require(
        threadListPath({ThreadList::Active, "../1"}).empty()
            && threadListPath({ThreadList::Archived, "1", "10", "a&token=bad"}).empty()
            && threadListPath({ThreadList::PrivateArchived, "1", "10", "123?limit=999"}).empty(),
        "All thread reads validate fixed routes and cursors"
    );
    auto page = parseThreadPage(
        {{"threads", json::array({thread("100"), thread("101", "10", false, 12)})},
         {"members", json::array({{{"id", "101"}}})}},
        active
    );
    require(
        page.threads.size() == 2 && page.threads[0].joined == false && page.threads[1].joined == true,
        "Membership list marks only joined threads"
    );
    auto archives = parseThreadPage(
        {{"threads", json::array({thread("99", "10", true), thread("98", "20", true)})}, {"has_more", true}},
        {ThreadList::Archived, "1", "10"}
    );
    require(
        archives.threads.size() == 1 && archives.before == "2026-10-17T08:00:00+00:00" && archives.more,
        "Archived results are scoped to the requested parent"
    );
    bool threw = false;
    try {
      parseThreadPage({{"threads", json::array()}, {"has_more", true}}, archived);
    } catch (...) {
      threw = true;
    }
    require(threw, "Reject a nonadvancing page instead of requesting it forever");
    threw = false;
    try {
      parseThreadPage(thread("100"), {ThreadList::Single, "2", "100"});
    } catch (...) {
      threw = true;
    }
    require(threw, "A single-channel lookup cannot cross guild identities");
    ThreadDirectory directory;
    directory.apply(update(
        "READY",
        {{"guilds",
          json::array(
              {{{"id", "1"}, {"threads", json::array({thread("100"), thread("101", "20"), thread("102", "10", true)})}}}
          )}}
    ));
    require(
        directory.list("1").size() == 3 && directory.list("2").empty(),
        "READY imports guild threads with isolated guild ownership"
    );
    directory.apply(update(
        "THREAD_UPDATE",
        {{"id", "100"}, {"guild_id", "1"}, {"name", "Renamed"}, {"thread_metadata", {{"locked", true}}}}
    ));
    require(
        directory.find("1", "100")->name == "Renamed"
            && directory.find("1", "100")->locked == true
            && directory.find("1", "100")->archived == false
            && directory.find("1", "100")->parentId == "10",
        "Partial updates preserve parent, archive state and counts"
    );
    directory.apply(update(
        "THREAD_LIST_SYNC",
        {{"guild_id", "1"},
         {"channel_ids", json::array({"10"})},
         {"threads", json::array({thread("103"), thread("104", "20")})},
         {"members", json::array({{{"id", "103"}}})}}
    ));
    require(
        !directory.find("1", "100")
            && directory.find("1", "101")
            && directory.find("1", "102")
            && directory.find("1", "103")->joined == true
            && !directory.find("1", "104"),
        "Scoped sync clears missing active threads only within its parents, preserves archives and ignores "
        "out-of-scope rows"
    );
    directory.apply(
        update("THREAD_LIST_SYNC", {{"guild_id", "1"}, {"channel_ids", json::array()}, {"threads", json::array()}})
    );
    require(directory.find("1", "103") != nullptr, "An explicitly empty parent scope is not a full-guild sync");
    ThreadQuery inFlight{ThreadList::Active, "1", {}, {}, 3, directory.revision()};
    directory.apply(update("THREAD_DELETE", {{"id", "103"}, {"guild_id", "1"}}));
    directory.apply(update("THREAD_UPDATE", {{"id", "101"}, {"guild_id", "1"}, {"name", "Live name"}}));
    directory.apply(update("THREAD_CREATE", thread("105")));
    directory.page(inFlight, {{*parseThread(thread("103")), *parseThread(thread("101", "20"))}});
    require(
        !directory.find("1", "103") && directory.find("1", "101")->name == "Live name" && directory.find("1", "105"),
        "In-flight REST snapshots cannot resurrect deletions, overwrite updates or remove fresh creations"
    );
    directory.apply(update("THREAD_CREATE", thread("110", "10", false, 12)));
    directory.apply(update("THREAD_MEMBER_UPDATE", {{"id", "110"}, {"guild_id", "1"}, {"user_id", "7"}}));
    require(
        directory.find("1", "110")->type == 12 && directory.find("1", "110")->joined == true,
        "Membership updates preserve private thread type"
    );
    directory.apply(update(
        "THREAD_MEMBERS_UPDATE",
        {{"id", "110"}, {"guild_id", "1"}, {"member_count", 2}, {"removed_member_ids", json::array({"8"})}}
    ));
    require(directory.find("1", "110")->joined == true, "Other members leaving cannot change our membership");
    directory.apply(
        update("THREAD_MEMBERS_UPDATE", {{"id", "110"}, {"guild_id", "1"}, {"removed_member_ids", json::array({"7"})}})
    );
    require(!directory.find("1", "110"), "Losing private membership removes the private thread");
    directory.apply(update("THREAD_MEMBER_UPDATE", {{"id", "105"}, {"guild_id", "1"}, {"user_id", "7"}}));
    directory.apply(
        update("THREAD_MEMBERS_UPDATE", {{"id", "105"}, {"guild_id", "1"}, {"removed_member_ids", json::array({"7"})}})
    );
    require(
        directory.find("1", "105") && directory.find("1", "105")->joined == false,
        "Leaving public threads keeps them discoverable"
    );
    directory.apply(update("CHANNEL_DELETE", {{"id", "10"}, {"guild_id", "1"}}));
    require(
        directory.list("1", "10").empty() && directory.find("1", "101"), "Deleting a parent removes its threads only"
    );
    directory.apply(update("GUILD_DELETE", {{"id", "1"}, {"unavailable", true}}));
    require(directory.find("1", "101"), "Temporary unavailability preserves threads");
    directory.apply(update("GUILD_DELETE", {{"id", "1"}}));
    require(directory.list("1").empty(), "Leaving a guild clears its thread directory");
    directory.apply(update("THREAD_CREATE", thread("120")));
    auto old = ThreadQuery{ThreadList::Active, "1", {}, {}, 5, directory.revision()};
    directory.apply(update("READY", {{"guilds", json::array()}}));
    directory.page(old, page);
    require(directory.list("1").empty(), "A fresh READY rejects earlier in-flight snapshots");
    auto message = parseMessages(
                       json::array({{{"id", "100"}, {"channel_id", "10"}, {"flags", 32}, {"thread", thread("100")}}})
    ).front();
    require(
        message.hasThread && message.thread && message.thread->parentId == "10",
        "Messages retain thread association and HAS_THREAD flags"
    );
    parseMessagePatch({{"content", "edited"}}).apply(message);
    require(message.hasThread && message.thread, "Partial message edits retain associated threads");
    parseMessagePatch({{"flags", 0}, {"thread", nullptr}}).apply(message);
    require(!message.hasThread && !message.thread, "Explicit fields clear thread association");
    Interaction create{.kind = InteractionKind::CreateThread, .channel = "10", .message = "100", .text = "Plans"};
    auto request = interactionRequest(create);
    require(
        request
            && request->method == "POST"
            && request->path == "/channels/10/messages/100/threads"
            && request->body["name"] == "Plans",
        "Create a named thread from a message"
    );
    create.message.clear();
    request = interactionRequest(create);
    require(
        request && request->path == "/channels/10/threads" && request->body["type"] == 11,
        "Standalone creation must explicitly request public type instead of Discord's private default"
    );
    for (const auto& name : {std::string{}, std::string("  "), std::string("bad\nname"), std::string(101, 'a')}) {
      create.text = name;
      require(!interactionRequest(create), "Reject invalid thread names");
    }
    auto join = interactionRequest({.kind = InteractionKind::JoinThread, .channel = "100"});
    auto leave = interactionRequest({.kind = InteractionKind::LeaveThread, .channel = "100"});
    auto reopen = interactionRequest({.kind = InteractionKind::UnarchiveThread, .channel = "100"});
    require(
        join
            && join->method == "PUT"
            && leave
            && leave->method == "DELETE"
            && join->path == "/channels/100/thread-members/@me"
            && reopen
            && reopen->body == json{{"archived", false}},
        "Join, leave and unarchive use fixed minimal mutations"
    );
    auto forum = parseChannels(
                     json::array(
                         {{{"id", "10"},
                           {"type", 15},
                           {"flags", 16},
                           {"available_tags", json::array({{{"id", "70"}, {"name", "Linux"}, {"emoji_name", "🐧"}}})}}}
                     )
    ).front();
    require(
        forum.requireTag && forum.availableTags.size() == 1 && forum.availableTags[0].emojiName == "🐧",
        "Project forum tag requirements and icons"
    );
    auto forumThread = thread("200");
    forumThread["applied_tags"] = json::array({"70"});
    auto forumPage = parseThreadPage(
        {{"threads", json::array({forumThread})},
         {"has_more", true},
         {"first_messages",
          json::array(
              {{{"id", "200"},
                {"channel_id", "200"},
                {"content", "Preview"},
                {"author", {{"id", "7"}, {"username", "Maya"}}}}}
          )}},
        active
    );
    require(
        forumPage.before == "1"
            && forumPage.firstMessages.size() == 1
            && forumPage.threads[0].appliedTags->front() == "70",
        "Forum search projects preview messages, applied tags and advancing offset"
    );
    auto incompleteStarter = starterData;
    incompleteStarter.erase("content");
    const auto incompletePage = parseThreadPage(
        {{"threads", json::array({forumThread})}, {"first_messages", json::array({incompleteStarter})}}, active
    );
    require(
        incompletePage.threads.size() == 1 && incompletePage.firstMessages.empty(),
        "Incomplete bundled starter data does not masquerade as a fully loaded post preview"
    );
    auto post = interactionRequest(
        {.kind = InteractionKind::CreateForumPost,
         .channel = "10",
         .text = "My setup",
         .content = "Description <@7> <@&8> @here",
         .tags = {"70"}}
    );
    require(
        post
            && post->method == "POST"
            && post->body["message"]["content"] == "Description <@7> <@&8> @here"
            && post->body["message"]["allowed_mentions"]["parse"] == json::array({"users", "roles", "everyone"})
            && post->body["applied_tags"] == json::array({"70"}),
        "Explicit forum posts carry a name, message, normal mention parsing and tags"
    );
    require(
        !interactionRequest({.kind = InteractionKind::CreateForumPost, .channel = "10", .text = "Empty"}),
        "A forum post cannot be created without a starter message"
    );
    directory.apply(update("THREAD_CREATE", thread("300")));
    auto beforeLeave = ThreadQuery{ThreadList::Single, "1", "301", {}, 0, directory.revision()};
    directory.apply(update("GUILD_DELETE", {{"id", "1"}}));
    directory.page(beforeLeave, {{*parseThread(thread("301"))}});
    require(
        directory.list("1").empty(), "Late reads cannot introduce previously unknown threads after leaving a guild"
    );
    ReadState reads;
    reads.ready(
        {{"guilds", json::array({{{"id", "1"}, {"threads", json::array({thread("100")})}}})},
         {"read_state", json::array({{{"id", "100"}, {"last_message_id", "998"}, {"mention_count", 1}}})}}
    );
    require(
        reads.unread().contains("100") && reads.unread().at("100") == "1",
        "Thread read states contribute to the correct server's unread indicators"
    );
    reads.context("THREAD_DELETE", {{"id", "100"}, {"guild_id", "1"}}, "7");
    require(!reads.unread().contains("100"), "Deleted threads clear read state");
    std::cout << "Thread protocol, directory, race, message and route tests passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
