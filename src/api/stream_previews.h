#pragma once

#include "api/streams.h"

#include <atomic>
#include <memory>

namespace api {
  enum class StreamPreviewKind { Read, Upload };
  struct StreamPreviewQuery {
    StreamPreviewKind kind = StreamPreviewKind::Read;
    StreamKey key;
    std::uint64_t request = 0, generation = 0;
    std::vector<std::uint8_t> jpeg;
    std::shared_ptr<std::atomic_bool> revoked;
  };
  struct StreamPreviewResult {
    StreamKey key;
    std::uint64_t request = 0;
    std::string url;
    bool uploaded = false;
  };
  struct StreamPreviewHttpRequest {
    std::string path, method;
    nlohmann::json body;
  };
  std::optional<StreamPreviewHttpRequest>
  streamPreviewHttpRequest(const StreamPreviewQuery& query, std::uint64_t version);
  // Accepts only a CDN image stored under the requested stream key, or an explicit null.
  std::string parseStreamPreview(const nlohmann::json& response, const StreamKey& key);
  bool streamPreviewRevoked(const StreamPreviewQuery& query);
} // namespace api
