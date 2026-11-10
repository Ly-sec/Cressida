#include "updates/flatpak.h"

#include <chrono>
#include <filesystem>
#include <gio/gio.h>
#include <memory>
#include <stdexcept>
#include <thread>

namespace updates {
  namespace {
    constexpr const char* portal = "org.freedesktop.portal.Flatpak";
    constexpr const char* portalPath = "/org/freedesktop/portal/Flatpak";
    constexpr const char* monitorInterface = "org.freedesktop.portal.Flatpak.UpdateMonitor";
    constexpr int callDeadlineMs = 15000;
    constexpr auto checkDeadline = std::chrono::seconds{45};
    constexpr auto installDeadline = std::chrono::minutes{10};
    constexpr auto dispatchInterval = std::chrono::milliseconds{20};
    constexpr unsigned statusRunning = 0, statusEmpty = 1, statusDone = 2, statusFailed = 3;
    constexpr unsigned latestVersionFlag = 2;
    template <class T> using Object = std::unique_ptr<T, decltype(&g_object_unref)>;
    using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
    Variant call(
        GDBusConnection* bus, const char* path, const char* interface, const char* method, GVariant* parameters,
        const GVariantType* reply, GCancellable* cancellation
    ) {
      GError* error = nullptr;
      Variant result(
          g_dbus_connection_call_sync(
              bus, portal, path, interface, method, parameters, reply, G_DBUS_CALL_FLAGS_NONE, callDeadlineMs,
              cancellation, &error
          ),
          g_variant_unref
      );
      if (!result) {
        if (error)
          g_error_free(error);
        throw std::runtime_error(
            "Flatpak could not complete this request. Update through your software manager or install a newer release "
            "bundle."
        );
      }
      return result;
    }
    Object<GDBusConnection> connection(GCancellable* cancellation) {
      GError* error = nullptr;
      Object<GDBusConnection> result(g_bus_get_sync(G_BUS_TYPE_SESSION, cancellation, &error), g_object_unref);
      if (!result) {
        if (error)
          g_error_free(error);
        throw std::runtime_error("The Flatpak update service is unavailable. Use your software manager to update.");
      }
      return result;
    }
    struct Session {
      GMainContext* context = g_main_context_new();
      Object<GCancellable> cancellation{g_cancellable_new(), g_object_unref};
      Object<GDBusConnection> bus{nullptr, g_object_unref};
      std::string path;
      unsigned subscription = 0;
      Session() { g_main_context_push_thread_default(context); }
      ~Session() {
        if (subscription)
          g_dbus_connection_signal_unsubscribe(bus.get(), subscription);
        if (bus && !path.empty()) {
          constexpr int closeDeadlineMs = 1000;
          Variant closed(
              g_dbus_connection_call_sync(
                  bus.get(), portal, path.c_str(), monitorInterface, "Close", nullptr, G_VARIANT_TYPE_UNIT,
                  G_DBUS_CALL_FLAGS_NONE, closeDeadlineMs, nullptr, nullptr
              ),
              g_variant_unref
          );
        }
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
      }
    };
  } // namespace
  Snapshot flatpakUpdate(bool install, std::stop_token stop, const std::function<void(Snapshot)>& progress) {
    Session session;
    std::stop_callback cancel(stop, [&] { g_cancellable_cancel(session.cancellation.get()); });
    session.bus = connection(session.cancellation.get());
    // Subscribe before creating the monitor so an immediate first signal is retained.
    struct Events {
      std::string path;
      bool known = false, available = false, installed = false, finished = false;
      unsigned percent = 0, status = statusRunning;
    } events;
    session.subscription = g_dbus_connection_signal_subscribe(
        session.bus.get(), portal, monitorInterface, nullptr, nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar* path, const gchar*, const gchar* signal, GVariant* parameters,
            gpointer data) {
          if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(a{sv})")))
            return;
          auto& events = *static_cast<Events*>(data);
          if (!events.path.empty() && events.path != path)
            return;
          Variant info(g_variant_get_child_value(parameters, 0), g_variant_unref);
          if (std::string_view(signal) == "UpdateAvailable") {
            const char* running = nullptr;
            const char* local = nullptr;
            const char* remote = nullptr;
            g_variant_lookup(info.get(), "running-commit", "&s", &running);
            g_variant_lookup(info.get(), "local-commit", "&s", &local);
            g_variant_lookup(info.get(), "remote-commit", "&s", &remote);
            if (!running || !local || !remote || !*remote)
              return;
            events.known = true;
            events.installed = std::string_view(running) != local;
            events.available = std::string_view(local) != remote;
          } else if (std::string_view(signal) == "Progress") {
            g_variant_lookup(info.get(), "progress", "u", &events.percent);
            g_variant_lookup(info.get(), "status", "u", &events.status);
            events.finished = events.status != statusRunning;
          }
        },
        &events, nullptr
    );
    auto monitor = call(
        session.bus.get(), portalPath, portal, "CreateUpdateMonitor", g_variant_new("(a{sv})", nullptr),
        G_VARIANT_TYPE("(o)"), session.cancellation.get()
    );
    const char* path = nullptr;
    g_variant_get(monitor.get(), "(&o)", &path);
    session.path = events.path = path;
    const auto deadline = std::chrono::steady_clock::now() + (install ? installDeadline : checkDeadline);
    const auto dispatch = [&] {
      if (stop.stop_requested())
        throw std::runtime_error("Update cancelled.");
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error(
            "Flatpak did not finish checking or updating. Use your software manager, or check the release bundles."
        );
      while (g_main_context_pending(session.context))
        g_main_context_iteration(session.context, false);
      std::this_thread::sleep_for(dispatchInterval);
    };
    while (!events.known)
      dispatch();
    Snapshot snapshot;
    if (!install) {
      snapshot.phase = events.available ? Phase::Available : events.installed ? Phase::Installed : Phase::Current;
      return snapshot;
    }
    if (!events.available) {
      snapshot.phase = events.installed ? Phase::Installed : Phase::Current;
      return snapshot;
    }
    call(
        session.bus.get(), session.path.c_str(), monitorInterface, "Update", g_variant_new("(sa{sv})", "", nullptr),
        G_VARIANT_TYPE_UNIT, session.cancellation.get()
    );
    snapshot.phase = Phase::Downloading;
    snapshot.total = 100;
    while (!events.finished) {
      dispatch();
      snapshot.downloaded = std::min(events.percent, 100U);
      progress(snapshot);
    }
    if (events.status != statusEmpty && events.status != statusDone && events.status != statusFailed)
      throw std::runtime_error(
          "Flatpak returned an unknown update result. Check your software manager before trying again."
      );
    if (events.status == statusFailed)
      throw std::runtime_error(
          "Flatpak could not install this update. It may require changed permissions; use your software manager to "
          "continue."
      );
    snapshot.phase = events.status == statusDone || events.installed ? Phase::Installed : Phase::Current;
    return snapshot;
  }
  void relaunchFlatpak(char* const* arguments) {
    auto bus = connection(nullptr);
    GVariantBuilder argv, fds, env, options;
    g_variant_builder_init(&argv, G_VARIANT_TYPE("aay"));
    g_variant_builder_add_value(&argv, g_variant_new_bytestring("/app/bin/cressida"));
    for (std::size_t i = 1; arguments[i]; ++i)
      g_variant_builder_add_value(&argv, g_variant_new_bytestring(arguments[i]));
    g_variant_builder_init(&fds, G_VARIANT_TYPE("a{uh}"));
    g_variant_builder_init(&env, G_VARIANT_TYPE("a{ss}"));
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    std::error_code directoryError;
    const auto currentDirectory = std::filesystem::current_path(directoryError).string();
    call(
        bus.get(), portalPath, portal, "Spawn",
        g_variant_new(
            "(@ayaaya{uh}a{ss}ua{sv})", g_variant_new_bytestring(directoryError ? "/" : currentDirectory.c_str()),
            &argv, &fds, &env, latestVersionFlag, &options
        ),
        G_VARIANT_TYPE("(u)"), nullptr
    );
  }
} // namespace updates
