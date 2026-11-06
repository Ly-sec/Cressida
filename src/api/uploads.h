#pragma once
#include "api/models.h"

#include <curl/curl.h>
#include <memory>
#include <string_view>

namespace api {
  using MessageMime = std::unique_ptr<curl_mime, decltype(&curl_mime_free)>;
  bool randomizeMediaFilenames(std::vector<Upload>& uploads, bool enabled);
  nlohmann::json voiceMessageBody(const Upload& upload, const std::string& nonce);
  MessageMime makeMessageMime(CURL* curl, std::string_view payload, const std::vector<Upload>& uploads);
} // namespace api
