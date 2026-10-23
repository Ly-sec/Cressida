#include "core/log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct FormatProbe {
  std::atomic<int>* calls;
};
template <> struct std::formatter<FormatProbe> : std::formatter<std::string_view> {
  auto format(const FormatProbe& probe, std::format_context& context) const {
    ++*probe.calls;
    return std::formatter<std::string_view>::format("formatted", context);
  }
};
namespace {
  constexpr std::size_t maximumFileBytes = 1024 * 1024, maximumLineBytes = 8192;
  int failures = 0;
  void require(bool value, const char* why) {
    if (!value) {
      std::fprintf(stderr, "log_test: %s\n", why);
      ++failures;
    }
  }
  std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
  }
  struct Fixture {
    std::filesystem::path root, directory, current, backup;
    Fixture() {
      closeLogFile();
      char pattern[] = "/tmp/cressida-log-test-XXXXXX";
      const auto* path = ::mkdtemp(pattern);
      if (!path)
        std::abort();
      root = path;
      directory = root / "cressida/logs";
      current = directory / "cressida.log";
      backup = directory / "cressida.log.1";
      ::setenv("XDG_CACHE_HOME", root.c_str(), 1);
      setLogLevel(LogLevel::Error);
    }
    ~Fixture() {
      closeLogFile();
      std::filesystem::remove_all(root);
    }
  };
  void parsesLevelsAndEnvironment() {
    Fixture fixture;
    require(
        parseLogLevel("debug") == LogLevel::Debug
            && parseLogLevel("info") == LogLevel::Info
            && parseLogLevel("warn") == LogLevel::Warn
            && parseLogLevel("error") == LogLevel::Error,
        "canonical levels parse"
    );
    require(!parseLogLevel("DEBUG") && !parseLogLevel(""), "invalid levels rejected");
    ::setenv("DISCORD_CPP_LOG_LEVEL", "debug", 1);
    ::setenv("CRESSIDA_LOG_LEVEL", "warn", 1);
    initLogLevelFromEnvironment();
    require(currentLogLevel() == LogLevel::Warn, "current environment takes precedence");
    ::setenv("CRESSIDA_LOG_LEVEL", "", 1);
    initLogLevelFromEnvironment();
    require(currentLogLevel() == LogLevel::Warn, "empty current value does not fall back");
    initLogFile();
    ::setenv("CRESSIDA_LOG_LEVEL", "PRIVATE-SENTINEL", 1);
    initLogLevelFromEnvironment();
    flushLogFile();
    require(!read(fixture.current).contains("PRIVATE-SENTINEL"), "invalid environment value is not echoed");
    ::unsetenv("CRESSIDA_LOG_LEVEL");
    ::unsetenv("DISCORD_CPP_LOG_LEVEL");
  }
  void lazyFormattingAndPrivateFiles() {
    Fixture fixture;
    std::atomic<int> calls{0};
    logDebug("{}", FormatProbe{&calls});
    require(calls == 0, "disabled debug does not format");
    initLogFile();
    logDebug("{}", FormatProbe{&calls});
    flushLogFile();
    require(calls == 1 && read(fixture.current).contains("formatted"), "file retains debug independently of terminal");
    struct stat file{}, directory{}, lock{};
    require(::stat(fixture.current.c_str(), &file) == 0 && (file.st_mode & 0777) == 0600, "log file is private");
    require(
        ::stat(fixture.directory.c_str(), &directory) == 0 && (directory.st_mode & 0777) == 0700,
        "log directory is private"
    );
    require(
        ::stat((fixture.directory / ".lock").c_str(), &lock) == 0 && (lock.st_mode & 0777) == 0600, "lock is private"
    );
  }
  void escapedCappedLines() {
    Fixture fixture;
    initLogFile();
    Logger("test\nsection").warn("{}", std::string("before\nforged\r\033[31m") + std::string(1, '\0') + "after");
    logWarn("{}", std::string(10000, 'x'));
    std::string unicode;
    for (int i = 0; i < 4000; ++i)
      unicode += "\xe2\x98\x85";
    logWarn("{}", unicode);
    flushLogFile();
    const auto data = read(fixture.current);
    require(data.contains("before\\nforged\\r\\x1b[31m\\x00after"), "controls escaped");
    require(!data.contains('\033') && !data.contains('\0'), "no raw terminal escape or NUL");
    std::size_t start = 0, lines = 0;
    while (start < data.size()) {
      const auto end = data.find('\n', start);
      require(end != std::string::npos && end - start + 1 <= maximumLineBytes, "complete line respects cap");
      if (end == std::string::npos)
        break;
      ++lines;
      start = end + 1;
    }
    require(
        lines == 3 && data.contains("truncated, original=10000 bytes"),
        "single physical line per record and truncation marker"
    );
    const auto suffix = data.rfind(" ... [truncated");
    require(
        suffix != std::string::npos && (suffix == 0 || static_cast<unsigned char>(data[suffix - 1]) == 0x85),
        "Unicode truncated on codepoint boundary"
    );
  }
  void rotationAndThreadedRecords() {
    Fixture fixture;
    std::filesystem::create_directories(fixture.directory);
    const auto initial = maximumFileBytes - 8;
    {
      std::ofstream output(fixture.current);
      output << std::string(initial, 'x');
    }
    initLogFile();
    logWarn("rotation boundary");
    require(std::filesystem::file_size(fixture.backup) == initial, "rotation retains previous file");
    require(read(fixture.current).contains("rotation boundary"), "record goes into new file");
    for (int i = 0; i < 150; ++i)
      logDebug("{}", std::string(maximumLineBytes, 'x'));
    flushLogFile();
    require(
        std::filesystem::file_size(fixture.current) <= maximumFileBytes
            && std::filesystem::file_size(fixture.backup) <= maximumFileBytes,
        "both files remain bounded"
    );
    closeLogFile();
    std::filesystem::remove(fixture.current);
    initLogFile();
    std::vector<std::jthread> threads;
    constexpr int workers = 4, perWorker = 100;
    for (int worker = 0; worker < workers; ++worker)
      threads.emplace_back([worker] {
        for (int n = 0; n < perWorker; ++n)
          Logger("thread").debug("worker={} record={}", worker, n);
      });
    threads.clear();
    flushLogFile();
    const auto data = read(fixture.current);
    std::set<std::string> records;
    for (std::size_t start = 0; start < data.size();) {
      const auto end = data.find('\n', start), record = data.find("worker=", start);
      require(end != data.npos && record < end, "thread record is complete");
      if (end == data.npos)
        break;
      records.insert(data.substr(record, end - record));
      start = end + 1;
    }
    require(records.size() == workers * perWorker, "thread records do not interleave or disappear");
  }
  void refusesUnsafeTargets() {
    Fixture fixture;
    std::filesystem::create_directories(fixture.directory);
    const auto target = fixture.root / "untouched";
    {
      std::ofstream output(target);
      output << "PRIVATE-SENTINEL";
    }
    for (const auto* name : {".lock", "cressida.log"}) {
      const auto path = fixture.directory / name;
      std::filesystem::remove(path);
      std::filesystem::create_symlink(target, path);
      initLogFile();
      require(!detail::logEnabled(LogLevel::Debug), "symlink target rejected");
      require(read(target) == "PRIVATE-SENTINEL", "symlink target untouched");
      std::filesystem::remove(path);
      std::filesystem::create_hard_link(target, path);
      initLogFile();
      require(!detail::logEnabled(LogLevel::Debug), "hardlinked target rejected");
      std::filesystem::remove(path);
      require(::mkfifo(path.c_str(), 0600) == 0, "FIFO fixture created");
      initLogFile();
      require(!detail::logEnabled(LogLevel::Debug), "FIFO rejected without blocking");
      std::filesystem::remove(path);
    }
  }
  void lockSurvivesRotation() {
    Fixture fixture;
    initLogFile();
    for (int i = 0; i < 135; ++i)
      logDebug("{}", std::string(maximumLineBytes, 'x'));
    const auto child = ::fork();
    require(child >= 0, "child process created");
    if (child == 0) {
      initLogFile();
      ::_exit(detail::logEnabled(LogLevel::Debug) ? 1 : 0);
    }
    if (child > 0) {
      int status = 0;
      require(
          ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "second process cannot own file after rotation"
      );
    }
    logWarn("primary remains active");
    require(read(fixture.current).contains("primary remains active"), "primary remains owner");
  }
  void disablesFileAfterRotationFailure() {
    Fixture fixture;
    std::filesystem::create_directories(fixture.directory);
    {
      std::ofstream output(fixture.current);
      output << std::string(maximumFileBytes - 8, 'x');
    }
    std::filesystem::create_directory(fixture.backup);
    initLogFile();
    logWarn("forces failed rotation");
    require(!detail::logEnabled(LogLevel::Debug), "rotation failure disables file sink");
    logDebug("must not grow");
    require(
        std::filesystem::file_size(fixture.current) == maximumFileBytes - 8,
        "failed rotation never appends indefinitely"
    );
  }
} // namespace
int main() {
  parsesLevelsAndEnvironment();
  lazyFormattingAndPrivateFiles();
  escapedCappedLines();
  rotationAndThreadedRecords();
  refusesUnsafeTargets();
  lockSurvivesRotation();
  disablesFileAfterRotationFailure();
  closeLogFile();
  return failures ? 1 : 0;
}
