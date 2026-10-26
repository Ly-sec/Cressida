#include "app/desktop_activation.h"
#include "app/single_instance.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
  void require(bool value, const char* reason) {
    if (!value)
      throw std::runtime_error(reason);
  }
  struct Runtime {
    std::filesystem::path path;
    Runtime() {
      char pattern[] = "/tmp/cressida-instance-test-XXXXXX";
      const char* created = ::mkdtemp(pattern);
      require(created, "Create a private test runtime");
      path = created;
    }
    ~Runtime() { std::filesystem::remove_all(path); }
  };
  std::string packet(
      std::string token,
      std::uint64_t timestamp =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count()
  ) {
    return "Cressida.Open/1\n" + std::to_string(timestamp) + '\n' + token;
  }
  void send(const Runtime& runtime, const std::string& data) {
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "Open a test sender");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = (runtime.path / "cressida/instance.sock").string();
    require(path.size() < sizeof(address.sun_path), "The fixture socket path fits");
    path.copy(address.sun_path, sizeof(address.sun_path) - 1);
    const auto count = ::sendto(
        fd, data.data(), data.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&address),
        offsetof(sockaddr_un, sun_path) + path.size() + 1
    );
    ::close(fd);
    require(count == static_cast<ssize_t>(data.size()), "Send the complete test activation");
  }
  std::uint64_t milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  std::string voicePacket(
      std::string nonce, std::string action = "input-mute toggle", std::uint64_t expires = milliseconds() + 1500
  ) {
    return "Cressida.Voice/1\n" + nonce + '\n' + std::to_string(expires) + '\n' + action;
  }
  struct VoicePeer {
    int fd = -1;
    explicit VoicePeer(const Runtime& runtime) {
      fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      require(fd >= 0, "Create a voice command peer");
      sockaddr_un source{};
      source.sun_family = AF_UNIX;
      require(
          ::bind(fd, reinterpret_cast<const sockaddr*>(&source), offsetof(sockaddr_un, sun_path)) == 0,
          "Allocate an ephemeral reply address without creating a file"
      );
      sockaddr_un target{};
      target.sun_family = AF_UNIX;
      const auto path = (runtime.path / "cressida/instance.sock").string();
      require(path.size() < sizeof(target.sun_path), "The voice fixture endpoint fits");
      path.copy(target.sun_path, sizeof(target.sun_path) - 1);
      require(
          ::connect(fd, reinterpret_cast<const sockaddr*>(&target), offsetof(sockaddr_un, sun_path) + path.size() + 1)
              == 0,
          "Connect the voice peer to the owner"
      );
    }
    ~VoicePeer() { ::close(fd); }
    void send(const std::string& bytes) const {
      require(
          ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size()),
          "Send a bounded synthetic voice request"
      );
    }
    std::string reply() const {
      pollfd ready{fd, POLLIN, 0};
      require(::poll(&ready, 1, 1000) == 1, "A reply arrives within its deadline");
      std::array<char, 1024> bytes{};
      const auto count = ::recv(fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
      require(count > 0, "Receive the voice acknowledgement");
      return {bytes.data(), static_cast<std::size_t>(count)};
    }
  };
  void ready(const app::SingleInstance& owner) {
    pollfd descriptor{owner.eventFd(), POLLIN, 0};
    require(::poll(&descriptor, 1, 1500) == 1, "A voice request wakes the native event loop");
  }
  int wait(pid_t child) {
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status), "The child exits normally");
    return WEXITSTATUS(status);
  }
  int launch(const Runtime& runtime, const char* binary, const char* option) {
    const auto child = ::fork();
    require(child >= 0, "Fork a real launcher");
    if (child == 0) {
      ::setenv("XDG_RUNTIME_DIR", runtime.path.c_str(), 1);
      ::setenv("XDG_ACTIVATION_TOKEN", "offline-launch-token", 1);
      ::setenv("XDG_CONFIG_HOME", (runtime.path / "config").c_str(), 1);
      ::setenv("XDG_STATE_HOME", (runtime.path / "state").c_str(), 1);
      ::setenv("XDG_CACHE_HOME", (runtime.path / "cache").c_str(), 1);
      ::setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/cressida-test-no-session-bus", 1);
      ::setenv("WAYLAND_DISPLAY", (runtime.path / "no-wayland-display").c_str(), 1);
      const int sink = ::open("/dev/null", O_WRONLY);
      ::dup2(sink, STDOUT_FILENO);
      ::dup2(sink, STDERR_FILENO);
      ::close(sink);
      // A missing token file prevents any account or graphics startup if handoff regresses.
      if (*option)
        ::execl(binary, binary, option, "--token-file", "/cressida-test-no-such-token-file", nullptr);
      else
        ::execl(binary, binary, "--token-file", "/cressida-test-no-such-token-file", nullptr);
      ::_exit(127);
    }
    return wait(child);
  }
  pid_t launchVoice(const Runtime& runtime, const char* binary, std::initializer_list<const char*> arguments) {
    const auto child = ::fork();
    require(child >= 0, "Fork the real voice command launcher");
    if (child == 0) {
      ::setenv("XDG_RUNTIME_DIR", runtime.path.c_str(), 1);
      ::setenv("XDG_ACTIVATION_TOKEN", "must-not-activate", 1);
      ::setenv("XDG_CONFIG_HOME", (runtime.path / "config").c_str(), 1);
      ::setenv("XDG_STATE_HOME", (runtime.path / "state").c_str(), 1);
      ::setenv("XDG_CACHE_HOME", (runtime.path / "cache").c_str(), 1);
      ::setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/cressida-test-no-session-bus", 1);
      ::setenv("WAYLAND_DISPLAY", (runtime.path / "no-wayland-display").c_str(), 1);
      ::unsetenv("CRESSIDA_TOKEN");
      const int sink = ::open("/dev/null", O_WRONLY);
      ::dup2(sink, STDOUT_FILENO);
      ::dup2(sink, STDERR_FILENO);
      ::close(sink);
      std::vector<char*> values{const_cast<char*>(binary)};
      for (const auto argument : arguments)
        values.push_back(const_cast<char*>(argument));
      values.push_back(nullptr);
      ::execv(binary, values.data());
      ::_exit(127);
    }
    return child;
  }
  char readReport(int fd) {
    pollfd descriptor{fd, POLLIN, 0};
    constexpr int deadlineMs = 10000;
    require(::poll(&descriptor, 1, deadlineMs) == 1, "Concurrent startup completes within its deadline");
    char value = 0;
    require(::read(fd, &value, sizeof(value)) == sizeof(value), "Read a startup result");
    return value;
  }
} // namespace
int main(int argc, char** argv) {
  try {
    {
      Runtime runtime;
      using Status = app::SingleInstance::VoiceResult::Status;
      require(
          app::SingleInstance::sendVoice(app::VoiceCommand::InputMuteToggle, runtime.path).status == Status::NotRunning
              && std::filesystem::is_empty(runtime.path),
          "A voice command without an owner never creates the application directory or claims its lock"
      );
      require(
          app::SingleInstance::sendVoice(app::VoiceCommand::InputMuteOff, runtime.path / "missing").status
                  == Status::NotRunning
              && std::filesystem::is_empty(runtime.path),
          "An absent runtime remains absent after a voice command"
      );
      for (const auto unknown : {0, 4, 5})
        require(
            app::SingleInstance::sendVoice(static_cast<app::VoiceCommand>(unknown), runtime.path).status
                == Status::TransportError,
            "Unrecognized typed commands cannot reach the socket"
        );
      if (argc > 1) {
        require(
            wait(launchVoice(runtime, argv[1], {"voice", "input-mute", "toggle"})) == 1
                && std::filesystem::is_empty(runtime.path),
            "The actual CLI exits without credentials, graphics, logging or an owner election when no client runs"
        );
      }
    }
    {
      Runtime runtime;
      app::SingleInstance owner(runtime.path);
      require(owner.claim() == app::SingleInstance::Launch::Owner, "Elect an owner for voice commands");
      std::uint64_t previousId = 0;
      for (const auto& [command, name] : app::voiceCommandNames) {
        auto response =
            std::async(std::launch::async, [&] { return app::SingleInstance::sendVoice(command, runtime.path); });
        ready(owner);
        require(!owner.poll(), "A voice command never requests an Open or activation token");
        const auto requests = owner.pollVoice();
        require(
            requests.size() == 1 && requests.front().command == command && requests.front().id > previousId,
            "Each supported voice action has an individual ordered request"
        );
        previousId = requests.front().id;
        require(
            !owner.replyVoice({app::VoiceCommand::InputMuteToggle, previousId + 1}, true),
            "An unrelated reply handle cannot acknowledge a request"
        );
        require(owner.replyVoice(requests.front(), true, name), "A same-user command receives an explicit owner reply");
        require(!owner.replyVoice(requests.front(), true), "A request cannot be acknowledged twice");
        const auto result = response.get();
        require(
            result.applied() && result.message == name, "The requester receives the matching bounded success result"
        );
        require(owner.pollVoice().empty() && !owner.poll(), "A successful command never repeats or opens the window");
      }
      auto rejected = std::async(std::launch::async, [&] {
        return app::SingleInstance::sendVoice(app::VoiceCommand::InputMuteOn, runtime.path);
      });
      ready(owner);
      send(runtime, packet("independent-open"));
      require(owner.poll() == "independent-open", "Open activation survives a simultaneous voice command");
      const auto pending = owner.pollVoice();
      require(pending.size() == 1, "The simultaneous voice command remains available separately");
      require(
          owner.replyVoice(pending.front(), false, "Sign in before using voice controls."),
          "Reply with a domain rejection"
      );
      const auto result = rejected.get();
      require(
          result.status == app::SingleInstance::VoiceResult::Status::Rejected
              && result.message == "Sign in before using voice controls.",
          "Domain rejection is distinct from transport failure"
      );
      send(runtime, packet("next-open"));
      require(owner.pollVoice().empty(), "Voice retrieval never drains a newly arrived Open");
      require(owner.poll() == "next-open", "The next owning-loop drain delivers that Open normally");
      auto bounded = std::async(std::launch::async, [&] {
        return app::SingleInstance::sendVoice(app::VoiceCommand::InputMuteOn, runtime.path);
      });
      ready(owner);
      require(!owner.poll(), "The bounded reply fixture does not activate");
      const auto boundedRequest = owner.pollVoice();
      require(
          boundedRequest.size() == 1 && owner.replyVoice(boundedRequest.front(), true, std::string(512, 'x')),
          "Oversized status output is replaced with bounded text"
      );
      require(bounded.get().message == "Voice command applied.", "The peer receives only the bounded fallback message");
      auto timeout = std::async(std::launch::async, [&] {
        return app::SingleInstance::sendVoice(
            app::VoiceCommand::InputMuteToggle, runtime.path, std::chrono::milliseconds(30)
        );
      });
      ready(owner);
      require(
          timeout.get().status == app::SingleInstance::VoiceResult::Status::TimedOut,
          "An unacknowledged command has a finite uncertain outcome"
      );
      require(
          !owner.poll() && owner.pollVoice().empty(),
          "An expired queued toggle is discarded without replay or application"
      );
      require(!owner.poll(), "The requester sent the timed-out toggle only once");
      auto detachedTimeout = std::async(std::launch::async, [&] {
        return app::SingleInstance::sendVoice(
            app::VoiceCommand::InputMuteToggle, runtime.path, std::chrono::milliseconds(200)
        );
      });
      ready(owner);
      require(!owner.poll(), "A detached request remains independent of activation");
      const auto detached = owner.pollVoice();
      require(
          detached.size() == 1 && owner.voicePending(detached.front()),
          "A freshly detached command can be checked immediately before application"
      );
      require(
          detachedTimeout.get().status == app::SingleInstance::VoiceResult::Status::TimedOut,
          "A detached command can expire while earlier owning-loop work is pending"
      );
      require(
          !owner.voicePending(detached.front())
              && !owner.replyVoice(detached.front(), true)
              && owner.pollVoice().empty(),
          "The final pre-application guard rejects an expired detached request and its late acknowledgement"
      );
      if (argc > 1) {
        for (const auto& [command, name] : app::voiceCommandNames) {
          const auto separator = name.find(' ');
          const std::string action(name.substr(0, separator)), value(name.substr(separator + 1));
          const auto child = launchVoice(runtime, argv[1], {"voice", action.c_str(), value.c_str()});
          ready(owner);
          require(!owner.poll(), "The executable voice route never opens the window");
          const auto requests = owner.pollVoice();
          require(
              requests.size() == 1 && requests.front().command == command,
              "The executable forwards every supported microphone mute action"
          );
          require(
              owner.replyVoice(requests.front(), true, "Applied by the offline owner."),
              "Acknowledge the executable command"
          );
          require(wait(child) == 0, "An applied executable command exits successfully");
        }
        const auto rejectedChild = launchVoice(runtime, argv[1], {"voice", "input-mute", "on"});
        ready(owner);
        require(!owner.poll(), "A rejected command never activates the owner");
        const auto request = owner.pollVoice();
        require(
            request.size() == 1 && owner.replyVoice(request.front(), false, "Sign in first."),
            "Reject a real CLI request"
        );
        require(wait(rejectedChild) == 1, "The actual CLI reports an owner rejection as failure");
        for (const auto arguments :
             {std::initializer_list<const char*>{"voice", "input-mute", "invalid"},
              {"voice", "push-to-talk", "toggle"},
              {"voice", "push-to-talk", "on"},
              {"voice", "push-to-talk", "off"},
              {"voice", "input-mute", "on", "--new-instance"},
              {"--new-instance", "voice", "input-mute", "on"}})
          require(
              wait(launchVoice(runtime, argv[1], arguments)) == 2 && !owner.poll() && owner.pollVoice().empty(),
              "Invalid CLI arguments fail before IPC or account startup"
          );
        require(
            wait(launchVoice(runtime, argv[1], {"voice", "--help"})) == 0 && !owner.poll(),
            "Voice help never contacts the running client"
        );
        const auto timedOutChild = launchVoice(runtime, argv[1], {"voice", "input-mute", "toggle"});
        ready(owner);
        require(wait(timedOutChild) == 1, "The actual CLI stops waiting at its bounded reply deadline");
        require(
            !owner.poll() && owner.pollVoice().empty(), "The real timed-out command cannot apply after its deadline"
        );
        for (const char* path : {"config", "state", "cache"})
          require(
              !std::filesystem::exists(runtime.path / path),
              "The voice CLI never starts account storage or diagnostic logging"
          );
      }
    }
    {
      Runtime runtime;
      app::SingleInstance owner(runtime.path);
      require(owner.claim() == app::SingleInstance::Launch::Owner, "Elect the malformed voice fixture owner");
      VoicePeer peer(runtime);
      const std::string nonce(32, 'a');
      for (const auto& invalid :
           {voicePacket("short"), voicePacket(nonce, "input-mute on", 0),
            voicePacket(nonce, "input-mute on", milliseconds() + 10000), voicePacket(nonce, "input-mute maybe"),
            voicePacket(nonce, "push-to-talk on"), voicePacket(nonce, "push-to-talk off"),
            voicePacket(nonce, std::string("input-mute on\0", 14)), voicePacket(nonce, std::string(4096, 'x'))}) {
        peer.send(invalid);
        require(
            !owner.poll() && owner.pollVoice().empty(), "Malformed, expired and future voice payloads have no effects"
        );
      }
      send(runtime, voicePacket(nonce));
      require(!owner.poll() && owner.pollVoice().empty(), "Voice requests without a reply address cannot execute");
      const auto duplicate = voicePacket(nonce);
      peer.send(duplicate);
      peer.send(duplicate);
      require(!owner.poll(), "Repeated voice datagrams never activate");
      const auto requests = owner.pollVoice();
      require(requests.size() == 1, "A repeated nonce produces at most one state-changing request");
      require(owner.replyVoice(requests.front(), true, "applied"), "Acknowledge the unique request");
      require(peer.reply().ends_with("\nok\napplied"), "The acknowledgement includes the expected success framing");
      peer.send(duplicate);
      require(
          !owner.poll() && owner.pollVoice().empty(), "Acknowledged toggles remain protected against duplicate delivery"
      );
    }
    {
      Runtime runtime;
      app::SingleInstance owner(runtime.path);
      require(owner.claim() == app::SingleInstance::Launch::Owner, "Elect the bounded queue fixture owner");
      VoicePeer peer(runtime);
      constexpr unsigned requestLimit = 32;
      for (unsigned index = 0; index <= requestLimit; ++index) {
        const auto suffix = std::to_string(index);
        peer.send(voicePacket(std::string(32 - suffix.size(), '0') + suffix));
        require(!owner.poll(), "Queued commands remain separate from window activation");
      }
      const auto requests = owner.pollVoice();
      require(requests.size() == requestLimit, "The command queue has a fixed bound without coalescing toggles");
      for (unsigned index = 0; index < requests.size(); ++index)
        require(
            requests[index].command == app::VoiceCommand::InputMuteToggle && requests[index].id == index + 1,
            "Independent toggles retain their arrival order"
        );
      require(
          peer.reply().ends_with("\nerror\nThe local voice command queue is busy."),
          "Queue overflow receives a bounded rejection"
      );
    }
    {
      Runtime runtime;
      app::SingleInstance owner(runtime.path);
      require(owner.claim() == app::SingleInstance::Launch::Owner, "The first launch owns the application");
      require((::fcntl(owner.eventFd(), F_GETFD) & FD_CLOEXEC) != 0, "The activation socket never leaks through exec");
      struct stat info{};
      require(
          ::stat((runtime.path / "cressida/instance.sock").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "Only the current user can send activation requests"
      );
      app::SingleInstance second(runtime.path);
      require(
          second.claim("desktop-token") == app::SingleInstance::Launch::Forwarded && second.eventFd() < 0,
          "Another launch forwards before starting an application"
      );
      pollfd ready{owner.eventFd(), POLLIN, 0};
      require(::poll(&ready, 1, 0) == 1, "A forwarded Open wakes the native event loop");
      require(owner.poll() == "desktop-token" && !owner.poll(), "Activation tokens are delivered once");
      app::SingleInstance third(runtime.path), fourth(runtime.path);
      require(
          third.claim("latest-token") == app::SingleInstance::Launch::Forwarded
              && fourth.claim() == app::SingleInstance::Launch::Forwarded,
          "Launches coalesce without starting a second owner"
      );
      require(owner.poll() == "latest-token", "An empty request preserves a coalesced valid activation handle");
      send(runtime, "garbage");
      send(runtime, "Cressida.Open/1\nnot-a-timestamp\nsecret");
      send(runtime, packet(std::string(app::DesktopActivationToken::maximumBytes + 256, 'x')));
      require(!owner.poll(), "Malformed and oversized datagrams cannot request activation");
      send(runtime, packet("expired-token", 0));
      require(owner.poll() == "", "An old Open still restores the window without reusing an expired token");
      send(runtime, packet("future-token", std::numeric_limits<std::uint64_t>::max()));
      require(owner.poll() == "", "Future timestamps cannot make an activation token immortal");
      send(runtime, packet(std::string("bad\0token", 9)));
      require(owner.poll() == "", "Embedded NULs never reach Wayland activation");
      const std::string maximum(app::DesktopActivationToken::maximumBytes, 'x');
      app::SingleInstance bounded(runtime.path);
      require(
          bounded.claim(maximum) == app::SingleInstance::Launch::Forwarded && owner.poll() == maximum,
          "The complete bounded activation handle fits in one datagram"
      );
      if (argc > 1) {
        require(
            launch(runtime, argv[1], "") == 0 && owner.poll() == "offline-launch-token",
            "An ordinary command launch activates the owner before reading credentials"
        );
        require(
            launch(runtime, argv[1], "--start-minimized") == 0 && owner.poll() == "offline-launch-token",
            "The real launcher hands off before reading credentials, including start-minimized launches"
        );
        for (const char* option : {"--new-instance", "-n"})
          require(
              launch(runtime, argv[1], option) == 1 && !owner.poll(),
              "Explicit separate launches bypass handoff and continue to their own token validation"
          );
        for (const char* option : {"--demo", "--smoke-test"})
          require(
              launch(runtime, argv[1], option) == 1 && !owner.poll(),
              "Offline previews bypass handoff and reach the private missing-display fixture"
          );
        require(
            launch(runtime, argv[1], "--new-instance=true") == 2 && !owner.poll(),
            "Invalid instance options fail before contacting the owner"
        );
        for (const char* option : {"--help", "--version"})
          require(
              launch(runtime, argv[1], option) == 0 && !owner.poll(), "Help and version never contact another instance"
          );
      }
    }
    {
      Runtime runtime;
      const auto child = ::fork();
      require(child >= 0, "Fork an owner that exits without cleanup");
      if (child == 0) {
        app::SingleInstance owner(runtime.path);
        ::_exit(owner.claim() == app::SingleInstance::Launch::Owner ? 0 : 1);
      }
      require(wait(child) == 0, "The crashed fixture acquired the lock");
      require(std::filesystem::exists(runtime.path / "cressida/instance.sock"), "Abrupt exit leaves a stale socket");
      struct stat before{}, after{};
      require(::stat((runtime.path / "cressida/instance.sock").c_str(), &before) == 0, "Inspect the crashed endpoint");
      require(
          app::SingleInstance::sendVoice(app::VoiceCommand::InputMuteToggle, runtime.path).status
                  == app::SingleInstance::VoiceResult::Status::NotRunning
              && ::stat((runtime.path / "cressida/instance.sock").c_str(), &after) == 0
              && before.st_ino == after.st_ino,
          "A voice command neither claims a dead owner's lock nor replaces its stale socket"
      );
      app::SingleInstance replacement(runtime.path);
      require(
          replacement.claim() == app::SingleInstance::Launch::Owner, "A fresh launch safely replaces a crashed owner"
      );
    }
    {
      Runtime runtime;
      int start[2]{}, reports[2]{}, release[2]{};
      require(::pipe(start) == 0 && ::pipe(reports) == 0 && ::pipe(release) == 0, "Create startup race barriers");
      constexpr unsigned racers = 4;
      pid_t children[racers]{};
      for (auto& child : children) {
        child = ::fork();
        require(child >= 0, "Start concurrent launchers");
        if (child == 0) {
          char go;
          if (::read(start[0], &go, sizeof(go)) != sizeof(go))
            ::_exit(1);
          app::SingleInstance instance(runtime.path);
          const bool owner = instance.claim() == app::SingleInstance::Launch::Owner;
          const char result = owner ? 'O' : 'F';
          if (::write(reports[1], &result, sizeof(result)) != sizeof(result))
            ::_exit(1);
          if (owner && ::read(release[0], &go, sizeof(go)) != sizeof(go))
            ::_exit(1);
          ::_exit(0);
        }
      }
      for (unsigned i = 0; i < racers; ++i)
        require(::write(start[1], "s", 1) == 1, "Release a launcher");
      unsigned owners = 0;
      for (unsigned i = 0; i < racers; ++i) {
        const char result = readReport(reports[0]);
        require(result == 'O' || result == 'F', "Every concurrent launch elects or forwards");
        owners += result == 'O';
      }
      require(owners == 1, "Simultaneous launches create exactly one application owner");
      require(::write(release[1], "s", 1) == 1, "Release the elected owner");
      for (auto child : children)
        require(wait(child) == 0, "All launchers exit cleanly");
      for (int fd : {start[0], start[1], reports[0], reports[1], release[0], release[1]})
        ::close(fd);
    }
    {
      Runtime runtime;
      std::filesystem::create_directory(runtime.path / "cressida");
      ::chmod((runtime.path / "cressida").c_str(), 0700);
      const auto path = runtime.path / "cressida/instance.sock";
      std::ofstream(path) << "preserve this ordinary file";
      require(
          app::SingleInstance::sendVoice(app::VoiceCommand::InputMuteOff, runtime.path).status
                  == app::SingleInstance::VoiceResult::Status::TransportError
              && !std::filesystem::exists(runtime.path / "cressida/instance.lock"),
          "A command rejects an ordinary-file endpoint without opening the instance lock"
      );
      bool rejected = false;
      try {
        app::SingleInstance instance(runtime.path);
        instance.claim();
      } catch (const std::exception&) {
        rejected = true;
      }
      std::ifstream file(path);
      std::string content;
      std::getline(file, content);
      require(rejected && content == "preserve this ordinary file", "Stale cleanup never removes an ordinary file");
    }
    for (const bool directory : {true, false}) {
      Runtime runtime, outside;
      std::filesystem::create_directory(runtime.path / "cressida");
      ::chmod((runtime.path / "cressida").c_str(), 0700);
      if (directory) {
        std::filesystem::remove(runtime.path / "cressida");
        std::filesystem::create_directory_symlink(outside.path, runtime.path / "cressida");
        require(
            app::SingleInstance::sendVoice(app::VoiceCommand::InputMuteOn, runtime.path).status
                    == app::SingleInstance::VoiceResult::Status::TransportError
                && std::filesystem::is_empty(outside.path),
            "Voice commands reject a symlinked application directory without following it"
        );
      } else
        std::filesystem::create_symlink(outside.path / "untouched", runtime.path / "cressida/instance.lock");
      bool rejected = false;
      try {
        app::SingleInstance instance(runtime.path);
        instance.claim();
      } catch (const std::exception&) {
        rejected = true;
      }
      require(
          rejected && std::filesystem::is_empty(outside.path),
          "Activation startup never follows directory or lock symlinks"
      );
    }
    std::cout
        << "Single-instance election, activation, voice commands, stale recovery, race and bootstrap checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
