#include "updates/updater.h"

#include "core/process/wake_event.h"
#include "updates/flatpak.h"

#include <array>
#include <condition_variable>
#include <curl/curl.h>
#include <fcntl.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace updates {
  namespace {
    constexpr long connectSeconds = 15, transferSeconds = 600, lowSpeedSeconds = 30, lowSpeedBytes = 1024;
    constexpr long maximumRedirects = 5;
    constexpr unsigned renameExchange = 2;
    constexpr std::size_t elfHeaderBytes = 64, elfMachineOffset = 18;
    constexpr unsigned elf64 = 2, littleEndian = 1, x86Machine = 62, armMachine = 183;
    struct File {
      int fd = -1;
      explicit File(int value = -1) : fd(value) {}
      ~File() {
        if (fd >= 0)
          ::close(fd);
      }
      File(const File&) = delete;
      File& operator=(const File&) = delete;
    };
    void cancelled(std::stop_token stop) {
      if (stop.stop_requested())
        throw std::runtime_error("Update cancelled.");
    }
    std::string metadata(const Fetch& fetch, const std::string& url, std::stop_token stop) {
      std::string result;
      fetch(
          url,
          [&](std::span<const unsigned char> bytes) {
            cancelled(stop);
            if (bytes.size() > maximumMetadataBytes - result.size())
              throw std::runtime_error("The release response is too large.");
            result.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
          },
          stop
      );
      cancelled(stop);
      return result;
    }
    struct StagedImage {
      File directory, original, staged;
      std::string name, temporary;
      struct stat identity{}, prepared{};
      bool retained = false;
      explicit StagedImage(const Installation& installation)
          : directory(::open(installation.image.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)),
            name(installation.image.filename()) {
        if (directory.fd < 0 || name.empty())
          throw std::runtime_error("The AppImage folder is unavailable. Download the update from Releases.");
        struct stat folder{};
        if (::fstat(directory.fd, &folder) || folder.st_uid != ::getuid() || (folder.st_mode & (S_IWOTH | S_IWGRP)))
          throw std::runtime_error("Use an AppImage in a folder owned by your user to update it here.");
        original.fd = ::openat(directory.fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (original.fd < 0
            || ::fstat(original.fd, &identity)
            || !S_ISREG(identity.st_mode)
            || identity.st_uid != ::getuid()
            || !(identity.st_mode & S_IXUSR)
            || identity.st_nlink != 1)
          throw std::runtime_error("This AppImage cannot be replaced here. Download the update from Releases.");
        // Keep staging and atomic replacement on the original filesystem.
        auto pattern = installation.image.parent_path() / ("." + name + ".update-XXXXXX");
        auto path = pattern.string();
        staged.fd = ::mkostemp(path.data(), O_CLOEXEC);
        if (staged.fd < 0)
          throw std::runtime_error("The AppImage folder is not writable, or its disk is full.");
        temporary = std::filesystem::path(path).filename();
      }
      ~StagedImage() {
        if (!retained && !temporary.empty())
          ::unlinkat(directory.fd, temporary.c_str(), 0);
      }
      void write(std::span<const unsigned char> bytes) {
        while (!bytes.empty()) {
          const auto count = ::write(staged.fd, bytes.data(), bytes.size());
          if (count < 0 && errno == EINTR)
            continue;
          if (count <= 0)
            throw std::runtime_error("Could not save the update. Check the available disk space.");
          bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
      }
      void validate(std::string_view architecture) {
        std::array<unsigned char, elfHeaderBytes> header{};
        if (::pread(staged.fd, header.data(), header.size(), 0) != static_cast<ssize_t>(header.size())
            || header[0] != 0x7f
            || header[1] != 'E'
            || header[2] != 'L'
            || header[3] != 'F'
            || header[4] != elf64
            || header[5] != littleEndian
            || header[8] != 'A'
            || header[9] != 'I'
            || header[10] != 2)
          throw std::runtime_error("The downloaded file is not a supported AppImage.");
        const unsigned machine = header[elfMachineOffset] | (header[elfMachineOffset + 1] << 8);
        if ((architecture != "x86_64" || machine != x86Machine) && (architecture != "aarch64" || machine != armMachine))
          throw std::runtime_error("The downloaded AppImage does not match this computer's architecture.");
        if (::fsync(staged.fd) || ::fstat(staged.fd, &prepared))
          throw std::runtime_error("Could not finish writing the update.");
      }
      std::string install(const std::filesystem::path& parent) {
        if (::flock(original.fd, LOCK_EX | LOCK_NB))
          throw std::runtime_error("Another updater is using this AppImage. Try again after it finishes.");
        const auto sameContents = [](const struct stat& a, const struct stat& b) {
          return a.st_size == b.st_size
              && a.st_mtim.tv_sec == b.st_mtim.tv_sec
              && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
              && a.st_ctim.tv_sec == b.st_ctim.tv_sec
              && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
        };
        struct stat current{}, pending{}, folder{}, namedFolder{};
        if (::fstat(directory.fd, &folder)
            || ::stat(parent.c_str(), &namedFolder)
            || folder.st_dev != namedFolder.st_dev
            || folder.st_ino != namedFolder.st_ino
            || ::fstatat(directory.fd, name.c_str(), &current, AT_SYMLINK_NOFOLLOW)
            || current.st_dev != identity.st_dev
            || current.st_ino != identity.st_ino
            || !sameContents(current, identity)
            || current.st_nlink != 1
            || !S_ISREG(current.st_mode)
            || current.st_uid != ::getuid()
            || ::fstat(staged.fd, &pending)
            || pending.st_uid != ::getuid()
            || pending.st_nlink != 1
            || !sameContents(pending, prepared)
            || ::fstatat(directory.fd, temporary.c_str(), &current, AT_SYMLINK_NOFOLLOW)
            || current.st_dev != pending.st_dev
            || current.st_ino != pending.st_ino
            || !S_ISREG(current.st_mode))
          throw std::runtime_error(
              "The AppImage changed while the update was downloading. Check again before updating."
          );
        if (::fchmod(staged.fd, identity.st_mode & (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH))
            || ::fsync(staged.fd))
          throw std::runtime_error("Could not prepare the downloaded AppImage for launch.");
        if (::syscall(SYS_renameat2, directory.fd, name.c_str(), directory.fd, temporary.c_str(), renameExchange))
          throw std::runtime_error(
              "This folder does not support replacing the AppImage atomically. Use Releases to update."
          );
        if (::fsync(directory.fd)) {
          if (::syscall(SYS_renameat2, directory.fd, name.c_str(), directory.fd, temporary.c_str(), renameExchange))
            retained = true;
          throw std::runtime_error(
              "The update could not be committed to disk. Both images have been kept where possible."
          );
        }
        retained = true;
        return (parent / temporary).string();
      }
    };
  } // namespace
  void fetchHttps(const std::string& url, const Sink& sink, std::stop_token stop) {
    struct Transfer {
      const Sink& sink;
      std::stop_token stop;
      std::exception_ptr error;
    } transfer{sink, stop, {}};
    static std::once_flag initialized;
    std::call_once(initialized, [] {
      if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        throw std::runtime_error("Could not initialize the update connection.");
    });
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
      throw std::runtime_error("Could not initialize the update connection.");
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, maximumRedirects);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, connectSeconds);
    constexpr long metadataSeconds = 30;
    curl_easy_setopt(
        curl.get(), CURLOPT_TIMEOUT,
        url == releasesApi || url.ends_with("/SHA256SUMS") ? metadataSeconds : transferSeconds
    );
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, lowSpeedSeconds);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, lowSpeedBytes);
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "Cressida-Updater");
    curl_easy_setopt(
        curl.get(), CURLOPT_WRITEFUNCTION,
        +[](char* data, std::size_t size, std::size_t count, void* raw) -> std::size_t {
          auto& transfer = *static_cast<Transfer*>(raw);
          const auto bytes = size * count;
          try {
            cancelled(transfer.stop);
            transfer.sink({reinterpret_cast<const unsigned char*>(data), bytes});
            return bytes;
          } catch (...) {
            transfer.error = std::current_exception();
            return 0;
          }
        }
    );
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        curl.get(), CURLOPT_XFERINFOFUNCTION, +[](void* raw, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
          return static_cast<Transfer*>(raw)->stop.stop_requested() ? 1 : 0;
        }
    );
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
    const auto result = curl_easy_perform(curl.get());
    if (transfer.error)
      std::rethrow_exception(transfer.error);
    cancelled(stop);
    if (result != CURLE_OK)
      throw std::runtime_error("Could not download update information or files. Check your connection and try again.");
  }
  struct Updater::Impl {
    enum class Command { Check, Download, Install, Discard };
    Options options;
    process::WakeEvent wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Command> command;
    Snapshot state;
    bool dirty = false, prereleases = false;
    std::uint64_t generation = 0;
    std::stop_source cancellation;
    std::unique_ptr<StagedImage> staged;
    std::jthread worker;
    explicit Impl(Options value) : options(std::move(value)) {
      if (!options.fetch)
        options.fetch = fetchHttps;
    }
    ~Impl() {
      cancellation.request_stop();
      worker.request_stop();
      condition.notify_all();
      if (worker.joinable())
        worker.join();
    }
    void publish(Snapshot value, std::uint64_t expected) {
      std::lock_guard lock(mutex);
      if (generation != expected)
        return;
      state = std::move(value);
      dirty = true;
      (void)wake.signal();
    }
    bool start(Command next, bool preview = false) {
      std::lock_guard lock(mutex);
      if ((next == Command::Check && state.phase == Phase::Ready)
          || state.phase == Phase::Installed
          || (command && *command != Command::Discard)
          || state.phase == Phase::Checking
          || state.phase == Phase::Downloading
          || state.phase == Phase::Installing)
        return false;
      const bool flatpak = options.installation.kind == InstallationKind::Flatpak;
      if ((next == Command::Download
           && ((!flatpak
                && (!state.release
                    || state.release->image.name.empty()
                    || options.installation.kind != InstallationKind::AppImage))
               || (state.phase != Phase::Available && state.phase != Phase::Error)))
          || (next == Command::Install && state.phase != Phase::Ready))
        return false;
      cancellation = std::stop_source{};
      ++generation;
      prereleases = preview;
      state.phase = next == Command::Check ? Phase::Checking
          : next == Command::Download      ? Phase::Downloading
                                           : Phase::Installing;
      state.error.clear();
      command = next;
      dirty = true;
      (void)wake.signal();
      if (!worker.joinable())
        worker = std::jthread([this](std::stop_token stop) { run(stop); });
      condition.notify_one();
      return true;
    }
    void run(std::stop_token lifetime) {
      while (!lifetime.stop_requested()) {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return command.has_value() || lifetime.stop_requested(); });
        if (lifetime.stop_requested())
          break;
        const auto task = *std::exchange(command, {});
        auto snapshot = state;
        const auto expected = generation;
        const bool includePreviews = prereleases;
        const auto stop = cancellation.get_token();
        lock.unlock();
        try {
          if (task == Command::Discard) {
            staged.reset();
            snapshot.phase = Phase::Idle;
            snapshot.error.clear();
          } else if (options.installation.kind == InstallationKind::Flatpak) {
            snapshot = flatpakUpdate(task == Command::Download, stop, [&](Snapshot value) {
              publish(std::move(value), expected);
            });
          } else if (task == Command::Check) {
            staged.reset();
            snapshot.release = selectRelease(
                nlohmann::json::parse(metadata(options.fetch, std::string(releasesApi), stop)), options.currentVersion,
                options.installation.architecture, includePreviews
            );
            snapshot.phase = snapshot.release ? Phase::Available : Phase::Current;
            snapshot.downloaded = snapshot.total = 0;
          } else if (task == Command::Download) {
            const auto& release = *snapshot.release;
            if (release.checksumsUrl.empty())
              throw std::runtime_error("This release has no checksum file. Use Releases to inspect it.");
            const auto digest = checksumFor(metadata(options.fetch, release.checksumsUrl, stop), release.image);
            staged = std::make_unique<StagedImage>(options.installation);
            std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
            if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1)
              throw std::runtime_error("Could not verify the downloaded file.");
            snapshot.downloaded = 0;
            snapshot.total = release.image.size;
            options.fetch(
                release.image.url,
                [&](std::span<const unsigned char> bytes) {
                  cancelled(stop);
                  if (bytes.size() > snapshot.total - snapshot.downloaded)
                    throw std::runtime_error("The download is larger than the published release file.");
                  staged->write(bytes);
                  if (EVP_DigestUpdate(hash.get(), bytes.data(), bytes.size()) != 1)
                    throw std::runtime_error("Could not verify the downloaded file.");
                  snapshot.downloaded += bytes.size();
                  publish(snapshot, expected);
                },
                stop
            );
            cancelled(stop);
            std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
            unsigned length = 0;
            if (EVP_DigestFinal_ex(hash.get(), bytes.data(), &length) != 1 || snapshot.downloaded != snapshot.total)
              throw std::runtime_error("The download is incomplete.");
            constexpr std::string_view hex = "0123456789abcdef";
            std::string actual;
            for (unsigned i = 0; i < length; ++i) {
              actual += hex[bytes[i] >> 4];
              actual += hex[bytes[i] & 15];
            }
            if (actual != digest)
              throw std::runtime_error("The downloaded file did not match the release checksum. It was not installed.");
            staged->validate(options.installation.architecture);
            snapshot.phase = Phase::Ready;
          } else {
            if (!staged)
              throw std::runtime_error("The prepared update is no longer available. Download it again.");
            snapshot.backup = staged->install(options.installation.image.parent_path());
            staged.reset();
            snapshot.phase = Phase::Installed;
          }
          cancelled(stop);
          publish(std::move(snapshot), expected);
        } catch (const std::exception& error) {
          staged.reset();
          snapshot.phase = Phase::Error;
          snapshot.error = error.what();
          publish(std::move(snapshot), expected);
        }
      }
    }
  };
  Updater::Updater(Options options) : m(std::make_unique<Impl>(std::move(options))) {}
  Updater::~Updater() = default;
  bool Updater::check(bool prereleases) { return m->start(Impl::Command::Check, prereleases); }
  bool Updater::download() { return m->start(Impl::Command::Download); }
  bool Updater::install() { return m->start(Impl::Command::Install); }
  void Updater::cancel() {
    std::lock_guard lock(m->mutex);
    if (m->state.phase == Phase::Installing || m->state.phase == Phase::Installed)
      return;
    m->cancellation.request_stop();
    ++m->generation;
    m->command = Impl::Command::Discard;
    m->condition.notify_one();
    m->state.phase = Phase::Idle;
    m->state.error.clear();
    m->dirty = true;
    (void)m->wake.signal();
  }
  std::optional<Snapshot> Updater::poll() {
    m->wake.drain();
    std::lock_guard lock(m->mutex);
    if (!std::exchange(m->dirty, false))
      return {};
    return m->state;
  }
  int Updater::eventFd() const { return m->wake.fd(); }
  const Installation& Updater::installation() const { return m->options.installation; }
} // namespace updates
