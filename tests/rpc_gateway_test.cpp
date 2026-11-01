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
    CurlRuntime() { require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "Private HTTP runtime initializes"); }
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
    const auto activity = rpc::parseActivity(
        "44",
        {{"type", 2},
         {"name", "Synthetic player"},
         {"details", "An offline track"},
         {"timestamps", {{"start", 1793534400}, {"end", 1793534640}}},
         {"assets", {{"large_image", "123"}}},
         {"buttons",
          nlohmann::json::array(
              {{{"label", "Website"}, {"url", "https://example.com/music"}},
               {{"label", "Source"}, {"url", "https://example.com/source"}}}
          )}}
    );
    require(activity.has_value(), "A synthetic activity validates");
    {
      api::Gateway gateway(
          generation,
          [&](std::uint64_t session, const std::string& type, const nlohmann::json&) {
            try {
              if (type == "READY" && session == 1) {
                require(
                    !active->setActivities(0, {*activity}) && !active->setActivities(2, {*activity}),
                    "Other account generations cannot publish activities"
                );
                require(active->setActivities(1, {*activity}), "The active generation publishes activity");
              } else if (type == "RPC_CLEAR") {
                require(active->setActivities(1, {}), "Clearing activity retains ordinary presence");
              } else if (type == "RPC_RESET") {
                generation = 2;
                active->reset(1);
                require(!active->setActivities(1, {*activity}), "A reset rejects the old generation");
                active->authenticate(2, "synthetic-rpc-next-token", {"idle", "A second status"});
              } else if (type == "RPC_COMPLETE") {
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
      require(!gateway.setActivities(1, {*activity}), "Activity publication needs authentication");
      gateway.authenticate(1, "synthetic-rpc-token", {"dnd", "Keep this status"});
      std::unique_lock lock(mutex);
      require(
          changed.wait_for(lock, std::chrono::seconds(12), [&] { return complete || !error.empty(); }),
          "The private RPC Gateway scenario completes within its deadline"
      );
      require(error.empty(), error.c_str());
      lock.unlock();
      generation = 3;
      gateway.reset(2);
    }
    std::cout << "Activity Gateway publication, custom-status coexistence, clearing and session isolation passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
