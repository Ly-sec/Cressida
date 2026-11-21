#include "app/window_activation.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-activation-v1-server-protocol.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <vector>
#include <wayland-client.h>
#include <wayland-server.h>

namespace {
  void require(bool ok, const char* reason) {
    if (!ok)
      throw std::runtime_error(reason);
  }
  struct Snapshot {
    unsigned requests = 0, destroyed = 0, serials = 0, surfaceCommits = 0, frames = 0;
    std::string appId;
    bool requestingSurface = false;
    std::uint32_t lastSerial = 0;
    bool requestingSeat = false;
    std::vector<std::string> activations;
  };
  class Compositor {
  public:
    wl_display* display = wl_display_create();
    std::atomic<bool> hold = false;
    std::mutex mutex;
    Snapshot state;
    std::jthread worker;
    int clientFd = -1;
    struct Pending {
      Compositor* owner;
      std::string appId;
      wl_resource* surface = nullptr;
    };
    static void destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
    Compositor() {
      require(display, "Create a private protocol compositor");
      wl_global_create(
          display, &wl_seat_interface, 1, this, [](wl_client* client, void*, std::uint32_t version, std::uint32_t id) {
            auto* resource = wl_resource_create(client, &wl_seat_interface, version, id);
            static const struct wl_seat_interface implementation{};
            wl_resource_set_implementation(resource, &implementation, nullptr, nullptr);
          }
      );
      wl_global_create(
          display, &wl_compositor_interface, 1, this,
          [](wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
            auto* resource = wl_resource_create(client, &wl_compositor_interface, version, id);
            static const struct wl_compositor_interface implementation{
                .create_surface =
                    [](wl_client* client, wl_resource* compositor, std::uint32_t id) {
                      auto* surface = wl_resource_create(client, &wl_surface_interface, 1, id);
                      static const struct wl_surface_interface implementation{
                          .destroy = destroy,
                          .attach = [](wl_client*, wl_resource*, wl_resource*, std::int32_t, std::int32_t) {},
                          .damage = [](wl_client*, wl_resource*, std::int32_t, std::int32_t, std::int32_t,
                                       std::int32_t) {},
                          .frame =
                              [](wl_client* client, wl_resource* surface, std::uint32_t id) {
                                auto& self = *static_cast<Compositor*>(wl_resource_get_user_data(surface));
                                std::lock_guard lock(self.mutex);
                                ++self.state.frames;
                                // Deliberately never deliver frame callbacks, like an occluded/minimized window.
                                wl_resource_create(client, &wl_callback_interface, 1, id);
                              },
                          .set_opaque_region = [](wl_client*, wl_resource*, wl_resource*) {},
                          .set_input_region = [](wl_client*, wl_resource*, wl_resource*) {},
                          .commit =
                              [](wl_client*, wl_resource* surface) {
                                auto& self = *static_cast<Compositor*>(wl_resource_get_user_data(surface));
                                std::lock_guard lock(self.mutex);
                                ++self.state.surfaceCommits;
                              }
                      };
                      wl_resource_set_implementation(
                          surface, &implementation, wl_resource_get_user_data(compositor), nullptr
                      );
                    },
                .create_region = [](wl_client*, wl_resource*, std::uint32_t) {}
            };
            wl_resource_set_implementation(resource, &implementation, data, nullptr);
          }
      );
      wl_global_create(
          display, &xdg_activation_v1_interface, 1, this,
          [](wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
            auto* resource = wl_resource_create(client, &xdg_activation_v1_interface, version, id);
            static const struct xdg_activation_v1_interface implementation{
                .destroy = destroy,
                .get_activation_token =
                    [](wl_client* client, wl_resource* manager, std::uint32_t id) {
                      auto& self = *static_cast<Compositor*>(wl_resource_get_user_data(manager));
                      auto* request = wl_resource_create(client, &xdg_activation_token_v1_interface, 1, id);
                      auto* pending = new Pending{&self};
                      {
                        std::lock_guard lock(self.mutex);
                        ++self.state.requests;
                      }
                      static const struct xdg_activation_token_v1_interface implementation{
                          .set_serial =
                              [](wl_client*, wl_resource* resource, std::uint32_t serial, wl_resource* seat) {
                                auto& self = *static_cast<Pending*>(wl_resource_get_user_data(resource))->owner;
                                std::lock_guard lock(self.mutex);
                                ++self.state.serials;
                                self.state.lastSerial = serial;
                                self.state.requestingSeat = seat != nullptr;
                              },
                          .set_app_id =
                              [](wl_client*, wl_resource* resource, const char* appId) {
                                static_cast<Pending*>(wl_resource_get_user_data(resource))->appId = appId;
                              },
                          .set_surface =
                              [](wl_client*, wl_resource* resource, wl_resource* surface) {
                                static_cast<Pending*>(wl_resource_get_user_data(resource))->surface = surface;
                              },
                          .commit =
                              [](wl_client*, wl_resource* resource) {
                                auto& pending = *static_cast<Pending*>(wl_resource_get_user_data(resource));
                                auto& self = *pending.owner;
                                {
                                  std::lock_guard lock(self.mutex);
                                  self.state.appId = pending.appId;
                                  self.state.requestingSurface = pending.surface != nullptr;
                                }
                                if (!self.hold)
                                  xdg_activation_token_v1_send_done(resource, "generated-focus-token");
                              },
                          .destroy = destroy
                      };
                      wl_resource_set_implementation(request, &implementation, pending, [](wl_resource* resource) {
                        auto* pending = static_cast<Pending*>(wl_resource_get_user_data(resource));
                        {
                          std::lock_guard lock(pending->owner->mutex);
                          ++pending->owner->state.destroyed;
                        }
                        delete pending;
                      });
                    },
                .activate =
                    [](wl_client*, wl_resource* resource, const char* token, wl_resource*) {
                      auto& self = *static_cast<Compositor*>(wl_resource_get_user_data(resource));
                      std::lock_guard lock(self.mutex);
                      self.state.activations.emplace_back(token);
                    }
            };
            wl_resource_set_implementation(resource, &implementation, data, nullptr);
          }
      );
      int pair[2];
      require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "Create isolated compositor transport");
      require(wl_client_create(display, pair[0]), "Accept a private Wayland client");
      clientFd = pair[1];
      worker = std::jthread([this](std::stop_token stop) {
        auto* loop = wl_display_get_event_loop(display);
        while (!stop.stop_requested()) {
          wl_event_loop_dispatch(loop, 10);
          wl_display_flush_clients(display);
        }
      });
    }
    ~Compositor() {
      worker.request_stop();
      worker.join();
      wl_display_destroy_clients(display);
      wl_display_destroy(display);
    }
    Snapshot snapshot() {
      std::lock_guard lock(mutex);
      return state;
    }
  };
} // namespace
int main() {
  try {
    Compositor server;
    auto* display = wl_display_connect_to_fd(server.clientFd);
    require(display, "Connect without opening the user's desktop");
    struct Globals {
      wl_compositor* compositor = nullptr;
      xdg_activation_v1* manager = nullptr;
      wl_seat* seat = nullptr;
    } globals;
    auto* registry = wl_display_get_registry(display);
    const wl_registry_listener listener{
        .global =
            [](void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t) {
              auto& globals = *static_cast<Globals*>(data);
              if (std::strcmp(interface, "wl_compositor") == 0)
                globals.compositor =
                    static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
              else if (std::strcmp(interface, "xdg_activation_v1") == 0)
                globals.manager =
                    static_cast<xdg_activation_v1*>(wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
              else if (std::strcmp(interface, "wl_seat") == 0)
                globals.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
            },
        .global_remove = [](void*, wl_registry*, std::uint32_t) {}
    };
    wl_registry_add_listener(registry, &listener, &globals);
    require(
        wl_display_roundtrip(display) >= 0 && globals.compositor && globals.manager,
        "Bind private activation interfaces"
    );
    auto* surface = wl_compositor_create_surface(globals.compositor);
    const auto pump = [&] {
      require(
          wl_display_roundtrip(display) >= 0 && wl_display_roundtrip(display) >= 0,
          "Dispatch native activation messages"
      );
    };
    {
      app::WindowActivation activation;
      activation.bind(globals.manager, surface);
      activation.request("hidden-window-token");
      activation.request();
      pump();
      require(
          server.snapshot().activations.empty() && server.snapshot().requests == 0,
          "A pending desktop token waits for a real map and cannot be replaced by a tokenless duplicate"
      );
      activation.presented();
      pump();
      require(
          server.snapshot().activations == std::vector<std::string>{"hidden-window-token"},
          "A newly mapped surface activates once"
      );
      activation.request("notification-token");
      pump();
      require(
          server.snapshot().activations.back() == "notification-token",
          "A mapped surface activates without another draw or frame callback"
      );
      activation.request();
      pump();
      require(
          server.snapshot().requests == 1
              && server.snapshot().activations.back() == "generated-focus-token"
              && server.snapshot().appId == app::desktopApplicationId
              && server.snapshot().requestingSurface
              && server.snapshot().serials == 0,
          "Tokenless desktop clicks asynchronously request native activation without stale seat serials"
      );
      server.hold = true;
      activation.request();
      activation.request();
      pump();
      require(server.snapshot().requests == 2, "Repeated clicks coalesce a pending compositor token request");
      const auto cancelledBefore = server.snapshot().destroyed;
      activation.request("new-host-token");
      pump();
      require(
          server.snapshot().destroyed == cancelledBefore + 1
              && server.snapshot().activations.back() == "new-host-token",
          "A supplied desktop token cancels a stalled fallback and activates immediately"
      );
      activation.request();
      pump();
      const auto activationsBefore = server.snapshot().activations.size();
      activation.unmap();
      pump();
      activation.request("cancel-before-map");
      activation.unmap();
      activation.presented();
      pump();
      require(
          server.snapshot().activations.size() == activationsBefore,
          "Hiding cancels pending requests and queued activation"
      );
      const auto focusBefore = server.snapshot().activations.size();
      std::string exported;
      unsigned delivered = 0;
      server.hold = false;
      require(
          activation.exportToken(
              globals.seat, std::numeric_limits<std::uint32_t>::max(),
              [&](std::string token) {
                exported = std::move(token);
                ++delivered;
              }
          ),
          "Accept a browser activation request"
      );
      pump();
      require(
          delivered == 1
              && exported == "generated-focus-token"
              && server.snapshot().lastSerial == std::numeric_limits<std::uint32_t>::max()
              && server.snapshot().requestingSeat
              && server.snapshot().appId.empty()
              && server.snapshot().activations.size() == focusBefore,
          "Browser handoff exports the actual seat/serial without targeting or activating our own app"
      );
      server.hold = true;
      activation.exportToken(globals.seat, 7, [&](std::string) { ++delivered; });
      pump();
      activation.unmap();
      pump();
      require(delivered == 1, "Hiding cancels a pending browser launch");
      activation.exportToken(globals.seat, 8, [&](std::string token) {
        require(token.empty(), "An unresponsive compositor falls back without an activation handle");
        ++delivered;
      });
      pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(1100));
      TimerManager::instance().tick();
      pump();
      require(delivered == 2, "An unresponsive compositor cannot block opening a link indefinitely");
      constexpr unsigned maximumPendingExports = 8;
      for (unsigned i = 0; i < maximumPendingExports; ++i)
        require(
            activation.exportToken(globals.seat, i + 1, [&](std::string) { ++delivered; }),
            "Accept bounded simultaneous clicks"
        );
      require(
          !activation.exportToken(globals.seat, 9, [&](std::string) { ++delivered; }), "Reject excess pending clicks"
      );
      activation.unmap();
      activation.exportToken(globals.seat, 0, [&](std::string token) {
        require(token.empty(), "Missing input never reuses an old activation token");
        ++delivered;
      });
      require(delivered == 3, "Missing input permits a normal link launch");
      activation.bind(nullptr, surface);
      activation.exportToken(globals.seat, 9, [&](std::string token) {
        require(token.empty(), "Missing protocol permits an ordinary browser launch");
        ++delivered;
      });
      require(delivered == 4, "Missing protocol completes the browser handoff");
      activation.request("unsupported-manager");
      activation.presented();
      pump();
      require(
          server.snapshot().activations.size() == activationsBefore,
          "A desktop without activation support remains a harmless no-op"
      );
    }
    require(
        server.snapshot().surfaceCommits == 0 && server.snapshot().frames == 0,
        "Focus requests neither remap a surface nor wait for rendering"
    );
    wl_surface_destroy(surface);
    wl_seat_destroy(globals.seat);
    xdg_activation_v1_destroy(globals.manager);
    wl_compositor_destroy(globals.compositor);
    wl_registry_destroy(registry);
    pump();
    wl_display_disconnect(display);
    std::cout << "Native Wayland activation without redraw/remapping passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
