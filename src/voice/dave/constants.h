#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace voice::dave {
  inline constexpr std::uint16_t protocolVersion = 1;
  inline constexpr std::size_t maximumParticipants = 256;
  inline constexpr std::size_t maximumHandshakeBytes = 1024 * 1024;
  inline constexpr std::size_t maximumMediaBytes = 4096;
  inline constexpr std::size_t maximumPendingMessages = 32;
  inline constexpr std::size_t maximumPendingTransitions = 8;
  inline constexpr std::size_t maximumProposals = 256;
  inline constexpr unsigned maximumRecoveryAttempts = 3;
  inline constexpr std::uint16_t initialTransition = 0;
  inline constexpr std::uint64_t firstEpoch = 1;
  inline constexpr std::size_t identityBytes = sizeof(std::uint64_t);
  inline constexpr unsigned byteBits = 8;
  inline constexpr std::size_t keyBytes = 16;
  inline constexpr std::size_t nonceBytes = 12;
  inline constexpr std::size_t tagBytes = 8;
  inline constexpr std::size_t suffixBytes = 3;
  inline constexpr std::uint8_t markerByte = 0xfa;
  inline constexpr unsigned generationShift = 24;
  inline constexpr std::uint64_t generationWrap = 256;
  inline constexpr std::uint64_t maximumGenerationAdvance = 250;
  inline constexpr std::uint64_t maximumFramesPerSecond = 170;
  inline constexpr std::size_t replayWindow = 1024;
  inline constexpr auto keyGrace = std::chrono::seconds(10);
  inline constexpr unsigned lebBits = 7;
  inline constexpr std::uint8_t lebContinuation = 0x80;
  inline constexpr std::uint8_t lebMask = 0x7f;
  enum Opcode : int {
    Prepare = 21,
    Execute = 22,
    Ready = 23,
    Epoch = 24,
    ExternalSender = 25,
    KeyPackage = 26,
    Proposals = 27,
    CommitWelcome = 28,
    Commit = 29,
    Welcome = 30,
    InvalidUpdate = 31,
  };
} // namespace voice::dave
