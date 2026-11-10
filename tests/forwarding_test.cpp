#include "api/actions.h"
#include "api/forwarding.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

int main() {
  const auto require = [](bool valid, const char* reason) {
    if (!valid)
      throw std::runtime_error(reason);
  };
  try {
    const api::ForwardReference source{"20", "10", "1"};
    const auto body = api::forwardMessageBody(source, "1234");
    require(
        body["message_reference"]
            == nlohmann::json(
                {{"type", 1},
                 {"message_id", "20"},
                 {"channel_id", "10"},
                 {"guild_id", "1"},
                 {"fail_if_not_exists", true}}
            ),
        "Forward requests use the typed source reference, including its original channel and server"
    );
    require(
        !body.contains("content")
            && !body.contains("message_snapshots")
            && !body.contains("attachments")
            && body["nonce"] == "1234"
            && body["enforce_nonce"] == true
            && body["allowed_mentions"]["parse"].empty()
            && body["allowed_mentions"]["replied_user"] == false,
        "The server owns snapshots, forwards have independent nonces and snapshot mentions do not notify recipients"
    );
    require(
        !api::forwardMessageBody({"20", "10", ""}, "1235")["message_reference"].contains("guild_id"),
        "Private sources omit server identity"
    );
    for (const api::ForwardReference& invalid :
         {api::ForwardReference{"", "10", "1"}, {"../20", "10", "1"}, {"20", "", "1"}, {"20", "10", "@me"}}) {
      require(!api::validForwardReference(invalid), "Every source identity is validated");
      bool rejected = false;
      try {
        api::forwardMessageBody(invalid, "1234");
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      require(rejected, "Invalid source references never become requests");
    }
    api::Message message{
        .id = "20",
        .channelId = "10",
        .author = "Original author",
        .content = "Snapshot text",
        .timestamp = "2026-11-10T10:00:00Z",
        .authorId = "7"
    };
    for (const int type : {0, 19, 20, 23}) {
      message.type = type;
      require(api::canForwardMessage(message), "Basic messages and replies support forwarding");
    }
    for (const int type : {3, 6, 7, 18, 21, 24, 46}) {
      message.type = type;
      require(!api::canForwardMessage(message), "System and bound messages cannot be forwarded");
    }
    message.type = 0;
    for (const std::uint64_t flags : {1ULL << 6, 1ULL << 7}) {
      message.flags = flags;
      require(!api::canForwardMessage(message), "Private and loading messages cannot be forwarded");
    }
    message.flags = 0;
    message.deleted = true;
    require(!api::canForwardMessage(message), "Deleted messages cannot be forwarded");
    message.deleted = false;
    message.poll.emplace();
    require(!api::canForwardMessage(message), "Polls cannot be forwarded");
    message.poll.reset();
    api::parseMessagePatch({{"activity", nlohmann::json::object()}}).apply(message);
    api::parseMessagePatch({{"content", "Updated"}}).apply(message);
    require(!api::canForwardMessage(message), "Partial edits preserve activity binding restrictions");
    api::parseMessagePatch({{"activity", nullptr}, {"call", nlohmann::json::object()}}).apply(message);
    require(!api::canForwardMessage(message), "Call bindings are independently retained");
    api::parseMessagePatch({{"call", nullptr}}).apply(message);
    require(api::canForwardMessage(message), "Explicit null clears obsolete bindings");
    message.attachments.push_back({.id = "5", .filename = "image.png"});
    message.mentions = {{"7", "Original author"}};
    const auto snapshot = api::forwardPreview(message, source);
    require(
        snapshot.available
            && snapshot.message == "20"
            && snapshot.channel == "10"
            && snapshot.guild == "1"
            && snapshot.content == message.content
            && snapshot.attachments.size() == 1
            && snapshot.mentions == message.mentions,
        "Pending forwards keep bounded presentation data without uploading or copying an author identity"
    );
    api::Message response{.id = "30", .channelId = "11"};
    response.forwarded = snapshot;
    require(api::confirmsForward(response, source), "Delivery confirmation matches the source reference");
    response.forwarded->guild.clear();
    require(
        api::confirmsForward(response, source),
        "An omitted optional server field retains the validated source channel identity"
    );
    response.forwarded->guild = "99";
    require(!api::confirmsForward(response, source), "An explicitly different source server cannot confirm delivery");
    response.forwarded->guild = source.guild;
    response.forwarded->channel = "99";
    require(!api::confirmsForward(response, source), "A different source channel cannot confirm a forward");
    response.forwarded.reset();
    require(!api::confirmsForward(response, source), "An ordinary message cannot confirm a forward");
    message.forwarded = snapshot;
    const auto again = api::forwardPreview(message, {"20", "10", "1"});
    require(
        again.content == snapshot.content && again.attachments == snapshot.attachments,
        "Reforwarding preserves the received snapshot instead of inventing nested authors"
    );
    std::cout << "Forward payload, source eligibility, snapshots and confirmation checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
