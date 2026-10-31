#include "app/desktop_tray.h"

#include "app/desktop_activation.h"
#include "core/log.h"
#include "core/process/wake_event.h"
#include "render/core/image_file_loader.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <gio/gio.h>
#include <mutex>
#include <thread>
#include <utility>

namespace app {
  namespace {
    constexpr Logger logger{"tray"};
    constexpr auto itemPath = "/StatusNotifierItem";
    constexpr auto menuPath = "/MenuBar";
    constexpr auto menuInterface = "com.canonical.dbusmenu";
    constexpr auto watcherPath = "/StatusNotifierWatcher";
    constexpr int callDeadlineMs = 2000;
    constexpr std::size_t maximumPendingEvents = 32;
    constexpr unsigned menuVersion = 4;
    constexpr std::array labels{"", "Open", "Restart", "Close", "Stop Sharing"};
    constexpr auto itemBody = R"XML(
      <property name="Category" type="s" access="read"/><property name="Id" type="s" access="read"/>
      <property name="Title" type="s" access="read"/><property name="Status" type="s" access="read"/>
      <property name="WindowId" type="i" access="read"/><property name="IconThemePath" type="s" access="read"/>
      <property name="IconName" type="s" access="read"/><property name="IconPixmap" type="a(iiay)" access="read"/>
      <property name="OverlayIconName" type="s" access="read"/><property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
      <property name="AttentionIconName" type="s" access="read"/><property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
      <property name="AttentionMovieName" type="s" access="read"/><property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
      <property name="ItemIsMenu" type="b" access="read"/><property name="Menu" type="o" access="read"/>
      <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
      <method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
      <method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
      <method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
      <method name="ProvideXdgActivationToken"><arg type="s" direction="in"/></method>
      <signal name="NewIcon"/><signal name="NewTitle"/><signal name="NewToolTip"/>
      <signal name="NewStatus"><arg type="s"/></signal><signal name="NewMenu"/>
    )XML";
    constexpr auto menuXml = R"XML(<node><interface name="com.canonical.dbusmenu">
      <property name="Version" type="u" access="read"/><property name="TextDirection" type="s" access="read"/>
      <property name="Status" type="s" access="read"/><property name="IconThemePath" type="as" access="read"/>
      <method name="GetLayout"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
        <arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/></method>
      <method name="GetGroupProperties"><arg type="ai" direction="in"/><arg type="as" direction="in"/>
        <arg type="a(ia{sv})" direction="out"/></method>
      <method name="GetProperty"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
      <method name="Event"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/><arg type="u" direction="in"/></method>
      <method name="EventGroup"><arg type="a(isvu)" direction="in"/><arg type="ai" direction="out"/></method>
      <method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
      <method name="AboutToShowGroup"><arg type="ai" direction="in"/><arg type="ai" direction="out"/><arg type="ai" direction="out"/></method>
      <signal name="LayoutUpdated"><arg type="u"/><arg type="i"/></signal>
      <signal name="ItemsPropertiesUpdated"><arg type="a(ia{sv})"/><arg type="a(ias)"/></signal>
    </interface></node>)XML";
    bool validId(int id) { return id >= 0 && id < static_cast<int>(labels.size()); }
    GVariant* menuProperty(int id, std::string_view name, bool sharing) {
      if (name == "label")
        return g_variant_new_string(labels[id]);
      if (name == "enabled" || name == "visible")
        return g_variant_new_boolean(id != 4 || sharing);
      if (name == "type")
        return g_variant_new_string("standard");
      if (name == "children-display")
        return g_variant_new_string(id == 0 ? "submenu" : "");
      if (name == "icon-name")
        return g_variant_new_string(
            id == 1       ? "window-restore"
                : id == 2 ? "view-refresh"
                : id == 3 ? "application-exit"
                          : ""
        );
      return nullptr;
    }
    GVariant* properties(int id, GVariant* names, bool sharing) {
      GVariantBuilder result;
      g_variant_builder_init(&result, G_VARIANT_TYPE_VARDICT);
      if (!names || g_variant_n_children(names) == 0) {
        for (auto name : {"label", "enabled", "visible", "type", "children-display", "icon-name"})
          g_variant_builder_add(&result, "{sv}", name, menuProperty(id, name, sharing));
      } else {
        GVariantIter iterator;
        const char* name;
        g_variant_iter_init(&iterator, names);
        while (g_variant_iter_next(&iterator, "&s", &name))
          if (auto* value = menuProperty(id, name, sharing))
            g_variant_builder_add(&result, "{sv}", name, value);
      }
      return g_variant_builder_end(&result);
    }
    GVariant* layout(int id, int depth, GVariant* names, bool sharing) {
      GVariantBuilder children;
      g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
      if (id == 0 && depth != 0)
        for (int child = 1; child < static_cast<int>(labels.size()); ++child)
          if (child != 4 || sharing)
            g_variant_builder_add(&children, "v", layout(child, 0, names, sharing));
      return g_variant_new("(i@a{sv}@av)", id, properties(id, names, sharing), g_variant_builder_end(&children));
    }
  } // namespace
  struct DesktopTray::Impl {
    process::WakeEvent wake;
    GMainContext* context = g_main_context_new();
    GMainLoop* loop = g_main_loop_new(context, false);
    GCancellable* cancel = g_cancellable_new();
    std::jthread worker;
    std::atomic<bool> stopping = false;
    GDBusConnection* bus = nullptr;
    std::mutex mutex;
    std::deque<TrayEvent> events;
    std::string icon;
    DesktopActivationToken activationToken;
    GVariant* pixmap = nullptr;
    bool available = false;
    bool sharing = false;
    unsigned menuRevision = 1;
    struct Watcher {
      Impl* self;
      const char* name;
      bool registered = false, host = false;
      guint watch = 0, signal = 0;
    };
    std::array<Watcher, 2> watchers{
        {{this, "org.kde.StatusNotifierWatcher"}, {this, "org.freedesktop.StatusNotifierWatcher"}}
    };
    ~Impl() {
      stopping = true;
      g_cancellable_cancel(cancel);
      if (worker.joinable()) {
        auto* source = g_idle_source_new();
        g_source_set_callback(
            source,
            [](gpointer data) -> gboolean {
              g_main_loop_quit(static_cast<Impl*>(data)->loop);
              return G_SOURCE_REMOVE;
            },
            this, nullptr
        );
        g_source_attach(source, context);
        g_source_unref(source);
        worker.join();
      }
      if (pixmap)
        g_variant_unref(pixmap);
      g_object_unref(cancel);
      g_main_loop_unref(loop);
      g_main_context_unref(context);
    }
    void emit(TrayEvent event) {
      if (stopping)
        return;
      {
        std::lock_guard lock(mutex);
        if (event.command == TrayCommand::Availability)
          std::erase_if(events, [](const auto& queued) { return queued.command == TrayCommand::Availability; });
        if (events.size() >= maximumPendingEvents)
          events.pop_front();
        events.push_back(std::move(event));
      }
      (void)wake.signal();
    }
    void updateAvailability() {
      const bool next =
          std::ranges::any_of(watchers, [](const auto& watcher) { return watcher.registered && watcher.host; });
      if (next != available) {
        available = next;
        logger.info("host availability changed available={}", next);
        emit({.available = next});
      }
    }
    void command(TrayCommand value) {
      logger.debug("command received command={}", static_cast<unsigned>(value));
      TrayEvent event{.command = value};
      if (value == TrayCommand::Open)
        event.activationToken = activationToken.take();
      else
        activationToken.clear();
      emit(std::move(event));
    }
    void menuEvent(GVariant* event) {
      gint32 id;
      const char* kind;
      GVariant* data;
      guint32 timestamp;
      g_variant_get(event, "(i&svu)", &id, &kind, &data, &timestamp);
      g_variant_unref(data);
      if (id > 0 && validId(id) && (id != 4 || sharing) && std::string_view(kind) == "clicked")
        command(
            id == 1       ? TrayCommand::Open
                : id == 2 ? TrayCommand::Restart
                : id == 4 ? TrayCommand::StopSharing
                          : TrayCommand::Close
        );
    }
    void method(const char* interface, const char* name, GVariant* args, GDBusMethodInvocation* invocation) {
      const std::string_view method(name);
      if (std::string_view(interface) != menuInterface) {
        if (method == "Activate" || method == "SecondaryActivate")
          command(TrayCommand::Open);
        else if (method == "ProvideXdgActivationToken") {
          const char* token;
          g_variant_get(args, "(&s)", &token);
          activationToken.assign(token);
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
      }
      if (method == "GetLayout") {
        gint32 id, depth;
        GVariant* names;
        g_variant_get(args, "(ii@as)", &id, &depth, &names);
        if (validId(id))
          g_dbus_method_invocation_return_value(
              invocation, g_variant_new("(u@(ia{sv}av))", menuRevision, layout(id, depth, names, sharing))
          );
        else
          g_dbus_method_invocation_return_error(
              invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Unknown menu item"
          );
        g_variant_unref(names);
      } else if (method == "GetGroupProperties") {
        GVariant *ids, *names;
        g_variant_get(args, "(@ai@as)", &ids, &names);
        GVariantBuilder result;
        g_variant_builder_init(&result, G_VARIANT_TYPE("a(ia{sv})"));
        if (g_variant_n_children(ids) == 0) {
          for (int id = 0; id < static_cast<int>(labels.size()); ++id)
            if (id != 4 || sharing)
              g_variant_builder_add(&result, "(i@a{sv})", id, properties(id, names, sharing));
        } else {
          GVariantIter iterator;
          gint32 id;
          g_variant_iter_init(&iterator, ids);
          while (g_variant_iter_next(&iterator, "i", &id))
            if (validId(id))
              g_variant_builder_add(&result, "(i@a{sv})", id, properties(id, names, sharing));
        }
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&result))
        );
        g_variant_unref(ids);
        g_variant_unref(names);
      } else if (method == "GetProperty") {
        gint32 id;
        const char* name;
        g_variant_get(args, "(i&s)", &id, &name);
        auto* value = validId(id) ? menuProperty(id, name, sharing) : nullptr;
        if (value)
          g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
        else
          g_dbus_method_invocation_return_error(
              invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Unknown menu property"
          );
      } else if (method == "Event") {
        menuEvent(args);
        g_dbus_method_invocation_return_value(invocation, nullptr);
      } else if (method == "EventGroup") {
        auto* group = g_variant_get_child_value(args, 0);
        GVariantBuilder errors;
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        for (gsize i = 0; i < g_variant_n_children(group) && i < maximumPendingEvents; ++i) {
          auto* event = g_variant_get_child_value(group, i);
          menuEvent(event);
          g_variant_unref(event);
        }
        g_variant_unref(group);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai)", g_variant_builder_end(&errors)));
      } else if (method == "AboutToShow")
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", false));
      else if (method == "AboutToShowGroup") {
        GVariantBuilder updates, errors;
        g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@ai@ai)", g_variant_builder_end(&updates), g_variant_builder_end(&errors))
        );
      }
    }
    GVariant* property(const char* interface, std::string_view name) {
      if (std::string_view(interface) == menuInterface) {
        if (name == "Version")
          return g_variant_new_uint32(menuVersion);
        if (name == "TextDirection")
          return g_variant_new_string("ltr");
        if (name == "Status")
          return g_variant_new_string("normal");
        if (name == "IconThemePath")
          return g_variant_new_strv(nullptr, 0);
      }
      if (name == "Id")
        return g_variant_new_string("cressida");
      if (name == "Title")
        return g_variant_new_string("Cressida");
      if (name == "Category")
        return g_variant_new_string("Communications");
      if (name == "Status")
        return g_variant_new_string("Active");
      if (name == "IconName")
        return g_variant_new_string(icon.c_str());
      if (name == "Menu")
        return g_variant_new_object_path(menuPath);
      if (name == "ItemIsMenu")
        return g_variant_new_boolean(false);
      if (name == "WindowId")
        return std::string_view(interface) == "org.kde.StatusNotifierItem" ? g_variant_new_int32(0)
                                                                           : g_variant_new_uint32(0);
      if (name == "IconPixmap")
        return g_variant_ref(pixmap);
      if (name == "ToolTip")
        return g_variant_new("(s@a(iiay)ss)", "", g_variant_ref(pixmap), "Cressida", "Open Cressida");
      if (name.ends_with("Pixmap"))
        return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0);
      return g_variant_new_string("");
    }
    void registerWatcher(Watcher& watcher, const char* owner) {
      watcher.registered = watcher.host = false;
      auto* result = g_dbus_connection_call_sync(
          bus, owner, watcherPath, watcher.name, "RegisterStatusNotifierItem",
          g_variant_new("(s)", g_dbus_connection_get_unique_name(bus)), nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START,
          callDeadlineMs, cancel, nullptr
      );
      if (result) {
        watcher.registered = true;
        g_variant_unref(result);
        result = g_dbus_connection_call_sync(
            bus, owner, watcherPath, "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", watcher.name, "IsStatusNotifierHostRegistered"), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NO_AUTO_START, callDeadlineMs, cancel, nullptr
        );
        if (result) {
          GVariant* value;
          g_variant_get(result, "(v)", &value);
          watcher.host = g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value);
          g_variant_unref(value);
          g_variant_unref(result);
        }
      }
      if (!watcher.registered && !stopping)
        logger.warn("host registration failed stage=register");
      else if (!watcher.host && !stopping)
        logger.debug("registered watcher has no active host");
      updateAvailability();
    }
    void run() {
      g_main_context_push_thread_default(context);
      logger.debug("worker starting");
      GVariantBuilder pixels;
      g_variant_builder_init(&pixels, G_VARIANT_TYPE("a(iiay)"));
      for (const auto size : {Style::baseGlyphSize, Style::glyphSizeDisplay, Style::glyphSizeDisplay * 2}) {
        const auto image = loadImageFile(icon, static_cast<int>(size));
        if (!image)
          continue;
        auto argb = image->rgba;
        for (std::size_t i = 0; i + 3 < argb.size(); i += 4) {
          argb[i] = image->rgba[i + 3];
          argb[i + 1] = image->rgba[i];
          argb[i + 2] = image->rgba[i + 1];
          argb[i + 3] = image->rgba[i + 2];
        }
        g_variant_builder_add(
            &pixels, "(ii@ay)", image->width, image->height,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, argb.data(), argb.size(), 1)
        );
      }
      pixmap = g_variant_ref_sink(g_variant_builder_end(&pixels));
      if (g_variant_n_children(pixmap) == 0)
        logger.warn("tray icon fallback unavailable stage=rasterize");
      auto* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, cancel, nullptr);
      if (address) {
        bus = g_dbus_connection_new_for_address_sync(
            address,
            GDBusConnectionFlags(
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
            ),
            nullptr, cancel, nullptr
        );
        g_free(address);
      }
      if (!bus && !stopping)
        logger.warn("tray unavailable stage=session-bus");
      if (bus) {
        logger.debug("session bus connected");
        g_dbus_connection_set_exit_on_close(bus, false);
        std::string fdBody(itemBody);
        fdBody.replace(
            fdBody.find("name=\"WindowId\" type=\"i\""), std::string("name=\"WindowId\" type=\"i\"").size(),
            "name=\"WindowId\" type=\"u\""
        );
        const auto xml = "<node><interface name=\"org.kde.StatusNotifierItem\">"
            + std::string(itemBody)
            + "</interface><interface name=\"org.freedesktop.StatusNotifierItem\">"
            + fdBody
            + "</interface></node>";
        auto* info = g_dbus_node_info_new_for_xml(xml.c_str(), nullptr);
        auto* menu = g_dbus_node_info_new_for_xml(menuXml, nullptr);
        static const GDBusInterfaceVTable table{
            .method_call = [](GDBusConnection*, const gchar*, const gchar*, const gchar* interface, const gchar* method,
                              GVariant* args, GDBusMethodInvocation* invocation,
                              gpointer data) { static_cast<Impl*>(data)->method(interface, method, args, invocation); },
            .get_property = [](GDBusConnection*, const gchar*, const gchar*, const gchar* interface, const gchar* name,
                               GError**, gpointer data) -> GVariant* {
              return static_cast<Impl*>(data)->property(interface, name);
            }
        };
        std::array<guint, 3> objects{
            g_dbus_connection_register_object(bus, itemPath, info->interfaces[0], &table, this, nullptr, nullptr),
            g_dbus_connection_register_object(bus, itemPath, info->interfaces[1], &table, this, nullptr, nullptr),
            g_dbus_connection_register_object(bus, menuPath, menu->interfaces[0], &table, this, nullptr, nullptr)
        };
        if (std::ranges::any_of(objects, [](guint object) { return object == 0; }) && !stopping)
          logger.warn("tray export failed stage=interfaces");
        else
          logger.debug("tray interfaces exported");
        g_signal_connect(
            bus, "closed", G_CALLBACK(+[](GDBusConnection*, gboolean, GError*, gpointer data) {
              auto& self = *static_cast<Impl*>(data);
              if (!self.stopping)
                logger.warn("session bus disconnected");
              for (auto& watcher : self.watchers)
                watcher.registered = watcher.host = false;
              self.updateAvailability();
            }),
            this
        );
        for (auto& watcher : watchers) {
          watcher.signal = g_dbus_connection_signal_subscribe(
              bus, watcher.name, watcher.name, nullptr, watcherPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
              [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* signal, GVariant*,
                 gpointer data) {
                auto& watcher = *static_cast<Watcher*>(data);
                const std::string_view name(signal);
                if (name == "StatusNotifierHostRegistered")
                  watcher.host = true;
                else if (name == "StatusNotifierHostUnregistered")
                  watcher.host = false;
                watcher.self->updateAvailability();
              },
              &watcher, nullptr
          );
          watcher.watch = g_bus_watch_name_on_connection(
              bus, watcher.name, G_BUS_NAME_WATCHER_FLAGS_NONE,
              [](GDBusConnection*, const gchar*, const gchar* owner, gpointer data) {
                auto& watcher = *static_cast<Watcher*>(data);
                watcher.self->registerWatcher(watcher, owner);
              },
              [](GDBusConnection*, const gchar*, gpointer data) {
                auto& watcher = *static_cast<Watcher*>(data);
                watcher.registered = watcher.host = false;
                watcher.self->updateAvailability();
              },
              &watcher, nullptr
          );
        }
        if (!stopping)
          g_main_loop_run(loop);
        for (auto& watcher : watchers) {
          g_bus_unwatch_name(watcher.watch);
          g_dbus_connection_signal_unsubscribe(bus, watcher.signal);
        }
        for (const auto object : objects)
          if (object)
            g_dbus_connection_unregister_object(bus, object);
        g_dbus_node_info_unref(info);
        g_dbus_node_info_unref(menu);
        g_dbus_connection_close_sync(bus, nullptr, nullptr);
        g_object_unref(bus);
        bus = nullptr;
      }
      while (g_main_context_pending(context))
        g_main_context_iteration(context, false);
      g_main_context_pop_thread_default(context);
      logger.debug("worker stopped");
    }
  };
  DesktopTray::DesktopTray() : m(std::make_unique<Impl>()) {}
  DesktopTray::~DesktopTray() = default;
  void DesktopTray::start(std::string iconPath) {
    if (m->worker.joinable())
      return;
    m->icon = std::move(iconPath);
    m->worker = std::jthread([this] { m->run(); });
  }
  int DesktopTray::eventFd() const { return m->wake.fd(); }
  void DesktopTray::sharing(bool active) {
    struct Change {
      Impl* impl;
      bool active;
    };
    g_main_context_invoke_full(
        m->context, G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          const auto& change = *static_cast<Change*>(data);
          auto& self = *change.impl;
          if (self.stopping || self.sharing == change.active)
            return G_SOURCE_REMOVE;
          self.sharing = change.active;
          ++self.menuRevision;
          if (self.bus)
            g_dbus_connection_emit_signal(
                self.bus, nullptr, menuPath, menuInterface, "LayoutUpdated",
                g_variant_new("(ui)", self.menuRevision, 0), nullptr
            );
          return G_SOURCE_REMOVE;
        },
        new Change{m.get(), active}, [](gpointer data) { delete static_cast<Change*>(data); }
    );
  }
  std::optional<TrayEvent> DesktopTray::poll() {
    m->wake.drain();
    std::lock_guard lock(m->mutex);
    if (m->events.empty())
      return {};
    auto event = std::move(m->events.front());
    m->events.pop_front();
    return event;
  }
} // namespace app
