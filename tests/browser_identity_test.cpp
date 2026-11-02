#include "core/browser_identity.h"

#include <charconv>
#include <curl/curl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
  using Json = nlohmann::json;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }

  struct CurlGlobal {
    CurlGlobal() { require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "Initialize the private HTTP peer"); }
    ~CurlGlobal() { curl_global_cleanup(); }
  };

  std::string decodeBase64(const std::string& encoded) {
    require(!encoded.empty() && encoded.size() % 4 == 0, "Public properties use padded standard Base64");
    require(
        encoded.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
        "Public properties fit the Base64 decoder's bounded input"
    );
    std::string decoded(encoded.size() / 4 * 3, '\0');
    const auto length = EVP_DecodeBlock(
        reinterpret_cast<unsigned char*>(decoded.data()), reinterpret_cast<const unsigned char*>(encoded.data()),
        static_cast<int>(encoded.size())
    );
    require(length >= 0, "Public properties contain valid standard Base64");
    const auto padding = encoded.ends_with("==") ? 2 : encoded.ends_with('=') ? 1 : 0;
    require(length >= padding, "Public properties have valid Base64 padding");
    decoded.resize(static_cast<std::size_t>(length - padding));
    return decoded;
  }

  void publicProfile() {
    const auto& agent = browser_identity::userAgent();
    require(
        agent.starts_with("Mozilla/5.0 (") && agent.contains("Linux") && agent.contains("Chrome/128.0.0.0"),
        "The pinned identity carries a consistent browser User-Agent"
    );
    const Json expected{
        {"os", "Linux"},
        {"browser", "Chrome"},
        {"device", ""},
        {"system_locale", "en-US"},
        {"browser_user_agent", agent},
        {"browser_version", "128.0.0.0"},
        {"os_version", ""},
        {"referrer", ""},
        {"referring_domain", ""},
        {"referrer_current", ""},
        {"referring_domain_current", ""},
        {"release_channel", "stable"},
        {"client_build_number", 634304},
        {"client_event_source", nullptr},
    };
    const auto& properties = browser_identity::properties();
    require(properties == expected, "HTTP and Identify share the complete pinned public profile");
    for (const auto* field : {"token", "authorization", "session_id", "user_id", "account_id", "installation_id"})
      require(!properties.contains(field), "Identity properties exclude credentials and account identifiers");
    const auto encoded = browser_identity::encodedProperties();
    require(
        Json::parse(decodeBase64(encoded)) == properties,
        "The HTTP properties header decodes to the same object used by Identify"
    );
    require(
        encoded == browser_identity::encodedProperties()
            && agent == browser_identity::userAgent()
            && properties == browser_identity::properties(),
        "Public identity is deterministic across repeated requests"
    );
  }

  void append(browser_identity::Headers& headers, const char* value) {
    auto* next = curl_slist_append(headers.get(), value);
    require(next != nullptr, "Allocate synthetic request headers");
    (void)headers.release();
    headers.reset(next);
  }

  std::size_t receive(char* data, std::size_t size, std::size_t count, void* context) {
    const auto bytes = size * count;
    static_cast<std::string*>(context)->append(data, bytes);
    return bytes;
  }

  void privateRequests(std::string_view origin) {
    constexpr std::string_view prefix = "http://127.0.0.1:";
    constexpr unsigned maximumTcpPort = 65535;
    require(origin.starts_with(prefix), "Identity capture requires an explicit loopback HTTP origin");
    const auto portText = origin.substr(prefix.size());
    unsigned port = 0;
    const auto parsed = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    require(
        parsed.ec == std::errc{}
            && parsed.ptr == portText.data() + portText.size()
            && port > 0
            && port <= maximumTcpPort,
        "The fixture origin contains only a valid numeric loopback port"
    );
    CurlGlobal global;
    browser_identity::Headers headers(nullptr, curl_slist_free_all);
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    require(curl != nullptr, "Allocate the private HTTP handle");
    const auto send = [&](CURL* handle, const char* path) {
      constexpr long requestTimeoutMs = 3000;
      const auto url = std::string(origin) + path;
      std::string response;
      curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
      curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http");
      curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(handle, CURLOPT_PROXY, "");
      curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, requestTimeoutMs);
      curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, receive);
      curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
      const auto result = curl_easy_perform(handle);
      long status = 0;
      curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
      require(result == CURLE_OK && status == 200 && response == "ok", "The private peer accepts the emitted headers");
    };
    for (const auto* path : {"/rest-first", "/rest-reset"}) {
      curl_easy_reset(curl.get());
      headers.reset();
      append(headers, "Authorization: synthetic-identity-token");
      append(headers, "Content-Type: application/json");
      require(
          browser_identity::apply(curl.get(), browser_identity::Transport::Rest, headers),
          "Apply the production REST identity after a handle reset"
      );
      send(curl.get(), path);
    }
    curl_easy_reset(curl.get());
    headers.reset();
    require(
        browser_identity::apply(curl.get(), browser_identity::Transport::Gateway, headers),
        "Apply the production Gateway handshake identity"
    );
    send(curl.get(), "/gateway");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> unrelated(curl_easy_init(), curl_easy_cleanup);
    require(unrelated != nullptr, "Allocate an unrelated origin handle");
    send(unrelated.get(), "/unrelated");
  }
} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "A private loopback fixture origin is required");
    publicProfile();
    privateRequests(argv[1]);
    std::cout << "Shared browser identity checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
