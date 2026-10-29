#include "app/file_picker.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <gio/gio.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <webp/decode.h>

namespace app {
  namespace {
    constexpr std::size_t maximumFile = 10 * 1024 * 1024;
    bool cropDimensions(std::span<const std::uint8_t> bytes, std::string_view mime) {
      int w = 0, h = 0;
      const auto bounded = [&] { return w > 0 && h > 0 && w <= 4096 && h <= 4096 && std::int64_t(w) * h <= 8388608; };
      if (mime == "image/webp") {
        return WebPGetInfo(bytes.data(), bytes.size(), &w, &h) && bounded();
      }
      if (mime == "image/png" && bytes.size() >= 24) {
        const auto be = [&](std::size_t at) {
          return std::uint32_t(bytes[at]) << 24
              | std::uint32_t(bytes[at + 1]) << 16
              | std::uint32_t(bytes[at + 2]) << 8
              | bytes[at + 3];
        };
        if (be(16) > 4096 || be(20) > 4096)
          return false;
        w = be(16);
        h = be(20);
        return bounded();
      }
      if (mime == "image/gif" && bytes.size() >= 10) {
        w = bytes[6] | bytes[7] << 8;
        h = bytes[8] | bytes[9] << 8;
        return bounded();
      }
      if (mime == "image/jpeg" && bytes.size() >= 4 && bytes[0] == 0xff && bytes[1] == 0xd8) {
        std::size_t at = 2;
        while (at + 4 <= bytes.size()) {
          if (bytes[at++] != 0xff)
            return false;
          while (at < bytes.size() && bytes[at] == 0xff)
            ++at;
          if (at >= bytes.size())
            return false;
          const auto marker = bytes[at++];
          if (marker == 0xd9 || marker == 0xda)
            return false;
          if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
            continue;
          if (at + 2 > bytes.size())
            return false;
          const std::size_t length = bytes[at] << 8 | bytes[at + 1];
          if (length < 2 || length > bytes.size() - at)
            return false;
          if ((marker >= 0xc0 && marker <= 0xcf) && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (length < 8)
              return false;
            h = bytes[at + 3] << 8 | bytes[at + 4];
            w = bytes[at + 5] << 8 | bytes[at + 6];
            return bounded();
          }
          at += length;
        }
      }
      return false;
    }
    struct Selection {
      explicit Selection(GMainContext* context) : loop(g_main_loop_new(context, false)) {}
      ~Selection() { g_main_loop_unref(loop); }
      GMainLoop* loop;
      std::string path, uri, error;
      std::vector<std::string> uris;
      bool multiple = false;
      bool received = false, cancelled = false;
    };
    using SelectionRef = std::shared_ptr<Selection>;
    PickedFile selectFile(
        GMainContext* context, GCancellable* cancellation, std::uint64_t id, const std::string& title,
        const std::vector<std::string>& types, std::size_t maximum, bool multiple, bool saving, const std::string& name,
        std::span<const std::uint8_t> bytes
    ) {
      PickedFile result{.request = id};
      GError* error = nullptr;
      auto* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, cancellation, &error);
      if (!connection) {
        result.cancelled = g_cancellable_is_cancelled(cancellation);
        if (!result.cancelled)
          result.error = "The desktop file chooser is unavailable.";
        g_clear_error(&error);
        return result;
      }
      auto state = std::make_shared<Selection>(context);
      state->multiple = multiple;
      const auto token = "cressida_" + std::to_string(getpid()) + "_" + std::to_string(id);
      std::string sender = g_dbus_connection_get_unique_name(connection);
      if (sender.starts_with(':'))
        sender.erase(0, 1);
      std::ranges::replace(sender, '.', '_');
      state->path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
      const auto subscription = g_dbus_connection_signal_subscribe(
          connection, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request", "Response", nullptr, nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE,
          [](GDBusConnection*, const gchar*, const gchar* path, const gchar*, const gchar*, GVariant* parameters,
             gpointer data) {
            const auto& selection = *static_cast<SelectionRef*>(data);
            if (selection->path != path || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})")))
              return;
            guint response;
            GVariant* values = nullptr;
            g_variant_get(parameters, "(u@a{sv})", &response, &values);
            selection->received = true;
            selection->cancelled = response == 1;
            if (response == 0) {
              gchar** uris = nullptr;
              if (g_variant_lookup(values, "uris", "^as", &uris) && uris && uris[0]) {
                for (std::size_t i = 0; uris[i]; ++i) {
                  if (i >= 50 || (!selection->multiple && i > 0)) {
                    selection->error =
                        selection->multiple ? "Select no more than 50 local files." : "Select exactly one local file.";
                    break;
                  }
                  selection->uris.emplace_back(uris[i]);
                }
                selection->uri = uris[0];
              } else
                selection->error = "Choose a local file.";
              g_strfreev(uris);
            } else if (!selection->cancelled)
              selection->error = "The desktop file chooser could not select this file.";
            g_variant_unref(values);
            g_main_loop_quit(selection->loop);
          },
          new SelectionRef(state), [](gpointer data) { delete static_cast<SelectionRef*>(data); }
      );
      const auto cancelled = g_cancellable_connect(
          cancellation, G_CALLBACK((+[](GCancellable*, gpointer data) {
            const auto& selection = *static_cast<SelectionRef*>(data);
            g_main_context_invoke_full(
                g_main_loop_get_context(selection->loop), G_PRIORITY_HIGH,
                [](gpointer data) -> gboolean {
                  const auto& state = *static_cast<SelectionRef*>(data);
                  state->cancelled = true;
                  g_main_loop_quit(state->loop);
                  return G_SOURCE_REMOVE;
                },
                new SelectionRef(selection), [](gpointer data) { delete static_cast<SelectionRef*>(data); }
            );
          })),
          new SelectionRef(state), [](gpointer data) { delete static_cast<SelectionRef*>(data); }
      );
      GVariantBuilder options, filters, choices;
      g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
      g_variant_builder_add(&options, "{sv}", "modal", g_variant_new_boolean(true));
      if (saving)
        g_variant_builder_add(&options, "{sv}", "current_name", g_variant_new_string(name.c_str()));
      else
        g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(multiple));
      g_variant_builder_add(
          &options, "{sv}", "accept_label",
          g_variant_new_string(
              saving         ? "Save Image"
                  : multiple ? "Choose Files"
                             : "Choose File"
          )
      );
      g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
      g_variant_builder_init(&choices, G_VARIANT_TYPE("a(us)"));
      for (const auto& type : types)
        g_variant_builder_add(&choices, "(us)", type == "*/*" ? 0U : 1U, type == "*/*" ? "*" : type.c_str());
      g_variant_builder_add(&filters, "(s@a(us))", "Supported files", g_variant_builder_end(&choices));
      g_variant_builder_add(&options, "{sv}", "filters", g_variant_builder_end(&filters));
      auto* reply = g_dbus_connection_call_sync(
          connection, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
          "org.freedesktop.portal.FileChooser", saving ? "SaveFile" : "OpenFile",
          g_variant_new("(ss@a{sv})", "", title.c_str(), g_variant_builder_end(&options)), G_VARIANT_TYPE("(o)"),
          G_DBUS_CALL_FLAGS_NONE, 5000, cancellation, &error
      );
      if (reply) {
        const gchar* handle;
        g_variant_get(reply, "(&o)", &handle);
        state->path = handle;
        g_variant_unref(reply);
        auto* timeout = g_timeout_source_new_seconds(300);
        g_source_set_callback(
            timeout,
            [](gpointer data) -> gboolean {
              auto& state = *static_cast<Selection*>(data);
              state.error = "File selection timed out. Try again.";
              g_main_loop_quit(state.loop);
              return G_SOURCE_REMOVE;
            },
            state.get(), nullptr
        );
        g_source_attach(timeout, context);
        if (!g_cancellable_is_cancelled(cancellation))
          g_main_loop_run(state->loop);
        else
          state->cancelled = true;
        g_source_destroy(timeout);
        g_source_unref(timeout);
      } else if (!g_cancellable_is_cancelled(cancellation))
        state->error = "The desktop file chooser is unavailable. Check your portal service.";
      else
        state->cancelled = true;
      g_clear_error(&error);
      if (!state->received)
        g_dbus_connection_call(
            connection, "org.freedesktop.portal.Desktop", state->path.c_str(), "org.freedesktop.portal.Request",
            "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr, nullptr
        );
      g_cancellable_disconnect(cancellation, cancelled);
      g_dbus_connection_signal_unsubscribe(connection, subscription);
      while (g_main_context_pending(context))
        g_main_context_iteration(context, false);
      g_object_unref(connection);
      result.cancelled = state->cancelled || g_cancellable_is_cancelled(cancellation);
      result.error = state->error;
      if (!result.cancelled && result.error.empty() && !state->uri.empty()) {
        if (saving)
          result = FilePicker::writeSelection(state->uri, bytes, cancellation);
        else if (multiple) {
          std::size_t total = 0;
          for (const auto& uri : state->uris) {
            if (g_cancellable_is_cancelled(cancellation)) {
              result.cancelled = true;
              result.files.clear();
              break;
            }
            auto file = FilePicker::readSelection(uri, types, maximum);
            total += file.bytes.size();
            if (total > 50 * 256 * 1024) {
              result.files.clear();
              result.error = "Choose files totalling no more than 12.5 MiB.";
              break;
            }
            result.files.push_back(std::move(file));
          }
        } else
          result = FilePicker::readSelection(state->uri, types, maximum);
        result.request = id;
      }
      return result;
    }
  } // namespace

  FilePicker::FilePicker() : m_worker([this](std::stop_token stop) { run(stop); }) {}
  FilePicker::~FilePicker() {
    m_worker.request_stop();
    cancel();
    m_condition.notify_all();
    m_worker.join();
  }
  void FilePicker::open(
      std::uint64_t id, std::string title, std::vector<std::string> types, std::size_t maximum, bool multiple
  ) {
    cancel();
    {
      std::lock_guard lock(m_mutex);
      m_pending = Request{
          .id = id,
          .title = std::move(title),
          .types = std::move(types),
          .maximum = std::min(maximumFile, maximum),
          .multiple = multiple
      };
    }
    m_condition.notify_all();
  }
  void FilePicker::cancel() {
    std::lock_guard lock(m_mutex);
    m_pending.reset();
    if (m_active)
      g_cancellable_cancel(m_active);
  }
  void FilePicker::save(std::uint64_t id, std::string name, std::string mime, std::vector<std::uint8_t> bytes) {
    auto* validName = g_utf8_make_valid(name.data(), name.size());
    name = validName;
    g_free(validName);
    cancel();
    {
      std::lock_guard lock(m_mutex);
      m_pending = Request{
          .id = id,
          .title = "Save Image",
          .types = {std::move(mime)},
          .maximum = maximumSavedBytes,
          .saving = true,
          .name = std::move(name),
          .bytes = std::move(bytes)
      };
    }
    m_condition.notify_all();
  }
  std::optional<PickedFile> FilePicker::poll() {
    m_wake.drain();
    std::lock_guard lock(m_mutex);
    if (m_results.empty())
      return {};
    auto result = std::move(m_results.front());
    m_results.pop_front();
    return result;
  }
  void FilePicker::run(std::stop_token stop) {
    auto* context = g_main_context_new();
    g_main_context_push_thread_default(context);
    while (!stop.stop_requested()) {
      Request request;
      auto* cancellation = g_cancellable_new();
      {
        std::unique_lock lock(m_mutex);
        m_condition.wait(lock, stop, [&] { return m_pending.has_value(); });
        if (stop.stop_requested()) {
          g_object_unref(cancellation);
          break;
        }
        request = std::move(*m_pending);
        m_pending.reset();
        m_active = cancellation;
      }
      PickedFile result;
      if (request.saving && (request.bytes.empty() || request.bytes.size() > maximumSavedBytes))
        result.error = "This image is empty or too large to save.";
      else
        result = selectFile(
            context, cancellation, request.id, request.title, request.types, request.maximum, request.multiple,
            request.saving, request.name, request.bytes
        );
      result.request = request.id;
      result.saving = request.saving;
      const auto raster = [&](PickedFile& file, bool thumbnail) {
        if (g_cancellable_is_cancelled(cancellation)
            || !file.error.empty()
            || file.cancelled
            || !file.mime.starts_with("image/")
            || !cropDimensions(file.bytes, file.mime))
          return;
        auto decoded = decodeRasterImage(file.bytes.data(), file.bytes.size());
        if (!decoded)
          return;
        if (thumbnail && (decoded->width > 128 || decoded->height > 128)) {
          DecodedRasterImage small;
          const auto scale = 128.0 / std::max(decoded->width, decoded->height);
          small.width = std::max(1, int(decoded->width * scale));
          small.height = std::max(1, int(decoded->height * scale));
          small.pixels.resize(small.width * small.height * 4);
          for (int y = 0; y < small.height; ++y)
            for (int x = 0; x < small.width; ++x) {
              const auto source = (std::size_t(y) * decoded->height / small.height * decoded->width
                                   + std::size_t(x) * decoded->width / small.width)
                  * 4;
              std::copy_n(decoded->pixels.data() + source, 4, small.pixels.data() + (y * small.width + x) * 4);
            }
          file.raster = std::move(small);
        } else
          file.raster = std::move(*decoded);
      };
      if (!result.saving) {
        if (result.files.empty())
          raster(result, false);
        else
          for (auto& file : result.files)
            raster(file, true);
      }
      if (g_cancellable_is_cancelled(cancellation)) {
        result.cancelled = true;
        result.bytes.clear();
        result.raster.reset();
        result.files.clear();
      }
      {
        std::lock_guard lock(m_mutex);
        m_active = nullptr;
        m_results.clear();
        m_results.push_back(std::move(result));
        g_object_unref(cancellation);
      }
      (void)m_wake.signal();
    }
    while (g_main_context_pending(context))
      g_main_context_iteration(context, false);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
  }
  PickedFile
  FilePicker::readSelection(const std::string& uri, std::span<const std::string> types, std::size_t maximum) {
    PickedFile result;
    if (!uri.starts_with("file://") || uri.size() > 16384 || uri.find('\0') != std::string::npos) {
      result.error = "Select a local file.";
      return result;
    }
    gchar* host = nullptr;
    auto* filename = g_filename_from_uri(uri.c_str(), &host, nullptr);
    const bool local = filename && filename[0] == '/' && (!host || !*host || std::string_view(host) == "localhost");
    g_free(host);
    if (!local) {
      g_free(filename);
      result.error = "Select a local file.";
      return result;
    }
    const int descriptor = ::open(filename, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    auto* basename = g_path_get_basename(filename);
    result.name = basename;
    g_free(basename);
    g_free(filename);
    struct Descriptor {
      int value;
      ~Descriptor() {
        if (value >= 0)
          ::close(value);
      }
    } file{descriptor};
    struct stat information{};
    if (descriptor < 0 || ::fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode)) {
      result.error = "Could not read the selected file.";
      return result;
    }
    maximum = std::min(maximum, maximumFile);
    std::array<char, 16384> buffer;
    for (;;) {
      const auto read = ::read(descriptor, buffer.data(), buffer.size());
      if (read < 0 && errno == EINTR)
        continue;
      if (read < 0) {
        result.bytes.clear();
        result.error = "Could not read the selected file.";
        return result;
      }
      if (read == 0)
        break;
      const auto count = static_cast<std::size_t>(read);
      if (result.bytes.size() + count > maximum) {
        result.bytes.clear();
        result.error = "The selected file is too large.";
        return result;
      }
      result.bytes.insert(result.bytes.end(), buffer.data(), buffer.data() + count);
    }
    if (result.bytes.empty()) {
      result.error = "The selected file is empty or could not be read.";
      result.bytes.clear();
      return result;
    }
    auto* content = g_content_type_guess(nullptr, result.bytes.data(), result.bytes.size(), nullptr);
    auto* mime = content ? g_content_type_get_mime_type(content) : nullptr;
    result.mime = mime ? mime : "application/octet-stream";
    if ((result.mime == "application/ogg" || result.mime == "audio/x-vorbis+ogg" || result.mime == "audio/x-opus+ogg")
        && result.bytes.size() >= 4
        && std::equal(result.bytes.begin(), result.bytes.begin() + 4, "OggS"))
      result.mime = "audio/ogg";
    g_free(mime);
    g_free(content);
    if (std::ranges::find(types, "*/*") == types.end() && std::ranges::find(types, result.mime) == types.end()) {
      result.bytes.clear();
      result.error = "This file type is not supported for this upload.";
    }
    return result;
  }
  PickedFile
  FilePicker::writeSelection(const std::string& uri, std::span<const std::uint8_t> bytes, GCancellable* cancellation) {
    PickedFile result{.saving = true};
    const auto cancelled = [&] { return cancellation && g_cancellable_is_cancelled(cancellation); };
    if (cancelled()) {
      result.cancelled = true;
      return result;
    }
    if (bytes.empty() || bytes.size() > maximumSavedBytes) {
      result.error = "This image is empty or too large to save.";
      return result;
    }
    if (!uri.starts_with("file://") || uri.size() > 16384 || uri.find('\0') != std::string::npos) {
      result.error = "Choose a local file destination.";
      return result;
    }
    gchar* host = nullptr;
    auto* path = g_filename_from_uri(uri.c_str(), &host, nullptr);
    const bool local = path && path[0] == '/' && (!host || !*host || std::string_view(host) == "localhost");
    g_free(host);
    if (!local) {
      g_free(path);
      result.error = "Choose a local file destination.";
      return result;
    }
    auto* parent = g_path_get_dirname(path);
    auto* basename = g_path_get_basename(path);
    result.name = basename;
    g_free(basename);
    g_free(path);
    const int directory = ::open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    g_free(parent);
    struct Directory {
      int fd;
      ~Directory() {
        if (fd >= 0)
          ::close(fd);
      }
    } directoryGuard{directory};
    struct stat target{};
    const int status = directory < 0 ? -1 : ::fstatat(directory, result.name.c_str(), &target, AT_SYMLINK_NOFOLLOW);
    if (directory < 0 || (status == 0 && !S_ISREG(target.st_mode)) || (status < 0 && errno != ENOENT)) {
      result.error = "Could not save to the selected file.";
      return result;
    }
    auto* random = g_uuid_string_random();
    const std::string temporary = ".cressida-image-" + std::string(random);
    g_free(random);
    const int descriptor =
        ::openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct Temporary {
      int directory, fd;
      const std::string& name;
      ~Temporary() {
        if (fd >= 0) {
          ::close(fd);
          ::unlinkat(directory, name.c_str(), 0);
        }
      }
    } temporaryGuard{directory, descriptor, temporary};
    if (descriptor < 0) {
      result.error = "Could not save to the selected file.";
      return result;
    }
    constexpr std::size_t writeChunkSize = 64 * 1024;
    for (std::size_t at = 0; at < bytes.size();) {
      if (cancelled()) {
        result.cancelled = true;
        return result;
      }
      const auto written = ::write(descriptor, bytes.data() + at, std::min(writeChunkSize, bytes.size() - at));
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0) {
        result.error = "Could not write the selected image.";
        return result;
      }
      at += static_cast<std::size_t>(written);
    }
    if (::fsync(descriptor) != 0) {
      result.error = "Could not finish writing the selected image.";
      return result;
    }
    if (cancelled())
      result.cancelled = true;
    else if (::renameat(directory, temporary.c_str(), directory, result.name.c_str()) != 0)
      result.error = "Could not finish saving the selected image.";
    return result;
  }
} // namespace app
