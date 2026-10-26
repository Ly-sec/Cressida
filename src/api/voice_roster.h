#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace api {
  inline constexpr std::size_t maximumVoiceMembers = 4096;
  inline constexpr std::size_t maximumGuildVoiceMembers = 1024;
  inline constexpr std::uint16_t allVoiceMemberFields = 0x3ff;
  struct VoiceMember {
    std::string guild, channel, user, name, avatar;
    bool muted = false, deafened = false, selfMuted = false, selfDeafened = false;
    bool suppressed = false, video = false, stream = false;
    // Parsed partial updates carry presence bits so omitted fields retain cached values.
    std::uint16_t present = allVoiceMemberFields;
  };
  struct VoiceRosterUpdate {
    bool reset = false;
    std::vector<std::string> resetGuilds, removedChannels;
    std::vector<VoiceMember> members;
  };
  std::optional<VoiceRosterUpdate> voiceRosterUpdate(std::string_view type, const nlohmann::json& data);
  class VoiceRoster {
  public:
    void clear();
    void apply(const VoiceRosterUpdate& update);
    std::vector<VoiceMember> members(const std::string& guild, const std::string& channel) const;
    std::size_t count(const std::string& guild, const std::string& channel) const;

  private:
    std::map<std::string, std::map<std::string, VoiceMember>> m_guilds;
    std::size_t m_count = 0;
  };
} // namespace api
