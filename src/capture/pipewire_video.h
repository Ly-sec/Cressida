#pragma once

#include "capture/screen.h"

namespace capture {
  class PipeWireVideo {
  public:
    using Failed = std::function<void(std::string)>;
    PipeWireVideo();
    ~PipeWireVideo();
    PipeWireVideo(const PipeWireVideo&) = delete;
    PipeWireVideo& operator=(const PipeWireVideo&) = delete;
    // Takes ownership of the portal's restricted remote descriptor.
    bool start(int remote, std::uint32_t node, Screen::Frame frame, Failed failed);
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };
} // namespace capture
