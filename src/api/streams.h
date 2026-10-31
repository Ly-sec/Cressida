#pragma once

#include "voice/connection.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace api {
  inline constexpr std::size_t maximumStreams = 50;
  inline constexpr std::size_t maximumStreamViewers = 256;
  inline constexpr std::size_t maximumStreamKeyBytes = 96;
  inline constexpr auto streamSetupDeadline = std::chrono::seconds{20};
  inline constexpr auto streamPingInterval = std::chrono::seconds{10};
  struct StreamKey {
    std::string guild, channel, owner;
    std::string value() const;
    bool operator==(const StreamKey&) const = default;
  };
  std::optional<StreamKey> parseStreamKey(std::string_view value);
  bool
  shareQualityAvailable(voice::ShareSettings settings, std::optional<int> premiumType, std::optional<int> serverTier);
  enum class StreamCommandKind { Create, Delete, Watch, Ping, Pause };
  struct StreamCommand {
    StreamCommandKind kind;
    StreamKey target;
    bool paused = false;
  };
  std::optional<nlohmann::json> streamCommand(const StreamCommand& command);
  struct StreamRecord {
    StreamKey key;
    std::vector<std::string> viewers;
    std::string region;
    bool paused = false;
    bool operator==(const StreamRecord&) const = default;
  };
  struct StreamUpdate {
    enum class Kind { Create, Update, Delete } kind;
    StreamRecord stream;
    bool viewersPresent = false, pausedPresent = false, regionPresent = false;
  };
  std::optional<StreamUpdate> parseStreamUpdate(std::string_view type, const nlohmann::json& data);
  class StreamDirectory {
  public:
    void clear();
    void apply(const StreamUpdate& update);
    std::vector<StreamRecord> records() const;
    std::optional<StreamRecord> find(const std::string& key) const;

  private:
    std::map<std::string, StreamRecord> m_records;
  };
  // Credentials stay on the backend and are retired with this explicit intention.
  class StreamSession {
  public:
    enum class Update { None, Ready, Ended, Failed };
    bool begin(
        std::uint64_t generation, std::uint64_t revision, StreamKey key, std::string self, std::string parentSession,
        bool publishing, bool reused = false
    );
    Update receive(std::string_view type, const nlohmann::json& data);
    void clear();
    bool active() const { return !m_key.channel.empty(); }
    const StreamKey& key() const { return m_key; }
    bool publishing() const { return m_publishing; }
    std::uint64_t revision() const { return m_info.revision; }
    bool expired(std::chrono::steady_clock::time_point now) const;
    std::optional<voice::ConnectionInfo> take();

  private:
    StreamKey m_key;
    voice::ConnectionInfo m_info;
    std::chrono::steady_clock::time_point m_deadline{};
    bool m_publishing = false, m_created = false, m_started = false, m_reused = false;
  };
} // namespace api
