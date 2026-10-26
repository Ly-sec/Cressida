#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace voice::protocol {
  inline constexpr std::size_t maximumParticipants = 256;
  inline constexpr std::size_t maximumDatagramBytes = 8192;
  inline constexpr std::size_t maximumSignalBytes = 1024 * 1024;
  inline constexpr std::size_t keyBytes = 32;
  inline constexpr std::size_t nonceSuffixBytes = 4;
  inline constexpr std::size_t rtpHeaderBytes = 12;
  inline constexpr std::size_t discoveryBytes = 74;
  inline constexpr std::string_view transportMode = "aead_xchacha20_poly1305_rtpsize";
  inline constexpr std::array<std::uint8_t, 3> silence{0xf8, 0xff, 0xfe};
  bool validId(std::string_view value);
  std::optional<std::string> endpoint(std::string_view value);
  struct Ready {
    std::uint32_t ssrc = 0;
    std::string ip;
    std::uint16_t port = 0;
  };
  std::optional<Ready> ready(const nlohmann::json& value);
  struct Session {
    std::array<std::uint8_t, keyBytes> key{};
    std::uint16_t version = 0;
  };
  std::optional<Session> session(const nlohmann::json& value, std::uint16_t maximumVersion);
  std::array<std::uint8_t, discoveryBytes> discoveryRequest(std::uint32_t ssrc);
  struct Address {
    std::string ip;
    std::uint16_t port = 0;
  };
  std::optional<Address> discoveryResponse(std::span<const std::uint8_t> value, std::uint32_t ssrc);
  std::uint16_t read16(std::span<const std::uint8_t> value, std::size_t offset);
  std::uint32_t read32(std::span<const std::uint8_t> value, std::size_t offset);
  void write16(std::span<std::uint8_t> value, std::size_t offset, std::uint16_t number);
  void write32(std::span<std::uint8_t> value, std::size_t offset, std::uint32_t number);

  class Heartbeat {
  public:
    using Clock = std::chrono::steady_clock;
    bool hello(const nlohmann::json& value, Clock::time_point now);
    bool due(Clock::time_point now) const;
    bool missed(Clock::time_point now) const;
    std::uint64_t sent(Clock::time_point now);
    void acknowledge(const nlohmann::json& value);
    Clock::time_point deadline() const { return m_next; }
    bool initialized() const { return m_initialized; }

  private:
    std::chrono::milliseconds m_interval{};
    Clock::time_point m_next{};
    std::uint64_t m_nonce = 0;
    bool m_initialized = false, m_pending = false;
  };
  struct Packet {
    std::uint32_t ssrc = 0, timestamp = 0;
    std::uint16_t sequence = 0;
    std::vector<std::uint8_t> payload;
  };
  class RtpCipher {
  public:
    RtpCipher();
    ~RtpCipher();
    RtpCipher(const RtpCipher&) = delete;
    RtpCipher& operator=(const RtpCipher&) = delete;
    bool setKey(std::span<const std::uint8_t> key);
    void clear();
    std::vector<std::uint8_t> seal(const Packet& packet);
    std::optional<Packet> open(std::span<const std::uint8_t> packet);

  private:
    struct Replay {
      std::uint32_t latest = 0;
      std::uint64_t seen = 0;
    };
    std::array<std::uint8_t, keyBytes> m_key{};
    std::map<std::uint32_t, Replay> m_replay;
    std::uint64_t m_counter = 0;
    bool m_initialized = false;
  };
} // namespace voice::protocol
