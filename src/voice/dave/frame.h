#pragma once

#include "voice/dave/constants.h"
#include "voice/media.h"

#include <array>
#include <bitset>
#include <chrono>
#include <functional>
#include <map>
#include <mls/key_schedule.h>
#include <optional>
#include <span>
#include <vector>

namespace voice::dave {
  using Bytes = std::vector<std::uint8_t>;
  using Secret = cressida_mls::bytes_ns::bytes;
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;

  // The adapter authenticates into a private buffer and releases it only on success.
  using Ranges = std::vector<std::pair<std::size_t, std::size_t>>;
  Bytes seal(
      std::span<const std::uint8_t> plaintext, const Secret& key, std::uint32_t nonce, const Ranges& ranges = {},
      std::size_t maximum = maximumMediaBytes
  );
  struct PreparedMedia {
    Bytes bytes;
    Ranges ranges;
  };
  std::optional<PreparedMedia> prepareMedia(std::span<const std::uint8_t> bytes, MediaCodec codec);
  struct Frame {
    std::uint32_t nonce = 0;
    Bytes ciphertext, authenticated;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    std::array<std::uint8_t, tagBytes> tag{};
  };
  std::optional<Frame> parse(std::span<const std::uint8_t> frame, std::size_t maximum = maximumMediaBytes);
  Bytes open(const Frame& frame, const Secret& key, std::size_t maximum = maximumMediaBytes);

  class Sender {
  public:
    bool install(const Secret& secret);
    void clear();
    bool ready() const { return m_ratchet.has_value(); }
    Bytes encrypt(std::span<const std::uint8_t> media, MediaCodec codec = MediaCodec::Opus);

  private:
    Secret m_domain;
    std::optional<cressida_mls::HashRatchet> m_ratchet;
    std::uint64_t m_counter = 0;
  };

  class Receiver {
  public:
    explicit Receiver(Now now = [] { return Clock::now(); }) : m_now(std::move(now)) {}
    bool install(const Secret& secret);
    Bytes decrypt(std::span<const std::uint8_t> frame, MediaCodec codec = MediaCodec::Opus);
    void expire();

  private:
    struct Domain {
      Secret identity;
      cressida_mls::HashRatchet ratchet;
      Clock::time_point created, expires = Clock::time_point::max();
      std::optional<std::uint64_t> latest;
      std::bitset<replayWindow> seen;
      std::map<std::uint32_t, Clock::time_point> keyExpiry;
    };
    Now m_now;
    std::vector<Domain> m_domains;
  };
} // namespace voice::dave
