#include "auth/qr_login.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <curl/curl.h>
#include <curl/websockets.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace auth {
  namespace {
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    constexpr int rsaBits = 2048;
    constexpr std::size_t ciphertextBytes = rsaBits / 8;
    constexpr std::size_t ciphertextEncodedBytes = (ciphertextBytes + 2) / 3 * 4;
    constexpr std::size_t maximumSignalBytes = 32 * 1024;
    constexpr std::size_t maximumTicketBytes = 4096;
    constexpr std::size_t maximumNameBytes = 128;
    constexpr std::size_t maximumIdentityBytes = 20;
    constexpr std::size_t avatarHashBytes = 32;
    constexpr std::size_t maximumReceiveBatch = 16;
    constexpr auto setupTimeout = 20s;
    constexpr auto transferTimeout = 20s;
    constexpr auto maximumLifetime = 10min;
    constexpr auto minimumHeartbeat = 100ms;
    constexpr auto maximumHeartbeat = 2min;
    constexpr auto maximumPoll = 100ms;
    constexpr std::string_view qrPrefix = "https://discord.com/ra/";
    constexpr std::string_view originHeader = "Origin: https://discord.com";
    constexpr std::string_view userAgent = "Cressida/0.1";

    struct Failure : std::runtime_error {
      using std::runtime_error::runtime_error;
    };
    void wipe(std::string& value) {
      if (!value.empty())
        OPENSSL_cleanse(value.data(), value.size());
      value.clear();
    }
    std::string base64(std::span<const unsigned char> bytes, bool url = false) {
      std::string result((bytes.size() + 2) / 3 * 4, '\0');
      const auto length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), bytes.data(), bytes.size());
      if (length < 0)
        throw Failure("QR sign-in encryption failed.");
      result.resize(length);
      if (url) {
        std::replace(result.begin(), result.end(), '+', '-');
        std::replace(result.begin(), result.end(), '/', '_');
        while (result.ends_with('='))
          result.pop_back();
      }
      return result;
    }
    bool digits(std::string_view value) {
      return !value.empty() && std::ranges::all_of(value, [](unsigned char c) { return c >= '0' && c <= '9'; });
    }
    bool identity(std::string_view value) {
      if (value.empty() || value.size() > maximumIdentityBytes || value.front() == '0' || !digits(value))
        return false;
      std::uint64_t number = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
      return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && number != 0;
    }
    bool text(std::string_view value, std::size_t maximum, bool ascii = false) {
      if (value.empty() || value.size() > maximum)
        return false;
      for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 32 || first == 127 || (ascii && first >= 128))
          return false;
        if (first < 128)
          continue;
        unsigned count = 0;
        std::uint32_t scalar = 0, minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) {
          count = 1;
          scalar = first & 0x1F;
          minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
          count = 2;
          scalar = first & 0x0F;
          minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
          count = 3;
          scalar = first & 0x07;
          minimum = 0x10000;
        } else
          return false;
        if (count > value.size() - i)
          return false;
        while (count--) {
          const auto next = static_cast<unsigned char>(value[i++]);
          if ((next & 0xC0) != 0x80)
            return false;
          scalar = scalar << 6 | (next & 0x3F);
        }
        if (scalar < minimum
            || scalar > 0x10FFFF
            || (scalar >= 0xD800 && scalar <= 0xDFFF)
            || (scalar >= 0x80 && scalar <= 0x9F))
          return false;
      }
      return true;
    }
    std::optional<QrAccount> account(std::string_view payload) {
      std::array<std::string_view, 4> parts;
      for (std::size_t i = 0; i < parts.size() - 1; ++i) {
        const auto delimiter = payload.find(':');
        if (delimiter == std::string_view::npos)
          return {};
        parts[i] = payload.substr(0, delimiter);
        payload.remove_prefix(delimiter + 1);
      }
      parts.back() = payload;
      if (!identity(parts[0]) || parts[1].size() > 4 || !digits(parts[1]) || !text(parts[3], maximumNameBytes))
        return {};
      auto avatar = parts[2];
      if (avatar == "0" || avatar == "None" || avatar == "null")
        avatar = {};
      else {
        if (avatar.starts_with("a_"))
          avatar.remove_prefix(2);
        if (avatar.size() != avatarHashBytes || !std::ranges::all_of(avatar, [](unsigned char c) {
              return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
          return {};
        avatar = parts[2];
      }
      return QrAccount{std::string(parts[0]), std::string(parts[3]), std::string(avatar)};
    }
    std::string field(const Json& value, const char* name, std::size_t maximum) {
      const auto found = value.find(name);
      if (found == value.end() || !found->is_string())
        throw Failure("QR sign-in received an invalid response. Try browser sign-in.");
      const auto& result = found->get_ref<const std::string&>();
      if (result.empty() || result.size() > maximum)
        throw Failure("QR sign-in received an invalid response. Try browser sign-in.");
      return result;
    }
    std::chrono::milliseconds milliseconds(const Json& value, const char* name, std::chrono::milliseconds maximum) {
      const auto found = value.find(name);
      if (found == value.end()
          || !found->is_number_integer()
          || (found->is_number_unsigned() && found->get<std::uint64_t>() > static_cast<std::uint64_t>(maximum.count())))
        throw Failure("QR sign-in received invalid timing information.");
      const auto count = found->get<std::int64_t>();
      if (count <= 0 || count > maximum.count())
        throw Failure("QR sign-in received invalid timing information.");
      return std::chrono::milliseconds(count);
    }
    bool terminal(QrStage stage) {
      return stage == QrStage::Authenticated
          || stage == QrStage::Expired
          || stage == QrStage::Cancelled
          || stage == QrStage::Failed;
    }
    bool loopback(std::string_view value, std::string_view scheme, std::string_view route) {
      const std::string prefix = std::string(scheme) + "://127.0.0.1:";
      if (!value.starts_with(prefix))
        return false;
      value.remove_prefix(prefix.size());
      const auto delimiter = value.find('/');
      if (delimiter == std::string_view::npos || value.substr(delimiter) != route)
        return false;
      const auto port = value.substr(0, delimiter);
      unsigned number = 0;
      const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
      return digits(port)
          && parsed.ec == std::errc{}
      && number > 0
          && number <= std::numeric_limits<std::uint16_t>::max();
    }
    bool validEndpoints(const QrEndpoints& endpoints) {
      const QrEndpoints defaults;
      return (endpoints.gateway == defaults.gateway && endpoints.login == defaults.login)
          || (loopback(endpoints.gateway, "ws", "/?v=2")
              && loopback(endpoints.login, "http", "/api/v9/users/@me/remote-auth/login"));
    }
    struct Context {
      std::stop_token stop;
      const process::WakeEvent& wake;
      bool cancelled() const { return stop.stop_requested(); }
    };
    int progress(void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
      return static_cast<Context*>(data)->cancelled();
    }
    struct Transfer {
      std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy{curl_easy_init(), curl_easy_cleanup};
      std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi{curl_multi_init(), curl_multi_cleanup};
      bool added = false;
      Transfer() {
        if (!easy || !multi)
          throw Failure("QR sign-in could not start. Try browser sign-in.");
      }
      ~Transfer() {
        if (added)
          curl_multi_remove_handle(multi.get(), easy.get());
      }
      CURLcode perform(const Context& context, Clock::time_point deadline) {
        if (curl_multi_add_handle(multi.get(), easy.get()) != CURLM_OK)
          throw Failure("QR sign-in could not start. Try browser sign-in.");
        added = true;
        int running = 0;
        do {
          if (context.cancelled())
            throw Failure("QR sign-in was cancelled.");
          if (Clock::now() >= deadline)
            throw Failure("QR sign-in timed out. Try again or use browser sign-in.");
          if (curl_multi_perform(multi.get(), &running) != CURLM_OK)
            throw Failure("QR sign-in connection failed. Try browser sign-in.");
          if (running) {
            curl_waitfd descriptor{context.wake.fd(), CURL_WAIT_POLLIN, 0};
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            const auto wait = static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, maximumPoll.count()));
            if (curl_multi_poll(multi.get(), &descriptor, 1, wait, nullptr) != CURLM_OK)
              throw Failure("QR sign-in connection failed. Try browser sign-in.");
            if (descriptor.revents)
              context.wake.drain();
          }
        } while (running);
        int remaining = 0;
        while (const auto* message = curl_multi_info_read(multi.get(), &remaining))
          if (message->msg == CURLMSG_DONE)
            return message->data.result;
        return CURLE_FAILED_INIT;
      }
    };
    void configure(CURL* easy, const std::string& url, bool local, const Context& context) {
      curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
      curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, local ? "http,ws" : "https,wss");
      curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
      curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(
          easy, CURLOPT_CONNECTTIMEOUT_MS,
          static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(transferTimeout).count())
      );
      curl_easy_setopt(
          easy, CURLOPT_TIMEOUT_MS,
          static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(transferTimeout).count())
      );
      curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, progress);
      curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &context);
      curl_easy_setopt(easy, CURLOPT_USERAGENT, userAgent.data());
      if (local)
        curl_easy_setopt(easy, CURLOPT_NOPROXY, "127.0.0.1");
    }
    void wait(CURL* easy, const Context& context, short events, Clock::time_point deadline) {
      curl_socket_t socket = CURL_SOCKET_BAD;
      curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &socket);
      pollfd descriptors[]{{socket, events, 0}, {context.wake.fd(), POLLIN, 0}};
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
      ::poll(
          descriptors, std::size(descriptors),
          static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, maximumPoll.count()))
      );
      if (descriptors[1].revents)
        context.wake.drain();
    }
    void send(CURL* easy, const Json& message, const Context& context) {
      const auto bytes = message.dump();
      const auto deadline = Clock::now() + transferTimeout;
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        if (context.cancelled())
          throw Failure("QR sign-in was cancelled.");
        if (Clock::now() >= deadline)
          throw Failure("QR sign-in timed out. Try browser sign-in.");
        std::size_t written = 0;
        const auto result = curl_ws_send(easy, bytes.data() + offset, bytes.size() - offset, &written, 0, CURLWS_TEXT);
        offset += written;
        if (result == CURLE_AGAIN)
          wait(easy, context, POLLOUT, deadline);
        else if (result != CURLE_OK)
          throw Failure("QR sign-in connection was interrupted. Try again or use browser sign-in.");
      }
    }
    size_t responseBytes(char* bytes, size_t size, size_t count, void* data) {
      auto& response = *static_cast<std::string*>(data);
      if (count > maximumSignalBytes || size > maximumSignalBytes / std::max<std::size_t>(1, count))
        return 0;
      const auto length = size * count;
      if (length > maximumSignalBytes - response.size())
        return 0;
      response.append(bytes, length);
      return length;
    }
    Json exchange(
        const QrEndpoints& endpoints, const std::string& ticket, const Context& context, Clock::time_point expiry
    ) {
      Transfer request;
      const bool local = endpoints.login.starts_with("http://127.0.0.1:");
      configure(request.easy.get(), endpoints.login, local, context);
      std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
          curl_slist_append(nullptr, "Content-Type: application/json"), curl_slist_free_all
      );
      if (!headers)
        throw Failure("QR sign-in could not complete. Try browser sign-in.");
      const auto body = Json{{"ticket", ticket}}.dump();
      std::string response;
      curl_easy_setopt(request.easy.get(), CURLOPT_HTTPHEADER, headers.get());
      curl_easy_setopt(request.easy.get(), CURLOPT_POST, 1L);
      curl_easy_setopt(request.easy.get(), CURLOPT_POSTFIELDS, body.c_str());
      curl_easy_setopt(request.easy.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
      curl_easy_setopt(request.easy.get(), CURLOPT_WRITEFUNCTION, responseBytes);
      curl_easy_setopt(request.easy.get(), CURLOPT_WRITEDATA, &response);
      const auto result = request.perform(context, std::min(expiry, Clock::now() + transferTimeout));
      long status = 0;
      curl_easy_getinfo(request.easy.get(), CURLINFO_RESPONSE_CODE, &status);
      const auto parsed = Json::parse(response, nullptr, false);
      wipe(response);
      if (parsed.is_object()
          && (parsed.contains("captcha_key") || parsed.contains("captcha_service") || parsed.value("mfa", false)))
        throw Failure("This sign-in needs an additional security check. Continue with browser sign-in.");
      if (result != CURLE_OK || status < 200 || status >= 300 || !parsed.is_object())
        throw Failure("QR sign-in could not be confirmed. Try again or use browser sign-in.");
      return parsed;
    }
  } // namespace

  struct QrSession::Impl {
    enum class Step { Hello, Nonce, Fingerprint, Scan, Approval, Exchange, Done };
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
    std::vector<unsigned char> publicDer;
    std::string publicEncoded, fingerprint, ticket, token;
    QrUpdate update;
    Step step = Step::Hello;
    Clock::time_point created, setupDeadline, heartbeatAt;
    std::chrono::milliseconds heartbeatInterval{};
    bool waitingForAck = false;
    explicit Impl(Clock::time_point now)
        : created(now), setupDeadline(now + setupTimeout), heartbeatAt(Clock::time_point::max()) {
      update.expiresAt = now + maximumLifetime;
      key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", rsaBits));
      if (!key)
        throw Failure("QR sign-in encryption could not start. Try browser sign-in.");
      const auto size = i2d_PUBKEY(key.get(), nullptr);
      if (size <= 0)
        throw Failure("QR sign-in encryption could not start. Try browser sign-in.");
      publicDer.resize(size);
      auto* output = publicDer.data();
      if (i2d_PUBKEY(key.get(), &output) != size)
        throw Failure("QR sign-in encryption could not start. Try browser sign-in.");
      publicEncoded = base64(publicDer);
      std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
      unsigned length = 0;
      if (EVP_Digest(publicDer.data(), publicDer.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1)
        throw Failure("QR sign-in encryption could not start. Try browser sign-in.");
      fingerprint = base64(std::span(digest).first(length), true);
    }
    ~Impl() {
      wipe(ticket);
      wipe(token);
    }
    std::string decrypt(const std::string& encoded) const {
      if (encoded.size() != ciphertextEncodedBytes)
        throw Failure("QR sign-in received invalid encrypted data. Try browser sign-in.");
      const auto padding = encoded.ends_with("==") ? 2 : encoded.ends_with('=') ? 1 : 0;
      if (!std::ranges::all_of(std::string_view(encoded).substr(0, encoded.size() - padding), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
          }))
        throw Failure("QR sign-in received invalid encrypted data. Try browser sign-in.");
      std::vector<unsigned char> encrypted(encoded.size() / 4 * 3);
      const auto decoded =
          EVP_DecodeBlock(encrypted.data(), reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
      if (decoded < 0 || static_cast<std::size_t>(decoded - padding) != ciphertextBytes)
        throw Failure("QR sign-in received invalid encrypted data. Try browser sign-in.");
      encrypted.resize(ciphertextBytes);
      std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
          EVP_PKEY_CTX_new(key.get(), nullptr), EVP_PKEY_CTX_free
      );
      if (!context
          || EVP_PKEY_decrypt_init(context.get()) <= 0
          || EVP_PKEY_CTX_set_rsa_padding(context.get(), RSA_PKCS1_OAEP_PADDING) <= 0
          || EVP_PKEY_CTX_set_rsa_oaep_md(context.get(), EVP_sha256()) <= 0
          || EVP_PKEY_CTX_set_rsa_mgf1_md(context.get(), EVP_sha256()) <= 0)
        throw Failure("QR sign-in encryption failed. Try browser sign-in.");
      std::size_t length = 0;
      if (EVP_PKEY_decrypt(context.get(), nullptr, &length, encrypted.data(), encrypted.size()) <= 0
          || length > ciphertextBytes)
        throw Failure("QR sign-in received invalid encrypted data. Try browser sign-in.");
      std::string plaintext(length, '\0');
      if (EVP_PKEY_decrypt(
              context.get(), reinterpret_cast<unsigned char*>(plaintext.data()), &length, encrypted.data(),
              encrypted.size()
          )
          <= 0) {
        wipe(plaintext);
        throw Failure("QR sign-in received invalid encrypted data. Try browser sign-in.");
      }
      plaintext.resize(length);
      return plaintext;
    }
  };

  QrSession::QrSession(Clock::time_point now) : m(std::make_unique<Impl>(now)) {}
  QrSession::~QrSession() = default;
  const QrUpdate& QrSession::view() const { return m->update; }
  bool QrSession::terminal() const { return auth::terminal(m->update.stage); }
  void QrSession::fail(std::string error) {
    m->update.stage = QrStage::Failed;
    m->update.error = std::move(error);
    m->update.qrUrl.clear();
    m->update.account.reset();
    wipe(m->ticket);
    wipe(m->token);
    m->step = Impl::Step::Done;
  }
  QrSession::Clock::time_point QrSession::nextDeadline() const {
    auto next = std::min(m->update.expiresAt, m->heartbeatAt);
    if (m->step == Impl::Step::Hello || m->step == Impl::Step::Nonce || m->step == Impl::Step::Fingerprint)
      next = std::min(next, m->setupDeadline);
    return next;
  }
  std::optional<Json> QrSession::tick(Clock::time_point now) {
    if (terminal())
      return {};
    if (now >= m->update.expiresAt) {
      fail("This QR code has expired. Generate a new code to try again.");
      m->update.stage = QrStage::Expired;
      return {};
    }
    if ((m->step == Impl::Step::Hello || m->step == Impl::Step::Nonce || m->step == Impl::Step::Fingerprint)
        && now >= m->setupDeadline) {
      fail("QR sign-in could not start in time. Try again or use browser sign-in.");
      return {};
    }
    if (now >= m->heartbeatAt) {
      if (m->waitingForAck) {
        fail("QR sign-in lost its connection. Try again or use browser sign-in.");
        return {};
      }
      m->waitingForAck = true;
      m->heartbeatAt = now + m->heartbeatInterval;
      return Json{{"op", "heartbeat"}};
    }
    return {};
  }
  std::optional<Json> QrSession::receive(const Json& message, Clock::time_point now) {
    if (terminal())
      return {};
    const bool settingUp =
        m->step == Impl::Step::Hello || m->step == Impl::Step::Nonce || m->step == Impl::Step::Fingerprint;
    if (now >= m->update.expiresAt
        || (settingUp && now >= m->setupDeadline)
        || (m->waitingForAck && now >= m->heartbeatAt)) {
      tick(now);
      return {};
    }
    try {
      if (!message.is_object())
        throw Failure("QR sign-in received an invalid response. Try browser sign-in.");
      const auto op = field(message, "op", 64);
      if (op == "heartbeat_ack") {
        if (!m->waitingForAck)
          throw Failure("QR sign-in received an unexpected heartbeat response.");
        m->waitingForAck = false;
        return {};
      }
      if (op == "cancel") {
        fail("Sign-in was cancelled on your phone. Generate a new code to try again.");
        m->update.stage = QrStage::Cancelled;
        return {};
      }
      if (op == "hello" && m->step == Impl::Step::Hello) {
        m->heartbeatInterval = milliseconds(message, "heartbeat_interval", maximumHeartbeat);
        if (m->heartbeatInterval < minimumHeartbeat)
          throw Failure("QR sign-in received invalid timing information.");
        const auto timeout = milliseconds(message, "timeout_ms", maximumLifetime);
        m->update.expiresAt = std::min(m->created + maximumLifetime, now + timeout);
        m->heartbeatAt = now + m->heartbeatInterval;
        m->step = Impl::Step::Nonce;
        return Json{{"op", "init"}, {"encoded_public_key", m->publicEncoded}};
      }
      if (op == "nonce_proof" && m->step == Impl::Step::Nonce) {
        auto nonce = m->decrypt(field(message, "encrypted_nonce", ciphertextEncodedBytes));
        if (nonce.empty())
          throw Failure("QR sign-in received an invalid challenge.");
        const auto proof = base64({reinterpret_cast<const unsigned char*>(nonce.data()), nonce.size()}, true);
        wipe(nonce);
        m->step = Impl::Step::Fingerprint;
        return Json{{"op", "nonce_proof"}, {"nonce", proof}};
      }
      if (op == "pending_remote_init" && m->step == Impl::Step::Fingerprint) {
        const auto fingerprint = field(message, "fingerprint", m->fingerprint.size());
        if (fingerprint.size() != m->fingerprint.size()
            || CRYPTO_memcmp(fingerprint.data(), m->fingerprint.data(), fingerprint.size()) != 0)
          throw Failure("QR sign-in could not verify this code. Try browser sign-in.");
        m->update.qrUrl = std::string(qrPrefix) + fingerprint;
        m->update.stage = QrStage::Ready;
        m->step = Impl::Step::Scan;
        return {};
      }
      if (op == "pending_ticket" && m->step == Impl::Step::Scan) {
        auto payload = m->decrypt(field(message, "encrypted_user_payload", ciphertextEncodedBytes));
        m->update.account = account(payload);
        wipe(payload);
        if (!m->update.account)
          throw Failure("QR sign-in received an invalid account preview. Try browser sign-in.");
        m->update.stage = QrStage::AwaitingApproval;
        m->step = Impl::Step::Approval;
        return {};
      }
      if (op == "pending_login" && m->step == Impl::Step::Approval) {
        m->ticket = field(message, "ticket", maximumTicketBytes);
        if (!text(m->ticket, maximumTicketBytes, true))
          throw Failure("QR sign-in received an invalid approval. Try browser sign-in.");
        m->update.stage = QrStage::Completing;
        m->update.qrUrl.clear();
        m->step = Impl::Step::Exchange;
        return {};
      }
      throw Failure("QR sign-in received an unexpected response. Try browser sign-in.");
    } catch (const Failure& error) {
      fail(error.what());
      return {};
    } catch (...) {
      fail("QR sign-in received an invalid response. Try browser sign-in.");
      return {};
    }
  }
  std::optional<std::string> QrSession::takeTicket() {
    if (m->step != Impl::Step::Exchange || m->ticket.empty())
      return {};
    return std::exchange(m->ticket, {});
  }
  void QrSession::finish(const Json& response) {
    if (m->step != Impl::Step::Exchange || terminal())
      return;
    try {
      m->token = m->decrypt(field(response, "encrypted_token", ciphertextEncodedBytes));
      if (!text(m->token, ciphertextBytes, true) || m->token.contains(' '))
        throw Failure("QR sign-in returned an invalid credential. Try browser sign-in.");
      m->update.stage = QrStage::Authenticated;
      m->update.qrUrl.clear();
      m->step = Impl::Step::Done;
    } catch (...) {
      fail("QR sign-in could not be confirmed. Try browser sign-in.");
    }
  }
  std::optional<std::string> QrSession::takeToken() {
    if (m->update.stage != QrStage::Authenticated || m->token.empty())
      return {};
    return std::exchange(m->token, {});
  }

  QrLogin::QrLogin(QrEndpoints endpoints) : m_endpoints(std::move(endpoints)) {}
  QrLogin::~QrLogin() { cancel(); }
  bool QrLogin::start() {
    cancel();
    if (!validEndpoints(m_endpoints) || !m_updatesWake.valid() || !m_cancelWake.valid())
      return false;
    static std::once_flag initialized;
    static CURLcode initialization = CURLE_FAILED_INIT;
    std::call_once(initialized, [] { initialization = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initialization != CURLE_OK)
      return false;
    m_active = true;
    publish({});
    try {
      m_worker = std::jthread([this](std::stop_token stop) { run(stop); });
    } catch (...) {
      m_active = false;
      publish({.stage = QrStage::Failed, .error = "QR sign-in could not start. Try browser sign-in."});
      return false;
    }
    return true;
  }
  void QrLogin::cancel() {
    if (m_worker.joinable()) {
      m_worker.request_stop();
      (void)m_cancelWake.signal();
      m_worker.join();
    }
    m_cancelWake.drain();
    m_updatesWake.drain();
    std::lock_guard lock(m_mutex);
    m_update.reset();
    if (m_token)
      wipe(*m_token);
    m_token.reset();
    m_active = false;
  }
  void QrLogin::publish(const QrUpdate& update) {
    {
      std::lock_guard lock(m_mutex);
      m_update = update;
    }
    (void)m_updatesWake.signal();
  }
  std::optional<QrUpdate> QrLogin::poll() {
    m_updatesWake.drain();
    std::lock_guard lock(m_mutex);
    return std::exchange(m_update, {});
  }
  std::optional<std::string> QrLogin::takeToken() {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_token, {});
  }
  void QrLogin::run(std::stop_token stop) {
    const Context context{stop, m_cancelWake};
    try {
      QrSession session;
      Transfer connection;
      const bool local = m_endpoints.gateway.starts_with("ws://127.0.0.1:");
      configure(connection.easy.get(), m_endpoints.gateway, local, context);
      std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
          curl_slist_append(nullptr, originHeader.data()), curl_slist_free_all
      );
      if (!headers)
        throw Failure("QR sign-in could not start. Try browser sign-in.");
      curl_easy_setopt(connection.easy.get(), CURLOPT_HTTPHEADER, headers.get());
      curl_easy_setopt(connection.easy.get(), CURLOPT_CONNECT_ONLY, 2L);
      if (connection.perform(context, session.nextDeadline()) != CURLE_OK)
        throw Failure("QR sign-in could not connect. Try again or use browser sign-in.");
      std::string signal;
      while (!context.cancelled() && !session.terminal()) {
        if (const auto heartbeat = session.tick(Clock::now()))
          send(connection.easy.get(), *heartbeat, context);
        if (session.terminal())
          break;
        for (std::size_t batch = 0; batch < maximumReceiveBatch && !session.terminal(); ++batch) {
          std::array<char, maximumSignalBytes> bytes{};
          std::size_t received = 0;
          const curl_ws_frame* metadata = nullptr;
          const auto result = curl_ws_recv(connection.easy.get(), bytes.data(), bytes.size(), &received, &metadata);
          if (result == CURLE_AGAIN)
            break;
          if (result != CURLE_OK || !metadata || (metadata->flags & CURLWS_CLOSE))
            throw Failure("QR sign-in connection closed. Try again or use browser sign-in.");
          if (metadata->flags & (CURLWS_PING | CURLWS_PONG))
            continue;
          if ((metadata->flags & CURLWS_BINARY) || received > maximumSignalBytes - signal.size())
            throw Failure("QR sign-in received an invalid response. Try browser sign-in.");
          signal.append(bytes.data(), received);
          if (metadata->bytesleft || (metadata->flags & CURLWS_CONT))
            continue;
          const auto message = Json::parse(signal, nullptr, false);
          signal.clear();
          if (const auto reply = session.receive(message, Clock::now()))
            send(connection.easy.get(), *reply, context);
          publish(session.view());
          if (session.view().stage == QrStage::Completing)
            break;
        }
        if (auto ticket = session.takeTicket()) {
          const auto response = exchange(m_endpoints, *ticket, context, session.view().expiresAt);
          wipe(*ticket);
          session.finish(response);
          if (auto token = session.takeToken()) {
            std::lock_guard lock(m_mutex);
            m_token = std::move(token);
          }
          break;
        }
        if (!session.terminal())
          wait(connection.easy.get(), context, POLLIN, session.nextDeadline());
      }
      if (!context.cancelled())
        publish(session.view());
    } catch (const Failure& error) {
      if (!context.cancelled())
        publish({.stage = QrStage::Failed, .error = error.what()});
    } catch (...) {
      if (!context.cancelled())
        publish({.stage = QrStage::Failed, .error = "QR sign-in could not complete. Try browser sign-in."});
    }
    m_active = false;
    (void)m_updatesWake.signal();
  }
} // namespace auth
