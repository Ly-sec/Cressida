#include "updates/model.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sys/utsname.h>
#include <vector>

namespace updates {
  namespace {
    constexpr std::size_t maximumVersionBytes = 96, maximumReleases = 100;
    struct Version {
      std::array<std::uint64_t, 3> numbers{};
      std::vector<std::string_view> prerelease;
    };
    bool digits(std::string_view value) {
      return !value.empty() && std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; });
    }
    std::optional<Version> parse(std::string_view text) {
      if (text.empty() || text.size() > maximumVersionBytes)
        return {};
      if (text.front() == 'v')
        text.remove_prefix(1);
      const auto identifier = [](std::string_view part) {
        return !part.empty() && std::ranges::all_of(part, [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
        });
      };
      if (const auto plus = text.find('+'); plus != text.npos) {
        auto build = text.substr(plus + 1);
        while (true) {
          const auto dot = build.find('.');
          if (!identifier(build.substr(0, dot)))
            return {};
          if (dot == build.npos)
            break;
          build.remove_prefix(dot + 1);
        }
        text = text.substr(0, plus);
      }
      Version result;
      if (const auto dash = text.find('-'); dash != text.npos) {
        auto pre = text.substr(dash + 1);
        while (true) {
          const auto dot = pre.find('.');
          const auto part = pre.substr(0, dot);
          if (!identifier(part) || (digits(part) && part.size() > 1 && part.front() == '0'))
            return {};
          result.prerelease.push_back(part);
          if (dot == pre.npos)
            break;
          pre.remove_prefix(dot + 1);
        }
        text = text.substr(0, dash);
      }
      for (std::size_t i = 0; i < result.numbers.size(); ++i) {
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        if (!digits(part) || (part.size() > 1 && part.front() == '0') || (i < 2) != (dot != text.npos))
          return {};
        const auto converted = std::from_chars(part.data(), part.data() + part.size(), result.numbers[i]);
        if (converted.ec != std::errc{})
          return {};
        if (dot != text.npos)
          text.remove_prefix(dot + 1);
      }
      return result;
    }
    std::string field(const nlohmann::json& value, const char* name) {
      const auto at = value.find(name);
      return at != value.end() && at->is_string() ? at->get<std::string>() : std::string{};
    }
  } // namespace
  bool validVersion(std::string_view version) { return parse(version).has_value(); }
  std::optional<int> compareVersions(std::string_view a, std::string_view b) {
    const auto left = parse(a), right = parse(b);
    if (!left || !right)
      return {};
    if (left->numbers != right->numbers)
      return left->numbers > right->numbers ? 1 : -1;
    if (left->prerelease.empty() != right->prerelease.empty())
      return left->prerelease.empty() ? 1 : -1;
    for (std::size_t i = 0; i < std::min(left->prerelease.size(), right->prerelease.size()); ++i) {
      const auto l = left->prerelease[i], r = right->prerelease[i];
      if (l == r)
        continue;
      const bool ln = digits(l), rn = digits(r);
      if (ln != rn)
        return ln ? -1 : 1;
      if (ln && l.size() != r.size())
        return l.size() > r.size() ? 1 : -1;
      return l > r ? 1 : -1;
    }
    return left->prerelease.size() == right->prerelease.size() ? 0
        : left->prerelease.size() > right->prerelease.size()   ? 1
                                                               : -1;
  }
  bool validDigest(std::string_view digest) {
    constexpr std::size_t sha256Characters = 64;
    return digest.size() == sha256Characters
        && std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
  }
  std::optional<Release> selectRelease(
      const nlohmann::json& response, std::string_view current, std::string_view architecture, bool prereleases
  ) {
    if (!response.is_array() || response.size() > maximumReleases || !validVersion(current))
      throw std::runtime_error("The release service returned invalid version information.");
    std::optional<Release> result;
    for (const auto& item : response) {
      if (!item.is_object()
          || !item.contains("draft")
          || !item["draft"].is_boolean()
          || item["draft"].get<bool>()
          || !item.contains("prerelease")
          || !item["prerelease"].is_boolean())
        continue;
      const auto version = field(item, "tag_name");
      const auto parsed = parse(version);
      if (!parsed
          || version.front() != 'v'
          || version.contains('+')
          || (!prereleases && (item["prerelease"].get<bool>() || !parsed->prerelease.empty())))
        continue;
      if (compareVersions(version, current).value_or(0) <= 0
          || (result && compareVersions(version, result->version).value_or(0) <= 0))
        continue;
      Release release{
          .version = version, .notes = field(item, "body"), .url = std::string(releasesUrl) + "/tag/" + version
      };
      if (release.notes.size() > maximumNotesBytes) {
        auto end = maximumNotesBytes;
        while (end && (static_cast<unsigned char>(release.notes[end]) & 0xc0) == 0x80)
          --end;
        release.notes.resize(end);
      }
      const auto prefix = std::string(releasesUrl) + "/download/" + version + "/";
      const auto name = "cressida-" + version.substr(1) + "-" + std::string(architecture) + ".AppImage";
      if (const auto assets = item.find("assets"); assets != item.end() && assets->is_array()) {
        unsigned imageCount = 0, checksumCount = 0;
        for (const auto& asset : *assets) {
          if (!asset.is_object())
            continue;
          const auto assetName = field(asset, "name");
          const auto url = field(asset, "browser_download_url");
          if (assetName == "SHA256SUMS") {
            ++checksumCount;
            if (url == prefix + assetName)
              release.checksumsUrl = url;
          } else if (assetName == name) {
            ++imageCount;
            const auto size = asset.find("size");
            if (url != prefix + name
                || size == asset.end()
                || !size->is_number_unsigned()
                || size->get<std::uint64_t>() == 0
                || size->get<std::uint64_t>() > maximumImageBytes)
              continue;
            auto digest = field(asset, "digest");
            if (digest.starts_with("sha256:"))
              digest.erase(0, std::string_view("sha256:").size());
            if (!digest.empty() && !validDigest(digest))
              continue;
            release.image = {name, url, digest, size->get<std::uint64_t>()};
          }
        }
        if (imageCount > 1 || checksumCount > 1)
          throw std::runtime_error("The release contains ambiguous download files.");
      }
      result = std::move(release);
    }
    return result;
  }
  std::string checksumFor(std::string_view manifest, const Asset& asset) {
    std::string result;
    while (!manifest.empty()) {
      const auto end = manifest.find('\n');
      auto line = manifest.substr(0, end);
      if (line.ends_with('\r'))
        line.remove_suffix(1);
      constexpr std::size_t digestWidth = 64, checksumSeparator = 2;
      if (line.size() > digestWidth + checksumSeparator
          && line.substr(digestWidth, checksumSeparator) == "  "
          && line.substr(digestWidth + checksumSeparator) == asset.name) {
        if (!result.empty() || !validDigest(line.substr(0, digestWidth)))
          throw std::runtime_error("The release checksum is invalid or duplicated.");
        result = line.substr(0, digestWidth);
      }
      if (end == manifest.npos)
        break;
      manifest.remove_prefix(end + 1);
    }
    if (result.empty() || (!asset.digest.empty() && result != asset.digest))
      throw std::runtime_error("The release checksum is missing or does not match its download metadata.");
    return result;
  }
  Installation detectInstallation() {
    Installation result;
    struct utsname system{};
    if (::uname(&system) == 0)
      result.architecture = system.machine;
    std::error_code error;
    if (std::filesystem::is_regular_file("/.flatpak-info", error))
      result.kind = InstallationKind::Flatpak;
    else if (const char* image = std::getenv("APPIMAGE"); image && *image) {
      const auto* directory = std::getenv("APPDIR");
      const auto executable = std::filesystem::read_symlink("/proc/self/exe", error).string();
      if (directory
          && std::filesystem::path(image).is_absolute()
          && executable.starts_with(std::string(directory) + "/")) {
        result.kind = InstallationKind::AppImage;
        result.image = image;
      }
    } else {
      const auto executable = std::filesystem::read_symlink("/proc/self/exe", error).string();
      if (executable.starts_with("/nix/store/"))
        result.kind = InstallationKind::Nix;
      else if (executable.starts_with("/gnu/store/"))
        result.kind = InstallationKind::Guix;
    }
    return result;
  }
  std::string installationName(InstallationKind kind) {
    switch (kind) {
    case InstallationKind::AppImage:
      return "AppImage";
    case InstallationKind::Flatpak:
      return "Flatpak";
    case InstallationKind::Nix:
      return "Nix";
    case InstallationKind::Guix:
      return "Guix";
    default:
      return "Package or source installation";
    }
  }
  std::string installationHelp(InstallationKind kind) {
    switch (kind) {
    case InstallationKind::AppImage:
      return "Download an update here, then restart when you are ready. The previous image is kept as a backup.";
    case InstallationKind::Flatpak:
      return "Updates use your installed Flatpak repository. A standalone bundle without an update source needs a "
             "newer bundle from Releases.";
    case InstallationKind::Nix:
      return "Update Cressida through your Nix configuration or profile.";
    case InstallationKind::Guix:
      return "Update your Guix channel and upgrade the Cressida package.";
    default:
      return "Use your package manager to update Cressida, or rebuild and install a newer source release.";
    }
  }
} // namespace updates
