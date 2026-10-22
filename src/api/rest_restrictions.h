#pragma once

#include <cstdint>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace api {
  int restServiceCode(const nlohmann::json& body);
  // Session-local pauses only. Never retain challenge payloads or replay a write.
  class RestRestrictions {
  public:
    void reset(std::uint64_t session);
    std::string blocked(std::uint64_t session, std::string_view method, std::string_view path) const;
    std::string observe(
        std::uint64_t session, std::string_view method, std::string_view path, long status, const nlohmann::json& body
    );
    std::optional<std::string> requiredAction(std::uint64_t session, const nlohmann::json& data);

  private:
    enum class Family { Messages, Conversations, Relationships, Invites, Profile, ReadState, Other };
    static Family family(std::string_view method, std::string_view path);
    static bool mutation(std::string_view method);
    mutable std::mutex m_mutex;
    std::uint64_t m_session = 0;
    bool m_httpVerification = false, m_gatewayAction = false;
    bool m_messages = false, m_conversations = false, m_joins = false;
    unsigned m_challenges = 0;
  };
} // namespace api
