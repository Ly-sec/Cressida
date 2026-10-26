#include "text-input-unstable-v3-client-protocol.h"
#include "text-input-unstable-v3-server-protocol.h"
#include "wayland/text_input_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>
#include <wayland-client.h>
#include <wayland-server.h>

namespace {
  void require(bool value, const char* reason) {
    if (!value)
      throw std::runtime_error(reason);
  }
  struct Snapshot {
    std::uint32_t commits = 0, enables = 0, disables = 0, surroundingRequests = 0, surfaceCommits = 0;
    std::string surrounding;
    std::int32_t cursor = 0, anchor = 0;
    std::array<std::int32_t, 4> rectangle{};
    std::uint32_t hint = 0, purpose = 0;
  };
  class Compositor {
    using Event = std::function<void(wl_resource*, wl_resource*, std::uint32_t)>;
    wl_display* m_display = wl_display_create();
    wl_resource* m_textInput = nullptr;
    wl_resource* m_surface = nullptr;
    std::mutex m_mutex;
    Snapshot m_state;
    std::vector<Event> m_events;
    std::jthread m_worker;
    static void destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
    static Compositor& owner(wl_resource* resource) {
      return *static_cast<Compositor*>(wl_resource_get_user_data(resource));
    }

  public:
    int clientFd = -1;
    explicit Compositor(std::uint32_t version) {
      require(m_display, "Create an isolated text-input compositor");
      wl_global_create(
          m_display, &wl_compositor_interface, 1, this,
          [](wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
            auto* resource = wl_resource_create(client, &wl_compositor_interface, version, id);
            static const struct wl_compositor_interface implementation{
                .create_surface = [](wl_client* client, wl_resource* compositor, std::uint32_t id) {
                  auto& self = owner(compositor);
                  self.m_surface = wl_resource_create(client, &wl_surface_interface, 1, id);
                  static const struct wl_surface_interface implementation{
                      .destroy = destroy, .commit = [](wl_client*, wl_resource* resource) {
                        auto& self = owner(resource);
                        std::lock_guard lock(self.m_mutex);
                        ++self.m_state.surfaceCommits;
                      }
                  };
                  wl_resource_set_implementation(self.m_surface, &implementation, &self, nullptr);
                }
            };
            wl_resource_set_implementation(resource, &implementation, data, nullptr);
          }
      );
      wl_global_create(
          m_display, &wl_seat_interface, 1, this,
          [](wl_client* client, void*, std::uint32_t version, std::uint32_t id) {
            auto* resource = wl_resource_create(client, &wl_seat_interface, version, id);
            wl_resource_set_implementation(resource, nullptr, nullptr, nullptr);
          }
      );
      wl_global_create(
          m_display, &zwp_text_input_manager_v3_interface, version, this,
          [](wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
            auto* resource = wl_resource_create(client, &zwp_text_input_manager_v3_interface, version, id);
            static const struct zwp_text_input_manager_v3_interface implementation{
                .destroy = destroy,
                .get_text_input = [](wl_client* client, wl_resource* manager, std::uint32_t id, wl_resource*) {
                  auto& self = owner(manager);
                  self.m_textInput =
                      wl_resource_create(client, &zwp_text_input_v3_interface, wl_resource_get_version(manager), id);
                  static const struct zwp_text_input_v3_interface implementation{
                      .destroy = destroy,
                      .enable =
                          [](wl_client*, wl_resource* resource) {
                            auto& self = owner(resource);
                            std::lock_guard lock(self.m_mutex);
                            ++self.m_state.enables;
                            self.m_state.surrounding.clear();
                            self.m_state.cursor = self.m_state.anchor = 0;
                          },
                      .disable =
                          [](wl_client*, wl_resource* resource) {
                            auto& self = owner(resource);
                            std::lock_guard lock(self.m_mutex);
                            ++self.m_state.disables;
                          },
                      .set_surrounding_text =
                          [](wl_client*, wl_resource* resource, const char* text, std::int32_t cursor,
                             std::int32_t anchor) {
                            auto& self = owner(resource);
                            std::lock_guard lock(self.m_mutex);
                            ++self.m_state.surroundingRequests;
                            self.m_state.surrounding = text;
                            self.m_state.cursor = cursor;
                            self.m_state.anchor = anchor;
                          },
                      .set_text_change_cause = [](wl_client*, wl_resource*, std::uint32_t) {},
                      .set_content_type =
                          [](wl_client*, wl_resource* resource, std::uint32_t hint, std::uint32_t purpose) {
                            auto& self = owner(resource);
                            std::lock_guard lock(self.m_mutex);
                            self.m_state.hint = hint;
                            self.m_state.purpose = purpose;
                          },
                      .set_cursor_rectangle =
                          [](wl_client*, wl_resource* resource, std::int32_t x, std::int32_t y, std::int32_t width,
                             std::int32_t height) {
                            auto& self = owner(resource);
                            std::lock_guard lock(self.m_mutex);
                            self.m_state.rectangle = {x, y, width, height};
                          },
                      .commit =
                          [](wl_client*, wl_resource* resource) {
                            auto& self = owner(resource);
                            std::lock_guard lock(self.m_mutex);
                            ++self.m_state.commits;
                          }
                  };
                  wl_resource_set_implementation(self.m_textInput, &implementation, &self, [](wl_resource* resource) {
                    owner(resource).m_textInput = nullptr;
                  });
                }
            };
            wl_resource_set_implementation(resource, &implementation, data, nullptr);
          }
      );
      int pair[2];
      require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "Create a private protocol socket pair");
      require(wl_client_create(m_display, pair[0]), "Accept the private protocol client");
      clientFd = pair[1];
      m_worker = std::jthread([this](std::stop_token stop) {
        auto* loop = wl_display_get_event_loop(m_display);
        while (!stop.stop_requested()) {
          wl_event_loop_dispatch(loop, 10);
          std::vector<Event> events;
          std::uint32_t serial;
          {
            std::lock_guard lock(m_mutex);
            events.swap(m_events);
            serial = m_state.commits;
          }
          for (auto& event : events)
            if (m_textInput)
              event(m_textInput, m_surface, serial);
          wl_display_flush_clients(m_display);
        }
      });
    }
    ~Compositor() {
      m_worker.request_stop();
      m_worker.join();
      wl_display_destroy_clients(m_display);
      wl_display_destroy(m_display);
    }
    void emit(Event event) {
      std::lock_guard lock(m_mutex);
      m_events.push_back(std::move(event));
    }
    Snapshot snapshot() {
      std::lock_guard lock(m_mutex);
      return m_state;
    }
  };
  struct Client final : TextInputClient {
    TextInputState state;
    TextInputService* service = nullptr;
    std::vector<TextInputEdit> edits;
    std::function<void(const TextInputEdit&)> applied;
    unsigned resets = 0;
    TextInputState textInputState() const override { return state; }
    void textInputApplyEdit(const TextInputEdit& edit) override {
      require(service && service->hasPendingEdit(), "The batch remains guarded during reentrant control callbacks");
      edits.push_back(edit);
      if (applied)
        applied(edit);
    }
    void textInputResetPreedit() override { ++resets; }
    void textInputActivated(TextInputService& active) override { service = &active; }
    void textInputDeactivated(TextInputService&) override { service = nullptr; }
  };
} // namespace

