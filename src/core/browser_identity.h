#pragma once

#include <curl/curl.h>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace browser_identity {
  enum class Transport { Rest, Gateway };
  using Headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

  const std::string& userAgent();
  const nlohmann::json& properties();
  std::string encodedProperties();
  // The caller retains the header list until the transfer has finished.
  bool apply(CURL* connection, Transport transport, Headers& headers);
} // namespace browser_identity
