#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace app {
  inline constexpr std::size_t maxQrCodeBytes = 256;

  struct QrCode {
    static constexpr int quietZoneModules = 4;
    int side = 0;
    std::vector<std::uint8_t> modules;

    // The stored matrix excludes the required clear margin on each side.
    [[nodiscard]] bool at(int x, int y) const noexcept;
  };

  std::optional<QrCode> encodeQrCode(std::string_view payload);
} // namespace app
