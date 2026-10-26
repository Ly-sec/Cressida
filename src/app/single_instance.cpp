#include "app/single_instance.h"

#include "app/desktop_activation.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace app {
  namespace {
    constexpr char socketName[] = "instance.sock", lockName[] = "instance.lock";
    constexpr std::string_view command = "Cressida.Open/1\n";
    constexpr std::string_view voiceCommand = "Cressida.Voice/1\n", voiceReply = "Cressida.VoiceReply/1\n";
    constexpr std::size_t nonceBytes = 16;
    constexpr std::size_t nonceCharacters = nonceBytes * 2;
    constexpr std::size_t maximumVoiceMessage = 256;
    constexpr std::size_t maximumVoiceCommandName = 32;
    constexpr std::size_t maximumVoicePacket = voiceCommand.size()
        + nonceCharacters
        + std::numeric_limits<std::uint64_t>::digits10
        + maximumVoiceCommandName
        + 4;
    constexpr std::size_t maximumVoiceReply = voiceReply.size() + nonceCharacters + maximumVoiceMessage + 8;
    constexpr std::size_t maximumPendingVoice = 32;
    constexpr std::size_t maximumRecentVoice = 128;
    constexpr auto launchDeadline = std::chrono::seconds(5);
    constexpr auto launchRetry = std::chrono::milliseconds(20);
    constexpr std::size_t maximumDatagram =
        command.size() + std::numeric_limits<std::uint64_t>::digits10 + 2 + DesktopActivationToken::maximumBytes;
    constexpr unsigned maximumBatch = 32;
    using Clock = std::chrono::steady_clock;
    std::uint64_t milliseconds() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    }
    [[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
    bool privateDirectory(int fd) {
      struct stat info{};
      return fd >= 0
          && ::fstat(fd, &info) == 0
          && S_ISDIR(info.st_mode)
          && info.st_uid == ::getuid()
          && (info.st_mode & (S_IRWXG | S_IRWXO)) == 0;
    }
    int openDirectory(const std::filesystem::path& path) {
      const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (!privateDirectory(fd)) {
        if (fd >= 0)
          ::close(fd);
        fail("Single-instance startup requires a private, user-owned runtime directory.");
      }
      return fd;
    }
    struct Descriptor {
      int fd = -1;
      explicit Descriptor(int value = -1) : fd(value) {}
      ~Descriptor() {
        if (fd >= 0)
          ::close(fd);
      }
      Descriptor(const Descriptor&) = delete;
      Descriptor& operator=(const Descriptor&) = delete;
    };
    struct Datagram {
      ssize_t size = -1;
      sockaddr_un sender{};
      socklen_t senderSize = sizeof(sender);
      std::optional<ucred> credentials;
      bool truncated = false;
    };
    Datagram receive(int socket, char* bytes, std::size_t capacity) {
      Datagram result;
      iovec data{bytes, capacity};
      alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(ucred))> control{};
      msghdr message{};
      message.msg_name = &result.sender;
      message.msg_namelen = sizeof(result.sender);
      message.msg_iov = &data;
      message.msg_iovlen = 1;
      message.msg_control = control.data();
      message.msg_controllen = control.size();
      result.size = ::recvmsg(socket, &message, MSG_DONTWAIT | MSG_TRUNC);
      result.senderSize = message.msg_namelen;
      result.truncated = (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
      if (result.size < 0)
        return result;
      for (const auto* entry = CMSG_FIRSTHDR(&message); entry;
           entry = CMSG_NXTHDR(&message, const_cast<cmsghdr*>(entry)))
        if (entry->cmsg_level == SOL_SOCKET
            && entry->cmsg_type == SCM_CREDENTIALS
            && entry->cmsg_len == CMSG_LEN(sizeof(ucred))) {
          ucred credentials{};
          std::memcpy(&credentials, CMSG_DATA(entry), sizeof(credentials));
          result.credentials = credentials;
        }
      return result;
    }
    bool sameUser(const Datagram& datagram) {
      return datagram.credentials && datagram.credentials->uid == ::getuid() && datagram.credentials->pid > 0;
    }
    sockaddr_un addressFor(int directory, socklen_t& length) {
      const auto path = "/proc/self/fd/" + std::to_string(directory) + "/" + socketName;
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      path.copy(address.sun_path, sizeof(address.sun_path) - 1);
      length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
      return address;
    }
    bool validMessage(std::string_view message) {
      return message.size() <= maximumVoiceMessage
          && std::ranges::none_of(message, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
    }
    bool validNonce(std::string_view nonce) {
      return nonce.size() == nonceCharacters && std::ranges::all_of(nonce, [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
             });
    }
    std::optional<std::string> newNonce() {
      std::array<unsigned char, nonceBytes> bytes{};
      std::size_t read = 0;
      while (read < bytes.size()) {
        const auto count = ::getrandom(bytes.data() + read, bytes.size() - read, GRND_NONBLOCK);
        if (count < 0 && errno == EINTR)
          continue;
        if (count <= 0)
          return {};
        read += count;
      }
      constexpr std::string_view digits = "0123456789abcdef";
      std::string result;
      result.reserve(nonceCharacters);
      for (const auto byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 0xf];
      }
      return result;
    }
    struct VoicePacket {
      VoiceCommand command;
      std::string nonce;
      std::uint64_t expires;
    };
    std::optional<VoicePacket> parseVoice(std::string_view packet) {
      if (!packet.starts_with(voiceCommand) || packet.size() > maximumVoicePacket)
        return {};
      packet.remove_prefix(voiceCommand.size());
      const auto nonceEnd = packet.find('\n');
      if (nonceEnd == std::string_view::npos || !validNonce(packet.substr(0, nonceEnd)))
        return {};
      const std::string nonce(packet.substr(0, nonceEnd));
      packet.remove_prefix(nonceEnd + 1);
      const auto deadlineEnd = packet.find('\n');
      if (deadlineEnd == std::string_view::npos)
        return {};
      std::uint64_t expires = 0;
      const auto parsed = std::from_chars(packet.data(), packet.data() + deadlineEnd, expires);
      if (parsed.ec != std::errc{} || parsed.ptr != packet.data() + deadlineEnd)
        return {};
      const auto action = voiceCommandFromName(packet.substr(deadlineEnd + 1));
      if (!action)
        return {};
      return VoicePacket{*action, nonce, expires};
    }
    bool sendReply(int socket, const Datagram& sender, std::string_view nonce, bool applied, std::string_view text) {
      if (!validMessage(text) || text.empty())
        text = applied ? "Voice command applied." : "Voice command was rejected.";
      const auto packet =
          std::string(voiceReply) + std::string(nonce) + '\n' + (applied ? "ok\n" : "error\n") + std::string(text);
      return ::sendto(
                 socket, packet.data(), packet.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&sender.sender),
                 sender.senderSize
             )
          == static_cast<ssize_t>(packet.size());
    }
  } // namespace
  struct SingleInstance::Pending {
    struct Reply {
      VoiceRequest request;
      Datagram sender;
      std::string nonce;
      std::uint64_t expires;
    };
    std::uint64_t next = 0;
    std::deque<VoiceRequest> ready;
    std::map<std::uint64_t, Reply> replies;
    std::map<std::string, std::uint64_t> seen;
    void prune(std::uint64_t now) {
      std::erase_if(replies, [&](const auto& item) { return item.second.expires <= now; });
      std::erase_if(seen, [&](const auto& item) { return item.second <= now; });
      std::erase_if(ready, [&](const auto& item) { return !replies.contains(item.id); });
    }
  };
  SingleInstance::SingleInstance(std::filesystem::path runtime) : m_runtime(std::move(runtime)) {}
  SingleInstance::~SingleInstance() {
    if (m_socket >= 0)
      ::close(m_socket);
    if (m_owner)
      ::unlinkat(m_directory, socketName, 0);
    if (m_lock >= 0)
      ::close(m_lock);
    if (m_directory >= 0)
      ::close(m_directory);
  }
  SingleInstance::Launch SingleInstance::claim(std::string_view activationToken) {
    if (m_directory >= 0)
      fail("Single-instance startup was already initialized.");
    if (m_runtime.empty()) {
      if (const auto* directory = std::getenv("XDG_RUNTIME_DIR"); directory && *directory)
        m_runtime = directory;
      else {
        m_runtime = std::filesystem::path("/run/user") / std::to_string(::getuid());
        if (!std::filesystem::exists(m_runtime)) {
          m_runtime = std::filesystem::path("/tmp") / ("cressida-runtime-" + std::to_string(::getuid()));
          if (::mkdir(m_runtime.c_str(), S_IRWXU) < 0 && errno != EEXIST)
            fail("Could not create the private activation directory.");
        }
      }
    }
    const int runtime = openDirectory(m_runtime);
    if (::mkdirat(runtime, "cressida", S_IRWXU) < 0 && errno != EEXIST) {
      ::close(runtime);
      fail("Could not create the activation directory.");
    }
    m_directory = ::openat(runtime, "cressida", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ::close(runtime);
    if (!privateDirectory(m_directory))
      fail("The activation directory must be private and owned by the current user.");
    m_lock = ::openat(m_directory, lockName, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    struct stat info{};
    if (m_lock < 0
        || ::fstat(m_lock, &info) < 0
        || !S_ISREG(info.st_mode)
        || info.st_uid != ::getuid()
        || info.st_nlink != 1)
      fail("Could not safely open the application instance lock.");
    m_socket = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_socket < 0)
      fail("Could not open the application activation socket.");
    // Resolve through the retained directory descriptor, avoiding path length and symlink races.
    const auto path = "/proc/self/fd/" + std::to_string(m_directory) + "/" + socketName;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    path.copy(address.sun_path, sizeof(address.sun_path) - 1);
    const auto addressSize = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    DesktopActivationToken token;
    token.assign(activationToken);
    const auto packet = std::string(command) + std::to_string(milliseconds()) + '\n' + token.take();
    const auto deadline = Clock::now() + launchDeadline;
    do {
      if (::flock(m_lock, LOCK_EX | LOCK_NB) == 0) {
        if (::fstatat(m_directory, socketName, &info, AT_SYMLINK_NOFOLLOW) == 0) {
          if (!S_ISSOCK(info.st_mode) || info.st_uid != ::getuid() || ::unlinkat(m_directory, socketName, 0) < 0)
            fail("Could not safely replace the stale activation socket.");
        } else if (errno != ENOENT)
          fail("Could not inspect the activation socket.");
        if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&address), addressSize) < 0)
          fail("Could not bind the application activation socket.");
        m_owner = true;
        if (::fchmodat(m_directory, socketName, S_IRUSR | S_IWUSR, 0) < 0)
          fail("Could not protect the application activation socket.");
        const int credentials = 1;
        if (::setsockopt(m_socket, SOL_SOCKET, SO_PASSCRED, &credentials, sizeof(credentials)) < 0)
          fail("Could not authenticate local application commands.");
        return Launch::Owner;
      }
      if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
        fail("Could not acquire the application instance lock.");
      if (::sendto(
              m_socket, packet.data(), packet.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&address),
              addressSize
          )
          == static_cast<ssize_t>(packet.size()))
        return Launch::Forwarded;
      if (errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN && errno != EINTR)
        fail("Could not activate the running Cressida instance.");
      ::poll(nullptr, 0, launchRetry.count());
    } while (Clock::now() < deadline);
    fail("The running Cressida instance could not be reached. Use --new-instance to launch separately.");
  }
  std::optional<std::string> SingleInstance::poll() {
    if (!m_owner)
      return {};
    if (!m_pending)
      m_pending = std::make_unique<Pending>();
    m_pending->prune(milliseconds());
    bool open = false;
    DesktopActivationToken token;
    std::array<char, maximumDatagram> bytes{};
    for (unsigned i = 0; i < maximumBatch; ++i) {
      const auto datagram = receive(m_socket, bytes.data(), bytes.size());
      if (datagram.size < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (datagram.truncated || static_cast<std::size_t>(datagram.size) > bytes.size())
        continue;
      const std::string_view packet(bytes.data(), datagram.size);
      if (packet.starts_with(voiceCommand)) {
        const auto action = parseVoice(packet);
        const auto now = milliseconds();
        const auto lifetime = std::chrono::duration_cast<std::chrono::milliseconds>(voiceTimeout).count();
        if (!action
            || !sameUser(datagram)
            || datagram.sender.sun_family != AF_UNIX
            || datagram.senderSize <= offsetof(sockaddr_un, sun_path)
            || datagram.senderSize > sizeof(sockaddr_un)
            || datagram.sender.sun_path[0] != '\0'
            || action->expires <= now
            || action->expires - now > static_cast<std::uint64_t>(lifetime)
            || m_pending->seen.contains(action->nonce))
          continue;
        if (m_pending->seen.size() >= maximumRecentVoice) {
          sendReply(m_socket, datagram, action->nonce, false, "The local voice command queue is busy.");
          continue;
        }
        m_pending->seen.emplace(action->nonce, action->expires);
        if (m_pending->replies.size() >= maximumPendingVoice) {
          sendReply(m_socket, datagram, action->nonce, false, "The local voice command queue is busy.");
          continue;
        }
        if (++m_pending->next == 0)
          ++m_pending->next;
        const VoiceRequest request{action->command, m_pending->next};
        m_pending->replies.emplace(request.id, Pending::Reply{request, datagram, action->nonce, action->expires});
        m_pending->ready.push_back(request);
        continue;
      }
      if (!packet.starts_with(command))
        continue;
      const auto end = packet.find('\n', command.size());
      if (end == std::string_view::npos)
        continue;
      std::uint64_t sent = 0;
      const auto parsed = std::from_chars(packet.data() + command.size(), packet.data() + end, sent);
      if (parsed.ec != std::errc{} || parsed.ptr != packet.data() + end)
        continue;
      open = true;
      const auto value = packet.substr(end + 1);
      const auto now = milliseconds();
      const auto lifetime =
          std::chrono::duration_cast<std::chrono::milliseconds>(DesktopActivationToken::lifetime).count();
      if (now >= sent && now - sent <= static_cast<std::uint64_t>(lifetime) && !value.empty()) {
        DesktopActivationToken valid;
        if (valid.assign(value))
          token.assign(valid.take());
      }
    }
    return open ? std::optional<std::string>(token.take()) : std::nullopt;
  }
  std::vector<SingleInstance::VoiceRequest> SingleInstance::pollVoice() {
    std::vector<VoiceRequest> result;
    if (!m_owner || !m_pending)
      return result;
    m_pending->prune(milliseconds());
    result.reserve(m_pending->ready.size());
    while (!m_pending->ready.empty()) {
      result.push_back(m_pending->ready.front());
      m_pending->ready.pop_front();
    }
    return result;
  }
  bool SingleInstance::voicePending(const VoiceRequest& request) {
    if (!m_owner || !m_pending)
      return false;
    m_pending->prune(milliseconds());
    const auto found = m_pending->replies.find(request.id);
    return found != m_pending->replies.end() && found->second.request.command == request.command;
  }
  bool SingleInstance::replyVoice(const VoiceRequest& request, bool applied, std::string_view message) {
    if (!voicePending(request))
      return false;
    const auto found = m_pending->replies.find(request.id);
    auto reply = std::move(found->second);
    m_pending->replies.erase(found);
    return sendReply(m_socket, reply.sender, reply.nonce, applied, message);
  }
  SingleInstance::VoiceResult
  SingleInstance::sendVoice(VoiceCommand action, std::filesystem::path runtime, std::chrono::milliseconds timeout) {
    using Status = VoiceResult::Status;
    const VoiceResult missing{Status::NotRunning, "No running Cressida client was found."};
    const VoiceResult transport{Status::TransportError, "Could not contact the running Cressida client."};
    if (voiceCommandName(action).empty() || timeout <= std::chrono::milliseconds::zero() || timeout > voiceTimeout)
      return transport;
    try {
      if (runtime.empty()) {
        if (const auto* directory = std::getenv("XDG_RUNTIME_DIR"); directory && *directory)
          runtime = directory;
        else {
          runtime = std::filesystem::path("/run/user") / std::to_string(::getuid());
          if (!std::filesystem::exists(runtime))
            runtime = std::filesystem::path("/tmp") / ("cressida-runtime-" + std::to_string(::getuid()));
        }
      }
      const Descriptor base(::open(runtime.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (base.fd < 0)
        return errno == ENOENT ? missing : transport;
      if (!privateDirectory(base.fd))
        return transport;
      const Descriptor directory(::openat(base.fd, "cressida", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (directory.fd < 0)
        return errno == ENOENT ? missing : transport;
      if (!privateDirectory(directory.fd))
        return transport;
      struct stat info{};
      if (::fstatat(directory.fd, socketName, &info, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? missing : transport;
      if (!S_ISSOCK(info.st_mode) || info.st_uid != ::getuid() || (info.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        return transport;
      const Descriptor client(::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
      if (client.fd < 0)
        return transport;
      const int credentials = 1;
      if (::setsockopt(client.fd, SOL_SOCKET, SO_PASSCRED, &credentials, sizeof(credentials)) < 0)
        return transport;
      sockaddr_un source{};
      source.sun_family = AF_UNIX;
      if (::bind(client.fd, reinterpret_cast<const sockaddr*>(&source), offsetof(sockaddr_un, sun_path)) < 0)
        return transport;
      socklen_t addressSize = 0;
      const auto address = addressFor(directory.fd, addressSize);
      if (::connect(client.fd, reinterpret_cast<const sockaddr*>(&address), addressSize) < 0)
        return errno == ENOENT || errno == ECONNREFUSED ? missing : transport;
      const auto nonce = newNonce();
      if (!nonce)
        return transport;
      const auto deadline = Clock::now() + timeout;
      const auto expires = std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count();
      const auto packet = std::string(voiceCommand)
          + *nonce
          + '\n'
          + std::to_string(expires)
          + '\n'
          + std::string(voiceCommandName(action));
      // A state-changing command is sent once; a timeout leaves its outcome uncertain.
      if (::send(client.fd, packet.data(), packet.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(packet.size()))
        return transport;
      std::array<char, maximumVoiceReply> bytes{};
      while (Clock::now() < deadline) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        pollfd ready{client.fd, POLLIN, 0};
        const int readable = ::poll(&ready, 1, std::max(0, static_cast<int>(remaining.count())));
        if (readable < 0) {
          if (errno == EINTR)
            continue;
          return transport;
        }
        if (readable == 0)
          break;
        if ((ready.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
          return transport;
        const auto datagram = receive(client.fd, bytes.data(), bytes.size());
        if (datagram.size < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;
          return transport;
        }
        if (datagram.truncated || !sameUser(datagram) || static_cast<std::size_t>(datagram.size) > bytes.size())
          continue;
        std::string_view reply(bytes.data(), datagram.size);
        const auto prefix = std::string(voiceReply) + *nonce + '\n';
        if (!reply.starts_with(prefix))
          continue;
        reply.remove_prefix(prefix.size());
        const auto resultEnd = reply.find('\n');
        if (resultEnd == std::string_view::npos || !validMessage(reply.substr(resultEnd + 1)))
          continue;
        const auto result = reply.substr(0, resultEnd);
        if (result == "ok" || result == "error")
          return {result == "ok" ? Status::Applied : Status::Rejected, std::string(reply.substr(resultEnd + 1))};
      }
      return {Status::TimedOut, "The voice command was not acknowledged. Its state may have changed."};
    } catch (const std::exception&) {
      return transport;
    }
  }
} // namespace app
