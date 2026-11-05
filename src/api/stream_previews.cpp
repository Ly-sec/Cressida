#include "api/stream_previews.h"

#include "api/models.h"
#include "voice/stream_preview.h"

#include <algorithm>
#include <glib.h>
#include <limits>
#include <stdexcept>
#include <string_view>

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
  namespace {
    constexpr std::size_t minimumPreviewHashBytes = 8, maximumPreviewHashBytes = 128;
    // The CDN serves a preview at /streams/{stream_key}/{hash}.png and may sign it with a query.
    bool previewPathMatches(std::string_view path, std::string_view key) {
      std::string normalized;
      for (std::size_t at = 0; at < path.size(); ++at) {
        if (path[at] == '%'
            && at + 2 < path.size()
            && path[at + 1] == '3'
            && (path[at + 2] == 'A' || path[at + 2] == 'a')) {
          normalized += ':';
          at += 2;
        } else
          normalized += path[at];
      }
      const auto prefix = std::string(key) + "/";
      if (!normalized.starts_with(prefix))
        return false;
      auto hash = std::string_view(normalized).substr(prefix.size());
      hash = hash.substr(0, hash.find_first_of("?#"));
      for (const auto extension : {".png", ".jpg", ".jpeg", ".webp"})
        if (hash.ends_with(extension)) {
          hash.remove_suffix(std::string_view(extension).size());
          break;
        }
      return hash.size() >= minimumPreviewHashBytes
          && hash.size() <= maximumPreviewHashBytes
          && std::ranges::all_of(hash, [](unsigned char c) {
               return (c >= '0' && c <= '9')
                   || (c >= 'a' && c <= 'z')
                   || (c >= 'A' && c <= 'Z')
                   || c == '-'
                   || c == '_';
             });
    }
  } // namespace
  std::string parseStreamPreview(const nlohmann::json& response, const StreamKey& key) {
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
    const auto expected = key.value();
    constexpr std::string_view cdn = "https://cdn.discordapp.com/streams/",
                               proxy = "https://media.discordapp.net/streams/";
    const auto path = url.starts_with(cdn) ? std::string_view(url).substr(cdn.size())
        : url.starts_with(proxy)           ? std::string_view(url).substr(proxy.size())
                                           : std::string_view{};
    if (expected.empty() || !previewPathMatches(path, expected))
      throw std::runtime_error("The stream preview belongs to a different stream.");
    return url;
  }
} // namespace api
