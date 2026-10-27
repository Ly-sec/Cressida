#include "auth/qr_login.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
  using Json = nlohmann::json;
  using Clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  constexpr std::size_t maximumRequest = 32 * 1024;
  constexpr std::string_view syntheticToken = "synthetic-approved-token";
  constexpr std::string_view syntheticTicket = "synthetic-one-use-ticket";
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  std::string base64(std::span<const unsigned char> input, bool url = false) {
    std::string value((input.size() + 2) / 3 * 4, '\0');
    const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(value.data()), input.data(), input.size());
    require(size >= 0, "Could not encode synthetic bytes");
    value.resize(size);
    if (url) {
      std::replace(value.begin(), value.end(), '+', '-');
      std::replace(value.begin(), value.end(), '/', '_');
      while (value.ends_with('='))
        value.pop_back();
    }
    return value;
  }
  class PublicKey {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> m_key{nullptr, EVP_PKEY_free};
    std::vector<unsigned char> m_der;

  public:
    explicit PublicKey(const Json& init) {
      require(init.value("op", "") == "init", "Hello produces the RSA public-key initialization");
      const auto encoded = init.at("encoded_public_key").get<std::string>();
      require(encoded.size() % 4 == 0, "The session public key uses standard base64 DER");
      m_der.resize(encoded.size() / 4 * 3);
      auto length =
          EVP_DecodeBlock(m_der.data(), reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
      require(length > 0, "Could not decode the session public key");
      if (encoded.ends_with('='))
        --length;
      if (encoded.ends_with("=="))
        --length;
      m_der.resize(length);
      const auto* bytes = m_der.data();
      m_key.reset(d2i_PUBKEY(nullptr, &bytes, m_der.size()));
      require(
          m_key && bytes == m_der.data() + m_der.size() && EVP_PKEY_bits(m_key.get()) == 2048,
          "Each attempt advertises a valid 2048-bit SubjectPublicKeyInfo key"
      );
    }
    std::string fingerprint() const {
      std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
      unsigned length = 0;
      require(
          EVP_Digest(m_der.data(), m_der.size(), bytes.data(), &length, EVP_sha256(), nullptr) == 1,
          "Could not hash the session public key"
      );
      return base64(std::span(bytes).first(length), true);
    }
    std::string encrypt(std::string_view plaintext) const {
      std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
          EVP_PKEY_CTX_new(m_key.get(), nullptr), EVP_PKEY_CTX_free
      );
      require(
          context
              && EVP_PKEY_encrypt_init(context.get()) > 0
              && EVP_PKEY_CTX_set_rsa_padding(context.get(), RSA_PKCS1_OAEP_PADDING) > 0
              && EVP_PKEY_CTX_set_rsa_oaep_md(context.get(), EVP_sha256()) > 0
              && EVP_PKEY_CTX_set_rsa_mgf1_md(context.get(), EVP_sha256()) > 0,
          "Could not initialize synthetic OAEP encryption"
      );
      std::size_t size = 0;
      require(
          EVP_PKEY_encrypt(
              context.get(), nullptr, &size, reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size()
          ) > 0,
          "Could not size synthetic encrypted data"
      );
      std::vector<unsigned char> encrypted(size);
      require(
          EVP_PKEY_encrypt(
              context.get(), encrypted.data(), &size, reinterpret_cast<const unsigned char*>(plaintext.data()),
              plaintext.size()
          ) > 0,
          "Could not encrypt synthetic data"
      );
      encrypted.resize(size);
      return base64(encrypted);
    }
  };
  Json hello(int heartbeat = 1000, int timeout = 120000) {
    return {{"op", "hello"}, {"heartbeat_interval", heartbeat}, {"timeout_ms", timeout}};
  }
  PublicKey initialize(auth::QrSession& session, Clock::time_point now) {
    const auto init = session.receive(hello(), now);
    require(init.has_value(), "A fresh session accepts exactly one hello");
    return PublicKey(*init);
  }
  void prove(auth::QrSession& session, const PublicKey& key, Clock::time_point now) {
    const std::string nonce("\0\1\2\3\xFB\xFF", 6);
    const auto proof = session.receive({{"op", "nonce_proof"}, {"encrypted_nonce", key.encrypt(nonce)}}, now);
    require(
        proof && *proof == Json{{"op", "nonce_proof"}, {"nonce", "AAECA_v_"}},
        "Nonce proof returns the raw decrypted bytes as unpadded URL-safe base64"
    );
    session.receive({{"op", "pending_remote_init"}, {"fingerprint", key.fingerprint()}}, now);
    require(
        session.view().stage == auth::QrStage::Ready && session.view().qrUrl.ends_with(key.fingerprint()),
        "Only the SHA256 fingerprint of this attempt's public key becomes a QR payload"
    );
  }
  void approve(auth::QrSession& session, const PublicKey& key, Clock::time_point now) {
    session.receive(
        {{"op", "pending_ticket"}, {"encrypted_user_payload", key.encrypt("123:0:None:Alex: Example")}}, now
    );
    require(
        session.view().stage == auth::QrStage::AwaitingApproval
            && session.view().account
            && session.view().account->id == "123"
            && session.view().account->name == "Alex: Example"
            && session.view().account->avatar.empty()
            && !session.takeToken(),
        "Scanning projects a literal bounded account preview while still requiring phone approval"
    );
    session.receive({{"op", "pending_login"}, {"ticket", syntheticTicket}}, now);
    require(
        session.view().stage == auth::QrStage::Completing && session.view().qrUrl.empty(),
        "Phone approval retires the displayed QR payload before the ticket exchange"
    );
  }
  void protocolTests() {
    const auto now = Clock::time_point(100s);
    auth::QrSession session(now);
    const auto key = initialize(session, now);
    prove(session, key, now);
    approve(session, key, now);
    const auto ticket = session.takeTicket();
    require(ticket && *ticket == syntheticTicket && !session.takeTicket(), "Approval tickets are consumed once");
    session.finish({{"encrypted_token", key.encrypt(syntheticToken)}});
    require(
        session.view().stage == auth::QrStage::Authenticated
            && session.view().qrUrl.empty()
            && session.view().error.empty(),
        "Encrypted approval produces only a sanitized completion update"
    );
    const auto token = session.takeToken();
    require(
        token && *token == syntheticToken && !session.takeToken(),
        "The account-connection path consumes credentials once"
    );
    session.receive({{"op", "cancel"}}, now);
    require(
        session.view().stage == auth::QrStage::Authenticated && !session.takeToken(),
        "A completed session ignores later peer messages without replaying its credential"
    );

    auth::QrSession next(now);
    const auto nextKey = initialize(next, now);
    require(nextKey.fingerprint() != key.fingerprint(), "Every new attempt uses a fresh ephemeral key");
    next.receive({{"op", "nonce_proof"}, {"encrypted_nonce", key.encrypt("challenge")}}, now);
    require(
        next.view().stage == auth::QrStage::Failed && next.view().qrUrl.empty(),
        "Encrypted callbacks from another attempt cannot advance this attempt"
    );

    for (const auto& invalid :
         {Json::array(), Json{{"op", "pending_login"}, {"ticket", syntheticTicket}},
          Json{{"op", "hello"}, {"heartbeat_interval", -1}, {"timeout_ms", 120000}},
          Json{{"op", "hello"}, {"heartbeat_interval", 1}, {"timeout_ms", 120000}},
          Json{{"op", "hello"}, {"heartbeat_interval", 1000}, {"timeout_ms", 0}},
          Json{{"op", "hello"}, {"heartbeat_interval", 1000.5}, {"timeout_ms", 120000}}}) {
      auth::QrSession malformed(now);
      malformed.receive(invalid, now);
      require(
          malformed.view().stage == auth::QrStage::Failed && !malformed.takeTicket() && !malformed.takeToken(),
          "Malformed or out-of-order protocol input cannot produce a ticket or credential"
      );
    }
    {
      auth::QrSession mismatch(now);
      const auto matchKey = initialize(mismatch, now);
      mismatch.receive({{"op", "nonce_proof"}, {"encrypted_nonce", matchKey.encrypt("nonce")}}, now);
      mismatch.receive({{"op", "pending_remote_init"}, {"fingerprint", std::string(43, 'A')}}, now);
      require(
          mismatch.view().stage == auth::QrStage::Failed && mismatch.view().qrUrl.empty(),
          "A mismatched server fingerprint is never rendered as a QR code"
      );
    }
    for (const auto& payload :
         {"0:0:None:Alex", "123:0:https://example.com/avatar:Alex", "123:0:None:Bad\nName",
          "123:0:None:", "123:0:None:\xFF"}) {
      auth::QrSession malformed(now);
      const auto current = initialize(malformed, now);
      prove(malformed, current, now);
      malformed.receive({{"op", "pending_ticket"}, {"encrypted_user_payload", current.encrypt(payload)}}, now);
      require(
          malformed.view().stage == auth::QrStage::Failed && !malformed.view().account,
          "Malformed identities, avatar routes, control characters and invalid text never reach the account preview"
      );
    }
    {
      auth::QrSession noApproval(now);
      const auto current = initialize(noApproval, now);
      prove(noApproval, current, now);
      noApproval.receive({{"op", "pending_login"}, {"ticket", syntheticTicket}}, now);
      require(
          noApproval.view().stage == auth::QrStage::Failed && !noApproval.takeTicket(),
          "A ticket cannot bypass the scanned-account approval step"
      );
    }
    {
      auth::QrSession defaultAvatar(now);
      const auto current = initialize(defaultAvatar, now);
      prove(defaultAvatar, current, now);
      defaultAvatar.receive(
          {{"op", "pending_ticket"}, {"encrypted_user_payload", current.encrypt("123:0:0:Alex")}}, now
      );
      require(
          defaultAvatar.view().stage == auth::QrStage::AwaitingApproval
              && defaultAvatar.view().account
              && defaultAvatar.view().account->id == "123"
              && defaultAvatar.view().account->name == "Alex"
              && defaultAvatar.view().account->avatar.empty()
              && !defaultAvatar.takeToken(),
          "The encrypted zero-avatar sentinel projects a default avatar while still requiring phone approval"
      );
    }
    {
      auth::QrSession heartbeats(now);
      initialize(heartbeats, now);
      require(!heartbeats.tick(now + 999ms), "Heartbeat pacing does not send early");
      require(heartbeats.tick(now + 1000ms) == Json{{"op", "heartbeat"}}, "Hello controls heartbeat pacing");
      heartbeats.receive({{"op", "heartbeat_ack"}}, now + 1001ms);
      require(
          heartbeats.tick(now + 2000ms) == Json{{"op", "heartbeat"}}, "Acknowledged connections continue heartbeating"
      );
      heartbeats.tick(now + 3000ms);
      require(heartbeats.view().stage == auth::QrStage::Failed, "A missing heartbeat acknowledgement ends the attempt");
    }
    {
      auth::QrSession expired(now);
      const auto current = initialize(expired, now);
      prove(expired, current, now);
      expired.tick(now + 120s);
      require(
          expired.view().stage == auth::QrStage::Expired && expired.view().qrUrl.empty(),
          "The server deadline retires expired QR payloads without automatic retries"
      );
      expired.receive(
          {{"op", "pending_ticket"}, {"encrypted_user_payload", current.encrypt("123:0:None:Late")}}, now + 121s
      );
      require(!expired.view().account && !expired.takeToken(), "Expired attempts reject late scanned-account data");
    }
    {
      auth::QrSession setup(now);
      setup.tick(now + 21s);
      require(setup.view().stage == auth::QrStage::Failed, "Connection setup has a finite independent deadline");
    }
    {
      auth::QrSession cancelled(now);
      const auto current = initialize(cancelled, now);
      prove(cancelled, current, now);
      cancelled.receive({{"op", "cancel"}}, now);
      require(
          cancelled.view().stage == auth::QrStage::Cancelled && cancelled.view().qrUrl.empty(),
          "Phone cancellation retires the QR payload and requires a new attempt"
      );
    }
  }

  class Socket {
    int m_fd = -1;

  public:
    explicit Socket(int fd = -1) : m_fd(fd) {}
    ~Socket() {
      if (m_fd >= 0)
        ::close(m_fd);
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    int fd() const { return m_fd; }
  };
  void sendBytes(int fd, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto sent = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
      require(sent > 0, "Could not send a private peer fixture");
      offset += sent;
    }
  }
  std::string readBytes(int fd, std::size_t count) {
    require(count <= maximumRequest, "Private fixture input stays bounded");
    std::string bytes(count, '\0');
    std::size_t offset = 0;
    while (offset < count) {
      const auto received = ::recv(fd, bytes.data() + offset, count - offset, 0);
      require(received > 0, "The private peer did not receive its expected bytes");
      offset += received;
    }
    return bytes;
  }
  std::string httpHeaders(int fd) {
    std::string headers;
    while (!headers.ends_with("\r\n\r\n")) {
      require(headers.size() < maximumRequest, "Private HTTP headers stay bounded");
      headers += readBytes(fd, 1);
    }
    return headers;
  }
  std::string header(std::string_view headers, std::string_view name) {
    const auto start = headers.find(std::string("\r\n") + std::string(name) + ": ");
    require(start != std::string_view::npos, "A required private fixture header is present");
    const auto first = start + name.size() + 4;
    return std::string(headers.substr(first, headers.find("\r\n", first) - first));
  }
  void websocket(int fd, const Json& value, bool binary = false) {
    const auto payload = value.dump();
    std::string frame;
    frame += static_cast<char>(binary ? 0x82 : 0x81);
    if (payload.size() < 126)
      frame += static_cast<char>(payload.size());
    else {
      frame += static_cast<char>(126);
      frame += static_cast<char>(payload.size() >> 8);
      frame += static_cast<char>(payload.size());
    }
    frame += payload;
    sendBytes(fd, frame);
  }
  Json websocket(int fd) {
    const auto initial = readBytes(fd, 2);
    require(
        (static_cast<unsigned char>(initial[0]) & 0x0F) == 1 && (static_cast<unsigned char>(initial[1]) & 0x80),
        "The client uses masked text WebSocket frames"
    );
    std::size_t length = static_cast<unsigned char>(initial[1]) & 0x7F;
    if (length == 126) {
      const auto extra = readBytes(fd, 2);
      length = static_cast<unsigned char>(extra[0]) * 256 + static_cast<unsigned char>(extra[1]);
    }
    require(length != 127 && length <= maximumRequest, "Private client frames stay bounded");
    const auto mask = readBytes(fd, 4);
    auto payload = readBytes(fd, length);
    for (std::size_t i = 0; i < payload.size(); ++i)
      payload[i] ^= mask[i % mask.size()];
    return Json::parse(payload);
  }
  class Peer {
  public:
    enum class Mode { Complete, Silent, Binary, Redirect, SecurityCheck, Expire };
    explicit Peer(Mode mode) : m_mode(mode), m_listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
      require(m_listener.fd() >= 0, "Could not create a private peer listener");
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      require(
          ::bind(m_listener.fd(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0
              && ::listen(m_listener.fd(), 4) == 0,
          "Could not bind a private peer listener"
      );
      socklen_t length = sizeof(address);
      require(
          ::getsockname(m_listener.fd(), reinterpret_cast<sockaddr*>(&address), &length) == 0,
          "Could not inspect the private listener address"
      );
      m_port = ntohs(address.sin_port);
      m_worker = std::jthread([this](std::stop_token stop) {
        try {
          run(stop);
        } catch (const std::exception& error) {
          std::lock_guard lock(m_mutex);
          m_error = error.what();
        }
      });
    }
    ~Peer() {
      m_worker.request_stop();
      if (m_worker.joinable())
        m_worker.join();
    }
    auth::QrEndpoints endpoints() const {
      return {
          "ws://127.0.0.1:" + std::to_string(m_port) + "/?v=2",
          "http://127.0.0.1:" + std::to_string(m_port) + "/api/v9/users/@me/remote-auth/login"
      };
    }
    void awaitHandshake() {
      std::unique_lock lock(m_mutex);
      require(m_changed.wait_for(lock, 3s, [&] { return m_handshaken; }), "The private WebSocket handshake completes");
    }
    void check() {
      if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
      }
      std::lock_guard lock(m_mutex);
      require(m_error.empty(), m_error.empty() ? "Private peer completed" : m_error.c_str());
    }

  private:
    Socket accept(std::stop_token stop) {
      while (!stop.stop_requested()) {
        pollfd waiting{m_listener.fd(), POLLIN, 0};
        if (::poll(&waiting, 1, 50) <= 0)
          continue;
        const auto fd = ::accept4(m_listener.fd(), nullptr, nullptr, SOCK_CLOEXEC);
        require(fd >= 0, "Could not accept the private peer");
        timeval timeout{3, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        return Socket(fd);
      }
      return Socket();
    }
    void run(std::stop_token stop) {
      auto ws = accept(stop);
      if (ws.fd() < 0)
        return;
      const auto request = httpHeaders(ws.fd());
      require(
          request.starts_with("GET /?v=2 HTTP/1.1\r\n")
              && request.contains("\r\nOrigin: https://discord.com\r\n")
              && !request.contains("Authorization:"),
          "QR setup uses only its fixed route and public origin"
      );
      const auto challenge = header(request, "Sec-WebSocket-Key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
      std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
      unsigned digestLength = 0;
      require(
          EVP_Digest(challenge.data(), challenge.size(), digest.data(), &digestLength, EVP_sha1(), nullptr) == 1,
          "Could not create the private WebSocket acceptance"
      );
      sendBytes(
          ws.fd(),
          "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
              + base64(std::span(digest).first(digestLength))
              + "\r\n\r\n"
      );
      {
        std::lock_guard lock(m_mutex);
        m_handshaken = true;
        m_changed.notify_all();
      }
      if (m_mode == Mode::Silent) {
        while (!stop.stop_requested()) {
          pollfd waiting{ws.fd(), POLLIN, 0};
          if (::poll(&waiting, 1, 50) > 0)
            return;
        }
        return;
      }
      if (m_mode == Mode::Binary) {
        websocket(ws.fd(), hello(), true);
        return;
      }
      websocket(ws.fd(), hello(1000, m_mode == Mode::Expire ? 250 : 120000));
      const PublicKey key(websocket(ws.fd()));
      websocket(ws.fd(), {{"op", "nonce_proof"}, {"encrypted_nonce", key.encrypt("nonce")}});
      require(
          websocket(ws.fd()) == Json{{"op", "nonce_proof"}, {"nonce", "bm9uY2U"}},
          "The real worker sends the raw OAEP nonce proof"
      );
      websocket(ws.fd(), {{"op", "pending_remote_init"}, {"fingerprint", key.fingerprint()}});
      if (m_mode == Mode::Expire) {
        while (!stop.stop_requested()) {
          pollfd waiting{ws.fd(), POLLIN, 0};
          if (::poll(&waiting, 1, 50) > 0)
            return;
        }
        return;
      }
      websocket(ws.fd(), {{"op", "pending_ticket"}, {"encrypted_user_payload", key.encrypt("123:0:0:Alex: Example")}});
      websocket(ws.fd(), {{"op", "pending_login"}, {"ticket", syntheticTicket}});
      auto rest = accept(stop);
      if (rest.fd() < 0)
        return;
      const auto headers = httpHeaders(rest.fd());
      require(
          headers.starts_with("POST /api/v9/users/@me/remote-auth/login HTTP/1.1\r\n")
              && !headers.contains("Authorization:")
              && headers.contains("Content-Type: application/json"),
          "The approved ticket exchange stays unauthenticated on its fixed route"
      );
      const auto size = std::stoul(header(headers, "Content-Length"));
      require(
          Json::parse(readBytes(rest.fd(), size)) == Json{{"ticket", syntheticTicket}},
          "The one-use ticket is the entire exchange body"
      );
      if (m_mode == Mode::Redirect) {
        sendBytes(
            rest.fd(),
            "HTTP/1.1 302 Found\r\nLocation: https://example.invalid/credential\r\nContent-Length: 0\r\nConnection: "
            "close\r\n\r\n"
        );
      } else {
        const auto body = m_mode == Mode::SecurityCheck
            ? Json{{"captcha_key", Json::array({"security-check"})}, {"captcha_service", "hcaptcha"}}.dump()
            : Json{{"encrypted_token", key.encrypt(syntheticToken)}}.dump();
        sendBytes(
            rest.fd(),
            "HTTP/1.1 "
                + std::string(m_mode == Mode::SecurityCheck ? "400 Bad Request" : "200 OK")
                + "\r\nContent-Type: application/json\r\nContent-Length: "
                + std::to_string(body.size())
                + "\r\nConnection: close\r\n\r\n"
                + body
        );
      }
    }
    Mode m_mode;
    Socket m_listener;
    std::uint16_t m_port = 0;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_handshaken = false;
    std::string m_error;
    std::jthread m_worker;
  };
  auth::QrUpdate awaitTerminal(auth::QrLogin& login) {
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline) {
      if (auto update = login.poll()) {
        if (update->stage == auth::QrStage::Authenticated
            || update->stage == auth::QrStage::Failed
            || update->stage == auth::QrStage::Expired
            || update->stage == auth::QrStage::Cancelled)
          return *update;
      }
      pollfd waiting{login.fd(), POLLIN, 0};
      ::poll(&waiting, 1, 100);
    }
    throw std::runtime_error("The real QR worker did not finish before its private test deadline");
  }
  void workerTests() {
    for (const auto& endpoints :
         {auth::QrEndpoints{"ws://example.invalid/?v=2", "https://example.invalid/login"},
          auth::QrEndpoints{"ws://127.0.0.1:1/?v=2", "http://127.0.0.1:1/other"}}) {
      auth::QrLogin rejected(endpoints);
      require(
          !rejected.start() && !rejected.active() && !rejected.takeToken(),
          "Only production routes or explicitly injected numeric-loopback fixtures can start"
      );
    }
    {
      Peer peer(Peer::Mode::Complete);
      auth::QrLogin login(peer.endpoints());
      require(login.fd() >= 0 && login.start(), "The real QR worker exposes its pollable update descriptor");
      const auto update = awaitTerminal(login);
      if (update.stage != auth::QrStage::Authenticated)
        throw std::runtime_error("Private QR worker failure: " + update.error);
      require(
          update.stage == auth::QrStage::Authenticated
              && update.account
              && update.account->name == "Alex: Example"
              && update.qrUrl.empty()
              && update.error.empty(),
          "A private peer completes actual WebSocket and REST approval"
      );
      const auto token = login.takeToken();
      require(token && *token == syntheticToken && !login.takeToken(), "Actual worker credentials are consumed once");
      peer.check();
      login.cancel();
      require(
          !login.active() && !login.poll() && !login.takeToken(),
          "Cancellation clears queued updates and consumed credentials"
      );
    }
    {
      Peer peer(Peer::Mode::Silent);
      auth::QrLogin login(peer.endpoints());
      require(login.start(), "The silent private peer starts a cancellable attempt");
      peer.awaitHandshake();
      const auto before = Clock::now();
      login.cancel();
      require(
          Clock::now() - before < 500ms && !login.active() && !login.poll() && !login.takeToken(),
          "Cancellation wakes idle network waits and discards pending work promptly"
      );
      peer.check();
    }
    for (const auto mode : {Peer::Mode::Binary, Peer::Mode::Redirect, Peer::Mode::SecurityCheck, Peer::Mode::Expire}) {
      Peer peer(mode);
      auth::QrLogin login(peer.endpoints());
      require(login.start(), "The private failure fixture starts");
      const auto update = awaitTerminal(login);
      require(
          update.stage == (mode == Peer::Mode::Expire ? auth::QrStage::Expired : auth::QrStage::Failed)
              && update.qrUrl.empty()
              && !login.takeToken(),
          "Binary messages, redirects, security checks and expiry never produce credentials"
      );
      if (mode == Peer::Mode::SecurityCheck)
        require(update.error.contains("security check"), "Additional verification directs the user to browser sign-in");
      peer.check();
    }
  }
} // namespace

int main() {
  try {
    protocolTests();
    workerTests();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
