#pragma once

#include "api/models.h"
#include "core/process/wake_event.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace api {
  // Server-issued resume routes may change the Gateway shard, never its service.
  std::optional<std::string> gatewayResumeEndpoint(std::string_view address);

  // One persistent TLS WebSocket worker, independent of REST/media requests.
  class Gateway {
  public:
    using Dispatch = std::function<void(std::uint64_t, const std::string&, const nlohmann::json&)>;
    Gateway(
        const std::atomic<std::uint64_t>& generation, Dispatch dispatch,
        std::string endpoint = "wss://gateway.discord.gg/?v=9&encoding=json"
    );
    ~Gateway();
    void authenticate(std::uint64_t session, std::string token, Presence presence = {});
    // Late failures may reset only the session that produced them.
    void reset(std::uint64_t expectedSession = 0);
    void requestMembers(std::uint64_t session, const std::string& guild, const std::vector<std::string>& users);
    bool
    searchMembers(std::uint64_t session, const std::string& guild, const std::string& query, const std::string& nonce);
    bool setPresence(std::uint64_t session, std::string status, std::optional<std::string> customText = {});
    bool restorePresence(std::uint64_t session, Presence presence);
    bool setVoice(std::uint64_t session, std::string guild, std::string channel, bool muted, bool deafened);
    void subscribe(std::string guild, std::string channel, int first, bool force = false, bool thread = false);

  private:
    void run(std::stop_token stop);
    const std::atomic<std::uint64_t>& m_generation;
    Dispatch m_dispatch;
    std::string m_endpoint;
    std::mutex m_mutex;
    std::condition_variable_any m_ready;
    process::WakeEvent m_wake;
    std::uint64_t m_session = 0, m_subscriptionVersion = 0;
    std::string m_token, m_guild, m_channel;
    Presence m_presence;
    bool m_presenceChosen = false, m_presenceRestored = false;
    std::uint64_t m_presenceVersion = 0;
    std::string m_voiceGuild, m_voiceChannel;
    bool m_voiceMuted = false, m_voiceDeafened = false, m_voiceChosen = false;
    std::uint64_t m_voiceVersion = 0;
    int m_first = 0;
    bool m_thread = false;
    std::set<std::pair<std::string, std::string>> m_memberRequested;
    std::map<std::string, std::set<std::string>> m_memberPending;
    std::size_t m_memberPendingCount = 0;
    struct MemberSearch {
      std::string guild, query, nonce;
      std::chrono::steady_clock::time_point queuedAt;
    };
    std::optional<MemberSearch> m_memberSearch;
    std::jthread m_worker;
  };
} // namespace api
