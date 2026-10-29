#include "api/inbox.h"
#include "api/request_diagnostics.h"

#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

int main() {
  using namespace api::request_diagnostics;
  bool valid = true;
  const auto require = [&](bool condition, const char* message) {
    if (!condition) {
      std::cerr << message << '\n';
      valid = false;
    }
  };
  const auto line = format(
      {.id = nextId(),
       .action = "messages",
       .method = "POST",
       .path = "/channels/123456789012345678/messages/987654321098765432/ack?token=private-query",
       .status = 403,
       .serviceCode = 40002,
       .durationMs = 123,
       .attempt = 1,
       .outcome = Outcome::Restricted}
  );
  require(line.contains("route=/channels/:id/messages/:id/ack"), "Known endpoints retain a useful fixed template");
  require(line.contains("status=403 service_code=40002 category=restricted"), "Failures retain safe numeric context");
  for (const auto secret : {"123456789012345678", "987654321098765432", "private-query", "token="})
    require(!line.contains(secret), "Route identities and query values never enter output");
  require(route("/invites/private-invite") == "/invites/:code", "Invitation codes are opaque");
  require(route("/webhooks/123/private-token") == "/webhooks/:id/:token", "Webhook tokens are opaque");
  require(
      route("/channels/123/messages/456/reactions/private-emoji/@me")
          == "/channels/:id/messages/:id/reactions/:emoji/@me",
      "Emoji names are opaque"
  );
  require(route("/users/@me/channels") == "/users/@me/channels", "Literal account endpoints stay specific");
  require(
      route("/channels/123/application-command-index") == "/channels/:id/application-command-index"
          && route("/users/@me/application-command-index") == "/users/@me/application-command-index"
          && route("/channels/123/attachments") == "/channels/:id/attachments"
          && action("application-commands") == "application-commands",
      "Command discovery and attachment staging retain fixed diagnostic labels without identities"
  );
  require(route("/channels//messages") == "/unclassified", "Empty identities never masquerade as useful routes");
  for (const auto input : {"https://private.example/secret", "/private-endpoint/secret", "", "/channels/123/messages/"})
    require(route(input) == "/unclassified", "Unknown or malformed routes are entirely hidden");
  require(route(std::string(8192, 'x')) == "/unclassified", "Route parsing has a fixed size limit");
  const auto hidden = format(
      {.id = nextId(),
       .action = "private-action\nINJECT",
       .method = "private-method",
       .path = "/private-route",
       .status = 99999,
       .serviceCode = -1,
       .durationMs = -10,
       .attempt = 99,
       .outcome = Outcome::Cancelled,
       .transportCode = std::numeric_limits<int>::max()}
  );
  require(hidden.contains("action=other method=OTHER route=/unclassified"), "Labels must come from fixed allowlists");
  require(!hidden.contains("private") && !hidden.contains('\n'), "Arbitrary labels cannot inject log text");
  require(
      hidden.contains("status=0 service_code=0") && hidden.contains("duration_ms=0 attempt=3/3"),
      "Numeric diagnostic fields have bounded representations"
  );
  const auto initialBookmarks = std::format("/users/@me/saved-messages/bookmarks?limit={}", api::inboxPageSize);
  constexpr auto initialReminders = "/users/@me/saved-messages?bookmark_ids=true";
  for (const auto& path : {initialBookmarks, std::string(initialReminders)}) {
    require(
        expectedEmptyCollection("GET", path, 404, 10133, api::inboxPageSize),
        "Only the recognized initial missing collection is an expected empty read"
    );
    require(
        !expectedEmptyCollection("PUT", path, 404, 10133, api::inboxPageSize),
        "Collection mutation failures remain failures"
    );
    require(
        !expectedEmptyCollection("GET", path, 404, 10003, api::inboxPageSize), "Other not-found codes remain failures"
    );
    require(
        !expectedEmptyCollection("GET", path, 403, 10133, api::inboxPageSize),
        "Permission failures cannot be classified as empty"
    );
  }
  for (const auto& path :
       {initialBookmarks + "&before=123", initialBookmarks + "&before=", std::string(initialReminders) + "&before=123",
        std::string("/users/@me/saved-messages/bookmarks?limit=1"), std::string("/users/@me/saved-messages/123")})
    require(
        !expectedEmptyCollection("GET", path, 404, 10133, api::inboxPageSize),
        "Pagination, other query shapes and item routes remain failures"
    );
  require(
      route(initialBookmarks) == "/users/@me/saved-messages/bookmarks",
      "The saved bookmark collection retains its specific fixed route"
  );
  require(
      category(Outcome::ExpectedEmpty) == "expected-empty",
      "Expected empty diagnostics retain an accurate distinct category"
  );
  std::mutex mutex;
  std::set<std::uint64_t> ids;
  std::vector<std::jthread> workers;
  constexpr int workerCount = 4, requestsPerWorker = 256;
  for (int worker = 0; worker < workerCount; ++worker)
    workers.emplace_back([&] {
      for (int request = 0; request < requestsPerWorker; ++request) {
        const auto id = nextId();
        std::lock_guard lock(mutex);
        ids.insert(id);
      }
    });
  workers.clear();
  require(ids.size() == workerCount * requestsPerWorker, "Concurrent workers receive unique process-local IDs");
  return valid ? 0 : 1;
}
