#include "voice/protocol.h"

#include <algorithm>
#include <arpa/inet.h>
#include <charconv>
#include <cmath>
#include <limits>
#include <sodium.h>
#include <stdexcept>

namespace voice::protocol {
  namespace {
    constexpr std::size_t authenticationBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;
    constexpr std::size_t maximumEndpointBytes = 256;
    constexpr std::size_t maximumIdBytes = 20;
    constexpr std::size_t rtpCsrcBytes = 4;
    constexpr std::size_t rtpExtensionPreambleBytes = 4;
    constexpr std::size_t replayBits = 64;
    bool ipAddress(const std::string& address) {
      in_addr v4{};
      in6_addr v6{};
      return inet_pton(AF_INET, address.c_str(), &v4) == 1 || inet_pton(AF_INET6, address.c_str(), &v6) == 1;
    }
    std::optional<std::uint64_t> integer(const nlohmann::json& value, std::uint64_t maximum) {
      if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        return {};
      const auto result = value.get<std::uint64_t>();
      return result <= maximum ? std::optional{result} : std::nullopt;
    }
  } // namespace
  bool validId(std::string_view value) {
    if (value.empty() || value.size() > maximumIdBytes || value.front() == '0')
      return false;
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result != 0;
  }
  std::optional<std::string> endpoint(std::string_view value) {
    if (value.empty() || value.size() > maximumEndpointBytes)
      return {};
    if (value.starts_with("wss://"))
      value.remove_prefix(std::string_view{"wss://"}.size());
    if (value.ends_with('/'))
      value.remove_suffix(1);
    const auto port = value.find(':');
    const auto host = value.substr(0, port);
    if (!(host.ends_with(".discord.media") || host.ends_with(".discord.gg"))
        || host.front() == '.'
        || host.front() == '-'
        || !std::ranges::all_of(host, [](char c) {
             return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
           }))
      return {};
    if (port != std::string_view::npos) {
      const auto number = value.substr(port + 1);
      unsigned result = 0;
      const auto parsed = std::from_chars(number.data(), number.data() + number.size(), result);
      if (number.empty()
          || parsed.ec != std::errc{}
          || parsed.ptr != number.data() + number.size()
          || !result
          || result > std::numeric_limits<std::uint16_t>::max())
        return {};
    }
    return "wss://" + std::string(value) + "/?v=8";
  }
  std::optional<Ready> ready(const nlohmann::json& value) {
    if (!value.is_object()
        || !value.contains("ssrc")
        || !value.contains("port")
        || !value.contains("ip")
        || !value["ip"].is_string()
        || !value.contains("modes")
        || !value["modes"].is_array())
      return {};
    const auto ssrc = integer(value["ssrc"], std::numeric_limits<std::uint32_t>::max());
    const auto port = integer(value["port"], std::numeric_limits<std::uint16_t>::max());
    const auto ip = value["ip"].get<std::string>();
    if (!ssrc
        || !*ssrc
        || !port
        || !*port
        || ip.size() > INET6_ADDRSTRLEN
        || !ipAddress(ip)
        || !std::ranges::any_of(value["modes"], [](const auto& mode) {
             return mode.is_string() && mode.template get<std::string_view>() == transportMode;
           }))
      return {};
    return Ready{static_cast<std::uint32_t>(*ssrc), ip, static_cast<std::uint16_t>(*port)};
  }
  std::optional<Session> session(const nlohmann::json& value, std::uint16_t maximumVersion) {
    if (!value.is_object()
        || !value.contains("mode")
        || value["mode"] != transportMode
        || !value.contains("secret_key")
        || !value["secret_key"].is_array()
        || value["secret_key"].size() != keyBytes
        || !value.contains("dave_protocol_version"))
      return {};
    const auto version = integer(value["dave_protocol_version"], maximumVersion);
    if (!version || *version == 0)
      return {};
    Session result;
    result.version = static_cast<std::uint16_t>(*version);
    for (std::size_t at = 0; at < keyBytes; ++at) {
      const auto byte = integer(value["secret_key"][at], std::numeric_limits<std::uint8_t>::max());
      if (!byte) {
        sodium_memzero(result.key.data(), result.key.size());
        return {};
      }
      result.key[at] = static_cast<std::uint8_t>(*byte);
    }
    return result;
  }
  std::uint16_t read16(std::span<const std::uint8_t> value, std::size_t offset) {
    return (static_cast<std::uint16_t>(value[offset]) << 8) | value[offset + 1];
  }
  std::uint32_t read32(std::span<const std::uint8_t> value, std::size_t offset) {
    return (static_cast<std::uint32_t>(read16(value, offset)) << 16) | read16(value, offset + 2);
  }
  void write16(std::span<std::uint8_t> value, std::size_t offset, std::uint16_t number) {
    value[offset] = static_cast<std::uint8_t>(number >> 8);
    value[offset + 1] = static_cast<std::uint8_t>(number);
  }
  void write32(std::span<std::uint8_t> value, std::size_t offset, std::uint32_t number) {
    write16(value, offset, static_cast<std::uint16_t>(number >> 16));
    write16(value, offset + 2, static_cast<std::uint16_t>(number));
  }
  std::array<std::uint8_t, discoveryBytes> discoveryRequest(std::uint32_t ssrc) {
    std::array<std::uint8_t, discoveryBytes> result{};
    write16(result, 0, 1);
    write16(result, 2, discoveryBytes - nonceSuffixBytes);
    write32(result, 4, ssrc);
    return result;
  }
  std::optional<Address> discoveryResponse(std::span<const std::uint8_t> value, std::uint32_t ssrc) {
    constexpr std::size_t addressOffset = 8, addressBytes = 64;
    if (value.size() != discoveryBytes
        || read16(value, 0) != 2
        || read16(value, 2) != discoveryBytes - nonceSuffixBytes
        || read32(value, 4) != ssrc)
      return {};
    const auto address = value.subspan(addressOffset, addressBytes);
    const auto end = std::ranges::find(address, std::uint8_t{});
    if (end == address.end() || end == address.begin())
      return {};
    Address result{std::string(address.begin(), end), read16(value, discoveryBytes - sizeof(std::uint16_t))};
    return result.port && ipAddress(result.ip) ? std::optional{std::move(result)} : std::nullopt;
  }
  bool Heartbeat::hello(const nlohmann::json& value, Clock::time_point now) {
    constexpr auto maximumInterval = std::chrono::minutes{2};
    if (m_initialized
        || !value.is_object()
        || !value.contains("heartbeat_interval")
        || !value["heartbeat_interval"].is_number())
      return false;
    const auto interval = value["heartbeat_interval"].get<double>();
    if (!std::isfinite(interval)
        || interval < 1
        || interval > std::chrono::duration_cast<std::chrono::milliseconds>(maximumInterval).count())
      return false;
    m_interval = std::chrono::milliseconds{static_cast<std::int64_t>(interval)};
    m_next = now;
    m_initialized = true;
    return true;
  }
  bool Heartbeat::due(Clock::time_point now) const { return m_initialized && now >= m_next; }
  bool Heartbeat::missed(Clock::time_point now) const { return due(now) && m_pending; }
  std::uint64_t Heartbeat::sent(Clock::time_point now) {
    m_next = now + m_interval;
    m_pending = true;
    return ++m_nonce;
  }
  void Heartbeat::acknowledge(const nlohmann::json& value) {
    if (value.is_object()
        && value.contains("t")
        && integer(value["t"], std::numeric_limits<std::uint64_t>::max()) == m_nonce)
      m_pending = false;
  }
  RtpCipher::RtpCipher() {
    if (sodium_init() < 0)
      throw std::runtime_error("Voice transport cryptography is unavailable");
    m_payloadTypes[120] = true;
  }
  RtpCipher::~RtpCipher() { clear(); }
  void RtpCipher::clear() {
    sodium_memzero(m_key.data(), m_key.size());
    m_initialized = false;
    m_counter = 0;
    m_replay.clear();
  }
  bool RtpCipher::setKey(std::span<const std::uint8_t> key) {
    if (m_initialized && key.size() == m_key.size() && sodium_memcmp(key.data(), m_key.data(), m_key.size()) == 0)
      return true;
    clear();
    if (key.size() != m_key.size())
      return false;
    std::ranges::copy(key, m_key.begin());
    m_initialized = true;
    return true;
  }
  void RtpCipher::payloadTypes(std::span<const std::uint8_t> types) {
    if (m_initialized)
      return;
    m_payloadTypes.fill(false);
    for (auto type : types)
      if (type < m_payloadTypes.size() && (type < 64 || type > 95))
        m_payloadTypes[type] = true;
    m_replayBits =
        std::ranges::any_of(types, [](auto type) { return type != 120; }) ? maximumMediaReplayPackets : replayBits;
  }
  bool RtpCipher::replayAllowed(std::uint32_t ssrc, std::uint32_t counter) const {
    if (!ssrc)
      return false;
    const auto prior = m_replay.find(ssrc);
    if (prior == m_replay.end())
      return m_replay.size() < maximumParticipants;
    if (counter > prior->second.latest)
      return true;
    const auto age = prior->second.latest - counter;
    return age < m_replayBits && !prior->second.seen.test(age);
  }
  void RtpCipher::accepted(std::uint32_t ssrc, std::uint32_t counter) {
    auto& replay = m_replay[ssrc];
    if (counter > replay.latest) {
      const auto gap = counter - replay.latest;
      if (gap >= m_replayBits)
        replay.seen.reset();
      else
        replay.seen <<= gap;
      replay.seen.set(0);
      replay.latest = counter;
    } else
      replay.seen.set(replay.latest - counter);
  }
  std::vector<std::uint8_t> RtpCipher::seal(const Packet& packet) {
    if (!m_initialized
        || !packet.ssrc
        || packet.payloadType >= m_payloadTypes.size()
        || !m_payloadTypes[packet.payloadType]
        || packet.payload.empty()
        || packet.payload.size() > maximumDatagramBytes - rtpHeaderBytes - authenticationBytes - nonceSuffixBytes
        || m_counter > std::numeric_limits<std::uint32_t>::max())
      return {};
    std::vector<std::uint8_t> result(rtpHeaderBytes + packet.payload.size() + authenticationBytes + nonceSuffixBytes);
    result[0] = 0x80;
    result[1] = packet.payloadType | (packet.marker ? 0x80 : 0);
    write16(result, 2, packet.sequence);
    write32(result, 4, packet.timestamp);
    write32(result, 8, packet.ssrc);
    std::array<std::uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    write32(nonce, 0, static_cast<std::uint32_t>(m_counter++));
    unsigned long long written = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            result.data() + rtpHeaderBytes, &written, packet.payload.data(), packet.payload.size(), result.data(),
            rtpHeaderBytes, nullptr, nonce.data(), m_key.data()
        )
        != 0)
      return {};
    std::ranges::copy(std::span{nonce}.first(nonceSuffixBytes), result.end() - nonceSuffixBytes);
    return result;
  }
  std::optional<Packet> RtpCipher::open(std::span<const std::uint8_t> packet) {
    if (!m_initialized
        || packet.size() < rtpHeaderBytes + authenticationBytes + nonceSuffixBytes
        || packet.size() > maximumDatagramBytes
        || (packet[0] >> 6) != 2
        || !m_payloadTypes[packet[1] & 0x7f])
      return {};
    std::size_t authenticated = rtpHeaderBytes + (packet[0] & 0x0f) * rtpCsrcBytes;
    const bool extension = (packet[0] & 0x10) != 0;
    if (extension)
      authenticated += rtpExtensionPreambleBytes;
    if (authenticated > packet.size() - authenticationBytes - nonceSuffixBytes)
      return {};
    const auto ssrc = read32(packet, 8), counter = read32(packet, packet.size() - nonceSuffixBytes);
    if (!replayAllowed(ssrc, counter))
      return {};
    std::array<std::uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    std::ranges::copy(packet.last(nonceSuffixBytes), nonce.begin());
    Packet result{ssrc, read32(packet, 4), read16(packet, 2), {}};
    result.payloadType = packet[1] & 0x7f;
    result.marker = (packet[1] & 0x80) != 0;
    result.payload.resize(packet.size() - authenticated - authenticationBytes - nonceSuffixBytes);
    unsigned long long written = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            result.payload.data(), &written, nullptr, packet.data() + authenticated,
            packet.size() - authenticated - nonceSuffixBytes, packet.data(), authenticated, nonce.data(), m_key.data()
        )
        != 0)
      return {};
    result.payload.resize(written);
    if (extension) {
      const std::size_t extensionBytes = read16(packet, authenticated - sizeof(std::uint16_t)) * rtpCsrcBytes;
      if (extensionBytes > result.payload.size())
        return {};
      result.payload.erase(result.payload.begin(), result.payload.begin() + extensionBytes);
    }
    if (packet[0] & 0x20) {
      if (result.payload.empty() || !result.payload.back() || result.payload.back() > result.payload.size())
        return {};
      result.payload.resize(result.payload.size() - result.payload.back());
    }
    if (result.payload.empty())
      return {};
    accepted(ssrc, counter);
    return result;
  }
  std::vector<std::uint8_t> RtpCipher::sealControl(std::span<const std::uint8_t> packet) {
    constexpr std::size_t header = 8;
    if (!m_initialized
        || packet.size() < header
        || packet.size() % 4
        || packet.size() > maximumDatagramBytes - authenticationBytes - nonceSuffixBytes
        || packet[0] >> 6 != 2
        || packet[1] < 200
        || packet[1] > 206
        || !read32(packet, 4)
        || m_counter > std::numeric_limits<std::uint32_t>::max())
      return {};
    std::vector<std::uint8_t> result(packet.begin(), packet.end());
    result.resize(packet.size() + authenticationBytes + nonceSuffixBytes);
    std::array<std::uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    write32(nonce, 0, static_cast<std::uint32_t>(m_counter++));
    unsigned long long written = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            result.data() + header, &written, packet.data() + header, packet.size() - header, result.data(), header,
            nullptr, nonce.data(), m_key.data()
        )
        != 0)
      return {};
    std::ranges::copy(std::span{nonce}.first(nonceSuffixBytes), result.end() - nonceSuffixBytes);
    return result;
  }
  std::vector<std::uint8_t> RtpCipher::openControl(std::span<const std::uint8_t> packet) {
    constexpr std::size_t header = 8;
    if (!m_initialized
        || packet.size() < header + authenticationBytes + nonceSuffixBytes
        || packet.size() > maximumDatagramBytes
        || packet[0] >> 6 != 2
        || packet[1] < 200
        || packet[1] > 206)
      return {};
    const auto ssrc = read32(packet, 4), counter = read32(packet, packet.size() - nonceSuffixBytes);
    if (!replayAllowed(ssrc, counter))
      return {};
    std::array<std::uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    std::ranges::copy(packet.last(nonceSuffixBytes), nonce.begin());
    std::vector<std::uint8_t> result(packet.begin(), packet.begin() + header);
    result.resize(packet.size() - authenticationBytes - nonceSuffixBytes);
    unsigned long long written = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            result.data() + header, &written, nullptr, packet.data() + header,
            packet.size() - header - nonceSuffixBytes, packet.data(), header, nonce.data(), m_key.data()
        )
        != 0)
      return {};
    if (header + written != result.size())
      return {};
    accepted(ssrc, counter);
    return result;
  }
} // namespace voice::protocol
