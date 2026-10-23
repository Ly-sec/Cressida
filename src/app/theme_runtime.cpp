#include "app/theme_runtime.h"

#include "core/log.h"
#include "core/process/wake_event.h"

#include <gio/gio.h>
#include <mutex>
#include <thread>
#include <utility>

namespace app {
  namespace {
    constexpr Logger logger{"themes"};
    constexpr auto portalName = "org.freedesktop.portal.Desktop";
    constexpr auto portalPath = "/org/freedesktop/portal/desktop";
    constexpr auto settingsInterface = "org.freedesktop.portal.Settings";
    bool lightPreference(GVariant* value) {
      auto* current = g_variant_ref(value);
      while (g_variant_is_of_type(current, G_VARIANT_TYPE_VARIANT)) {
        auto* next = g_variant_get_variant(current);
        g_variant_unref(current);
        current = next;
      }
      const bool light = g_variant_is_of_type(current, G_VARIANT_TYPE_UINT32) && g_variant_get_uint32(current) == 2;
      g_variant_unref(current);
      return light;
    }
  } // namespace
  struct ThemeRuntime::Impl {
    process::WakeEvent wake;
    std::mutex mutex;
    std::optional<ThemeSnapshot> pending;
    GMainContext* context = g_main_context_new();
    GMainLoop* loop = g_main_loop_new(context, false);
    GCancellable* cancellation = g_cancellable_new();
    std::jthread worker;
    ~Impl() {
      if (worker.joinable()) {
        worker.request_stop();
        g_cancellable_cancel(cancellation);
        g_main_context_invoke(
            context,
            [](void* data) -> gboolean {
              g_main_loop_quit(static_cast<GMainLoop*>(data));
              return G_SOURCE_REMOVE;
            },
            loop
        );
        worker.join();
      }
      g_object_unref(cancellation);
      g_main_loop_unref(loop);
      g_main_context_unref(context);
    }
    void publish(const ThemeSnapshot& snapshot) {
      {
        std::lock_guard lock(mutex);
        pending = snapshot;
      }
      (void)wake.signal();
    }
    void run(std::stop_token stop, ThemeCatalog catalog, std::filesystem::path user, bool desktop) {
      g_main_context_push_thread_default(context);
      logger.debug("watcher starting");
      {
        struct State {
          Impl& owner;
          ThemeCatalog catalog;
          std::filesystem::path directory;
          std::vector<GFileMonitor*> monitors;
          GSource* debounce = nullptr;
          GDBusConnection* bus = nullptr;
          guint subscription = 0, nameWatch = 0;
          bool systemLight = false;
          bool monitorFailureLogged = false;
          bool stopping = false, subscriptionReleased = true, watchReleased = true;
          std::optional<ThemeSnapshot> previous;
          void publish() {
            auto snapshot = catalog.reload();
            snapshot.systemLight = systemLight;
            if (previous != snapshot) {
              logger.debug("catalog refreshed themes={} issues={}", snapshot.themes.size(), snapshot.issues.size());
              if (!snapshot.issues.empty() && (!previous || previous->issues != snapshot.issues))
                logger.warn("catalog contains invalid themes issues={}", snapshot.issues.size());
              previous = snapshot;
              owner.publish(snapshot);
            }
          }
          void watch() {
            for (auto* monitor : monitors) {
              g_signal_handlers_disconnect_by_data(monitor, this);
              g_file_monitor_cancel(monitor);
              g_object_unref(monitor);
            }
            monitors.clear();
            if (directory.empty())
              return;
            for (const auto& path : {directory, directory.parent_path(), directory.parent_path().parent_path()}) {
              if (path.empty())
                continue;
              auto* file = g_file_new_for_path(path.c_str());
              GError* error = nullptr;
              auto* monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, owner.cancellation, &error);
              g_object_unref(file);
              if (!monitor
                  && error
                  && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
                  && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                  && !monitorFailureLogged) {
                monitorFailureLogged = true;
                logger.warn("theme watching failed stage=directory-monitor repeated_warnings=suppressed");
              }
              if (error)
                g_error_free(error);
              if (!monitor)
                continue;
              g_signal_connect(
                  monitor, "changed",
                  G_CALLBACK((+[](GFileMonitor*, GFile* file, GFile* other, GFileMonitorEvent, void* data) {
                    auto& self = *static_cast<State*>(data);
                    for (auto* changed : {file, other}) {
                      if (!changed)
                        continue;
                      auto* name = g_file_get_path(changed);
                      const auto path = name ? std::filesystem::path(name) : std::filesystem::path{};
                      g_free(name);
                      if (path == self.directory
                          || path == self.directory.parent_path()
                          || (path.parent_path() == self.directory && path.extension() == ".toml")) {
                        self.schedule();
                        break;
                      }
                    }
                  })),
                  this
              );
              monitors.push_back(monitor);
            }
          }
          void schedule() {
            if (stopping)
              return;
            if (debounce) {
              g_source_destroy(debounce);
              g_source_unref(debounce);
            }
            debounce = g_timeout_source_new(100);
            g_source_set_callback(
                debounce,
                [](void* data) -> gboolean {
                  auto& self = *static_cast<State*>(data);
                  g_source_unref(std::exchange(self.debounce, nullptr));
                  self.watch();
                  self.publish();
                  return G_SOURCE_REMOVE;
                },
                this, nullptr
            );
            g_source_attach(debounce, owner.context);
          }
          void readPreference() {
            if (stopping)
              return;
            auto* reply = g_dbus_connection_call_sync(
                bus, portalName, portalPath, settingsInterface, "Read",
                g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"), G_VARIANT_TYPE("(v)"),
                G_DBUS_CALL_FLAGS_NONE, 1500, owner.cancellation, nullptr
            );
            if (reply) {
              auto* value = g_variant_get_child_value(reply, 0);
              systemLight = lightPreference(value);
              g_variant_unref(value);
              g_variant_unref(reply);
            } else {
              systemLight = false;
              if (!stopping)
                logger.debug("system appearance unavailable stage=portal-read");
            }
            publish();
          }
          void connectDesktop() {
            bus = g_bus_get_sync(G_BUS_TYPE_SESSION, owner.cancellation, nullptr);
            if (!bus) {
              if (!stopping)
                logger.warn("system appearance unavailable stage=session-bus");
              return;
            }
            logger.debug("system appearance subscription starting");
            subscriptionReleased = false;
            subscription = g_dbus_connection_signal_subscribe(
                bus, portalName, settingsInterface, "SettingChanged", portalPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* parameters,
                   void* data) {
                  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ssv)")))
                    return;
                  auto& self = *static_cast<State*>(data);
                  if (self.stopping)
                    return;
                  const char* space = nullptr;
                  const char* key = nullptr;
                  GVariant* value = nullptr;
                  g_variant_get(parameters, "(&s&s@v)", &space, &key, &value);
                  if (std::string_view(space) == "org.freedesktop.appearance"
                      && std::string_view(key) == "color-scheme") {
                    self.systemLight = lightPreference(value);
                    self.publish();
                  }
                  g_variant_unref(value);
                },
                this, [](void* data) { static_cast<State*>(data)->subscriptionReleased = true; }
            );
            watchReleased = false;
            nameWatch = g_bus_watch_name_on_connection(
                bus, portalName, G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                [](GDBusConnection*, const char*, const char*, void* data) {
                  logger.debug("appearance portal available");
                  static_cast<State*>(data)->readPreference();
                },
                [](GDBusConnection*, const char*, void* data) {
                  auto& self = *static_cast<State*>(data);
                  if (self.stopping)
                    return;
                  logger.debug("appearance portal unavailable; using fallback");
                  self.systemLight = false;
                  self.publish();
                },
                this, [](void* data) { static_cast<State*>(data)->watchReleased = true; }
            );
          }
          ~State() {
            stopping = true;
            if (nameWatch)
              g_bus_unwatch_name(nameWatch);
            if (subscription)
              g_dbus_connection_signal_unsubscribe(bus, subscription);
            if (debounce) {
              g_source_destroy(debounce);
              g_source_unref(debounce);
            }
            for (auto* monitor : monitors) {
              g_signal_handlers_disconnect_by_data(monitor, this);
              g_file_monitor_cancel(monitor);
              g_object_unref(monitor);
            }
            // Release queued GDBus/GFileMonitor dispatch data on its owning context
            // before the private loop and callback state are destroyed.
            while (!subscriptionReleased || !watchReleased)
              g_main_context_iteration(owner.context, true);
            if (bus)
              g_object_unref(bus);
          }
        } state{*this, std::move(catalog), user};
        if (!user.empty()) {
          std::error_code error;
          std::filesystem::create_directories(user, error);
          if (error && !stop.stop_requested())
            logger.warn("theme watching failed stage=directory-create");
        }
        state.watch();
        state.publish();
        if (desktop)
          state.connectDesktop();
        if (!stop.stop_requested())
          g_main_loop_run(loop);
      }
      g_main_context_pop_thread_default(context);
      logger.debug("watcher stopped");
    }
  };
  ThemeRuntime::ThemeRuntime() : m(std::make_unique<Impl>()) {}
  ThemeRuntime::~ThemeRuntime() = default;
  ThemeSnapshot ThemeRuntime::start(
      const std::filesystem::path& bundled, const std::filesystem::path& user, bool watch, bool desktop
  ) {
    m = std::make_unique<Impl>();
    ThemeCatalog catalog(bundled, user);
    auto initial = catalog.reload();
    logger.debug("catalog loaded themes={} issues={}", initial.themes.size(), initial.issues.size());
    if (!watch && !initial.issues.empty())
      logger.warn("catalog contains invalid themes issues={}", initial.issues.size());
    if (watch)
      m->worker =
          std::jthread([impl = m.get(), catalog = std::move(catalog), user, desktop](std::stop_token stop) mutable {
            impl->run(stop, std::move(catalog), user, desktop);
          });
    return initial;
  }
  int ThemeRuntime::eventFd() const { return m->wake.fd(); }
  std::optional<ThemeSnapshot> ThemeRuntime::drain() {
    m->wake.drain();
    std::lock_guard lock(m->mutex);
    return std::exchange(m->pending, std::nullopt);
  }
} // namespace app
