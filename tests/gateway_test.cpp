#include "api/client.h"
#include "api/gateway.h"
#include "api/role_styles.h"
#include "api/thread_member_list.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>

namespace {
  bool checkRateLimitCancellation(const std::string& endpoint) {
    std::atomic<std::uint64_t> generation{1};
    std::mutex mutex;
    std::condition_variable changed;
    bool complete = false;
    auto cancelAt = std::chrono::steady_clock::now();
    {
      api::Gateway gateway(
          generation,
          [&](auto, const auto& type, const auto& data) {
            std::lock_guard lock(mutex);
            if (type == "RECONNECTING" && data.value("reason", "") == "Gateway closed (4008)") {
              complete = true;
              changed.notify_all();
            }
          },
          endpoint
      );
      gateway.authenticate(1, "synthetic-token", {"online"});
      std::unique_lock lock(mutex);
      if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return complete; })) {
        std::cerr << "Gateway did not report its rate-limit cooldown\n";
        return false;
      }
      lock.unlock();
      cancelAt = std::chrono::steady_clock::now();
      ++generation;
      gateway.reset();
    }
    if (std::chrono::steady_clock::now() - cancelAt > std::chrono::milliseconds(500)) {
      std::cerr << "Gateway cooldown delayed cancellation\n";
      return false;
    }
    return true;
  }
  bool checkThreadSubscription(const std::string& endpoint) {
    std::atomic<std::uint64_t> generation{1};
    std::mutex mutex;
    std::condition_variable changed;
    bool complete = false, valid = true, received = false, typing = false;
    api::Gateway* active = nullptr;
    api::Gateway gateway(
        generation,
        [&](auto, const auto& type, const auto& data) {
          std::lock_guard lock(mutex);
          if (type == "THREAD_MEMBER_LIST_UPDATE") {
            const auto rows = api::threadMemberList(data, "1", "40");
            received = rows
                && rows->size() == 2
                && rows->front().user.name == "Thread nickname"
                && rows->front().status == "online";
            valid &= received;
            active->subscribe("1", "2", 0);
          }
          if (type == "COALESCE_START")
            active->subscribe("1", "3", 0);
          if (type == "COALESCE_FINISH") {
            for (int update = 0; update < 20; ++update) {
              active->setPresence(1, update % 2 ? "dnd" : "online");
              active->subscribe("1", std::to_string(10 + update), 0);
            }
            active->setPresence(1, "idle");
            active->subscribe("1", "4", 0);
          }
          if (type == "TYPING_START") {
            const auto event = api::parseTypingStart(data);
            typing = event
                && event->channel == "40"
                && event->guild == "1"
                && event->user == "7"
                && event->name == "Thread nickname";
          }
          if (type == "THREAD_TEST_COMPLETE") {
            complete = true;
            changed.notify_all();
          }
        },
        endpoint
    );
    active = &gateway;
    gateway.authenticate(1, "synthetic-token", {"online"});
    gateway.subscribe("1", "40", 0, false, true);
    std::unique_lock lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return complete; })) {
      std::cerr << "Thread member subscription did not complete\n";
      valid = false;
    }
    const bool result = valid && received && typing;
    lock.unlock();
    ++generation;
    gateway.reset();
    return result;
  }
  bool checkPresenceRestore(const std::string& endpoint, int phase) {
    std::atomic<std::uint64_t> generation{1};
    std::mutex mutex;
    std::condition_variable changed;
    bool complete = false, valid = true;
    api::Gateway* active = nullptr;
    api::Gateway gateway(
        generation,
        [&](auto, const auto& type, const auto&) {
          std::lock_guard lock(mutex);
          if (type == "RESTORE_PRESENCE") {
            if (phase == 1)
              valid = active->restorePresence(1, {"online", "late custom status", "", "🌙"}) && valid;
            else if (phase == 2) {
              valid = active->setPresence(1, "online") && valid;
              valid = !active->restorePresence(1, {"idle", "stale", "77", "wave"}) && valid;
            }
          }
          if (type == "RESTORE_COMPLETE") {
            valid = !active->restorePresence(1, {"idle", "second restore", "", ""}) && valid;
            complete = true;
            changed.notify_all();
          }
        },
        endpoint
    );
    active = &gateway;
    gateway.authenticate(1, "synthetic-token");
    gateway.subscribe("1", "2", 0);
    if (phase == 0) {
      std::lock_guard lock(mutex);
      valid = !gateway.restorePresence(2, {"online", "stale session", "", ""}) && valid;
      valid = !gateway.restorePresence(1, {"invalid", "", "", ""}) && valid;
      valid = !gateway.restorePresence(1, {"online", "", "invalid", "wave"}) && valid;
      valid = gateway.restorePresence(1, {"idle", "early custom status", "77", "wave"}) && valid;
    }
    std::unique_lock lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return complete; })) {
      std::cerr << "Gateway presence restoration did not complete phase " << phase << '\n';
      valid = false;
    }
    const bool result = valid;
    lock.unlock();
    ++generation;
    gateway.reset();
    return result;
  }
} // namespace

