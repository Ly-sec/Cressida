#include "voice/connection.h"

#include "core/log.h"
#include "core/process/wake_event.h"
#include "voice/encryption.h"
#include "voice/media_clock.h"
#include "voice/protocol.h"
#include "voice/speaking.h"
#include "voice/stream_audio.h"
#include "voice/video_transport.h"

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
    constexpr unsigned microphoneSpeakingFlag = 1U << 0;
    constexpr unsigned soundshareSpeakingFlag = 1U << 1;
    constexpr auto connectTimeout = 15s;
    constexpr auto setupTimeout = 30s;
    constexpr auto discoveryTimeout = 5s;
    constexpr auto sendTimeout = 5s;
    constexpr auto mediaTick = 20ms;
    constexpr auto udpKeepaliveInterval = 5s;
    constexpr std::size_t maximumReceiveBatch = 64;
    constexpr std::size_t maximumDiagnosticOpcodes = 32;
    constexpr std::size_t diagnosticHistory = 8;
    constexpr auto maximumMediaRoundTrip = 10s;
    constexpr std::uint32_t rtcpClockRate = 65536;
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
      std::shared_ptr<std::atomic_bool> revoked;
      bool cancelled() const { return stop.stop_requested() || revision != version || (revoked && revoked->load()); }
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
    struct VideoQueue {
      std::mutex mutex;
      std::deque<EncodedVideo> frames;
      bool dropped = false;
    };
  } // namespace

  class Connection::Impl {
  public:
    Impl(
        std::shared_ptr<Audio> audio, Changed changed, std::string overrideEndpoint,
        std::shared_ptr<VideoPipeline> video
    )
        : audio(std::move(audio)), video(std::move(video)), changed(std::move(changed)) {
      if (!overrideEndpoint.empty()) {
        const auto validated = testEndpoint(overrideEndpoint);
        if (!validated)
          throw std::invalid_argument("Voice test endpoint must use an explicit loopback socket");
        endpointOverride = *validated;
      }
      worker = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ~Impl() {
      if (video) {
        video->sending(false);
        video->capture({});
      }
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
    void pauseVideo(bool value) {
      paused = value;
      (void)wake.signal();
    }
    void videoSubscription(int height, int frameRate) {
      const auto previous = subscriptionHeight.exchange(std::clamp(height, 0, maximumVideoDimension));
      subscriptionRate = std::clamp(frameRate, 0, streamFrameRates.back());
      if (previous && !subscriptionHeight && video)
        video->clear();
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
        auto videoQueue = std::make_shared<VideoQueue>();
        std::map<std::string, VideoRoute> offered;
        Json codecs = Json::array({{{"name", "opus"}, {"type", "audio"}, {"priority", 1000}, {"payload_type", 120}}});
        std::vector<std::uint8_t> payloadTypes{120};
        if (video) {
          unsigned index = 0;
          for (const auto& support : videoCodecs()) {
            VideoRoute route{
                support.codec, static_cast<std::uint8_t>(101 + index * 2), static_cast<std::uint8_t>(102 + index * 2),
                0, 0
            };
            offered.emplace(std::string(codecName(support.codec)), route);
            codecs.push_back(
                {{"name", codecName(support.codec)},
                 {"type", "video"},
                 {"priority", 1000 * (++index)},
                 {"payload_type", route.payloadType},
                 {"rtx_payload_type", route.rtxPayloadType},
                 {"encode", support.encode},
                 {"decode", support.decode}}
            );
            payloadTypes.push_back(route.payloadType);
            payloadTypes.push_back(route.rtxPayloadType);
          }
          cipher.payloadTypes(payloadTypes);
          video->capture([weak = std::weak_ptr(videoQueue)](EncodedVideo frame) {
            if (auto queue = weak.lock()) {
              std::lock_guard lock(queue->mutex);
              constexpr std::size_t maximumQueuedVideoFrames = 2;
              if (queue->frames.size() >= maximumQueuedVideoFrames) {
                queue->frames.clear();
                queue->dropped = true;
              }
              queue->frames.push_back(std::move(frame));
            }
          });
        }
        VideoRoute outgoing, incoming;
        VideoAssembler assembler;
        VideoRetransmit retransmit;
        VideoPacer videoPacer;
        VideoReception reception;
        bool needsVideoKeyframe = true;
        int remoteVideoBitrate = maximumVideoBitrate;
        std::uint64_t remoteVideoPixels = 0;
        if (video)
          video->networkConstraints(0, 0);
        const auto constrainVideo = [&] {
          if (video->networkConstraints(remoteVideoBitrate, remoteVideoPixels)) {
            videoPacer.clear();
            retransmit.clear();
            needsVideoKeyframe = true;
            {
              std::lock_guard lock(videoQueue->mutex);
              videoQueue->frames.clear();
              videoQueue->dropped = false;
            }
            video->keyframe();
          }
        };
        std::string videoSender;
        std::uint16_t videoSequence = 0;
        randombytes_buf(&videoSequence, sizeof(videoSequence));
        bool announced = false, wasPaused = false;
        int requestedHeight = -1, requestedRate = -1;
        auto nextKeyframeRequest = Clock::time_point{};
        auto nextVideoReport = Clock::time_point{};
        auto nextReceiverReport = Clock::time_point{};
        std::deque<std::uint32_t> recentVideoReports;
        std::uint32_t videoPacketCount = 0, videoOctetCount = 0, lastVideoTimestamp = 0;
        std::uint32_t audioPacketCount = 0, audioOctetCount = 0, lastAudioTimestamp = 0;
        std::int64_t lastVideoAt = 0, lastAudioAt = 0;
        MediaClock mediaClock;
        const auto streamAudio = std::dynamic_pointer_cast<StreamAudio>(audio);
        bool remoteWantsVideo = true;
        VideoQuality announcedQuality{};
        auto selectCodec = [&](const Json& data, bool required) {
          if (!video)
            return;
          if (!data.contains("video_codec") || !data["video_codec"].is_string()) {
            if (required)
              throw Failure("The stream server did not select a video codec.");
            return;
          }
          const auto selected = offered.find(data["video_codec"].get<std::string>());
          if (selected == offered.end())
            throw Failure("The stream server selected an unsupported video codec.");
          outgoing.codec = incoming.codec = selected->second.codec;
          outgoing.payloadType = incoming.payloadType = selected->second.payloadType;
          outgoing.rtxPayloadType = incoming.rtxPayloadType = selected->second.rtxPayloadType;
          assembler.route(incoming);
          video->codec(selected->second.codec);
        };
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
              curl.get(),
              Json{
                  {"op", 5},
                  {"d",
                   {{"speaking", value ? (info.stream ? soundshareSpeakingFlag : microphoneSpeakingFlag) : 0U},
                    {"delay", 0},
                    {"ssrc", remote->ssrc}}}
              },
              context
          );
          speaking = value;
        };
        auto sendAudio = [&](const EncodedAudio& frame, bool silence) {
          if (context.cancelled() || (info.stream && !info.publishing))
            return;
          const auto input =
              silence ? std::span<const std::uint8_t>{protocol::silence} : std::span<const std::uint8_t>{frame.opus};
          auto encrypted = encryption.encrypt(remote->ssrc, input);
          if (encrypted.empty())
            throw Failure("Voice audio encryption failed");
          const auto octets = encrypted.size();
          const auto packet = cipher.seal({remote->ssrc, frame.timestamp, sequence++, std::move(encrypted)});
          if (packet.empty())
            throw Failure("Voice transport encryption failed");
          const auto sent = ::send(udp.fd, packet.data(), packet.size(), 0);
          if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            throw Failure("The voice network connection was interrupted");
          if (sent == static_cast<ssize_t>(packet.size())) {
            ++audioPacketCount;
            audioOctetCount += octets;
            lastAudioTimestamp = frame.timestamp;
            lastAudioAt = frame.capturedAtUs
                ? frame.capturedAtUs
                : std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
          }
        };
        auto sendDatagram = [&](const std::vector<std::uint8_t>& packet) {
          if (packet.empty() || context.cancelled())
            return false;
          const auto written = ::send(udp.fd, packet.data(), packet.size(), 0);
          if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            throw Failure("The stream network connection was interrupted");
          return written == static_cast<ssize_t>(packet.size());
        };
        auto announceVideo = [&](bool active) {
          if (!video || !info.publishing || !outgoing.ssrc)
            return;
          const auto stats = video->statistics();
          const auto image = video->frame();
          const auto height = stats.quality.height;
          const auto width = image && image->height
              ? static_cast<int>(static_cast<std::int64_t>(image->width) * height / image->height)
              : height;
          send(
              curl.get(),
              Json{
                  {"op", 12},
                  {"d",
                   {{"audio_ssrc", remote->ssrc},
                    {"video_ssrc", outgoing.ssrc},
                    {"rtx_ssrc", outgoing.rtxSsrc},
                    {"streams",
                     Json::array(
                         {{{"type", "screen"},
                           {"rid", "100"},
                           {"ssrc", outgoing.ssrc},
                           {"rtx_ssrc", outgoing.rtxSsrc},
                           {"active", active},
                           {"quality", 100},
                           {"max_bitrate", stats.quality.bitrate},
                           {"max_framerate", stats.quality.frameRate},
                           {"max_resolution", {{"type", "fixed"}, {"width", width}, {"height", height}}}}}
                     )}}}
              },
              context
          );
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
          if (settingsChanged
              && audioStarted
              && !info.stream
              && (activeSettings.muted
                  || activeSettings.deafened
                  || (activeSettings.pushToTalk && !activeSettings.pttPressed)))
            sendSpeaking(false);
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
          if (video) {
            constrainVideo();
            const bool pause = paused.load();
            const bool transmitting = secured && discovered && info.publishing && !pause && remoteWantsVideo;
            video->sending(transmitting);
            if (!transmitting) {
              videoPacer.clear();
              retransmit.clear();
              needsVideoKeyframe = true;
            }
            const auto quality = video->statistics().quality;
            if (described
                && discovered
                && info.publishing
                && (!announced || pause != wasPaused || announcedQuality != quality)) {
              announceVideo(!pause);
              announced = true;
              wasPaused = pause;
              announcedQuality = quality;
            }
            const int height = subscriptionHeight.load(), rate = subscriptionRate.load();
            if (described
                && !info.publishing
                && incoming.ssrc
                && (height != requestedHeight || rate != requestedRate)) {
              Json wants{
                  {std::to_string(incoming.ssrc), height && rate ? 100 : 0},
                  {"pixelCounts",
                   {{std::to_string(incoming.ssrc), static_cast<std::uint64_t>(height) * height * 16 / 9}}}
              };
              send(curl.get(), Json{{"op", 15}, {"d", wants}}, context);
              assembler.clear();
              if (!height || !rate)
                reception.clear();
              if (height && rate && (!requestedHeight || !requestedRate))
                sendDatagram(cipher.sealControl(videoKeyframeRequest(remote->ssrc, incoming.ssrc)));
              requestedHeight = height;
              requestedRate = rate;
            }
            if (described && discovered && secured) {
              std::deque<EncodedVideo> captured;
              bool dropped = false;
              {
                std::lock_guard lock(videoQueue->mutex);
                captured.swap(videoQueue->frames);
                dropped = std::exchange(videoQueue->dropped, false);
              }
              if (transmitting && dropped) {
                captured.clear();
                videoPacer.clear();
                retransmit.clear();
                needsVideoKeyframe = true;
                video->keyframe();
                video->feedback(1, maximumVideoQueueDelay);
              }
              if (transmitting)
                for (const auto& frame : captured) {
                  if (needsVideoKeyframe && !frame.keyframe)
                    continue;
                  const auto protectedFrame = encryption.encrypt(outgoing.ssrc, frame.bytes, frame.codec);
                  if (protectedFrame.empty())
                    throw Failure("Stream frame encryption failed.");
                  auto packets = packetizeVideo(protectedFrame, frame.timestamp, outgoing, videoSequence);
                  if (!videoPacer.enqueue(
                          std::move(packets), std::min(quality.bitrate, remoteVideoBitrate), now, frame.capturedAtUs
                      )) {
                    needsVideoKeyframe = true;
                    break;
                  }
                  needsVideoKeyframe = false;
                }
              for (const auto& paced : videoPacer.take(now)) {
                const auto& packet = paced.packet;
                if (!sendDatagram(cipher.seal(packet))) {
                  videoPacer.clear();
                  needsVideoKeyframe = true;
                  video->keyframe();
                  video->feedback(1, maximumVideoQueueDelay);
                  break;
                }
                if (!paced.retransmission) {
                  retransmit.sent(packet, now);
                  ++videoPacketCount;
                  videoOctetCount += packet.payload.size();
                  lastVideoTimestamp = packet.timestamp;
                  lastVideoAt = paced.capturedAtUs
                      ? paced.capturedAtUs
                      : std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
                }
              }
              if (videoPacer.needsKeyframe()) {
                needsVideoKeyframe = true;
                retransmit.clear();
                video->keyframe();
                video->feedback(1, maximumVideoQueueDelay);
              }
              if (incoming.ssrc) {
                const auto missing = assembler.missing(now);
                if (!missing.empty())
                  sendDatagram(cipher.sealControl(videoNack(remote->ssrc, incoming.ssrc, missing)));
                if ((assembler.needsKeyframe() || video->needsKeyframe()) && now >= nextKeyframeRequest) {
                  sendDatagram(cipher.sealControl(videoKeyframeRequest(remote->ssrc, incoming.ssrc)));
                  nextKeyframeRequest = now + 1s;
                }
              }
              if (info.publishing && (videoPacketCount || audioPacketCount) && now >= nextVideoReport) {
                const auto mono = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
                const auto ntp = ntpTimestamp(std::chrono::system_clock::now());
                const auto report = [&](std::uint32_t ssrc, std::uint32_t timestamp, std::int64_t at, int rate,
                                        std::uint32_t packets, std::uint32_t octets) {
                  if (!packets)
                    return;
                  const auto elapsed = (mono - at) * rate / 1000000;
                  sendDatagram(cipher.sealControl(senderReport(
                      {ssrc, static_cast<std::uint32_t>(timestamp + elapsed), packets, octets, ntp},
                      "cressida-" + info.user
                  )));
                  if (ssrc == outgoing.ssrc) {
                    recentVideoReports.push_back(static_cast<std::uint32_t>(ntp >> 16));
                    if (recentVideoReports.size() > diagnosticHistory)
                      recentVideoReports.pop_front();
                  }
                };
                report(
                    outgoing.ssrc, lastVideoTimestamp, lastVideoAt, videoClockRate, videoPacketCount, videoOctetCount
                );
                report(remote->ssrc, lastAudioTimestamp, lastAudioAt, sampleRate, audioPacketCount, audioOctetCount);
                nextVideoReport = now + 1s;
              }
              if (!info.publishing
                  && incoming.ssrc
                  && subscriptionHeight
                  && subscriptionRate
                  && now >= nextReceiverReport) {
                sendDatagram(
                    cipher.sealControl(reception.report(remote->ssrc, incoming.ssrc, now, "cressida-" + info.user))
                );
                nextReceiverReport = now + 1s;
              }
            } else {
              std::lock_guard lock(videoQueue->mutex);
              videoQueue->frames.clear();
            }
          }
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
              if (!info.stream && !secured)
                sendSpeaking(transmitting);
              if (!secured || (info.stream && (!info.publishing || paused)))
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
                Json selection{
                    {"op", 1},
                    {"d",
                     {{"protocol", "udp"},
                      {"data", {{"address", address->ip}, {"port", address->port}, {"mode", protocol::transportMode}}}}}
                };
                if (video) {
                  std::array<std::uint8_t, 16> uuid{};
                  randombytes_buf(uuid.data(), uuid.size());
                  uuid[6] = (uuid[6] & 0x0f) | 0x40;
                  uuid[8] = (uuid[8] & 0x3f) | 0x80;
                  constexpr std::string_view hex = "0123456789abcdef";
                  std::string id;
                  for (std::size_t i = 0; i < uuid.size(); ++i) {
                    if (i == 4 || i == 6 || i == 8 || i == 10)
                      id += '-';
                    id += hex[uuid[i] >> 4];
                    id += hex[uuid[i] & 0xf];
                  }
                  selection["d"]["codecs"] = codecs;
                  selection["d"]["rtc_connection_id"] = id;
                  selection["d"]["experiments"] = Json::array();
                }
                send(curl.get(), selection, context);
                discovered = true;
                state.phase = Phase::Securing;
                publish(state, activeSettings, context);
              }
              continue;
            }
            if (!described || !audioStarted)
              continue;
            if (video && packet.size() >= 2 && packet[1] >= 200 && packet[1] <= 206) {
              const auto control = cipher.openControl(packet);
              if (!info.publishing)
                if (const auto reports = senderReports(control))
                  for (const auto& report : *reports) {
                    if (incoming.ssrc && report.ssrc == incoming.ssrc) {
                      mediaClock.report(report, videoClockRate, now);
                      reception.synchronization(report.ntp, now);
                    } else if (
                        const auto user = senders.find(report.ssrc);
                        user != senders.end() && user->second == info.publisher
                    )
                      mediaClock.report(report, sampleRate, now);
                  }
              if (const auto feedback = videoFeedback(control, outgoing.ssrc)) {
                if (feedback->keyframe) {
                  videoPacer.clear();
                  needsVideoKeyframe = true;
                  video->keyframe();
                }
                if (feedback->bitrate)
                  remoteVideoBitrate = static_cast<int>(
                      std::clamp<std::uint64_t>(feedback->bitrate, minimumVideoBitrate, maximumVideoBitrate)
                  );
                auto delay = std::chrono::milliseconds{};
                if (feedback->receiverReport
                    && feedback->lastSenderReport
                    && std::ranges::find(recentVideoReports, feedback->lastSenderReport) != recentVideoReports.end()) {
                  const auto current = static_cast<std::uint32_t>(ntpTimestamp(std::chrono::system_clock::now()) >> 16);
                  const auto ticks = current - feedback->lastSenderReport - feedback->senderReportDelay;
                  if (ticks <= maximumMediaRoundTrip.count() * rtcpClockRate)
                    delay = std::chrono::milliseconds{static_cast<std::int64_t>(ticks) * 1000 / rtcpClockRate};
                }
                if (feedback->receiverReport)
                  video->feedback(feedback->loss, delay);
                if (info.publishing && !paused && remoteWantsVideo && secured)
                  videoPacer.enqueue(
                      retransmit.request(feedback->lost, outgoing, now),
                      std::min(video->statistics().quality.bitrate, remoteVideoBitrate), now, 0, true
                  );
              }
              continue;
            }
            auto decrypted = cipher.open(packet);
            if (!decrypted || decrypted->ssrc == remote->ssrc)
              continue;
            if (video && decrypted->payloadType != 120) {
              if (!subscriptionHeight || !subscriptionRate)
                continue;
              if (decrypted->ssrc == incoming.ssrc && decrypted->payloadType == incoming.payloadType)
                reception.packet(decrypted->sequence, decrypted->timestamp, now);
              else if (
                  decrypted->ssrc == incoming.rtxSsrc
                  && decrypted->payloadType == incoming.rtxPayloadType
                  && decrypted->payload.size() > 2
              )
                reception.packet(protocol::read16(decrypted->payload, 0), decrypted->timestamp, now);
              if (const auto frame = assembler.receive(std::move(*decrypted), now)) {
                auto plain = encryption.decrypt(videoSender, frame->bytes, frame->codec);
                if (!plain.empty()) {
                  auto encoded = *frame;
                  encoded.bytes = std::move(plain);
                  encoded.presentAt = mediaClock.due(incoming.ssrc, encoded.timestamp, videoClockRate, now);
                  video->receive(std::move(encoded));
                }
              }
              continue;
            }
            if (activeSettings.deafened)
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
              if (info.stream && streamAudio)
                streamAudio->receiveAt(
                    decrypted->ssrc, decrypted->sequence, decrypted->timestamp, opus,
                    mediaClock.due(decrypted->ssrc, decrypted->timestamp, sampleRate, now)
                );
              else
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
                          {"channel_id", info.channel},
                          {"video", static_cast<bool>(video)},
                          {"streams",
                           video ? Json::array({{{"type", "screen"}, {"rid", "100"}, {"quality", 100}}})
                                 : Json::array()},
                          {"max_dave_protocol_version", Encryption::maximumVersion()}}}
                    },
                    context
                );
              } else if (opcode == 6)
                heartbeat.acknowledge(data);
              else if (opcode == 2) {
                if (!heartbeat.initialized() || remote || !(remote = protocol::ready(data)))
                  throw Failure("The voice server does not support the required audio transport");
                if (video && info.publishing) {
                  if (data.contains("streams") && data["streams"].is_array() && !data["streams"].empty()) {
                    const auto& stream = data["streams"].front();
                    const auto ssrc = stream.contains("ssrc") ? uint32(stream["ssrc"]) : std::nullopt;
                    const auto rtx = stream.contains("rtx_ssrc") ? uint32(stream["rtx_ssrc"]) : std::nullopt;
                    if (!ssrc || !*ssrc || !rtx || !*rtx || *ssrc == *rtx)
                      throw Failure("The stream server supplied invalid video routing.");
                    outgoing.ssrc = *ssrc;
                    outgoing.rtxSsrc = *rtx;
                  } else {
                    if (remote->ssrc > std::numeric_limits<std::uint32_t>::max() - 2)
                      throw Failure("The stream server supplied invalid video routing.");
                    outgoing.ssrc = remote->ssrc + 1;
                    outgoing.rtxSsrc = remote->ssrc + 2;
                  }
                }
                openUdp(udp, *remote);
                discoveryDeadline = now + discoveryTimeout;
              } else if (opcode == 4) {
                if (!remote || !discovered || described)
                  throw Failure("The voice server returned an unexpected session description");
                auto description = protocol::session(data, Encryption::maximumVersion());
                if (!description)
                  throw Failure("The voice server did not establish an encrypted session");
                selectCodec(data, true);
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
                  if (video && !info.publishing && opcode == 12 && user == info.publisher) {
                    std::optional<std::uint32_t> videoSsrc, rtxSsrc;
                    if (data.contains("streams") && data["streams"].is_array()) {
                      if (data["streams"].size() > maximumPendingSignals)
                        throw Failure("The stream server supplied too many video layers.");
                      for (const auto& stream : data["streams"]) {
                        if (!stream.is_object() || !stream.value("active", true))
                          continue;
                        if (stream.contains("ssrc"))
                          videoSsrc = uint32(stream["ssrc"]);
                        if (stream.contains("rtx_ssrc"))
                          rtxSsrc = uint32(stream["rtx_ssrc"]);
                        if (videoSsrc && *videoSsrc)
                          break;
                      }
                    }
                    if (!videoSsrc && data.contains("video_ssrc"))
                      videoSsrc = uint32(data["video_ssrc"]);
                    if (!rtxSsrc && data.contains("rtx_ssrc"))
                      rtxSsrc = uint32(data["rtx_ssrc"]);
                    if (videoSsrc && *videoSsrc && rtxSsrc && *rtxSsrc && *videoSsrc != *rtxSsrc) {
                      incoming.ssrc = *videoSsrc;
                      incoming.rtxSsrc = *rtxSsrc;
                      videoSender = user;
                      assembler.route(incoming);
                      reception.clear();
                      requestedHeight = -1;
                      requestedRate = -1;
                      sendDatagram(cipher.sealControl(videoKeyframeRequest(remote->ssrc, incoming.ssrc)));
                    }
                  }
                }
                updateParticipants();
              } else if (opcode == 14 && video) {
                selectCodec(data, false);
                announced = false;
                requestedHeight = -1;
              } else if (opcode == 15 && video && info.publishing && data.is_object()) {
                const auto key = std::to_string(outgoing.ssrc);
                if (data.contains(key)) {
                  if (const auto wants = uint32(data[key]))
                    remoteWantsVideo = *wants > 0;
                } else if (data.contains("any")) {
                  if (const auto wants = uint32(data["any"]))
                    remoteWantsVideo = *wants > 0;
                }
                if (!data.contains("pixelCounts") || data["pixelCounts"].is_null())
                  remoteVideoPixels = 0;
                else if (data["pixelCounts"].is_object()) {
                  const auto count = data["pixelCounts"].find(key);
                  if (count == data["pixelCounts"].end())
                    remoteVideoPixels = 0;
                  else if (const auto pixels = uint32(*count))
                    remoteVideoPixels = *pixels;
                }
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
          if (video)
            deadline = std::min(deadline, videoPacer.deadline());
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
      if (video) {
        video->sending(false);
        video->capture({});
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
        if (info) {
          auto revoked = info->revoked;
          session(std::move(*info), {stop, version, current, wake, std::move(revoked)});
        }
      }
    }
    std::shared_ptr<Audio> audio;
    std::shared_ptr<VideoPipeline> video;
    Changed changed;
    Phase diagnosticPhase = Phase::Disconnected;
    std::string endpointOverride;
    std::mutex mutex;
    std::condition_variable_any ready;
    process::WakeEvent wake;
    std::atomic<std::uint64_t> version{0};
    std::atomic_bool paused = false;
    std::atomic_int subscriptionHeight = 720, subscriptionRate = 30;
    std::uint64_t settingsRevision = 0;
    std::uint64_t membershipRevision = 0;
    std::vector<std::string> membership;
    std::optional<ConnectionInfo> desired;
    AudioSettings settings;
    std::deque<EncodedAudio> frames;
    std::jthread worker;
  };
  Connection::Connection(
      std::shared_ptr<Audio> audio, Changed changed, std::string loopbackEndpoint, std::shared_ptr<VideoPipeline> video
  )
      : m_impl(
            std::make_unique<Impl>(std::move(audio), std::move(changed), std::move(loopbackEndpoint), std::move(video))
        ) {}
  Connection::~Connection() = default;
  bool Connection::connect(ConnectionInfo info) { return m_impl->connect(std::move(info)); }
  void Connection::disconnect() { m_impl->disconnect(); }
  void Connection::configure(AudioSettings settings) { m_impl->configure(std::move(settings)); }
  void Connection::participants(std::vector<std::string> users) { m_impl->participants(std::move(users)); }
  void Connection::pauseVideo(bool paused) { m_impl->pauseVideo(paused); }
  void Connection::videoSubscription(int height, int rate) { m_impl->videoSubscription(height, rate); }
} // namespace voice
