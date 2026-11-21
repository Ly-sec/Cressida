#include "auth/browser_login.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
  using namespace std::chrono_literals;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  template <class Predicate> void waitFor(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + 7s;
    while (!ready()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      require(std::chrono::steady_clock::now() < deadline, "Browser handoff did not finish in time");
      std::this_thread::sleep_for(1ms);
    }
  }
  struct Portal {
    unsigned calls = 0;
    std::string url, token, error;
    bool hold = false;
    bool privatePeer = false;
    GDBusMethodInvocation* pending = nullptr;
  };
} // namespace

int main(int argc, char** argv) {
  // This executable also substitutes for xdg-open. No test opens a real browser.
  if (argc > 1) {
    const auto* output = std::getenv("CRESSIDA_TEST_BROWSER_OUTPUT");
    if (!output)
      return 2;
    std::ofstream capture(output);
    const auto* token = std::getenv("XDG_ACTIVATION_TOKEN");
    const auto* startup = std::getenv("DESKTOP_STARTUP_ID");
    capture << argv[1] << '\n' << (token ? token : "") << '\n' << (startup ? startup : "") << '\n';
    return 0;
  }
  try {
    auto* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    GError* error = nullptr;
    auto* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    require(connection && !error, "Connect to private test bus");
    auto* owned = g_dbus_connection_call_sync(
        connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0U), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE,
        1000, nullptr, &error
    );
    require(owned && !error, "Own private portal name");
    g_variant_unref(owned);
    auto* info = g_dbus_node_info_new_for_xml(
        "<node><interface name='org.freedesktop.portal.OpenURI'><method name='OpenURI'>"
        "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='a{sv}' direction='in'/>"
        "<arg type='o' direction='out'/></method></interface></node>",
        &error
    );
    require(info && !error, "Parse private portal interface");
    Portal portal;
    const GDBusInterfaceVTable vtable{
        .method_call = [](GDBusConnection* connection, const char* sender, const char*, const char*, const char*,
                          GVariant* params, GDBusMethodInvocation* invocation, void* data) {
          auto& portal = *static_cast<Portal*>(data);
          portal.privatePeer = std::string(sender) != g_dbus_connection_get_unique_name(connection);
          ++portal.calls;
          const char *parent, *url;
          GVariant* options = nullptr;
          g_variant_get(params, "(&s&s@a{sv})", &parent, &url, &options);
          const char* token = nullptr;
          g_variant_lookup(options, "activation_token", "&s", &token);
          portal.url = url;
          portal.token = token ? token : "";
          const char* handle = nullptr;
          g_variant_lookup(options, "handle_token", "&s", &handle);
          require(handle, "Portal requests provide a response handle");
          std::string identity = sender + 1;
          for (auto& ch : identity)
            if (ch == '.')
              ch = '_';
          const auto path = "/org/freedesktop/portal/desktop/request/" + identity + "/" + handle;
          g_variant_unref(options);
          if (portal.hold)
            portal.pending = G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
          else if (!portal.error.empty())
            g_dbus_method_invocation_return_dbus_error(invocation, portal.error.c_str(), "Synthetic failure");
          else {
            GVariantBuilder results;
            g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
            g_dbus_connection_emit_signal(
                connection, sender, path.c_str(), "org.freedesktop.portal.Request", "Response",
                g_variant_new("(u@a{sv})", 0U, g_variant_builder_end(&results)), nullptr
            );
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", path.c_str()));
          }
        }
    };
    const auto registration = g_dbus_connection_register_object(
        connection, "/org/freedesktop/portal/desktop", info->interfaces[0], &vtable, &portal, nullptr, &error
    );
    require(registration && !error, "Register synthetic portal");
    auto* directory = g_dir_make_tmp("cressida-browser-XXXXXX", &error);
    require(directory && !error, "Create a private launch capture");
    const auto output = std::filesystem::path(directory) / "capture";
    g_setenv("CRESSIDA_TEST_BROWSER_OUTPUT", output.c_str(), true);
    g_setenv("XDG_ACTIVATION_TOKEN", "inherited-used-token", true);
    g_setenv("DESKTOP_STARTUP_ID", "inherited-startup", true);
    require(
        !auth::openExternalUrl("file:///tmp/test", "token")
            && !auth::openExternalUrl("https://example.test/\n", "token")
            && !auth::openExternalUrl(std::string("https://example.test/") + '\0' + "hidden", "token"),
        "Reject unsupported schemes, whitespace and embedded NULs before launch"
    );
    require(auth::openExternalUrl("https://example.test/first", "first-token"), "Queue activated link");
    waitFor([&] { return portal.calls == 1; });
    require(
        portal.url == "https://example.test/first" && portal.token == "first-token" && portal.privatePeer,
        "Portal receives the selected URL and fresh activation token"
    );
    require(!std::filesystem::exists(output), "A successful portal launch never invokes the fallback");
    require(
        std::string(std::getenv("XDG_ACTIVATION_TOKEN")) == "inherited-used-token",
        "Opening links does not change the parent environment"
    );

    portal.error = "org.freedesktop.DBus.Error.UnknownMethod";
    require(auth::openExternalUrl("https://example.test/fallback", "fallback-token"), "Queue portal fallback");
    waitFor([&] { return std::filesystem::exists(output) && std::filesystem::file_size(output) > 0; });
    std::ifstream capture(output);
    std::string url, token, startup;
    std::getline(capture, url);
    std::getline(capture, token);
    std::getline(capture, startup);
    require(
        url == "https://example.test/fallback" && token == "fallback-token" && startup.empty(),
        "Fallback scopes the fresh handle to its child and strips stale startup handles"
    );
    std::filesystem::remove(output);

    portal.error = "org.freedesktop.portal.Error.NotAllowed";
    require(auth::openExternalUrl("https://example.test/denied", "denied-token"), "Queue denied link");
    waitFor([&] { return portal.calls == 3; });
    portal.hold = true;
    require(auth::openExternalUrl("https://example.test/timeout", "timeout-token"), "Queue bounded request");
    waitFor([&] { return portal.calls == 4; });
    const auto deadline = std::chrono::steady_clock::now() + 5500ms;
    while (std::chrono::steady_clock::now() < deadline) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      std::this_thread::sleep_for(1ms);
    }
    require(!std::filesystem::exists(output), "Denial and timeout never trigger another browser launch");
    g_dbus_method_invocation_return_value(
        portal.pending, g_variant_new("(o)", "/org/freedesktop/portal/desktop/request/late")
    );
    g_object_unref(portal.pending);

    require(auth::openExternalUrl("https://example.test/plain"), "Launch without compositor support");
    waitFor([&] { return std::filesystem::exists(output) && std::filesystem::file_size(output) > 0; });
    std::ifstream plain(output);
    std::getline(plain, url);
    std::getline(plain, token);
    std::getline(plain, startup);
    require(
        url == "https://example.test/plain" && token.empty() && startup.empty(),
        "An ordinary launch cannot reuse inherited handles"
    );
    std::filesystem::remove_all(directory);
    g_free(directory);
    g_dbus_connection_unregister_object(connection, registration);
    g_dbus_node_info_unref(info);
    g_object_unref(connection);
    g_test_dbus_down(bus);
    g_object_unref(bus);
    std::cout << "Activated browser handoff and fallback passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
