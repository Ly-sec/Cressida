#pragma once

#include "voice/audio.h"
#include "voice/state.h"
#include "voice/video_codec.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace voice {
  // The callback runs on a worker and must be queued onto the application's loop.
  class Connection {
  public:
    using Changed = std::function<void(State)>;
    explicit Connection(
        std::shared_ptr<Audio> audio, Changed changed, std::string loopbackEndpoint = {},
        std::shared_ptr<VideoPipeline> video = {}
    );
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    bool connect(ConnectionInfo info);
    void disconnect();
    void configure(AudioSettings settings);
    void participants(std::vector<std::string> users);
    void pauseVideo(bool paused);
    void videoSubscription(int height, int frameRate);

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
  };
} // namespace voice
