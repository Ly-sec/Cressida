#include "app/desktop_tray.h"
#include "render/core/image_file_loader.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <gio/gio.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
  using namespace std::chrono_literals;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  template <class Predicate> void waitFor(Predicate predicate, const char* message) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < end) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      if (predicate())
        return;
      poll(nullptr, 0, 5);
    }
    throw std::runtime_error(message);
  }
  struct Host {
    GTestDBus* test = g_test_dbus_new(G_TEST_DBUS_NONE);
    GDBusConnection* bus = nullptr;
    GDBusNodeInfo* info = nullptr;
    guint kdeObject = 0, fdObject = 0;
    std::string item;
    unsigned registrations = 0;
    bool host = true, hold = false;
    GDBusMethodInvocation* held = nullptr;
    Host() {
      g_test_dbus_up(test);
      bus = g_dbus_connection_new_for_address_sync(
          g_test_dbus_get_bus_address(test),
          GDBusConnectionFlags(
              G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
          ),
          nullptr, nullptr, nullptr
      );
      require(bus, "Connect a private tray host without desktop access");
      const std::string body = R"XML(
        <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
        <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
        <signal name="StatusNotifierHostRegistered"/><signal name="StatusNotifierHostUnregistered"/>
      )XML";
      const auto xml = "<node><interface name=\"org.kde.StatusNotifierWatcher\">"
          + body
          + "</interface><interface name=\"org.freedesktop.StatusNotifierWatcher\">"
          + body
          + "</interface></node>";
      info = g_dbus_node_info_new_for_xml(xml.c_str(), nullptr);
      static const GDBusInterfaceVTable table{
          .method_call =
              [](GDBusConnection*, const gchar* sender, const gchar*, const gchar*, const gchar*, GVariant* args,
                 GDBusMethodInvocation* invocation, gpointer data) {
                auto& self = *static_cast<Host*>(data);
                const char* item;
                g_variant_get(args, "(&s)", &item);
                self.item = item;
                if (self.item != sender) {
                  g_dbus_method_invocation_return_error(
                      invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Use the exported connection identity"
                  );
                  return;
                }
                ++self.registrations;
                if (self.hold)
                  self.held = G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
                else
                  g_dbus_method_invocation_return_value(invocation, nullptr);
              },
          .get_property = [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GError**,
                             gpointer data) -> GVariant* {
            return g_variant_new_boolean(static_cast<Host*>(data)->host);
          }
      };
      kdeObject = g_dbus_connection_register_object(
          bus, "/StatusNotifierWatcher", info->interfaces[0], &table, this, nullptr, nullptr
      );
      fdObject = g_dbus_connection_register_object(
          bus, "/StatusNotifierWatcher", info->interfaces[1], &table, this, nullptr, nullptr
      );
      require(kdeObject && fdObject, "Export the private watcher protocols");
      own("org.kde.StatusNotifierWatcher", true);
    }
    void own(const char* name, bool enabled) {
      auto* result = g_dbus_connection_call_sync(
          bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
          enabled ? "RequestName" : "ReleaseName",
          enabled ? g_variant_new("(su)", name, 0U) : g_variant_new("(s)", name), nullptr, G_DBUS_CALL_FLAGS_NONE, 1000,
          nullptr, nullptr
      );
      require(result, "Change private watcher ownership");
      g_variant_unref(result);
    }
    void present(bool value) {
      host = value;
      require(
          g_dbus_connection_emit_signal(
              bus, nullptr, "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
              value ? "StatusNotifierHostRegistered" : "StatusNotifierHostUnregistered", nullptr, nullptr
          ),
          "Signal a private tray host change"
      );
    }
    GVariant* call(const char* path, const char* interface, const char* method, GVariant* args, bool succeeds = true) {
      GError* error = nullptr;
      struct Call {
        GVariant* result = nullptr;
        GError* error = nullptr;
        bool done = false;
      } call;
      g_dbus_connection_call(
          bus, item.c_str(), path, interface, method, args, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr,
          [](GObject* source, GAsyncResult* response, gpointer data) {
            auto& call = *static_cast<Call*>(data);
            call.result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), response, &call.error);
            call.done = true;
          },
          &call
      );
      waitFor([&] { return call.done; }, "Private tray method completes while the host dispatches");
      auto* result = call.result;
      error = call.error;
      if (succeeds && !result) {
        const std::string message = error ? error->message : "Private tray call failed";
        g_clear_error(&error);
        throw std::runtime_error(message);
      }
      if (!succeeds)
        require(!result && error, "Invalid tray queries return D-Bus errors");
      g_clear_error(&error);
      return result;
    }
    ~Host() {
      if (held) {
        g_dbus_method_invocation_return_error(held, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Test complete");
        g_object_unref(held);
      }
      g_dbus_connection_unregister_object(bus, kdeObject);
      g_dbus_connection_unregister_object(bus, fdObject);
      g_dbus_node_info_unref(info);
      g_dbus_connection_close_sync(bus, nullptr, nullptr);
      g_object_unref(bus);
      g_test_dbus_down(test);
      g_object_unref(test);
    }
  };
  app::TrayEvent event(app::DesktopTray& tray, app::TrayCommand command) {
    std::optional<app::TrayEvent> result;
    waitFor(
        [&] {
          while (auto value = tray.poll()) {
            require(value->command == command, "Tray commands preserve their order and identity");
            result = std::move(value);
            return true;
          }
          return false;
        },
        "The tray event wakes the owning loop"
    );
    return std::move(*result);
  }
} // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 2, "Pass the bundled logo path");
    Host host;
    {
      app::DesktopTray tray;
      tray.start(argv[1]);
      require(
          event(tray, app::TrayCommand::Availability).available && host.registrations == 1,
          "A registered live host enables background operation"
      );
      pollfd wake{tray.eventFd(), POLLIN, 0};
      auto* all = host.call(
          "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "GetAll",
          g_variant_new("(s)", "org.kde.StatusNotifierItem")
      );
      GVariant* values;
      g_variant_get(all, "(@a{sv})", &values);
      const char *title = nullptr, *menu = nullptr, *icon = nullptr;
      gboolean itemIsMenu = true;
      require(
          g_variant_lookup(values, "Title", "&s", &title)
              && std::string_view(title) == "Cressida"
              && g_variant_lookup(values, "Menu", "&o", &menu)
              && std::string_view(menu) == "/MenuBar"
              && g_variant_lookup(values, "IconName", "&s", &icon)
              && std::string_view(icon) == argv[1]
              && g_variant_lookup(values, "ItemIsMenu", "b", &itemIsMenu)
              && !itemIsMenu,
          "The tray publishes the real logo, activation behavior and native menu path"
      );
      auto* pixmaps = g_variant_lookup_value(values, "IconPixmap", G_VARIANT_TYPE("a(iiay)"));
      require(
          pixmaps && g_variant_n_children(pixmaps) == 3, "The tray supplies native small, medium and HiDPI logo pixmaps"
      );
      gint32 width, height;
      GVariant* pixels;
      g_variant_get_child(pixmaps, 2, "(ii@ay)", &width, &height, &pixels);
      gsize length;
      const auto* argb = static_cast<const guint8*>(g_variant_get_fixed_array(pixels, &length, 1));
      const auto image = loadImageFile(argv[1], static_cast<int>(Style::glyphSizeDisplay * 2));
      require(
          image && width == image->width && height == image->height && length == image->rgba.size(),
          "The logo pixmap has valid raster bounds"
      );
      for (std::size_t i = 0; i < length; i += 4)
        require(
            argb[i] == image->rgba[i + 3]
                && argb[i + 1] == image->rgba[i]
                && argb[i + 2] == image->rgba[i + 1]
                && argb[i + 3] == image->rgba[i + 2],
            "Tray pixmaps use network-order ARGB without swapping colors or alpha"
        );
      g_variant_unref(pixels);
      g_variant_unref(pixmaps);
      g_variant_unref(values);
      g_variant_unref(all);
      auto* result = host.call(
          "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
          g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(nullptr, 0))
      );
      guint32 revision;
      GVariant* layout;
      g_variant_get(result, "(u@(ia{sv}av))", &revision, &layout);
      auto* children = g_variant_get_child_value(layout, 2);
      require(
          revision == 1 && g_variant_n_children(children) == 3, "The native tray menu contains exactly three actions"
      );
      const std::array expected{"Open", "Restart", "Close"};
      for (gsize i = 0; i < expected.size(); ++i) {
        auto* variant = g_variant_get_child_value(children, i);
        auto* child = g_variant_get_variant(variant);
        auto* props = g_variant_get_child_value(child, 1);
        const char* label;
        require(
            g_variant_lookup(props, "label", "&s", &label) && std::string_view(label) == expected[i],
            "Tray action order and captions match the requested menu"
        );
        g_variant_unref(props);
        g_variant_unref(child);
        g_variant_unref(variant);
      }
      g_variant_unref(children);
      g_variant_unref(layout);
      g_variant_unref(result);
      const char* names[]{"label"};
      result = host.call(
          "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
          g_variant_new("(ii@as)", 0, 0, g_variant_new_strv(names, 1))
      );
      g_variant_get(result, "(u@(ia{sv}av))", &revision, &layout);
      auto* props = g_variant_get_child_value(layout, 1);
      children = g_variant_get_child_value(layout, 2);
      require(
          g_variant_n_children(children) == 0 && g_variant_n_children(props) == 1,
          "Menu queries honor depth and property filters"
      );
      g_variant_unref(props);
      g_variant_unref(children);
      g_variant_unref(layout);
      g_variant_unref(result);
      host.call(
          "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
          g_variant_new("(ii@as)", 99, -1, g_variant_new_strv(nullptr, 0)), false
      );
      result = host.call(
          "/MenuBar", "com.canonical.dbusmenu", "GetGroupProperties",
          g_variant_new("(@ai@as)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0), g_variant_new_strv(names, 1))
      );
      auto* group = g_variant_get_child_value(result, 0);
      require(g_variant_n_children(group) == 4, "Group queries include the root and all actions");
      tray.sharing(true);
      waitFor(
          [&] {
            auto* reply = host.call(
                "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
                g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(nullptr, 0))
            );
            guint32 version = 0;
            GVariant* tree = nullptr;
            g_variant_get(reply, "(u@(ia{sv}av))", &version, &tree);
            auto* items = g_variant_get_child_value(tree, 2);
            const bool updated = version >= 2 && g_variant_n_children(items) == 4;
            g_variant_unref(items);
            g_variant_unref(tree);
            g_variant_unref(reply);
            return updated;
          },
          "An active share publishes the stop action with a new menu revision"
      );
      auto* stopped = host.call(
          "/MenuBar", "com.canonical.dbusmenu", "Event",
          g_variant_new("(isvu)", 4, "clicked", g_variant_new_int32(0), 0U)
      );
      g_variant_unref(stopped);
      event(tray, app::TrayCommand::StopSharing);
      tray.sharing(false);
      waitFor(
          [&] {
            auto* reply = host.call(
                "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
                g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(nullptr, 0))
            );
            guint32 version = 0;
            GVariant* tree = nullptr;
            g_variant_get(reply, "(u@(ia{sv}av))", &version, &tree);
            auto* items = g_variant_get_child_value(tree, 2);
            const bool updated = version >= 3 && g_variant_n_children(items) == 3;
            g_variant_unref(items);
            g_variant_unref(tree);
            g_variant_unref(reply);
            return updated;
          },
          "Stopping a share removes its tray action"
      );
      g_variant_unref(group);
      g_variant_unref(result);
      result = host.call(
          "/MenuBar", "com.canonical.dbusmenu", "Event",
          g_variant_new("(isvu)", 1, "hovered", g_variant_new_int32(0), 0U)
      );
      g_variant_unref(result);
      require(!tray.poll(), "Hovering never activates tray actions");
      result = host.call(
          "/StatusNotifierItem", "org.kde.StatusNotifierItem", "ProvideXdgActivationToken",
          g_variant_new("(s)", "private-test-token")
      );
      g_variant_unref(result);
      result = host.call("/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate", g_variant_new("(ii)", 0, 0));
      g_variant_unref(result);
      require(
          ::poll(&wake, 1, 1000) > 0 && event(tray, app::TrayCommand::Open).activationToken == "private-test-token",
          "Native tray activation forwards its token through the wake descriptor"
      );
      result = host.call(
          "/StatusNotifierItem", "org.kde.StatusNotifierItem", "SecondaryActivate", g_variant_new("(ii)", 0, 0)
      );
      g_variant_unref(result);
      require(event(tray, app::TrayCommand::Open).activationToken.empty(), "Activation tokens are consumed only once");
      GVariantBuilder clicks;
      g_variant_builder_init(&clicks, G_VARIANT_TYPE("a(isvu)"));
      for (int id : {1, 2, 3})
        g_variant_builder_add(&clicks, "(isvu)", id, "clicked", g_variant_new_int32(0), 0U);
      result = host.call(
          "/MenuBar", "com.canonical.dbusmenu", "EventGroup",
          g_variant_new("(@a(isvu))", g_variant_builder_end(&clicks))
      );
      g_variant_unref(result);
      event(tray, app::TrayCommand::Open);
      event(tray, app::TrayCommand::Restart);
      event(tray, app::TrayCommand::Close);
      host.present(false);
      require(!event(tray, app::TrayCommand::Availability).available, "Removing the tray host disables hiding");
      host.present(true);
      require(event(tray, app::TrayCommand::Availability).available, "A returning tray host enables hiding again");
      host.own("org.kde.StatusNotifierWatcher", false);
      require(!event(tray, app::TrayCommand::Availability).available, "Watcher loss is delivered without polling");
      host.own("org.kde.StatusNotifierWatcher", true);
      require(
          event(tray, app::TrayCommand::Availability).available && host.registrations == 2,
          "A restarted watcher receives a fresh registration"
      );
      host.own("org.freedesktop.StatusNotifierWatcher", true);
      waitFor([&] { return host.registrations == 3; }, "The freedesktop watcher interface is supported");
      host.own("org.kde.StatusNotifierWatcher", false);
      // A property round trip lets the worker process queued owner changes.
      result = host.call(
          "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "GetAll",
          g_variant_new("(s)", "org.freedesktop.StatusNotifierItem")
      );
      g_variant_unref(result);
      require(!tray.poll(), "An alternate registered host prevents a false unavailable transition");
      host.own("org.freedesktop.StatusNotifierWatcher", false);
      require(
          !event(tray, app::TrayCommand::Availability).available,
          "Losing the last host produces one unavailable transition"
      );
    }
    host.hold = true;
    host.own("org.kde.StatusNotifierWatcher", true);
    const auto start = std::chrono::steady_clock::now();
    {
      app::DesktopTray tray;
      tray.start(argv[1]);
      waitFor([&] { return host.held != nullptr; }, "The private watcher holds an in-flight registration");
    }
    require(
        std::chrono::steady_clock::now() - start < 2s, "Shutdown cancels an unresponsive tray registration promptly"
    );
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
