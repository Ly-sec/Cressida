#include "app/theme_runtime.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

namespace {
  void require(bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  }
  template <class Predicate>
  app::ThemeSnapshot waitFor(app::ThemeRuntime& runtime, Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      // The private fake portal dispatches its method handlers on this context.
      while (g_main_context_iteration(nullptr, false)) {
      }
      if (auto snapshot = runtime.drain(); snapshot && predicate(*snapshot)) {
        std::clog << message << ": passed\n";
        return std::move(*snapshot);
      }
      pollfd event{runtime.eventFd(), POLLIN, 0};
      (void)::poll(&event, 1, 10);
    }
    std::cerr << "Timed out: " << message << '\n';
    throw std::runtime_error(message);
  }
  void save(const std::filesystem::path& file, const std::string& text) {
    std::ofstream stream(file, std::ios::binary);
    stream << text;
    require(static_cast<bool>(stream), "Write synthetic theme file");
  }
  std::string changedTemplate() {
    auto value = app::themeTemplate();
    value.replace(value.find("#17181d"), 7, "#123456");
    value.replace(value.find("#f5f5f7"), 7, "#fedcba");
    return value;
  }
  void fileEvents(const std::filesystem::path& bundled, const std::filesystem::path& user) {
    app::ThemeRuntime runtime;
    runtime.start(bundled, user, true, false);
    waitFor(
        runtime, [](const auto& snapshot) { return snapshot.themes.size() == 4; },
        "Initial theme snapshot must wake the owning loop"
    );
    require(std::filesystem::is_directory(user), "Normal startup creates the user theme directory");
    save(user / "new.tmp", app::themeTemplate());
    std::filesystem::rename(user / "new.tmp", user / "custom.toml");
    waitFor(
        runtime, [](const auto& snapshot) { return snapshot.selected("custom").id == "custom"; },
        "Atomic theme creation must hot reload"
    );
    save(user / "custom.toml", "version=1\nname='Incomplete'\n[dark]\n");
    waitFor(
        runtime,
        [](const auto& snapshot) {
          return !snapshot.issues.empty() && snapshot.selected("custom").dark.background == 0x17181d;
        },
        "Invalid edits must expose an error and retain the last good palette"
    );
    save(user / "replacement.tmp", changedTemplate());
    std::filesystem::rename(user / "replacement.tmp", user / "custom.toml");
    waitFor(
        runtime,
        [](const auto& snapshot) {
          const auto& theme = snapshot.selected("custom");
          return snapshot.issues.empty() && theme.dark.background == 0x123456 && theme.light.background == 0xfedcba;
        },
        "Atomic replacement must apply both variants together"
    );
    auto alpha = changedTemplate();
    alpha.replace(alpha.find("#123456"), 7, "#12345680");
    alpha.replace(alpha.find("#fedcba"), 7, "#fedcba00");
    save(user / "replacement.tmp", alpha);
    std::filesystem::rename(user / "replacement.tmp", user / "custom.toml");
    waitFor(
        runtime,
        [](const auto& snapshot) {
          const auto& theme = snapshot.selected("custom");
          return snapshot.issues.empty()
              && theme.dark.background == app::ThemeColor{0x123456, 128}
          && theme.light.background == app::ThemeColor{0xfedcba, 0};
        },
        "Alpha-only edits must publish a new immutable theme snapshot"
    );
    std::filesystem::rename(user / "custom.toml", user / "renamed.toml");
    waitFor(
        runtime,
        [](const auto& snapshot) {
          return snapshot.selected("custom").id == "cressida" && snapshot.selected("renamed").id == "renamed";
        },
        "Renaming a theme updates its filename identity"
    );
    std::filesystem::remove_all(user);
    waitFor(
        runtime, [](const auto& snapshot) { return snapshot.themes.size() == 4; },
        "Removing the theme directory clears custom themes"
    );
    std::filesystem::create_directories(user);
    save(user / "restored.toml", changedTemplate());
    waitFor(
        runtime, [](const auto& snapshot) { return snapshot.selected("restored").id == "restored"; },
        "Directory recreation must restore file watching"
    );
  }
  struct Portal {
    GTestDBus* test = g_test_dbus_new(G_TEST_DBUS_NONE);
    GDBusConnection* connection = nullptr;
    GDBusNodeInfo* info = nullptr;
    guint object = 0;
    guint32 scheme = 2;
    Portal() {
      g_test_dbus_up(test);
      connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
      require(connection, "Open private test session bus");
      info = g_dbus_node_info_new_for_xml(
          "<node><interface name='org.freedesktop.portal.Settings'><method name='Read'>"
          "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/>"
          "</method><signal name='SettingChanged'><arg type='s'/><arg type='s'/><arg type='v'/></signal>"
          "</interface></node>",
          nullptr
      );
      static const GDBusInterfaceVTable table{
          .method_call = [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* parameters,
                            GDBusMethodInvocation* call, void* data) {
            const char *space, *key;
            g_variant_get(parameters, "(&s&s)", &space, &key);
            if (std::string_view(space) != "org.freedesktop.appearance" || std::string_view(key) != "color-scheme") {
              g_dbus_method_invocation_return_dbus_error(call, "org.test.UnknownSetting", "Unknown setting");
              return;
            }
            g_dbus_method_invocation_return_value(
                call, g_variant_new("(v)", g_variant_new_uint32(static_cast<Portal*>(data)->scheme))
            );
          }
      };
      object = g_dbus_connection_register_object(
          connection, "/org/freedesktop/portal/desktop", info->interfaces[0], &table, this, nullptr, nullptr
      );
      require(object != 0, "Register the synthetic settings portal");
      name(true);
    }
    void name(bool own) {
      auto* reply = g_dbus_connection_call_sync(
          connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
          own ? "RequestName" : "ReleaseName",
          own ? g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0U)
              : g_variant_new("(s)", "org.freedesktop.portal.Desktop"),
          nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr
      );
      require(reply, "Change the synthetic portal owner");
      g_variant_unref(reply);
    }
    void signal(guint32 value) {
      scheme = value;
      require(
          g_dbus_connection_emit_signal(
              connection, nullptr, "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings",
              "SettingChanged",
              g_variant_new("(ssv)", "org.freedesktop.appearance", "color-scheme", g_variant_new_uint32(value)), nullptr
          ),
          "Send desktop preference signal"
      );
    }
    ~Portal() {
      g_dbus_connection_unregister_object(connection, object);
      g_dbus_node_info_unref(info);
      g_dbus_connection_close_sync(connection, nullptr, nullptr);
      g_object_unref(connection);
      g_test_dbus_down(test);
      g_object_unref(test);
    }
  };
} // namespace
int main(int argc, char** argv) {
  char temporary[] = "/tmp/cressida-theme-runtime-XXXXXX";
  if (!mkdtemp(temporary))
    return 1;
  const auto root = std::filesystem::path(temporary);
  try {
    require(argc == 2, "Pass the bundled theme directory");
    g_setenv("GIO_USE_VFS", "local", true);
    Portal portal;
    fileEvents(argv[1], root / "config/cressida/themes");
    {
      const auto user = root / "startup/themes";
      std::filesystem::create_directories(user);
      save(user / "custom.toml", changedTemplate());
      app::ThemeRuntime runtime;
      const auto initial = runtime.start(argv[1], user, true, false);
      require(
          initial.selected("custom").dark.background == 0x123456
              && initial.selected("custom").light.background == 0xfedcba,
          "The first frame can use a custom theme before worker events arrive"
      );
      save(user / "custom.toml", "Incomplete save");
      waitFor(
          runtime,
          [](const auto& snapshot) {
            return !snapshot.issues.empty() && snapshot.selected("custom").dark.background == 0x123456;
          },
          "The watcher retains the valid startup palette across an incomplete edit"
      );
    }
    {
      app::ThemeRuntime runtime;
      runtime.start(argv[1], root / "portal/themes", true);
      waitFor(
          runtime, [](const auto& snapshot) { return snapshot.systemLight; },
          "System mode must read the desktop's initial preference"
      );
      portal.signal(1);
      waitFor(
          runtime, [](const auto& snapshot) { return !snapshot.systemLight; },
          "Desktop dark preference updates without polling"
      );
      portal.signal(2);
      waitFor(
          runtime, [](const auto& snapshot) { return snapshot.systemLight; },
          "Desktop light preference updates without polling"
      );
      portal.signal(99);
      waitFor(
          runtime, [](const auto& snapshot) { return !snapshot.systemLight; },
          "Unknown desktop preferences fall back to dark"
      );
      portal.signal(2);
      waitFor(runtime, [](const auto& snapshot) { return snapshot.systemLight; }, "Restore a light desktop preference");
      portal.name(false);
      waitFor(
          runtime, [](const auto& snapshot) { return !snapshot.systemLight; },
          "Portal disappearance restores the default preference"
      );
      portal.name(true);
      waitFor(
          runtime, [](const auto& snapshot) { return snapshot.systemLight; },
          "Portal restart re-reads the desktop preference"
      );
    }
    for (int i = 0; i < 4; ++i) {
      app::ThemeRuntime runtime;
      runtime.start(argv[1], root / "fast/themes", true, false);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    std::filesystem::remove_all(root);
    return 1;
  }
  std::filesystem::remove_all(root);
}
