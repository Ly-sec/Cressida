#include "api/client.h"
#include "api/gateway.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace {
  constexpr auto completionDeadline = std::chrono::seconds(12);
  constexpr std::string_view originalToken = "synthetic-original-credential";
  constexpr std::string_view replacementToken = "synthetic-replacement-credential";
} // namespace

int main(int argc, char** argv) {
  if (argc != 3)
    return 2;
  const std::string_view mode = argv[2];
  if (mode != "rotation" && mode != "late-failure" && mode != "reset" && mode != "unchanged" && mode != "account")
    return 2;
  api::Client runtime;
  std::atomic<std::uint64_t> generation{1};
  std::mutex mutex;
  std::condition_variable changed;
  bool complete = false, valid = true;
  unsigned readyCount = 0, failureCount = 0;
  api::Gateway* active = nullptr;
  {
    api::Gateway gateway(
        generation,
        [&](auto session, const auto& type, const auto& data) {
          std::lock_guard lock(mutex);
          if (session != generation)
            valid = false;
          if (type == "READY" && ++readyCount == 1) {
            if (mode == "rotation")
              active->authenticate(1, std::string(replacementToken));
            else if (mode == "reset") {
              active->reset(1);
              active->authenticate(1, std::string(originalToken), {"dnd"});
            } else if (mode == "unchanged")
              active->authenticate(1, std::string(originalToken));
            else if (mode == "account") {
              ++generation;
              active->authenticate(2, std::string(replacementToken), {"dnd"});
            }
          }
          if (type == "CONNECTION_FAILED") {
            if (mode != "late-failure" || ++failureCount != 1 || !data.value("authentication_failed", false))
              valid = false;
            else
              active->authenticate(1, std::string(replacementToken));
          }
          if (type == "AUTHENTICATION_TEST_COMPLETE") {
            complete = true;
            changed.notify_all();
          }
        },
        argv[1]
    );
    active = &gateway;
    gateway.authenticate(1, std::string(originalToken), {"dnd"});
    std::unique_lock lock(mutex);
    if (!changed.wait_for(lock, completionDeadline, [&] { return complete; })) {
      std::cerr << "Gateway authentication transition did not complete\n";
      valid = false;
    }
    if (readyCount != (mode == "unchanged" ? 1U : 2U)) {
      std::cerr << "Gateway authentication transition repeated or lost READY\n";
      valid = false;
    }
    lock.unlock();
    ++generation;
    gateway.reset();
  }
  return valid ? 0 : 1;
}
