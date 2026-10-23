#include "core/log.h"

#include "core/environment.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  constexpr std::size_t maximumLogBytes = 1024 * 1024;
  constexpr std::size_t maximumLineBytes = 8 * 1024;
  constexpr std::size_t maximumSectionBytes = 64;
  constexpr std::size_t bufferedLines = 64;
  constexpr auto flushInterval = std::chrono::milliseconds(500);
  constexpr const char* levelEnvironment = "CRESSIDA_LOG_LEVEL";
  constexpr const char* currentFilename = "cressida.log";
  constexpr const char* backupFilename = "cressida.log.1";
  constexpr const char* lockFilename = ".lock";
  std::atomic<LogLevel> minimumLevel{LogLevel::Info};
  std::atomic<bool> fileActive{false};
  std::mutex logMutex;
  FILE* logFile = nullptr;
  int directoryFd = -1, lockFd = -1;
  std::size_t logSize = 0, pendingLines = 0;
  auto lastFlush = std::chrono::steady_clock::now();
  bool exitRegistered = false;

  const char* levelTag(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
      return "DBG";
    case LogLevel::Info:
      return "INF";
    case LogLevel::Warn:
      return "WRN";
    case LogLevel::Error:
      return "ERR";
    }
    return "???";
  }

  std::size_t utf8Boundary(std::string_view value, std::size_t length) {
    if (length >= value.size())
      return value.size();
    while (length && (static_cast<unsigned char>(value[length]) & 0xc0U) == 0x80U)
      --length;
    return length;
  }

  std::string singleLine(std::string_view value, std::size_t limit) {
    std::string result;
    result.reserve(std::min(value.size(), limit));
    constexpr char hex[] = "0123456789abcdef";
    std::size_t at = 0;
    while (at < value.size() && result.size() <= limit) {
      const auto byte = static_cast<unsigned char>(value[at++]);
      if (byte == '\n')
        result += "\\n";
      else if (byte == '\r')
        result += "\\r";
      else if (byte == '\t')
        result += "\\t";
      else if (byte < 0x20U || byte == 0x7fU) {
        result += "\\x";
        result += hex[byte >> 4];
        result += hex[byte & 0xfU];
      } else
        result += static_cast<char>(byte);
    }
    if (at < value.size() || result.size() > limit) {
      const auto suffix = std::format(" ... [truncated, original={} bytes]", value.size());
      const auto keep = limit > suffix.size() ? utf8Boundary(result, limit - suffix.size()) : 0;
      result.resize(keep);
      result += std::string_view(suffix).substr(0, limit - keep);
    }
    return result;
  }

  std::string prefix(LogLevel level, const char* section, bool file, bool color) {
    std::timespec ts{};
    std::timespec_get(&ts, TIME_UTC);
    std::tm time{};
    localtime_r(&ts.tv_sec, &time);
    char timestamp[64];
    if (file)
      std::snprintf(
          timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d.%03ld", time.tm_year + 1900, time.tm_mon + 1,
          time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec, ts.tv_nsec / 1'000'000
      );
    else
      std::snprintf(
          timestamp, sizeof(timestamp), "%02d:%02d:%02d.%03ld", time.tm_hour, time.tm_min, time.tm_sec,
          ts.tv_nsec / 1'000'000
      );
    const char* ansi = level == LogLevel::Error ? "\033[31m"
        : level == LogLevel::Warn               ? "\033[33m"
        : level == LogLevel::Info               ? "\033[32m"
                                                : "\033[36m";
    auto result = std::format(
        "{} [{}{}{}] [pid={}]", timestamp, color ? ansi : "", levelTag(level), color ? "\033[0m" : "", getpid()
    );
    if (section && *section) {
      const auto available = std::string_view(section, strnlen(section, maximumSectionBytes + 1));
      const auto length = utf8Boundary(available, maximumSectionBytes);
      result += std::format(" [{}]", singleLine(available.substr(0, length), maximumSectionBytes));
    }
    result += ' ';
    return result;
  }

  void closeFileUnlocked() {
    fileActive.store(false, std::memory_order_relaxed);
    if (logFile) {
      std::fclose(logFile);
      logFile = nullptr;
    }
  }
  void closeAllUnlocked() {
    closeFileUnlocked();
    if (lockFd >= 0)
      ::close(lockFd);
    if (directoryFd >= 0)
      ::close(directoryFd);
    lockFd = directoryFd = -1;
  }
  bool flushUnlocked() {
    if (!logFile)
      return true;
    if (std::fflush(logFile) != 0) {
      closeFileUnlocked();
      std::fputs("[logging] File output failed; continuing with terminal logging.\n", stderr);
      return false;
    }
    pendingLines = 0;
    lastFlush = std::chrono::steady_clock::now();
    return true;
  }
  int openPrivateFile(const char* filename) {
    const auto fd =
        ::openat(directoryFd, filename, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0)
      return -1;
    struct stat info{};
    if (::fstat(fd, &info) != 0
        || !S_ISREG(info.st_mode)
        || info.st_uid != ::geteuid()
        || info.st_nlink != 1
        || ::fchmod(fd, 0600) != 0) {
      ::close(fd);
      return -1;
    }
    return fd;
  }
  bool openFileUnlocked() {
    const auto fd = openPrivateFile(currentFilename);
    if (fd < 0)
      return false;
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
      ::close(fd);
      return false;
    }
    logFile = ::fdopen(fd, "a");
    if (!logFile) {
      ::close(fd);
      return false;
    }
    logSize = static_cast<std::size_t>(info.st_size);
    pendingLines = 0;
    lastFlush = std::chrono::steady_clock::now();
    fileActive.store(true, std::memory_order_relaxed);
    return true;
  }
  bool rotateUnlocked() {
    closeFileUnlocked();
    return ::renameat(directoryFd, currentFilename, directoryFd, backupFilename) == 0 && openFileUnlocked();
  }
} // namespace

