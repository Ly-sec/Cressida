#pragma once

#include "core/process/wake_event.h"
#include "rpc/activity.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rpc {
  struct Update {
    std::uint64_t session = 0, connection = 0, revision = 0;
    std::string application, command, nonce, user;
    std::optional<Activity> activity;
    bool disconnected = false;
  };
  struct Configuration {
    std::uint64_t session = 0;
    nlohmann::json user;
    bool enabled = false;
    std::vector<std::string> blocked;
    bool operator==(const Configuration&) const = default;
  };
  class Server {
  public:
    explicit Server(std::filesystem::path directory = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    void configure(Configuration configuration);
    std::vector<Update> drain();
    void complete(const Update& update, const std::optional<Activity>& activity, std::string error = {});
    void dispatch(const std::string& application, const std::string& event, nlohmann::json data);
    int eventFd() const;
    std::string endpoint() const;
    std::string error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };
} // namespace rpc
