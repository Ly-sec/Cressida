#include "app/window.h"

#include "app/desktop_activation.h"
#include "app/layout_metrics.h"
#include "app/surface_scale.h"
#include "app/window_activation.h"
#include "app/window_decorations.h"
#include "core/deferred_call.h"
#include "core/environment.h"
#include "core/log.h"
#include "core/timer_manager.h"
#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "render/gl_shared_context.h"
#include "render/scene/input_area.h"
#include "ui/style.h"
#include "viewporter-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

namespace app {
  using Clock = std::chrono::steady_clock;
  namespace {
    constexpr Logger kLog("window");
    [[noreturn]] void windowFailure(const char* stage, const char* message, int code = 0) {
      kLog.error("window failed stage={} code={}", stage, code);
      throw std::runtime_error(message);
    }
  } // namespace

  struct Window::Impl {
    Window& owner;
    bool keyboardFocused = false;
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* shell = nullptr;
    xdg_activation_v1* activation = nullptr;
    WindowActivation activationState;
    std::uint64_t toplevelGeneration = 0;
    wl_surface* surface = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_viewport* viewport = nullptr;
    wp_fractional_scale_manager_v1* fractionalManager = nullptr;
    wp_fractional_scale_v1* fractionalScale = nullptr;
    SurfaceScale surfaceScale;
    struct Output {
      Impl* owner;
      std::uint32_t name;
      wl_output* handle = nullptr;
      int pendingScale = 1;
    };
    std::map<std::uint32_t, std::unique_ptr<Output>> outputs;
    wl_callback* frameCallback = nullptr;
    xdg_surface* role = nullptr;
    xdg_toplevel* toplevel = nullptr;
    zxdg_decoration_manager_v1* decorationManager = nullptr;
    zxdg_toplevel_decoration_v1* decoration = nullptr;
    WindowDecorations decorations;
    WindowCapabilities capabilities;
    WindowControlPreferences windowControls;
    bool maximized = false, fullscreen = false;
    std::uint32_t pointerSerial = 0;
    Clock::time_point titleClick{};
    std::uint32_t cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_pointer* pointer = nullptr;
    wp_cursor_shape_manager_v1* cursorManager = nullptr;
    wp_cursor_shape_device_v1* cursorDevice = nullptr;
    wl_data_device_manager* dataManager = nullptr;
    wl_data_device* dataDevice = nullptr;
    struct Offer {
      wl_data_offer* handle;
      std::string mime, imageMime;
    };
    std::vector<std::unique_ptr<Offer>> offers;
    Offer* selection = nullptr;
    xkb_context* xkb = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* state = nullptr;
    xkb_compose_table* composeTable = nullptr;
    xkb_compose_state* compose = nullptr;
    int repeatRate = 25;
    int repeatDelay = 500;
    std::uint32_t repeatKey = 0;
    Clock::time_point repeatAt{};
    int pasteFd = -1;
    std::string pasteText, pasteMime;
    Clock::time_point pasteDeadline;
    std::uint32_t inputSerial = 0;
    struct Source {
      Impl* owner;
      wl_data_source* handle;
      std::shared_ptr<const std::string> bytes;
      std::string mime;
    };
    std::vector<std::unique_ptr<Source>> sources;
    struct Write {
      int fd;
      std::shared_ptr<const std::string> bytes;
      std::size_t offset = 0;
      Clock::time_point deadline;
    };
    std::vector<Write> writes;
    float pointerX = 0;
    float pointerY = 0;
    double axisValue = 0;
    std::int32_t axisDiscrete = 0, axisValue120 = 0;
    std::uint32_t axisSource = WL_POINTER_AXIS_SOURCE_WHEEL;
    std::uint32_t axisTime = 0;
    bool traceScroll = false;
    int width = 1120;
    int height = 760;
    float uiScale = 1;
    bool configured = false;
    bool running = true;
    bool visible = true, restoreMaximized = false, restoreFullscreen = false;
    bool dirty = true;
    GlSharedContext shared;
    RenderContext context;
    RenderTarget target;

    explicit Impl(Window& window) : owner(window) {
      const auto* trace = std::getenv("CRESSIDA_TRACE_SCROLL");
      traceScroll = trace && std::strcmp(trace, "1") == 0;
    }
    ~Impl() {
      activationState.unmap();
      // EGL resources must be destroyed before their wl_surface and display.
      target.destroy();
      if (frameCallback)
        wl_callback_destroy(frameCallback);
      context.cleanup();
      shared.cleanup();
      if (pasteFd >= 0)
        ::close(pasteFd);
      for (auto& write : writes)
        ::close(write.fd);
      for (auto& source : sources)
        wl_data_source_destroy(source->handle);
      for (auto& offer : offers)
        wl_data_offer_destroy(offer->handle);
      if (dataDevice)
        wl_data_device_release(dataDevice);
      if (dataManager)
        wl_data_device_manager_destroy(dataManager);
      if (cursorDevice)
        wp_cursor_shape_device_v1_destroy(cursorDevice);
      if (cursorManager)
        wp_cursor_shape_manager_v1_destroy(cursorManager);
      if (pointer)
        wl_pointer_release(pointer);
      if (keyboard)
        wl_keyboard_release(keyboard);
      if (seat)
        wl_seat_release(seat);
      if (compose)
        xkb_compose_state_unref(compose);
      if (composeTable)
        xkb_compose_table_unref(composeTable);
      if (state)
        xkb_state_unref(state);
      if (keymap)
        xkb_keymap_unref(keymap);
      if (xkb)
        xkb_context_unref(xkb);
      if (decoration)
        zxdg_toplevel_decoration_v1_destroy(decoration);
      if (decorationManager)
        zxdg_decoration_manager_v1_destroy(decorationManager);
      if (fractionalScale)
        wp_fractional_scale_v1_destroy(fractionalScale);
      if (viewport)
        wp_viewport_destroy(viewport);
      if (toplevel)
        xdg_toplevel_destroy(toplevel);
      if (role)
        xdg_surface_destroy(role);
      if (surface)
        wl_surface_destroy(surface);
      for (const auto& [name, output] : outputs)
        releaseOutput(output->handle);
      if (fractionalManager)
        wp_fractional_scale_manager_v1_destroy(fractionalManager);
      if (viewporter)
        wp_viewporter_destroy(viewporter);
      if (shell)
        xdg_wm_base_destroy(shell);
      if (activation)
        xdg_activation_v1_destroy(activation);
      if (compositor)
        wl_compositor_destroy(compositor);
      if (registry)
        wl_registry_destroy(registry);
      if (display)
        wl_display_disconnect(display);
    }