int main(int argc, char** argv) {
  if (argc != 2)
    return 2;
  // Initialize libcurl through the same process runtime as the actual client.
  api::Client runtime;
  for (const auto* address :
       {"wss://gateway.discord.gg", "wss://gateway.discord.gg/", "wss://gateway-us-east1-b.discord.gg"})
    if (!api::gatewayResumeEndpoint(address)) {
      std::cerr << "Trusted Gateway resume route was rejected\n";
      return 1;
    }
  if (api::gatewayResumeEndpoint("wss://gateway-us-east1-b.discord.gg/")
      != "wss://gateway-us-east1-b.discord.gg/?v=9&encoding=json") {
    std::cerr << "Gateway resume route did not retain the protocol version and encoding\n";
    return 1;
  }
  for (const auto* address :
       {"ws://gateway.discord.gg",
        "https://gateway.discord.gg",
        "wss://gateway.discord.gg.example.com",
        "wss://gateway-us-east1-b.discord.gg.example.com",
        "wss://gateway.discord.gg@evil.example",
        "wss://user@gateway.discord.gg",
        "wss://gateway.discord.gg:443",
        "wss://gateway.discord.gg/private",
        "wss://gateway.discord.gg?token=secret",
        "wss://gateway.discord.gg#fragment",
        "wss://gateway.discord.gg//",
        "wss://gateway-.discord.gg",
        "wss://gateway--us-east1-b.discord.gg",
        "wss://gateway-us-.discord.gg",
        "wss://gateway-us.evil.discord.gg",
        "wss://gateway%2dus.discord.gg",
        "wss://gateway-us\\.discord.gg",
        "wss://gateway-us\n.discord.gg",
        "wss://127.0.0.1:8765",
        "wss://cdn.discord.gg",
        ""})
    if (api::gatewayResumeEndpoint(address)) {
      std::cerr << "Untrusted Gateway resume route was accepted\n";
      return 1;
    }
  if (api::gatewayResumeEndpoint("wss://gateway-" + std::string(64, 'x') + ".discord.gg")) {
    std::cerr << "Overlong Gateway hostname label was accepted\n";
    return 1;
  }
  for (int phase = 0; phase < 3; ++phase)
    if (!checkPresenceRestore(argv[1], phase))
      return 1;
  if (!checkThreadSubscription(argv[1]) || !checkRateLimitCancellation(argv[1]))
    return 1;
  std::atomic<std::uint64_t> generation{1};
  std::mutex mutex;
  std::condition_variable changed;
  std::set<std::string> events;
  bool valid = true;
  api::UserDirectory users;
  api::RoleDirectory roles;
  api::Gateway* active = nullptr;
  {
    api::Gateway gateway(
        generation,
        [&](auto session, const auto& type, const auto& data) {
          std::lock_guard lock(mutex);
          if (session != 1)
            valid = false;
          users.consume(type, data);
          roles.apply(api::parseRoleUpdates(type, data));
          if (type == "GUILD_MEMBERS_CHUNK" && roles.memberships("1", "7") != std::vector<std::string>{"50"})
            valid = false;
          if (type == "GUILD_MEMBERS_CHUNK") {
            if (data.value("nonce", "") == "complete-test")
              events.insert("MEMBER_SEARCH");
            else if (active && !active->searchMembers(1, "1", "May", "complete-test"))
              valid = false;
          }
          if (type == "READY_SUPPLEMENTAL" && users.status("7") != "online")
            valid = false;
          if (type == "MESSAGE_CREATE" && (users.status("7") != "idle" || users.name("7", "Original") != "My friend"))
            valid = false;
          if (type == "MESSAGE_DELETE" && (users.status("7") != "idle" || users.name("7", "Original") != "Original"))
            valid = false;
          if (type == "CONNECTION_FAILED" && !data.value("authentication_failed", false))
            valid = false;
          if (type == "MESSAGE_CREATE" && data.value("content", "") != "synthetic 🌿")
            valid = false;
          if (type == "SWITCH_CHANNEL" && active)
            active->subscribe("1", "3", 0);
          events.insert(type);
          changed.notify_all();
        },
        argv[1]
    );
    active = &gateway;
    gateway.authenticate(1, "synthetic-token", {"online", "working", "77", "wave"});
    gateway.reset(2); // A delayed failure from another session cannot clear this login.
    if (gateway.searchMembers(2, "1", "May", "complete-invalid")
        || gateway.searchMembers(1, "1", "", "complete-invalid")
        || gateway.searchMembers(1, "1", "May", std::string(33, 'x')))
      valid = false;
    if (!gateway.setPresence(1, "dnd") || gateway.setPresence(2, "online") || gateway.setPresence(1, "busy"))
      valid = false;
    gateway.subscribe("1", "2", 100);
    gateway.requestMembers(2, "1", {"9"});
    gateway.requestMembers(1, "../invalid", {"9"});
    gateway.requestMembers(1, "1", {"7", "7", "bad", "8"});
    gateway.requestMembers(1, "1", {"7", "8"});
    std::unique_lock lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(7), [&] {
          return events.contains("READY")
              && events.contains("RESUMED")
              && events.contains("MESSAGE_CREATE")
              && events.contains("MESSAGE_UPDATE")
              && events.contains("MESSAGE_DELETE")
              && events.contains("CONNECTION_FAILED")
              && events.contains("RELATIONSHIP_REMOVE")
              && events.contains("PRESENCE_UPDATE")
              && events.contains("GUILD_MEMBER_LIST_UPDATE")
              && events.contains("GUILD_MEMBERS_CHUNK")
              && events.contains("MEMBER_SEARCH")
              && events.contains("SWITCH_CHANNEL");
        })) {
      std::cerr << "Gateway did not complete identify, fragmented dispatch and resume\n";
      for (const auto& event : events)
        std::cerr << "Observed: " << event << '\n';
      valid = false;
    }
    lock.unlock();
    ++generation;
    gateway.reset();
  }
  return valid ? 0 : 1;
}
