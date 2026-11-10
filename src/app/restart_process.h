#pragma once

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace app {
  // Call only after application destruction: retain the same PID, argv and
  // launch environment, without a shell, duplicate client or credentials in args.
  [[noreturn]] inline void restartProcess(char* const* arguments, const std::string& image = {}) {
    ::execv(image.empty() ? "/proc/self/exe" : image.c_str(), arguments);
    throw std::runtime_error(std::string("Could not restart Cressida: ") + std::strerror(errno));
  }
} // namespace app
