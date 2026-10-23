#include "app/desktop_notifications.h"

#include "app/desktop_activation.h"
#include "core/log.h"
#include "core/process/wake_event.h"
#include "render/core/image_decoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <gio/gio.h>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace app {
  namespace {
    constexpr Logger logger{"notifications"};
    constexpr auto service = "org.freedesktop.Notifications";
    constexpr auto path = "/org/freedesktop/Notifications";
    constexpr std::size_t maxAvatarBytes = 256 * 1024;
    // Avatar routes request static PNGs. Check the canvas before decoding so
    // malformed or unexpectedly large images cannot expand on this worker.
    GVariant* avatarImage(std::span<const std::uint8_t> bytes) {
      constexpr std::array<std::uint8_t, 16> header{137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
      if (bytes.size() < 33 || bytes.size() > maxAvatarBytes || !std::ranges::equal(bytes.first(16), header))
        return nullptr;
      const auto dimension = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t i = offset; i < offset + 4; ++i)
          value = (value << 8) | bytes[i];
        return value;
      };
      const auto width = dimension(16), height = dimension(20);
      if (!width || !height || width > 512 || height > 512)
        return nullptr;
      auto image = decodeRasterImage(bytes.data(), bytes.size());
      if (!image
          || image->width <= 0
          || image->height <= 0
          || image->width > 512
          || image->height > 512
          || image->pixels.size() != std::size_t(image->width) * image->height * 4)
        return nullptr;
      // The notification service receives straight RGBA, so shape the avatar
      // here rather than relying on the desktop's image corner treatment.
      const float centerX = image->width / 2.0F, centerY = image->height / 2.0F;
      const float radius = std::min(centerX, centerY);
      for (int y = 0; y < image->height; ++y)
        for (int x = 0; x < image->width; ++x) {
          const float coverage =
              std::clamp(radius + .5F - std::hypot(x + .5F - centerX, y + .5F - centerY), 0.0F, 1.0F);
          auto& alpha = image->pixels[(std::size_t(y) * image->width + x) * 4 + 3];
          alpha = static_cast<std::uint8_t>(std::lround(alpha * coverage));
        }
      return g_variant_new(
          "(iiibii@ay)", image->width, image->height, image->width * 4, true, 8, 4,
          g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, image->pixels.data(), image->pixels.size(), 1)
      );
    }
    std::string plain(std::string text, bool markup = false) {
      if (!g_utf8_validate(text.data(), text.size(), nullptr))
        return {};
      if (const auto zero = text.find('\0'); zero != std::string::npos)
        text.resize(zero);
      if (text.size() > 1024) {
        std::size_t end = 1024;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
          --end;
        text.resize(end);
      }
      if (!markup)
        return text;
      auto* escaped = g_markup_escape_text(text.data(), text.size());
      std::string result = escaped;
      g_free(escaped);
      return result;
    }
  } // namespace
  struct DesktopNotifications::Impl {
    process::WakeEvent wake;
    GMainContext* context = g_main_context_new();
    GMainLoop* loop = g_main_loop_new(context, false);
    GCancellable* cancel = g_cancellable_new();
    GDBusConnection* bus = nullptr;
    std::jthread worker;
    std::atomic<bool> stopping = false, scheduled = false;
    bool bodyMarkup = false;
    bool deliveryFailed = false, capacityWarning = false, queueWarning = false;
    std::mutex mutex;
    struct Queued {
      DesktopNotice notice;
      std::chrono::steady_clock::time_point deadline;
      bool avatarReady;
    };
    std::deque<Queued> queue;
    GSource* avatarDeadline = nullptr;
    std::deque<DesktopNoticeAction> actions;
    std::uint64_t epoch = 1;
    struct Active {
      DesktopNoticeAction target;
      std::uint64_t epoch;
      DesktopActivationToken activationToken;
    };
    std::map<guint32, Active> active;
    bool clearPending = false;
    unsigned pendingCalls = 0;
    void cancelDeadline() {
      if (avatarDeadline) {
        g_source_destroy(avatarDeadline);
        g_source_unref(avatarDeadline);
        avatarDeadline = nullptr;
      }
    }
    ~Impl() {
      stopping = true;
      g_cancellable_cancel(cancel);
      if (worker.joinable()) {
        schedule();
        worker.join();
      }
      g_object_unref(cancel);
      g_main_loop_unref(loop);
      g_main_context_unref(context);
    }
    void schedule() {
      if (scheduled.exchange(true))
        return;
      auto* source = g_idle_source_new();
      g_source_set_callback(
          source,
          [](void* data) -> gboolean {
            auto* self = static_cast<Impl*>(data);
            self->scheduled = false;
            self->flush();
            return G_SOURCE_REMOVE;
          },
          this, nullptr
      );
      g_source_attach(source, context);
      g_source_unref(source);
    }
    void close(guint32 id) {
      if (bus)
        g_dbus_connection_call(
            bus, service, path, service, "CloseNotification", g_variant_new("(u)", id), nullptr,
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, stopping ? nullptr : cancel, nullptr, nullptr
        );
    }
    void flush() {
      cancelDeadline();
      if (stopping) {
        g_main_loop_quit(loop);
        return;
      }
      std::deque<DesktopNotice> batch;
      std::uint64_t current;
      bool clear;
      const auto now = std::chrono::steady_clock::now();
      auto deadline = std::chrono::steady_clock::time_point::max();
      {
        std::lock_guard lock(mutex);
        for (auto it = queue.begin(); it != queue.end();) {
          if (it->avatarReady || it->deadline <= now) {
            batch.push_back(std::move(it->notice));
            it = queue.erase(it);
          } else {
            deadline = std::min(deadline, it->deadline);
            ++it;
          }
        }
        current = epoch;
        clear = std::exchange(clearPending, false);
      }
      if (deadline != std::chrono::steady_clock::time_point::max()) {
        const auto delay = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        avatarDeadline = g_timeout_source_new(static_cast<guint>(std::max<std::int64_t>(1, delay)));
        g_source_set_callback(
            avatarDeadline,
            [](void* data) -> gboolean {
              static_cast<Impl*>(data)->flush();
              return G_SOURCE_REMOVE;
            },
            this, nullptr
        );
        g_source_attach(avatarDeadline, context);
      }
      if (clear) {
        for (const auto& [id, _] : active)
          close(id);
        active.clear();
      }
      if (!bus)
        return;
      for (const auto& notice : batch) {
        if (pendingCalls >= 32) {
          if (!capacityWarning) {
            capacityWarning = true;
            logger.warn("delivery capacity reached; some notifications discarded");
          }
          break;
        }
        GVariantBuilder hints;
        g_variant_builder_init(&hints, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&hints, "{sv}", "category", g_variant_new_string("im.received"));
        g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(desktopApplicationId));
        g_variant_builder_add(&hints, "{sv}", "suppress-sound", g_variant_new_boolean(!notice.sound));
        if (notice.sound)
          g_variant_builder_add(&hints, "{sv}", "sound-name", g_variant_new_string("message-new-instant"));
        if (auto* image = avatarImage(notice.avatarBytes))
          g_variant_builder_add(&hints, "{sv}", "image-data", image);
        const char* buttons[] = {"default", "Open conversation", nullptr};
        struct Pending {
          Impl* owner;
          DesktopNoticeAction target;
          std::uint64_t epoch;
        };
        auto* pending = new Pending{
            this, {notice.account, notice.guild, notice.channel, notice.message, notice.generation}, current
        };
        // Body markup is escaped before crossing the notification service boundary.
        ++pendingCalls;
        g_dbus_connection_call(
            bus, service, path, service, "Notify",
            g_variant_new(
                "(susss^as@a{sv}i)", "Cressida", 0U, desktopApplicationId, plain(notice.title).c_str(),
                plain(notice.body, bodyMarkup).c_str(), buttons, g_variant_builder_end(&hints), 7000
            ),
            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, cancel,
            [](GObject* object, GAsyncResult* result, void* data) {
              std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
              --pending->owner->pendingCalls;
              GError* error = nullptr;
              auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
              if (reply) {
                if (pending->owner->deliveryFailed) {
                  pending->owner->deliveryFailed = false;
                  logger.debug("desktop delivery recovered");
                }
                guint32 id = 0;
                g_variant_get(reply, "(u)", &id);
                g_variant_unref(reply);
                auto& self = *pending->owner;
                bool valid;
                {
                  std::lock_guard lock(self.mutex);
                  valid = pending->epoch == self.epoch;
                }
                if (valid && !self.stopping && id) {
                  if (self.active.size() >= 128) {
                    self.close(self.active.begin()->first);
                    self.active.erase(self.active.begin());
                  }
                  self.active[id] = Active{std::move(pending->target), pending->epoch};
                } else
                  self.close(id);
              }
              if (!reply && !pending->owner->stopping && !pending->owner->deliveryFailed) {
                pending->owner->deliveryFailed = true;
                logger.warn("desktop delivery failed stage=notify repeated_warnings=suppressed");
              }
              if (error)
                g_error_free(error);
            },
            pending
        );
      }
    }
    void run() {
      g_main_context_push_thread_default(context);
      logger.debug("worker starting");
      bus = g_bus_get_sync(G_BUS_TYPE_SESSION, cancel, nullptr);
      guint action = 0, closed = 0, activation = 0;
      if (!bus && !stopping)
        logger.warn("desktop notifications unavailable stage=session-bus");
      if (bus) {
        logger.debug("session bus connected");
        // Only the optional notification body supports markup. A server without
        // that capability receives ordinary text, including literal '&'/'<'.
        auto* capabilities = g_dbus_connection_call_sync(
            bus, service, path, service, "GetCapabilities", nullptr, G_VARIANT_TYPE("(as)"),
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, cancel, nullptr
        );
        if (capabilities) {
          GVariant* values = g_variant_get_child_value(capabilities, 0);
          GVariantIter it;
          g_variant_iter_init(&it, values);
          const char* capability;
          while (g_variant_iter_next(&it, "&s", &capability))
            if (std::string_view(capability) == "body-markup")
              bodyMarkup = true;
          g_variant_unref(values);
          g_variant_unref(capabilities);
        } else if (!stopping)
          logger.debug("notification capabilities unavailable");
        activation = g_dbus_connection_signal_subscribe(
            bus, service, service, "ActivationToken", path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* value, void* data) {
              if (!g_variant_is_of_type(value, G_VARIANT_TYPE("(us)")))
                return;
              auto& self = *static_cast<Impl*>(data);
              guint32 id;
              const char* token;
              g_variant_get(value, "(u&s)", &id, &token);
              const auto found = self.active.find(id);
              if (found != self.active.end())
                found->second.activationToken.assign(token);
            },
            this, nullptr
        );
        action = g_dbus_connection_signal_subscribe(
            bus, service, service, "ActionInvoked", path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* value, void* data) {
              auto& self = *static_cast<Impl*>(data);
              guint32 id;
              const char* key;
              if (!g_variant_is_of_type(value, G_VARIANT_TYPE("(us)")))
                return;
              g_variant_get(value, "(u&s)", &id, &key);
              const auto found = self.active.find(id);
              if (found == self.active.end() || std::string_view(key) != "default")
                return;
              {
                std::lock_guard lock(self.mutex);
                if (found->second.epoch == self.epoch && self.actions.size() < 32) {
                  auto target = std::move(found->second.target);
                  target.activationToken = found->second.activationToken.take();
                  self.actions.push_back(std::move(target));
                }
              }
              self.close(id);
              self.active.erase(found);
              (void)self.wake.signal();
            },
            this, nullptr
        );
        closed = g_dbus_connection_signal_subscribe(
            bus, service, service, "NotificationClosed", path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* value, void* data) {
              if (!g_variant_is_of_type(value, G_VARIANT_TYPE("(uu)")))
                return;
              guint32 id, reason;
              g_variant_get(value, "(uu)", &id, &reason);
              static_cast<Impl*>(data)->active.erase(id);
            },
            this, nullptr
        );
      }
      if (!stopping)
        g_main_loop_run(loop);
      cancelDeadline();
      if (bus) {
        for (const auto& [id, _] : active)
          close(id);
        active.clear();
        g_dbus_connection_signal_unsubscribe(bus, activation);
        g_dbus_connection_signal_unsubscribe(bus, action);
        g_dbus_connection_signal_unsubscribe(bus, closed);
        g_object_unref(bus);
        bus = nullptr;
      }
      // Drain cancelled call completions before releasing their owner.
      while (pendingCalls)
        g_main_context_iteration(context, true);
      while (g_main_context_pending(context))
        g_main_context_iteration(context, false);
      g_main_context_pop_thread_default(context);
      logger.debug("worker stopped");
    }
  };
  DesktopNotifications::DesktopNotifications() : m(std::make_unique<Impl>()) {}
  DesktopNotifications::~DesktopNotifications() = default;
  void DesktopNotifications::start() {
    if (!m->worker.joinable())
      m->worker = std::jthread([this] { m->run(); });
  }
  void DesktopNotifications::show(DesktopNotice notice) {
    if (!m->worker.joinable() || m->stopping || notice.account.empty())
      return;
    {
      std::lock_guard lock(m->mutex);
      if (m->queue.size() == 32) {
        if (!m->queueWarning) {
          m->queueWarning = true;
          logger.warn("queue capacity reached; oldest notification discarded");
        }
        m->queue.pop_front();
      }
      const bool ready = notice.avatar.empty() || !notice.avatarBytes.empty();
      if (notice.avatarBytes.size() > maxAvatarBytes)
        notice.avatarBytes.clear();
      m->queue.push_back({std::move(notice), std::chrono::steady_clock::now() + std::chrono::milliseconds(750), ready});
    }
    m->schedule();
  }
  void DesktopNotifications::avatarLoaded(const std::string& path, std::span<const std::uint8_t> bytes) {
    bool changed = false;
    {
      std::lock_guard lock(m->mutex);
      for (auto& queued : m->queue)
        if (!queued.avatarReady && queued.notice.avatar == path) {
          if (bytes.size() <= maxAvatarBytes)
            queued.notice.avatarBytes.assign(bytes.begin(), bytes.end());
          queued.avatarReady = true;
          changed = true;
        }
    }
    if (changed)
      m->schedule();
  }
  void DesktopNotifications::clear() {
    {
      std::lock_guard lock(m->mutex);
      ++m->epoch;
      m->queue.clear();
      m->actions.clear();
      m->clearPending = true;
    }
    if (m->worker.joinable())
      m->schedule();
  }
  int DesktopNotifications::eventFd() const { return m->wake.fd(); }
  std::optional<DesktopNoticeAction> DesktopNotifications::poll() {
    m->wake.drain();
    std::lock_guard lock(m->mutex);
    if (m->actions.empty())
      return {};
    auto result = std::move(m->actions.front());
    m->actions.pop_front();
    return result;
  }
} // namespace app
