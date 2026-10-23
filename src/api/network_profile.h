#pragma once

#include "api/request_diagnostics.h"
#include "core/environment.h"
#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <curl/curl.h>

namespace api::network_profile {
  using Clock = std::chrono::steady_clock;
  inline bool enabled() {
    static const bool value = [] {
      const auto* option = cressida::environment("CRESSIDA_PROFILE", "DISCORD_CPP_PROFILE");
      return option && *option;
    }();
    return value;
  }
  inline void elapsed(const char* kind, const char* stage, Clock::time_point started) {
    if (enabled())
      Logger("network-profile")
          .info(
              "action={} stage={} duration_ms={:.1f}", request_diagnostics::action(kind),
              std::string_view(stage) == "queue" ? "queue" : "ready",
              std::chrono::duration<double, std::milli>(Clock::now() - started).count()
          );
  }
  inline void queued(const char* kind, Clock::time_point accepted) { elapsed(kind, "queue", accepted); }
  inline void transferred(const char* kind, CURL* curl, CURLcode result) {
    if (!enabled())
      return;
    curl_off_t dns = 0, connected = 0, tls = 0, firstByte = 0, total = 0;
    long status = 0, connections = 0;
    curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME_T, &dns);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &connected);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME_T, &tls);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, &firstByte);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &connections);
    // Only fixed operation labels and numeric transport data. No URLs, IDs,
    // headers, bodies, search terms, credentials or libcurl verbose output.
    Logger("network-profile")
        .info(
            "action={} transport_code={} status={} new_connections={} dns_ms={:.1f} tcp_ms={:.1f} tls_ms={:.1f} "
            "first_byte_ms={:.1f} total_ms={:.1f}",
            request_diagnostics::action(kind), static_cast<int>(result), status, connections, dns / 1000.0,
            std::max<curl_off_t>(0, connected - dns) / 1000.0, std::max<curl_off_t>(0, tls - connected) / 1000.0,
            firstByte / 1000.0, total / 1000.0
        );
  }
} // namespace api::network_profile
