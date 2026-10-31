#include "capture/screen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <iostream>
#include <map>
#include <mutex>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace {
  using namespace std::chrono_literals;
  constexpr std::uint32_t hiddenCursor = 1, embeddedCursor = 2, metadataCursor = 4, legacyCursorModes = 0;
  constexpr auto revocationDeadline = 1s;
  constexpr auto lateResponseSettle = 100ms;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  template <class Predicate> void until(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!predicate()) {
      require(std::chrono::steady_clock::now() < deadline, message);
      while (g_main_context_iteration(nullptr, false)) {
      }
      poll(nullptr, 0, 2);
    }
  }
  struct VideoServer {
    pw_thread_loop* loop = pw_thread_loop_new("screen-fixture", nullptr);
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;
    spa_hook listener{};
    std::atomic<std::uint32_t> node = PW_ID_ANY;
    std::atomic_bool allowNewLinks = true;
    std::atomic_uint blockedInputs = 0;
    struct Port {
      std::uint32_t node;
      bool input;
    };
    std::map<std::uint32_t, Port> ports;
    std::set<std::pair<std::uint32_t, std::uint32_t>> linked;
    std::vector<pw_proxy*> links;
    VideoServer() {
      context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
      core = pw_context_connect(context, nullptr, 0);
      require(core, "The fixture connects only to its private video server");
      registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
      static const pw_registry_events events{
          .version = PW_VERSION_REGISTRY_EVENTS,
          .global =
              [](void* data, std::uint32_t id, std::uint32_t, const char* type, std::uint32_t,
                 const spa_dict* properties) {
                auto& self = *static_cast<VideoServer*>(data);
                if (!properties)
                  return;
                if (!std::strcmp(type, PW_TYPE_INTERFACE_Node)) {
                  const auto* name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
                  if (name && !std::strcmp(name, "synthetic-screen"))
                    self.node = id;
                } else if (!std::strcmp(type, PW_TYPE_INTERFACE_Port)) {
                  const auto* owner = spa_dict_lookup(properties, PW_KEY_NODE_ID);
                  const auto* direction = spa_dict_lookup(properties, PW_KEY_PORT_DIRECTION);
                  if (owner && direction)
                    self.ports[id] = {static_cast<std::uint32_t>(std::stoul(owner)), !std::strcmp(direction, "in")};
                  if (direction && !std::strcmp(direction, "in") && !self.allowNewLinks)
                    ++self.blockedInputs;
                }
                if (!self.allowNewLinks)
                  return;
                for (const auto& [output, source] : self.ports)
                  for (const auto& [input, sink] : self.ports) {
                    if (source.input
                        || source.node != self.node
                        || !sink.input
                        || !self.linked.emplace(output, input).second)
                      continue;
                    auto* props = pw_properties_new(
                        PW_KEY_LINK_OUTPUT_NODE, std::to_string(source.node).c_str(), PW_KEY_LINK_OUTPUT_PORT,
                        std::to_string(output).c_str(), PW_KEY_LINK_INPUT_NODE, std::to_string(sink.node).c_str(),
                        PW_KEY_LINK_INPUT_PORT, std::to_string(input).c_str(), nullptr
                    );
                    auto* link = static_cast<pw_proxy*>(pw_core_create_object(
                        self.core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0
                    ));
                    pw_properties_free(props);
                    if (link)
                      self.links.push_back(link);
                  }
              },
          .global_remove =
              [](void* data, std::uint32_t id) {
                auto& self = *static_cast<VideoServer*>(data);
                self.ports.erase(id);
                std::erase_if(self.linked, [id](const auto& link) { return link.first == id || link.second == id; });
              }
      };
      pw_registry_add_listener(registry, &listener, &events, this);
      require(pw_thread_loop_start(loop) == 0, "The private video graph runs");
      until([&] { return node != PW_ID_ANY; }, "A synthetic source is exported without a desktop");
    }
    ~VideoServer() {
      pw_thread_loop_stop(loop);
      for (auto* link : links)
        pw_proxy_destroy(link);
      spa_hook_remove(&listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
      pw_core_disconnect(core);
      pw_context_destroy(context);
      pw_thread_loop_destroy(loop);
    }
  };
  struct Portal {
    GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    GDBusConnection* connection = nullptr;
    GDBusNodeInfo* info = nullptr;
    guint object = 0;
    std::vector<guint> sessions;
    std::vector<guint> requests;
    std::set<std::string> closedSessions;
    std::string latestSession;
    std::string heldRequest, heldSender;
    GVariant* heldResults = nullptr;
    std::string socketPath;
    std::uint32_t node;
    std::uint32_t cursorModes = hiddenCursor | embeddedCursor;
    unsigned creates = 0, selects = 0, starts = 0, opens = 0, closes = 0, requestCloses = 0;
    bool cancel = false, invalid = false, holdStart = false;
    explicit Portal(std::uint32_t source) : node(source) {
      const auto* runtime = std::getenv("XDG_RUNTIME_DIR");
      require(runtime, "The fixture supplies a private runtime directory");
      socketPath = std::string(runtime) + "/cressida-screen-test";
      g_test_dbus_up(bus);
      connection = g_dbus_connection_new_for_address_sync(
          g_test_dbus_get_bus_address(bus),
          GDBusConnectionFlags(
              G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
          ),
          nullptr, nullptr, nullptr
      );
      require(connection, "The capture portal has its own private bus");
      info = g_dbus_node_info_new_for_xml(
          R"XML(<node>
        <interface name="org.freedesktop.portal.ScreenCast">
          <property name="AvailableSourceTypes" type="u" access="read"/>
          <property name="AvailableCursorModes" type="u" access="read"/>
          <method name="CreateSession"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
          <method name="SelectSources"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
          <method name="Start"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
          <method name="OpenPipeWireRemote"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="h" direction="out"/></method>
        </interface>
        <interface name="org.freedesktop.portal.Session"><method name="Close"/><signal name="Closed"/></interface>
        <interface name="org.freedesktop.portal.Request"><method name="Close"/><signal name="Response"><arg type="u"/><arg type="a{sv}"/></signal></interface>
      </node>)XML",
          nullptr
      );
      static const GDBusInterfaceVTable handlers{
          .method_call = [](GDBusConnection*, const gchar* sender, const gchar*, const gchar*, const gchar* method,
                            GVariant* parameters, GDBusMethodInvocation* call,
                            gpointer data) { static_cast<Portal*>(data)->method(sender, method, parameters, call); },
          .get_property = [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* property,
                             GError**, gpointer data) -> GVariant* {
            return g_variant_new_uint32(
                std::string_view(property) == "AvailableCursorModes" ? static_cast<Portal*>(data)->cursorModes : 3
            );
          }
      };
      object = g_dbus_connection_register_object(
          connection, "/org/freedesktop/portal/desktop", info->interfaces[0], &handlers, this, nullptr, nullptr
      );
      require(object, "The fake capture portal exports the real interface");
      auto* reply = g_dbus_connection_call_sync(
          connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
          g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0U), nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr,
          nullptr
      );
      require(reply, "The private portal owns its bus name");
      g_variant_unref(reply);
    }
    ~Portal() {
      if (heldResults)
        g_variant_unref(heldResults);
      for (auto request : requests)
        g_dbus_connection_unregister_object(connection, request);
      for (auto session : sessions)
        g_dbus_connection_unregister_object(connection, session);
      g_dbus_connection_unregister_object(connection, object);
      g_dbus_node_info_unref(info);
      g_dbus_connection_close_sync(connection, nullptr, nullptr);
      g_object_unref(connection);
      g_test_dbus_down(bus);
      g_object_unref(bus);
    }
    void finishHeld(bool cancelled) {
      require(heldResults, "A chooser response was held for explicit completion");
      g_dbus_connection_emit_signal(
          connection, heldSender.c_str(), heldRequest.c_str(), "org.freedesktop.portal.Request", "Response",
          g_variant_new("(u@a{sv})", cancelled ? 1U : 0U, std::exchange(heldResults, nullptr)), nullptr
      );
      heldRequest.clear();
      heldSender.clear();
    }
    void method(const char* sender, std::string_view method, GVariant* parameters, GDBusMethodInvocation* call) {
      if (method == "OpenPipeWireRemote") {
        ++opens;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto& path = socketPath;
        require(path.size() < sizeof(address.sun_path), "The private socket path fits");
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        const auto fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(
            fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "The portal returns only a synthetic video remote"
        );
        auto* descriptors = g_unix_fd_list_new();
        const auto index = g_unix_fd_list_append(descriptors, fd, nullptr);
        close(fd);
        g_dbus_method_invocation_return_value_with_unix_fd_list(call, g_variant_new("(h)", index), descriptors);
        g_object_unref(descriptors);
        return;
      }
      const auto count = g_variant_n_children(parameters);
      auto* options = g_variant_get_child_value(parameters, count - 1);
      const char* token = nullptr;
      g_variant_lookup(options, "handle_token", "&s", &token);
      require(token, "Every source request has a bounded distinct handle");
      std::string owner(sender);
      owner.erase(0, 1);
      std::ranges::replace(owner, '.', '_');
      const auto request = "/org/freedesktop/portal/desktop/request/" + owner + "/" + token;
      GVariantBuilder results;
      g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
      if (method == "CreateSession") {
        ++creates;
        const char* sessionToken = nullptr;
        g_variant_lookup(options, "session_handle_token", "&s", &sessionToken);
        require(sessionToken, "Source sessions use a fresh handle");
        latestSession = "/org/freedesktop/portal/desktop/session/" + owner + "/" + sessionToken;
        static const GDBusInterfaceVTable sessionHandlers{
            .method_call = [](GDBusConnection*, const gchar*, const gchar* path, const gchar*, const gchar*, GVariant*,
                              GDBusMethodInvocation* call, gpointer data) {
              auto& self = *static_cast<Portal*>(data);
              ++self.closes;
              self.closedSessions.emplace(path);
              g_dbus_method_invocation_return_value(call, nullptr);
            }
        };
        sessions.push_back(g_dbus_connection_register_object(
            connection, latestSession.c_str(), info->interfaces[1], &sessionHandlers, this, nullptr, nullptr
        ));
        g_variant_builder_add(
            &results, "{sv}", "session_handle",
            g_variant_new_string(invalid ? "invalid-session" : latestSession.c_str())
        );
      } else if (method == "SelectSources") {
        ++selects;
        guint types = 0, cursor = 0;
        gboolean multiple = true;
        require(
            g_variant_lookup(options, "types", "u", &types) && types == 3, "The selector offers screens and windows"
        );
        require(
            g_variant_lookup(options, "multiple", "b", &multiple) && !multiple,
            "Exactly one consented source is selected"
        );
        const bool hasCursor = g_variant_lookup(options, "cursor_mode", "u", &cursor);
        require(
            cursorModes ? hasCursor
                    && (cursor == hiddenCursor || cursor == embeddedCursor || cursor == metadataCursor)
                    && (cursorModes & cursor)
                        : !hasCursor,
            "Cursor selection follows supported modes and omits unavailable legacy options"
        );
      } else if (method == "Start") {
        ++starts;
        GVariantBuilder streams, attributes;
        g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
        g_variant_builder_init(&attributes, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&attributes, "{sv}", "source_type", g_variant_new_uint32(2));
        g_variant_builder_add(&streams, "(u@a{sv})", node, g_variant_builder_end(&attributes));
        g_variant_builder_add(&results, "{sv}", "streams", g_variant_builder_end(&streams));
      }
      g_variant_unref(options);
      g_dbus_method_invocation_return_value(call, g_variant_new("(o)", request.c_str()));
      if (method == "Start" && holdStart) {
        require(!heldResults, "The fixture holds only one chooser at a time");
        heldRequest = request;
        heldSender = sender;
        heldResults = g_variant_ref_sink(g_variant_builder_end(&results));
        static const GDBusInterfaceVTable requestHandlers{
            .method_call = [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*,
                              GDBusMethodInvocation* call, gpointer data) {
              ++static_cast<Portal*>(data)->requestCloses;
              g_dbus_method_invocation_return_value(call, nullptr);
            }
        };
        requests.push_back(g_dbus_connection_register_object(
            connection, heldRequest.c_str(), info->interfaces[2], &requestHandlers, this, nullptr, nullptr
        ));
        return;
      }
      g_dbus_connection_emit_signal(
          connection, sender, request.c_str(), "org.freedesktop.portal.Request", "Response",
          g_variant_new("(u@a{sv})", cancel && method == "Start" ? 1U : 0U, g_variant_builder_end(&results)), nullptr
      );
    }
  };
} // namespace
int main() {
  try {
    pw_init(nullptr, nullptr);
    VideoServer video;
    Portal portal(video.node);
    std::atomic<unsigned> frames = 0;
    std::mutex mutex;
    std::shared_ptr<const voice::VideoFrame> latest;
    {
      capture::Screen screen([&](auto frame) {
        std::lock_guard lock(mutex);
        latest = std::move(frame);
        ++frames;
      });
      require(portal.creates == 0 && frames == 0, "Construction does not request screen access");
      screen.select("", true);
      until(
          [&] {
            return screen.state().phase == capture::ScreenPhase::Ready
                || screen.state().phase == capture::ScreenPhase::Failed;
          },
          "Consented capture settles"
      );
      require(screen.state().phase == capture::ScreenPhase::Ready, "The real portal response produces a first frame");
      {
        std::lock_guard lock(mutex);
        require(latest && latest->valid(), "PipeWire delivers real valid native RGBA pixels");
        const auto now =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        require(
            std::abs(latest->timestamp - now) < std::chrono::duration_cast<std::chrono::microseconds>(2s).count(),
            "Captured video uses the playback synchronization clock"
        );
      }
      require(
          portal.creates == 1 && portal.selects == 1 && portal.starts == 1 && portal.opens == 1,
          "Capture completes the full consent and descriptor exchange"
      );
      const auto before = frames.load();
      portal.cancel = true;
      screen.select("", false);
      until(
          [&] { return portal.starts == 2 && screen.state().phase == capture::ScreenPhase::Ready; },
          "Cancelling replacement preserves the active source"
      );
      until([&] { return frames > before; }, "The previous source keeps producing pixels after cancellation");
      portal.cancel = false;
      screen.select("", false);
      until(
          [&] {
            return portal.opens == 2 && screen.state().phase == capture::ScreenPhase::Ready && !screen.state().cursor;
          },
          "A replacement waits for valid new pixels"
      );
      g_dbus_connection_emit_signal(
          portal.connection, nullptr, portal.latestSession.c_str(), "org.freedesktop.portal.Session", "Closed",
          g_variant_new("()"), nullptr
      );
      until(
          [&] { return screen.state().phase == capture::ScreenPhase::Failed; },
          "Desktop revocation stops native capture"
      );
      screen.stop();
      require(screen.state().phase == capture::ScreenPhase::Idle, "Explicit stop resets capture immediately");
      portal.invalid = true;
      screen.select("", true);
      until(
          [&] { return screen.state().phase == capture::ScreenPhase::Failed; },
          "Malformed session handles fail without reaching PipeWire"
      );
      require(portal.opens == 2, "Invalid handles never open a media remote");
      portal.invalid = false;
      for (const auto modes : {hiddenCursor, metadataCursor, legacyCursorModes}) {
        portal.cursorModes = modes;
        const auto opened = portal.opens;
        const auto created = portal.creates;
        screen.select("", true);
        until(
            [&] {
              const auto state = screen.state();
              if (portal.creates > created && state.phase == capture::ScreenPhase::Failed)
                throw std::runtime_error("Cursor fallback " + std::to_string(modes) + ": " + state.error);
              return portal.opens == opened + 1 && state.phase == capture::ScreenPhase::Ready;
            },
            "Unsupported embedded cursors retain valid capture"
        );
        require(
            !screen.state().cursor && !screen.state().error.empty(),
            "A fallback cursor mode reports that the shared frames omit the cursor"
        );
      }
      const auto activeSession = portal.latestSession;
      const auto opensBefore = portal.opens;
      portal.holdStart = true;
      screen.select("", true);
      until(
          [&] { return !portal.heldRequest.empty() && screen.state().phase == capture::ScreenPhase::Selecting; },
          "Source replacement can remain in the desktop chooser"
      );
      require(portal.latestSession != activeSession, "Replacement has its own consent session");
      const auto replacementSession = portal.latestSession;
      const auto revokedAt = std::chrono::steady_clock::now();
      g_dbus_connection_emit_signal(
          portal.connection, nullptr, activeSession.c_str(), "org.freedesktop.portal.Session", "Closed",
          g_variant_new("()"), nullptr
      );
      until(
          [&] { return screen.state().phase == capture::ScreenPhase::Failed; },
          "Revoking the active source fails immediately while a chooser is pending"
      );
      require(
          std::chrono::steady_clock::now() - revokedAt < revocationDeadline,
          "Desktop revocation does not wait for the pending chooser response"
      );
      until(
          [&] {
            return portal.requestCloses == 1
                && portal.closedSessions.contains(activeSession)
                && portal.closedSessions.contains(replacementSession);
          },
          "Revocation retires the old capture and cancels the pending request and session"
      );
      const auto afterRevocation = frames.load();
      portal.finishHeld(true);
      screen.cancelSelection();
      const auto settledAt = std::chrono::steady_clock::now();
      until(
          [&] { return std::chrono::steady_clock::now() - settledAt >= lateResponseSettle; },
          "A late chooser cancellation is delivered after revocation"
      );
      require(
          screen.state().phase == capture::ScreenPhase::Failed && frames == afterRevocation,
          "Late cancellation cannot restore a revoked source or deliver stale pixels"
      );
      require(portal.opens == opensBefore, "The revoked replacement never opens a new media remote");
      screen.stop();
      portal.holdStart = false;
      screen.select("", true);
      until(
          [&] { return screen.state().phase == capture::ScreenPhase::Ready; },
          "Capture can be started explicitly after desktop revocation"
      );
      const auto priorSession = portal.latestSession;
      video.allowNewLinks = false;
      auto blockedBefore = video.blockedInputs.load();
      screen.select("", true);
      until(
          [&] { return video.blockedInputs > blockedBefore; },
          "Replacement can wait for its first valid frame while the old source remains active"
      );
      const auto waitingSession = portal.latestSession;
      const auto revokedBeforeFrame = std::chrono::steady_clock::now();
      g_dbus_connection_emit_signal(
          portal.connection, nullptr, priorSession.c_str(), "org.freedesktop.portal.Session", "Closed",
          g_variant_new("()"), nullptr
      );
      until(
          [&] { return screen.state().phase == capture::ScreenPhase::Failed; },
          "Revocation interrupts a replacement waiting for its first frame"
      );
      require(
          std::chrono::steady_clock::now() - revokedBeforeFrame < revocationDeadline,
          "Revocation is processed before the first-frame deadline"
      );
      until(
          [&] {
            return portal.closedSessions.contains(priorSession) && portal.closedSessions.contains(waitingSession);
          },
          "Both sources retire after revocation during first-frame preparation"
      );
      screen.stop();
      blockedBefore = video.blockedInputs.load();
      screen.select("", true);
      until([&] { return video.blockedInputs > blockedBefore; }, "Initial capture can wait for its first frame");
      const auto initialSession = portal.latestSession;
      const auto revokedInitial = std::chrono::steady_clock::now();
      g_dbus_connection_emit_signal(
          portal.connection, nullptr, initialSession.c_str(), "org.freedesktop.portal.Session", "Closed",
          g_variant_new("()"), nullptr
      );
      until(
          [&] { return screen.state().phase == capture::ScreenPhase::Failed; },
          "Revoking a source before its first frame never installs it"
      );
      require(
          std::chrono::steady_clock::now() - revokedInitial < revocationDeadline,
          "Initial first-frame preparation processes desktop revocation promptly"
      );
      until(
          [&] { return portal.closedSessions.contains(initialSession); },
          "A revoked source is released without waiting for a frame"
      );
      screen.stop();
    }
    std::cout << "Private portal consent, pixels, replacement and revocation passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
