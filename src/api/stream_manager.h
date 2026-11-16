#pragma once

#include "api/stream_previews.h"
#include "api/stream_state.h"
#include "api/voice_roster.h"
#include "voice/audio.h"

#include <functional>

namespace api {
  struct StreamParent {
    std::uint64_t generation = 0;
    std::string guild, channel, self, session;
  };
  class StreamManager {
  public:
    using Send = std::function<bool(std::uint64_t, StreamCommand)>;
    using Preview = std::function<bool(StreamPreviewQuery)>;
    explicit StreamManager(Send send, Preview preview = {});
    ~StreamManager();
    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;
    void parent(StreamParent parent);
    void reset(bool clearDirectory = false);
    void event(std::uint64_t generation, std::string type, nlohmann::json data);
    void roster(std::uint64_t generation, VoiceRosterUpdate update);
    bool start(voice::ShareSettings settings);
    bool watch(std::string key);
    void stop(std::string key = {});
    void pause(bool paused);
    void configure(voice::ShareSettings settings);
    void playback(voice::AudioSettings settings);
    void subscription(std::string key, int height, int frameRate, float volume);
    std::shared_ptr<voice::VideoPipeline> preview() const;
    std::shared_ptr<voice::StreamAudio> sharedAudio() const;
    StreamSnapshot snapshot() const;
    int eventFd() const;
    void drainEvents();

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };
} // namespace api
