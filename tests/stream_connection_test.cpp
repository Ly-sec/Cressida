#include "voice/connection.h"
#include "voice/media_clock.h"
#include "voice/video_transport.h"
#include "voice_test_peer.h"

#include <atomic>
#include <condition_variable>
#include <curl/curl.h>
#include <mutex>
#include <opus/opus.h>

namespace {
  using namespace voice_test;
  using namespace std::chrono_literals;
  class InjectedAudio final : public voice::Audio {
  public:
    std::mutex mutex;
    Capture callback;
    Bytes played;
    std::atomic<unsigned> received = 0;
    bool start(Capture capture, std::string&) override {
      {
        std::lock_guard lock(mutex);
        callback = capture;
      }
      capture({{1, 2, 3}, voice::frameSamples, true});
      return true;
    }
    void emit(const Bytes& opus) {
      Capture capture;
      {
        std::lock_guard lock(mutex);
        capture = callback;
      }
      if (capture)
        capture({opus, voice::frameSamples * 2, true});
    }
    void stop() override {
      std::lock_guard lock(mutex);
      callback = {};
    }
    void configure(voice::AudioSettings) override {}
    void receive(std::uint32_t, std::uint16_t, std::uint32_t, std::span<const std::uint8_t> bytes) override {
      std::lock_guard lock(mutex);
      played.assign(bytes.begin(), bytes.end());
      ++received;
    }
    void remove(std::uint32_t) override {}
    voice::AudioState state() const override { return {}; }
    void refreshDevices() override {}
    bool startTest(std::string&) override { return false; }
    void stopTest() override {}
    int eventFd() const noexcept override { return -1; }
    void drainEvents() override {}
  };
  Bytes opusFrame() {
    int error = 0;
    auto* encoder = opus_encoder_create(voice::sampleRate, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    require(encoder && error == OPUS_OK, "The stream fixture uses genuine stereo Opus");
    std::vector<opus_int16> samples(voice::frameSamples * 2);
    for (int i = 0; i < voice::frameSamples; ++i) {
      samples[i * 2] = (i % 31) * 20;
      samples[i * 2 + 1] = (i % 47) * 30;
    }
    constexpr std::size_t maximumStereoPacketBytes = 1275;
    Bytes result(maximumStereoPacketBytes);
    const auto bytes = opus_encode(encoder, samples.data(), voice::frameSamples, result.data(), result.size());
    opus_encoder_destroy(encoder);
    require(bytes > 0, "Stereo audio encodes for the private stream peer");
    result.resize(bytes);
    return result;
  }
  std::shared_ptr<voice::VideoFrame> image() {
    auto frame = std::make_shared<voice::VideoFrame>();
    frame->width = 320;
    frame->height = 180;
    frame->stride = frame->width * 4;
    frame->timestamp =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    frame->rgba.resize(frame->stride * frame->height);
    std::uint32_t noise = 7;
    for (std::size_t i = 0; i < frame->rgba.size(); i += 4) {
      noise = noise * 1664525 + 1013904223;
      frame->rgba[i] = noise >> 24;
      frame->rgba[i + 1] = noise >> 16;
      frame->rgba[i + 2] = noise >> 8;
      frame->rgba[i + 3] = 255;
    }
    return frame;
  }
} // namespace
int main(int argc, char** argv) {
  using namespace voice_test;
  using namespace voice;
  using namespace std::chrono_literals;
  try {
    require(argc == 4, "A private stream endpoint, direction and codec are supplied");
    require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "Private media transport initializes");
    const bool publishing = std::string_view(argv[2]) == "publish";
    MediaCodec codec = MediaCodec::VP8;
    for (const auto candidate : {MediaCodec::H264, MediaCodec::VP8, MediaCodec::VP9})
      if (codecName(candidate) == argv[3])
        codec = candidate;
    auto audio = std::make_shared<InjectedAudio>();
    auto video = std::make_shared<VideoPipeline>();
    video->configure({.resolution = 480, .hardware = false});
    auto source = image();
    if (publishing)
      video->source(source);
    std::mutex mutex;
    std::condition_variable ready;
    State state;
    {
      Connection connection(
          audio,
          [&](State next) {
            std::lock_guard lock(mutex);
            state = std::move(next);
            ready.notify_all();
          },
          argv[1], video
      );
      require(
          connection.connect(
              {.server = "1",
               .user = "2",
               .session = "synthetic-session",
               .token = "synthetic-token",
               .endpoint = "voice-test.discord.media",
               .channel = "3",
               .generation = 11,
               .revision = 7,
               .participants = {"4"},
               .stream = true,
               .publishing = publishing,
               .publisher = publishing ? "2" : "4"}
          ),
          "The private stream begins explicitly"
      );
      Server server;
      Encryption remote("4", Server::room);
      remote.addUser("2");
      require(
          remote.initialize(1) && remote.handleBinary(25, server.sender()),
          "The stream peer authenticates its group authority"
      );
      message(remote, 26);
      std::cout << Json{{"external", hex(server.sender())}}.dump() << std::endl;
      auto setup = command();
      require(
          remote.handleBinary(27, server.propose(unhex(setup.at("package").get<std::string>()))),
          "The peer accepts the actual stream key package"
      );
      auto [commit, welcome] = Server::split(message(remote, 28));
      require(
          remote.handleBinary(29, transition(std::move(commit), 0)), "The peer activates the private encrypted stream"
      );
      protocol::RtpCipher cipher;
      const std::array<std::uint8_t, 3> types{
          120, setup.at("pt").get<std::uint8_t>(), setup.at("rtx").get<std::uint8_t>()
      };
      cipher.payloadTypes(types);
      std::array<std::uint8_t, 32> key{};
      for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = i;
      require(cipher.setKey(key), "The private relay uses the negotiated transport key");
      const auto opus = opusFrame();
      VideoRoute route{codec, types[1], types[2], publishing ? 456U : 124U, publishing ? 457U : 125U};
      std::cout << Json{{"welcome", hex(transition(std::move(welcome), 0))}}.dump() << std::endl;
      {
        std::unique_lock lock(mutex);
        require(
            ready.wait_for(lock, 5s, [&] { return state.phase == Phase::Connected || state.phase == Phase::Failed; }),
            "The private stream completes group setup"
        );
        require(
            state.phase == Phase::Connected && state.encrypted, "Actual stream transport reaches encrypted readiness"
        );
      }
      require(
          audio->received == 0 && (publishing || video->statistics().decoded == 0),
          "No pre-key audio or received video reaches playback"
      );
      if (publishing) {
        audio->emit(opus);
        std::cout << Json{{"emitted", true}}.dump() << std::endl;
        const auto media = command();
        VideoAssembler assembler(route);
        bool decoded = false, heard = false;
        VideoDecoder decoder;
        for (const auto& value : media.at("packets")) {
          const auto packet = cipher.open(unhex(value.get<std::string>()));
          if (!packet)
            continue;
          if (packet->payloadType == 120)
            heard = remote.decrypt("2", packet->payload) == opus;
          else if (const auto assembled = assembler.receive(*packet, std::chrono::steady_clock::now())) {
            auto encoded = *assembled;
            encoded.bytes = remote.decrypt("2", encoded.bytes, codec);
            std::string error;
            const auto output = decoder.decode(encoded, error, false);
            decoded = output && output->valid() && output->width == source->width && output->height == source->height;
          }
        }
        require(decoded && heard, "The actual publisher sends decodable authenticated video and exact stereo audio");
        const auto verifySize = [&](const Json& media, int width, int height) {
          bool matching = false;
          for (const auto& value : media.at("packets")) {
            const auto packet = cipher.open(unhex(value.get<std::string>()));
            if (!packet || packet->payloadType != route.payloadType)
              continue;
            if (const auto assembled = assembler.receive(*packet, std::chrono::steady_clock::now())) {
              auto encoded = *assembled;
              encoded.bytes = remote.decrypt("2", encoded.bytes, codec);
              std::string error;
              const auto output = decoder.decode(encoded, error, false);
              matching |= output && output->width == width && output->height == height;
            }
          }
          require(matching, "Negotiated receiver constraints produce the actual expected encoded dimensions");
        };
        std::cout << Json{{"constrain", true}}.dump() << std::endl;
        verifySize(command(), 160, 90);
        std::cout << Json{{"release", true}}.dump() << std::endl;
        verifySize(command(), source->width, source->height);
        std::cout << Json{{"no_sink", true}}.dump() << std::endl;
        require(command().value("suspended", false), "The private relay applies a zero sink request");
        const auto suspended = video->statistics().encoded;
        video->source(image());
        std::cout << Json{{"suspended_source", true}}.dump() << std::endl;
        require(
            command().value("quiet", false) && video->statistics().encoded == suspended,
            "A zero sink request suspends real encoding while the source preview remains available"
        );
        std::cout << Json{{"restore_sink", true}}.dump() << std::endl;
        verifySize(command(), source->width, source->height);
        connection.pauseVideo(true);
        std::cout << Json{{"pause", true}}.dump() << std::endl;
        require(command().value("paused", false), "The relay observes the inactive stream announcement");
        const auto count = video->statistics().encoded;
        source = image();
        video->source(source);
        audio->emit(opus);
        std::cout << Json{{"paused_source", true}}.dump() << std::endl;
        require(
            command().value("quiet", false) && video->statistics().encoded == count,
            "Paused streams send neither screen frames nor shared audio"
        );
        connection.pauseVideo(false);
        std::cout << Json{{"resume", true}}.dump() << std::endl;
        require(command().value("resumed", false), "The relay observes the active stream announcement");
        std::cout << Json{{"emitted", true}}.dump() << std::endl;
        const auto resumed = command();
        bool recovered = false;
        for (const auto& value : resumed.at("packets"))
          if (auto packet = cipher.open(unhex(value.get<std::string>())))
            if (auto frame = assembler.receive(std::move(*packet), std::chrono::steady_clock::now()))
              recovered = !remote.decrypt("2", frame->bytes, codec).empty();
        require(recovered, "Resuming publishes fresh media with continuous transport and frame nonces");
      } else {
        VideoEncoder encoder;
        std::string error;
        auto encoded =
            encoder.encode(*source, codec, {180, 30, 3000000}, false, true, ContentPreference::Clarity, error);
        require(encoded.size() == 1, "The stream fixture creates a real codec frame");
        auto frame = encoded.front();
        auto protectedFrame = remote.encrypt(route.ssrc, frame.bytes, codec);
        std::uint16_t sequence = 65530;
        const auto packets = packetizeVideo(protectedFrame, frame.timestamp, route, sequence);
        require(packets.size() > 3, "The native watcher exercises fragmented frames");
        Json output = Json::array();
        VideoRetransmit retransmit;
        for (const auto& packet : packets) {
          retransmit.sent(packet, std::chrono::steady_clock::now());
          output.push_back(hex(cipher.seal(packet)));
        }
        audio->emit(opus);
        std::cout
            << Json{{"video", output}, {"audio", hex(cipher.seal({126, frameSamples * 2, 1, remote.encrypt(126, opus)}))}}
                   .dump()
            << std::endl;
        const auto lost = command();
        Json repair = Json::array();
        for (const auto& datagram : lost.at("feedback")) {
          const auto control = cipher.openControl(unhex(datagram.get<std::string>()));
          if (auto feedback = videoFeedback(control, route.ssrc))
            for (const auto& packet : retransmit.request(feedback->lost, route, std::chrono::steady_clock::now()))
              repair.push_back(hex(cipher.seal(packet)));
        }
        require(!repair.empty(), "The actual watcher requests missing RTP and the peer supplies negotiated RTX");
        std::cout << Json{{"repair", repair}}.dump() << std::endl;
        require(command().value("delivered", false), "The private relay supplies authenticated repaired media");
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while ((!video->frame() || audio->received == 0) && std::chrono::steady_clock::now() < deadline) {
          std::unique_lock lock(mutex);
          ready.wait_for(lock, 5ms);
        }
        require(
            video->frame() && video->frame()->valid() && video->frame()->width == source->width,
            "The actual watcher paints a decoded repaired stream frame"
        );
        {
          std::lock_guard lock(audio->mutex);
          require(audio->played == opus, "The watcher receives exact authenticated stereo audio");
        }
        AudioSettings settings;
        settings.deafened = true;
        connection.configure(settings);
        {
          std::unique_lock lock(mutex);
          require(ready.wait_for(lock, 2s, [&] { return state.deafened; }), "Deafen reaches the stream connection");
        }
        const auto decoded = video->statistics().decoded;
        const auto received = audio->received.load();
        source = image();
        encoded = encoder.encode(*source, codec, {180, 30, 3000000}, false, true, ContentPreference::Clarity, error);
        require(encoded.size() == 1, "A second real frame is encoded while the watcher is deafened");
        output = Json::array();
        for (const auto& packet : packetizeVideo(
                 remote.encrypt(route.ssrc, encoded.front().bytes, codec), encoded.front().timestamp, route, sequence
             ))
          output.push_back(hex(cipher.seal(packet)));
        std::cout
            << Json{{"deaf_video", output}, {"audio", hex(cipher.seal({126, frameSamples * 3, 2, remote.encrypt(126, opus)}))}}
                   .dump()
            << std::endl;
        require(command().value("delivered", false), "The relay supplies video and audio while deafened");
        const auto end = std::chrono::steady_clock::now() + 3s;
        while (video->statistics().decoded == decoded && std::chrono::steady_clock::now() < end) {
          std::unique_lock lock(mutex);
          ready.wait_for(lock, 5ms);
        }
        require(
            video->statistics().decoded > decoded && audio->received == received,
            "Deafening silences stream audio while video continues"
        );
      }
      connection.disconnect();
      {
        std::unique_lock lock(mutex);
        require(
            ready.wait_for(lock, 2s, [&] { return state.phase == Phase::Disconnected; }),
            "Stopping a stream retires the native transport promptly"
        );
      }
    }
    curl_global_cleanup();
    std::cout << Json{{"passed", true}}.dump() << std::endl;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
