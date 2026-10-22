#include "api/rest_restrictions.h"

#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace api {
  namespace {
    constexpr int verificationRequired = 40002;
    constexpr int conversationsLimited = 40003;
    constexpr int messagesDisabled = 40004;
    constexpr int joiningLimited = 340015;
    const std::string verificationNotice = "Your account requires verification. Writes are paused. Complete "
                                           "verification in the official app, then reconnect Cressida.";
    const std::string requiredActionNotice = "Your account requires attention. Writes are paused. Complete the "
                                             "requested action in the official app, then reconnect Cressida.";
    const std::string messagesNotice = "Sending messages is restricted for this account. Message sends are paused. "
                                       "Review Account Standing in the official app, then reconnect Cressida.";
    const std::string conversationsNotice =
        "Opening new conversations is restricted for this account. New conversations are paused. Review your account "
        "in the official app, then reconnect Cressida.";
    const std::string joiningNotice = "Joining servers is restricted for this account. Joins are paused. Review your "
                                      "account in the official app, then reconnect Cressida.";
    const std::string challengeNotice = "This action requires a security check. Similar writes are paused. Complete "
                                        "the check in the official app, then reconnect Cressida.";
  } // namespace
  int restServiceCode(const nlohmann::json& body) {
    if (!body.is_object())
      return 0;
    const auto at = body.find("code");
    if (at == body.end())
      return 0;
    if (at->is_number_unsigned()) {
      const auto value = at->get<std::uint64_t>();
      return value <= std::numeric_limits<int>::max() ? static_cast<int>(value) : 0;
    }
    if (at->is_number_integer()) {
      const auto value = at->get<std::int64_t>();
      return value >= 0 && value <= std::numeric_limits<int>::max() ? static_cast<int>(value) : 0;
    }
    return 0;
  }
  bool RestRestrictions::mutation(std::string_view method) {
    return method != "GET" && method != "HEAD" && method != "OPTIONS";
  }
  RestRestrictions::Family RestRestrictions::family(std::string_view method, std::string_view path) {
    path = path.substr(0, path.find('?'));
    if (method == "POST"
        && path.starts_with("/channels/")
        && (path.ends_with("/messages") || path.ends_with("/threads")))
      return Family::Messages;
    if (method == "POST" && path == "/users/@me/channels")
      return Family::Conversations;
    if (path.starts_with("/users/@me/relationships"))
      return Family::Relationships;
    if (method == "POST" && path.starts_with("/invites/"))
      return Family::Invites;
    if (method == "PATCH" && (path == "/users/@me" || path.starts_with("/users/@me/profile")))
      return Family::Profile;
    if (path.ends_with("/ack") || path == "/read-states/ack-bulk")
      return Family::ReadState;
    return Family::Other;
  }
  void RestRestrictions::reset(std::uint64_t session) {
    std::lock_guard lock(m_mutex);
    m_session = session;
    m_httpVerification = m_gatewayAction = m_messages = m_conversations = m_joins = false;
    m_challenges = 0;
  }
  std::string RestRestrictions::blocked(std::uint64_t session, std::string_view method, std::string_view path) const {
    if (!mutation(method))
      return {};
    std::lock_guard lock(m_mutex);
    if (session != m_session)
      return {};
    if (m_httpVerification)
      return verificationNotice;
    if (m_gatewayAction)
      return requiredActionNotice;
    const auto scope = family(method, path);
    if (m_messages && scope == Family::Messages)
      return messagesNotice;
    if (m_conversations && scope == Family::Conversations)
      return conversationsNotice;
    if (m_joins && scope == Family::Invites)
      return joiningNotice;
    return m_challenges & (1U << std::to_underlying(scope)) ? challengeNotice : "";
  }
  std::string RestRestrictions::observe(
      std::uint64_t session, std::string_view method, std::string_view path, long status, const nlohmann::json& body
  ) {
    if ((status != 400 && status != 403) || !body.is_object())
      return {};
    const auto code = restServiceCode(body);
    const bool challenge = status == 400 && body.contains("captcha_key");
    std::lock_guard lock(m_mutex);
    if (session != m_session)
      return {};
    if (code == verificationRequired) {
      m_httpVerification = true;
      return verificationNotice;
    }
    if (code == messagesDisabled) {
      m_messages = true;
      return messagesNotice;
    }
    if (code == conversationsLimited) {
      m_conversations = true;
      return conversationsNotice;
    }
    if (code == joiningLimited) {
      m_joins = true;
      return joiningNotice;
    }
    if (challenge) {
      m_challenges |= 1U << std::to_underlying(family(method, path));
      return challengeNotice;
    }
    return {};
  }
  std::optional<std::string> RestRestrictions::requiredAction(std::uint64_t session, const nlohmann::json& data) {
    if (!data.is_object())
      return {};
    const auto at = data.find("required_action");
    if (at == data.end() || (!at->is_null() && !at->is_string()))
      return {};
    const bool restricted = at->is_string() && !at->get_ref<const std::string&>().empty();
    std::lock_guard lock(m_mutex);
    if (session != m_session || m_gatewayAction == restricted)
      return {};
    m_gatewayAction = restricted;
    return restricted ? requiredActionNotice : std::string{};
  }
} // namespace api
