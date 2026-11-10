#include "app/restart_process.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--updated-image") {
      const auto* pid = std::getenv("CRESSIDA_RESTART_TEST_PID");
      return pid
              && std::string(pid) == std::to_string(getpid())
              && std::string_view(argv[0]) == "outer image fixture"
              && std::filesystem::read_symlink("/proc/self/exe") == argv[2]
          ? 0
          : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--restart-fixture") {
      if (const auto* pid = std::getenv("CRESSIDA_RESTART_TEST_PID"))
        return std::string_view(argv[0]) == "fixture executable name"
                && std::string_view(argv[2]) == "spaces ; $(literal)"
                && std::string(pid) == std::to_string(getpid())
            ? 0
            : 1;
      const auto pid = std::to_string(getpid());
      setenv("CRESSIDA_RESTART_TEST_PID", pid.c_str(), 1);
      app::restartProcess(argv);
    }
    const auto child = fork();
    if (child < 0)
      return 1;
    if (child == 0) {
      unsetenv("CRESSIDA_RESTART_TEST_PID");
      execl("/proc/self/exe", "fixture executable name", "--restart-fixture", "spaces ; $(literal)", nullptr);
      _exit(1);
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
      throw std::runtime_error(
          "Restart must replace the process while preserving PID, argv and environment without a shell"
      );
    char updated[] = "/tmp/cressida-updated-image.XXXXXX";
    const int temporary = mkstemp(updated);
    if (temporary < 0)
      throw std::runtime_error("Updated-image fixture could not be created");
    close(temporary);
    std::filesystem::copy_file("/proc/self/exe", updated, std::filesystem::copy_options::overwrite_existing);
    chmod(updated, 0755);
    const auto imageChild = fork();
    if (imageChild == 0) {
      const auto pid = std::to_string(getpid());
      setenv("CRESSIDA_RESTART_TEST_PID", pid.c_str(), 1);
      char name[] = "outer image fixture", flag[] = "--updated-image";
      char* arguments[] = {name, flag, updated, nullptr};
      app::restartProcess(arguments, updated);
    }
    const bool relaunched = imageChild > 0
        && waitpid(imageChild, &status, 0) == imageChild
        && WIFEXITED(status)
        && WEXITSTATUS(status) == 0;
    unlink(updated);
    if (!relaunched)
      throw std::runtime_error("An updated AppImage restarts its outer image instead of the old mounted executable");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
