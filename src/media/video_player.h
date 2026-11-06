#pragma once

#include "core/process/wake_event.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace media {
  // Reads occur on the playback engine's worker, never on the UI thread.
  // Implementations provide bounded random access and cancellable IO.
  class VideoInput {
  public:
    virtual ~VideoInput() = default;
    virtual std::int64_t read(std::uint64_t offset, std::span<char> output) = 0;
    virtual std::int64_t size() = 0;
    virtual void cancel() noexcept = 0;
  };
  std::shared_ptr<VideoInput> memoryVideoInput(std::span<const std::uint8_t> bytes);
  struct VideoState {
    bool loading = false, playing = false, buffering = false, ended = false, muted = false, hasAudio = false;
    double position = 0, duration = 0, volume = 25, speed = 1;
    int width = 0, height = 0;
    std::string error;
  };
  // One active video, with bounded streaming/decoder queues rather than a stored
  // RGBA loop. All public calls belong to the owning UI/render thread.
  class VideoPlayer {
  public:
    VideoPlayer();
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;
    using GetProc = void* (*)(void*, const char*);
    bool open(std::shared_ptr<VideoInput> input, GetProc getProc, bool silent = false);
    bool openAudio(std::shared_ptr<VideoInput> input, bool silent = false);
    void close();
    bool drain();
    void update();
    void pause(bool paused);
    void seek(double seconds);
    void volume(double percent);
    void speed(double rate);
    void mute(bool muted);
    bool render(unsigned framebuffer, int width, int height, bool force = false);
    void presented();
    const VideoState& state() const;
    int eventFd() const noexcept;

  private:
    bool openInternal(std::shared_ptr<VideoInput> input, GetProc getProc, bool silent, bool audioOnly);
    struct Impl;
    std::unique_ptr<Impl> m;
  };
} // namespace media
