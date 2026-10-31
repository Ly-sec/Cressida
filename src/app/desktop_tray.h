#pragma once

#include <memory>
#include <optional>
#include <string>

namespace app {
  enum class TrayCommand { Availability, Open, Restart, Close, StopSharing };
  struct TrayEvent {
    TrayCommand command = TrayCommand::Availability;
    bool available = false;
    std::string activationToken;
  };
  // D-Bus runs on a private worker. Commands wake the owning application loop.
  class DesktopTray {
  public:
    DesktopTray();
    ~DesktopTray();
    DesktopTray(const DesktopTray&) = delete;
    DesktopTray& operator=(const DesktopTray&) = delete;
    void start(std::string iconPath);
    void sharing(bool active);
    int eventFd() const;
    std::optional<TrayEvent> poll();

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };
} // namespace app
