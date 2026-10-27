#pragma once

#include "core/process/wake_event.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <thread>

namespace auth {
  enum class QrStage { Starting, Ready, AwaitingApproval, Completing, Authenticated, Expired, Cancelled, Failed };
  struct QrAccount {
    std::string id, name, avatar;
  };
  struct QrUpdate {
    QrStage stage = QrStage::Starting;
    std::string qrUrl;
    std::optional<QrAccount> account;
    std::string error;
    std::chrono::steady_clock::time_point expiresAt{};
  };

  // Pure session state is also exercised with synthetic encrypted peer messages.
  class QrSession {
  public:
    using Clock = std::chrono::steady_clock;
    explicit QrSession(Clock::time_point now = Clock::now());
    ~QrSession();
    QrSession(const QrSession&) = delete;
    QrSession& operator=(const QrSession&) = delete;
    std::optional<nlohmann::json> receive(const nlohmann::json& message, Clock::time_point now);
    std::optional<nlohmann::json> tick(Clock::time_point now);
    const QrUpdate& view() const;
    Clock::time_point nextDeadline() const;
    bool terminal() const;
    std::optional<std::string> takeTicket();
    void finish(const nlohmann::json& response);
    std::optional<std::string> takeToken();
    void fail(std::string error);

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };

  struct QrEndpoints {
    std::string gateway = "wss://remote-auth-gateway.discord.gg/?v=2";
    std::string login = "https://discord.com/api/v9/users/@me/remote-auth/login";
  };
  // Production endpoints are fixed. Overrides accept only private loopback peers.
  class QrLogin {
  public:
    explicit QrLogin(QrEndpoints endpoints = {});
    ~QrLogin();
    QrLogin(const QrLogin&) = delete;
    QrLogin& operator=(const QrLogin&) = delete;
    bool start();
    void cancel();
    bool active() const noexcept { return m_active; }
    int fd() const noexcept { return m_updatesWake.fd(); }
    std::optional<QrUpdate> poll();
    // Only the account-connection path consumes this credential, never drawing.
    std::optional<std::string> takeToken();

  private:
    void run(std::stop_token stop);
    void publish(const QrUpdate& update);
    QrEndpoints m_endpoints;
    process::WakeEvent m_updatesWake, m_cancelWake;
    std::mutex m_mutex;
    std::optional<QrUpdate> m_update;
    std::optional<std::string> m_token;
    std::atomic<bool> m_active = false;
    std::jthread m_worker;
  };
} // namespace auth
