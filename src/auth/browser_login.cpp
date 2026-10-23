#include "auth/browser_login.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <spawn.h>
#include <string_view>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#ifndef CRESSIDA_OPEN_URI
#define CRESSIDA_OPEN_URI "xdg-open"
#endif

extern char** environ;

namespace auth {
  namespace {
    constexpr Logger logger{"browser-login"};
    using namespace std::chrono_literals;
    using Json = nlohmann::json;
    struct Socket {
      int fd = -1;
      ~Socket() {
        if (fd >= 0)
          close(fd);
      }
    };

    bool openBrowser(const std::string& url) {
      posix_spawn_file_actions_t actions;
      if (posix_spawn_file_actions_init(&actions) != 0)
        return false;
      posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
      posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
      posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
      char program[] = CRESSIDA_OPEN_URI;
      char* args[] = {program, const_cast<char*>(url.c_str()), nullptr};
      pid_t pid = -1;
      const auto error = posix_spawnp(&pid, program, &actions, nullptr, args, environ);
      posix_spawn_file_actions_destroy(&actions);
      if (error != 0)
        return false;
      // Do not kill or wait on the user's browser when cancelling sign-in.
      std::thread([pid] {
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
        }
      }).detach();
      return true;
    }

    std::optional<std::string> randomKey() {
      std::array<unsigned char, 32> bytes{};
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR)
          continue;
        if (count <= 0)
          return std::nullopt;
        offset += count;
      }
      std::string key;
      for (auto byte : bytes) {
        key += "0123456789abcdef"[byte >> 4];
        key += "0123456789abcdef"[byte & 15];
      }
      return key;
    }

    std::string encode(std::string_view value) {
      std::string result;
      for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
          result += ch;
        else {
          result += '%';
          result += "0123456789ABCDEF"[ch >> 4];
          result += "0123456789ABCDEF"[ch & 15];
        }
      }
      return result;
    }

    std::optional<std::string> decode(std::string_view value) {
      std::string result;
      for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%') {
          unsigned int byte;
          if (i + 2 >= value.size())
            return std::nullopt;
          const auto [end, error] = std::from_chars(value.data() + i + 1, value.data() + i + 3, byte, 16);
          if (error != std::errc{} || end != value.data() + i + 3 || byte < 32 || byte == 127)
            return std::nullopt;
          result += static_cast<char>(byte);
          i += 2;
        } else
          result += value[i] == '+' ? ' ' : value[i];
      }
      return result;
    }

    std::optional<std::string> parameter(std::string_view query, std::string_view key) {
      std::optional<std::string> result;
      while (!query.empty()) {
        const auto amp = query.find('&');
        const auto item = query.substr(0, amp);
        const auto equals = item.find('=');
        if (item.substr(0, equals) == key) {
          if (result || equals == std::string_view::npos)
            return std::nullopt;
          result = decode(item.substr(equals + 1));
          if (!result)
            return std::nullopt;
        }
        if (amp == std::string_view::npos)
          break;
        query.remove_prefix(amp + 1);
      }
      return result;
    }

    void reply(int fd, int status, std::string_view body = {}, bool redirect = false) {
      std::string response = "HTTP/1.1 "
          + std::to_string(status)
          + (status == 200       ? " OK\r\n"
                 : status == 302 ? " Found\r\n"
                                 : " Bad Request\r\n");
      response += "Connection: close\r\nCache-Control: no-store\r\nContent-Type: application/json\r\n"
                  "Access-Control-Allow-Origin: https://discord.com\r\nVary: Origin\r\n"
                  "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
                  "Access-Control-Allow-Headers: Content-Type\r\nAccess-Control-Allow-Private-Network: true\r\n";
      if (redirect)
        response += "Location: https://discord.com/handoff?done=true\r\n";
      response += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
      response += body;
      std::size_t sent = 0;
      while (sent < response.size()) {
        const auto count = send(fd, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR)
          continue;
        if (count <= 0)
          break;
        sent += count;
      }
    }

    std::optional<BrowserResult> receiveHandoff(int fd, std::uint16_t port, std::stop_token stop) {
      // A browser may issue a CORS/private-network preflight before its POST.
      std::string request;
      std::size_t bodyStart = std::string::npos, bodySize = 0;
      std::string method, target, origin, host;
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        pollfd event{fd, POLLIN, 0};
        if (::poll(&event, 1, 50) <= 0)
          continue;
        char buffer[2048];
        const auto count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
          break;
        request.append(buffer, count);
        if (request.size() > 16384)
          break;
        if (bodyStart == std::string::npos) {
          const auto end = request.find("\r\n\r\n");
          if (end == std::string::npos)
            continue;
          bodyStart = end + 4;
          const auto line = request.find("\r\n");
          const auto first = request.find(' '), last = request.find(' ', first + 1);
          if (first == std::string::npos || last == std::string::npos || last >= line)
            break;
          method = request.substr(0, first);
          target = request.substr(first + 1, last - first - 1);
          if (request.substr(last + 1, line - last - 1) != "HTTP/1.1")
            break;
          std::size_t position = line + 2;
          bool invalid = false, hasLength = false, hasOrigin = false, hasHost = false;
          while (position < end) {
            const auto next = request.find("\r\n", position), colon = request.find(':', position);
            if (colon == std::string::npos || colon >= next) {
              invalid = true;
              break;
            }
            auto name = request.substr(position, colon - position);
            std::ranges::transform(name, name.begin(), [](unsigned char ch) { return std::tolower(ch); });
            auto value = std::string_view(request).substr(colon + 1, next - colon - 1);
            while (!value.empty() && value.front() == ' ')
              value.remove_prefix(1);
            while (!value.empty() && value.back() == ' ')
              value.remove_suffix(1);
            if (name == "host") {
              if (hasHost)
                invalid = true;
              host = value;
              hasHost = true;
            }
            if (name == "origin") {
              if (hasOrigin)
                invalid = true;
              origin = value;
              hasOrigin = true;
            }
            if (name == "transfer-encoding")
              invalid = true;
            if (name == "content-length") {
              const auto [finish, error] = std::from_chars(value.data(), value.data() + value.size(), bodySize);
              if (hasLength || error != std::errc{} || finish != value.data() + value.size() || bodySize > 8192)
                invalid = true;
              hasLength = true;
            }
            position = next + 2;
          }
          if (invalid
              || host != "127.0.0.1:" + std::to_string(port)
              || (!origin.empty() && origin != "https://discord.com")
              || ((method == "POST" || method == "OPTIONS") && origin.empty()))
            break;
          if (target.substr(0, target.find('?')) != "/rpc")
            break;
          if (method == "OPTIONS") {
            reply(fd, 200);
            return std::nullopt;
          }
          if (method != "GET" && method != "POST")
            break;
          if (method == "POST" && !hasLength)
            break;
        }
        if (request.size() < bodyStart + bodySize)
          continue;
        const auto queryStart = target.find('?');
        const std::string_view query =
            queryStart == std::string::npos ? std::string_view{} : std::string_view(target).substr(queryStart + 1);
        std::optional<std::string> payload;
        if (method == "GET") {
          if (parameter(query, "callback") != "https://discord.com/handoff?done=true")
            break;
          payload = parameter(query, "payload");
        } else
          payload = request.substr(bodyStart, bodySize);
        if (!payload)
          break;
        const auto value = Json::parse(*payload, nullptr, false);
        if (!value.is_object()
            || value.value("cmd", Json{}) != "BROWSER_HANDOFF"
            || !value.contains("args")
            || !value["args"].is_object())
          break;
        const auto& args = value["args"];
        BrowserResult result;
        if (!args.contains("handoffToken") || args["handoffToken"].is_null()) {
          result.error = "Sign in to Discord in your browser, then try again.";
        } else if (args["handoffToken"].is_string()) {
          result.handoffToken = args["handoffToken"].get<std::string>();
          if (result.handoffToken.empty()
              || result.handoffToken.size() > 4096
              || !std::ranges::all_of(result.handoffToken, [](unsigned char ch) { return ch >= 33 && ch <= 126; }))
            break;
        } else
          break;
        const auto nonce = value.value("nonce", Json{});
        if (!nonce.is_string() || nonce.get_ref<const std::string&>().size() > 128)
          break;
        const auto body =
            Json{{"cmd", "BROWSER_HANDOFF"}, {"data", nullptr}, {"evt", nullptr}, {"nonce", nonce}}.dump();
        reply(fd, method == "GET" ? 302 : 200, body, method == "GET");
        return result;
      }
      if (!stop.stop_requested())
        reply(fd, 400);
      return std::nullopt;
    }
  } // namespace

  bool openExternalUrl(const std::string& url) {
    if ((!url.starts_with("https://") && !url.starts_with("http://"))
        || url.find_first_of("\r\n\t ") != std::string::npos)
      return false;
    return openBrowser(url);
  }

  BrowserLogin::BrowserLogin(Opener opener) : m_opener(opener ? std::move(opener) : openBrowser) {}
  BrowserLogin::~BrowserLogin() { cancel(); }

  bool BrowserLogin::start() {
    cancel();
    logger.debug("sign-in starting");
    const auto fail = [](const char* stage) {
      logger.warn("sign-in setup failed stage={}", stage);
      return false;
    };
    auto key = randomKey();
    if (!key)
      return fail("randomness");
    Socket listener{socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (listener.fd < 0)
      return fail("listener-create");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
      return fail("listener-bind");
    if (listen(listener.fd, 8) != 0)
      return fail("listener-listen");
    socklen_t length = sizeof(address);
    if (getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &length) != 0)
      return fail("listener-address");
    const auto port = ntohs(address.sin_port);
    const std::string handoff = "/handoff?rpc=" + std::to_string(port) + "&key=" + *key;
    // Login handles passwords, MFA and CAPTCHA on Discord's own origin, then returns to handoff.
    // RPC's browser module reads its port when loaded, before the SPA redirects.
    const std::string url = "https://discord.com/login?redirect_to=" + encode(handoff) + "&rpc=" + std::to_string(port);
    if (!m_opener(url))
      return fail("browser-open");
    logger.info("waiting for browser sign-in");
    m_active = true;
    m_worker = std::jthread([this, fd = listener.fd, port, key = std::move(*key)](std::stop_token stop) mutable {
      run(stop, fd, port, std::move(key));
    });
    listener.fd = -1;
    return true;
  }

  void BrowserLogin::run(std::stop_token stop, int descriptor, std::uint16_t port, std::string key) {
    Socket listener{descriptor};
    const auto deadline = std::chrono::steady_clock::now() + 5min;
    BrowserResult result;
    try {
      while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        pollfd event{listener.fd, POLLIN, 0};
        if (::poll(&event, 1, 50) <= 0)
          continue;
        Socket connection{accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
        if (connection.fd < 0)
          continue;
        if (auto handoff = receiveHandoff(connection.fd, port, stop)) {
          result = std::move(*handoff);
          result.key = std::move(key);
          break;
        }
      }
      if (result.handoffToken.empty() && result.error.empty()) {
        result.error = "Browser sign-in timed out. Please try again.";
        if (!stop.stop_requested())
          logger.warn("sign-in failed stage=handoff-timeout");
      } else if (!stop.stop_requested())
        logger.debug("browser handoff received outcome={}", result.error.empty() ? "ready" : "sign-in-required");
    } catch (...) {
      result = {{}, {}, "Could not complete browser sign-in. Please try again."};
      if (!stop.stop_requested())
        logger.warn("sign-in failed stage=handoff-processing");
    }
    if (stop.stop_requested())
      logger.debug("sign-in cancelled");
    if (!stop.stop_requested()) {
      std::lock_guard lock(m_mutex);
      m_result = std::move(result);
    }
    m_active = false;
  }

  void BrowserLogin::cancel() {
    if (m_worker.joinable()) {
      m_worker.request_stop();
      m_worker.join();
    }
    m_active = false;
    std::lock_guard lock(m_mutex);
    m_result.reset();
  }

  std::optional<BrowserResult> BrowserLogin::poll() {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_result, {});
  }
} // namespace auth
