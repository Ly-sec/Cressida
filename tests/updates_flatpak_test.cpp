#include "updates/flatpak.h"

#include <atomic>
#include <chrono>
#include <future>
#include <gio/gio.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <thread>

namespace {
  constexpr const char* service = "org.freedesktop.portal.Flatpak";
  constexpr const char* path = "/org/freedesktop/portal/Flatpak";
  constexpr const char* monitor = "/org/freedesktop/portal/Flatpak/update_monitor/fixture";
  constexpr const char* monitorType = "org.freedesktop.portal.Flatpak.UpdateMonitor";
  constexpr auto deadline = std::chrono::seconds{5};
  void require(bool value, const char* reason) {
    if (!value)
      throw std::runtime_error(reason);
  }
  updates::Snapshot wait(updates::Updater& updater, updates::Phase phase) {
    const auto end = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < end) {
      if (auto state = updater.poll()) {
        if (state->phase == phase)
          return *state;
        if (state->phase == updates::Phase::Error)
          throw std::runtime_error(state->error);
      }
      pollfd wake{updater.eventFd(), POLLIN, 0};
      ::poll(&wake, 1, 50);
    }
    throw std::runtime_error("Flatpak fixture timed out");
  }
  struct Peer {
    GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    GMainLoop* loop = nullptr;
    GMainContext* context = nullptr;
    std::atomic_bool available = true, failCheck = false, failUpdate = false, silent = false;
    std::atomic_bool created = false, spawned = false, spawnValid = false;
    std::atomic<unsigned> installs = 0;
    std::promise<void> ready;
    std::jthread worker;
    Peer() {
      g_test_dbus_up(bus);
      worker = std::jthread([this] {
        context = g_main_context_new();
        g_main_context_push_thread_default(context);
        loop = g_main_loop_new(context, false);
        auto* connection = g_dbus_connection_new_for_address_sync(
            g_test_dbus_get_bus_address(bus),
            static_cast<GDBusConnectionFlags>(
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
            ),
            nullptr, nullptr, nullptr
        );
        if (!connection) {
          ready.set_exception(std::make_exception_ptr(std::runtime_error("Private bus connection")));
          return;
        }
        auto* name = g_dbus_connection_call_sync(
            connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
            g_variant_new("(su)", service, 0U), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr
        );
        if (!name) {
          ready.set_exception(std::make_exception_ptr(std::runtime_error("Private portal name")));
          return;
        }
        g_variant_unref(name);
        constexpr const char* xml = R"(<node>
          <interface name="org.freedesktop.portal.Flatpak">
            <method name="CreateUpdateMonitor"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
            <method name="Spawn"><arg type="ay" direction="in"/><arg type="aay" direction="in"/><arg type="a{uh}" direction="in"/>
              <arg type="a{ss}" direction="in"/><arg type="u" direction="in"/><arg type="a{sv}" direction="in"/><arg type="u" direction="out"/></method>
          </interface>
          <interface name="org.freedesktop.portal.Flatpak.UpdateMonitor">
            <method name="Close"/>
            <method name="Update"><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/></method>
            <signal name="UpdateAvailable"><arg type="a{sv}"/></signal>
            <signal name="Progress"><arg type="a{sv}"/></signal>
          </interface></node>)";
        auto* node = g_dbus_node_info_new_for_xml(xml, nullptr);
        static const GDBusInterfaceVTable vtable{
            .method_call = +[](GDBusConnection* connection, const gchar*, const gchar*, const gchar*,
                               const gchar* method, GVariant* parameters, GDBusMethodInvocation* invocation,
                               gpointer data) {
              auto& peer = *static_cast<Peer*>(data);
              const std::string_view action(method);
              if (action == "CreateUpdateMonitor") {
                peer.created = true;
                if (peer.failCheck) {
                  g_dbus_method_invocation_return_dbus_error(
                      invocation, "org.freedesktop.DBus.Error.NotSupported", "Synthetic missing update source"
                  );
                  return;
                }
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", monitor));
                if (!peer.silent) {
                  GVariantBuilder fields;
                  g_variant_builder_init(&fields, G_VARIANT_TYPE_VARDICT);
                  g_variant_builder_add(&fields, "{sv}", "running-commit", g_variant_new_string("running"));
                  g_variant_builder_add(&fields, "{sv}", "local-commit", g_variant_new_string("running"));
                  g_variant_builder_add(
                      &fields, "{sv}", "remote-commit", g_variant_new_string(peer.available ? "new" : "running")
                  );
                  g_dbus_connection_emit_signal(
                      connection, nullptr, monitor, monitorType, "UpdateAvailable", g_variant_new("(a{sv})", &fields),
                      nullptr
                  );
                }
              } else if (action == "Update") {
                ++peer.installs;
                g_dbus_method_invocation_return_value(invocation, nullptr);
                GVariantBuilder fields;
                g_variant_builder_init(&fields, G_VARIANT_TYPE_VARDICT);
                g_variant_builder_add(&fields, "{sv}", "progress", g_variant_new_uint32(100));
                g_variant_builder_add(&fields, "{sv}", "status", g_variant_new_uint32(peer.failUpdate ? 3 : 2));
                g_dbus_connection_emit_signal(
                    connection, nullptr, monitor, monitorType, "Progress", g_variant_new("(a{sv})", &fields), nullptr
                );
              } else if (action == "Spawn") {
                auto* argv = g_variant_get_child_value(parameters, 1);
                auto* executable = g_variant_get_child_value(argv, 0);
                auto* argument = g_variant_get_child_value(argv, 1);
                auto* flags = g_variant_get_child_value(parameters, 4);
                peer.spawnValid = g_variant_get_uint32(flags) == 2
                    && std::string_view(g_variant_get_bytestring(executable)) == "/app/bin/cressida"
                    && std::string_view(g_variant_get_bytestring(argument)) == "spaces ; $(literal)";
                g_variant_unref(flags);
                g_variant_unref(argument);
                g_variant_unref(executable);
                g_variant_unref(argv);
                peer.spawned = true;
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 123U));
              } else
                g_dbus_method_invocation_return_value(invocation, nullptr);
            }
        };
        const auto first =
            g_dbus_connection_register_object(connection, path, node->interfaces[0], &vtable, this, nullptr, nullptr);
        const auto second = g_dbus_connection_register_object(
            connection, monitor, node->interfaces[1], &vtable, this, nullptr, nullptr
        );
        ready.set_value();
        g_main_loop_run(loop);
        g_dbus_connection_unregister_object(connection, first);
        g_dbus_connection_unregister_object(connection, second);
        g_dbus_node_info_unref(node);
        g_dbus_connection_close_sync(connection, nullptr, nullptr);
        g_object_unref(connection);
        g_main_loop_unref(loop);
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
      });
      ready.get_future().get();
    }
    ~Peer() {
      g_main_loop_quit(loop);
      worker.join();
      g_test_dbus_down(bus);
      g_object_unref(bus);
    }
  };
  updates::Options options() {
    return {
        .installation = {.kind = updates::InstallationKind::Flatpak},
        .currentVersion = "v1.0.0",
        .fetch = [](const auto&, const auto&, auto) {
          throw std::runtime_error("Flatpak updates must not use the release downloader");
        }
    };
  }
} // namespace
int main() {
  try {
    Peer peer;
    {
      updates::Updater updater(options());
      require(updater.check(), "Explicit Flatpak check");
      wait(updater, updates::Phase::Available);
      require(updater.download(), "Explicit Flatpak installation");
      wait(updater, updates::Phase::Installed);
      require(peer.installs == 1, "One explicit update request installs once");
    }
    char name[] = "old sandbox executable", literal[] = "spaces ; $(literal)";
    char* arguments[] = {name, literal, nullptr};
    updates::relaunchFlatpak(arguments);
    require(
        peer.spawned && peer.spawnValid, "Restart requests a new sandbox with latest-version and literal arguments"
    );
    peer.available = false;
    {
      updates::Updater updater(options());
      updater.check();
      wait(updater, updates::Phase::Current);
      require(!updater.download(), "An unchanged Flatpak does not install again");
    }
    peer.failCheck = true;
    {
      updates::Updater updater(options());
      updater.check();
      require(
          !wait(updater, updates::Phase::Error).error.empty(),
          "Missing portal or origin produces an explicit update error"
      );
    }
    peer.failCheck = false;
    peer.available = true;
    peer.failUpdate = true;
    {
      updates::Updater updater(options());
      updater.check();
      wait(updater, updates::Phase::Available);
      updater.download();
      require(!wait(updater, updates::Phase::Error).error.empty(), "Rejected Flatpak installation remains explicit");
    }
    peer.failUpdate = false;
    peer.silent = true;
    peer.created = false;
    {
      updates::Updater updater(options());
      updater.check();
      const auto end = std::chrono::steady_clock::now() + deadline;
      while (!peer.created && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
      require(peer.created, "Silent monitor was created");
      updater.cancel();
      wait(updater, updates::Phase::Idle);
    }
    std::cout << "Private Flatpak monitor, installation, errors, cancellation and latest-version relaunch passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
