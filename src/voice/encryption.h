#pragma once

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace voice {
  struct EncryptionMessage {
    int opcode = 0;
    bool binary = false;
    std::vector<std::uint8_t> bytes;
    nlohmann::json data;
  };

  // The connection worker owns this session and serializes all its calls.
  class Encryption {
  public:
    static std::uint16_t maximumVersion();
    Encryption(std::string user, std::uint64_t channel);
    ~Encryption();
    Encryption(const Encryption&) = delete;
    Encryption& operator=(const Encryption&) = delete;
    bool initialize(std::uint16_t version);
    bool handleJson(int opcode, const nlohmann::json& data);
    bool handleBinary(int opcode, std::span<const std::uint8_t> payload);
    void addUser(std::string user);
    void removeUser(std::string_view user);
    std::vector<EncryptionMessage> takeMessages();
    std::vector<std::uint8_t> encrypt(std::uint32_t ssrc, std::span<const std::uint8_t> opus);
    std::vector<std::uint8_t> decrypt(std::string_view user, std::span<const std::uint8_t> frame);
    void tick();
    bool ready() const;
    std::string error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };
} // namespace voice