std::string_view logLevelName(LogLevel level) {
  switch (level) {
  case LogLevel::Debug:
    return "debug";
  case LogLevel::Info:
    return "info";
  case LogLevel::Warn:
    return "warn";
  case LogLevel::Error:
    return "error";
  }
  return "unknown";
}
std::optional<LogLevel> parseLogLevel(std::string_view value) {
  if (value == "debug")
    return LogLevel::Debug;
  if (value == "info")
    return LogLevel::Info;
  if (value == "warn")
    return LogLevel::Warn;
  if (value == "error")
    return LogLevel::Error;
  return std::nullopt;
}
LogLevel currentLogLevel() { return minimumLevel.load(std::memory_order_relaxed); }
void setLogLevel(LogLevel level) { minimumLevel.store(level, std::memory_order_relaxed); }
void initLogLevelFromEnvironment() {
  const auto* value = cressida::environment(levelEnvironment, "DISCORD_CPP_LOG_LEVEL");
  if (!value)
    return;
  if (const auto level = parseLogLevel(value))
    setLogLevel(*level);
  else
    logWarn("invalid {}; expected debug, info, warn, or error", levelEnvironment);
}

void initLogFile() {
  const auto* base = std::getenv("XDG_CACHE_HOME");
  std::filesystem::path directory;
  if (base && *base && std::filesystem::path(base).is_absolute())
    directory = std::filesystem::path(base) / "cressida/logs";
  else if (const auto* home = std::getenv("HOME"); home && *home && std::filesystem::path(home).is_absolute())
    directory = std::filesystem::path(home) / ".cache/cressida/logs";
  const char* failure = nullptr;
  {
    std::scoped_lock lock(logMutex);
    closeAllUnlocked();
    std::error_code error;
    if (!directory.empty())
      std::filesystem::create_directories(directory, error);
    if (directory.empty() || error)
      failure = "cache-directory";
    else {
      directoryFd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      struct stat info{};
      if (directoryFd < 0
          || ::fstat(directoryFd, &info) != 0
          || info.st_uid != ::geteuid()
          || ::fchmod(directoryFd, 0700) != 0)
        failure = "private-directory";
      else if ((lockFd = openPrivateFile(lockFilename)) < 0)
        failure = "lock-file";
      else if (::flock(lockFd, LOCK_EX | LOCK_NB) != 0)
        failure = "another-instance-owns-log";
      else if (!openFileUnlocked())
        failure = "open-file";
      else if (logSize >= maximumLogBytes && !rotateUnlocked())
        failure = "rotation";
    }
    if (failure)
      closeAllUnlocked();
    else if (!exitRegistered) {
      std::atexit(closeLogFile);
      exitRegistered = true;
    }
  }
  if (failure)
    Logger("logging").warn("file logging unavailable stage={}; using terminal logging", failure);
}
void flushLogFile() {
  std::scoped_lock lock(logMutex);
  flushUnlocked();
}
void closeLogFile() {
  std::scoped_lock lock(logMutex);
  closeAllUnlocked();
}

namespace detail {
  bool logEnabled(LogLevel level) noexcept {
    return level >= minimumLevel.load(std::memory_order_relaxed) || fileActive.load(std::memory_order_relaxed);
  }
  void logMessage(LogLevel level, const char* section, std::string_view msg) {
    if (!logEnabled(level))
      return;
    std::scoped_lock lock(logMutex);
    if (level >= minimumLevel.load(std::memory_order_relaxed)) {
      const auto heading = prefix(level, section, false, ::isatty(STDERR_FILENO) && !std::getenv("NO_COLOR"));
      const auto body = singleLine(msg, maximumLineBytes - heading.size() - 1);
      std::fwrite(heading.data(), 1, heading.size(), stderr);
      std::fwrite(body.data(), 1, body.size(), stderr);
      std::fputc('\n', stderr);
    }
    if (!logFile)
      return;
    const auto heading = prefix(level, section, true, false);
    const auto body = singleLine(msg, maximumLineBytes - heading.size() - 1);
    const auto line = heading + body + '\n';
    if (logSize + line.size() > maximumLogBytes && !rotateUnlocked()) {
      std::fputs("[logging] Log rotation failed; continuing with terminal logging.\n", stderr);
      return;
    }
    const auto written = std::fwrite(line.data(), 1, line.size(), logFile);
    logSize += written;
    if (written != line.size()) {
      closeFileUnlocked();
      std::fputs("[logging] File output failed; continuing with terminal logging.\n", stderr);
      return;
    }
    if (level >= LogLevel::Warn
        || ++pendingLines >= bufferedLines
        || std::chrono::steady_clock::now() - lastFlush >= flushInterval)
      flushUnlocked();
  }
} // namespace detail
