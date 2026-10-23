#include "app/client_application.h"
#include "app/restart_process.h"
#include "app/single_instance.h"
#include "core/environment.h"
#include "core/log.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>

namespace {
  constexpr Logger kLog("application");
  struct LogLifetime {
    LogLifetime(std::optional<LogLevel> level) {
      initLogLevelFromEnvironment();
      if (level)
        setLogLevel(*level);
      initLogFile();
    }
    ~LogLifetime() { closeLogFile(); }
  };
} // namespace

int main(int argc, char** argv) {
  bool demo = false, smoke = false, startMinimized = false, newInstance = false;
  std::string tokenFile;
  std::optional<LogLevel> consoleLevel;
  const char* stage = "startup";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help") {
      std::cout << "Cressida\n\nUsage: cressida [--new-instance | -n] [--start-minimized] [--demo] [--smoke-test] "
                   "[--token-file PATH] [--log-level LEVEL]\n"
                   "--log-level LEVEL sets terminal verbosity: debug, info, warn, error.\n"
                   "Rotating diagnostic files also retain debug details.\n"
                   "\nAccount tokens can also be entered in the window or provided through CRESSIDA_TOKEN.\n"
                   "--demo opens sample conversations without network access.\n"
                   "--smoke-test renders one demo frame and exits.\n"
                   "--start-minimized starts hidden in the tray; shows the window if no tray host appears within five "
                   "seconds.\n"
                   "It is ignored in demo and smoke-test modes.\n"
                   "By default, another launch opens or focuses the running client.\n"
                   "--new-instance, -n opens a separate client. Demo and smoke-test modes always run separately.\n";
      return 0;
    }
    if (arg == "--version") {
      std::cout << "cressida 0.1.0\n";
      return 0;
    }
    if (arg == "--demo")
      demo = true;
    else if (arg == "--start-minimized")
      startMinimized = true;
    else if (arg == "--new-instance" || arg == "-n")
      newInstance = true;
    else if (arg == "--smoke-test") {
      smoke = true;
      demo = true;
    } else if (arg == "--log-level" && i + 1 < argc) {
      consoleLevel = parseLogLevel(argv[++i]);
      if (!consoleLevel) {
        std::cerr << "Invalid log level. Use debug, info, warn, or error.\n";
        return 2;
      }
    } else if (arg == "--token-file" && i + 1 < argc)
      tokenFile = argv[++i];
    else {
      std::cerr << "Unknown or incomplete option. Use --help.\n";
      return 2;
    }
  }
  std::unique_ptr<LogLifetime> logging;
  try {
    int result;
    bool restart;
    {
      std::unique_ptr<app::SingleInstance> instance;
      if (!demo && !newInstance) {
        instance = std::make_unique<app::SingleInstance>();
        const char* activation = std::getenv("XDG_ACTIVATION_TOKEN");
        if (instance->claim(activation ? activation : "") == app::SingleInstance::Launch::Forwarded)
          return 0;
      }
      logging = std::make_unique<LogLifetime>(consoleLevel);
      kLog.info(
          "starting Cressida version=0.1.0 demo={} smoke={} start_minimized={} separate_instance={}", demo, smoke,
          startMinimized, newInstance
      );
      stage = "credential-input";
      std::string token;
      if (!demo) {
        if (!tokenFile.empty()) {
          std::ifstream stream(tokenFile);
          if (!stream)
            throw std::runtime_error("Could not open the token file");
          char bytes[4098]{};
          stream.read(bytes, sizeof(bytes));
          if (stream.gcount() > 4096)
            throw std::runtime_error("Token file is too large");
          token.assign(bytes, stream.gcount());
          while (!token.empty() && (token.back() == '\n' || token.back() == '\r'))
            token.pop_back();
        } else if (const char* value = cressida::environment("CRESSIDA_TOKEN", "DISCORD_CPP_TOKEN"))
          token = value;
      }
      stage = "application-initialization";
      app::ClientApplication application(demo, std::move(token), instance.get());
      stage = "event-loop";
      kLog.info("application initialized; entering event loop");
      result = application.run(smoke, startMinimized);
      restart = application.restartRequested();
      kLog.info("event loop stopped result={} restart={}", result, restart);
    }
    if (restart && !demo) {
      stage = "restart";
      kLog.info("restarting process");
      flushLogFile();
      app::restartProcess(argv);
    }
    kLog.info("shutdown complete result={}", result);
    return result;
  } catch (const std::exception& error) {
    kLog.error("application failed stage={}; exception details omitted", stage);
    std::cerr << "cressida: " << error.what() << '\n';
    return 1;
  }
}
