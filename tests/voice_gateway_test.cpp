#include "api/gateway.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <curl/curl.h>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }
  struct CurlRuntime {
    CurlRuntime() {
      require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "The synthetic Gateway runtime initializes");
    }
    ~CurlRuntime() { curl_global_cleanup(); }
  };
} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "A private Gateway endpoint is required");
    CurlRuntime runtime;
    std::atomic<std::uint64_t> generation{1};
    std::mutex mutex;
    std::condition_variable changed;
    bool complete = false;
    std::string error;
    api::Gateway* active = nullptr;
    {
      api::Gateway gateway(
          generation,
          [&](std::uint64_t session, const std::string& type, const nlohmann::json&) {
            try {
              if (type == "READY" && session == 1) {
                for (const auto* invalid : {"x", "1/2", "-1", "1?", "123456789012345678901", "1\n"}) {
                  require(
                      !active->setVoice(1, invalid, "10", false, false),
                      "Malformed guild IDs cannot enter the voice queue"
                  );
                  require(
                      !active->setVoice(1, "1", invalid, false, false),
                      "Malformed channel IDs cannot enter the voice queue"
                  );
                }
                require(
                    !active->setVoice(0, "1", "10", false, false) && !active->setVoice(2, "1", "10", false, false),
                    "Stale and future sessions cannot choose a voice target"
                );
                require(active->setVoice(1, "1", "10", true, false), "The current session can explicitly join voice");
              } else if (type == "STREAM_TEST_START") {
                require(
                    !active->stream(0, {api::StreamCommandKind::Create, {"1", "10", "500"}})
                        && !active->stream(1, {api::StreamCommandKind::Create, {"1", "11", "500"}}),
                    "Stream commands reject stale generations and other voice targets"
                );
                require(
                    active->stream(1, {api::StreamCommandKind::Create, {"1", "10", "500"}}),
                    "An explicit stream uses the established main voice target"
                );
              } else if (type == "STREAM_TEST_WATCH") {
                require(
                    active->stream(1, {api::StreamCommandKind::Watch, {"1", "10", "501"}}),
                    "Watching another stream has its own main Gateway command"
                );
              } else if (type == "STREAM_TEST_PAUSE") {
                require(
                    active->stream(1, {api::StreamCommandKind::Pause, {"1", "10", "500"}, true}),
                    "Pausing queues an explicit stream intention"
                );
              } else if (type == "STREAM_TEST_PING") {
                require(
                    active->stream(1, {api::StreamCommandKind::Ping, {"1", "10", "500"}}),
                    "The main Gateway accepts bounded stream liveness checks"
                );
              } else if (type == "STREAM_TEST_STOP") {
                require(
                    active->stream(1, {api::StreamCommandKind::Delete, {"1", "10", "500"}}),
                    "Stopping publication uses the same validated stream target"
                );
              } else if (type == "VOICE_BURST") {
                require(session == 1, "Rapid voice changes retain the authenticated session");
                require(
                    active->stream(1, {api::StreamCommandKind::Watch, {"1", "10", "501"}}),
                    "Changing voice channels may retire a queued stream watch"
                );
                for (unsigned update = 0; update < 64; ++update)
                  require(
                      active->setVoice(1, "1", std::to_string(100 + update), update % 2, update % 3 == 0),
                      "Rapid target and flag changes are accepted as latest intentions"
                  );
                require(
                    active->setVoice(1, "2", "20", false, true),
                    "The final rapid voice target replaces queued intermediate targets"
                );
              } else if (type == "VOICE_LEAVE") {
                require(active->setVoice(1, "2", "", false, false), "An explicit leave clears the selected channel");
              } else if (type == "VOICE_DM") {
                require(active->setVoice(1, "", "30", true, true), "A direct voice conversation has no guild ID");
              } else if (type == "VOICE_RESET") {
                require(
                    active->setVoice(1, "1", "998", false, false),
                    "The old session can have a pending target before reset"
                );
                require(
                    active->stream(1, {api::StreamCommandKind::Watch, {"1", "998", "501"}}),
                    "A stream command can be pending when the account resets"
                );
                generation = 2;
                active->reset(1);
                require(!active->setVoice(1, "1", "999", false, false), "Reset rejects the former session immediately");
                active->authenticate(2, "synthetic-voice-gateway-next-token", {"online"});
              } else if (type == "READY" && session == 2) {
                active->reset(1);
                require(
                    !active->setVoice(1, "1", "999", false, false), "A stale reset cannot make the old session active"
                );
              } else if (type == "VOICE_SECOND_JOIN") {
                require(
                    session == 2 && active->setVoice(2, "3", "40", false, false),
                    "The new session joins only after a fresh explicit choice"
                );
              } else if (type == "VOICE_FINAL_LEAVE") {
                require(active->setVoice(2, "3", "", false, false), "The new session can leave normally");
              } else if (type == "VOICE_COMPLETE") {
                std::lock_guard lock(mutex);
                complete = true;
                changed.notify_all();
              }
            } catch (const std::exception& failure) {
              std::lock_guard lock(mutex);
              error = failure.what();
              changed.notify_all();
            }
          },
          argv[1]
      );
      active = &gateway;
      require(!gateway.setVoice(1, "1", "10", false, false), "Voice requests are unavailable before authentication");
      gateway.authenticate(1, "synthetic-voice-gateway-token", {"online"});
      std::unique_lock lock(mutex);
      require(
          changed.wait_for(lock, std::chrono::seconds{14}, [&] { return complete || !error.empty(); }),
          "The loopback voice Gateway scenario completes within its deadline"
      );
      require(error.empty(), error.c_str());
      lock.unlock();
      generation = 3;
      gateway.reset(2);
    }
    std::cout << "Main Gateway voice frames, pacing, coalescing, leave and session reset passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
