#pragma once
#include "updates/updater.h"

namespace updates {
  Snapshot flatpakUpdate(bool install, std::stop_token stop, const std::function<void(Snapshot)>& progress);
  void relaunchFlatpak(char* const* arguments);
} // namespace updates
