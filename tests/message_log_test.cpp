#include "app/message_group.h"
#include "app/message_history.h"
#include "app/message_log.h"
#include "app/message_mentions.h"
#include "app/session_store.h"
#include "app/unread_timeline.h"

#include <iostream>
#include <stdexcept>

int main() {
  const auto require = [](bool ok, const char* reason) {
    if (!ok)
      throw std::runtime_error(reason);
  };
  const auto message = [](std::string id, std::string content = "last known content") {
    return api::Message{id, "10", "Maya", content, "2026-10-18T09:41:00Z", "11"};
  };
  try {
    require(!app::Preferences{}.logDeletedMessages && !app::Preferences{}.logEditHistory, "Logger must default off");
    app::MessageLog log;
    std::vector<std::string> ids{"101", "102", "109"};
    auto first = message("101", "edited before deletion");
    first.editedTimestamp = "2026-10-18T09:42:00Z";
    first.reactions = {{"", "❤️", 2, true}};
    first.mentions["1"] = "Alex";
    first.mentionEveryone = true;
    log.capture("10", first);
    log.capture("10", message("102"));
    log.removed("10", ids);
    log.capture("10", message("101", "late stale content"));
    std::vector<api::Message> rows{message("100"), message("101", "stale REST"), message("102"), message("109")};
    log.filter("10", rows);
    require(
        rows.size() == 3
            && rows[1].deleted
            && rows[1].content == first.content
            && rows[1].editedTimestamp == first.editedTimestamp
            && rows[1].reactions.size() == 1,
        "Single/bulk deletion preserves immutable last-known rich content, never stale content"
    );
    require(
        !app::continuesMessageGroup(rows[0], rows[1]) && !app::continuesMessageGroup(rows[1], rows[2]),
        "Deleted messages always keep a visible author/deletion header"
    );
    require(!app::messageMentions(rows[1], "1", {}), "Deleted rows cannot get a live mention highlight");
    app::UnreadTimeline unread;
    unread.open("10");
    unread.snapshot({{"10", {"100", "102"}}});
    require(!unread.first(rows), "Retained deletions do not become unread messages");
    auto ephemeral = message("103");
    ephemeral.flags = 64;
    log.capture("10", ephemeral);
    require(!log.retained("10", "103"), "Ephemeral messages are never retained");
    rows = {message("100"), message("110")};
    log.merge("10", rows, false, false);
    require(
        rows.size() == 4 && rows[1].id == "101" && rows[2].deleted, "Refresh merges saved deletions in timeline order"
    );
    rows = {message("105"), message("110")};
    log.merge("10", rows, false, false);
    require(rows.size() == 2, "Historical windows cannot import disconnected saved rows");
    rows.clear();
    log.merge("10", rows, false, true);
    require(rows.empty(), "An unknown empty history edge cannot import disconnected rows");
    log.merge("10", rows, true, true);
    require(rows.size() == 2, "An empty complete channel retains known deletions");
    rows = {message("101")};
    log.filter("20", rows);
    require(!rows.front().deleted, "Channel identities are isolated");
    log.clearRecords();
    log.filter("10", rows);
    require(rows.empty() && !log.size(), "Disabling clears saved content and stale copies");
    log.capture("10", first);
    require(!log.size() && log.known("10", "101"), "Re-enabling cannot recover an earlier deletion");
    rows = {message("101")};
    log.merge("10", rows, true, true);
    require(rows.empty(), "Stale history cannot resurrect content after disabling");
    log.reset();
    require(!log.known("10", "101") && !log.size(), "Account reset clears all retention and deletion identities");
    for (int i = 1000; i <= 1200; ++i)
      log.capture("10", message(std::to_string(i)));
    require(
        log.size() == 201 && log.retained("10", "1000") && log.retained("10", "1200"),
        "Retained messages are not capped at 200 or evicted"
    );
    log.capture("10", message("1300", std::string(9 * 1024 * 1024, 'x')));
    require(log.retained("10", "1300") && log.retained("10", "1000"), "There is no content-size eviction cap");
    log.clearChannel("10");
    require(log.size() == 0, "Channel deletion releases its snapshots");
    log.reset();
    for (int i = 1000; i < 6000; ++i) {
      ids = {std::to_string(i)};
      log.removed("10", ids);
    }
    require(!log.known("10", "1000") && log.known("10", "5999"), "Deletion identities are bounded too");
    // The server history journal stays authoritative; saved copies are a separate display merge.
    log.reset();
    log.capture("10", first);
    ids = {"101"};
    log.removed("10", ids);
    app::MessageHistory history;
    rows = {message("100"), first};
    const auto query = history.begin(api::HistoryDirection::Latest, rows);
    require(
        history.apply(*query, rows, {message("100"), message("101", "stale")}, {{"101", std::nullopt}}),
        "History request applies"
    );
    log.merge("10", rows, history.olderEnd(), history.atLatest());
    require(
        rows.size() == 2 && rows.back().deleted && rows.back().content == first.content,
        "Deletion during an in-flight page returns only the saved marked copy"
    );
    rows.clear();
    log.merge("10", rows, false, false, "101", "101");
    require(
        rows.size() == 1 && rows.front().deleted,
        "A fully deleted in-flight page retains its saved rows inside the requested range"
    );
    log.reset();
    auto before = message("101", "Original <@1> message");
    before.mentions["1"] = "Alex";
    auto after = before;
    after.content = "First edit";
    after.editedTimestamp = "2026-10-18T09:42:00Z";
    require(log.recordEdit("10", before, after), "Content changes capture the previous version");
    require(!log.recordEdit("10", after, after), "Duplicate REST/Gateway acknowledgements add no version");
    auto last = after;
    last.content = "Second edit";
    last.editedTimestamp = "2026-10-18T09:43:00Z";
    require(
        log.recordEdit("10", after, last)
            && log.edits("10", "101").size() == 2
            && log.edits("10", "101").front().content == before.content,
        "Every successive edit retains an immutable original and intermediate version"
    );
    require(!log.recordEdit("10", last, after), "An older edit response cannot invent a new version");
    auto reaction = last;
    reaction.reactions = {{"", "❤️", 2, true}};
    require(!log.recordEdit("10", last, reaction), "Reactions do not create edit history");
    auto embed = last;
    embed.embeds.push_back({"Preview"});
    require(!log.recordEdit("10", last, embed), "Automatic embed population is not a message edit");
    auto file = last;
    file.attachments.push_back({"1", "file.txt", "https://cdn.discordapp.com/attachments/1/2/file.txt"});
    require(log.recordEdit("10", last, file), "Attachment edits retain their prior version too");
    auto refreshed = file;
    refreshed.attachments.front().url += "?ex=changed&hm=signature";
    refreshed.attachments.front().image = {"https://media.discordapp.net/refreshed", 400, 200};
    require(!log.recordEdit("10", file, refreshed), "Refreshed CDN signatures and projected sizes are not user edits");

    ephemeral = last;
    ephemeral.flags = 64;
    require(
        !log.recordEdit("10", ephemeral, file) && log.edits("20", "101").empty(),
        "Ephemeral messages and other channels cannot leak into edit history"
    );
    before = last;
    for (int i = 0; i < 250; ++i) {
      after = before;
      after.content = "Revision " + std::to_string(i);
      log.recordEdit("10", before, after);
      before = after;
    }
    require(log.edits("10", "101").size() == 253, "Edit history has no 200-version cap");
    log.capture("10", last);
    ids = {"101"};
    log.removed("10", ids);
    require(
        log.edits("10", "101").size() == 253 && !log.recordEdit("10", last, after),
        "Deleting an edited message preserves history and rejects later patches"
    );
    log.clearEdits();
    require(
        log.edits("10", "101").empty() && log.retained("10", "101"),
        "Clearing edit history leaves deleted snapshots intact"
    );
    before = message("201", "Another original");
    after = before;
    after.content = "Another edit";
    require(log.recordEdit("10", before, after), "A separate live message has edit history");
    log.clearDeleted();
    require(!log.size() && log.edits("10", "201").size() == 1, "Clearing deleted snapshots leaves edit history intact");
    log.clearRecords();
    require(log.edits("10", "201").empty() && !log.size(), "Clearing both releases all saved content");
    // Per-message forgetting touches exactly one key, including its deletion identity
    log.capture("10", first);
    log.capture("10", message("102"));
    ids = {"101", "102"};
    log.removed("10", ids);
    log.forget("10", "101", true, true);
    require(log.known("10", "102"), "Forgetting touches only its own message");
    require(!log.known("10", "101"), "Forgetting clears the snapshot and deletion identity together");
    ids = {"101"};
    log.removed("10", ids);
    require(log.known("10", "101"), "A message can log again after being forgotten");
    before = message("301", "Live text");
    after = before;
    after.content = "Edited text";
    after.editedTimestamp = "2026-10-18T09:44:00Z";
    require(log.recordEdit("10", before, after), "A live version is captured for the forget check");
    log.forget("10", "301", false, true);
    require(log.edits("10", "301").empty() && !log.retained("10", "301"), "Edit-only forget drops remembered versions");
    std::cout << "Message logger retention passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
