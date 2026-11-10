#include "updates/updater.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
  void require(bool condition, const char* reason) {
    if (!condition)
      throw std::runtime_error(reason);
  }
  template <class F> void rejects(F function, const char* reason) {
    bool failed = false;
    try {
      function();
    } catch (const std::exception&) {
      failed = true;
    }
    require(failed, reason);
  }
  std::string read(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  std::string digest(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
    unsigned size = 0;
    require(EVP_Digest(bytes.data(), bytes.size(), output.data(), &size, EVP_sha256(), nullptr) == 1, "Hash fixture");
    std::string result;
    for (unsigned i = 0; i < size; ++i) {
      constexpr std::string_view hex = "0123456789abcdef";
      result += hex[output[i] >> 4];
      result += hex[output[i] & 15];
    }
    return result;
  }
  nlohmann::json release(std::string version, std::string_view bytes) {
    const auto base = std::string(updates::releasesUrl) + "/download/" + version + "/";
    const auto name = "cressida-" + version.substr(1) + "-x86_64.AppImage";
    return {
        {"tag_name", version},
        {"draft", false},
        {"prerelease", version.contains('-')},
        {"body", "Release notes"},
        {"assets",
         nlohmann::json::array(
             {{{"name", name},
               {"size", static_cast<std::uint64_t>(bytes.size())},
               {"digest", "sha256:" + digest(bytes)},
               {"browser_download_url", base + name}},
              {{"name", "SHA256SUMS"}, {"browser_download_url", base + "SHA256SUMS"}}}
         )}
    };
  }
  updates::Snapshot wait(updates::Updater& updater, updates::Phase phase) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
      if (auto state = updater.poll()) {
        if (state->phase == phase)
          return *state;
        if (state->phase == updates::Phase::Error)
          throw std::runtime_error(state->error);
      }
      pollfd event{updater.eventFd(), POLLIN, 0};
      ::poll(&event, 1, 50);
    }
    throw std::runtime_error("Update fixture timed out");
  }
  struct Fixture {
    std::filesystem::path root, image;
    std::string bytes = std::string(128, '\0');
    nlohmann::json response;
    bool corrupt = false, truncate = false, block = false;
    std::atomic_bool started = false;
    Fixture() {
      std::string pattern = "/tmp/cressida-updates-test.XXXXXX";
      require(::mkdtemp(pattern.data()), "Private test directory");
      root = pattern;
      image = root / "Cressida.AppImage";
      std::ofstream(image) << "old image";
      ::chmod(image.c_str(), 0755);
      bytes[0] = 0x7f;
      bytes[1] = 'E';
      bytes[2] = 'L';
      bytes[3] = 'F';
      bytes[4] = 2;
      bytes[5] = 1;
      bytes[8] = 'A';
      bytes[9] = 'I';
      bytes[10] = 2;
      bytes[18] = 62;
      response = nlohmann::json::array({release("v2.0.0", bytes)});
    }
    ~Fixture() { std::filesystem::remove_all(root); }
    updates::Options options() {
      return {
          .installation = {updates::InstallationKind::AppImage, image, "x86_64"},
          .currentVersion = "v1.0.0",
          .fetch = [this](const std::string& url, const updates::Sink& sink, std::stop_token stop) {
            std::string data;
            if (url == updates::releasesApi)
              data = response.dump();
            else if (url.ends_with("/SHA256SUMS"))
              data = digest(bytes) + "  cressida-2.0.0-x86_64.AppImage\n";
            else {
              started = true;
              while (block && !stop.stop_requested())
                std::this_thread::yield();
              if (stop.stop_requested())
                throw std::runtime_error("Cancelled fixture transfer");
              data = bytes;
              if (corrupt)
                data.back() ^= 1;
              if (truncate)
                data.pop_back();
            }
            sink({reinterpret_cast<const unsigned char*>(data.data()), data.size()});
          }
      };
    }
  };
} // namespace
int main() {
  try {
    require(updates::compareVersions("v1.10.0", "1.9.9") == 1, "Numeric versions do not compare lexically");
    require(updates::compareVersions("v1.0.0-rc.10", "1.0.0-rc.2") == 1, "Numeric prerelease identifiers");
    require(updates::compareVersions("1.0.0", "1.0.0-rc.1") == 1, "Stable versions follow prereleases");
    require(updates::compareVersions("1.0.0+abc", "1.0.0+other") == 0, "Build identifiers do not change precedence");
    for (const auto invalid : {"v1", "1.2", "1.2.3.4", "01.2.3", "1.2.3-01", "1.2.3-", "1.2.3+", "1.2.3/../../bad"})
      require(!updates::validVersion(invalid), "Malformed versions are rejected");
    Fixture fixture;
    auto response = fixture.response;
    response.push_back(release("v3.0.0-rc.1", fixture.bytes));
    auto draft = release("v9.0.0", fixture.bytes);
    draft["draft"] = true;
    response.push_back(draft);
    require(
        updates::selectRelease(response, "1.0.0", "x86_64", false)->version == "v2.0.0",
        "Stable channel excludes drafts and prereleases"
    );
    require(
        updates::selectRelease(response, "1.0.0", "x86_64", true)->version == "v3.0.0-rc.1",
        "Preview channel selects the newest semantic version"
    );
    require(!updates::selectRelease(response, "v4.0.0", "x86_64", true), "Never offer a downgrade");
    auto foreign = fixture.response;
    foreign[0]["assets"][0]["browser_download_url"] = "https://example.test/untrusted.AppImage";
    require(
        updates::selectRelease(foreign, "1.0.0", "x86_64", false)->image.name.empty(),
        "Download URLs remain in the canonical release"
    );
    auto asset = updates::selectRelease(fixture.response, "1.0.0", "x86_64", false)->image;
    const auto checksum = digest(fixture.bytes) + "  " + asset.name + "\n";
    require(updates::checksumFor(checksum, asset) == digest(fixture.bytes), "Exact checksum identity");
    rejects([&] { updates::checksumFor(checksum + checksum, asset); }, "Duplicate checksums are rejected");
    rejects(
        [&] { updates::checksumFor(std::string(64, '0') + "  " + asset.name, asset); },
        "Metadata and checksum disagreement is rejected"
    );
    {
      updates::Updater updater(fixture.options());
      require(updater.check(), "Explicit check starts the worker");
      wait(updater, updates::Phase::Available);
      require(updater.download(), "Explicit download");
      wait(updater, updates::Phase::Ready);
      require(read(fixture.image) == "old image", "Downloading never replaces the running image");
      require(!updater.check(), "Automatic checks retain a prepared update until it is installed or cancelled");
      require(updater.install(), "Explicit installation");
      const auto installed = wait(updater, updates::Phase::Installed);
      require(
          read(fixture.image) == fixture.bytes && read(installed.backup) == "old image",
          "Atomic replacement retains the original image as a backup"
      );
      require(!updater.check() && !updater.download(), "An installed update waits for restart");
    }
    for (const bool truncated : {false, true}) {
      Fixture broken;
      broken.corrupt = !truncated;
      broken.truncate = truncated;
      updates::Updater updater(broken.options());
      updater.check();
      wait(updater, updates::Phase::Available);
      updater.download();
      const auto error = wait(updater, updates::Phase::Error);
      require(
          !error.error.empty() && read(broken.image) == "old image",
          "Damaged and truncated downloads never replace the image"
      );
      broken.corrupt = broken.truncate = false;
      require(updater.download(), "Failures allow an explicit retry");
      wait(updater, updates::Phase::Ready);
    }
    {
      Fixture changed;
      updates::Updater updater(changed.options());
      updater.check();
      wait(updater, updates::Phase::Available);
      updater.download();
      wait(updater, updates::Phase::Ready);
      std::filesystem::rename(changed.image, changed.root / "moved");
      std::ofstream(changed.image) << "a different image";
      updater.install();
      wait(updater, updates::Phase::Error);
      require(read(changed.image) == "a different image", "A replaced original invalidates a prepared installation");
    }
    {
      Fixture changed;
      updates::Updater updater(changed.options());
      updater.check();
      wait(updater, updates::Phase::Available);
      updater.download();
      wait(updater, updates::Phase::Ready);
      std::ofstream(changed.image) << "modified in place";
      updater.install();
      wait(updater, updates::Phase::Error);
      require(read(changed.image) == "modified in place", "In-place changes to the original invalidate installation");
    }
    {
      Fixture changed;
      updates::Updater updater(changed.options());
      updater.check();
      wait(updater, updates::Phase::Available);
      updater.download();
      wait(updater, updates::Phase::Ready);
      for (const auto& file : std::filesystem::directory_iterator(changed.root))
        if (file.path() != changed.image)
          std::ofstream(file.path(), std::ios::app) << "modified";
      updater.install();
      wait(updater, updates::Phase::Error);
      require(read(changed.image) == "old image", "A changed staged file cannot be installed");
    }
    {
      Fixture managed;
      auto configuration = managed.options();
      configuration.installation.kind = updates::InstallationKind::Managed;
      updates::Updater updater(std::move(configuration));
      updater.check();
      wait(updater, updates::Phase::Available);
      require(
          !updater.download() && read(managed.image) == "old image",
          "Managed installations never replace application files"
      );
    }
    {
      Fixture linked;
      std::filesystem::rename(linked.image, linked.root / "target");
      std::filesystem::create_symlink(linked.root / "target", linked.image);
      updates::Updater updater(linked.options());
      updater.check();
      wait(updater, updates::Phase::Available);
      updater.download();
      wait(updater, updates::Phase::Error);
      require(read(linked.root / "target") == "old image", "Updater never follows an image symlink");
    }
    {
      Fixture pending;
      pending.block = true;
      updates::Updater updater(pending.options());
      updater.check();
      wait(updater, updates::Phase::Available);
      updater.download();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
      while (!pending.started && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
      require(pending.started, "Cancellable transfer started");
      updater.cancel();
      wait(updater, updates::Phase::Idle);
      require(read(pending.image) == "old image", "Cancellation preserves the original image");
      require(updater.check(), "A new generation may check after cancellation");
      wait(updater, updates::Phase::Available);
    }
    std::cout << "Release selection, integrity, staging, cancellation, atomic replacement and backup checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