    void createToplevel() {
      ++toplevelGeneration;
      static_assert(WindowCapabilities::maximizeCapability == XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE);
      static_assert(WindowCapabilities::minimizeCapability == XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
      capabilities = {};
      role = xdg_wm_base_get_xdg_surface(shell, surface);
      toplevel = xdg_surface_get_toplevel(role);
      xdg_toplevel_set_title(toplevel, "Cressida");
      xdg_toplevel_set_app_id(toplevel, desktopApplicationId);
      xdg_toplevel_set_min_size(toplevel, layout::Window::minimumWidth, layout::Window::minimumHeight);
      static const xdg_toplevel_listener toplevelListener{
          .configure =
              [](void* data, xdg_toplevel*, std::int32_t width, std::int32_t height, wl_array* states) {
                auto& self = *static_cast<Impl*>(data);
                if (width > 0)
                  self.width = width;
                if (height > 0)
                  self.height = height;
                self.maximized = self.fullscreen = false;
                auto* values = static_cast<const std::uint32_t*>(states->data);
                for (std::size_t i = 0; i < states->size / sizeof(std::uint32_t); ++i) {
                  self.maximized |= values[i] == XDG_TOPLEVEL_STATE_MAXIMIZED;
                  self.fullscreen |= values[i] == XDG_TOPLEVEL_STATE_FULLSCREEN;
                }
              },
          .close = [](void* data, xdg_toplevel*) { static_cast<Impl*>(data)->owner.requestClose(); },
          .configure_bounds = [](void*, xdg_toplevel*, std::int32_t, std::int32_t) {},
          .wm_capabilities =
              [](void* data, xdg_toplevel*, wl_array* values) {
                static_cast<Impl*>(data)->capabilities.configure(
                    {static_cast<const std::uint32_t*>(values->data), values->size / sizeof(std::uint32_t)}
                );
              },
      };
      xdg_toplevel_add_listener(toplevel, &toplevelListener, this);
      if (decorationManager) {
        decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(decorationManager, toplevel);
        static const zxdg_toplevel_decoration_v1_listener listener{
            .configure = [](void* data, zxdg_toplevel_decoration_v1*, std::uint32_t mode) {
              static_cast<Impl*>(data)->decorations.configure(mode);
            },
        };
        zxdg_toplevel_decoration_v1_add_listener(decoration, &listener, this);
        static_assert(
            static_cast<std::uint32_t>(WindowDecorations::Mode::Client) == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
        );
        static_assert(
            static_cast<std::uint32_t>(WindowDecorations::Mode::Server) == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
        );
        zxdg_toplevel_decoration_v1_set_mode(decoration, static_cast<std::uint32_t>(decorations.requested()));
      }
      static const xdg_surface_listener roleListener{
          .configure = [](void* data, xdg_surface* role, std::uint32_t serial) {
            auto& self = *static_cast<Impl*>(data);
            xdg_surface_ack_configure(role, serial);
            self.configured = true;
            self.decorations.surfaceConfigured();
            self.capabilities.surfaceConfigured();
            self.resize();
          },
      };
      xdg_surface_add_listener(role, &roleListener, this);
    }

    void resize() {
      if (!target.surfaceTarget())
        return;
      const auto geometry = wl_surface_get_version(surface) >= 3 || surfaceScale.usesViewport()
          ? surfaceScale.geometry(width, height, uiScale)
          : SurfaceScale{}.geometry(width, height, uiScale);
      if (wl_surface_get_version(surface) >= 3)
        wl_surface_set_buffer_scale(surface, geometry.bufferScale);
      if (viewport)
        wp_viewport_set_destination(viewport, width, height);
      target.setContentScale(geometry.contentScale);
      target.setLogicalSize(geometry.logicalWidth, geometry.logicalHeight);
      target.resize(geometry.bufferWidth, geometry.bufferHeight);
      dirty = true;
    }

    static void releaseOutput(wl_output* output) {
      if (wl_output_get_version(output) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
        wl_output_release(output);
      else
        wl_output_destroy(output);
    }

    void outputEntered(wl_output* handle, bool entered) {
      for (const auto& [name, output] : outputs)
        if (output->handle == handle) {
          if (entered)
            surfaceScale.enter(name);
          else
            surfaceScale.leave(name);
          resize();
          return;
        }
    }

    void dispatchAxis() {
      if (axisValue == 0 && axisDiscrete == 0 && axisValue120 == 0)
        return;
      if (traceScroll)
        std::fprintf(
            stderr, "scroll wayland time=%u source=%u value=%.4f discrete=%d value120=%d\n", axisTime, axisSource,
            axisValue, axisDiscrete, axisValue120
        );
      InputArea::PointerData event;
      event.axisSource = axisSource;
      event.axisValue = axisValue;
      event.axisLines = axisValue120 ? axisValue120 / 120.0F : float(axisDiscrete);
      if (state && xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 && owner.zoom)
        owner.zoom(axisValue != 0 ? axisValue : event.axisLines);
      else if (owner.scroll)
        owner.scroll(event.scrollDelta(Style::scrollWheelStep) / uiScale);
      axisValue = 0;
      axisDiscrete = axisValue120 = 0;
    }

    void requestFrame() {
      frameCallback = wl_surface_frame(surface);
      static const wl_callback_listener listener{.done = [](void* data, wl_callback* callback, std::uint32_t) {
        auto& self = *static_cast<Impl*>(data);
        self.frameCallback = nullptr;
        wl_callback_destroy(callback);
      }};
      wl_callback_add_listener(frameCallback, &listener, this);
    }

    void emitKey(std::uint32_t code) {
      if (!visible || !state || !owner.key)
        return;
      Key event;
      event.symbol = xkb_state_key_get_one_sym(state, code);
      event.control = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
      event.shift = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
      event.alt = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
      event.super = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0;
      char text[128]{};
      if (compose && !event.control && xkb_compose_state_feed(compose, event.symbol) == XKB_COMPOSE_FEED_ACCEPTED) {
        switch (xkb_compose_state_get_status(compose)) {
        case XKB_COMPOSE_COMPOSING:
          return;
        case XKB_COMPOSE_COMPOSED:
          xkb_compose_state_get_utf8(compose, text, sizeof(text));
          xkb_compose_state_reset(compose);
          break;
        case XKB_COMPOSE_CANCELLED:
          xkb_compose_state_reset(compose);
          return;
        default:
          xkb_state_key_get_utf8(state, code, text, sizeof(text));
          break;
        }
      } else
        xkb_state_key_get_utf8(state, code, text, sizeof(text));
      event.text = text;
      owner.key(event);
    }

    void bindCursor() {
      if (cursorManager && pointer && !cursorDevice)
        cursorDevice = wp_cursor_shape_manager_v1_get_pointer(cursorManager, pointer);
    }

    static inline const wl_keyboard_listener keyboardListener{
        .keymap =
            [](void* data, wl_keyboard*, std::uint32_t format, int fd, std::uint32_t size) {
              auto& self = *static_cast<Impl*>(data);
              if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0 || size > 4 * 1024 * 1024) {
                ::close(fd);
                return;
              }
              void* bytes = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
              ::close(fd);
              if (bytes == MAP_FAILED)
                return;
              xkb_keymap* next = nullptr;
              if (static_cast<char*>(bytes)[size - 1] == '\0')
                next = xkb_keymap_new_from_string(
                    self.xkb, static_cast<char*>(bytes), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS
                );
              munmap(bytes, size);
              if (!next)
                return;
              auto* nextState = xkb_state_new(next);
              if (!nextState) {
                xkb_keymap_unref(next);
                return;
              }
              if (self.state)
                xkb_state_unref(self.state);
              if (self.keymap)
                xkb_keymap_unref(self.keymap);
              self.keymap = next;
              self.state = nextState;
            },
        .enter =
            [](void* data, wl_keyboard*, std::uint32_t, wl_surface*, wl_array*) {
              auto& self = *static_cast<Impl*>(data);
              if (!self.visible)
                return;
              const bool changed = !self.keyboardFocused;
              self.keyboardFocused = true;
              if (changed && self.owner.focusChanged)
                self.owner.focusChanged(true);
              self.owner.invalidate();
            },
        .leave =
            [](void* data, wl_keyboard*, std::uint32_t, wl_surface*) {
              auto& self = *static_cast<Impl*>(data);
              const bool changed = self.keyboardFocused;
              self.keyboardFocused = false;
              self.repeatKey = 0;
              if (changed && self.owner.focusChanged)
                self.owner.focusChanged(false);
              self.owner.invalidate();
            },
        .key =
            [](void* data, wl_keyboard*, std::uint32_t serial, std::uint32_t, std::uint32_t key, std::uint32_t state) {
              auto& self = *static_cast<Impl*>(data);
              if (!self.visible)
                return;
              if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
                self.inputSerial = serial;
                self.emitKey(key + 8);
                if (self.keymap && xkb_keymap_key_repeats(self.keymap, key + 8) && self.repeatRate > 0) {
                  self.repeatKey = key + 8;
                  self.repeatAt = Clock::now() + std::chrono::milliseconds(self.repeatDelay);
                }
              } else {
                if (self.repeatKey == key + 8)
                  self.repeatKey = 0;
                if (self.state && self.owner.keyReleased)
                  self.owner.keyReleased(xkb_state_key_get_one_sym(self.state, key + 8));
              }
            },
        .modifiers =
            [](void* data, wl_keyboard*, std::uint32_t, std::uint32_t depressed, std::uint32_t latched,
               std::uint32_t locked, std::uint32_t group) {
              auto& self = *static_cast<Impl*>(data);
              if (self.state)
                xkb_state_update_mask(self.state, depressed, latched, locked, 0, 0, group);
            },
        .repeat_info =
            [](void* data, wl_keyboard*, std::int32_t rate, std::int32_t delay) {
              auto& self = *static_cast<Impl*>(data);
              self.repeatRate = std::clamp(rate, 0, 100);
              self.repeatDelay = std::clamp(delay, 0, 5000);
            },
    };

