#pragma once
#include "voice/encryption.h"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <mls/messages.h>
#include <stdexcept>
namespace voice_test {
  using Bytes = std::vector<std::uint8_t>;
  using Json = nlohmann::json;
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
} // namespace voice_test
