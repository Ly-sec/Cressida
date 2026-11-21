#include "auth/browser_login.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <gio/gio.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#ifndef CRESSIDA_OPEN_URI
#define CRESSIDA_OPEN_URI "xdg-open"
#endif

extern char** environ;

namespace auth {
  namespace {
    constexpr unsigned maximumLaunches = 8;
    constexpr unsigned launchDeadlineMs = 5000;
    constexpr auto portalResponseDeadline = std::chrono::minutes(2);
    std::atomic<unsigned> launches = 0;
    std::atomic<std::uint64_t> nextRequest = 0;

    bool spawnBrowser(const std::string& url, const std::string& token) {
      // Scope the handle to this launch. Never mutate the application's environment
      // or forward an inherited, already consumed desktop activation handle.
      std::vector<std::string> environment;
      for (auto** entry = environ; *entry; ++entry) {
        const std::string_view value(*entry);
        if (!value.starts_with("XDG_ACTIVATION_TOKEN=") && !value.starts_with("DESKTOP_STARTUP_ID="))
          environment.emplace_back(value);
      }
      if (!token.empty())
        environment.push_back("XDG_ACTIVATION_TOKEN=" + token);
      std::vector<char*> envp;
      for (auto& value : environment)
        envp.push_back(value.data());
      envp.push_back(nullptr);
      posix_spawn_file_actions_t actions;
      if (posix_spawn_file_actions_init(&actions) != 0)
        return false;
      const auto input = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
      const auto output = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
      const auto errorOutput = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
      char program[] = CRESSIDA_OPEN_URI;
      char* args[] = {program, const_cast<char*>(url.c_str()), nullptr};
      pid_t pid = -1;
      const auto error =
          input || output || errorOutput ? EIO : posix_spawnp(&pid, program, &actions, nullptr, args, envp.data());
      posix_spawn_file_actions_destroy(&actions);
      if (error != 0)
        return false;
      std::thread([pid] {
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
        }
      }).detach();
      return true;
    }

    struct PortalLaunch {
      std::string url, token;
      GMainLoop* loop;
      GCancellable* cancel;
      GSource* deadline = nullptr;
      GDBusConnection* bus = nullptr;
      guint subscription = 0;
      std::string requestPath;
      bool methodDone = false, responseDone = false;
      bool unavailable = false;
    };
    void launchBrowser(std::string url, std::string token) {
      const auto* address = g_getenv("DBUS_SESSION_BUS_ADDRESS");
      if (!address || !*address) {
        spawnBrowser(url, token);
        --launches;
        return;
      }
      auto* context = g_main_context_new();
      g_main_context_push_thread_default(context);
      PortalLaunch launch{std::move(url), std::move(token), g_main_loop_new(context, false), g_cancellable_new()};
      auto* deadline = g_timeout_source_new(launchDeadlineMs);
      launch.deadline = deadline;
      g_source_set_callback(
          deadline,
          [](void* data) -> gboolean {
            auto& launch = *static_cast<PortalLaunch*>(data);
            g_cancellable_cancel(launch.cancel);
            if (launch.methodDone)
              g_main_loop_quit(launch.loop);
            return G_SOURCE_REMOVE;
          },
          &launch, nullptr
      );
      g_source_attach(deadline, context);
      g_dbus_connection_new_for_address(
          address,
          static_cast<GDBusConnectionFlags>(
              G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
          ),
          nullptr, launch.cancel,
          [](GObject*, GAsyncResult* result, void* data) {
            auto& launch = *static_cast<PortalLaunch*>(data);
            GError* error = nullptr;
            auto* bus = g_dbus_connection_new_for_address_finish(result, &error);
            if (!bus) {
              launch.unavailable = !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
              g_clear_error(&error);
              g_main_loop_quit(launch.loop);
              return;
            }
            launch.bus = bus;
            const auto handle = "cressida_link_" + std::to_string(++nextRequest);
            std::string sender = g_dbus_connection_get_unique_name(bus);
            sender.erase(0, 1);
            std::replace(sender.begin(), sender.end(), '.', '_');
            const auto path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + handle;
            launch.requestPath = path;
            // Subscribe before OpenURI: a portal may complete before its method
            // reply. Keep the connection alive until that request completes.
            launch.subscription = g_dbus_connection_signal_subscribe(
                bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request", "Response", path.c_str(),
                nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, void* data) {
                  auto& launch = *static_cast<PortalLaunch*>(data);
                  launch.responseDone = true;
                  if (launch.methodDone)
                    g_main_loop_quit(launch.loop);
                },
                &launch, nullptr
            );
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(handle.c_str()));
            g_variant_builder_add(&options, "{sv}", "activation_token", g_variant_new_string(launch.token.c_str()));
            g_dbus_connection_call(
                bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.OpenURI", "OpenURI",
                g_variant_new("(ss@a{sv})", "", launch.url.c_str(), g_variant_builder_end(&options)),
                G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, launchDeadlineMs, launch.cancel,
                [](GObject* source, GAsyncResult* result, void* data) {
                  auto& launch = *static_cast<PortalLaunch*>(data);
                  GError* error = nullptr;
                  auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
                  launch.methodDone = true;
                  if (reply) {
                    g_variant_unref(reply);
                    if (launch.responseDone || g_cancellable_is_cancelled(launch.cancel))
                      g_main_loop_quit(launch.loop);
                    else
                      g_source_set_ready_time(
                          launch.deadline,
                          g_get_monotonic_time()
                              + std::chrono::duration_cast<std::chrono::microseconds>(portalResponseDeadline).count()
                      );
                    return;
                  }
                  // Only fall back when no portal could accept the request. A timeout,
                  // denial or cancelled chooser must never open the same link twice.
                  launch.unavailable = g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)
                      || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER)
                      || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)
                      || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE);
                  g_clear_error(&error);
                  g_main_loop_quit(launch.loop);
                },
                &launch
            );
          },
          &launch
      );
      g_main_loop_run(launch.loop);
      g_source_destroy(deadline);
      g_source_unref(deadline);
      if (launch.bus) {
        if (g_cancellable_is_cancelled(launch.cancel) && !launch.responseDone)
          g_dbus_connection_call(
              launch.bus, "org.freedesktop.portal.Desktop", launch.requestPath.c_str(),
              "org.freedesktop.portal.Request", "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, launchDeadlineMs,
              nullptr, nullptr, nullptr
          );
        g_dbus_connection_signal_unsubscribe(launch.bus, launch.subscription);
        g_object_unref(launch.bus);
      }
      g_object_unref(launch.cancel);
      g_main_loop_unref(launch.loop);
      g_main_context_pop_thread_default(context);
      g_main_context_unref(context);
      if (launch.unavailable)
        spawnBrowser(launch.url, launch.token);
      --launches;
    }
  } // namespace

  bool isExternalUrl(std::string_view url) {
    return (url.starts_with("https://") || url.starts_with("http://"))
        && url.find_first_of("\r\n\t ") == std::string_view::npos
        && url.find('\0') == std::string_view::npos;
  }
  bool openExternalUrl(const std::string& url, std::string activationToken) {
    if (!isExternalUrl(url))
      return false;
    if (activationToken.empty())
      return spawnBrowser(url, {});
    if (launches.fetch_add(1) >= maximumLaunches) {
      --launches;
      return false;
    }
    try {
      std::thread(launchBrowser, url, std::move(activationToken)).detach();
    } catch (...) {
      --launches;
      return false;
    }
    return true;
  }
} // namespace auth