    static inline const wl_pointer_listener pointerListener{
        .enter =
            [](void* data, wl_pointer*, std::uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
              auto& self = *static_cast<Impl*>(data);
              self.pointerX = wl_fixed_to_double(x);
              self.pointerY = wl_fixed_to_double(y);
              self.pointerSerial = serial;
              if (self.cursorDevice)
                wp_cursor_shape_device_v1_set_shape(self.cursorDevice, serial, self.cursorShape);
              if (self.owner.motion)
                self.owner.motion(self.pointerX / self.uiScale, self.pointerY / self.uiScale);
            },
        .leave =
            [](void* data, wl_pointer*, std::uint32_t, wl_surface*) {
              auto& self = *static_cast<Impl*>(data);
              self.pointerSerial = 0;
              if (self.owner.pointerLeft)
                self.owner.pointerLeft();
            },
        .motion =
            [](void* data, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
              auto& self = *static_cast<Impl*>(data);
              self.pointerX = wl_fixed_to_double(x);
              self.pointerY = wl_fixed_to_double(y);
              if (self.owner.motion)
                self.owner.motion(self.pointerX / self.uiScale, self.pointerY / self.uiScale);
            },
        .button =
            [](void* data, wl_pointer*, std::uint32_t serial, std::uint32_t, std::uint32_t button,
               std::uint32_t state) {
              auto& self = *static_cast<Impl*>(data);
              if (button == 0x111) {
                if (state == WL_POINTER_BUTTON_STATE_PRESSED && self.owner.contextClick) {
                  self.inputSerial = serial;
                  self.owner.contextClick(self.pointerX / self.uiScale, self.pointerY / self.uiScale);
                }
                return;
              }
              if (button != 0x110)
                return;
              self.inputSerial = serial;
              if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
                if (self.owner.released)
                  self.owner.released();
                return;
              }
              if (!self.maximized
                  && !self.fullscreen
                  && self.decorations.clientSide()
                  && (self.pointerX >= self.width - 7 || self.pointerY >= self.height - 7)) {
                const auto edge = self.pointerX >= self.width - 7
                    ? (self.pointerY >= self.height - 7 ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT
                                                        : XDG_TOPLEVEL_RESIZE_EDGE_RIGHT)
                    : XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
                xdg_toplevel_resize(self.toplevel, self.seat, serial, edge);
              } else if (self.owner.titlebarDragContains(self.pointerX / self.uiScale, self.pointerY / self.uiScale)) {
                if (Clock::now() - self.titleClick < std::chrono::milliseconds(350)) {
                  self.owner.toggleMaximized();
                  self.titleClick = {};
                } else {
                  self.titleClick = Clock::now();
                  xdg_toplevel_move(self.toplevel, self.seat, serial);
                }
              } else if (self.owner.click)
                self.owner.click(self.pointerX / self.uiScale, self.pointerY / self.uiScale, serial);
            },
        .axis =
            [](void* data, wl_pointer* pointer, std::uint32_t time, std::uint32_t axis, wl_fixed_t value) {
              auto& self = *static_cast<Impl*>(data);
              if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
                self.axisTime = time;
                self.axisValue += wl_fixed_to_double(value);
                if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(pointer)) < WL_POINTER_FRAME_SINCE_VERSION)
                  self.dispatchAxis();
              }
            },
        .frame = [](void* data, wl_pointer*) { static_cast<Impl*>(data)->dispatchAxis(); },
        .axis_source = [](void* data, wl_pointer*,
                          std::uint32_t source) { static_cast<Impl*>(data)->axisSource = source; },
        .axis_stop = [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {},
        .axis_discrete =
            [](void* data, wl_pointer*, std::uint32_t axis, std::int32_t value) {
              if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
                static_cast<Impl*>(data)->axisDiscrete += value;
            },
        .axis_value120 =
            [](void* data, wl_pointer*, std::uint32_t axis, std::int32_t value) {
              if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
                static_cast<Impl*>(data)->axisValue120 += value;
            },
    };

    void bindDataDevice() {
      if (!dataManager || !seat || dataDevice)
        return;
      dataDevice = wl_data_device_manager_get_data_device(dataManager, seat);
      static const wl_data_device_listener listener{
          .data_offer =
              [](void* data, wl_data_device*, wl_data_offer* handle) {
                auto& self = *static_cast<Impl*>(data);
                auto offer = std::make_unique<Offer>(Offer{handle, {}});
                static const wl_data_offer_listener offerListener{
                    .offer =
                        [](void* data, wl_data_offer*, const char* mime) {
                          auto& item = *static_cast<Offer*>(data);
                          if (std::strcmp(mime, "text/plain;charset=utf-8") == 0
                              || (item.mime.empty() && std::strcmp(mime, "text/plain") == 0))
                            item.mime = mime;
                          if (std::strcmp(mime, "image/png") == 0
                              || (item.imageMime.empty()
                                  && (std::strcmp(mime, "image/jpeg") == 0 || std::strcmp(mime, "image/webp") == 0)))
                            item.imageMime = mime;
                        },
                    .source_actions = [](void*, wl_data_offer*, std::uint32_t) {},
                    .action = [](void*, wl_data_offer*, std::uint32_t) {},
                };
                wl_data_offer_add_listener(handle, &offerListener, offer.get());
                self.offers.push_back(std::move(offer));
              },
          .enter = [](void*, wl_data_device*, std::uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer*) {},
          .leave = [](void*, wl_data_device*) {},
          .motion = [](void*, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) {},
          .drop = [](void*, wl_data_device*) {},
          .selection =
              [](void* data, wl_data_device*, wl_data_offer* handle) {
                auto& self = *static_cast<Impl*>(data);
                self.selection = nullptr;
                for (auto& offer : self.offers)
                  if (offer->handle == handle)
                    self.selection = offer.get();
                // Offers are valid through the selection callback; retire replaced offers here.
                std::erase_if(self.offers, [&](const auto& offer) {
                  if (offer.get() == self.selection)
                    return false;
                  wl_data_offer_destroy(offer->handle);
                  return true;
                });
              },
      };
      wl_data_device_add_listener(dataDevice, &listener, this);
    }

    void readPaste() {
      if (pasteFd < 0)
        return;
      char bytes[4096];
      for (;;) {
        const auto count = ::read(pasteFd, bytes, sizeof(bytes));
        if (count > 0) {
          if (pasteText.size() + count > 20 * 1024 * 1024) {
            ::close(pasteFd);
            pasteFd = -1;
            pasteText.clear();
            if (owner.pasted)
              owner.pasted({}, "error");
            return;
          }
          pasteText.append(bytes, count);
        } else if (count == 0) {
          ::close(pasteFd);
          pasteFd = -1;
          if (owner.pasted)
            owner.pasted(std::exchange(pasteText, {}), std::exchange(pasteMime, {}));
          return;
        } else {
          if (errno == EINTR)
            continue;
          if (errno != EAGAIN) {
            ::close(pasteFd);
            pasteFd = -1;
            pasteText.clear();
          }
          return;
        }
      }
    }
  };

  Window::Window() : m(std::make_unique<Impl>(*this)) {}
  Window::~Window() = default;

  void Window::initialize() {
    std::signal(SIGPIPE, SIG_IGN);
    const char* emojiFont = cressida::environment("CRESSIDA_EMOJI_FONT", "DISCORD_CPP_EMOJI_FONT");
#ifdef CRESSIDA_EMOJI_FONT
    if (!emojiFont || !*emojiFont)
      emojiFont = CRESSIDA_EMOJI_FONT;
#endif
    if (emojiFont && *emojiFont)
      FcConfigAppFontAddFile(FcConfigGetCurrent(), reinterpret_cast<const FcChar8*>(emojiFont));
    m->display = wl_display_connect(nullptr);
    if (!m->display)
      windowFailure("connect", "Could not connect to the Wayland desktop", errno);
    m->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!m->xkb)
      windowFailure("keyboard", "Could not initialize keyboard input", 0);
    const char* locale = std::setlocale(LC_CTYPE, "");
    m->composeTable = xkb_compose_table_new_from_locale(m->xkb, locale ? locale : "C", XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (m->composeTable)
      m->compose = xkb_compose_state_new(m->composeTable, XKB_COMPOSE_STATE_NO_FLAGS);
    m->registry = wl_display_get_registry(m->display);
    static const wl_registry_listener registryListener{
        .global =
            [](void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
              auto& self = *static_cast<Impl*>(data);
              if (std::strcmp(interface, wl_compositor_interface.name) == 0)
                self.compositor = static_cast<wl_compositor*>(
                    wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 6U))
                );
              else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
                self.shell = static_cast<xdg_wm_base*>(wl_registry_bind(
                    registry, name, &xdg_wm_base_interface, std::min(version, WindowCapabilities::protocolVersion)
                ));
                static const xdg_wm_base_listener listener{.ping = [](void*, xdg_wm_base* shell, std::uint32_t serial) {
                  xdg_wm_base_pong(shell, serial);
                }};
                xdg_wm_base_add_listener(self.shell, &listener, nullptr);
              } else if (std::strcmp(interface, xdg_activation_v1_interface.name) == 0 && !self.activation) {
                self.activation =
                    static_cast<xdg_activation_v1*>(wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
              } else if (std::strcmp(interface, wp_viewporter_interface.name) == 0 && !self.viewporter) {
                self.viewporter =
                    static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
              } else if (
                  std::strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0 && !self.fractionalManager
              ) {
                self.fractionalManager = static_cast<wp_fractional_scale_manager_v1*>(
                    wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1)
                );
              } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
                auto output = std::make_unique<Impl::Output>();
                output->owner = &self;
                output->name = name;
                output->handle = static_cast<wl_output*>(
                    wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4U))
                );
                self.surfaceScale.output(name, 1);
                static const wl_output_listener listener{
                    .geometry = [](void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                                   std::int32_t, const char*, const char*, std::int32_t) {},
                    .mode = [](void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {},
                    .done =
                        [](void* data, wl_output*) {
                          auto& output = *static_cast<Impl::Output*>(data);
                          output.owner->surfaceScale.output(output.name, output.pendingScale);
                          output.owner->resize();
                        },
                    .scale = [](void* data, wl_output*,
                                std::int32_t scale) { static_cast<Impl::Output*>(data)->pendingScale = scale; },
                    .name = [](void*, wl_output*, const char*) {},
                    .description = [](void*, wl_output*, const char*) {},
                };
                wl_output_add_listener(output->handle, &listener, output.get());
                self.outputs.emplace(name, std::move(output));
              } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !self.seat) {
                self.seat =
                    static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 8U)));
                static const wl_seat_listener listener{
                    .capabilities =
                        [](void* data, wl_seat* seat, std::uint32_t capabilities) {
                          auto& self = *static_cast<Impl*>(data);
                          if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !self.keyboard) {
                            self.keyboard = wl_seat_get_keyboard(seat);
                            wl_keyboard_add_listener(self.keyboard, &Impl::keyboardListener, &self);
                          } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && self.keyboard) {
                            wl_keyboard_release(self.keyboard);
                            self.keyboard = nullptr;
                            self.repeatKey = 0;
                          }
                          if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !self.pointer) {
                            self.pointer = wl_seat_get_pointer(seat);
                            wl_pointer_add_listener(self.pointer, &Impl::pointerListener, &self);
                            self.bindCursor();
                          } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && self.pointer) {
                            if (self.cursorDevice) {
                              wp_cursor_shape_device_v1_destroy(self.cursorDevice);
                              self.cursorDevice = nullptr;
                            }
                            wl_pointer_release(self.pointer);
                            self.pointer = nullptr;
                          }
                        },
                    .name = [](void*, wl_seat*, const char*) {},
                };
                wl_seat_add_listener(self.seat, &listener, &self);
                self.bindDataDevice();
              } else if (std::strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
                self.cursorManager = static_cast<wp_cursor_shape_manager_v1*>(
                    wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1)
                );
                self.bindCursor();
              } else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
                self.decorationManager = static_cast<zxdg_decoration_manager_v1*>(
                    wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1)
                );
              } else if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0 && version >= 3) {
                self.dataManager = static_cast<wl_data_device_manager*>(
                    wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3)
                );
                self.bindDataDevice();
              }
            },
        .global_remove =
            [](void* data, wl_registry*, std::uint32_t name) {
              auto& self = *static_cast<Impl*>(data);
              if (const auto output = self.outputs.find(name); output != self.outputs.end()) {
                self.surfaceScale.remove(name);
                Impl::releaseOutput(output->second->handle);
                self.outputs.erase(output);
                self.resize();
              }
            },
    };
    wl_registry_add_listener(m->registry, &registryListener, m.get());
    kLog.info("Wayland connection established");
    if (wl_display_roundtrip(m->display) < 0 || !m->compositor || !m->shell)
      windowFailure("desktop-protocol", "The desktop does not provide xdg-shell", wl_display_get_error(m->display));
    m->surface = wl_compositor_create_surface(m->compositor);
    m->activationState.bind(m->activation, m->surface);
    static const wl_surface_listener surfaceListener{
        .enter = [](void* data, wl_surface*,
                    wl_output* output) { static_cast<Impl*>(data)->outputEntered(output, true); },
        .leave = [](void* data, wl_surface*,
                    wl_output* output) { static_cast<Impl*>(data)->outputEntered(output, false); },
        .preferred_buffer_scale =
            [](void* data, wl_surface*, std::int32_t scale) {
              auto& self = *static_cast<Impl*>(data);
              self.surfaceScale.preferredInteger(scale);
              self.resize();
            },
        .preferred_buffer_transform = [](void*, wl_surface*, std::uint32_t) {},
    };
    wl_surface_add_listener(m->surface, &surfaceListener, m.get());
    if (m->viewporter && m->fractionalManager) {
      m->viewport = wp_viewporter_get_viewport(m->viewporter, m->surface);
      m->fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(m->fractionalManager, m->surface);
      m->surfaceScale.setFractionalAvailable(true);
      static const wp_fractional_scale_v1_listener listener{
          .preferred_scale = [](void* data, wp_fractional_scale_v1*, std::uint32_t numerator) {
            auto& self = *static_cast<Impl*>(data);
            self.surfaceScale.preferredFractional(numerator);
            self.resize();
          },
      };
      wp_fractional_scale_v1_add_listener(m->fractionalScale, &listener, m.get());
    }
    m->createToplevel();
    m->shared.initialize(m->display);
    m->context.initialize(m->shared);
    m->target.create(m->surface, m->context);
    m->resize();
    wl_surface_commit(m->surface);
  }

  void Window::run(
      int wakeFd, std::function<void()> tick, int frameLimit, int assetFd, int videoFd, int themeFd, int pickerFd,
      int audioFd, int notificationFd, int trayFd, int iconFd, int instanceFd, int voiceFd
  ) {
    int frames = 0;
    while (m->running) {
      if (wl_display_dispatch_pending(m->display) < 0)
        windowFailure("dispatch", "Wayland connection lost", wl_display_get_error(m->display));
      tick();
      if (m->pasteFd >= 0 && Clock::now() > m->pasteDeadline) {
        ::close(m->pasteFd);
        m->pasteFd = -1;
        m->pasteText.clear();
        if (pasted)
          pasted({}, "error");
      }
      if (m->repeatKey && Clock::now() >= m->repeatAt && m->repeatRate > 0) {
        m->emitKey(m->repeatKey);
        m->repeatAt = Clock::now() + std::chrono::milliseconds(1000 / m->repeatRate);
      }
      if (m->visible && m->configured && m->dirty && !m->frameCallback && draw) {
        m->dirty = false;
        const bool animating = animate && animate();
        m->requestFrame();
        const auto drawnGeneration = m->toplevelGeneration;
        draw();
        if (m->visible && m->configured && m->toplevelGeneration == drawnGeneration) {
          m->activationState.presented();
        }
        if (animating)
          m->dirty = true;
        if (frameLimit > 0 && ++frames >= frameLimit)
          break;
      }
      if (!m->running)
        break;
      bool redraw = false;
      while (wl_display_prepare_read(m->display) != 0) {
        if (wl_display_dispatch_pending(m->display) < 0)
          windowFailure("dispatch", "Wayland connection lost", wl_display_get_error(m->display));
        if (m->visible && m->configured && m->dirty && !m->frameCallback) {
          redraw = true;
          break;
        }
      }
      if (redraw)
        continue;
      const int flushed = wl_display_flush(m->display);
      if (flushed < 0 && errno != EAGAIN) {
        wl_display_cancel_read(m->display);
        windowFailure("flush", "Wayland flush failed", errno);
      }
      std::vector<pollfd> descriptors = {
          {wl_display_get_fd(m->display), static_cast<short>(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0},
          {wakeFd, POLLIN, 0},
          {m->pasteFd, POLLIN, 0}
      };
      for (const auto& write : m->writes)
        descriptors.push_back({write.fd, POLLOUT, 0});
      const auto repeatTimeout = m->repeatKey
          ? std::clamp<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(m->repeatAt - Clock::now()).count(), 0, 500
            )
          : 500;
      const auto timerTimeout = TimerManager::instance().pollTimeoutMs();
      const auto timeout = timerTimeout < 0 ? repeatTimeout : std::min(repeatTimeout, timerTimeout);
      descriptors.push_back({DeferredCall::wakeFd(), POLLIN, 0});
      descriptors.push_back({assetFd, POLLIN, 0});
      descriptors.push_back({videoFd, POLLIN, 0});
      descriptors.push_back({themeFd, POLLIN, 0});
      descriptors.push_back({pickerFd, POLLIN, 0});
      descriptors.push_back({audioFd, POLLIN, 0});
      descriptors.push_back({notificationFd, POLLIN, 0});
      descriptors.push_back({trayFd, POLLIN, 0});
      descriptors.push_back({iconFd, POLLIN, 0});
      descriptors.push_back({instanceFd, POLLIN, 0});
      descriptors.push_back({voiceFd, POLLIN, 0});
      const int ready = ::poll(descriptors.data(), descriptors.size(), timeout);
      if (ready > 0 && (descriptors[0].revents & POLLIN)) {
        if (wl_display_read_events(m->display) < 0)
          windowFailure("read", "Wayland read failed", errno);
      } else
        wl_display_cancel_read(m->display);
      if (ready < 0 && errno != EINTR)
        windowFailure("poll", "Window event polling failed", errno);
      if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        windowFailure("connection-closed", "Wayland connection closed", wl_display_get_error(m->display));
      if (descriptors[2].revents)
        m->readPaste();
      for (std::size_t i = m->writes.size(); i > 0; --i) {
        auto& write = m->writes[i - 1];
        bool done = Clock::now() > write.deadline || (descriptors[i + 2].revents & (POLLHUP | POLLERR | POLLNVAL));
        if (!done && (descriptors[i + 2].revents & POLLOUT)) {
          const auto count = ::write(
              write.fd, write.bytes->data() + write.offset,
              std::min<std::size_t>(65536, write.bytes->size() - write.offset)
          );
          if (count > 0)
            write.offset += count;
          else if (count < 0 && errno != EINTR && errno != EAGAIN)
            done = true;
        }
        if (done || write.offset == write.bytes->size()) {
          ::close(write.fd);
          m->writes.erase(m->writes.begin() + i - 1);
        }
      }
    }
  }

  void Window::paste(bool images) {
    if (!m->selection || m->pasteFd >= 0)
      return;
    const auto mime = images && !m->selection->imageMime.empty() ? m->selection->imageMime : m->selection->mime;
    if (mime.empty())
      return;
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) != 0)
      return;
    fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    m->pasteFd = descriptors[0];
    m->pasteText.clear();
    m->pasteMime = mime;
    m->pasteDeadline = Clock::now() + std::chrono::seconds(10);
    wl_data_offer_receive(m->selection->handle, mime.c_str(), descriptors[1]);
    ::close(descriptors[1]);
    wl_display_flush(m->display);
  }

  void Window::copy(std::string bytes, std::string mime) {
    if (!m->dataDevice || !m->dataManager || !m->inputSerial || bytes.empty())
      return;
    auto source = std::make_unique<Impl::Source>();
    source->owner = m.get();
    source->handle = wl_data_device_manager_create_data_source(m->dataManager);
    source->bytes = std::make_shared<const std::string>(std::move(bytes));
    source->mime = std::move(mime);
    static const wl_data_source_listener listener{
        .target = [](void*, wl_data_source*, const char*) {},
        .send =
            [](void* data, wl_data_source*, const char*, int fd) {
              auto& source = *static_cast<Impl::Source*>(data);
              auto& owner = *source.owner;
              if (owner.writes.size() >= 8) {
                ::close(fd);
                return;
              }
              fcntl(fd, F_SETFL, O_NONBLOCK);
              owner.writes.push_back({fd, source.bytes, 0, Clock::now() + std::chrono::seconds(10)});
            },
        .cancelled =
            [](void* data, wl_data_source* handle) {
              auto& source = *static_cast<Impl::Source*>(data);
              auto* owner = source.owner;
              wl_data_source_destroy(handle);
              std::erase_if(owner->sources, [handle](const auto& item) { return item->handle == handle; });
            },
        .dnd_drop_performed = [](void*, wl_data_source*) {},
        .dnd_finished = [](void*, wl_data_source*) {},
        .action = [](void*, wl_data_source*, std::uint32_t) {},
    };
    wl_data_source_add_listener(source->handle, &listener, source.get());
    wl_data_source_offer(source->handle, source->mime.c_str());
    if (source->mime.starts_with("text/plain"))
      wl_data_source_offer(source->handle, "text/plain");
    wl_data_device_set_selection(m->dataDevice, source->handle, m->inputSerial);
    m->sources.push_back(std::move(source));
  }

  void Window::minimize() {
    if (m->toplevel && m->capabilities.minimize())
      xdg_toplevel_set_minimized(m->toplevel);
  }
  void Window::toggleMaximized() {
    if (!m->toplevel || !m->capabilities.maximize())
      return;
    if (m->maximized)
      xdg_toplevel_unset_maximized(m->toplevel);
    else
      xdg_toplevel_set_maximized(m->toplevel);
  }
  bool Window::active() const { return m->visible && m->keyboardFocused; }
  bool Window::preciseScroll() const {
    return m->axisSource == WL_POINTER_AXIS_SOURCE_FINGER || m->axisSource == WL_POINTER_AXIS_SOURCE_CONTINUOUS;
  }
  bool Window::maximized() const { return m->maximized; }
  bool Window::fullscreen() const { return m->fullscreen; }
  void Window::setFullscreen(bool enabled) {
    if (!m->toplevel)
      return;
    if (enabled)
      xdg_toplevel_set_fullscreen(m->toplevel, nullptr);
    else
      xdg_toplevel_unset_fullscreen(m->toplevel);
    invalidate();
  }
  float Window::titlebarHeight() const {
    return m->decorations.needsTitlebar(m->fullscreen) ? layout::Window::titlebarHeight : 0;
  }
  void Window::setDesktopDecorations(bool enabled) {
    if (!m->decorations.preferDesktop(enabled))
      return;
    if (m->decoration)
      zxdg_toplevel_decoration_v1_set_mode(m->decoration, static_cast<std::uint32_t>(m->decorations.requested()));
    // Continue using the confirmed mode until the compositor configures it.
    invalidate();
  }
  bool Window::desktopDecorations() const { return m->decorations.desktopPreferred(); }
  bool Window::desktopDecorationsAvailable() const { return m->decoration != nullptr; }
  void Window::setWindowControls(const WindowControlPreferences& preferences) { m->windowControls = preferences; }
  const WindowCapabilities& Window::capabilities() const { return m->capabilities; }
  bool Window::titlebarDragContains(float x, float y) const {
    return titlebarHeight() > 0
        && layout::Titlebar::dragContains(
               x, y, width(), visibleWindowButtons(m->windowControls, m->capabilities).size(), m->windowControls.left,
               m->windowControls.buttonWidth
        );
  }
  void Window::setCursorShape(std::uint32_t shape) {
    if (m->cursorShape == shape)
      return;
    m->cursorShape = shape;
    if (m->cursorDevice && m->pointerSerial)
      wp_cursor_shape_device_v1_set_shape(m->cursorDevice, m->pointerSerial, shape);
  }
  std::uint32_t Window::cursorShape() const { return m->cursorShape; }
  void Window::close() { m->running = false; }
  void Window::requestClose() {
    if (closeRequested)
      closeRequested();
    else
      close();
  }
  bool Window::visible() const { return m->visible; }
  std::uint64_t Window::toplevelGeneration() const { return m->toplevelGeneration; }
  void Window::hide() {
    kLog.debug("hide requested visible={}", m->visible);
    if (!m->visible)
      return;
    m->visible = false;
    m->configured = false;
    m->activationState.unmap();
    m->restoreMaximized = m->maximized;
    m->restoreFullscreen = m->fullscreen;
    m->repeatKey = 0;
    if (m->frameCallback) {
      wl_callback_destroy(m->frameCallback);
      m->frameCallback = nullptr;
    }
    if (m->keyboardFocused) {
      m->keyboardFocused = false;
      if (focusChanged)
        focusChanged(false);
    }
    if (pointerLeft)
      pointerLeft();
    wl_surface_attach(m->surface, nullptr, 0, 0);
    wl_surface_commit(m->surface);
    // A fresh shell role guarantees a new configure on restore, including on
    // compositors that retain the old role's configure state after unmapping.
    // Keep the wl_surface, EGL target and shared resources alive for asset work.
    if (m->decoration) {
      zxdg_toplevel_decoration_v1_destroy(m->decoration);
      m->decoration = nullptr;
    }
    xdg_toplevel_destroy(m->toplevel);
    m->toplevel = nullptr;
    xdg_surface_destroy(m->role);
    m->role = nullptr;
    m->dirty = true;
  }
  void Window::show(std::string activationToken) {
    kLog.debug("show requested visible={} activation_supplied={}", m->visible, !activationToken.empty());
    if (!m->visible) {
      m->visible = true;
      m->configured = false;
      m->createToplevel();
      if (m->restoreMaximized)
        xdg_toplevel_set_maximized(m->toplevel);
      if (m->restoreFullscreen)
        xdg_toplevel_set_fullscreen(m->toplevel, nullptr);
      wl_surface_commit(m->surface);
    }
    m->activationState.request(activationToken);
    invalidate();
  }
  void Window::invalidate() { m->dirty = true; }
  void Window::setUiScale(float scale) {
    m->uiScale = std::isfinite(scale) ? std::clamp(scale, 0.67F, 1.5F) : 1.0F;
    m->resize();
  }
  float Window::uiScale() const { return m->uiScale; }
  int Window::width() const { return m->target.logicalWidth(); }
  int Window::height() const { return m->target.logicalHeight(); }
  RenderContext& Window::context() { return m->context; }
  RenderTarget& Window::target() { return m->target; }
  GlSharedContext& Window::graphics() { return m->shared; }
} // namespace app
