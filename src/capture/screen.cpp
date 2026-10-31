#include "capture/screen.h"

#include "capture/pipewire_video.h"
#include "core/process/wake_event.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <mutex>
#include <optional>
#include <thread>
#include <unistd.h>
#include <utility>

namespace capture {
  namespace {
    constexpr auto desktopName = "org.freedesktop.portal.Desktop";
    constexpr auto desktopPath = "/org/freedesktop/portal/desktop";
    constexpr auto screenInterface = "org.freedesktop.portal.ScreenCast";
    constexpr int rpcTimeout = 5000, closeTimeout = 1000, selectionTimeoutSeconds = 300;
    constexpr auto workerInterval = std::chrono::milliseconds{25};
    constexpr auto firstFrameDeadline = std::chrono::seconds{5};
    constexpr std::uint32_t monitorSource = 1, windowSource = 2, hiddenCursor = 1, embeddedCursor = 2,
                            metadataCursor = 4;
    using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
    Variant own(GVariant* value) { return {value, g_variant_unref}; }
    std::string token() {
      auto* uuid = g_uuid_string_random();
      std::string result = "cressida_" + std::string(uuid);
      g_free(uuid);
      std::ranges::replace(result, '-', '_');
      return result;
    }
    struct Reply {
      explicit Reply(GMainContext* context) : loop(g_main_loop_new(context, false)) {}
      ~Reply() {
        g_main_loop_unref(loop);
        if (values)
          g_variant_unref(values);
      }
      GMainLoop* loop;
      std::string path;
      GVariant* values = nullptr;
      bool received = false, cancelled = false;
    };
    using ReplyRef = std::shared_ptr<Reply>;
    struct PortalSession {
      GDBusConnection* connection = nullptr;
      std::string path;
      guint subscription = 0;
      std::atomic_bool closed = false;
      std::function<void()> onClosed;
      ~PortalSession() {
        if (!connection)
          return;
        if (subscription)
          g_dbus_connection_signal_unsubscribe(connection, subscription);
        if (!path.empty()) {
          auto reply = own(g_dbus_connection_call_sync(
              connection, desktopName, path.c_str(), "org.freedesktop.portal.Session", "Close", nullptr, nullptr,
              G_DBUS_CALL_FLAGS_NONE, closeTimeout, nullptr, nullptr
          ));
        }
        g_object_unref(connection);
      }
    };
    Variant request(
        GDBusConnection* connection, GMainContext* context, GCancellable* cancellation, const char* method,
        GVariant* arguments, const std::string& handle
    ) {
      auto state = std::make_shared<Reply>(context);
      std::string sender = g_dbus_connection_get_unique_name(connection);
      if (sender.starts_with(':'))
        sender.erase(0, 1);
      std::ranges::replace(sender, '.', '_');
      state->path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + handle;
      const auto subscription = g_dbus_connection_signal_subscribe(
          connection, desktopName, "org.freedesktop.portal.Request", "Response", nullptr, nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE,
          [](GDBusConnection*, const gchar*, const gchar* path, const gchar*, const gchar*, GVariant* parameters,
             gpointer data) {
            auto& state = **static_cast<ReplyRef*>(data);
            if (state.path != path || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})")) || state.received)
              return;
            guint response = 0;
            GVariant* values = nullptr;
            g_variant_get(parameters, "(u@a{sv})", &response, &values);
            state.received = true;
            state.cancelled = response == 1;
            if (response == 0)
              state.values = values;
            else
              g_variant_unref(values);
            g_main_loop_quit(state.loop);
          },
          new ReplyRef(state), [](gpointer data) { delete static_cast<ReplyRef*>(data); }
      );
      const auto cancel = g_cancellable_connect(
          cancellation, G_CALLBACK((+[](GCancellable*, gpointer data) {
            const auto& state = *static_cast<ReplyRef*>(data);
            g_main_context_invoke_full(
                g_main_loop_get_context(state->loop), G_PRIORITY_HIGH,
                [](gpointer data) -> gboolean {
                  auto& state = **static_cast<ReplyRef*>(data);
                  state.cancelled = true;
                  g_main_loop_quit(state.loop);
                  return G_SOURCE_REMOVE;
                },
                new ReplyRef(state), [](gpointer data) { delete static_cast<ReplyRef*>(data); }
            );
          })),
          new ReplyRef(state), [](gpointer data) { delete static_cast<ReplyRef*>(data); }
      );
      auto called = own(g_dbus_connection_call_sync(
          connection, desktopName, desktopPath, screenInterface, method, arguments, G_VARIANT_TYPE("(o)"),
          G_DBUS_CALL_FLAGS_NONE, rpcTimeout, cancellation, nullptr
      ));
      if (called) {
        const gchar* path = nullptr;
        g_variant_get(called.get(), "(&o)", &path);
        state->path = path;
        auto* timeout = g_timeout_source_new_seconds(selectionTimeoutSeconds);
        g_source_set_callback(
            timeout,
            [](gpointer data) -> gboolean {
              g_main_loop_quit(static_cast<Reply*>(data)->loop);
              return G_SOURCE_REMOVE;
            },
            state.get(), nullptr
        );
        g_source_attach(timeout, context);
        if (!state->received && !g_cancellable_is_cancelled(cancellation))
          g_main_loop_run(state->loop);
        g_source_destroy(timeout);
        g_source_unref(timeout);
      }
      if (!state->received) {
        auto closed = own(g_dbus_connection_call_sync(
            connection, desktopName, state->path.c_str(), "org.freedesktop.portal.Request", "Close", nullptr, nullptr,
            G_DBUS_CALL_FLAGS_NONE, closeTimeout, nullptr, nullptr
        ));
      }
      g_cancellable_disconnect(cancellation, cancel);
      g_dbus_connection_signal_unsubscribe(connection, subscription);
      if (state->cancelled)
        g_cancellable_cancel(cancellation);
      if (!state->values || state->cancelled || g_cancellable_is_cancelled(cancellation))
        return own(nullptr);
      return own(g_variant_ref(state->values));
    }
    struct Selection {
      std::unique_ptr<PortalSession> session;
      int descriptor = -1;
      std::uint32_t node = 0, sourceType = 0;
      bool cursor = false;
      ~Selection() {
        if (descriptor >= 0)
          close(descriptor);
      }
    };
    std::unique_ptr<Selection> selectSource(
        GMainContext* context, GCancellable* cancellation, const std::string& parent, bool cursor,
        std::function<void()> closed
    ) {
      auto result = std::make_unique<Selection>();
      result->session = std::make_unique<PortalSession>();
      auto& session = *result->session;
      session.onClosed = std::move(closed);
      session.connection = g_bus_get_sync(G_BUS_TYPE_SESSION, cancellation, nullptr);
      if (!session.connection)
        return {};
      auto properties = own(g_dbus_connection_call_sync(
          session.connection, desktopName, desktopPath, "org.freedesktop.DBus.Properties", "GetAll",
          g_variant_new("(s)", screenInterface), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, rpcTimeout,
          cancellation, nullptr
      ));
      std::uint32_t sourceTypes = 0, cursorModes = 0;
      if (!properties)
        return {};
      auto values = own(g_variant_get_child_value(properties.get(), 0));
      g_variant_lookup(values.get(), "AvailableSourceTypes", "u", &sourceTypes);
      g_variant_lookup(values.get(), "AvailableCursorModes", "u", &cursorModes);
      sourceTypes &= monitorSource | windowSource;
      if (!sourceTypes)
        return {};
      const auto createHandle = token(), sessionHandle = token();
      std::string sender = g_dbus_connection_get_unique_name(session.connection);
      if (sender.starts_with(':'))
        sender.erase(0, 1);
      std::ranges::replace(sender, '.', '_');
      session.path = "/org/freedesktop/portal/desktop/session/" + sender + "/" + sessionHandle;
      GVariantBuilder options;
      g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(createHandle.c_str()));
      g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(sessionHandle.c_str()));
      auto created = request(
          session.connection, context, cancellation, "CreateSession",
          g_variant_new("(@a{sv})", g_variant_builder_end(&options)), createHandle
      );
      const gchar* sessionPath = nullptr;
      if (!created
          || !g_variant_lookup(created.get(), "session_handle", "&s", &sessionPath)
          || !g_variant_is_object_path(sessionPath)
          || !std::string_view(sessionPath).starts_with("/org/freedesktop/portal/desktop/session/"))
        return {};
      session.path = sessionPath;
      session.subscription = g_dbus_connection_signal_subscribe(
          session.connection, desktopName, "org.freedesktop.portal.Session", "Closed", session.path.c_str(), nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE,
          [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data) {
            auto& session = *static_cast<PortalSession*>(data);
            session.closed = true;
            if (session.onClosed)
              session.onClosed();
          },
          &session, nullptr
      );
      const auto sourcesHandle = token();
      g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(sourcesHandle.c_str()));
      g_variant_builder_add(&options, "{sv}", "types", g_variant_new_uint32(sourceTypes));
      g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(false));
      const auto preferredCursor = cursor ? embeddedCursor : hiddenCursor;
      if (!cursor && cursorModes && !(cursorModes & (hiddenCursor | metadataCursor))) {
        g_variant_builder_clear(&options);
        return {};
      }
      const auto selectedCursor = (cursorModes & preferredCursor) ? preferredCursor
          : (cursorModes & hiddenCursor)                          ? hiddenCursor
          : (cursorModes & metadataCursor)                        ? metadataCursor
                                                                  : 0U;
      result->cursor = selectedCursor == embeddedCursor;
      if (selectedCursor)
        g_variant_builder_add(&options, "{sv}", "cursor_mode", g_variant_new_uint32(selectedCursor));
      auto selected = request(
          session.connection, context, cancellation, "SelectSources",
          g_variant_new("(o@a{sv})", session.path.c_str(), g_variant_builder_end(&options)), sourcesHandle
      );
      if (!selected)
        return {};
      const auto startHandle = token();
      g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(startHandle.c_str()));
      auto started = request(
          session.connection, context, cancellation, "Start",
          g_variant_new("(os@a{sv})", session.path.c_str(), parent.c_str(), g_variant_builder_end(&options)),
          startHandle
      );
      if (!started || session.closed)
        return {};
      auto streams = own(g_variant_lookup_value(started.get(), "streams", G_VARIANT_TYPE("a(ua{sv})")));
      if (!streams || g_variant_n_children(streams.get()) != 1)
        return {};
      auto stream = own(g_variant_get_child_value(streams.get(), 0));
      GVariant* attributes = nullptr;
      g_variant_get(stream.get(), "(u@a{sv})", &result->node, &attributes);
      auto attributesRef = own(attributes);
      g_variant_lookup(attributes, "source_type", "u", &result->sourceType);
      g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
      GUnixFDList* descriptors = nullptr;
      auto remote = own(g_dbus_connection_call_with_unix_fd_list_sync(
          session.connection, desktopName, desktopPath, screenInterface, "OpenPipeWireRemote",
          g_variant_new("(o@a{sv})", session.path.c_str(), g_variant_builder_end(&options)), G_VARIANT_TYPE("(h)"),
          G_DBUS_CALL_FLAGS_NONE, rpcTimeout, nullptr, &descriptors, cancellation, nullptr
      ));
      if (!remote || !descriptors) {
        if (descriptors)
          g_object_unref(descriptors);
        return {};
      }
      gint index = -1;
      g_variant_get(remote.get(), "(h)", &index);
      result->descriptor = g_unix_fd_list_get(descriptors, index, nullptr);
      g_object_unref(descriptors);
      if (result->descriptor < 0 || fcntl(result->descriptor, F_SETFD, FD_CLOEXEC) < 0)
        return {};
      return result;
    }
  } // namespace
  struct Screen::Impl {
    struct Command {
      std::uint64_t revision;
      std::string parent;
      bool cursor = true, stop = false;
    };
    mutable std::mutex mutex;
    std::mutex deliveryMutex;
    std::condition_variable_any condition;
    std::optional<Command> pending;
    std::uint64_t revision = 0, activeRevision = 0;
    GCancellable* active = nullptr;
    ScreenState state;
    Frame frame;
    process::WakeEvent wake;
    std::jthread worker;
    explicit Impl(Frame callback) : frame(std::move(callback)), worker([this](std::stop_token stop) { run(stop); }) {}
    ~Impl() {
      {
        std::lock_guard lock(mutex);
        if (active)
          g_cancellable_cancel(active);
      }
      worker.request_stop();
      condition.notify_all();
      if (worker.joinable())
        worker.join();
    }
    void run(std::stop_token stop) {
      auto* context = g_main_context_new();
      g_main_context_push_thread_default(context);
      std::unique_ptr<Selection> selection;
      std::unique_ptr<PipeWireVideo> capture;
      while (!stop.stop_requested()) {
        while (g_main_context_pending(context))
          g_main_context_iteration(context, false);
        std::optional<Command> command;
        {
          std::unique_lock lock(mutex);
          condition.wait_for(lock, stop, workerInterval, [&] { return pending.has_value(); });
          command = std::move(pending);
          pending.reset();
        }
        if (stop.stop_requested())
          break;
        if (selection && selection->session->closed) {
          capture.reset();
          selection.reset();
          std::lock_guard lock(mutex);
          state.phase = ScreenPhase::Failed;
          state.error = "Screen access was stopped by the desktop.";
          (void)wake.signal();
        }
        if (!command)
          continue;
        if (command->stop) {
          capture.reset();
          selection.reset();
          std::lock_guard lock(mutex);
          state = {.revision = command->revision};
          (void)wake.signal();
          continue;
        }
        auto* cancellation = g_cancellable_new();
        ScreenState previous;
        {
          std::lock_guard lock(mutex);
          previous = state;
          active = cancellation;
          if (command->revision != revision)
            g_cancellable_cancel(cancellation);
          state.phase = ScreenPhase::Selecting;
          state.error.clear();
          state.revision = command->revision;
          (void)wake.signal();
        }
        auto chosen =
            selectSource(context, cancellation, command->parent, command->cursor, [this, epoch = command->revision] {
              std::lock_guard lock(mutex);
              condition.notify_all();
              if (activeRevision != epoch)
                return;
              activeRevision = 0;
              state.phase = ScreenPhase::Failed;
              state.error = "Screen access was stopped by the desktop.";
              if (active)
                g_cancellable_cancel(active);
              (void)wake.signal();
            });
        bool cancelled = g_cancellable_is_cancelled(cancellation);
        {
          std::lock_guard lock(mutex);
          active = nullptr;
          cancelled |= command->revision != revision;
        }
        g_object_unref(cancellation);
        if (chosen && !cancelled) {
          struct FirstFrame {
            std::shared_ptr<const voice::VideoFrame> image;
            bool failed = false;
          };
          const auto first = std::make_shared<FirstFrame>();
          auto replacement = std::make_unique<PipeWireVideo>();
          const auto descriptor = std::exchange(chosen->descriptor, -1);
          const auto epoch = command->revision;
          const bool ready = replacement->start(
              descriptor, chosen->node,
              [this, epoch, first](auto image) {
                std::lock_guard delivery(deliveryMutex);
                {
                  std::lock_guard lock(mutex);
                  if (activeRevision != epoch) {
                    first->image = std::move(image);
                    condition.notify_all();
                    return;
                  }
                  if (state.width != image->width || state.height != image->height) {
                    state.width = image->width;
                    state.height = image->height;
                    (void)wake.signal();
                  }
                }
                if (frame)
                  frame(std::move(image));
              },
              [this, epoch, first](std::string error) {
                std::lock_guard lock(mutex);
                first->failed = true;
                condition.notify_all();
                if (activeRevision != epoch)
                  return;
                state.phase = ScreenPhase::Failed;
                state.error = std::move(error);
                (void)wake.signal();
              }
          );
          if (ready) {
            const auto deadline = std::chrono::steady_clock::now() + firstFrameDeadline;
            while (true) {
              while (g_main_context_pending(context))
                g_main_context_iteration(context, false);
              std::unique_lock lock(mutex);
              if (first->image
                  || first->failed
                  || revision != epoch
                  || stop.stop_requested()
                  || chosen->session->closed
                  || (selection && selection->session->closed)
                  || std::chrono::steady_clock::now() >= deadline)
                break;
              condition.wait_until(
                  lock, stop, std::min(deadline, std::chrono::steady_clock::now() + workerInterval),
                  [&] { return first->image || first->failed || revision != epoch; }
              );
            }
            std::shared_ptr<const voice::VideoFrame> image;
            std::unique_lock delivery(deliveryMutex);
            {
              std::lock_guard lock(mutex);
              if (revision != epoch || stop.stop_requested())
                continue;
              const bool revoked = selection && selection->session->closed;
              if (!first->image || first->failed || chosen->session->closed || revoked) {
                state = previous;
                state.revision = command->revision;
                state.phase = !revoked && selection ? ScreenPhase::Ready : ScreenPhase::Failed;
                state.error = revoked || chosen->session->closed
                    ? "Screen access was stopped by the desktop."
                    : "The selected source did not provide a video frame. Choose another source.";
                (void)wake.signal();
                continue;
              }
              image = first->image;
              activeRevision = epoch;
              state = {
                  .phase = ScreenPhase::Ready,
                  .revision = epoch,
                  .name = chosen->sourceType == windowSource ? "Window" : "Screen",
                  .error = command->cursor && !chosen->cursor
                      ? "This desktop does not include the cursor in shared frames."
                      : "",
                  .cursor = chosen->cursor
              };
              state.width = image->width;
              state.height = image->height;
              (void)wake.signal();
            }
            // Delivery is serialized with source callbacks before retiring the old graph.
            if (frame)
              frame(std::move(image));
            delivery.unlock();
            capture.reset();
            selection.reset();
            capture = std::move(replacement);
            selection = std::move(chosen);
            continue;
          }
        }
        std::lock_guard lock(mutex);
        if (command->revision != revision)
          continue;
        state = previous;
        state.revision = command->revision;
        const bool revoked = selection && selection->session->closed;
        state.phase = revoked ? ScreenPhase::Failed
            : selection       ? ScreenPhase::Ready
            : cancelled       ? ScreenPhase::Idle
                              : ScreenPhase::Failed;
        if (revoked)
          state.error = "Screen access was stopped by the desktop.";
        if (!cancelled)
          state.error = "Screen selection failed or was cancelled. Choose a source to try again.";
        (void)wake.signal();
      }
      capture.reset();
      selection.reset();
      while (g_main_context_pending(context))
        g_main_context_iteration(context, false);
      g_main_context_pop_thread_default(context);
      g_main_context_unref(context);
    }
  };
  Screen::Screen(Frame frame) : m(std::make_unique<Impl>(std::move(frame))) {}
  Screen::~Screen() = default;
  void Screen::select(std::string parent, bool cursor) {
    if (parent.size() > 4096 || (!parent.empty() && !parent.starts_with("wayland:")))
      parent.clear();
    std::lock_guard lock(m->mutex);
    if (m->active)
      g_cancellable_cancel(m->active);
    m->pending = Impl::Command{++m->revision, std::move(parent), cursor, false};
    m->condition.notify_all();
  }
  void Screen::cancelSelection() {
    std::lock_guard lock(m->mutex);
    if (m->active)
      g_cancellable_cancel(m->active);
    if (m->state.phase == ScreenPhase::Selecting || (m->pending && !m->pending->stop)) {
      ++m->revision;
      m->pending.reset();
      m->state.phase = m->activeRevision ? ScreenPhase::Ready : ScreenPhase::Idle;
      m->state.revision = m->revision;
      (void)m->wake.signal();
      m->condition.notify_all();
    }
  }
  void Screen::stop() {
    std::lock_guard delivery(m->deliveryMutex);
    std::lock_guard lock(m->mutex);
    if (m->active)
      g_cancellable_cancel(m->active);
    m->activeRevision = 0;
    m->state = {.revision = m->revision + 1};
    (void)m->wake.signal();
    m->pending = Impl::Command{++m->revision, {}, true, true};
    m->condition.notify_all();
  }
  ScreenState Screen::state() const {
    std::lock_guard lock(m->mutex);
    return m->state;
  }
  int Screen::eventFd() const { return m->wake.fd(); }
  void Screen::drainEvents() { m->wake.drain(); }
} // namespace capture
