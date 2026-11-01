#include "rpc/server.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
  using Json = nlohmann::json;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  void sendFrame(int fd, unsigned opcode, const Json& value) {
    const auto body = value.dump();
    std::string frame;
    for (const auto number : {opcode, static_cast<unsigned>(body.size())})
      for (unsigned shift = 0; shift < 32; shift += 8)
        frame.push_back(static_cast<char>((number >> shift) & 255));
    frame += body;
    for (const char byte : frame)
      require(::send(fd, &byte, 1, MSG_NOSIGNAL) == 1, "Fragmented private IPC frames are accepted");
  }
  Json receiveFrame(int fd) {
    std::string bytes;
    auto read = [&](std::size_t count) {
      while (bytes.size() < count) {
        pollfd descriptor{fd, POLLIN, 0};
        require(::poll(&descriptor, 1, 2000) > 0, "A private RPC peer responds within its deadline");
        char chunk[4096];
        const auto size = ::recv(fd, chunk, std::min(sizeof(chunk), count - bytes.size()), 0);
        require(size > 0, "The private RPC peer remains connected");
        bytes.append(chunk, static_cast<std::size_t>(size));
      }
    };
    read(8);
    unsigned length = 0;
    for (unsigned i = 0; i < 4; ++i)
      length |= static_cast<unsigned>(static_cast<unsigned char>(bytes[i + 4])) << (i * 8);
    require(length <= rpc::maximumActivityBytes, "RPC replies stay bounded");
    read(8 + length);
    return Json::parse(bytes.substr(8));
  }
} // namespace
int main() {
  try {
    const Json fields{
        {"type", 2},
        {"name", "Synthetic music"},
        {"details", "An offline track"},
        {"state", "A synthetic artist"},
        {"timestamps", {{"start", 100}, {"end", 200}}},
        {"assets", {{"large_image", "album"}, {"large_text", "Album artwork"}}},
        {"buttons", Json::array({{{"label", "Website"}, {"url", "https://example.com/music"}}})},
        {"party", {{"id", "synthetic-party"}, {"size", {1, 4}}}},
        {"secrets", {{"join", "synthetic-join"}}}
    };
    const auto activity = rpc::parseActivity("123", fields);
    require(activity.has_value() && activity->fields == fields, "Rich activity fields survive validation");
    auto clock = rpc::activityClock(*activity, 150000);
    require(
        clock
            && clock->progress
            && clock->elapsed == 50
            && clock->total == 100
            && clock->remaining == 50
            && clock->fraction == 0.5
            && clock->ticking,
        "Activity progress uses elapsed and total duration, independent of rendering"
    );
    clock = rpc::activityClock(*activity, 300000);
    require(
        clock && clock->elapsed == 100 && clock->fraction == 1 && !clock->ticking,
        "Completed activity clocks clamp to the end and stop scheduling ticks"
    );
    clock = rpc::activityClock(*activity, 50000);
    require(
        clock && clock->elapsed == 0 && clock->fraction == 0,
        "A future activity start never produces negative elapsed time or progress"
    );
    auto timed = *activity;
    constexpr std::int64_t epoch = 1793534400000LL;
    timed.fields["timestamps"] = {{"start", epoch}, {"end", epoch + 240000}};
    clock = rpc::activityClock(timed, epoch + 60000);
    require(
        clock && clock->elapsed == 60 && clock->total == 240 && clock->fraction == 0.25,
        "Gateway millisecond timestamps retain their units"
    );
    timed.fields["timestamps"] = {{"start", epoch}};
    clock = rpc::activityClock(timed, epoch + 60000);
    require(
        clock && !clock->progress && !clock->countdown && clock->elapsed == 60,
        "Start-only activities show elapsed time without inventing a duration"
    );
    timed.fields["timestamps"] = {{"end", epoch + 60000}};
    clock = rpc::activityClock(timed, epoch);
    require(
        clock && !clock->progress && clock->countdown && clock->remaining == 60,
        "End-only activities retain their remaining-time display"
    );
    timed.fields["timestamps"] = {{"start", epoch}, {"end", epoch}};
    clock = rpc::activityClock(timed, epoch);
    require(clock && !clock->progress && !clock->ticking, "Zero-duration activities cannot divide by zero");
    timed.fields["timestamps"] = {{"start", epoch + 1}, {"end", epoch}};
    require(!rpc::activityClock(timed, epoch), "Invalid reversed clocks never draw progress");
    require(
        rpc::activityTime(0) == "0:00" && rpc::activityTime(782) == "13:02" && rpc::activityTime(3601) == "1:00:01",
        "Activity time labels preserve minutes and hours"
    );
    auto received = rpc::gatewayActivity(*activity);
    require(
        received["buttons"] == Json::array({"Website"})
            && received["metadata"]["button_urls"] == Json::array({"https://example.com/music"}),
        "Personal-account Gateway activities keep button labels separate from their indexed link metadata"
    );
    received["buttons"] = Json::array({"Website"});
    received["metadata"] = {{"button_urls", {"https://example.com/music"}}};
    received["application_id"] = nullptr;
    received["instance"] = nullptr;
    received["status_display_type"] = nullptr;
    received["assets"]["small_text"] = nullptr;
    const auto imported = rpc::receivedActivities(Json::array({received}));
    require(
        imported.size() == 1
            && imported.front().application.empty()
            && imported.front().fields["buttons"][0]["url"] == "https://example.com/music"
            && !imported.front().fields.contains("secrets"),
        "Received provider activities support missing application IDs and optional null fields"
    );
    received.erase("metadata");
    received["session_id"] = "synthetic-session";
    const auto labelsOnly = rpc::receivedActivities(Json::array({received}));
    require(
        labelsOnly.size() == 1
            && labelsOnly.front().fields["buttons"].size() == 1
            && labelsOnly.front().fields["buttons"][0]["label"] == "Website"
            && !labelsOnly.front().fields["buttons"][0].contains("url"),
        "Received button labels remain renderable before their metadata is requested"
    );
    require(
        rpc::activityMetadataPath("7", labelsOnly.front())
            == "/users/7/sessions/synthetic-session/activities/0/metadata",
        "Provider activity metadata uses the activity session and zero for an absent application"
    );
    auto target = labelsOnly.front();
    target.application = "123";
    require(
        rpc::activityMetadataPath("7", target) == "/users/7/sessions/synthetic-session/activities/123/metadata",
        "Application activity metadata retains its scoped identity"
    );
    target.session = "../private?token=x";
    require(!rpc::activityMetadataPath("7", target), "Opaque activity sessions cannot alter the fixed metadata route");
    received["metadata"] = {{"button_urls", Json::array()}};
    require(
        rpc::receivedActivities(Json::array({received})).front().fields["buttons"].size() == 1,
        "Empty inline link metadata never hides the published button label"
    );
    require(
        rpc::activityButtonUrls({{"button_urls", {"https://example.com/music"}}}).has_value()
            && !rpc::activityButtonUrls({{"button_urls", {"file:///private"}}})
            && !rpc::activityButtonUrls({{"button_urls", {42}}}),
        "Button metadata projects only validated web links"
    );
    require(
        !rpc::savedActivity(labelsOnly.front()).contains("session_id"),
        "Received activity session identifiers stay out of saved editor data"
    );
    require(
        rpc::savedActivity(*activity).contains("buttons") && !rpc::savedActivity(*activity).contains("secrets"),
        "Saved custom activities retain buttons without persisting session secrets"
    );
    auto invalid = fields;
    invalid["buttons"][0]["url"] = "file:///private";
    require(!rpc::parseActivity("123", invalid), "Activity links reject non-web schemes");
    invalid = fields;
    invalid["party"]["size"] = {8, 2};
    require(!rpc::parseActivity("123", invalid), "Party sizes retain valid current and maximum bounds");
    invalid = fields;
    invalid["buttons"].push_back(invalid["buttons"][0]);
    invalid["buttons"].push_back(invalid["buttons"][0]);
    require(!rpc::parseActivity("123", invalid), "Rich activities support at most two buttons");
    char directory[] = "/tmp/cressida-rpc-test.XXXXXX";
    require(::mkdtemp(directory), "RPC tests use a private runtime directory");
    {
      rpc::Server server(directory);
      server.configure({1, {{"id", "7"}, {"username", "Synthetic account"}}, true});
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (server.endpoint().empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      require(!server.endpoint().empty(), "Explicit enablement binds a private compatibility socket");
      struct stat permissions{};
      require(
          ::lstat(server.endpoint().c_str(), &permissions) == 0 && (permissions.st_mode & 0777) == 0600,
          "Only the owning user can access the local RPC socket"
      );
      const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      std::strcpy(address.sun_path, server.endpoint().c_str());
      require(
          ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "A synthetic local app connects"
      );
      sendFrame(fd, 0, {{"v", 1}, {"client_id", "123"}});
      const auto ready = receiveFrame(fd);
      require(
          ready["evt"] == "READY" && ready["data"]["user"]["id"] == "7", "IPC handshake returns scoped account identity"
      );
      sendFrame(
          fd, 1, {{"cmd", "SET_ACTIVITY"}, {"nonce", "one"}, {"args", {{"pid", ::getpid()}, {"activity", fields}}}}
      );
      pollfd event{server.eventFd(), POLLIN, 0};
      require(::poll(&event, 1, 2000) > 0, "Application activity wakes the owning loop");
      auto updates = server.drain();
      while (updates.empty() && std::chrono::steady_clock::now() < deadline) {
        require(::poll(&event, 1, 2000) > 0, "Activity delivery stays bounded");
        updates = server.drain();
      }
      require(
          updates.size() == 1 && updates.front().activity == activity,
          "Validated application activity arrives on the owning loop"
      );
      server.complete(updates.front(), updates.front().activity);
      const auto acknowledged = receiveFrame(fd);
      require(
          acknowledged["nonce"] == "one" && acknowledged["evt"].is_null(), "Activity acknowledgement retains its nonce"
      );
      require(
          acknowledged["data"]["buttons"] == fields["buttons"],
          "IPC replies retain label/URL objects instead of the personal-account Gateway representation"
      );
      sendFrame(fd, 1, {{"cmd", "SUBSCRIBE"}, {"evt", "ACTIVITY_JOIN"}, {"nonce", "subscribe"}});
      require(receiveFrame(fd)["nonce"] == "subscribe", "Activity event subscriptions are acknowledged");
      server.dispatch("123", "ACTIVITY_JOIN", {{"secret", "synthetic-secret"}});
      require(
          receiveFrame(fd)["data"]["secret"] == "synthetic-secret",
          "Join events reach only subscribed application peers"
      );
      ::close(fd);
      server.configure({});
    }
    require(::rmdir(directory) == 0, "Stopping RPC removes only its own socket");
    std::cout << "Private RPC framing, activity validation, subscriptions and lifecycle passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
