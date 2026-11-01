#include "rpc/server.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace rpc {
  namespace {
    using Clock = std::chrono::steady_clock;
    constexpr std::size_t maximumConnections = 32, maximumPending = 128, maximumBuffered = 65536;
    constexpr std::uint32_t handshakeOpcode = 0, frameOpcode = 1, closeOpcode = 2, pingOpcode = 3, pongOpcode = 4;
    constexpr auto handshakeLifetime = std::chrono::seconds(5);
    constexpr auto idlePoll = std::chrono::seconds(1);
    constexpr unsigned socketCount = 10;
    std::string packet(std::uint32_t opcode, const std::string& text) {
      std::string result;
      const auto size = static_cast<std::uint32_t>(text.size());
      for (const auto value : {opcode, size})
        for (unsigned shift = 0; shift < 32; shift += 8)
          result.push_back(static_cast<char>((value >> shift) & 255));
      return result + text;
    }
    std::uint32_t integer(const char* bytes) {
      std::uint32_t result = 0;
      for (unsigned i = 0; i < 4; ++i)
        result |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
      return result;
    }
    bool id(const std::string& value) {
      if (value.empty() || value.size() > 20 || value.front() == '0')
        return false;
      std::uint64_t number = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
      return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && number;
    }
    bool eventName(const std::string& event) {
      return event == "ACTIVITY_JOIN" || event == "ACTIVITY_SPECTATE" || event == "ACTIVITY_JOIN_REQUEST";
    }
    nlohmann::json
    response(const std::string& command, const std::string& nonce, nlohmann::json data, const std::string& error = {}) {
      return {
          {"cmd", command},
          {"nonce", nonce},
          {"evt", error.empty() ? nlohmann::json(nullptr) : "ERROR"},
          {"data", error.empty() ? std::move(data) : nlohmann::json{{"code", 4000}, {"message", error}}}
      };
    }
  } // namespace
  struct Server::Impl {
    struct Peer {
      int fd = -1;
      std::uint64_t identity = 0, revision = 0;
      std::string application, input, output;
      std::set<std::string> subscriptions;
      Clock::time_point created = Clock::now();
      bool ready = false;
    };
    struct Reply {
      std::uint64_t session = 0, connection = 0, revision = 0;
      std::string application, event;
      nlohmann::json value;
    };
    std::filesystem::path directory;
    mutable std::mutex mutex;
    Configuration desired;
    bool resetRequested = false;
    std::vector<Update> events;
    std::deque<Reply> replies;
    std::string path, failure;
    process::WakeEvent wake, changed;
    std::jthread worker;
    explicit Impl(std::filesystem::path root)
        : directory(std::move(root)), worker([this](std::stop_token stop) { run(stop); }) {}
    ~Impl() {
      worker.request_stop();
      (void)wake.signal();
      worker.join();
    }
    bool publish(Update update) {
      std::lock_guard lock(mutex);
      if (update.disconnected)
        std::erase_if(events, [&](const auto& old) { return old.connection == update.connection; });
      if (events.size() >= maximumPending - (update.disconnected ? 0 : maximumConnections))
        return false;
      events.push_back(std::move(update));
      (void)changed.signal();
      return true;
    }
    int listen(std::string& bound) {
      auto root = directory;
      if (root.empty())
        if (const auto* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
          root = runtime;
      struct stat info{};
      if (root.empty()
          || ::lstat(root.c_str(), &info) != 0
          || !S_ISDIR(info.st_mode)
          || info.st_uid != ::getuid()
          || (info.st_mode & 0077))
        return -1;
      for (unsigned slot = 0; slot < socketCount; ++slot) {
        const auto candidate = root / ("discord-ipc-" + std::to_string(slot));
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (candidate.native().size() >= sizeof(address.sun_path))
          continue;
        std::memcpy(address.sun_path, candidate.c_str(), candidate.native().size() + 1);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
          return -1;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
          if (::chmod(candidate.c_str(), 0600) == 0 && ::listen(fd, static_cast<int>(maximumConnections)) == 0) {
            bound = candidate;
            return fd;
          }
          ::unlink(candidate.c_str());
        }
        ::close(fd);
      }
      return -1;
    }
    bool frame(Peer& peer, std::uint32_t opcode, const std::string& body, const Configuration& active) {
      if (opcode == pingOpcode) {
        peer.output += packet(pongOpcode, body);
        return true;
      }
      if (opcode == pongOpcode)
        return true;
      if (opcode == closeOpcode)
        return false;
      auto value = nlohmann::json::parse(body, nullptr, false);
      if (!value.is_object())
        return false;
      if (!peer.ready) {
        if (opcode != handshakeOpcode
            || !value.contains("v")
            || value["v"] != 1
            || !value.contains("client_id")
            || !value["client_id"].is_string())
          return false;
        peer.application = value["client_id"].get<std::string>();
        if (!id(peer.application) || std::ranges::find(active.blocked, peer.application) != active.blocked.end())
          return false;
        peer.ready = true;
        peer.output += packet(
            frameOpcode,
            nlohmann::json{
                {"cmd", "DISPATCH"},
                {"evt", "READY"},
                {"nonce", nullptr},
                {"data", {{"v", 1}, {"user", active.user}, {"config", {{"environment", "production"}}}}}
            }.dump()
        );
        return true;
      }
      if (opcode != frameOpcode
          || !value.contains("cmd")
          || !value["cmd"].is_string()
          || !value.contains("nonce")
          || !value["nonce"].is_string())
        return false;
      const auto command = value["cmd"].get<std::string>(), nonce = value["nonce"].get<std::string>();
      if (nonce.size() > 128 || command.size() > 64)
        return false;
      auto reject = [&](const std::string& message) {
        peer.output += packet(frameOpcode, response(command, nonce, nullptr, message).dump());
        return true;
      };
      if (command == "SUBSCRIBE" || command == "UNSUBSCRIBE") {
        const auto event = value.value("evt", std::string{});
        if (!eventName(event))
          return reject("This event is not available to activity integrations.");
        if (command == "SUBSCRIBE")
          peer.subscriptions.insert(event);
        else
          peer.subscriptions.erase(event);
        peer.output += packet(frameOpcode, response(command, nonce, {{"evt", event}}).dump());
        return true;
      }
      if (!value.contains("args") || !value["args"].is_object())
        return reject("Provide activity command arguments.");
      const auto& args = value["args"];
      Update update{active.session, peer.identity, ++peer.revision, peer.application, command, nonce};
      if (command == "SET_ACTIVITY") {
        if (!args.contains("pid") || !args["pid"].is_number_integer() || args["pid"] <= 0)
          return reject("Provide the application's process ID.");
        if (args.contains("activity") && !args["activity"].is_null()) {
          auto parsed = parseActivity(peer.application, args["activity"]);
          if (!parsed)
            return reject(parsed.error());
          update.activity = std::move(*parsed);
        }
      } else if (command == "SEND_ACTIVITY_JOIN_INVITE" || command == "CLOSE_ACTIVITY_REQUEST") {
        if (!args.contains("user_id") || !args["user_id"].is_string() || !id(args["user_id"].get<std::string>()))
          return reject("Provide a valid recipient.");
        update.user = args["user_id"].get<std::string>();
      } else
        return reject("Only activity commands are available through this connection.");
      if (!publish(std::move(update)))
        return reject("Too many pending activity updates. Try again later.");
      return true;
    }
    void run(std::stop_token stop) {
      Configuration active;
      std::vector<Peer> peers;
      int listener = -1;
      std::string bound;
      std::uint64_t nextIdentity = 0;
      const auto closeAll = [&] {
        for (auto& peer : peers)
          ::close(peer.fd);
        peers.clear();
        if (listener >= 0)
          ::close(std::exchange(listener, -1));
        if (!bound.empty())
          ::unlink(std::exchange(bound, {}).c_str());
      };
      while (!stop.stop_requested()) {
        std::deque<Reply> ready;
        {
          std::lock_guard lock(mutex);
          if (active != desired || resetRequested) {
            const bool reset = std::exchange(resetRequested, false)
                || active.session != desired.session
                || active.enabled != desired.enabled;
            if (reset)
              closeAll();
            active = desired;
            if (reset) {
              events.clear();
              replies.clear();
            }
            failure.clear();
            if (reset && active.enabled && active.session && active.user.is_object()) {
              listener = listen(bound);
              if (listener < 0)
                failure = "Could not open a private local RPC socket. Check the runtime directory and other clients.";
            }
            path = bound;
            (void)changed.signal();
          }
          ready.swap(replies);
        }
        for (const auto& reply : ready)
          if (reply.session == active.session)
            for (auto& peer : peers)
              if ((!reply.connection || peer.identity == reply.connection)
                  && (reply.application.empty() || peer.application == reply.application)
                  && (reply.event.empty() || peer.subscriptions.contains(reply.event)))
                peer.output += packet(frameOpcode, reply.value.dump());
        std::vector<pollfd> descriptors{{wake.fd(), POLLIN, 0}, {listener, POLLIN, 0}};
        for (const auto& peer : peers)
          descriptors.push_back({peer.fd, static_cast<short>(POLLIN | (peer.output.empty() ? 0 : POLLOUT)), 0});
        const int timeout = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(idlePoll).count());
        const int count = ::poll(descriptors.data(), descriptors.size(), timeout);
        if (count < 0 && errno != EINTR)
          break;
        wake.drain();
        for (std::size_t i = peers.size(); i-- > 0;) {
          auto& peer = peers[i];
          const auto flags = descriptors[i + 2].revents;
          bool keep = !(flags & (POLLERR | POLLHUP | POLLNVAL))
              && std::ranges::find(active.blocked, peer.application) == active.blocked.end();
          if (keep && (flags & POLLIN)) {
            std::array<char, maximumActivityBytes> bytes;
            const auto received = ::recv(peer.fd, bytes.data(), bytes.size(), 0);
            if (received > 0)
              peer.input.append(bytes.data(), static_cast<std::size_t>(received));
            else if (!received || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
              keep = false;
            std::size_t consumed = 0;
            while (keep && peer.input.size() - consumed >= 8) {
              const auto opcode = integer(peer.input.data() + consumed),
                         length = integer(peer.input.data() + consumed + 4);
              if (length > maximumActivityBytes) {
                keep = false;
                break;
              }
              if (peer.input.size() - consumed - 8 < length)
                break;
              try {
                keep = frame(peer, opcode, peer.input.substr(consumed + 8, length), active);
              } catch (...) {
                keep = false;
              }
              consumed += 8 + length;
            }
            peer.input.erase(0, consumed);
          }
          if (keep && (flags & POLLOUT) && !peer.output.empty()) {
            const auto sent = ::send(peer.fd, peer.output.data(), peer.output.size(), MSG_NOSIGNAL);
            if (sent > 0)
              peer.output.erase(0, static_cast<std::size_t>(sent));
            else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
              keep = false;
          }
          if (peer.input.size() > maximumBuffered
              || peer.output.size() > maximumBuffered
              || (!peer.ready && Clock::now() - peer.created > handshakeLifetime))
            keep = false;
          if (!keep) {
            if (peer.ready)
              publish({active.session, peer.identity, ++peer.revision, peer.application, {}, {}, {}, {}, true});
            ::close(peer.fd);
            peers.erase(peers.begin() + static_cast<std::ptrdiff_t>(i));
          }
        }
        if (listener >= 0 && (descriptors[1].revents & POLLIN)) {
          const int fd = ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (fd >= 0) {
            ucred credentials{};
            socklen_t size = sizeof(credentials);
            if (peers.size() < maximumConnections
                && ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0
                && credentials.uid == ::getuid())
              peers.push_back({.fd = fd, .identity = ++nextIdentity});
            else
              ::close(fd);
          }
        }
      }
      closeAll();
    }
  };
  Server::Server(std::filesystem::path directory) : m(std::make_unique<Impl>(std::move(directory))) {}
  Server::~Server() = default;
  void Server::configure(Configuration configuration) {
    std::lock_guard lock(m->mutex);
    m->resetRequested =
        m->resetRequested || m->desired.session != configuration.session || m->desired.enabled != configuration.enabled;
    m->desired = std::move(configuration);
    (void)m->wake.signal();
  }
  std::vector<Update> Server::drain() {
    m->changed.drain();
    std::lock_guard lock(m->mutex);
    return std::exchange(m->events, {});
  }
  void Server::complete(const Update& update, const std::optional<Activity>& activity, std::string error) {
    std::lock_guard lock(m->mutex);
    auto value = activity ? activity->fields : nlohmann::json(nullptr);
    if (activity)
      value["application_id"] = activity->application;
    if (m->replies.size() < maximumPending)
      m->replies.push_back(
          {update.session,
           update.connection,
           update.revision,
           {},
           {},
           response(update.command, update.nonce, std::move(value), error)}
      );
    (void)m->wake.signal();
  }
  void Server::dispatch(const std::string& application, const std::string& event, nlohmann::json data) {
    if (!eventName(event))
      return;
    std::lock_guard lock(m->mutex);
    if (m->replies.size() < maximumPending)
      m->replies.push_back(
          {m->desired.session,
           0,
           0,
           application,
           event,
           {{"cmd", "DISPATCH"}, {"evt", event}, {"nonce", nullptr}, {"data", std::move(data)}}}
      );
    (void)m->wake.signal();
  }
  int Server::eventFd() const { return m->changed.fd(); }
  std::string Server::endpoint() const {
    std::lock_guard lock(m->mutex);
    return m->path;
  }
  std::string Server::error() const {
    std::lock_guard lock(m->mutex);
    return m->failure;
  }
} // namespace rpc
