#pragma once

#include "voice/audio.h"

#include <cstdint>
#include <functional>
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
    std::uint64_t generation = 0, revision = 0;
    std::vector<std::string> participants;
  };
  // The callback runs on a worker and must be queued onto the application's loop.
  class Connection {
  public:
    using Changed = std::function<void(State)>;
    explicit Connection(std::shared_ptr<Audio> audio, Changed changed, std::string loopbackEndpoint = {});
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    bool connect(ConnectionInfo info);
    void disconnect();
    void configure(AudioSettings settings);
    void participants(std::vector<std::string> users);

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
  };
} // namespace voice
