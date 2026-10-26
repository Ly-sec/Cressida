#include "api/voice_session.h"

#include <iostream>
#include <stdexcept>

namespace {
  using Json = nlohmann::json;
  using Update = api::VoiceSession::Update;
  constexpr std::uint64_t generation = 7, revision = 11;
  const std::string guild = "100", channel = "200", user = "300";

  void require(bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  }
  Json state(const Json& target = channel) {
    return {{"guild_id", guild}, {"user_id", user}, {"channel_id", target}, {"session_id", "synthetic-session"},
            {"mute", false},     {"deaf", false},   {"suppress", false}};
  }
  Json server() { return {{"guild_id", guild}, {"token", "synthetic-token"}, {"endpoint", "voice.example:443"}}; }
  void begin(api::VoiceSession& session) { session.begin(generation, revision, guild, channel, user); }
  void complete(api::VoiceSession& session) {
    require(
        session.receive("VOICE_STATE_UPDATE", state()) == Update::None, "A state update alone has no server credentials"
    );
    require(
        session.receive("VOICE_SERVER_UPDATE", server()) == Update::Ready,
        "Matching signaling records complete one voice connection"
    );
  }
} // namespace

int main() {
  try {
    for (const bool serverFirst : {false, true}) {
      api::VoiceSession session;
      begin(session);
      require(
          session.active()
              && session.channel() == channel
              && session.guild() == guild
              && session.revision() == revision,
          "An explicit join retains its exact target and revision"
      );
      require(
          session.receive(serverFirst ? "VOICE_SERVER_UPDATE" : "VOICE_STATE_UPDATE", serverFirst ? server() : state())
              == Update::None,
          "The first half of signaling never starts a connection"
      );
      require(
          session.receive(serverFirst ? "VOICE_STATE_UPDATE" : "VOICE_SERVER_UPDATE", serverFirst ? state() : server())
              == Update::Ready,
          "State-first and server-first signaling both complete"
      );
      const auto connection = session.take();
      require(
          connection.server == guild
              && connection.user == user
              && connection.channel == channel
              && connection.session == "synthetic-session"
              && connection.token == "synthetic-token"
              && connection.endpoint == "voice.example:443"
              && connection.generation == generation
              && connection.revision == revision,
          "The transport receives only the paired target's credentials and owning generation"
      );
      require(
          session.receive("VOICE_STATE_UPDATE", state()) == Update::None,
          "Repeated state updates do not reuse consumed server credentials"
      );
      const auto consumed = session.take();
      require(consumed.token.empty() && consumed.endpoint.empty(), "Server credentials are removed after transfer");
      session.clear();
      const auto cleared = session.take();
      require(
          !session.active()
              && session.guild().empty()
              && !session.revision()
              && cleared.server.empty()
              && cleared.user.empty()
              && cleared.channel.empty()
              && cleared.session.empty()
              && cleared.token.empty()
              && cleared.endpoint.empty()
              && !cleared.generation
              && !cleared.revision
              && !session.muted
              && !session.deafened,
          "Clearing a session releases all identities, credentials, flags and generation state"
      );
      require(
          session.receive("VOICE_STATE_UPDATE", state()) == Update::None
              && session.receive("VOICE_SERVER_UPDATE", server()) == Update::None,
          "Unsolicited signaling cannot reactivate a cleared session"
      );
    }

    api::VoiceSession unrelated;
    begin(unrelated);
    for (const auto* field : {"user_id", "guild_id", "channel_id"}) {
      auto wrong = state();
      wrong[field] = "999";
      wrong["mute"] = wrong["deaf"] = true;
      require(
          unrelated.receive("VOICE_STATE_UPDATE", wrong) == Update::None
              && unrelated.active()
              && !unrelated.muted
              && !unrelated.deafened,
          "A different user, guild or pre-join channel cannot change the pending intention"
      );
    }
    auto wrongServer = server();
    wrongServer["guild_id"] = "999";
    require(
        unrelated.receive("VOICE_SERVER_UPDATE", wrongServer) == Update::None,
        "Server credentials are scoped to the intended server"
    );
    require(
        unrelated.receive("UNRELATED_EVENT", state()) == Update::None, "Unrelated dispatches cannot pair signaling"
    );
    complete(unrelated);

    for (const bool serverFirst : {false, true}) {
      api::VoiceSession leaving;
      begin(leaving);
      if (serverFirst)
        require(
            leaving.receive("VOICE_SERVER_UPDATE", server()) == Update::None,
            "Server-first credentials wait for local confirmation"
        );
      require(
          leaving.receive("VOICE_STATE_UPDATE", state(nullptr)) == Update::None && leaving.active(),
          "A delayed leave preceding the matching join cannot cancel a fresh intention"
      );
      require(
          leaving.receive("VOICE_STATE_UPDATE", state()) == (serverFirst ? Update::Ready : Update::None),
          "Matching local state survives an earlier leave"
      );
      require(
          leaving.receive("VOICE_STATE_UPDATE", state(nullptr)) == Update::Ended && !leaving.active(),
          "A leave after local join confirmation ends pending or established connections"
      );
    }

    for (const Json& target : {Json(nullptr), Json("400")}) {
      api::VoiceSession ended;
      begin(ended);
      complete(ended);
      (void)ended.take();
      require(
          ended.receive("VOICE_STATE_UPDATE", state(target)) == Update::Ended && !ended.active(),
          "An active forced disconnect or server move ends the original explicit connection"
      );
      require(
          ended.receive("VOICE_SERVER_UPDATE", server()) == Update::None,
          "Late server credentials cannot restore a terminated voice intention"
      );
    }

    api::VoiceSession migrating;
    begin(migrating);
    complete(migrating);
    (void)migrating.take();
    auto migration = server();
    migration["endpoint"] = nullptr;
    require(
        migrating.receive("VOICE_SERVER_UPDATE", migration) == Update::Migrating && migrating.active(),
        "A temporarily missing endpoint retains the explicit join while suspending transport"
    );
    require(
        migrating.receive("VOICE_STATE_UPDATE", state()) == Update::None,
        "Migration never reconnects using consumed or cleared credentials"
    );
    auto replacement = server();
    replacement["token"] = "replacement-token";
    replacement["endpoint"] = "replacement.example:443";
    require(
        migrating.receive("VOICE_SERVER_UPDATE", replacement) == Update::Ready,
        "A replacement endpoint pairs with the existing matching voice session"
    );
    const auto moved = migrating.take();
    require(
        moved.token == "replacement-token"
            && moved.endpoint == "replacement.example:443"
            && moved.session == "synthetic-session"
            && moved.generation == generation
            && moved.revision == revision,
        "Migration retains account identity and intention revision while replacing private server credentials"
    );

    for (const Json& bad : {Json(), Json::array(), Json("invalid"), Json(1), Json(false)}) {
      api::VoiceSession invalid;
      begin(invalid);
      require(
          invalid.receive("VOICE_STATE_UPDATE", bad) == Update::None
              && invalid.receive("VOICE_SERVER_UPDATE", bad) == Update::None
              && invalid.active(),
          "Nonobject payloads cannot affect a pending connection"
      );
    }
    for (const auto* field : {"session_id", "user_id", "guild_id", "channel_id"}) {
      for (const Json& bad : {Json(), Json(7), Json::array(), Json::object(), Json(std::string(129, 'x'))}) {
        api::VoiceSession invalid;
        begin(invalid);
        auto malformed = state();
        malformed[field] = bad;
        require(
            invalid.receive("VOICE_STATE_UPDATE", malformed) == Update::None,
            "Malformed or oversized state identities never complete a join"
        );
        require(
            invalid.receive("VOICE_SERVER_UPDATE", server()) == Update::None,
            "Rejected state payloads cannot leave a usable partial voice session"
        );
      }
    }
    for (const auto* field : {"token", "endpoint", "guild_id"}) {
      for (const Json& bad : {Json(7), Json::array(), Json::object(), Json(""), Json(std::string(4097, 'x'))}) {
        api::VoiceSession invalid;
        begin(invalid);
        auto malformed = server();
        malformed[field] = bad;
        require(
            invalid.receive("VOICE_SERVER_UPDATE", malformed) == Update::None,
            "Malformed or oversized server credentials never complete a join"
        );
        require(
            invalid.receive("VOICE_STATE_UPDATE", state()) == Update::None,
            "Rejected server payloads cannot leave a usable partial server credential"
        );
      }
    }
    api::VoiceSession invalidFlags;
    begin(invalidFlags);
    auto noSession = state();
    noSession.erase("session_id");
    noSession["mute"] = noSession["deaf"] = true;
    require(
        invalidFlags.receive("VOICE_STATE_UPDATE", noSession) == Update::None
            && !invalidFlags.muted
            && !invalidFlags.deafened,
        "Invalid state records cannot partially change effective audio flags"
    );
    auto forcedFlags = state();
    forcedFlags["suppress"] = forcedFlags["deaf"] = true;
    require(
        invalidFlags.receive("VOICE_STATE_UPDATE", forcedFlags) == Update::None
            && invalidFlags.muted
            && invalidFlags.deafened,
        "Valid server suppression and deafening flags remain independent of local preferences"
    );

    api::VoiceSession direct;
    direct.begin(generation, revision, {}, channel, user);
    auto directState = state();
    directState.erase("guild_id");
    const Json directServer{{"channel_id", channel}, {"endpoint", "direct.example:443"}, {"token", "direct-token"}};
    require(
        direct.receive("VOICE_SERVER_UPDATE", directServer) == Update::None
            && direct.receive("VOICE_STATE_UPDATE", directState) == Update::Ready,
        "A direct-call server update pairs by channel with a guildless local voice state"
    );
    require(direct.take().server == channel, "Direct-call transports use the channel as their server identity");

    api::VoiceSession replaced;
    begin(replaced);
    require(
        replaced.receive("VOICE_STATE_UPDATE", state()) == Update::None, "An old intention can hold partial signaling"
    );
    replaced.begin(generation + 1, revision + 1, "101", "201", user);
    require(
        replaced.receive("VOICE_SERVER_UPDATE", server()) == Update::None
            && replaced.receive("VOICE_STATE_UPDATE", state()) == Update::None,
        "Starting another account generation and target drops the previous pairing"
    );
    auto freshState = state("201");
    freshState["guild_id"] = "101";
    auto freshServer = server();
    freshServer["guild_id"] = "101";
    require(
        replaced.receive("VOICE_SERVER_UPDATE", freshServer) == Update::None
            && replaced.receive("VOICE_STATE_UPDATE", freshState) == Update::Ready,
        "The replacement intention can pair only its own target"
    );
    const auto fresh = replaced.take();
    require(
        fresh.generation == generation + 1
            && fresh.revision == revision + 1
            && fresh.server == "101"
            && fresh.channel == "201",
        "The replacement transport is fenced by the new account generation and intention revision"
    );

    std::cout << "Voice signaling pairing, migration, malformed payloads and intention lifecycle passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
