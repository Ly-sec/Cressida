#pragma once

#include "app/voice_command.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace app {
  // Ordinary launches elect an owner; later launches send Open or explicit voice commands.
  class SingleInstance {
  public:
    enum class Launch { Owner, Forwarded };
    struct VoiceRequest {
      VoiceCommand command = VoiceCommand::InputMuteToggle;
      std::uint64_t id = 0;
    };
    struct VoiceResult {
      enum class Status { Applied, Rejected, NotRunning, TimedOut, TransportError };
      Status status = Status::TransportError;
      std::string message;
      bool applied() const { return status == Status::Applied; }
    };
    static constexpr auto voiceTimeout = std::chrono::seconds{2};
    explicit SingleInstance(std::filesystem::path runtime = {});
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    Launch claim(std::string_view activationToken = {});
    int eventFd() const { return m_owner ? m_socket : -1; }
    const std::filesystem::path& runtimeDirectory() const { return m_runtime; }
    // Drain one bounded socket batch. Open stays separate from queued voice commands.
    std::optional<std::string> poll();
    // Consume voice commands from the preceding poll(), preserving individual order.
    std::vector<VoiceRequest> pollVoice();
    // Check immediately before applying a detached request on the owning loop.
    bool voicePending(const VoiceRequest& request);
    bool replyVoice(const VoiceRequest& request, bool applied, std::string_view message = {});
    static VoiceResult sendVoice(
        VoiceCommand command, std::filesystem::path runtime = {}, std::chrono::milliseconds timeout = voiceTimeout
    );

  private:
    struct Pending;
    std::unique_ptr<Pending> m_pending;
    std::filesystem::path m_runtime;
    int m_directory = -1, m_lock = -1, m_socket = -1;
    bool m_owner = false;
  };
} // namespace app
