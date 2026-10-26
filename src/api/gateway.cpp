#include "api/gateway.h"

#include "api/gateway_pacing.h"
#include "api/network_profile.h"
#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <curl/curl.h>
#include <curl/websockets.h>
#include <deque>
#include <memory>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <random>
#include <stdexcept>
#include <utility>

namespace api {
  namespace {
    using namespace std::chrono_literals;
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    const Logger gatewayLog("gateway");
    constexpr std::string_view gatewayQuery = "?v=9&encoding=json";
    constexpr auto voiceUpdateSpacing = 250ms;

    enum class IdentifyCapability : std::uint64_t {
      VersionedReadStates = 1ULL << 2,
      VersionedGuildSettings = 1ULL << 3,
      DeduplicatedUsers = 1ULL << 4,
      PrioritizedReady = 1ULL << 5,
      SettingsProto = 1ULL << 9,
      ClientStateV2 = 1ULL << 10
    };
    // Advertise only payload variants handled by the backend. Passive guild
    // updates, debounced reactions and token refresh require other handlers.
    constexpr auto identifyCapabilities = std::to_underlying(IdentifyCapability::VersionedReadStates)
        | std::to_underlying(IdentifyCapability::VersionedGuildSettings)
        | std::to_underlying(IdentifyCapability::DeduplicatedUsers)
        | std::to_underlying(IdentifyCapability::PrioritizedReady)
        | std::to_underlying(IdentifyCapability::SettingsProto)
        | std::to_underlying(IdentifyCapability::ClientStateV2);

