#include "app/qr_code.h"

#include <algorithm>
#include <memory>
#include <qrencode.h>

namespace app {
  namespace {
    constexpr int automaticVersion = 0;
    constexpr int minimumSide = 21, maximumSide = 177, versionSideStep = 4;
    constexpr unsigned char darkModuleMask = 1;
  } // namespace

  bool QrCode::at(int x, int y) const noexcept {
    if (x < 0 || y < 0 || x >= side || y >= side)
      return false;
    const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + static_cast<std::size_t>(x);
    return offset < modules.size() && modules[offset] != 0;
  }

  std::optional<QrCode> encodeQrCode(std::string_view payload) {
    if (payload.empty() || payload.size() > maxQrCodeBytes || payload.contains('\0'))
      return {};
    const std::unique_ptr<QRcode, decltype(&QRcode_free)> encoded{
        QRcode_encodeData(
            static_cast<int>(payload.size()), reinterpret_cast<const unsigned char*>(payload.data()), automaticVersion,
            QR_ECLEVEL_M
        ),
        QRcode_free
    };
    if (!encoded
        || !encoded->data
        || encoded->width < minimumSide
        || encoded->width > maximumSide
        || (encoded->width - minimumSide) % versionSideStep != 0)
      return {};
    QrCode result;
    result.side = encoded->width;
    result.modules.resize(static_cast<std::size_t>(result.side) * static_cast<std::size_t>(result.side));
    std::transform(
        encoded->data, encoded->data + result.modules.size(), result.modules.begin(),
        [](unsigned char value) { return static_cast<std::uint8_t>((value & darkModuleMask) != 0); }
    );
    return result;
  }
} // namespace app
