#include "core/files/resource_paths.h"

#include "core/environment.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace paths {

  namespace {

    constexpr Logger kLog("paths");

    std::filesystem::path installedAssetsRoot() {
      const std::filesystem::path datadir(CRESSIDA_INSTALL_DATADIR);
      if (datadir.is_absolute()) {
        return datadir / "cressida" / "assets";
      }
      return std::filesystem::path(CRESSIDA_INSTALL_PREFIX) / datadir / "cressida" / "assets";
    }

    bool isAssetRoot(const std::filesystem::path& root) {
      if (root.empty()) {
        return false;
      }

      std::error_code ec;
      return std::filesystem::exists(root / "emoji.json", ec)
          && std::filesystem::exists(root / "fonts" / "tabler-icons.ttf", ec)
          && std::filesystem::exists(root / "fonts" / "tabler.json", ec);
    }

    std::optional<std::filesystem::path> executablePath() {
      std::array<char, 4096> buffer{};
      const ssize_t count = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
      if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size() - 1) {
        return std::nullopt;
      }

      buffer[static_cast<std::size_t>(count)] = '\0';
      return std::filesystem::path(buffer.data());
    }

    void appendUnique(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& candidate) {
      if (candidate.empty()) {
        return;
      }
      if (!std::ranges::contains(candidates, candidate)) {
        candidates.push_back(candidate);
      }
    }

    std::vector<std::filesystem::path> assetCandidates() {
      std::vector<std::filesystem::path> candidates;

      if (const char* env = cressida::environment("CRESSIDA_ASSETS_DIR", "DISCORD_CPP_ASSETS_DIR");
          env != nullptr && env[0] != '\0') {
        const std::filesystem::path overridePath(env);
        if (isAssetRoot(overridePath)) {
          candidates.push_back(overridePath);
          return candidates;
        }
        kLog.warn("CRESSIDA_ASSETS_DIR is not a valid asset bundle");
      }

      if (auto exe = executablePath()) {
        const std::filesystem::path exeDir = exe->parent_path();
        appendUnique(candidates, exeDir / "assets");
        appendUnique(candidates, exeDir.parent_path() / "assets");

        const std::filesystem::path datadir(CRESSIDA_INSTALL_DATADIR);
        if (!datadir.empty() && !datadir.is_absolute()) {
          appendUnique(candidates, exeDir.parent_path() / datadir / "cressida" / "assets");
        }
        appendUnique(candidates, exeDir.parent_path() / "share" / "cressida" / "assets");
      }

      appendUnique(candidates, installedAssetsRoot());
      return candidates;
    }

    std::filesystem::path resolveAssetsRoot() {
      for (const auto& candidate : assetCandidates()) {
        if (isAssetRoot(candidate)) {
          kLog.debug("asset bundle located");
          return candidate;
        }
      }

      const std::filesystem::path fallback = installedAssetsRoot();
      kLog.warn("could not locate a valid asset bundle; using fallback");
      return fallback;
    }

  } // namespace

  const std::filesystem::path& assetsRoot() {
    static const std::filesystem::path s_root = resolveAssetsRoot();
    return s_root;
  }

  std::filesystem::path assetPath(std::string_view relativePath) {
    return assetsRoot() / std::filesystem::path(std::string(relativePath));
  }

} // namespace paths
