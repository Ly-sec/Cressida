#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace auth {
  bool isExternalUrl(std::string_view url);
  bool openExternalUrl(const std::string& url, std::string activationToken = {});
  struct BrowserResult {
    std::string key;
    std::string handoffToken;
    std::string error;
  };

  // A short-lived loopback handoff listener. The user's browser owns the login UI.
  class BrowserLogin {
  public:
    using Opener = std::function<bool(const std::string&)>;
    explicit BrowserLogin(Opener opener = {});
    ~BrowserLogin();
    bool start();
    void cancel();
    bool active() const noexcept { return m_active; }
    std::optional<BrowserResult> poll();

  private:
    void run(std::stop_token stop, int listener, std::uint16_t port, std::string key);
    Opener m_opener;
    std::atomic<bool> m_active{false};
    std::mutex m_mutex;
    std::optional<BrowserResult> m_result;
    std::jthread m_worker;
  };
} // namespace auth
