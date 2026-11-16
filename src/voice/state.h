#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace voice {
  enum class Phase { Disconnected, Connecting, Securing, WaitingForParticipants, Connected, Failed };
  struct Participant {
    std::string user;
    bool speaking = false;
  };
  struct State {
    Phase phase = Phase::Disconnected;
    std::uint64_t generation = 0, revision = 0;
    std::string channel, error;
    bool muted = false, deafened = false, encrypted = false;
    std::vector<Participant> participants;
  };
  struct ConnectionInfo {
    std::string server, user, session, token, endpoint, channel;
    // MLS group for end-to-end encryption. Empty means the channel, which is correct for calls and voice channels.
    std::string group;
    std::uint64_t generation = 0, revision = 0;
    std::vector<std::string> participants;
    bool stream = false, publishing = false;
    std::string publisher;
    std::shared_ptr<std::atomic_bool> revoked;
  };
} // namespace voice
