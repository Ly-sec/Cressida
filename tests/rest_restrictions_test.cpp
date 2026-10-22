#include "api/rest_restrictions.h"

#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

int main() {
  const auto require = [](bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  using Json = nlohmann::json;
  try {
    api::RestRestrictions state;
    state.reset(1);
    require(
        state.observe(1, "POST", "/channels/10/messages", 403, {{"code", 50013}}).empty()
            && state.blocked(1, "POST", "/channels/10/messages").empty(),
        "Ordinary missing permissions never pause unrelated account work"
    );
    const auto message = state.observe(1, "POST", "/channels/10/messages", 403, {{"code", 40004}});
    require(
        message.contains("paused")
            && !state.blocked(1, "POST", "/channels/20/messages").empty()
            && !state.blocked(1, "POST", "/channels/20/threads").empty()
            && state.blocked(1, "POST", "/channels/20/messages/30/ack").empty()
            && state.blocked(1, "PATCH", "/channels/20/messages/30").empty()
            && state.blocked(1, "GET", "/channels/20/messages").empty(),
        "Send restrictions stop queued message and thread creation across channels without stopping reads"
    );
    state.reset(2);
    require(
        state.blocked(2, "POST", "/channels/10/messages").empty(),
        "Explicit reconnect starts a new restriction generation without replaying any command"
    );
    state.observe(2, "POST", "/users/@me/channels", 400, {{"code", 40003}});
    require(
        !state.blocked(2, "POST", "/users/@me/channels").empty()
            && state.blocked(2, "POST", "/channels/10/messages").empty(),
        "Opening-DM restrictions do not disable existing conversations"
    );
    state.observe(2, "POST", "/invites/example", 403, {{"code", 340015}});
    require(
        !state.blocked(2, "POST", "/invites/another").empty(), "Join restrictions apply to subsequent server joins"
    );
    const auto challenge = state.observe(
        2, "POST", "/users/@me/relationships", 400,
        {{"captcha_key", Json::array({"private challenge"})}, {"captcha_rqtoken", "never retained"}}
    );
    require(
        challenge.contains("security check")
            && !challenge.contains("private")
            && !state.blocked(2, "PUT", "/users/@me/relationships/77").empty()
            && state.blocked(2, "PATCH", "/users/@me").empty(),
        "Challenges stop the affected action family and expose only fixed safe recovery text"
    );
    state.observe(2, "GET", "/users/@me", 403, {{"code", 40002}});
    require(
        !state.blocked(2, "POST", "/channels/10/messages").empty()
            && !state.blocked(2, "PATCH", "/users/@me").empty()
            && !state.blocked(2, "POST", "/channels/10/messages/11/ack").empty()
            && state.blocked(2, "GET", "/channels/10/messages").empty(),
        "Verification observed by a read lane stops account writes while retaining read access"
    );
    state.reset(3);
    std::jthread late([&] { state.observe(2, "POST", "/channels/10/messages", 403, {{"code", 40002}}); });
    late.join();
    require(
        state.blocked(3, "POST", "/channels/10/messages").empty(),
        "Old-session responses cannot pause a newly connected account"
    );
    require(
        !state.requiredAction(3, {{"required_action", 42}}), "Malformed required-action events do not pause writes"
    );
    require(
        state.requiredAction(3, {{"required_action", "AGREEMENTS"}}).has_value()
            && !state.blocked(3, "POST", "/channels/10/messages").empty(),
        "Required account agreements pause writes until completed externally"
    );
    state.requiredAction(3, {{"required_action", nullptr}});
    require(
        state.requiredAction(3, {{"required_action", "REQUIRE_VERIFIED_EMAIL"}}).has_value()
            && !state.blocked(3, "POST", "/channels/10/messages").empty(),
        "Required-action Gateway events pause writes before an HTTP failure"
    );
    require(
        !state.requiredAction(3, {{"required_action", "REQUIRE_VERIFIED_EMAIL"}}),
        "Repeated required-action events do not produce repeated notifications"
    );
    require(
        state.requiredAction(3, {{"required_action", nullptr}}).has_value()
            && state.blocked(3, "POST", "/channels/10/messages").empty(),
        "A cleared Gateway action does not retain an unrelated pause or replay failed actions"
    );
    state.observe(3, "POST", "/channels/10/messages", 403, {{"code", 40002}});
    state.requiredAction(3, {{"required_action", "REQUIRE_VERIFIED_EMAIL"}});
    state.requiredAction(3, {{"required_action", nullptr}});
    require(
        !state.blocked(3, "POST", "/channels/10/messages").empty(),
        "Gateway clearance cannot undo an independently observed HTTP verification requirement"
    );
    require(
        api::restServiceCode({{"code", 40002}}) == 40002
            && api::restServiceCode({{"code", -1}}) == 0
            && api::restServiceCode({{"code", "40002"}}) == 0
            && api::restServiceCode({{"code", 40002.5}}) == 0
            && api::restServiceCode({{"code", std::numeric_limits<std::uint64_t>::max()}}) == 0,
        "Only bounded integral service codes cross the request boundary"
    );
    state.reset(4);
    state.observe(4, "POST", "/channels/10/messages", 401, {{"code", 40002}});
    require(
        state.blocked(4, "POST", "/channels/10/messages").empty(),
        "Unauthorized responses retain their independent logout path"
    );
    state.observe(4, "POST", "/channels/10/messages", 200, {{"code", 40002}});
    require(
        state.blocked(4, "POST", "/channels/10/messages").empty(),
        "Successful response content cannot manufacture an account restriction"
    );
    std::cout << "Session-scoped restriction handling passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