int main() {
  try {
    for (const auto version : {1U, 2U}) {
      Compositor server(version);
      auto* display = wl_display_connect_to_fd(server.clientFd);
      require(display, "Connect without using a desktop or graphics device");
      struct Globals {
        wl_compositor* compositor = nullptr;
        wl_seat* seat = nullptr;
        zwp_text_input_manager_v3* manager = nullptr;
      } globals;
      auto* registry = wl_display_get_registry(display);
      const wl_registry_listener listener{
          .global =
              [](void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
                auto& globals = *static_cast<Globals*>(data);
                if (std::strcmp(interface, "wl_compositor") == 0)
                  globals.compositor =
                      static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
                else if (std::strcmp(interface, "wl_seat") == 0)
                  globals.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
                else if (std::strcmp(interface, "zwp_text_input_manager_v3") == 0)
                  globals.manager = static_cast<zwp_text_input_manager_v3*>(
                      wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, std::min(version, 2U))
                  );
              },
          .global_remove = [](void*, wl_registry*, std::uint32_t) {}
      };
      wl_registry_add_listener(registry, &listener, &globals);
      const auto pump = [&] {
        require(
            wl_display_roundtrip(display) >= 0 && wl_display_roundtrip(display) >= 0,
            "Dispatch private protocol messages"
        );
      };
      pump();
      require(globals.compositor && globals.seat && globals.manager, "Bind the isolated compositor globals");
      auto* surface = wl_compositor_create_surface(globals.compositor);
      {
        Client first, password, longText;
        first.state = {
            .surroundingText = "abc",
            .cursor = 3,
            .anchor = 3,
            .cursorRectX = 10,
            .cursorRectY = 20,
            .cursorRectWidth = 2,
            .cursorRectHeight = 18,
            .multiline = true
        };
        TextInputService service;
        unsigned changes = 0;
        service.setStateChanged([&] { ++changes; });
        require(
            !service.bind(nullptr, globals.seat) && !service.isAvailable(),
            "An absent manager remains a normal fallback"
        );
        require(service.bind(globals.manager, globals.seat), "Create the native text-input object");
        service.setFocusedClient(surface, &first);
        service.onKeyboardFocusSurface(surface, true);
        pump();
        require(
            !service.isEnabled() && server.snapshot().enables == 0,
            "Keyboard focus alone cannot enable compositor text input"
        );
        server.emit([](auto* input, auto* surface, auto) { zwp_text_input_v3_send_enter(input, surface); });
        pump();
        require(
            service.isEnabled() && server.snapshot().surrounding == "abc",
            "A text-input enter enables the focused editor and sends its state"
        );
        require(
            (server.snapshot().hint & ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE) != 0,
            "A multiline editor advertises the standard content hint on both protocol versions"
        );
        const auto initial = server.snapshot().commits;
        service.setFocusedClient(surface, &first);
        service.notifyClientStateChanged(&first, TextInputChangeCause::Other);
        pump();
        require(
            server.snapshot().commits == initial,
            "Repeated scene focus synchronization does not create commit feedback loops"
        );
        first.state.multiline = false;
        service.notifyClientStateChanged(&first, TextInputChangeCause::Other);
        pump();
        require(
            server.snapshot().commits == initial + 1
                && (server.snapshot().hint & ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE) == 0,
            "Changing multiline state refreshes the content hint instead of deduplicating it away"
        );
        service.setCoordinateScale(1.5F);
        pump();
        require(
            server.snapshot().rectangle == std::array<std::int32_t, 4>{15, 30, 3, 27},
            "Candidate coordinates follow UI zoom in surface logical units"
        );
        server.emit([](auto* input, auto*, auto) { zwp_text_input_v3_send_preedit_string(input, "as", 2, 2); });
        pump();
        require(
            service.hasPendingEdit() && !service.hasComposition() && first.edits.empty(),
            "Preedit events remain buffered until done"
        );
        server.emit([](auto* input, auto*, auto serial) { zwp_text_input_v3_send_done(input, serial); });
        pump();
        require(
            service.hasComposition() && !service.hasPendingEdit() && first.edits.back().preeditText == "as",
            "Done applies preedit once and releases the pending-key guard"
        );
        server.emit([](auto* input, auto*, auto serial) { zwp_text_input_v3_send_done(input, serial); });
        pump();
        require(
            !service.hasComposition() && !first.edits.back().hasPreedit,
            "A batch without fresh preedit clears the previous composition"
        );

        const auto stale = server.snapshot().commits;
        first.state.surroundingText = "abcd";
        first.state.cursor = first.state.anchor = 4;
        service.notifyClientStateChanged(&first, TextInputChangeCause::Other);
        pump();
        const auto current = server.snapshot().commits;
        first.applied = [&](const TextInputEdit& edit) {
          if (edit.hasCommitText) {
            first.state.surroundingText += edit.commitText;
            first.state.cursor = first.state.anchor = first.state.surroundingText.size();
            service.notifyClientStateChanged(&first, TextInputChangeCause::Other);
          }
        };
        server.emit([stale](auto* input, auto*, auto) {
          zwp_text_input_v3_send_commit_string(input, "á");
          zwp_text_input_v3_send_done(input, stale);
        });
        pump();
        require(
            first.state.surroundingText == "abcdá" && server.snapshot().commits == current,
            "A stale serial still applies committed text while deferring reentrant protocol changes"
        );
        service.notifyClientStateChanged(&first, TextInputChangeCause::Other);
        pump();
        require(server.snapshot().commits == current, "Deferred state stays pending until a matching done serial");
        server.emit([](auto* input, auto*, auto serial) { zwp_text_input_v3_send_done(input, serial); });
        pump();
        require(
            server.snapshot().surrounding == "abcdá" && server.snapshot().commits == current + 1,
            "A matching serial publishes the updated surrounding state once"
        );

        const auto retired = server.snapshot().commits;
        const auto generation = service.focusGeneration();
        password.state = {
            .surroundingText = {},
            .purpose = TextInputPurpose::Password,
            .sendSurroundingText = false,
            .sensitiveData = true,
            .hiddenText = true,
            .preeditVisible = false
        };
        const auto surrounds = server.snapshot().surroundingRequests;
        service.setFocusedClient(surface, &password);
        pump();
        require(
            service.focusGeneration() != generation && !first.service && password.service == &service,
            "Switching editors invalidates the old focus before new callbacks"
        );
        require(
            server.snapshot().surroundingRequests == surrounds
                && server.snapshot().surrounding.empty()
                && server.snapshot().purpose == ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD,
            "Password focus resets old context without disclosing surrounding text"
        );
        server.emit([retired](auto* input, auto*, auto) {
          zwp_text_input_v3_send_commit_string(input, "old editor");
          zwp_text_input_v3_send_done(input, retired);
        });
        pump();
        require(password.edits.empty(), "A retired editor's delayed batch cannot write into the newly focused field");

        for (unsigned index = 0; index < 3000; ++index)
          longText.state.surroundingText += "á";
        longText.state.cursor = 4000;
        longText.state.anchor = 4002;
        service.setFocusedClient(surface, &longText);
        pump();
        const auto surrounding = server.snapshot();
        require(
            surrounding.surrounding.size() <= 4000
                && surrounding.cursor >= 0
                && surrounding.anchor - surrounding.cursor == 2
                && surrounding.surrounding.substr(surrounding.cursor, 2) == "á",
            "Long drafts send a bounded UTF-8 window retaining the complete selection and adjusted byte offsets"
        );
        server.emit([](auto* input, auto*, auto serial) {
          zwp_text_input_v3_send_delete_surrounding_text(input, 2, 4);
          zwp_text_input_v3_send_commit_string(input, "ê");
          zwp_text_input_v3_send_preedit_string(input, "ới", -1, -1);
          zwp_text_input_v3_send_done(input, serial);
        });
        pump();
        require(
            longText.edits.size() == 1
                && longText.edits.back().hasDelete
                && longText.edits.back().deleteBeforeLength == 2
                && longText.edits.back().deleteAfterLength == 4
                && longText.edits.back().commitText == "ê"
                && longText.edits.back().preeditCursorBegin == -1
                && service.hasComposition(),
            "Deletion, commit and hidden-cursor preedit arrive in one ordered native edit batch"
        );
        server.emit([](auto* input, auto*, auto) { zwp_text_input_v3_send_commit_string(input, "late"); });
        pump();
        require(service.hasPendingEdit(), "A final commit is pending before keyboard focus is lost");
        const auto beforeLeave = service.focusGeneration();
        service.onKeyboardFocusSurface(surface, false);
        pump();
        require(
            !service.isEnabled()
                && !service.hasPendingEdit()
                && !service.hasComposition()
                && service.focusGeneration() != beforeLeave,
            "Keyboard focus loss invalidates buffered edits and cancels composition immediately"
        );
        server.emit([](auto* input, auto* surface, auto serial) {
          zwp_text_input_v3_send_done(input, serial);
          zwp_text_input_v3_send_leave(input, surface);
        });
        pump();
        require(longText.edits.size() == 1, "Late done after blur cannot apply stale text");
        require(server.snapshot().surfaceCommits == 0, "IME state never commits unrelated window buffers");
        require(changes > 0, "Native batch and lifecycle changes notify the owning loop");
        service.cleanup();
        pump();
        require(!service.isAvailable() && !longText.service, "Manager cleanup detaches the focused control");
      }
      wl_surface_destroy(surface);
      zwp_text_input_manager_v3_destroy(globals.manager);
      wl_seat_destroy(globals.seat);
      wl_compositor_destroy(globals.compositor);
      wl_registry_destroy(registry);
      wl_display_disconnect(display);
    }
    std::cout << "Native text-input protocol, focus, serial, composition and surrounding checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
