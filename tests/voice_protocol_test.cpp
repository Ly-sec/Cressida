#include "voice/protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <stdexcept>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  using namespace voice::protocol;
  const std::array<std::uint8_t, keyBytes> key = [] {
    std::array<std::uint8_t, keyBytes> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
      result[i] = static_cast<std::uint8_t>(i);
    return result;
  }();
  std::vector<std::uint8_t>
  protectedPacket(std::vector<std::uint8_t> header, std::span<const std::uint8_t> plaintext, std::uint32_t counter) {
    std::array<std::uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    write32(nonce, 0, counter);
    const auto headerSize = header.size();
    header.resize(headerSize + plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES + nonceSuffixBytes);
    unsigned long long written = 0;
    require(
        crypto_aead_xchacha20poly1305_ietf_encrypt(
            header.data() + headerSize, &written, plaintext.data(), plaintext.size(), header.data(), headerSize,
            nullptr, nonce.data(), key.data()
        ) == 0,
        "Independent test encryption succeeds"
    );
    std::ranges::copy(std::span{nonce}.first(nonceSuffixBytes), header.end() - nonceSuffixBytes);
    return header;
  }
} // namespace
int main() {
  using namespace std::chrono_literals;
  using Json = nlohmann::json;
  try {
    require(validId("18446744073709551615") && !validId("18446744073709551616"), "Identities reject overflow");
    for (const auto* bad : {"", "0", "01", "-1", "1/2", " 1", "1\n"})
      require(!validId(bad), "Malformed identities are rejected");
    require(
        endpoint("voice-test.discord.media:443") == "wss://voice-test.discord.media:443/?v=8"
            && endpoint("wss://voice-test.discord.gg/") == "wss://voice-test.discord.gg/?v=8",
        "Voice endpoints use the fixed TLS protocol version"
    );
    for (const auto* bad :
         {"", "ws://voice.discord.media", "voice.discord.media.evil.test", "voice.discord.media@evil.test",
          "voice.discord.media/path", "voice.discord.media/?token=secret", "127.0.0.1", "voice.discord.media:0",
          "voice.discord.media:65536", "voice.discord.media:-1", "voice.discord.media:443/../",
          "voice.discord.media\n"})
      require(!endpoint(bad), "Untrusted endpoints cannot redirect voice credentials");
    Json wire{{"ssrc", 123}, {"ip", "127.0.0.1"}, {"port", 54321}, {"modes", Json::array({transportMode})}};
    const auto remote = ready(wire);
    require(remote && remote->ssrc == 123 && remote->port == 54321, "Ready projects bounded numeric UDP routing");
    for (const auto& patch : std::vector<Json>{
             {{"ssrc", -1}},
             {{"ssrc", UINT64_MAX}},
             {{"port", 0}},
             {{"port", 65536}},
             {{"ip", "example.com"}},
             {{"ip", "127.0.0.1\n"}},
             {{"modes", Json::array({"xsalsa20_poly1305"})}},
             {{"modes", nullptr}}
         }) {
      auto malformed = wire;
      malformed.update(patch);
      require(!ready(malformed), "Malformed or deprecated voice transport negotiation fails");
    }
    Json description{{"mode", transportMode}, {"secret_key", key}, {"dave_protocol_version", 1}};
    const auto selected = session(description, 1);
    require(
        selected && selected->version == 1 && selected->key == key, "Session descriptions preserve exact transport keys"
    );
    for (const auto& patch : std::vector<Json>{
             {{"mode", "xsalsa20_poly1305"}},
             {{"dave_protocol_version", 0}},
             {{"dave_protocol_version", 2}},
             {{"dave_protocol_version", -1}},
             {{"secret_key", Json::array({1, 2, 3})}}
         }) {
      auto malformed = description;
      malformed.update(patch);
      require(!session(malformed, 1), "Transport-only sessions and unsupported versions fail closed");
    }
    description["secret_key"][0] = 256;
    require(!session(description, 1), "Secret key octets cannot truncate overflowing values");

    auto discovery = discoveryRequest(123);
    require(
        discovery.size() == discoveryBytes
            && read16(discovery, 0) == 1
            && read16(discovery, 2) == 70
            && read32(discovery, 4) == 123,
        "Discovery requests use the documented network-order structure"
    );
    write16(discovery, 0, 2);
    const std::string external = "203.0.113.42";
    std::ranges::copy(external, discovery.begin() + 8);
    write16(discovery, discoveryBytes - 2, 45678);
    const auto discovered = discoveryResponse(discovery, 123);
    require(
        discovered && discovered->ip == external && discovered->port == 45678,
        "Discovery response identity and address are validated"
    );
    require(
        !discoveryResponse(discovery, 124) && !discoveryResponse(std::span{discovery}.first(discovery.size() - 1), 123),
        "Stale and truncated discovery packets are rejected"
    );
    std::fill(discovery.begin() + 8, discovery.end() - 2, '1');
    require(!discoveryResponse(discovery, 123), "Unterminated discovered addresses cannot read past the packet");

    Heartbeat heartbeat;
    const auto now = Heartbeat::Clock::time_point{};
    require(!heartbeat.initialized() && !heartbeat.due(now), "Heartbeats wait for a validated hello");
    require(
        heartbeat.hello({{"heartbeat_interval", 1000}}, now) && heartbeat.due(now),
        "Hello schedules the first heartbeat immediately"
    );
    const auto nonce = heartbeat.sent(now);
    require(
        !heartbeat.due(now + 999ms) && heartbeat.missed(now + 1s), "Missing ACKs fail at the next negotiated heartbeat"
    );
    heartbeat.acknowledge({{"t", nonce + 1}});
    require(heartbeat.missed(now + 1s), "Unrelated heartbeat ACKs cannot extend connection lifetime");
    heartbeat.acknowledge({{"t", nonce}});
    require(!heartbeat.missed(now + 1s), "Only the matching heartbeat ACK clears the deadline");
    require(
        !heartbeat.hello({{"heartbeat_interval", 1000}}, now), "Repeated hello messages cannot reset heartbeat state"
    );
    for (const auto& invalid : std::vector<Json>{
             Json{},
             {{"heartbeat_interval", 0}},
             {{"heartbeat_interval", -1}},
             {{"heartbeat_interval", "1000"}},
             {{"heartbeat_interval", 120001}}
         }) {
      Heartbeat other;
      require(!other.hello(invalid, now), "Invalid heartbeat intervals are rejected");
    }

    RtpCipher sender, receiver;
    require(sender.setKey(key) && receiver.setKey(key), "The exact negotiated key initializes transport encryption");
    Packet original{123, 960, 65535, {0x11, 0x22, 0x33, 0x44}};
    const auto encrypted = sender.seal(original);
    require(
        encrypted.size()
                == rtpHeaderBytes
                    + original.payload.size()
                    + crypto_aead_xchacha20poly1305_ietf_ABYTES
                    + nonceSuffixBytes
            && encrypted[0] == 0x80
            && encrypted[1] == 120,
        "RTP headers and AEAD overhead match the negotiated transport"
    );
    auto corrupted = encrypted;
    corrupted[8] ^= 1;
    require(!receiver.open(corrupted), "SSRC header tampering fails authentication");
    corrupted = encrypted;
    corrupted[rtpHeaderBytes] ^= 1;
    require(!receiver.open(corrupted), "Audio tampering fails authentication without consuming the nonce");
    const auto decoded = receiver.open(encrypted);
    require(
        decoded
            && decoded->ssrc == original.ssrc
            && decoded->timestamp == original.timestamp
            && decoded->sequence == original.sequence
            && decoded->payload == original.payload,
        "Transport encryption round-trips every RTP field and audio byte"
    );
    require(!receiver.open(encrypted), "Replayed authenticated packets are rejected");
    for (std::size_t length = 0; length < encrypted.size(); ++length)
      require(!receiver.open(std::span{encrypted}.first(length)), "Every truncated RTP packet is safely rejected");
    std::vector<std::vector<std::uint8_t>> reordered;
    for (unsigned i = 0; i < 3; ++i) {
      original.sequence = static_cast<std::uint16_t>(i);
      reordered.push_back(sender.seal(original));
    }
    require(
        receiver.open(reordered[2]) && receiver.open(reordered[0]) && receiver.open(reordered[1]),
        "Bounded UDP reordering is accepted even across RTP sequence wrap"
    );
    for (unsigned i = 0; i < 65; ++i)
      require(receiver.open(sender.seal(original)).has_value(), "Fresh encrypted packets advance the replay window");
    require(!receiver.open(reordered[0]), "Old packets outside the replay window remain rejected");
    std::vector<std::uint8_t> header(20);
    header[0] = 0x91;
    header[1] = 120;
    write16(header, 2, 5);
    write32(header, 4, 1920);
    write32(header, 8, 124);
    write32(header, 12, 125);
    write16(header, 16, 0xbede);
    write16(header, 18, 1);
    const std::array<std::uint8_t, 7> extensionAudio{0x10, 0x40, 0, 0, 0x11, 0x22, 0x33};
    const auto extended = receiver.open(protectedPacket(header, extensionAudio, 500));
    require(
        extended && extended->payload == std::vector<std::uint8_t>({0x11, 0x22, 0x33}),
        "CSRC and extension preambles authenticate separately from encrypted extension contents"
    );
    write16(header, 18, 0xffff);
    require(
        !receiver.open(protectedPacket(header, extensionAudio, 501)),
        "Authenticated oversized extension lengths cannot underflow audio spans"
    );
    header.resize(rtpHeaderBytes);
    header[0] = 0xa0;
    const std::array<std::uint8_t, 6> paddedAudio{0x11, 0x22, 0x33, 0, 0, 3};
    const auto padded = receiver.open(protectedPacket(header, paddedAudio, 502));
    require(
        padded && padded->payload == std::vector<std::uint8_t>({0x11, 0x22, 0x33}),
        "RTP padding is removed only after authentication and bounds checks"
    );
    receiver.clear();
    require(
        !receiver.open(encrypted) && receiver.seal(original).empty(),
        "Clearing a session destroys transport key usability"
    );
    std::cout << "Voice routes, negotiation, heartbeats, discovery and authenticated RTP passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
