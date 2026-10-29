#include "voice/connection.h"

#include "core/log.h"
#include "core/process/wake_event.h"
#include "voice/encryption.h"
#include "voice/protocol.h"
#include "voice/speaking.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <curl/curl.h>
#include <curl/websockets.h>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <netdb.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <set>
#include <sodium.h>
#include <stdexcept>
#include <stop_token>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace voice {
  namespace {
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    constexpr std::size_t maximumFrames = 5;
    constexpr std::size_t maximumPendingSignals = 32;
    constexpr std::size_t maximumCredentialBytes = 4096;
    constexpr unsigned silenceFrames = 5;
    constexpr auto connectTimeout = 15s;
    constexpr auto setupTimeout = 30s;
    constexpr auto discoveryTimeout = 5s;
    constexpr auto sendTimeout = 5s;
    constexpr auto mediaTick = 20ms;
    constexpr auto udpKeepaliveInterval = 5s;
    constexpr std::size_t maximumReceiveBatch = 64;
    constexpr std::size_t maximumDiagnosticOpcodes = 32;
    constexpr std::size_t diagnosticHistory = 8;
    const Logger voiceLog("voice");
    const char* phaseName(Phase phase) {
      switch (phase) {
      case Phase::Disconnected:
        return "disconnected";
      case Phase::Connecting:
        return "connecting";
      case Phase::Securing:
        return "securing";
      case Phase::WaitingForParticipants:
        return "waiting-for-participants";
      case Phase::Connected:
        return "secure";
      case Phase::Failed:
        return "failed";
      }
      return "unknown";
    }
    struct Failure : std::runtime_error {
      using std::runtime_error::runtime_error;
    };
    struct Socket {
      int fd = -1;
      Socket() = default;
      ~Socket() { reset(); }
      Socket(const Socket&) = delete;
      Socket& operator=(const Socket&) = delete;
      void reset(int value = -1) {
        if (fd >= 0)
          ::close(fd);
        fd = value;
      }
    };
    struct Context {
      std::stop_token stop;
      const std::atomic<std::uint64_t>& revision;
      std::uint64_t version;
      const process::WakeEvent& wake;
      bool cancelled() const { return stop.stop_requested() || revision != version; }
    };
    int progress(void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
      return static_cast<Context*>(data)->cancelled();
    }
    void wait(CURL* curl, int udp, short events, const Context& context, Clock::time_point deadline) {
      curl_socket_t socket = CURL_SOCKET_BAD;
      curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket);
      pollfd descriptors[]{{socket, events, 0}, {context.wake.fd(), POLLIN, 0}, {udp, POLLIN, 0}};
      const auto duration = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
      ::poll(
          descriptors, std::size(descriptors), static_cast<int>(std::clamp<std::int64_t>(duration.count(), 0, 1000))
      );
      if (descriptors[1].revents & POLLIN)
        context.wake.drain();
    }
    void send(CURL* curl, std::span<const std::uint8_t> data, bool binary, const Context& context) {
      const auto deadline = Clock::now() + sendTimeout;
      std::size_t offset = 0;
      while (offset < data.size()) {
        if (context.cancelled())
          throw Failure("Voice connection cancelled");
        if (Clock::now() >= deadline)
          throw Failure("Voice signaling timed out");
        std::size_t written = 0;
        const auto status = curl_ws_send(
            curl, data.data() + offset, data.size() - offset, &written, 0, binary ? CURLWS_BINARY : CURLWS_TEXT
        );
        offset += written;
        if (status == CURLE_AGAIN)
          wait(curl, -1, POLLOUT, context, deadline);
        else if (status != CURLE_OK)
          throw Failure("Voice signaling was interrupted");
      }
    }
    void send(CURL* curl, const Json& value, const Context& context) {
      const auto text = value.dump();
      send(curl, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, false, context);
    }
    std::uint64_t channelNumber(const std::string& channel) {
      std::uint64_t result = 0;
      std::from_chars(channel.data(), channel.data() + channel.size(), result);
      return result;
    }
    std::optional<std::uint32_t> uint32(const Json& value) {
      if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        return {};
      const auto number = value.get<std::uint64_t>();
      if (number > std::numeric_limits<std::uint32_t>::max())
        return {};
      return static_cast<std::uint32_t>(number);
    }
    std::string userId(const Json& data, const char* key = "user_id") {
      if (!data.is_object() || !data.contains(key) || !data[key].is_string())
        return {};
      auto result = data[key].get<std::string>();
      return protocol::validId(result) ? result : std::string{};
    }
    bool secureText(const std::string& text) {
      return !text.empty()
          && text.size() <= maximumCredentialBytes
          && std::ranges::none_of(text, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
    }
    std::optional<std::string> testEndpoint(std::string_view value) {
      constexpr std::string_view prefix = "ws://127.0.0.1:";
      if (!value.starts_with(prefix) || !value.ends_with('/'))
        return {};
      value.remove_prefix(prefix.size());
      value.remove_suffix(1);
      unsigned port = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
      if (value.empty()
          || parsed.ec != std::errc{}
          || parsed.ptr != value.data() + value.size()
          || !port
          || port > std::numeric_limits<std::uint16_t>::max())
        return {};
      return std::string(prefix) + std::string(value) + "/?v=8";
    }
    void openUdp(Socket& socket, const protocol::Ready& ready) {
      addrinfo hints{};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_DGRAM;
      hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
      addrinfo* resolved = nullptr;
      const auto port = std::to_string(ready.port);
      if (getaddrinfo(ready.ip.c_str(), port.c_str(), &hints, &resolved) != 0)
        throw Failure("Voice server address is invalid");
      std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address(resolved, freeaddrinfo);
      socket.reset(::socket(address->ai_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
      if (socket.fd < 0 || ::connect(socket.fd, address->ai_addr, address->ai_addrlen) != 0)
        throw Failure("Could not open the voice network connection");
      const auto request = protocol::discoveryRequest(ready.ssrc);
      if (::send(socket.fd, request.data(), request.size(), 0) != static_cast<ssize_t>(request.size()))
        throw Failure("Could not discover the voice network address");
    }
    struct EarlySignal {
      int opcode;
      std::vector<std::uint8_t> binary;
      Json data;
    };
  } // namespace

  class Connection::Impl {
  public:
    Impl(std::shared_ptr<Audio> audio, Changed changed, std::string overrideEndpoint)
        : audio(std::move(audio)), changed(std::move(changed)) {
      if (!overrideEndpoint.empty()) {
        const auto validated = testEndpoint(overrideEndpoint);
        if (!validated)
          throw std::invalid_argument("Voice test endpoint must use an explicit loopback socket");
        endpointOverride = *validated;
      }
      worker = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ~Impl() {
      worker.request_stop();
      ++version;
      ready.notify_all();
      (void)wake.signal();
      worker.join();
    }
    bool connect(ConnectionInfo info) {
      if (!audio
          || !protocol::validId(info.server)
          || !protocol::validId(info.user)
          || !protocol::validId(info.channel)
          || !secureText(info.session)
          || !secureText(info.token)
          || !protocol::endpoint(info.endpoint)
          || !info.generation
          || !info.revision
          || info.participants.size() > protocol::maximumParticipants
          || (info.participants.size() == protocol::maximumParticipants
              && std::ranges::find(info.participants, info.user) == info.participants.end())
          || !std::ranges::all_of(info.participants, protocol::validId))
        return false;
      {
        std::lock_guard lock(mutex);
        desired = std::move(info);
        membership = desired->participants;
        ++membershipRevision;
        frames.clear();
        ++version;
      }
      ready.notify_all();
      (void)wake.signal();
      return true;
    }
    void disconnect() {
      {
        std::lock_guard lock(mutex);
        desired.reset();
        membership.clear();
        ++membershipRevision;
        frames.clear();
        ++version;
      }
      ready.notify_all();
      (void)wake.signal();
    }
    void configure(AudioSettings value) {
      {
        std::lock_guard lock(mutex);
        settings = sanitizeAudioSettings(std::move(value));
        ++settingsRevision;
      }
      (void)wake.signal();
    }
    void participants(std::vector<std::string> users) {
      if (users.size() > protocol::maximumParticipants || !std::ranges::all_of(users, protocol::validId))
        return;
      {
        std::lock_guard lock(mutex);
        if (!desired)
          return;
        if (users.size() == protocol::maximumParticipants && std::ranges::find(users, desired->user) == users.end())
          return;
        membership = std::move(users);
        ++membershipRevision;
      }
      (void)wake.signal();
    }

  private:
    void publish(State& state, const AudioSettings& settings, const Context& context) {
      if (context.cancelled())
        return;
      state.muted = settings.muted;
      state.deafened = settings.deafened;
      if (diagnosticPhase != state.phase) {
        voiceLog.info("Connection phase={}", phaseName(state.phase));
        diagnosticPhase = state.phase;
      }
      if (changed)
        changed(state);
    }
    void session(ConnectionInfo info, const Context& context) {
      State state;
      state.phase = Phase::Connecting;
      state.generation = info.generation;
      state.revision = info.revision;
      state.channel = info.channel;
      state.participants.push_back({info.user, false});
      AudioSettings activeSettings;
      std::uint64_t activeSettingsRevision = 0;
      std::uint64_t activeMembershipRevision = 0;
      std::vector<std::string> initialMembership;
      {
        std::lock_guard lock(mutex);
        activeSettings = settings;
        activeSettingsRevision = settingsRevision;
        activeMembershipRevision = membershipRevision;
        initialMembership = membership;
      }
      publish(state, activeSettings, context);
      audio->configure(activeSettings);
      bool audioStarted = false;
      std::deque<int> recentOpcodes;
      std::size_t diagnosticCount = 0;
      const auto recordOpcode = [&](int opcode) {
        if (recentOpcodes.size() == diagnosticHistory)
          recentOpcodes.pop_front();
        recentOpcodes.push_back(opcode);
        if (diagnosticCount < maximumDiagnosticOpcodes)
          voiceLog.debug("Signal opcode={}", opcode);
        if (++diagnosticCount == maximumDiagnosticOpcodes)
          voiceLog.debug("Signal opcode logging reached its session limit");
      };
      try {
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl)
          throw Failure("Voice networking is unavailable");
        const auto endpoint = endpointOverride.empty() ? *protocol::endpoint(info.endpoint) : endpointOverride;
        curl_easy_setopt(curl.get(), CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, endpointOverride.empty() ? "wss" : "ws");
        if (!endpointOverride.empty())
          curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "127.0.0.1");
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(
            curl.get(), CURLOPT_CONNECTTIMEOUT_MS,
            static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(connectTimeout).count())
        );
        curl_easy_setopt(
            curl.get(), CURLOPT_TIMEOUT_MS,
            static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(connectTimeout).count())
        );
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &context);
        if (curl_easy_perform(curl.get()) != CURLE_OK)
          throw Failure("Could not connect to the voice server");
        const auto setupDeadline = Clock::now() + setupTimeout;
        auto securityDeadline = Clock::time_point::max();
        auto discoveryDeadline = Clock::time_point::max();
        auto keepaliveAt = Clock::time_point::max();
        std::uint64_t keepaliveCounter = 0;
        protocol::Heartbeat heartbeat;
        protocol::RtpCipher cipher;
        Encryption encryption(info.user, channelNumber(info.channel));
        Socket udp;
        std::optional<protocol::Ready> remote;
        bool described = false, discovered = false, speaking = false, mediaReady = false;
        unsigned remainingSilence = 0;
        std::uint16_t sequence = 0;
        randombytes_buf(&sequence, sizeof(sequence));
        std::int64_t acknowledgedSequence = -1;
        std::map<std::uint32_t, std::string> senders;
        std::map<std::string, bool> participants;
        SpeakingActivity activity(info.user);
        participants.emplace(info.user, false);
        encryption.addUser(info.user);
        for (const auto& user : initialMembership) {
          participants.emplace(user, false);
          encryption.addUser(user);
        }
        std::deque<EarlySignal> early;
        std::size_t earlyBytes = 0;
        std::vector<std::uint8_t> signal;
        bool binary = false;
        auto updateParticipants = [&] {
          state.participants.clear();
          for (auto& [user, talking] : participants) {
            talking = activity.active(user, Clock::now());
            state.participants.push_back({user, talking});
          }
          publish(state, activeSettings, context);
        };
        updateParticipants();
        auto flushEncryption = [&] {
          if (!encryption.error().empty())
            throw Failure("The encrypted voice session could not be established");
          for (auto& message : encryption.takeMessages()) {
            if (message.binary) {
              if (message.bytes.size() >= protocol::maximumSignalBytes)
                throw Failure("The encrypted voice response is too large");
              message.bytes.insert(message.bytes.begin(), static_cast<std::uint8_t>(message.opcode));
              send(curl.get(), message.bytes, true, context);
            } else
              send(curl.get(), Json{{"op", message.opcode}, {"d", std::move(message.data)}}, context);
          }
        };
        auto encryptionSignal = [&](EarlySignal message) {
          if (!described) {
            const auto bytes = message.binary.size() + message.data.dump().size();
            if (early.size() >= maximumPendingSignals || bytes > protocol::maximumSignalBytes - earlyBytes)
              throw Failure("Too many voice setup messages");
            earlyBytes += bytes;
            early.push_back(std::move(message));
            return;
          }
          if ((message.opcode == 21 || message.opcode == 24 || message.opcode == 29 || message.opcode == 30)
              && activity.clear())
            updateParticipants();
          const bool accepted = message.binary.empty() ? encryption.handleJson(message.opcode, message.data)
                                                       : encryption.handleBinary(message.opcode, message.binary);
          if (!accepted)
            throw Failure("The encrypted voice session rejected a protocol message");
          flushEncryption();
        };
        auto sendSpeaking = [&](bool value) {
          if (speaking == value)
            return;
          send(
              curl.get(), Json{{"op", 5}, {"d", {{"speaking", value ? 1 : 0}, {"delay", 0}, {"ssrc", remote->ssrc}}}},
              context
          );
          speaking = value;
        };
        auto sendAudio = [&](const EncodedAudio& frame, bool silence) {
          const auto input =
              silence ? std::span<const std::uint8_t>{protocol::silence} : std::span<const std::uint8_t>{frame.opus};
          auto encrypted = encryption.encrypt(remote->ssrc, input);
          if (encrypted.empty())
            throw Failure("Voice audio encryption failed");
          const auto packet = cipher.seal({remote->ssrc, frame.timestamp, sequence++, std::move(encrypted)});
          if (packet.empty())
            throw Failure("Voice transport encryption failed");
          const auto sent = ::send(udp.fd, packet.data(), packet.size(), 0);
          if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            throw Failure("The voice network connection was interrupted");
        };
        while (!context.cancelled()) {
          const auto now = Clock::now();
          encryption.tick();
          if (activity.expire(now))
            updateParticipants();
          if (heartbeat.missed(now))
            throw Failure("The voice server stopped responding");
          if (heartbeat.due(now))
            send(
                curl.get(), Json{{"op", 3}, {"d", {{"t", heartbeat.sent(now)}, {"seq_ack", acknowledgedSequence}}}},
                context
            );
          if ((!described && now >= setupDeadline) || now >= securityDeadline)
            throw Failure("Voice connection setup timed out");
          if (!discovered && now >= discoveryDeadline)
            throw Failure("Voice UDP discovery timed out");
          if (described
              && discovered
              && !encryption.ready()
              && participants.size() > 1
              && securityDeadline == Clock::time_point::max())
            securityDeadline = now + setupTimeout;
          if (participants.size() <= 1)
            securityDeadline = Clock::time_point::max();
          if (described && discovered && !encryption.ready()) {
            const auto pendingPhase = participants.size() <= 1 ? Phase::WaitingForParticipants : Phase::Securing;
            if (state.phase != pendingPhase) {
              state.phase = pendingPhase;
              state.encrypted = false;
              publish(state, activeSettings, context);
            }
          }
          bool settingsChanged = false;
          std::optional<std::vector<std::string>> membershipUpdate;
          {
            std::lock_guard lock(mutex);
            if (activeSettingsRevision != settingsRevision) {
              activeSettings = settings;
              activeSettingsRevision = settingsRevision;
              settingsChanged = true;
            }
            if (activeMembershipRevision != membershipRevision) {
              membershipUpdate = membership;
              activeMembershipRevision = membershipRevision;
            }
          }
          if (membershipUpdate) {
            std::set<std::string> current(membershipUpdate->begin(), membershipUpdate->end());
            current.insert(info.user);
            for (const auto& user : current) {
              participants.try_emplace(user, false);
              encryption.addUser(user);
            }
            std::erase_if(participants, [&](const auto& participant) {
              if (current.contains(participant.first))
                return false;
              encryption.removeUser(participant.first);
              activity.remove(participant.first);
              return true;
            });
            std::erase_if(senders, [&](const auto& sender) {
              if (current.contains(sender.second))
                return false;
              audio->remove(sender.first);
              return true;
            });
            updateParticipants();
          }
          if (settingsChanged)
            audio->configure(activeSettings);
          if ((activeSettings.muted
               || activeSettings.deafened
               || (activeSettings.pushToTalk && !activeSettings.pttPressed))
              && activity.capture(false, activeSettings.muted, activeSettings.deafened, now))
            updateParticipants();
          if (state.muted != activeSettings.muted || state.deafened != activeSettings.deafened)
            publish(state, activeSettings, context);
          const bool secured = encryption.ready();
          if (secured && !mediaReady) {
            std::lock_guard lock(mutex);
            frames.clear();
          }
          if (!secured && mediaReady) {
            sendSpeaking(false);
            remainingSilence = 0;
          }
          mediaReady = secured;
          if (described && discovered && !audioStarted) {
            std::string error;
            if (!audio->start(
                    [this, version = context.version](EncodedAudio frame) {
                      std::lock_guard lock(mutex);
                      if (this->version != version)
                        return;
                      if (frames.size() == maximumFrames)
                        frames.pop_front();
                      frames.push_back(std::move(frame));
                      (void)wake.signal();
                    },
                    error
                ))
              throw Failure(error.empty() ? "Could not open voice audio devices" : error);
            audioStarted = true;
            if (secured)
              state.phase = Phase::Connected;
            state.encrypted = secured;
            keepaliveAt = now + udpKeepaliveInterval;
            updateParticipants();
          }
          if (audioStarted && secured && state.phase != Phase::Connected) {
            securityDeadline = Clock::time_point::max();
            state.phase = Phase::Connected;
            state.encrypted = true;
            publish(state, activeSettings, context);
          }
          std::deque<EncodedAudio> queued;
          {
            std::lock_guard lock(mutex);
            queued.swap(frames);
          }
          if (audioStarted) {
            bool activityChanged = false;
            for (const auto& frame : queued) {
              const bool transmitting = frame.speaking
                  && !frame.opus.empty()
                  && !activeSettings.muted
                  && !activeSettings.deafened
                  && (!activeSettings.pushToTalk || activeSettings.pttPressed);
              activityChanged =
                  activity.capture(transmitting, activeSettings.muted, activeSettings.deafened, now) || activityChanged;
              if (!secured)
                continue;
              if (transmitting) {
                sendSpeaking(true);
                remainingSilence = silenceFrames;
                sendAudio(frame, false);
              } else if (remainingSilence) {
                sendAudio(frame, true);
                if (--remainingSilence == 0)
                  sendSpeaking(false);
              }
            }
            if (activityChanged)
              updateParticipants();
          }
          if (udp.fd >= 0 && now >= keepaliveAt) {
            std::array<std::uint8_t, sizeof(std::uint64_t)> keepalive{};
            auto number = keepaliveCounter++;
            for (auto& byte : keepalive) {
              byte = static_cast<std::uint8_t>(number);
              number >>= 8;
            }
            (void)::send(udp.fd, keepalive.data(), keepalive.size(), 0);
            keepaliveAt = now + udpKeepaliveInterval;
          }
          for (std::size_t count = 0; udp.fd >= 0 && count < maximumReceiveBatch; ++count) {
            std::array<std::uint8_t, protocol::maximumDatagramBytes + 1> bytes{};
            const auto received = ::recv(udp.fd, bytes.data(), bytes.size(), 0);
            if (received < 0) {
              if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                throw Failure("The voice network connection was interrupted");
              break;
            }
            if (received == 0)
              continue;
            const auto packet = std::span{bytes}.first(received);
            if (!discovered) {
              if (const auto address = protocol::discoveryResponse(packet, remote->ssrc)) {
                send(
                    curl.get(),
                    Json{
                        {"op", 1},
                        {"d",
                         {{"protocol", "udp"},
                          {"data",
                           {{"address", address->ip}, {"port", address->port}, {"mode", protocol::transportMode}}}}}
                    },
                    context
                );
                discovered = true;
                state.phase = Phase::Securing;
                publish(state, activeSettings, context);
              }
              continue;
            }
            if (!described || !audioStarted || activeSettings.deafened)
              continue;
            auto decrypted = cipher.open(packet);
            if (!decrypted || decrypted->ssrc == remote->ssrc)
              continue;
            const auto sender = senders.find(decrypted->ssrc);
            if (sender == senders.end())
              continue;
            const bool relaySilence = std::ranges::equal(decrypted->payload, protocol::silence);
            const auto opus =
                relaySilence ? decrypted->payload : encryption.decrypt(sender->second, decrypted->payload);
            if (!opus.empty()) {
              const bool silence = std::ranges::equal(opus, protocol::silence);
              if (activity.packet(sender->second, true, silence, now))
                updateParticipants();
              audio->receive(decrypted->ssrc, decrypted->sequence, decrypted->timestamp, opus);
            }
          }
          for (std::size_t count = 0; count < maximumReceiveBatch; ++count) {
            std::array<std::uint8_t, protocol::maximumDatagramBytes> bytes{};
            std::size_t received = 0;
            const curl_ws_frame* metadata = nullptr;
            const auto status = curl_ws_recv(curl.get(), bytes.data(), bytes.size(), &received, &metadata);
            if (status == CURLE_AGAIN)
              break;
            if (status != CURLE_OK || !metadata || (metadata->flags & CURLWS_CLOSE))
              throw Failure("The voice server closed the connection");
            if (metadata->flags & (CURLWS_PING | CURLWS_PONG))
              continue;
            if (metadata->offset == 0 && signal.empty())
              binary = metadata->flags & CURLWS_BINARY;
            if (received > protocol::maximumSignalBytes - signal.size())
              throw Failure("The voice server sent an oversized message");
            signal.insert(signal.end(), bytes.begin(), bytes.begin() + received);
            if (metadata->bytesleft || (metadata->flags & CURLWS_CONT))
              continue;
            if (binary) {
              if (signal.size() < 3)
                throw Failure("The voice server sent a malformed binary message");
              acknowledgedSequence = protocol::read16(signal, 0);
              const int opcode = signal[2];
              recordOpcode(opcode);
              if (opcode == 25 || opcode == 27 || opcode == 29 || opcode == 30)
                encryptionSignal({opcode, {signal.begin() + 3, signal.end()}, {}});
            } else {
              const auto payload = Json::parse(signal.begin(), signal.end(), nullptr, false);
              if (!payload.is_object()
                  || !payload.contains("op")
                  || !payload["op"].is_number_integer()
                  || !payload.contains("d"))
                throw Failure("The voice server sent a malformed message");
              if (payload.contains("seq")) {
                const auto sequence = uint32(payload["seq"]);
                if (!sequence || *sequence > std::numeric_limits<std::uint16_t>::max())
                  throw Failure("The voice server sent an invalid sequence");
                acknowledgedSequence = *sequence;
              }
              const auto opcode = payload["op"].get<int>();
              recordOpcode(opcode);
              const auto& data = payload["d"];
              if (opcode == 8) {
                if (!heartbeat.hello(data, now))
                  throw Failure("The voice server sent an invalid heartbeat interval");
                send(
                    curl.get(),
                    Json{
                        {"op", 0},
                        {"d",
                         {{"server_id", info.server},
                          {"user_id", info.user},
                          {"session_id", info.session},
                          {"token", info.token},
                          {"video", false},
                          {"max_dave_protocol_version", Encryption::maximumVersion()}}}
                    },
                    context
                );
              } else if (opcode == 6)
                heartbeat.acknowledge(data);
              else if (opcode == 2) {
                if (!heartbeat.initialized() || remote || !(remote = protocol::ready(data)))
                  throw Failure("The voice server does not support the required audio transport");
                openUdp(udp, *remote);
                discoveryDeadline = now + discoveryTimeout;
              } else if (opcode == 4) {
                if (!remote || !discovered || described)
                  throw Failure("The voice server returned an unexpected session description");
                auto description = protocol::session(data, Encryption::maximumVersion());
                if (!description)
                  throw Failure("The voice server did not establish an encrypted session");
                const bool accepted = cipher.setKey(description->key) && encryption.initialize(description->version);
                sodium_memzero(description->key.data(), description->key.size());
                if (!accepted)
                  throw Failure("Encrypted voice setup failed");
                described = true;
                voiceLog.info("Connection phase=transport-ready");
                flushEncryption();
                while (!early.empty()) {
                  auto message = std::move(early.front());
                  early.pop_front();
                  encryptionSignal(std::move(message));
                }
                earlyBytes = 0;
              } else if (opcode == 11 || opcode == 12 || opcode == 5) {
                std::vector<std::string> users;
                if (opcode == 11 && data.is_object() && data.contains("user_ids") && data["user_ids"].is_array()) {
                  if (data["user_ids"].size() > protocol::maximumParticipants)
                    throw Failure("The voice call has too many participants");
                  for (const auto& item : data["user_ids"])
                    if (item.is_string() && protocol::validId(item.get_ref<const std::string&>()))
                      users.push_back(item.get<std::string>());
                } else if (auto user = userId(data); !user.empty())
                  users.push_back(std::move(user));
                for (const auto& user : users) {
                  if (!participants.contains(user) && participants.size() >= protocol::maximumParticipants)
                    throw Failure("The voice call has too many participants");
                  participants.try_emplace(user, false);
                  if (opcode == 5 && data.contains("speaking"))
                    if (const auto flags = uint32(data["speaking"]))
                      activity.hint(user, (*flags & 1) != 0, now);
                  encryption.addUser(user);
                  const char* key = opcode == 12 ? "audio_ssrc" : "ssrc";
                  if (data.is_object() && data.contains(key))
                    if (const auto ssrc = uint32(data[key]); ssrc && *ssrc) {
                      if (senders.size() >= protocol::maximumParticipants && !senders.contains(*ssrc))
                        throw Failure("The voice call has too many audio streams");
                      senders[*ssrc] = user;
                    }
                }
                updateParticipants();
              } else if (opcode == 13) {
                const auto user = userId(data);
                if (!user.empty()) {
                  participants.erase(user);
                  activity.remove(user);
                  encryption.removeUser(user);
                  std::erase_if(senders, [&](const auto& sender) {
                    if (sender.second != user)
                      return false;
                    audio->remove(sender.first);
                    return true;
                  });
                  updateParticipants();
                }
              } else if (opcode == 21 || opcode == 22 || opcode == 24)
                encryptionSignal({opcode, {}, data});
            }
            signal.clear();
          }
          auto deadline = std::min(now + mediaTick, heartbeat.initialized() ? heartbeat.deadline() : setupDeadline);
          if (!discovered)
            deadline = std::min(deadline, discoveryDeadline);
          wait(curl.get(), udp.fd, POLLIN, context, deadline);
        }
      } catch (const std::exception& error) {
        if (!context.cancelled()) {
          state.phase = Phase::Failed;
          state.error = error.what();
          state.encrypted = false;
          for (auto& participant : state.participants)
            participant.speaking = false;
          publish(state, activeSettings, context);
          for (const auto opcode : recentOpcodes)
            voiceLog.debug("Recent signal opcode={}", opcode);
        }
      }
      if (audioStarted)
        audio->stop();
      if (!context.stop.stop_requested() && context.cancelled()) {
        state.phase = Phase::Disconnected;
        state.encrypted = false;
        state.participants.clear();
        diagnosticPhase = state.phase;
        voiceLog.info("Connection phase=disconnected");
        if (changed)
          changed(std::move(state));
      }
    }
    void run(std::stop_token stop) {
      std::uint64_t seen = 0;
      while (!stop.stop_requested()) {
        std::optional<ConnectionInfo> info;
        std::uint64_t current;
        {
          std::unique_lock lock(mutex);
          ready.wait(lock, stop, [&] { return version != seen; });
          if (stop.stop_requested())
            return;
          current = version;
          seen = current;
          info = desired;
        }
        if (info)
          session(std::move(*info), {stop, version, current, wake});
      }
    }
    std::shared_ptr<Audio> audio;
    Changed changed;
    Phase diagnosticPhase = Phase::Disconnected;
    std::string endpointOverride;
    std::mutex mutex;
    std::condition_variable_any ready;
    process::WakeEvent wake;
    std::atomic<std::uint64_t> version{0};
    std::uint64_t settingsRevision = 0;
    std::uint64_t membershipRevision = 0;
    std::vector<std::string> membership;
    std::optional<ConnectionInfo> desired;
    AudioSettings settings;
    std::deque<EncodedAudio> frames;
    std::jthread worker;
  };
  Connection::Connection(std::shared_ptr<Audio> audio, Changed changed, std::string loopbackEndpoint)
      : m_impl(std::make_unique<Impl>(std::move(audio), std::move(changed), std::move(loopbackEndpoint))) {}
  Connection::~Connection() = default;
  bool Connection::connect(ConnectionInfo info) { return m_impl->connect(std::move(info)); }
  void Connection::disconnect() { m_impl->disconnect(); }
  void Connection::configure(AudioSettings settings) { m_impl->configure(std::move(settings)); }
  void Connection::participants(std::vector<std::string> users) { m_impl->participants(std::move(users)); }
} // namespace voice
