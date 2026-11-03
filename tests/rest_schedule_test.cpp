#include "api/connection_bootstrap.h"
#include "api/rest_rate_limits.h"
#include "api/rest_schedule.h"

#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }

  struct Work {
    std::string name;
    bool background = false;
    bool read = false;
  };
} // namespace

int main() {
  try {
    std::atomic<std::uint64_t> session{7}, revision{0}, ready{0};
    std::stop_source stop;
    api::RestRequestScope guilds{stop.get_token(), &session, 7, &revision, 0, &ready};
    std::deque<Work> pending{{"guilds-page", true}, {"initial-settings", true}, {"channel-details", true}};
    const auto next = [&] {
      const auto at = api::nextRestWork(pending, [](const auto& command) { return command.background; });
      auto result = std::move(*at);
      pending.erase(at);
      return result;
    };
    auto suspended = next();
    require(suspended.name == "guilds-page", "Bootstrap reads retain their order while no user work is waiting");
    pending.push_back({"channels"});
    pending.push_back({"messages"});
    pending.push_back({"send"});
    ++revision;
    require(
        guilds.cancellation() == api::RestCancellation::ForegroundReady,
        "A slow bootstrap page must yield when visible channel/history work is accepted"
    );
    pending.push_back(std::move(suspended));
    require(
        next().name == "channels" && next().name == "messages" && next().name == "send",
        "Selected-channel work runs before suspended metadata without reordering accepted foreground operations"
    );
    require(
        next().name == "initial-settings"
            && next().name == "channel-details"
            && next().name == "guilds-page"
            && pending.empty(),
        "Suspending a GET must keep exactly one continuation, including optional channel details"
    );
    api::RestRequestScope resumed{stop.get_token(), &session, 7, &revision, revision.load(), &ready};
    require(resumed.cancellation() == api::RestCancellation::None, "A resumed read uses the new queue revision");
    api::RestRequestScope mutation{stop.get_token(), &session, 7};
    std::atomic_bool messageRevoked{false};
    api::RestRequestScope messageWrite{stop.get_token(), &session, 7};
    messageWrite.revoked = &messageRevoked;
    require(
        messageWrite.cancellation() == api::RestCancellation::None,
        "Accepted message writes retain an independent permission lease"
    );
    messageRevoked = true;
    require(
        messageWrite.cancellation() == api::RestCancellation::Superseded,
        "Revoked message permission cancels queued admission and future rate-limit attempts"
    );
    ++revision;
    require(
        mutation.cancellation() == api::RestCancellation::None,
        "New visible work must never abort an accepted mutation or cause an uncertain write to be retried"
    );
    ready = 7;
    require(
        resumed.cancellation() == api::RestCancellation::Superseded,
        "A complete Gateway READY supersedes even an already-running fallback guild page"
    );
    session = 8;
    require(
        mutation.cancellation() == api::RestCancellation::SessionChanged
            && resumed.cancellation() == api::RestCancellation::SessionChanged,
        "Changing accounts cancels both reads and writes before another session can consume them"
    );
    {
      api::ConnectionBootstrap bootstrap;
      bootstrap.begin(8, true);
      api::RestRequestScope verification{stop.get_token(), &session, 8};
      verification.authoritativeSession = &bootstrap.resolved();
      require(
          !bootstrap.fail(8, api::BootstrapSource::Rest, false) && bootstrap.pending(8),
          "An HTTP transport failure cannot discard an authentication still arriving through Gateway"
      );
      const auto ready = bootstrap.succeed(8);
      require(
          ready
              && ready->authenticated
              && ready->restored
              && !ready->unauthorized
              && verification.cancellation() == api::RestCancellation::Superseded,
          "Gateway identity completes restored sign-in and cancels the redundant HTTP verification"
      );
      require(
          !bootstrap.fail(8, api::BootstrapSource::Rest, true) && !bootstrap.succeed(8),
          "A losing HTTP 401 or duplicate READY cannot replace the first authenticated identity"
      );
      bootstrap.begin(9, false);
      require(!bootstrap.succeed(8) && bootstrap.pending(9), "An old attempt cannot authenticate a new connection");
      const auto rejected = bootstrap.fail(9, api::BootstrapSource::Gateway, true);
      require(
          rejected
              && !rejected->authenticated
              && rejected->unauthorized
              && !rejected->restored
              && !bootstrap.succeed(9),
          "A current invalid credential closes sign-in immediately, before the other path can succeed"
      );
      bootstrap.begin(10, true);
      require(
          !bootstrap.fail(10, api::BootstrapSource::Gateway, false),
          "An unavailable Gateway leaves HTTP identity verification available"
      );
      const auto failed = bootstrap.fail(10, api::BootstrapSource::Rest, false);
      require(
          failed && !failed->authenticated && !failed->unauthorized && failed->restored,
          "Two transport failures return one recoverable restored-login failure without rejecting its credential"
      );
      bootstrap.begin(11, false);
      std::mutex mutex;
      unsigned completions = 0;
      const auto finish = [&] {
        std::lock_guard lock(mutex);
        if (bootstrap.succeed(11))
          ++completions;
      };
      std::jthread http(finish), gateway(finish);
      http.join();
      gateway.join();
      require(completions == 1, "Concurrent valid identity paths permit exactly one completion and credential save");
      bootstrap.begin(12, true);
      bootstrap.cancel();
      require(!bootstrap.succeed(12) && !bootstrap.pending(12), "Cancelled restoration cannot later authenticate");
    }
    {
      std::deque<Work> work{
          {"send", false, false},
          {"profile", false, false},
          {"optional", true, true},
          {"messages", false, true},
          {"channels", false, true}
      };
      const auto read = [](const auto& job) { return job.read; };
      const auto background = [](const auto& job) { return job.background; };
      auto at = api::nextRestRead(work, read, background, false);
      require(at->name == "messages", "A slow account job cannot own the independent message read lane");
      work.erase(at);
      at = api::nextRestRead(work, read, background, true);
      require(at->name == "channels", "Visible reads retain capacity while an optional read is active");
      work.erase(at);
      require(
          api::nextRestRead(work, read, background, true) == work.end(),
          "Only one optional read may run, reserving two lanes for visible work"
      );
      require(api::nextRestOwned(work, read, background)->name == "send", "Only the account owner dequeues mutations");
      work.erase(api::nextRestOwned(work, read, background));
      require(api::nextRestOwned(work, read, background)->name == "profile", "Account work retains its FIFO order");
    }
    {
      std::atomic<std::uint64_t> navigation{1}, credentials{1};
      api::RestRequestScope read;
      read.readRevision = &navigation;
      read.expectedReadRevision = 1;
      read.credentialRevision = &credentials;
      read.expectedCredentialRevision = 1;
      ++navigation;
      require(
          read.cancellation() == api::RestCancellation::Superseded,
          "An old history read yields immediately to a newer history request"
      );
      read.expectedReadRevision = navigation;
      ++credentials;
      require(
          read.cancellation() == api::RestCancellation::Superseded,
          "Credential changes invalidate old read replies before authentication handling"
      );
      require(!mutation.readRevision && !mutation.credentialRevision, "Read cancellation never applies to mutations");
    }
    {
      api::RestReadIntent intent;
      std::atomic<std::uint64_t> navigation{4}, visible{1}, credentials{1};
      api::RestRequestScope preload;
      preload.foregroundRevision = &visible;
      preload.expectedRevision = 1;
      preload.credentialRevision = &credentials;
      preload.expectedCredentialRevision = 1;
      preload.intent = &intent;
      preload.promotedNavigation = &navigation;
      ++visible;
      require(
          preload.cancellation() == api::RestCancellation::ForegroundReady,
          "An unselected speculative read yields to visible work"
      );
      intent.promote(navigation);
      require(
          preload.cancellation() == api::RestCancellation::None,
          "Selecting an in-flight preload keeps its transfer when foreground work is accepted"
      );
      ++credentials;
      require(
          preload.cancellation() == api::RestCancellation::Superseded,
          "Promotion cannot bypass credential-generation validation"
      );
      preload.expectedCredentialRevision = credentials;
      ++navigation;
      require(
          preload.cancellation() == api::RestCancellation::Superseded,
          "Leaving an adopted conversation cancels its old response"
      );
      intent.cancelled = true;
      intent.promote(navigation);
      require(
          preload.cancellation() == api::RestCancellation::Superseded,
          "A completed cancellation cannot be revived through promotion"
      );
    }
    {
      using namespace std::chrono_literals;
      api::RestRateLimits limits;
      const auto now = api::RestRateLimits::Clock::now();
      limits.limited("/channels/10/messages?before=99", 5s, false, now);
      require(
          limits.deadline("/channels/10/messages?after=100") == now + 5s
              && limits.deadline("/channels/11/messages") < now,
          "Cursor variants share a cooldown while unrelated conversations remain available"
      );
      limits.limited("/channels/10/messages/100/ack", 3s, false, now);
      require(
          limits.deadline("/channels/10/messages/101/ack") == now + 3s,
          "Changing a non-major ID cannot bypass a route cooldown"
      );
      limits.limited("/users/@me", 2s, true, now);
      require(
          limits.deadline("/guilds/20/channels") == now + 2s && limits.deadline("/channels/10/messages") == now + 5s,
          "Global limits cover every lane without shortening route limits"
      );
      limits.clear();
      require(limits.deadline("/channels/10/messages") < now, "Account reset clears shared admission state");
    }
    stop.request_stop();
    require(mutation.cancellation() == api::RestCancellation::Stopped, "Shutdown terminates any remaining request");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
