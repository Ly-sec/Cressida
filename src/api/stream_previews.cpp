#include "api/stream_previews.h"

#include "api/models.h"
#include "voice/stream_preview.h"

#include <glib.h>
#include <limits>
#include <stdexcept>

namespace api {
  bool streamPreviewRevoked(const StreamPreviewQuery& query) {
    return query.revoked ? query.revoked->load() : query.kind == StreamPreviewKind::Upload;
  }
  std::optional<StreamPreviewHttpRequest>
  streamPreviewHttpRequest(const StreamPreviewQuery& query, std::uint64_t version) {
    const auto key = query.key.value();
    if (key.empty() || !query.request || streamPreviewRevoked(query))
      return {};
    const auto path = "/streams/" + key + "/preview";
    if (query.kind == StreamPreviewKind::Read) {
      if (!query.jpeg.empty()
          || !version
          || version > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return {};
      return StreamPreviewHttpRequest{path + "?version=" + std::to_string(version), "GET", {}};
    }
    if (query.kind != StreamPreviewKind::Upload
        || query.jpeg.size() < 4
        || query.jpeg.size() > voice::maximumPreviewImageBytes
        || query.jpeg[0] != 0xff
        || query.jpeg[1] != 0xd8
        || query.jpeg[query.jpeg.size() - 2] != 0xff
        || query.jpeg.back() != 0xd9)
      return {};
    auto* encoded = g_base64_encode(query.jpeg.data(), query.jpeg.size());
    if (!encoded)
      return {};
    const std::string thumbnail = "data:image/jpeg;base64," + std::string(encoded);
    g_free(encoded);
    return StreamPreviewHttpRequest{path, "POST", {{"thumbnail", thumbnail}}};
  }
  std::string parseStreamPreview(const nlohmann::json& response) {
    if (!response.is_object() || !response.contains("url"))
      throw std::runtime_error("The stream preview response is invalid.");
    const auto& value = response["url"];
    if (value.is_null())
      return {};
    if (!value.is_string())
      throw std::runtime_error("The stream preview URL is invalid.");
    const auto raw = value.get<std::string>();
    if (raw.empty())
      return {};
    const auto url = mediaUrl(raw);
    if (url.empty())
      throw std::runtime_error("The stream preview URL is unavailable.");
    return url;
  }
} // namespace api
