#include "api/streams.h"

#include <iostream>
#include <stdexcept>

namespace {
  void require(bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  }
} // namespace
int main() {
  try {
    using namespace api;
    using Json = nlohmann::json;
    const StreamKey guild{"100", "200", "300"}, call{"", "200", "300"};
    require(parseStreamKey(guild.value()) == guild && parseStreamKey(call.value()) == call, "Scoped keys roundtrip");
    for (const auto* invalid :
         {"", "guild:100:200:300:", "guild:100:200:300:400", "call:200:300:", "call:0200:300", "guild::200:300",
          "call:0:300", "guild:1:2:18446744073709551616"})
      require(!parseStreamKey(invalid), "Malformed or ambiguous stream keys are rejected");
    const auto create = *streamCommand({StreamCommandKind::Create, guild});
    require(
        create["op"] == 18
            && create["d"]["type"] == "guild"
            && create["d"]["guild_id"] == "100"
            && create["d"]["channel_id"] == "200"
            && create["d"]["preferred_region"].is_null(),
        "Create uses the public client envelope without invented routes"
    );
    require((*streamCommand({StreamCommandKind::Create, call}))["d"]["guild_id"].is_null(), "Call shares omit a guild");
    for (const auto& [kind, opcode] :
         {std::pair{StreamCommandKind::Delete, 19},
          {StreamCommandKind::Watch, 20},
          {StreamCommandKind::Ping, 21},
          {StreamCommandKind::Pause, 22}}) {
      const auto packet = *streamCommand({kind, guild, true});
      require(
          packet["op"] == opcode && packet["d"]["stream_key"] == guild.value(), "Commands preserve target identity"
      );
    }
    auto created = Json{{"stream_key", guild.value()},  {"rtc_server_id", "400"}, {"rtc_channel_id", "500"},
                        {"viewer_ids", {"301", "302"}}, {"paused", false},        {"region", "test"}};
    auto server =
        Json{{"stream_key", guild.value()}, {"endpoint", "synthetic.discord.media"}, {"token", "synthetic-token"}};
    for (const bool serverFirst : {false, true}) {
      StreamSession session;
      require(session.begin(1, 2, guild, "300", "synthetic-session", true), "Explicit publishing session starts");
      require(
          session.receive(serverFirst ? "STREAM_SERVER_UPDATE" : "STREAM_CREATE", serverFirst ? server : created)
              == StreamSession::Update::None,
          "A single event cannot start a media connection"
      );
      require(
          session.receive(serverFirst ? "STREAM_CREATE" : "STREAM_SERVER_UPDATE", serverFirst ? created : server)
              == StreamSession::Update::Ready,
          "Setup accepts both event orders"
      );
      auto info = session.take();
      require(
          info
              && info->server == "400"
              && info->channel == "500"
              && info->user == "300"
              && info->session == "synthetic-session"
              && info->generation == 1
              && info->revision == 2
              && info->participants.size() == 3,
          "Media uses RTC identities and the stream's own roster"
      );
      require(
          !session.take() && session.receive("STREAM_CREATE", created) == StreamSession::Update::None,
          "Duplicate setup does not reuse credentials or restart transport"
      );
      require(
          session.receive("STREAM_DELETE", {{"stream_key", guild.value()}}) == StreamSession::Update::Ended,
          "Delete retires the explicit intention"
      );
      require(
          session.receive("STREAM_SERVER_UPDATE", server) == StreamSession::Update::None && !session.active(),
          "Late setup cannot revive a stopped stream"
      );
    }
    StreamSession session;
    for (const bool explicitNull : {false, true}) {
      auto legacy = created;
      legacy.erase("rtc_channel_id");
      if (explicitNull)
        legacy["rtc_channel_id"] = nullptr;
      require(
          session.begin(3, 4, guild, "300", "synthetic-session", true), "An explicit stream can omit its RTC channel"
      );
      require(
          session.receive("STREAM_CREATE", legacy) == StreamSession::Update::None,
          "The optional RTC channel fallback still waits for server credentials"
      );
      require(
          session.receive("STREAM_SERVER_UPDATE", server) == StreamSession::Update::Ready,
          "An omitted RTC channel does not prevent stream setup"
      );
      const auto info = session.take();
      require(
          info && info->server == "400" && info->channel == "399",
          "The public stream RTC fallback uses the preceding server identity"
      );
    }
    for (const auto& invalidChannel : {Json("invalid"), Json("0"), Json(false), Json(500)}) {
      auto malformedCreate = created;
      malformedCreate["rtc_channel_id"] = invalidChannel;
      require(session.begin(3, 4, guild, "300", "synthetic-session", true), "Malformed explicit-channel test starts");
      session.receive("STREAM_SERVER_UPDATE", server);
      require(
          session.receive("STREAM_CREATE", malformedCreate) == StreamSession::Update::None && !session.take(),
          "Only absent or null RTC channels use the identity fallback"
      );
    }
    require(
        !session.begin(3, 4, guild, "300", "bad\nsession", true), "Private parent sessions reject control characters"
    );
    require(session.begin(3, 4, guild, "301", "synthetic-session", false), "Watching retains a separate self identity");
    auto unrelated = server;
    unrelated["stream_key"] = "guild:101:200:300";
    require(
        session.receive("STREAM_SERVER_UPDATE", unrelated) == StreamSession::Update::None, "Other guilds are isolated"
    );
    auto malformed = server;
    malformed["endpoint"] = "attacker.example";
    require(
        session.receive("STREAM_SERVER_UPDATE", malformed) == StreamSession::Update::None,
        "Endpoints use the media allowlist"
    );
    require(
        session.expired(std::chrono::steady_clock::now() + streamSetupDeadline), "Missing setup has a finite deadline"
    );
    require(
        session.begin(3, 5, guild, "300", "synthetic-session", true, true),
        "A later explicit attempt has a fresh revision"
    );
    require(
        session.receive("STREAM_SERVER_UPDATE", server) == StreamSession::Update::None,
        "Reused keys reject unfenced credentials"
    );
    session.receive("STREAM_CREATE", created);
    require(!session.take(), "A reused key waits for credentials after the fresh create fence");
    require(
        session.receive("STREAM_SERVER_UPDATE", server) == StreamSession::Update::Ready, "Fresh fenced setup settles"
    );
    StreamDirectory directory;
    directory.apply(*parseStreamUpdate("STREAM_CREATE", created));
    directory.apply(*parseStreamUpdate("STREAM_UPDATE", {{"stream_key", guild.value()}, {"paused", true}}));
    require(
        directory.find(guild.value())->paused && directory.find(guild.value())->viewers.size() == 2,
        "Partial updates retain omitted viewer fields"
    );
    directory.apply(
        *parseStreamUpdate("STREAM_UPDATE", {{"stream_key", guild.value()}, {"viewer_ids", Json::array()}})
    );
    require(directory.find(guild.value())->viewers.empty(), "An accepted empty viewer list replaces the cache");
    malformed = created;
    malformed["viewer_ids"] = {"301", "301"};
    require(!parseStreamUpdate("STREAM_CREATE", malformed), "Duplicate viewer identities are rejected");
    malformed["viewer_ids"] = Json::array({false});
    require(!parseStreamUpdate("STREAM_CREATE", malformed), "Malformed viewer identities are rejected");
    for (std::size_t i = 0; i < maximumStreams + 1; ++i) {
      auto item = created;
      item["stream_key"] = StreamKey{"100", "200", std::to_string(1000 + i)}.value();
      directory.apply(*parseStreamUpdate("STREAM_CREATE", item));
    }
    require(directory.records().size() == maximumStreams, "Discovery cache stays bounded");
    directory.clear();
    require(directory.records().empty(), "Account reset drops discovery");
    std::cout << "Stream commands, identities, ordering, revisions and bounded discovery passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