    std::optional<std::string> loopbackResumeEndpoint(std::string_view endpoint, std::string_view address) {
      // A test peer can use a second route on its explicitly injected origin.
      // Production resume URLs never take this branch.
      if (!endpoint.starts_with("ws://127.0.0.1:"))
        return {};
      const auto path = endpoint.find('/', 5);
      if (path == std::string_view::npos)
        return {};
      const auto origin = endpoint.substr(0, path);
      if (!address.starts_with(origin) || address.size() <= origin.size() || address[origin.size()] != '/')
        return {};
      const auto route = address.substr(origin.size());
      if (route.size() > 128 || !std::ranges::all_of(route, [](unsigned char c) {
            return (c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9')
                || c == '/'
                || c == '-'
                || c == '_';
          }))
        return {};
      return std::string(address) + std::string(gatewayQuery);
    }
    struct GatewayFailure : std::runtime_error {
      using std::runtime_error::runtime_error;
    };
    struct Connection {
      std::stop_token stop;
      const std::atomic<std::uint64_t>& generation;
      std::uint64_t session;
      const process::WakeEvent& wake;
      std::deque<int> recentOpcodes;
      bool cancelled() const { return stop.stop_requested() || generation != session; }
    };
    int progress(void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
      return static_cast<Connection*>(data)->cancelled();
    }
    void wait(CURL* curl, short events, const Connection& connection, Clock::time_point deadline) {
      curl_socket_t socket = CURL_SOCKET_BAD;
      curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket);
      pollfd descriptors[] = {{socket, events, 0}, {connection.wake.fd(), POLLIN, 0}};
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
      ::poll(descriptors, 2, static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, 120000)));
      if (descriptors[1].revents & POLLIN)
        connection.wake.drain();
    }
    void waitUntil(const Connection& connection, Clock::time_point deadline) {
      while (!connection.cancelled() && Clock::now() < deadline) {
        pollfd descriptor{connection.wake.fd(), POLLIN, 0};
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        ::poll(&descriptor, 1, static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, 120000)));
        if (descriptor.revents & POLLIN)
          connection.wake.drain();
      }
    }
    void send(CURL* curl, const Json& payload, Connection& connection) {
      const auto bytes = payload.dump();
      std::size_t offset = 0;
      const auto deadline = Clock::now() + 5s;
      do {
        if (connection.cancelled() || Clock::now() > deadline)
          throw GatewayFailure("Gateway send cancelled");
        size_t sent = 0;
        const auto status = curl_ws_send(curl, bytes.data() + offset, bytes.size() - offset, &sent, 0, CURLWS_TEXT);
        offset += sent;
        if (status == CURLE_AGAIN)
          wait(curl, POLLOUT, connection, deadline);
        else if (status != CURLE_OK)
          throw GatewayFailure(std::string("Gateway send failed: ") + curl_easy_strerror(status));
      } while (offset < bytes.size());
      if (connection.recentOpcodes.size() == 8)
        connection.recentOpcodes.pop_front();
      connection.recentOpcodes.push_back(payload.value("op", -1));
    }
  } // namespace

  std::optional<std::string> gatewayResumeEndpoint(std::string_view address) {
    if (address.size() > 256 || !address.starts_with("wss://"))
      return {};
    auto host = address.substr(6);
    if (host.ends_with('/'))
      host.remove_suffix(1);
    constexpr std::string_view suffix = ".discord.gg";
    if (host != "gateway.discord.gg") {
      if (!host.starts_with("gateway-") || !host.ends_with(suffix))
        return {};
      const auto label = host.substr(0, host.size() - suffix.size());
      if (label.size() <= 8
          || label.size() > 63
          || label[8] == '-'
          || label.back() == '-'
          || !std::ranges::all_of(label, [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
             }))
        return {};
    }
    return "wss://" + std::string(host) + "/" + std::string(gatewayQuery);
  }

  Gateway::Gateway(const std::atomic<std::uint64_t>& generation, Dispatch dispatch, std::string endpoint)
      : m_generation(generation), m_dispatch(std::move(dispatch)), m_endpoint(std::move(endpoint)),
        m_worker([this](std::stop_token stop) { run(stop); }) {}
  Gateway::~Gateway() {
    m_worker.request_stop();
    m_ready.notify_all();
    (void)m_wake.signal();
    m_worker.join();
  }
  void Gateway::authenticate(std::uint64_t session, std::string token, Presence presence) {
    std::lock_guard lock(m_mutex);
    if (session != m_generation)
      return;
    const bool changed = m_session != session;
    if (changed) {
      m_memberRequested.clear();
      m_memberPending.clear();
      m_memberPendingCount = 0;
      m_memberSearch.reset();
      m_presenceChosen = false;
      m_presenceRestored = false;
      m_voiceChosen = false;
      m_voiceGuild.clear();
      m_voiceChannel.clear();
      m_voiceVersion = 0;
    }
    m_session = session;
    m_token = std::move(token);
    if (changed || m_presenceVersion == 0 || !validStatus(m_presence.status))
      m_presence = std::move(presence);
    ++m_presenceVersion;
    ++m_subscriptionVersion;
    m_ready.notify_all();
    (void)m_wake.signal();
  }
  void Gateway::reset(std::uint64_t expectedSession) {
    std::lock_guard lock(m_mutex);
    if (expectedSession && m_session != expectedSession)
      return;
    m_token.clear();
    m_memberRequested.clear();
    m_memberPending.clear();
    m_memberPendingCount = 0;
    m_memberSearch.reset();
    m_presence = {};
    m_presenceVersion = 0;
    m_presenceChosen = false;
    m_presenceRestored = false;
    m_voiceChosen = false;
    m_voiceGuild.clear();
    m_voiceChannel.clear();
    m_voiceVersion = 0;
    m_guild.clear();
    m_channel.clear();
    m_thread = false;
    ++m_subscriptionVersion;
    m_ready.notify_all();
    (void)m_wake.signal();
  }
  void Gateway::subscribe(std::string guild, std::string channel, int first, bool force, bool thread) {
    std::lock_guard lock(m_mutex);
    first = thread ? 0 : std::clamp(first / 100 * 100, 0, 10000000);
    if (!force && guild == m_guild && channel == m_channel && first == m_first && thread == m_thread)
      return;
    m_guild = std::move(guild);
    m_channel = std::move(channel);
    m_first = first;
    m_thread = thread;
    ++m_subscriptionVersion;
    (void)m_wake.signal();
  }

  void Gateway::requestMembers(std::uint64_t session, const std::string& guild, const std::vector<std::string>& users) {
    if (!validSnowflake(guild))
      return;
    std::lock_guard lock(m_mutex);
    if (session != m_generation || session != m_session || m_token.empty())
      return;
    for (std::size_t i = 0; i < users.size() && i < 100; ++i) {
      const auto& user = users[i];
      if (m_memberRequested.size() + m_memberPendingCount >= 20000)
        break;
      if (validSnowflake(user) && !m_memberRequested.contains({guild, user}))
        m_memberPendingCount += m_memberPending[guild].insert(user).second;
    }
    (void)m_wake.signal();
  }

  bool Gateway::searchMembers(
      std::uint64_t session, const std::string& guild, const std::string& query, const std::string& nonce
  ) {
    if (!validSnowflake(guild)
        || query.empty()
        || query.size() > 128
        || utf8Length(query) > 64
        || nonce.empty()
        || nonce.size() > 32)
      return false;
    std::lock_guard lock(m_mutex);
    if (session != m_generation || session != m_session || m_token.empty())
      return false;
    m_memberSearch = MemberSearch{guild, query, nonce, Clock::now()};
    (void)m_wake.signal();
    return true;
  }
  bool Gateway::setPresence(std::uint64_t session, std::string status, std::optional<std::string> customText) {
    if (!validStatus(status) || (customText && utf8Length(*customText) > 128))
      return false;
    std::lock_guard lock(m_mutex);
    if (session != m_generation || session != m_session || m_token.empty())
      return false;
    // Even choosing the already active status is an explicit choice. A late
    // bootstrap read must not subsequently replace its custom status.
    m_presenceChosen = true;
    if (m_presence.status == status
        && (!customText
            || (m_presence.text == *customText && m_presence.emojiId.empty() && m_presence.emojiName.empty())))
      return true;
    m_presence.status = std::move(status);
    if (customText) {
      m_presence.text = std::move(*customText);
      m_presence.emojiId.clear();
      m_presence.emojiName.clear();
    }
    ++m_presenceVersion;
    (void)m_wake.signal();
    return true;
  }

  bool Gateway::restorePresence(std::uint64_t session, Presence presence) {
    if ((presence.status != "unknown" && !validStatus(presence.status))
        || utf8Length(presence.text) > 128
        || (!presence.emojiId.empty() && !validSnowflake(presence.emojiId))
        || presence.emojiName.size() > 256)
      return false;
    std::lock_guard lock(m_mutex);
    if (session != m_generation || session != m_session || m_token.empty() || m_presenceChosen || m_presenceRestored)
      return false;
    m_presenceRestored = true;
    // READY has the current session's status; the optional settings read can be
    // older. Restore custom text/emoji without replacing that live status.
    if (validStatus(m_presence.status))
      presence.status = m_presence.status;
    if (m_presence.status != presence.status
        || m_presence.text != presence.text
        || m_presence.emojiId != presence.emojiId
        || m_presence.emojiName != presence.emojiName) {
      m_presence = std::move(presence);
      ++m_presenceVersion;
      (void)m_wake.signal();
    }
    return true;
  }

  bool Gateway::setVoice(std::uint64_t session, std::string guild, std::string channel, bool muted, bool deafened) {
    if ((!guild.empty() && !validSnowflake(guild)) || (!channel.empty() && !validSnowflake(channel)))
      return false;
    std::lock_guard lock(m_mutex);
    if (session != m_generation || session != m_session || m_token.empty())
      return false;
    m_voiceGuild = std::move(guild);
    m_voiceChannel = std::move(channel);
    m_voiceMuted = muted;
    m_voiceDeafened = deafened;
    m_voiceChosen = true;
    ++m_voiceVersion;
    (void)m_wake.signal();
    return true;
  }

  void Gateway::run(std::stop_token stop) {
    gateway_pacing::ApplicationBudget applicationBudget;
    gateway_pacing::ReconnectBudget reconnectBudget;
    std::deque<Clock::time_point> presenceTimes;
    auto memberRequestAt = Clock::time_point{};
    std::mt19937 random(std::random_device{}());
    auto retryAt = Clock::time_point{};
    bool handoff = false;
    std::string resumeId, resumeEndpoint;
    std::int64_t sequence = -1;
    std::uint64_t activeSession = 0;
    while (!stop.stop_requested()) {
      std::string token;
      std::uint64_t session;
      {
        std::unique_lock lock(m_mutex);
        m_ready.wait(lock, stop, [&] { return !m_token.empty() && m_session == m_generation; });
        if (stop.stop_requested())
          return;
        token = m_token;
        session = m_session;
      }
      if (activeSession != session) {
        activeSession = session;
        resumeId.clear();
        resumeEndpoint.clear();
        sequence = -1;
      }
      Connection connection{stop, m_generation, session, m_wake};
      bool fatal = false;
      bool authenticationFailed = false;
      auto retryCause = gateway_pacing::Failure::Transport;
      auto readyAt = Clock::time_point{};
      const bool resumable = !resumeId.empty() && sequence >= 0;
      waitUntil(connection, std::max(retryAt, reconnectBudget.ready(Clock::now(), resumable, handoff)));
      handoff = false;
      if (connection.cancelled())
        continue;
      reconnectBudget.connecting(Clock::now());
      std::string failure = "Live connection interrupted";
      const char* phase = "connect";
      const char* failureKind = "transport";
      unsigned diagnosticCloseCode = 0;
      gatewayLog.info("Connection starting resumable={}", resumable);
      try {
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl)
          throw std::runtime_error("Gateway unavailable");
        // The only alternative origin is an explicitly injected loopback test server.
        const bool loopback = m_endpoint.starts_with("ws://127.0.0.1:");
        if (!loopback && m_endpoint != "wss://gateway.discord.gg/?v=9&encoding=json")
          throw std::runtime_error("Invalid Gateway origin");
        const auto& endpoint =
            !resumeId.empty() && sequence >= 0 && !resumeEndpoint.empty() ? resumeEndpoint : m_endpoint;
        curl_easy_setopt(curl.get(), CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, loopback ? "ws" : "wss");
        if (loopback)
          curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "127.0.0.1");
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 20000L);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &connection);
        const auto connected = curl_easy_perform(curl.get());
        network_profile::transferred("gateway-connect", curl.get(), connected);
        if (connected != CURLE_OK)
          throw GatewayFailure(std::string("Gateway connection failed: ") + curl_easy_strerror(connected));
        const auto connectedAt = Clock::now();
        phase = "hello";
        gatewayLog.info("Transport connected");
        std::string message, subscribedGuild;
        std::uint64_t subscription = 0, presenceVersion = 0, voiceVersion = 0;
        auto voiceAt = Clock::time_point{};
        auto subscribedAt = Clock::time_point{};
        auto heartbeatAt = Clock::now() + 20s;
        std::chrono::milliseconds interval{30000};
        bool hello = false, ready = false, awaitingAck = false;
        while (!connection.cancelled()) {
          const auto now = Clock::now();
          if (now >= heartbeatAt) {
            if (!hello || awaitingAck) {
              failureKind = "heartbeat-timeout";
              throw GatewayFailure("Gateway heartbeat timed out");
            }
            send(curl.get(), Json{{"op", 1}, {"d", sequence < 0 ? Json(nullptr) : Json(sequence)}}, connection);
            awaitingAck = true;
            heartbeatAt = now + interval;
          }
          auto nextControl = heartbeatAt;
          if (ready) {
            Json voiceUpdate;
            std::uint64_t desiredVoiceVersion = 0;
            {
              std::lock_guard lock(m_mutex);
              if (m_voiceChosen && voiceVersion != m_voiceVersion) {
                desiredVoiceVersion = m_voiceVersion;
                voiceUpdate = {
                    {"guild_id", m_voiceGuild.empty() ? Json(nullptr) : Json(m_voiceGuild)},
                    {"channel_id", m_voiceChannel.empty() ? Json(nullptr) : Json(m_voiceChannel)},
                    {"self_mute", m_voiceMuted},
                    {"self_deaf", m_voiceDeafened},
                    {"self_video", false}
                };
              }
            }
            if (desiredVoiceVersion) {
              const auto sendAt = std::max(voiceAt, applicationBudget.ready(now));
              nextControl = std::min(nextControl, sendAt);
              if (now >= sendAt && applicationBudget.acquire(now)) {
                send(curl.get(), Json{{"op", 4}, {"d", std::move(voiceUpdate)}}, connection);
                voiceVersion = desiredVoiceVersion;
                voiceAt = now + voiceUpdateSpacing;
              }
            }
            Presence presence;
            std::uint64_t desiredVersion;
            {
              std::lock_guard lock(m_mutex);
              presence = m_presence;
              desiredVersion = m_presenceVersion;
            }
            while (!presenceTimes.empty() && now - presenceTimes.front() >= gateway_pacing::presenceWindow)
              presenceTimes.pop_front();
            if (presenceVersion != desiredVersion && !validStatus(presence.status))
              presenceVersion = desiredVersion;
            if (presenceVersion != desiredVersion) {
              const auto presenceAt = std::max(
                  applicationBudget.ready(now),
                  presenceTimes.size() >= gateway_pacing::presenceLimit
                      ? presenceTimes.front() + gateway_pacing::presenceWindow
                      : now
              );
              nextControl = std::min(nextControl, presenceAt);
              if (now >= presenceAt && applicationBudget.acquire(now)) {
                Json activities = Json::array();
                if (!presence.text.empty() || !presence.emojiId.empty() || !presence.emojiName.empty()) {
                  Json custom{{"name", "Custom Status"}, {"type", 4}, {"state", presence.text}};
                  if (!presence.emojiId.empty())
                    custom["emoji"] = {{"id", presence.emojiId}, {"name", presence.emojiName}};
                  else if (!presence.emojiName.empty())
                    custom["emoji"] = {{"name", presence.emojiName}};
                  activities.push_back(std::move(custom));
                }
                send(
                    curl.get(),
                    Json{
                        {"op", 3},
                        {"d",
                         {{"status", presence.status},
                          {"since",
                           presence.status == "idle" ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           std::chrono::system_clock::now().time_since_epoch()
                                                       )
                                                           .count()
                                                     : 0},
                          {"afk", false},
                          {"activities", activities}}}
                    },
                    connection
                );
                presenceTimes.push_back(now);
                m_dispatch(session, "PRESENCE_SENT", Json{{"status", presence.status}, {"text", presence.text}});
                presenceVersion = desiredVersion;
              }
            }
            std::string guild, channel;
            bool thread;
            int first;
            std::uint64_t version;
            {
              std::lock_guard lock(m_mutex);
              guild = m_guild;
              channel = m_channel;
              first = m_first;
              thread = m_thread;
              version = m_subscriptionVersion;
            }
            if (version != subscription) {
              const std::size_t commands = (!guild.empty() && guild != subscribedGuild ? 1 : 0)
                  + ((!subscribedGuild.empty() && subscribedGuild != guild) || (!guild.empty() && !channel.empty())
                         ? 1
                         : 0);
              const auto subscriptionAt = commands
                  ? std::max(subscribedAt + gateway_pacing::subscriptionSpacing, applicationBudget.ready(now, commands))
                  : now;
              nextControl = std::min(nextControl, subscriptionAt);
              if (now >= subscriptionAt && (!commands || applicationBudget.acquire(now, commands))) {
                Json subscriptions = Json::object();
                if (!subscribedGuild.empty() && subscribedGuild != guild)
                  subscriptions[subscribedGuild] = {
                      {"typing", false},
                      {"activities", false},
                      {"threads", false},
                      {"channels", Json::object()},
                      {"thread_member_lists", Json::array()}
                  };
                Json ranges = Json::array({Json::array({first, first + 99}), Json::array({first + 100, first + 199})});
                Json channels = thread ? Json::object() : Json{{channel, ranges}};
                if (!guild.empty() && guild != subscribedGuild) {
                  // A new guild subscription must precede its channel ranges in
                  // a separate WebSocket frame; otherwise the ranges may be ignored.
                  const Json initial = {
                      {"typing", true},         {"activities", true},         {"threads", true},
                      {"member_updates", true}, {"channels", Json::object()}, {"thread_member_lists", Json::array()}
                  };
                  send(curl.get(), Json{{"op", 37}, {"d", {{"subscriptions", {{guild, initial}}}}}}, connection);
                }
                if (!guild.empty() && !channel.empty())
                  subscriptions[guild] = {
                      {"typing", true},       {"activities", true},
                      {"threads", true},      {"member_updates", true},
                      {"channels", channels}, {"thread_member_lists", thread ? Json::array({channel}) : Json::array()}
                  };
                if (!subscriptions.empty())
                  send(curl.get(), Json{{"op", 37}, {"d", {{"subscriptions", subscriptions}}}}, connection);
                subscription = version;
                subscribedGuild = guild;
                subscribedAt = now;
              }
            }
            bool membersPending;
            {
              std::lock_guard lock(m_mutex);
              if (m_memberSearch && now - m_memberSearch->queuedAt >= gateway_pacing::memberSearchLifetime)
                m_memberSearch.reset();
              membersPending = m_memberSearch || !m_memberPending.empty();
            }
            const auto membersAt = std::max(memberRequestAt, applicationBudget.ready(now));
            if (membersPending)
              nextControl = std::min(nextControl, membersAt);
            if (membersPending && now >= membersAt && applicationBudget.acquire(now)) {
              std::string memberGuild;
              std::vector<std::string> users;
              std::optional<MemberSearch> search;
              {
                std::lock_guard lock(m_mutex);
                if (m_memberSearch)
                  search = std::exchange(m_memberSearch, {});
                else if (!m_memberPending.empty()) {
                  auto pending = m_memberPending.begin();
                  memberGuild = pending->first;
                  while (!pending->second.empty() && users.size() < 100) {
                    const auto id = pending->second.begin();
                    users.push_back(*id);
                    m_memberRequested.emplace(memberGuild, *id);
                    pending->second.erase(id);
                    --m_memberPendingCount;
                  }
                  if (pending->second.empty())
                    m_memberPending.erase(pending);
                }
              }
              if (search) {
                send(
                    curl.get(),
                    Json{
                        {"op", 8},
                        {"d",
                         {{"guild_id", search->guild},
                          {"query", search->query},
                          {"limit", 25},
                          {"presences", false},
                          {"nonce", search->nonce}}}
                    },
                    connection
                );
                memberRequestAt = now + gateway_pacing::memberSpacing;
              } else if (!users.empty()) {
                send(
                    curl.get(),
                    Json{{"op", 8}, {"d", {{"guild_id", memberGuild}, {"user_ids", users}, {"limit", 100}}}}, connection
                );
                memberRequestAt = now + gateway_pacing::memberSpacing;
              }
            }
          }
          char buffer[65536];
          size_t count = 0;
          const curl_ws_frame* frame = nullptr;
          const auto code = curl_ws_recv(curl.get(), buffer, sizeof(buffer), &count, &frame);
          if (code == CURLE_AGAIN) {
            wait(curl.get(), POLLIN, connection, nextControl);
            continue;
          }
          if (code != CURLE_OK || !frame)
            throw GatewayFailure(std::string("Gateway disconnected: ") + curl_easy_strerror(code));
          if (frame->flags & CURLWS_CLOSE) {
            const unsigned closeCode = count >= 2
                ? (static_cast<unsigned char>(buffer[0]) << 8) | static_cast<unsigned char>(buffer[1])
                : 1000;
            diagnosticCloseCode = closeCode;
            failureKind = "peer-close";
            if (closeCode == 4008)
              retryCause = gateway_pacing::Failure::RateLimited;
            authenticationFailed = closeCode == 4004;
            if (closeCode == 4004
                || closeCode == 4010
                || closeCode == 4011
                || closeCode == 4012
                || closeCode == 4013
                || closeCode == 4014)
              fatal = true;
            if (closeCode == 4007 || closeCode == 4009) {
              resumeId.clear();
              resumeEndpoint.clear();
              sequence = -1;
            }
            throw GatewayFailure("Gateway closed (" + std::to_string(closeCode) + ")");
          }
          if (!(frame->flags & CURLWS_TEXT))
            continue;
          if (message.size() + count > 32 * 1024 * 1024)
            throw std::runtime_error("Gateway payload too large");
          message.append(buffer, count);
          if (frame->bytesleft || (frame->flags & CURLWS_CONT))
            continue;
          auto packet = Json::parse(message, nullptr, false);
          message.clear();
          if (!packet.is_object())
            throw std::runtime_error("Malformed Gateway payload");
          const int op = packet.value("op", -1);
          const auto& data = packet["d"];
          if (packet.contains("s") && packet["s"].is_number_integer())
            sequence = packet["s"].get<std::int64_t>();
          if (op == 10) {
            if (hello)
              throw std::runtime_error("Duplicate Gateway hello");
            hello = true;
            interval = std::chrono::milliseconds(std::clamp(data.value("heartbeat_interval", 30000), 1000, 120000));
            heartbeatAt = Clock::now() + gateway_pacing::heartbeatDelay(interval, random());
            if (!resumeId.empty() && sequence >= 0) {
              phase = "resume";
              gatewayLog.debug("Handshake action=resume");
              send(
                  curl.get(), Json{{"op", 6}, {"d", {{"token", token}, {"session_id", resumeId}, {"seq", sequence}}}},
                  connection
              );
            } else {
              phase = "identify";
              gatewayLog.debug("Handshake action=identify");
              reconnectBudget.identifying(Clock::now());
              send(
                  curl.get(),
                  Json{
                      {"op", 2},
                      {"d",
                       {{"token", token},
                        {"compress", false},
                        {"capabilities", identifyCapabilities},
                        {"client_state", {{"guild_versions", Json::object()}}},
                        {"properties",
                         {{"os", "Linux"},
                          {"browser", "cressida"},
                          {"device", "cressida"},
                          {"system_locale", "en-US"},
                          {"release_channel", "stable"}}},
                        {"presence",
                         {{"status", "unknown"}, {"since", 0}, {"activities", Json::array()}, {"afk", false}}}}}
                  },
                  connection
              );
            }
          } else if (op == 11)
            awaitingAck = false;
          else if (op == 1) {
            send(curl.get(), Json{{"op", 1}, {"d", sequence < 0 ? Json(nullptr) : Json(sequence)}}, connection);
            awaitingAck = true;
          } else if (op == 7) {
            failureKind = "peer-reconnect";
            retryCause = gateway_pacing::Failure::Handoff;
            throw GatewayFailure("Gateway requested reconnect");
          } else if (op == 9) {
            failureKind = "invalid-session";
            gatewayLog.warn("Session invalidated resumable={} phase={}", data.is_boolean() && data.get<bool>(), phase);
            retryCause = gateway_pacing::Failure::InvalidSession;
            if (!data.is_boolean() || !data.get<bool>()) {
              resumeId.clear();
              resumeEndpoint.clear();
              sequence = -1;
            }
            throw GatewayFailure("Gateway session invalidated");
          } else if (op == 0) {
            const auto type = packet.value("t", std::string{});
            if (type == "READY" || type == "RESUMED") {
              network_profile::elapsed("gateway", "ready", connectedAt);
              phase = "ready";
              gatewayLog.info(
                  "Session established action={} duration_ms={}", type == "READY" ? "ready" : "resumed",
                  std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - connectedAt).count()
              );
              std::lock_guard lock(m_mutex);
              m_memberRequested.clear();
            }
            if (type == "READY") {
              resumeId = data.value("session_id", std::string{});
              resumeEndpoint.clear();
              if (const auto address = data.find("resume_gateway_url"); address != data.end() && address->is_string()) {
                const auto& url = address->get_ref<const std::string&>();
                resumeEndpoint = (loopback ? loopbackResumeEndpoint(m_endpoint, url) : gatewayResumeEndpoint(url))
                                     .value_or(std::string{});
              }
              {
                std::lock_guard lock(m_mutex);
                if (!m_presenceChosen && data.contains("sessions") && data["sessions"].is_array())
                  for (const auto& own : data["sessions"])
                    if (own.value("session_id", "") == resumeId) {
                      const auto state = own.value("status", "unknown");
                      if (validStatus(state) && m_presence.status != state) {
                        m_presence.status = state;
                        ++m_presenceVersion;
                      }
                    }
              }
              ready = true;
              readyAt = Clock::now();
            }
            if (type == "RESUMED") {
              ready = true;
              readyAt = Clock::now();
            }
            m_dispatch(session, type, data);
          }
        }
      } catch (const GatewayFailure& error) {
        failure = error.what();
      } catch (...) { // Exception details can contain private packet or transport data.
        failureKind = "protocol";
      }
      if (connection.cancelled()) {
        gatewayLog.debug("Connection cancelled phase={}", phase);
        continue;
      }
      std::string recent;
      for (const auto opcode : connection.recentOpcodes) {
        if (!recent.empty())
          recent += ',';
        recent += std::to_string(opcode);
      }
      if (fatal)
        gatewayLog.error(
            "Connection rejected close_code={} authentication_failed={} phase={} recent_opcodes=[{}]",
            diagnosticCloseCode, authenticationFailed, phase, recent
        );
      if (!connection.cancelled())
        m_dispatch(
            session, fatal ? "CONNECTION_FAILED" : "RECONNECTING",
            Json{{"reason", failure}, {"authentication_failed", authenticationFailed}}
        );
      if (fatal) {
        std::lock_guard lock(m_mutex);
        if (m_session == session)
          m_token.clear();
        continue;
      }
      // A short READY followed by another close must not reset failure pacing.
      const auto now = Clock::now();
      retryAt = reconnectBudget.failed(
          now, retryCause, readyAt == Clock::time_point{} ? Clock::duration{} : now - readyAt, random()
      );
      handoff = retryCause == gateway_pacing::Failure::Handoff;
      const bool canResume = !resumeId.empty() && sequence >= 0;
      const auto nextConnection = std::max(retryAt, reconnectBudget.ready(now, canResume, handoff));
      gatewayLog.warn(
          "Reconnect scheduled reason={} close_code={} phase={} resumable={} backoff_ms={} recent_opcodes=[{}]",
          failureKind, diagnosticCloseCode, phase, canResume,
          std::max<std::int64_t>(0, std::chrono::ceil<std::chrono::milliseconds>(nextConnection - now).count()), recent
      );
    }
  }
} // namespace api
