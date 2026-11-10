#pragma once

#include "updates/model.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>

namespace updates {
  enum class Phase { Idle, Checking, Current, Available, Downloading, Ready, Installing, Installed, Error };
  struct Snapshot {
    Phase phase = Phase::Idle;
    std::optional<Release> release;
    std::uint64_t downloaded = 0, total = 0;
    std::string error, backup;
  };
  using Sink = std::function<void(std::span<const unsigned char>)>;
  using Fetch = std::function<void(const std::string&, const Sink&, std::stop_token)>;
  struct Options {
    Installation installation;
    std::string currentVersion;
    Fetch fetch;
  };
  class Updater {
  public:
    explicit Updater(Options options);
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;
    bool check(bool prereleases = false);
    bool download();
    bool install();
    void cancel();
    std::optional<Snapshot> poll();
    int eventFd() const;
    const Installation& installation() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };
  void fetchHttps(const std::string& url, const Sink& sink, std::stop_token stop);
} // namespace updates
