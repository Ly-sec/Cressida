#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace updates {
  inline constexpr std::string_view releasesUrl = "https://github.com/Ly-sec/Cressida/releases";
  inline constexpr std::string_view releasesApi = "https://api.github.com/repos/Ly-sec/Cressida/releases?per_page=30";
  inline constexpr std::uint64_t maximumImageBytes = 2ULL * 1024 * 1024 * 1024;
  inline constexpr std::size_t maximumMetadataBytes = 1024 * 1024;
  inline constexpr std::size_t maximumNotesBytes = 12000;
  enum class InstallationKind { AppImage, Flatpak, Nix, Guix, Managed };
  struct Installation {
    InstallationKind kind = InstallationKind::Managed;
    std::filesystem::path image;
    std::string architecture;
  };
  struct Asset {
    std::string name, url, digest;
    std::uint64_t size = 0;
  };
  struct Release {
    std::string version, notes, url, checksumsUrl;
    Asset image;
  };
  bool validVersion(std::string_view version);
  std::optional<int> compareVersions(std::string_view a, std::string_view b);
  bool validDigest(std::string_view digest);
  std::optional<Release> selectRelease(
      const nlohmann::json& response, std::string_view current, std::string_view architecture, bool prereleases
  );
  std::string checksumFor(std::string_view manifest, const Asset& asset);
  Installation detectInstallation();
  std::string installationName(InstallationKind kind);
  std::string installationHelp(InstallationKind kind);
} // namespace updates
