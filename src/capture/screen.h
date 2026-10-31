#pragma once

#include "voice/media.h"

#include <functional>
#include <memory>

namespace capture {
  enum class ScreenPhase { Idle, Selecting, Ready, Failed };
  struct ScreenState {
    ScreenPhase phase = ScreenPhase::Idle;
    std::uint64_t revision = 0;
    std::string name, error;
    bool cursor = true;
    int width = 0, height = 0;
    bool operator==(const ScreenState&) const = default;
  };
  class Screen {
  public:
    using Frame = std::function<void(std::shared_ptr<const voice::VideoFrame>)>;
    explicit Screen(Frame frame);
    ~Screen();
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
    // Only an explicit user action opens the desktop source selector.
    void select(std::string parentWindow, bool cursor);
    void cancelSelection();
    void stop();
    ScreenState state() const;
    int eventFd() const;
    void drainEvents();

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };
} // namespace capture
