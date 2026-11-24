#include "app/desktop_activation.h"
#include "app/desktop_notifications.h"
#include "render/core/image_encoder.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <gio/gio.h>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  template <class Predicate> void waitFor(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      if (predicate())
        return;
      (void)::poll(nullptr, 0, 2);
    }
    throw std::runtime_error(message);
  }
  void dispatchFor(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
      while (g_main_context_iteration(nullptr, false)) {
      };
      (void)::poll(nullptr, 0, 2);
    }
  }
  struct Capture {
    unsigned id = 0;
    std::string application, icon, summary, body, desktop, category, sound;
    std::vector<std::string> actions;
    bool suppress = false;
    int timeout = 0;
    int width = 0, height = 0, rowstride = 0, bits = 0, channels = 0;
    gboolean alpha = false;
    std::vector<std::uint8_t> image;
  };
  class Service {
  public:
    GTestDBus* test = g_test_dbus_new(G_TEST_DBUS_NONE);
    GDBusConnection* connection = nullptr;
    GDBusNodeInfo* info = nullptr;
    guint object = 0;
    bool markup = true, hold = false;
    unsigned serial = 0;
    std::vector<Capture> captures;
    std::vector<std::pair<unsigned, GDBusMethodInvocation*>> pending;
    std::set<unsigned> closed;
    Service() {
      g_test_dbus_up(test);
      connection = g_dbus_connection_new_for_address_sync(
          g_test_dbus_get_bus_address(test),
          static_cast<GDBusConnectionFlags>(
              G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
          ),
          nullptr, nullptr, nullptr
      );
      require(connection, "Connect to the isolated notification test bus");
      info = g_dbus_node_info_new_for_xml(
          "<node><interface name='org.freedesktop.Notifications'>"
          "<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
          "<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/>"
          "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/>"
          "<arg type='as' direction='in'/><arg type='a{sv}' direction='in'/><arg type='i' direction='in'/>"
          "<arg type='u' direction='out'/></method>"
          "<method name='CloseNotification'><arg type='u' direction='in'/></method>"
          "<signal name='ActivationToken'><arg type='u'/><arg type='s'/></signal>"
          "<signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>"
          "<signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>"
          "</interface></node>",
          nullptr
      );
      static const GDBusInterfaceVTable table{
          .method_call = [](GDBusConnection*, const char*, const char*, const char*, const char* method,
                            GVariant* values, GDBusMethodInvocation* invocation, void* data) {
            auto& self = *static_cast<Service*>(data);
            if (std::string_view(method) == "GetCapabilities") {
              const char* capabilities[] = {"actions", "body", self.markup ? "body-markup" : nullptr, nullptr};
              g_dbus_method_invocation_return_value(invocation, g_variant_new("(^as)", capabilities));
            } else if (std::string_view(method) == "Notify") {
              Capture capture;
              capture.id = ++self.serial;
              const char *application, *icon, *summary, *body;
              guint32 replaces;
              GVariant *actions, *hints;
              g_variant_get(
                  values, "(&su&s&s&s@as@a{sv}i)", &application, &replaces, &icon, &summary, &body, &actions, &hints,
                  &capture.timeout
              );
              capture.application = application;
              capture.icon = icon;
              capture.summary = summary;
              capture.body = body;
              const char* value = nullptr;
              gboolean suppress = false;
              if (g_variant_lookup(hints, "desktop-entry", "&s", &value))
                capture.desktop = value;
              if (g_variant_lookup(hints, "category", "&s", &value))
                capture.category = value;
              if (g_variant_lookup(hints, "sound-name", "&s", &value))
                capture.sound = value;
              if (g_variant_lookup(hints, "suppress-sound", "b", &suppress))
                capture.suppress = suppress;
              if (auto* image = g_variant_lookup_value(hints, "image-data", G_VARIANT_TYPE("(iiibiiay)"))) {
                GVariant* pixels;
                g_variant_get(
                    image, "(iiibii@ay)", &capture.width, &capture.height, &capture.rowstride, &capture.alpha,
                    &capture.bits, &capture.channels, &pixels
                );
                gsize size = 0;
                const auto* data = static_cast<const std::uint8_t*>(g_variant_get_fixed_array(pixels, &size, 1));
                capture.image.assign(data, data + size);
                g_variant_unref(pixels);
                g_variant_unref(image);
              }
              GVariantIter it;
              g_variant_iter_init(&it, actions);
              while (g_variant_iter_next(&it, "&s", &value))
                capture.actions.emplace_back(value);
              g_variant_unref(actions);
              g_variant_unref(hints);
              self.captures.push_back(std::move(capture));
              if (self.hold)
                self.pending.emplace_back(self.serial, G_DBUS_METHOD_INVOCATION(g_object_ref(invocation)));
              else
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", self.serial));
            } else {
              guint id;
              g_variant_get(values, "(u)", &id);
              self.closed.insert(id);
              g_dbus_method_invocation_return_value(invocation, nullptr);
              g_dbus_connection_emit_signal(
                  self.connection, nullptr, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                  "NotificationClosed", g_variant_new("(uu)", id, 3U), nullptr
              );
            }
          }
      };
      object = g_dbus_connection_register_object(
          connection, "/org/freedesktop/Notifications", info->interfaces[0], &table, this, nullptr, nullptr
      );
      require(object != 0, "Register the fake notification service");
      auto* reply = g_dbus_connection_call_sync(
          connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
          g_variant_new("(su)", "org.freedesktop.Notifications", 0U), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE,
          1000, nullptr, nullptr
      );
      require(reply, "Own only the private fake notification service");
      g_variant_unref(reply);
    }
    void token(unsigned id, const std::string& value) {
      require(
          g_dbus_connection_emit_signal(
              connection, nullptr, "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "ActivationToken",
              g_variant_new("(us)", id, value.c_str()), nullptr
          ),
          "Emit a synthetic notification activation token"
      );
      g_dbus_connection_flush_sync(connection, nullptr, nullptr);
    }
    void action(unsigned id, const char* key = "default") {
      require(
          g_dbus_connection_emit_signal(
              connection, nullptr, "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "ActionInvoked",
              g_variant_new("(us)", id, key), nullptr
          ),
          "Emit a synthetic desktop action"
      );
      g_dbus_connection_flush_sync(connection, nullptr, nullptr);
    }
    void reply() {
      for (const auto& [id, call] : pending) {
        g_dbus_method_invocation_return_value(call, g_variant_new("(u)", id));
        g_object_unref(call);
      }
      pending.clear();
    }
    ~Service() {
      reply();
      g_dbus_connection_unregister_object(connection, object);
      g_dbus_node_info_unref(info);
      g_dbus_connection_close_sync(connection, nullptr, nullptr);
      g_object_unref(connection);
      g_test_dbus_down(test);
      g_object_unref(test);
    }
  };
  app::DesktopNotice notice(std::string title = "Alice <Admin>", std::string body = "<b>hello</b> & 'world'") {
    return {
        .account = "42",
        .guild = "100",
        .channel = "101",
        .message = "200",
        .title = std::move(title),
        .body = std::move(body),
        .sound = true,
        .generation = 17
    };
  }
} // namespace
int main() {
  try {
    g_setenv("GIO_USE_VFS", "local", true);
    Service service;
    {
      app::DesktopNotifications notifications;
      notifications.show(notice("Before start"));
      notifications.start();
      notifications.start();
      notifications.show(notice());
      waitFor([&] { return service.captures.size() == 1; }, "Worker delivers notification to private service");
      const auto first = service.captures.front();
      require(
          first.application == "Cressida"
              && first.icon == app::desktopApplicationId
              && first.desktop == app::desktopApplicationId
              && first.category == "im.received",
          "Notification identity uses native desktop metadata"
      );
      require(
          first.summary == "Alice <Admin>" && first.body == "&lt;b&gt;hello&lt;/b&gt; &amp; &apos;world&apos;",
          "Only markup-capable body is escaped; summary remains plain text"
      );
      require(
          first.actions == std::vector<std::string>{"default", "Open conversation"},
          "Notification has one explicit native conversation action"
      );
      require(
          !first.suppress && first.sound == "message-new-instant" && first.timeout == 7000,
          "Enabled notification sounds and expiry use documented hints"
      );
      require(first.image.empty(), "A notice without sender identity never contains an avatar");
      require(!notifications.poll(), "Delivering a notification does not activate its conversation");
      service.token(first.id, "private-notification-token");
      dispatchFor(std::chrono::milliseconds(30));
      require(!notifications.poll(), "An activation token alone does not activate its conversation");
      service.action(first.id, "unrecognized");
      dispatchFor(std::chrono::milliseconds(30));
      require(!notifications.poll(), "Unrecognized notification actions do not navigate");
      service.action(first.id);
      pollfd wake{notifications.eventFd(), POLLIN, 0};
      waitFor([&] { return ::poll(&wake, 1, 0) > 0; }, "Desktop action wakes the owning native loop");
      const auto target = notifications.poll();
      require(
          target
              && target->account == "42"
              && target->guild == "100"
              && target->channel == "101"
              && target->message == "200"
              && target->generation == 17
              && target->activationToken == "private-notification-token",
          "Notification action preserves session/account/conversation identity"
      );
      waitFor([&] { return service.closed.contains(first.id); }, "Activating notification closes its presentation");
      service.action(first.id);
      dispatchFor(std::chrono::milliseconds(30));
      require(!notifications.poll(), "Repeated desktop actions cannot reopen expired targets");

      service.token(first.id, "expired-target-token");
      notifications.show(notice("New target"));
      waitFor([&] { return service.captures.size() == 2; }, "Create a separate activation target");
      dispatchFor(std::chrono::milliseconds(30));
      const auto secondId = service.captures.back().id;
      service.action(secondId);
      waitFor([&] { return ::poll(&wake, 1, 0) > 0; }, "Tokenless action wakes the owning loop");
      require(notifications.poll()->activationToken.empty(), "An old notification token cannot leak into a new target");
      notifications.show(notice("Oversized token"));
      waitFor([&] { return service.captures.size() == 3; }, "Create a bounded activation target");
      dispatchFor(std::chrono::milliseconds(30));
      const auto oversizedId = service.captures.back().id;
      service.token(oversizedId, std::string(app::DesktopActivationToken::maximumBytes + 1, 'x'));
      service.action(oversizedId);
      waitFor([&] { return ::poll(&wake, 1, 0) > 0; }, "Oversized tokens cannot block the action");
      require(notifications.poll()->activationToken.empty(), "Overlong activation handles are rejected");
      service.captures.resize(1);
      auto silent = notice("Muted", std::string(1023, 'a') + "🙂");
      silent.sound = false;
      notifications.show(std::move(silent));
      waitFor([&] { return service.captures.size() == 2; }, "Deliver silent bounded notification");
      const auto second = service.captures.back();
      require(second.suppress && second.sound.empty(), "Disabled sound uses suppress-sound without a sound-name");
      require(
          second.body.size() == 1023 && g_utf8_validate(second.body.data(), second.body.size(), nullptr),
          "Text limit never splits a UTF-8 character"
      );
      notifications.clear();
      service.action(second.id);
      waitFor([&] { return service.closed.contains(second.id); }, "Account reset closes active desktop notifications");
      require(!notifications.poll(), "Action racing with reset is rejected by the notification epoch");

      service.hold = true;
      notifications.show(notice("Pending before reset"));
      waitFor([&] { return !service.pending.empty(); }, "Fake service holds a notification acknowledgement");
      const auto late = service.pending.front().first;
      notifications.clear();
      service.reply();
      waitFor(
          [&] { return service.closed.contains(late); }, "Late notification acknowledgements are closed after reset"
      );
      service.action(late);
      dispatchFor(std::chrono::milliseconds(30));
      require(!notifications.poll(), "Late callbacks cannot resurrect a stale desktop target");
      service.hold = false;
    }
    {
      app::DesktopNotifications notifications;
      notifications.start();
      std::vector<std::uint8_t> rgba(32 * 32 * 4);
      for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
          const auto pixel = (y * 32 + x) * 4;
          rgba[pixel] = 220;
          rgba[pixel + 1] = 40;
          rgba[pixel + 2] = 60;
          rgba[pixel + 3] = x < 16 ? 255 : 127;
        }
      const auto png = encodePng(rgba.data(), 32, 32);
      require(!png.empty(), "Build an isolated avatar fixture with transparency");
      std::size_t expected = service.captures.size();
      const auto awaitNotice = [&] {
        ++expected;
        waitFor([&] { return service.captures.size() == expected; }, "Deliver avatar notification");
      };
      const auto checkImage = [&] {
        const auto& image = service.captures.back();
        require(
            image.width == 32
                && image.height == 32
                && image.rowstride == 128
                && image.alpha
                && image.bits == 8
                && image.channels == 4
                && image.image.size() == rgba.size()
                && image.icon == app::desktopApplicationId,
            "Standard image-data carries the sender avatar and separate application icon"
        );
        bool antialiased = false;
        for (std::size_t pixel = 0; pixel < rgba.size(); pixel += 4) {
          require(
              image.image[pixel] == rgba[pixel]
                  && image.image[pixel + 1] == rgba[pixel + 1]
                  && image.image[pixel + 2] == rgba[pixel + 2]
                  && image.image[pixel + 3] <= rgba[pixel + 3],
              "Circular masking preserves RGB and never increases the source opacity"
          );
          antialiased |= image.image[pixel + 3] > 0 && image.image[pixel + 3] < rgba[pixel + 3];
        }
        require(
            image.image[3] == 0
                && image.image[(31 * 32 + 31) * 4 + 3] == 0
                && image.image[(16 * 32 + 15) * 4 + 3] == 255
                && image.image[(16 * 32 + 16) * 4 + 3] == 127
                && antialiased,
            "Notification avatars have transparent corners, smooth circular edges and unchanged center alpha"
        );
      };
      auto cached = notice("Cached avatar");
      cached.avatar = "/avatars/42/cached.png?size=128";
      cached.avatarBytes = png;
      notifications.show(std::move(cached));
      awaitNotice();
      checkImage();

      auto downloading = notice("Downloading avatar");
      downloading.avatar = "/avatars/43/new.png?size=128";
      notifications.show(downloading);
      dispatchFor(std::chrono::milliseconds(40));
      require(service.captures.size() == expected, "Uncached avatar briefly waits for the shared asset worker");
      notifications.avatarLoaded("/unrelated.png", png);
      dispatchFor(std::chrono::milliseconds(20));
      require(service.captures.size() == expected, "Unrelated image results cannot supply a sender avatar");
      notifications.avatarLoaded(downloading.avatar, png);
      awaitNotice();
      checkImage();

      // Completion may arrive before the notification worker has seen its queue.
      notifications.show(downloading);
      notifications.avatarLoaded(downloading.avatar, png);
      awaitNotice();
      checkImage();

      notifications.show(downloading);
      notifications.avatarLoaded(downloading.avatar, {});
      awaitNotice();
      require(service.captures.back().image.empty(), "Failed downloads fall back to the application icon");

      auto invalid = downloading;
      invalid.avatarBytes = {1, 2, 3};
      notifications.show(invalid);
      awaitNotice();
      require(service.captures.back().image.empty(), "Malformed avatar data cannot enter notification hints");
      invalid.avatarBytes = png;
      invalid.avatarBytes[16] = 1; // Oversized PNG canvas is rejected before decoding.
      notifications.show(invalid);
      awaitNotice();
      require(service.captures.back().image.empty(), "Avatar dimensions are bounded before allocation");
      invalid.avatarBytes.assign(256 * 1024 + 1, 0);
      notifications.show(std::move(invalid));
      awaitNotice();
      require(service.captures.back().image.empty(), "Encoded avatar size is bounded");

      const auto start = std::chrono::steady_clock::now();
      notifications.show(downloading);
      awaitNotice();
      require(
          service.captures.back().image.empty() && std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
          "Missing avatar results cannot indefinitely delay a desktop notification"
      );
      notifications.avatarLoaded(downloading.avatar, png);
      dispatchFor(std::chrono::milliseconds(40));
      require(service.captures.size() == expected, "A late image does not duplicate an expired delivery");

      notifications.show(downloading);
      dispatchFor(std::chrono::milliseconds(20));
      notifications.clear();
      notifications.avatarLoaded(downloading.avatar, png);
      dispatchFor(std::chrono::milliseconds(800));
      require(service.captures.size() == expected, "Reset cancels pending avatar deliveries and their deadlines");
      notifications.show(downloading);
      notifications.avatarLoaded(downloading.avatar, png);
      awaitNotice();
      checkImage();
      notifications.show(downloading); // Destruction must also cancel the image deadline.
    }
    service.markup = false;
    {
      const auto before = service.captures.size();
      app::DesktopNotifications notifications;
      notifications.start();
      notifications.show(notice("Plain & simple", "<xml> & literal"));
      waitFor(
          [&] { return service.captures.size() == before + 1; }, "Non-markup notification server receives plain content"
      );
      require(
          service.captures.back().summary == "Plain & simple" && service.captures.back().body == "<xml> & literal",
          "Servers without markup do not show escaped XML entities"
      );
      const auto active = service.captures.back().id;
      notifications.clear();
      waitFor([&] { return service.closed.contains(active); }, "Explicit notification clear closes the active item");
    }
    service.hold = true;
    {
      auto notifications = std::make_unique<app::DesktopNotifications>();
      notifications->start();
      const auto before = service.captures.size();
      for (int i = 0; i < 1000; ++i)
        notifications->show(notice("Bounded queue"));
      waitFor(
          [&] { return service.captures.size() > before; }, "Stress delivery reaches isolated notification service"
      );
      dispatchFor(std::chrono::milliseconds(60));
      require(
          service.captures.size() - before <= 32, "Notification requests are bounded even while all replies are held"
      );
      const auto start = std::chrono::steady_clock::now();
      notifications.reset();
      require(
          std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
          "Shutdown cancels outstanding D-Bus calls without waiting for the service"
      );
      service.reply();
    }
    std::cout << "Private desktop notification delivery, actions, reset and cancellation passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
