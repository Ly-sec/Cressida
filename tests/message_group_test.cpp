#include "app/message_group.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

int main() {
  const auto require = [](bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  try {
    for (const bool compact : {false, true}) {
      const float lineHeight = 22;
      const float firstBodyTop = 18;
      const float firstRowHeight = lineHeight + app::messageRowPadding(false, true, compact);
      const float nextRowHeight = lineHeight + app::messageRowPadding(true, true, compact);
      const float firstGap = firstRowHeight - firstBodyTop - lineHeight;
      const float nextGap = nextRowHeight - lineHeight;
      require(firstGap == nextGap, "The first-to-second message gap matches all subsequent gaps in the group");
      require(
          app::messageRowPadding(false, false, compact, 64, true) == firstBodyTop
              && app::messageRowPadding(true, false, compact, 64, true) == 0,
          "A final row never reserves the following group margin, even at maximum spacing"
      );
      require(nextGap == 0, "Consecutive messages use the same line spacing as wrapped lines without an extra row gap");
      require(
          app::messageRowPadding(true, false, compact) > nextGap,
          "Separate groups retain a larger gap than consecutive messages"
      );
    }
    api::Message first{"100", "10", "Lemmy", "First", "2026-10-17T09:41:00.000000+00:00", "11"};
    auto next = first;
    next.id = "101";
    next.content = "Second";
    next.timestamp = "2026-10-17T09:42:00Z";
    require(app::continuesMessageGroup(first, next), "Consecutive ordinary messages share the author header");
    next.authorBot = true;
    require(!app::continuesMessageGroup(first, next), "A changed bot identity retains its own APP header");
    next.authorBot = false;
    auto pending = next;
    pending.id = "pending:123";
    pending.author = "Account display name";
    pending.avatar = "account-avatar";
    require(
        !app::continuesMessageGroup(first, pending) && app::continuesMessageGroup(first, pending, true),
        "Pending follow-ups group by account identity before server profile fields arrive"
    );
    auto recent = first;
    recent.timestamp = "2026-10-17T09:41:00.999Z";
    pending.timestamp = "2026-10-17T09:41:00.998Z";
    require(
        app::continuesMessageGroup(recent, pending, true),
        "Small local clock differences do not split a pending follow-up"
    );
    pending.timestamp = "2026-10-17T09:40:00Z";
    require(
        !app::continuesMessageGroup(recent, pending, true),
        "Large reversed timestamp gaps retain a separate pending header"
    );
    pending.timestamp = "2026-10-17T09:48:00Z";
    require(
        !app::continuesMessageGroup(first, pending, true), "Pending messages retain the normal seven-minute boundary"
    );
    pending.timestamp = next.timestamp;
    pending.reply = api::MessageReference{.message = "99", .channel = first.channelId};
    require(!app::continuesMessageGroup(first, pending, true), "A pending reply always retains its own header");
    pending.reply.reset();
    pending.authorId = "12";
    require(!app::continuesMessageGroup(first, pending, true), "Pending grouping cannot merge different authors");
    pending = first;
    pending.timestamp = "2026-10-18T00:00:00.001Z";
    recent.timestamp = "2026-10-17T23:59:59.999Z";
    require(
        !app::continuesMessageGroup(recent, pending, true),
        "Pending messages retain a header across a calendar-day boundary"
    );
    next.timestamp = "2026-10-17T09:47:59.999Z";
    require(app::continuesMessageGroup(first, next), "Short runs can continue just below the seven-minute boundary");
    next.timestamp = "2026-10-17T09:48:00Z";
    require(!app::continuesMessageGroup(first, next), "A seven-minute gap starts a new group");
    next.timestamp = "2026-10-17T11:42:00+02:00";
    require(app::continuesMessageGroup(first, next), "Timestamp offsets normalize before comparing message gaps");
    next.timestamp = "2026-10-17T09:40:59Z";
    require(!app::continuesMessageGroup(first, next), "Out-of-order timestamps never hide an author header");
    for (const auto* value :
         {"", "invalid", "2026-02-30T09:41:00Z", "2026-10-17T24:00:00Z", "2026-10-17T09:41:00.",
          "2026-10-17T09:41:00+99:00", "2026-10-17T09:41:00Zjunk"}) {
      next.timestamp = value;
      require(!app::continuesMessageGroup(first, next), "Invalid or missing timestamps retain a separate header");
    }
    next = first;
    next.timestamp = "2026-10-18T00:00:10Z";
    first.timestamp = "2026-10-17T23:59:50Z";
    require(!app::continuesMessageGroup(first, next), "A calendar-day boundary starts a new group");
    next = first;
    next.authorId = "12";
    require(!app::continuesMessageGroup(first, next), "Identical display names never merge different accounts");
    next = first;
    next.authorId.clear();
    first.authorId.clear();
    require(!app::continuesMessageGroup(first, next), "Unknown authors cannot be grouped by display name alone");
    first.authorId = "11";
    auto reply = first;
    reply.reply = api::MessageReference{.message = "99", .channel = first.channelId};
    reply.type = 19;
    next = first;
    require(
        app::continuesMessageGroup(reply, next), "An ordinary follow-up continues the author group started by a reply"
    );
    require(
        !app::continuesMessageGroup(next, reply) && !app::continuesMessageGroup(reply, reply),
        "Each new reply starts its own author group and retains its reference preview"
    );
    reply.type = 0;
    require(
        app::continuesMessageGroup(reply, next),
        "Reference-only cached replies also allow an ordinary follow-up to continue"
    );
    reply.type = 19;
    for (const auto type : {7, 24}) {
      auto system = reply;
      system.type = type;
      require(
          !app::continuesMessageGroup(system, next),
          "References on system messages cannot make ordinary follow-ups continue their group"
      );
    }
    for (int reason = 0; reason < 8; ++reason) {
      next = first;
      switch (reason) {
      case 0:
        next.forwarded = api::ForwardedMessage{};
        break;
      case 1:
        next.flags = 64;
        break;
      case 2:
        next.type = 7;
        break;
      case 3:
        next.webhookId = "123";
        break;
      case 4:
        next.author = "Another webhook name";
        break;
      case 5:
        next.avatar = "different-avatar";
        break;
      case 6:
        next.channelId = "20";
        break;
      case 7:
        next.applicationId = "123";
        break;
      }
      require(
          !app::continuesMessageGroup(first, next) && !app::continuesMessageGroup(next, first),
          "Forwards, ephemeral/system messages and changed identities delimit groups in both directions"
      );
    }
    auto parsed = api::parseMessages(nlohmann::json::parse(R"JSON([
      {"id":"100","channel_id":"10","type":7,"webhook_id":"123","author":{"id":"11","username":"Lemmy"},"content":"Hello"}
    ])JSON"))
                      .front();
    api::parseMessagePatch({{"content", "Edited"}}).apply(parsed);
    require(parsed.type == 7 && parsed.webhookId == "123", "Partial edits retain message type and webhook identity");
    api::parseMessagePatch({{"type", 0}, {"webhook_id", nullptr}}).apply(parsed);
    require(parsed.type == 0 && parsed.webhookId.empty(), "Explicit fields can reset grouping metadata");
    std::cout << "Message grouping boundaries and identity projection passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
