#include "core/browser_identity.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <string_view>

namespace browser_identity {
  namespace {
    constexpr std::string_view browserVersion = "128.0.0.0";
    constexpr std::uint64_t webClientBuildNumber = 634304;
    constexpr std::string_view systemLocale = "en-US";
    constexpr std::string_view origin = "https://discord.com";
    constexpr std::string_view referer = "https://discord.com/channels/@me";
    constexpr std::size_t base64InputBlockBytes = 3;
    constexpr std::size_t base64OutputBlockBytes = 4;
    constexpr std::size_t terminatorBytes = 1;
#if defined(__aarch64__)
    constexpr std::string_view platform = "X11; Linux aarch64";
#elif defined(__x86_64__)
    constexpr std::string_view platform = "X11; Linux x86_64";
#else
    constexpr std::string_view platform = "X11; Linux";
#endif

    bool append(Headers& headers, std::string_view name, std::string_view value) {
      const auto line = std::string(name) + ": " + std::string(value);
      auto* next = curl_slist_append(headers.get(), line.c_str());
      if (!next)
        return false;
      (void)headers.release();
      headers.reset(next);
      return true;
    }
  } // namespace

  const std::string& userAgent() {
    static const auto value = std::string("Mozilla/5.0 (")
        + std::string(platform)
        + ") AppleWebKit/537.36 (KHTML, like Gecko) Chrome/"
        + std::string(browserVersion)
        + " Safari/537.36";
    return value;
  }

  const nlohmann::json& properties() {
    static const nlohmann::json value{
        {"os", "Linux"},
        {"browser", "Chrome"},
        {"device", ""},
        {"system_locale", systemLocale},
        {"browser_user_agent", userAgent()},
        {"browser_version", browserVersion},
        {"os_version", ""},
        {"referrer", ""},
        {"referring_domain", ""},
        {"referrer_current", ""},
        {"referring_domain_current", ""},
        {"release_channel", "stable"},
        {"client_build_number", webClientBuildNumber},
        {"client_event_source", nullptr}
    };
    return value;
  }

  std::string encodedProperties() {
    static const auto value = [] {
      const auto source = properties().dump();
      constexpr auto maximumEncodedLength = static_cast<std::size_t>(std::numeric_limits<int>::max());
      if (source.size() > maximumEncodedLength)
        return std::string{};
      const auto encodedLength =
          ((source.size() + base64InputBlockBytes - 1) / base64InputBlockBytes) * base64OutputBlockBytes;
      if (encodedLength > maximumEncodedLength)
        return std::string{};
      std::string encoded(encodedLength + terminatorBytes, '\0');
      const auto written = EVP_EncodeBlock(
          reinterpret_cast<unsigned char*>(encoded.data()), reinterpret_cast<const unsigned char*>(source.data()),
          static_cast<int>(source.size())
      );
      if (written < 0 || static_cast<std::size_t>(written) != encodedLength)
        return std::string{};
      encoded.resize(encodedLength);
      return encoded;
    }();
    return value;
  }

  bool apply(CURL* connection, Transport transport, Headers& headers) {
    if (!connection || (transport != Transport::Rest && transport != Transport::Gateway))
      return false;
    if (!append(headers, "Accept-Language", "en-US,en;q=0.9") || !append(headers, "Origin", origin))
      return false;
    if (transport == Transport::Rest) {
      const auto encoded = encodedProperties();
      if (encoded.empty()
          || !append(headers, "Referer", referer)
          || !append(headers, "X-Super-Properties", encoded)
          || !append(headers, "X-Discord-Locale", systemLocale)
          || !append(headers, "X-Discord-Timezone", "UTC"))
        return false;
    } else if (!append(headers, "Cache-Control", "no-cache") || !append(headers, "Pragma", "no-cache"))
      return false;
    return curl_easy_setopt(connection, CURLOPT_USERAGENT, userAgent().c_str()) == CURLE_OK
        && curl_easy_setopt(connection, CURLOPT_HTTPHEADER, headers.get()) == CURLE_OK;
  }
} // namespace browser_identity
