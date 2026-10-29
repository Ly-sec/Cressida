#include "voice/connection.h"
#include "voice/encryption.h"
#include "voice/protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <curl/curl.h>
#include <iostream>
#include <mls/messages.h>
#include <mutex>
#include <stdexcept>

namespace {
  using Bytes = std::vector<std::uint8_t>;
  using Json = nlohmann::json;
  const Bytes testOpus{0x78, 0x42, 0x19, 0x03, 0x8a, 0xff};
  const Bytes pendingOpus{0x70, 0x11, 0x22, 0x33, 0x44, 0x55};
  class Audio final : public voice::Audio {
  public:
    explicit Audio(bool media) : media(media) {}
    bool start(Capture capture, std::string&) override {
      ++starts;
      {
        std::lock_guard lock(mutex);
        this->capture = capture;
      }
      capture({pendingOpus, voice::frameSamples, true});
      return true;
    }
    void secureCapture() {
      Capture callback;
      {
        std::lock_guard lock(mutex);
        if (!media || secureEmitted || !capture)
          return;
        secureEmitted = true;
        callback = capture;
      }
      callback({testOpus, voice::frameSamples * 2, true});
    }
    void stop() override { ++stops; }
    void configure(voice::AudioSettings) override {}
    void receive(
        std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, std::span<const std::uint8_t> opus
    ) override {
      std::lock_guard lock(mutex);
      receivedFrame = {ssrc, timestamp, sequence, Bytes(opus.begin(), opus.end())};
      ++received;
      ready.notify_all();
    }
    void remove(std::uint32_t) override {}
    voice::AudioState state() const override { return {}; }
    void refreshDevices() override {}
    bool startTest(std::string&) override { return false; }
    void stopTest() override {}
    int eventFd() const noexcept override { return -1; }
    void drainEvents() override {}
    std::atomic<unsigned> starts{0}, stops{0}, received{0};
    std::mutex mutex;
    std::condition_variable ready;
    voice::protocol::Packet receivedFrame;
    bool media;
    Capture capture;
    bool secureEmitted = false;
  };
  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }
  std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    for (auto byte : bytes) {
      text += digits[byte >> 4];
      text += digits[byte & 0xf];
    }
    return text;
  }
  Bytes unhex(const std::string& text) {
    require(text.size() % 2 == 0, "The fixture supplies complete hexadecimal bytes");
    Bytes bytes(text.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      unsigned byte = 0;
      const auto parsed = std::from_chars(text.data() + i * 2, text.data() + i * 2 + 2, byte, 16);
      require(parsed.ec == std::errc{} && parsed.ptr == text.data() + i * 2 + 2, "Fixture bytes decode exactly");
      bytes[i] = static_cast<std::uint8_t>(byte);
    }
    return bytes;
  }
  Bytes message(voice::Encryption& encryption, int opcode) {
    auto messages = encryption.takeMessages();
    const auto found = std::ranges::find(messages, opcode, &voice::EncryptionMessage::opcode);
    require(
        found != messages.end() && found->binary && !found->bytes.empty(),
        "An expected encrypted handshake response exists"
    );
    return std::move(found->bytes);
  }
  Bytes transition(Bytes payload, std::uint16_t id) {
    payload.insert(payload.begin(), {static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id)});
    return payload;
  }
  struct Server {
    static constexpr std::uint64_t room = 3;
    cressida_mls::CipherSuite suite{cressida_mls::CipherSuite::ID::P256_AES128GCM_SHA256_P256};
    cressida_mls::SignaturePrivateKey signature = cressida_mls::SignaturePrivateKey::generate(suite);
    cressida_mls::bytes_ns::bytes group = cressida_mls::bytes_ns::bytes(sizeof(room), 0);
    Server() {
      for (std::size_t i = 0; i < sizeof(room); ++i)
        group.at(i) = static_cast<std::uint8_t>(room >> ((sizeof(room) - i - 1) * 8));
    }
    Bytes sender() const {
      const cressida_mls::ExternalSender sender{
          signature.public_key, cressida_mls::Credential::basic({0x00, 0x01, 0x01, 0x00})
      };
      return cressida_mls::tls::marshal(sender);
    }
    Bytes propose(const Bytes& keyPackage) const {
      const auto package = cressida_mls::tls::get<cressida_mls::KeyPackage>(cressida_mls::bytes_ns::bytes(keyPackage));
      const auto proposal = cressida_mls::external_proposal(
          suite, group, 0, cressida_mls::Proposal{cressida_mls::Add{package}}, 0, signature
      );
      cressida_mls::tls::ostream messages;
      messages << false << std::vector<cressida_mls::MLSMessage>{proposal};
      return messages.bytes();
    }
    static std::pair<Bytes, Bytes> split(const Bytes& payload) {
      cressida_mls::tls::istream stream{cressida_mls::bytes_ns::bytes(payload)};
      cressida_mls::MLSMessage commit;
      cressida_mls::Welcome welcome;
      stream >> commit >> welcome;
      return {cressida_mls::tls::marshal(commit), cressida_mls::tls::marshal(welcome)};
    }
  };
  Json command() {
    std::string text;
    require(static_cast<bool>(std::getline(std::cin, text)), "The private fixture provides its next protocol record");
    return Json::parse(text);
  }
  void exchangeMedia(
      Audio& audio, bool deferred, bool creator, bool activityChecks, std::mutex& stateMutex,
      std::condition_variable& stateReady, const std::vector<voice::State>& states
  ) {
    using namespace std::chrono_literals;
    Server server;
    voice::Encryption remote("4", Server::room);
    remote.addUser("2");
    require(
        remote.initialize(1) && remote.handleBinary(25, server.sender()),
        "The private peer initializes authenticated group state"
    );
    const auto peerPackage = message(remote, 26);
    std::cout << Json{{"external", hex(server.sender())}}.dump() << std::endl;
    const auto package = unhex(command().at("package").get<std::string>());
    {
      std::unique_lock lock(stateMutex);
      require(
          stateReady.wait_for(
              lock, 2s,
              [&] {
                return !states.empty()
                    && !states.back().encrypted
                    && std::ranges::any_of(states.back().participants, [](const auto& participant) {
                         return participant.user == "2" && participant.speaking;
                       });
              }
          ),
          "Explicit join starts real local capture activity while group keys are pending"
      );
      require(audio.starts == 1, "Pending local metering uses one continuous audio capture session");
    }
    const std::uint16_t id = deferred ? 9 : 0;
    Bytes commit, welcome;
    if (creator) {
      require(!package.empty(), "The connection supplied its genuine joining key package");
      std::cout << Json{{"proposal", hex(server.propose(peerPackage))}}.dump() << std::endl;
      std::tie(commit, welcome) = Server::split(unhex(command().at("commit").get<std::string>()));
      require(
          remote.handleBinary(30, transition(welcome, id)),
          "The private peer accepts a welcome produced by the actual connection"
      );
    } else {
      require(
          remote.handleBinary(27, server.propose(package)),
          "The private peer authenticates the actual connection key package"
      );
      std::tie(commit, welcome) = Server::split(message(remote, 28));
      require(remote.handleBinary(29, transition(commit, id)), "The private peer accepts the authentic group commit");
    }
    if (deferred)
      require(
          remote.handleJson(22, {{"transition_id", id}}), "The private peer activates its server-confirmed ratchet"
      );
    const auto encrypted = remote.encrypt(124, testOpus);
    require(!encrypted.empty(), "The private peer creates an encrypted audio frame");
    std::array<std::uint8_t, voice::protocol::keyBytes> key{};
    for (std::size_t i = 0; i < key.size(); ++i)
      key[i] = static_cast<std::uint8_t>(i);
    voice::protocol::RtpCipher cipher;
    require(cipher.setKey(key), "The private relay uses the negotiated transport key");
    const auto packet = cipher.seal({124, voice::frameSamples * 2, 17, encrypted});
    const auto silence = cipher.seal({124, voice::frameSamples * 3, 18, remote.encrypt(124, voice::protocol::silence)});
    std::cout
        << Json{{creator ? "commit" : "welcome", hex(transition(creator ? commit : welcome, id))}, {"rtp", hex(packet)}, {"silence", hex(silence)}}
               .dump()
        << std::endl;
    if (deferred) {
      require(
          command().value("before_execute", false) && audio.starts == 1,
          "Local capture remains active while transmission waits for execute-transition"
      );
      std::cout << Json{{"gated", true}}.dump() << std::endl;
    }
    if (activityChecks) {
      require(command().value("untrusted", false), "The private fixture confirms it delivered a tampered RTP frame");
      std::lock_guard lock(stateMutex);
      require(
          audio.received == 0
              && !states.empty()
              && std::ranges::none_of(
                  states.back().participants, [](const auto& user) { return user.user == "4" && user.speaking; }
              ),
          "Unauthenticated network audio never creates playback or speaking activity"
      );
      std::cout << Json{{"ignored", true}}.dump() << std::endl;
    }
    const auto received = cipher.open(unhex(command().at("packet").get<std::string>()));
    require(
        received && received->ssrc == 123 && received->timestamp == voice::frameSamples * 2,
        "Actual connection capture produces authenticated RTP with its own SSRC and audio clock"
    );
    require(
        remote.decrypt("2", received->payload) == testOpus,
        "Only new post-key capture reaches the authenticated peer; pending capture is discarded"
    );
    std::unique_lock lock(audio.mutex);
    require(
        audio.ready.wait_for(lock, 2s, [&] { return audio.received == 1; }),
        "Encrypted relay audio reaches injected playback"
    );
    require(
        audio.starts == 1
            && audio.receivedFrame.ssrc == 124
            && audio.receivedFrame.sequence == 17
            && audio.receivedFrame.timestamp == voice::frameSamples * 2
            && audio.receivedFrame.payload == testOpus,
        "Playback receives exact authenticated peer audio and RTP timing after group setup"
    );
  }
} // namespace
int main(int argc, char** argv) {
  using namespace std::chrono_literals;
  try {
    require(argc == 3, "The offline fixture supplies a loopback origin and scenario");
    require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "Network initialization succeeds");
    const bool cancel = std::string_view(argv[2]) == "cancel";
    const bool media = std::string_view(argv[2]).starts_with("media");
    auto audio = std::make_shared<Audio>(media);
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<voice::State> states;
    {
      voice::Connection connection(
          audio,
          [&](voice::State state) {
            const bool connected = state.phase == voice::Phase::Connected;
            {
              std::lock_guard lock(mutex);
              states.push_back(std::move(state));
              ready.notify_all();
            }
            if (connected)
              audio->secureCapture();
          },
          argv[1]
      );
      voice::ConnectionInfo info{
          .server = "1",
          .user = "2",
          .session = "synthetic-session",
          .token = "synthetic-token",
          .endpoint = "voice-test.discord.media",
          .channel = "3",
          .generation = 11,
          .revision = 7,
          .participants = {}
      };
      auto invalid = info;
      invalid.endpoint = "https://127.0.0.1/";
      require(
          !connection.connect(std::move(invalid)),
          "Production endpoint validation remains enabled for explicit test transports"
      );
      require(connection.connect(info), "The synthetic join begins");
      if (std::string_view(argv[2]) == "media-hints")
        connection.participants({"4"});
      if (media) {
        exchangeMedia(
            *audio, std::string_view(argv[2]) == "media-transition", std::string_view(argv[2]) == "media-creator",
            std::string_view(argv[2]) == "media-activity", mutex, ready, states
        );
        std::unique_lock lock(mutex);
        const auto remoteSpeaking = [&] {
          return !states.empty() && std::ranges::any_of(states.back().participants, [](const auto& participant) {
            return participant.user == "4" && participant.speaking;
          });
        };
        require(
            remoteSpeaking(), "Authenticated non-silence RTP creates a remote speaking indication without an op5 hint"
        );
        const bool activityChecks = std::string_view(argv[2]) == "media-activity";
        if (activityChecks)
          std::cout << Json{{"voiced", true}}.dump() << std::endl;
        require(
            ready.wait_for(lock, 1s, [&] { return !remoteSpeaking(); }),
            "Remote speaking expires after the last received packet"
        );
        require(
            states.back().phase == voice::Phase::Connected, "Speaking expiry does not depend on disconnecting the call"
        );
        if (activityChecks) {
          lock.unlock();
          std::unique_lock audioLock(audio->mutex);
          require(
              audio->ready.wait_for(audioLock, 1s, [&] { return audio->received == 2; }),
              "Authenticated encrypted silence reaches playback"
          );
          require(
              std::ranges::equal(audio->receivedFrame.payload, voice::protocol::silence),
              "The explicit stop frame was decrypted before evaluating speaking activity"
          );
          audioLock.unlock();
          lock.lock();
          require(!remoteSpeaking(), "DAVE-encrypted silence clears the remote indicator immediately");
        }
      } else {
        std::unique_lock lock(mutex);
        require(
            ready.wait_for(
                lock, 8s,
                [&] {
                  return !states.empty()
                      && (states.back().phase == voice::Phase::Failed
                          || (cancel
                              && states.back().phase == voice::Phase::WaitingForParticipants
                              && std::ranges::any_of(states.back().participants, [](const auto& participant) {
                                   return participant.user == "2" && participant.speaking;
                                 })));
                }
            ),
            "The voice fixture reaches a bounded terminal or waiting phase"
        );
        require(
            states.back().generation == 11 && states.back().revision == 7 && states.back().channel == "3",
            "Every worker callback retains the captured account and join revision"
        );
        require(
            !states.back().encrypted && audio->received == 0,
            "No remote audio plays before the group encryption handshake completes"
        );
        if (cancel)
          require(
              states.back().phase == voice::Phase::WaitingForParticipants,
              "A lone room meters local speech while waiting for encrypted transmission keys"
          );
        else
          require(
              states.back().phase == voice::Phase::Failed && !states.back().error.empty(),
              "Invalid signaling and missed heartbeats fail visibly"
          );
      }
      voice::AudioSettings settings;
      settings.muted = settings.deafened = true;
      connection.configure(settings);
      const auto stopped = std::chrono::steady_clock::now();
      connection.disconnect();
      if (cancel || media) {
        std::unique_lock lock(mutex);
        require(
            ready.wait_for(
                lock, 2s, [&] { return !states.empty() && states.back().phase == voice::Phase::Disconnected; }
            ),
            "Cancellation wakes an idle voice socket promptly"
        );
      }
      require(std::chrono::steady_clock::now() - stopped < 2s, "Voice cancellation has a finite deadline");
    }
    require(
        media
            || (audio->starts == (cancel || std::string_view(argv[2]) == "heartbeat" ? 1U : 0U)
                && audio->received == 0),
        "Capture starts only after an explicit join completes transport setup, with no unencrypted playback"
    );
    curl_global_cleanup();
    std::cout << "Offline voice setup, encryption gating and cancellation passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
