#include "app/qr_code.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }

  void finderPattern(const app::QrCode& code, int left, int top) {
    constexpr int side = 7;
    constexpr int innerStart = 2, innerEnd = 4;
    for (int y = 0; y < side; ++y)
      for (int x = 0; x < side; ++x) {
        const bool dark = x == 0
            || y == 0
            || x == side - 1
            || y == side - 1
            || (x >= innerStart && x <= innerEnd && y >= innerStart && y <= innerEnd);
        require(code.at(left + x, top + y) == dark, "All three finder patterns retain standard modules");
      }
  }

  bool hasMediumCorrection(const app::QrCode& code) {
    constexpr int formatCoordinate = 8;
    constexpr std::array<std::pair<int, int>, 15> coordinates{
        {{formatCoordinate, 0},
         {formatCoordinate, 1},
         {formatCoordinate, 2},
         {formatCoordinate, 3},
         {formatCoordinate, 4},
         {formatCoordinate, 5},
         {formatCoordinate, 7},
         {formatCoordinate, formatCoordinate},
         {7, formatCoordinate},
         {5, formatCoordinate},
         {4, formatCoordinate},
         {3, formatCoordinate},
         {2, formatCoordinate},
         {1, formatCoordinate},
         {0, formatCoordinate}}
    };
    unsigned int actual = 0;
    for (std::size_t bit = 0; bit < coordinates.size(); ++bit)
      if (code.at(coordinates[bit].first, coordinates[bit].second))
        actual |= 1U << bit;
    constexpr unsigned int maskCount = 8, parityBits = 10, parityGenerator = 0x537, formatMask = 0x5412;
    for (unsigned int mask = 0; mask < maskCount; ++mask) {
      auto parity = mask;
      for (unsigned int bit = 0; bit < parityBits; ++bit)
        parity = (parity << 1) ^ ((parity >> (parityBits - 1)) * parityGenerator);
      if (actual == (((mask << parityBits) | parity) ^ formatMask))
        return true;
    }
    return false;
  }
} // namespace

int main() {
  try {
    require(!app::encodeQrCode({}), "Empty values do not create a sign-in code");
    require(
        !app::encodeQrCode(std::string(app::maxQrCodeBytes + 1, 'x')), "Oversized values are rejected before encoding"
    );
    require(!app::encodeQrCode(std::string{"abc\0def", 7}), "Embedded NUL bytes are rejected");
    const std::string payload = "https://example.com/qr/synthetic-sign-in";
    const auto code = app::encodeQrCode(payload);
    require(code.has_value(), "A bounded sign-in URL produces a real encoded matrix");
    constexpr int minimumSide = 21, maximumSide = 177, versionSideStep = 4, finderSide = 7;
    require(
        code->side >= minimumSide && code->side <= maximumSide && (code->side - minimumSide) % versionSideStep == 0,
        "The matrix uses a valid QR Model 2 symbol size"
    );
    require(
        code->modules.size() == static_cast<std::size_t>(code->side) * static_cast<std::size_t>(code->side)
            && std::ranges::all_of(code->modules, [](auto module) { return module <= 1; }),
        "The native model contains one normalized byte per module"
    );
    finderPattern(*code, 0, 0);
    finderPattern(*code, code->side - finderSide, 0);
    finderPattern(*code, 0, code->side - finderSide);
    require(hasMediumCorrection(*code), "Format information advertises medium error correction");
    require(app::QrCode::quietZoneModules == 4, "Rendering reserves the complete four-module clear margin");
    for (int offset = -app::QrCode::quietZoneModules; offset < code->side + app::QrCode::quietZoneModules; ++offset)
      require(
          !code->at(-1, offset)
              && !code->at(offset, -1)
              && !code->at(code->side, offset)
              && !code->at(offset, code->side),
          "The virtual quiet zone stays clear on every edge"
      );
    const auto repeated = app::encodeQrCode(payload);
    const auto changed = app::encodeQrCode(payload + "-new");
    require(
        repeated && repeated->modules == code->modules && repeated->side == code->side,
        "Repainting a sign-in code keeps a deterministic matrix"
    );
    require(changed && changed->modules != code->modules, "A new sign-in payload changes the encoded matrix");
    require(
        app::encodeQrCode(std::string(app::maxQrCodeBytes, 'x')).has_value(),
        "The full documented input bound remains encodable"
    );
    require(!app::QrCode{}.at(0, 0), "An empty native model is safe to inspect");
    std::cout << "QR code tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
