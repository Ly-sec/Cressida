#pragma once

#include "api/streams.h"
#include "voice/state.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace voice {
  class StreamAudio;
  class VideoPipeline;
} // namespace voice

namespace api {
  struct StreamView {
    StreamRecord stream;
    voice::State connection;
    bool publishing = false, paused = false;
    std::shared_ptr<voice::VideoPipeline> video;
    std::shared_ptr<voice::StreamAudio> audio;
  };
  struct StreamSnapshot {
    std::uint64_t revision = 0, generation = 0;
    std::string channel, error;
    std::vector<StreamRecord> available;
    std::vector<StreamView> active;
  };
} // namespace api
