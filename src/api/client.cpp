#include "api/client.h"

#include "api/command_uploads.h"
#include "api/network_profile.h"
#include "api/request_diagnostics.h"
#include "api/rest_schedule.h"
#include "api/rest_transfer.h"
#include "api/rest_transport_cache.h"
#include "api/server_organization.h"
#include "api/thread_member_list.h"
#include "api/uploads.h"
#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <curl/curl.h>
#include <expected>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <stdexcept>
#include <utility>

namespace api {
  namespace {
    using Json = nlohmann::json;
    const Logger restLog("rest");
    const Logger accountLog("account");
    const char* operationLabel(Operation operation) {
      switch (operation) {
      case Operation::Connect:
        return "connect";
      case Operation::Send:
        return "send-message";
      case Operation::Acknowledge:
        return "read-receipt";
      case Operation::Reaction:
        return "reaction";
      case Operation::Interaction:
        return "interaction";
      case Operation::CommandIndex:
        return "application-commands";
      case Operation::Profile:
        return "profile";
      case Operation::SaveProfile:
        return "save-profile";
      case Operation::SavePresence:
        return "save-presence";
      case Operation::AccountSettings:
        return "account-settings";
      case Operation::SaveAccountSettings:
        return "save-account-settings";
      case Operation::SettingsList:
        return "settings-list";
      case Operation::GifFavorites:
      case Operation::SaveGifFavorite:
        return "gif-favorites";
      case Operation::EmojiPreferences:
      case Operation::SaveEmojiFavorite:
        return "emoji-preferences";
      case Operation::Messages:
        return "messages";
      case Operation::Channels:
        return "channels";
      case Operation::PrivateChannels:
        return "direct-messages";
      case Operation::Guilds:
        return "servers";
      case Operation::ChannelDetails:
        return "channel-details";
      case Operation::ChannelPage:
        return "channel-page";
      case Operation::InboxRead:
      case Operation::InboxWrite:
        return "inbox";
      case Operation::StreamPreviewRead:
      case Operation::StreamPreviewWrite:
        return "stream-preview";
      case Operation::Threads:
        return "threads";
      case Operation::GuildStickers:
        return "stickers";
      case Operation::StickerPacks:
        return "sticker-packs";
      case Operation::StickerDetails:
        return "sticker";
      case Operation::GuildEmojis:
        return "emojis";
      case Operation::GifSearch:
        return "gifs";
      case Operation::GuildSettings:
        return "server-settings";
      case Operation::UserSettings:
        return "user-settings";
      case Operation::BootstrapSettings:
        return "startup-settings";
      default:
        return "account";
      }
    }
    bool independentRead(Operation operation) {
      switch (operation) {
      case Operation::CommandIndex:
      case Operation::Messages:
      case Operation::Channels:
      case Operation::PrivateChannels:
      case Operation::ChannelDetails:
      case Operation::ChannelPage:
      case Operation::Threads:
      case Operation::GuildEmojis:
      case Operation::GuildStickers:
      case Operation::StickerPacks:
      case Operation::StickerDetails:
      case Operation::GifSearch:
      case Operation::InboxRead:
      case Operation::StreamPreviewRead:
        return true;
      default:
        return false;
      }
    }
    struct ScopeExit {
      std::function<void()> fn;
      ~ScopeExit() {
        if (fn)
          fn();
      }
    };
    struct Failure {
      std::string message;
      bool unauthorized = false;
      Json mfa;
      long httpStatus = 0;
      int serviceCode = 0;
    };
    struct Transfer {
      std::string body;
      RestRequestScope scope;
      CURL* connection = nullptr;
      CURLM* multi = nullptr;
      const process::WakeEvent* wake = nullptr;
      RestCancellation interrupted = RestCancellation::None;
      const char* label = "account";
      RestRestrictions* restrictions = nullptr;
      std::uint64_t diagnosticId = 0;
      bool retryRateLimits = true;
    };

    size_t receive(char* bytes, size_t size, size_t count, void* context) noexcept {
      auto& transfer = *static_cast<Transfer*>(context);
      const auto length = size * count;
      if (length > 8 * 1024 * 1024 - transfer.body.size())
        return 0;
      try {
        transfer.body.append(bytes, length);
      } catch (...) {
        return 0;
      }
      return length;
    }

    size_t responseHeader(char* bytes, size_t size, size_t count, void* context) noexcept {
      if (size && count > std::numeric_limits<size_t>::max() / size)
        return 0;
      const auto length = size * count;
      try {
        static_cast<RestRateLimits::Headers*>(context)->add(std::string_view(bytes, length));
      } catch (...) {
        return 0;
      }
      return length;
    }

    int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
      auto& transfer = *static_cast<Transfer*>(context);
      const auto reason = transfer.scope.cancellation();
      if (reason != RestCancellation::None)
        transfer.interrupted = reason;
      return reason != RestCancellation::None;
    }

    MessageMime makeStickerMime(CURL* curl, const Json& values) {
      MessageMime mime(curl_mime_init(curl), curl_mime_free);
      if (!mime)
        throw std::runtime_error("Could not prepare the sticker upload");
      for (const auto* name : {"name", "description", "tags"}) {
        if (!values.contains(name) || values[name].is_null())
          continue;
        auto* part = curl_mime_addpart(mime.get());
        const auto& field = values[name].get_ref<const std::string&>();
        if (!part
            || curl_mime_name(part, name) != CURLE_OK
            || curl_mime_data(part, field.data(), field.size()) != CURLE_OK)
          throw std::runtime_error("Could not prepare the sticker upload");
      }
      auto* part = curl_mime_addpart(mime.get());
      const auto& bytes = values["file"].get_binary();
      if (!part
          || curl_mime_name(part, "file") != CURLE_OK
          || curl_mime_filename(part, values["filename"].get_ref<const std::string&>().c_str()) != CURLE_OK
          || curl_mime_type(part, values["content_type"].get_ref<const std::string&>().c_str()) != CURLE_OK
          || curl_mime_data(part, reinterpret_cast<const char*>(bytes.data()), bytes.size()) != CURLE_OK)
        throw std::runtime_error("Could not prepare the sticker upload");
      return mime;
    }

    // All requests are HTTPS to a fixed origin. Never log authorization or response bodies.
    std::expected<Json, Failure> request(
        const std::string& token, const std::string& path, const Json* payload, Transfer& transfer,
        RestRateLimits& cooldown, const std::vector<Upload>& uploads = {}, const char* method = nullptr,
        const GuildSettingsRequest* serverSettings = nullptr, const std::string& mfaAuthorization = {},
        bool accountSettings = false
    ) {
      const bool stickerUpload = serverSettings
          && serverSettings->section == GuildSettingsSection::Stickers
          && serverSettings->verb == GuildSettingsVerb::Create
          && path.ends_with("/stickers");
      if (transfer.scope.foregroundRevision && (payload || (method && std::string_view(method) != "GET")))
        throw std::runtime_error("Only optional reads may yield to user work");
      constexpr int maximumRateLimitAttempts = 3;
      constexpr auto maximumInlineRateLimitWait = std::chrono::seconds(30);
      const std::string_view requestMethod = method      ? method
          : payload || !uploads.empty() || stickerUpload ? "POST"
                                                         : "GET";
      const auto restricted = [&]() {
        return !token.empty() && transfer.restrictions
            ? transfer.restrictions->blocked(transfer.scope.expectedSession, requestMethod, path)
            : std::string{};
      };
      const auto diagnosticId = transfer.diagnosticId = request_diagnostics::nextId();
      for (int attempt = 0; attempt < maximumRateLimitAttempts; ++attempt) {
        using Outcome = request_diagnostics::Outcome;
        const auto started = std::chrono::steady_clock::now();
        request_diagnostics::Record diagnostic{
            .id = diagnosticId, .action = transfer.label, .method = requestMethod, .path = path, .attempt = attempt + 1
        };
        ScopeExit logAttempt{[&] {
          diagnostic.durationMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
          const auto line = request_diagnostics::format(diagnostic);
          if (diagnostic.outcome == Outcome::Success
              || diagnostic.outcome == Outcome::Cancelled
              || diagnostic.outcome == Outcome::ExpectedEmpty)
            restLog.debug("{}", line);
          else if (diagnostic.outcome == Outcome::Setup || diagnostic.outcome == Outcome::Authentication)
            restLog.error("{}", line);
          else
            restLog.warn("{}", line);
        }};
        RestRateLimits::Admission admission;
        while (true) {
          if (progress(&transfer, 0, 0, 0, 0)) {
            diagnostic.outcome = Outcome::Cancelled;
            return std::unexpected(Failure{"Request cancelled"});
          }
          if (auto pause = restricted(); !pause.empty()) {
            diagnostic.outcome = Outcome::Restricted;
            return std::unexpected(Failure{std::move(pause)});
          }
          admission = cooldown.acquire(requestMethod, path);
          if (admission.lease)
            break;
          const auto remaining =
              std::chrono::ceil<std::chrono::milliseconds>(admission.retryAt - std::chrono::steady_clock::now());
          if (remaining > maximumInlineRateLimitWait) {
            diagnostic.outcome = Outcome::RateLimited;
            diagnostic.status = 429;
            return std::unexpected(Failure{"The service is rate limiting requests. Try again later.", false, {}, 429});
          }
          pollfd descriptor{transfer.wake->fd(), POLLIN, 0};
          (void)::poll(&descriptor, 1, static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 1, 86400100)));
          transfer.wake->drain();
        }
        // Reusing the easy handle retains its TLS/HTTP connection and DNS cache.
        auto* curlHandle = transfer.connection;
        if (curlHandle)
          curl_easy_reset(curlHandle);
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> owned(
            curlHandle ? nullptr : curl_easy_init(), curl_easy_cleanup
        );
        if (!curlHandle)
          curlHandle = owned.get();
        struct Handle {
          CURL* value;
          CURL* get() const { return value; }
          explicit operator bool() const { return value; }
        } curl{curlHandle};
        if (!curl)
          return std::unexpected(Failure{"Could not initialize the connection"});
        curl_slist* raw = curl_slist_append(
            nullptr, uploads.empty() && !stickerUpload ? "Content-Type: application/json" : "Accept: application/json"
        );
        if (!raw)
          return std::unexpected(Failure{"Could not allocate request headers"});
        auto* extended = token.empty() ? raw : curl_slist_append(raw, ("Authorization: " + token).c_str());
        if (!extended) {
          curl_slist_free_all(raw);
          return std::unexpected(Failure{"Could not allocate request headers"});
        }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(extended, curl_slist_free_all);
        if (!mfaAuthorization.empty()) {
          auto* next = curl_slist_append(headers.get(), ("X-Discord-MFA-Authorization: " + mfaAuthorization).c_str());
          if (!next)
            return std::unexpected(Failure{"Could not allocate verification headers"});
          (void)headers.release();
          headers.reset(next);
        }
        if (serverSettings && !serverSettings->reason.empty()) {
          std::unique_ptr<char, decltype(&curl_free)> escaped(
              curl_easy_escape(
                  curl.get(), serverSettings->reason.c_str(), static_cast<int>(serverSettings->reason.size())
              ),
              curl_free
          );
          if (!escaped)
            return std::unexpected(Failure{"Could not encode the audit log reason"});
          auto* next = curl_slist_append(headers.get(), (std::string("X-Audit-Log-Reason: ") + escaped.get()).c_str());
          if (!next)
            return std::unexpected(Failure{"Could not allocate request headers"});
          (void)headers.release();
          headers.reset(next);
        }
        const std::string url = "https://discord.com/api/v9" + path;
        const bool sticker = stickerUpload;
        const std::string body = payload && !sticker ? payload->dump() : std::string{};
        transfer.body.clear();
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "cressida/0.1.0");
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 30000L);
        curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
        RestRateLimits::Headers responseHeaders;
        curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, responseHeader);
        curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &responseHeaders);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
        std::unique_ptr<curl_mime, decltype(&curl_mime_free)> mime(nullptr, curl_mime_free);
        if (sticker) {
          mime = makeStickerMime(curl.get(), serverSettings->values);
          curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime.get());
        } else if (!uploads.empty()) {
          mime = makeMessageMime(curl.get(), body, uploads);
          curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime.get());
        } else if (payload) {
          curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
          curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        }
        if (method)
          curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method);
        if (progress(&transfer, 0, 0, 0, 0)) {
          diagnostic.outcome = Outcome::Cancelled;
          return std::unexpected(Failure{"Request cancelled"});
        }
        if (auto pause = restricted(); !pause.empty()) {
          diagnostic.outcome = Outcome::Restricted;
          return std::unexpected(Failure{std::move(pause)});
        }
        const auto code =
            performRestTransfer(curl.get(), transfer.multi, *transfer.wake, transfer.scope, transfer.interrupted);
        network_profile::transferred(transfer.label, curl.get(), code);
        long status = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        diagnostic.status = status;
        diagnostic.transportCode = static_cast<int>(code);
        auto json = decodeRestBody(status, transfer.body);
        diagnostic.serviceCode = restServiceCode(json);
        diagnostic.outcome = transfer.interrupted != RestCancellation::None ? Outcome::Cancelled
            : code != CURLE_OK                                              ? Outcome::Transport
            : status == 429                                                 ? Outcome::RateLimited
            : status == 401                                                 ? Outcome::Authentication
            : status == 403                                                 ? Outcome::Permission
            : status < 200 || status >= 300                                 ? Outcome::Http
            : json.is_discarded()                                           ? Outcome::InvalidResponse
                                                                            : Outcome::Success;
        if (diagnostic.outcome == Outcome::Http
            && request_diagnostics::expectedEmptyCollection(
                requestMethod, path, status, diagnostic.serviceCode, inboxPageSize
            ))
          diagnostic.outcome = Outcome::ExpectedEmpty;
        RestRateLimits::Fallback fallback;
        if (status == 429 && json.is_object()) {
          if (const auto at = json.find("retry_after"); at != json.end() && at->is_number())
            fallback.retryAfter = at->get<double>();
          if (const auto at = json.find("global"); at != json.end() && at->is_boolean())
            fallback.global = at->get<bool>();
        }
        const auto retryAt = admission.lease.complete(status, responseHeaders, fallback);
        if (code != CURLE_OK)
          return std::unexpected(
              Failure{
                  !token.empty() && (payload || (method && std::string_view(method) != "GET"))
                      ? (method ? "The update could not be confirmed. Check your settings before retrying."
                                : "Message delivery could not be confirmed. Check the conversation before retrying.")
                      : "Could not reach Discord. Try again."
              }
          );
        if (progress(&transfer, 0, 0, 0, 0)) {
          diagnostic.outcome = Outcome::Cancelled;
          return std::unexpected(Failure{"Request cancelled"});
        }
        const auto serviceCode = restServiceCode(json);
        if (!token.empty() && transfer.restrictions) {
          auto pause =
              transfer.restrictions->observe(transfer.scope.expectedSession, requestMethod, path, status, json);
          if (!pause.empty()) {
            diagnostic.outcome = Outcome::Restricted;
            return std::unexpected(Failure{std::move(pause), false, {}, status, serviceCode});
          }
        }
        if (status == 429) {
          if (!transfer.retryRateLimits)
            return std::unexpected(
                Failure{"The preview update was rate limited. A later fresh preview can update it.", false, {}, status, serviceCode}
            );
          if (retryAt - std::chrono::steady_clock::now() > maximumInlineRateLimitWait)
            return std::unexpected(
                Failure{"The service is rate limiting requests. Try again later.", false, {}, status, serviceCode}
            );
          diagnostic.retry = attempt + 1 < maximumRateLimitAttempts;
          continue;
        }
        if ((serverSettings || accountSettings)
            && !json.is_discarded()
            && json.is_object()
            && json.contains("code")
            && json["code"].is_number_integer()
            && (json["code"] == 60003 || json["code"] == 60008 || json["code"] == 60009)) {
          diagnostic.outcome = Outcome::Restricted;
          return std::unexpected(
              Failure{
                  "The service requires two-factor verification for this action.", false, json.value("mfa", Json{}),
                  status, serviceCode
              }
          );
        }
        if (status == 401 && !token.empty())
          return std::unexpected(
              Failure{"The account token was rejected. Reconnect with a valid token.", true, {}, status, serviceCode}
          );
        if ((status < 200 || status >= 300) && token.empty())
          return std::unexpected(Failure{"Discord could not complete browser sign-in. Please try again."});
        if (status == 403)
          return std::unexpected(
              Failure{
                  "You do not have permission to access this channel or perform this action.",
                  false,
                  {},
                  status,
                  serviceCode
              }
          );

        if (status < 200 || status >= 300) {
          return std::unexpected(
              Failure{
                  "The service returned an error (HTTP "
                      + std::to_string(status)
                      + ", code "
                      + std::to_string(serviceCode)
                      + ").",
                  false,
                  {},
                  status,
                  serviceCode
              }
          );
        }
        if (json.is_discarded())
          return std::unexpected(Failure{"Discord returned an unreadable response."});
        return json;
      }
      return std::unexpected(Failure{"The service is rate limiting requests. Try again later.", false, {}, 429});
    }
  } // namespace

  Client::Client(
      std::unique_ptr<auth::CredentialBackend> credentials, std::filesystem::path disabledLogin,
      std::shared_ptr<voice::Audio> audio
  ) {
    m_credentials = std::make_unique<auth::CredentialStore>(std::move(credentials), std::move(disabledLogin));
    // libcurl initialization is process-wide; this static lives beyond every client worker.
    struct CurlRuntime {
      CurlRuntime() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
          throw std::runtime_error("libcurl initialization failed");
      }
      ~CurlRuntime() { curl_global_cleanup(); }
    };
    static CurlRuntime runtime;
    m_transportCache = std::make_unique<RestTransportCache>();
    if (!m_wake.valid() || !m_requestWake.valid())
      throw std::runtime_error("Could not create the network event pipe");
    m_gateway = std::make_unique<Gateway>(m_session, [this](auto session, const auto& type, const auto& data) {
      gatewayEvent(session, type, data);
    });
    m_voiceAudio = audio ? std::move(audio) : voice::makeAudio();
    m_streams=std::make_unique<StreamManager>(
        [this](std::uint64_t session,StreamCommand command) { return m_gateway->stream(session,std::move(command)); },
        [this](StreamPreviewQuery query) { return streamPreview(std::move(query)); }
    );
    m_voice = std::make_unique<voice::Connection>(m_voiceAudio, [this](voice::State state) {
      if (!state.generation || state.generation != m_session || state.revision != m_voiceRevision)
        return;
      publish(Event{.operation = Operation::Voice, .session = state.generation, .voice = std::move(state)});
    });
    for (std::size_t lane = 0; lane < m_readWake.size(); ++lane) {
      if (!m_readWake[lane].valid())
        throw std::runtime_error("Could not create a read event pipe");
      m_readWorkers.emplace_back([this, lane](std::stop_token stop) { runReads(stop, lane); });
    }
    m_worker = std::jthread([this](std::stop_token stop) { run(stop); });
  }

  Client::~Client() {
    ++m_session;
    resetVoice(false);
    m_voice.reset();
    m_streams.reset();
    m_worker.request_stop();
    for (auto& worker : m_readWorkers)
      worker.request_stop();
    wakeRequests();
    m_credentials->shutdown();
    m_ready.notify_all();
    if (m_worker.joinable())
      m_worker.join();
    m_readWorkers.clear();
    m_gateway.reset();
  }

  void Client::wakeRequests() {
    (void)m_requestWake.signal();
    for (const auto& wake : m_readWake)
      (void)wake.signal();
    m_ready.notify_all();
  }

  std::uint64_t Client::connect(std::string token) { return beginConnect(std::move(token), {}); }

  std::uint64_t Client::connectHandoff(std::string key, std::string handoffToken) {
    return beginConnect(std::move(handoffToken), std::move(key));
  }

  std::uint64_t Client::beginConnect(std::string credential, std::optional<std::string> handoffKey) {
    m_credentials->cancelLoad();
    const auto session = ++m_session;
    resetVoice(false);
    m_restRestrictions.reset(session);
    m_authenticatedSession = 0;
    m_restoringSession = 0;
    m_gateway->reset();
    {
      std::lock_guard lock(m_mutex);
      m_commands.clear();
      m_preloadReads.clear();
      m_readToken.clear();
      m_authenticatedUser.clear();
      m_bootstrap.cancel();
      m_selectedMemberIdentity.reset();
      m_events.clear();
      m_commands.push_back({Operation::Connect, session, {}, std::move(credential), std::move(handoffKey)});
    }
    m_ready.notify_all();
    wakeRequests();
    return session;
  }

  std::uint64_t Client::disconnect() {
    m_credentials->cancelLoad();
    const auto session = ++m_session;
    resetVoice(false);
    m_restRestrictions.reset(session);
    m_authenticatedSession = 0;
    m_restoringSession = 0;
    m_gateway->reset();
    std::lock_guard lock(m_mutex);
    m_commands.clear();
    m_preloadReads.clear();
    m_readToken.clear();
    m_authenticatedUser.clear();
    m_bootstrap.cancel();
    m_selectedMemberIdentity.reset();
    m_events.clear();
    // Wake the worker to clear its credential even when no further commands arrive.
    m_ready.notify_all();
    wakeRequests();
    return session;
  }

  std::uint64_t Client::restoreLogin() {
    const auto session = disconnect();
    m_restoringSession = session;
    m_credentials->load(session, [this](auth::CredentialResult result) {
      if (!result.found || !result.error.empty()) {
        Event event{.operation = Operation::Connect, .session = result.request};
        event.error = std::move(result.error);
        event.noSavedLogin = event.error.empty();
        event.restoredLogin = true;
        publish(std::move(event));
        return;
      }
      {
        std::lock_guard lock(m_mutex);
        if (result.request != m_session)
          return;
        Command command{Operation::Connect, result.request, {}, std::move(result.token)};
        command.restoredLogin = true;
        m_commands.push_back(std::move(command));
      }
      m_ready.notify_all();
      wakeRequests();
    });
    return session;
  }
  void Client::saveLogin(std::uint64_t session, const std::string& token) {
    std::lock_guard lock(m_mutex);
    if (session != m_session)
      return;
    m_credentials->save(session, token, [this](auth::CredentialResult result) {
      Event event{.operation = Operation::Credentials, .session = result.request};
      event.credentialOperation = result.operation;
      event.error = std::move(result.error);
      publish(std::move(event));
    });
  }
  void Client::forgetLogin(std::uint64_t session) {
    std::lock_guard lock(m_mutex);
    if (session != m_session)
      return;
    m_credentials->forget(session, [this](auth::CredentialResult result) {
      Event event{.operation = Operation::Credentials, .session = result.request};
      event.credentialOperation = result.operation;
      event.error = std::move(result.error);
      publish(std::move(event));
    });
  }

  void Client::queueBootstrap(std::uint64_t session, bool guilds, bool settings) {
    std::lock_guard lock(m_mutex);
    if (session != m_session || session != m_authenticatedSession)
      return;
    const auto queued = [&](Operation operation) {
      return std::ranges::any_of(m_commands, [&](const auto& pending) {
        return pending.session == session && pending.operation == operation;
      });
    };
    if (guilds && m_guildsReadySession != session && !queued(Operation::Guilds)) {
      Command command{Operation::Guilds, session, {}, {}};
      command.background = true;
      m_commands.push_back(std::move(command));
    }
    if (settings && m_bootstrapSettingsReadySession != session && !queued(Operation::BootstrapSettings)) {
      Command command{Operation::BootstrapSettings, session, {}, {}};
      command.background = true;
      command.presenceRevision = m_presenceRevision;
      m_commands.push_back(std::move(command));
    }
    wakeRequests();
  }

  bool Client::completeConnect(Event event, BootstrapSource source) {
    std::string rememberToken;
    bool resetGateway = false, forget = false, authenticated = false;
    const auto session = event.session;
    {
      std::lock_guard lock(m_mutex);
      if (session != m_session)
        return true;
      if (!m_bootstrap.pending(session))
        return source == BootstrapSource::Rest && m_bootstrap.resolved() == session;
      const auto resolution =
          event.error.empty() ? m_bootstrap.succeed(session) : m_bootstrap.fail(session, source, event.unauthorized);
      if (!resolution)
        return true; // The other authentication path can still succeed.
      authenticated = resolution->authenticated;
      event.restoredLogin = resolution->restored;
      event.unauthorized = resolution->unauthorized;
      event.live = authenticated && source == BootstrapSource::Gateway;
      if (authenticated) {
        m_authenticatedUser = event.account.id;
        m_authenticatedSession = session;
        if (!resolution->restored)
          rememberToken = m_readToken;
      } else {
        m_authenticatedSession = 0;
        m_authenticatedUser.clear();
        m_readToken.clear();
        resetGateway = true;
        forget = event.unauthorized && resolution->restored;
      }
      // The same mutex guards Gateway publication: no live account snapshot
      // can overtake the single authentication completion.
      if (m_events.size() >= 2048)
        m_events.erase(m_events.begin());
      m_events.push_back(std::move(event));
      (void)m_wake.signal();
    }
    wakeRequests(); // Cancel the losing /users/@me transfer immediately.
    if (resetGateway)
      m_gateway->reset(session);
    if (forget)
      forgetLogin(session);
    if (!rememberToken.empty())
      saveLogin(session, rememberToken);
    if (authenticated && source == BootstrapSource::Rest)
      queueBootstrap(session, true, true);
    return true;
  }

  std::uint64_t Client::logout() {
    const auto session = disconnect();
    forgetLogin(session);
    return session;
  }

  bool Client::enqueue(Command command) {
    if (command.session == 0 || m_authenticatedSession != command.session)
      return false;
    command.background = command.operation == Operation::ChannelDetails
        || command.operation == Operation::PrivateChannels
        || command.operation == Operation::StreamPreviewRead
        || command.operation == Operation::StreamPreviewWrite
        || (command.operation == Operation::Messages && command.history && command.history->preload);
    if ((command.operation == Operation::Channels
         || command.operation == Operation::Messages
         || command.operation == Operation::Send)
        && !validSnowflake(command.resource))
      return false;
    std::lock_guard lock(m_mutex);
    if (!command.background) {
      bool cancelled = false;
      std::erase_if(m_commands, [&](const auto& pending) {
        if (!pending.history
            || !pending.history->preload
            || (pending.readIntent && pending.readIntent->foreground.load()))
          return false;
        Event event{
            .operation = Operation::Messages,
            .session = pending.session,
            .resource = pending.resource,
            .error = "Chat preload cancelled for foreground work."
        };
        event.history = pending.history;
        if (m_events.size() >= 2048)
          m_events.erase(m_events.begin());
        m_events.push_back(std::move(event));
        cancelled = true;
        return true;
      });
      if (cancelled)
        (void)m_wake.signal();
    }
    if (m_commands.size() >= 32)
      return false;
    if (command.operation == Operation::Messages && command.history && !command.history->preload) {
      command.readRevision = ++m_historyRevision;
      std::erase_if(m_commands, [](const auto& pending) {
        return pending.operation == Operation::Messages && pending.history && !pending.history->preload;
      });
    }
    if (command.operation == Operation::Messages && command.history && command.history->preload) {
      command.readIntent = std::make_shared<RestReadIntent>();
      std::erase_if(m_preloadReads, [](const auto& pending) { return pending.intent.expired(); });
      m_preloadReads.push_back({command.session, command.resource, *command.history, command.readIntent});
    }
    if (command.operation == Operation::Acknowledge) {
      // A burst of live messages only needs the newest acknowledgement per channel.
      for (auto& pending : m_commands)
        if (pending.operation == command.operation && pending.resource == command.resource) {
          pending.content = std::move(command.content);
          return true;
        }
    }
    if (command.operation == Operation::SavePresence) {
      for (const auto& pending : m_commands)
        if (pending.operation == Operation::SavePresence && !command.customStatus && pending.customStatus)
          command.customStatus = pending.customStatus;
      std::erase_if(m_commands, [](const auto& pending) { return pending.operation == Operation::SavePresence; });
    }
    if (command.operation == Operation::GifSearch)
      std::erase_if(m_commands, [](const auto& pending) { return pending.operation == Operation::GifSearch; });
    // Every accepted settings request receives its own scoped completion. The UI
    // rejects stale search identities without leaving dropped requests pending.
    if (command.operation != Operation::Send
        && command.operation != Operation::Interaction
        && command.operation != Operation::Threads
        && command.operation != Operation::InboxRead
        && command.operation != Operation::InboxWrite
        && command.operation != Operation::StreamPreviewRead
        && command.operation != Operation::StreamPreviewWrite
        && command.operation != Operation::ChannelPage
        && command.operation != Operation::GuildSettings
        && command.operation != Operation::UserSettings
        && command.operation != Operation::CommandIndex
        && command.operation != Operation::SaveGifFavorite
        && command.operation != Operation::SaveEmojiFavorite
        && std::ranges::any_of(m_commands, [&](const auto& pending) {
             return pending.operation == command.operation
                 && pending.resource == command.resource
                 && (command.operation != Operation::Messages || pending.history == command.history);
           }))
      return true;
    m_commands.push_back(std::move(command));
    if (!m_commands.back().background) {
      ++m_foregroundRevision;
      wakeRequests();
    }
    m_ready.notify_all();
    return true;
  }

  bool Client::channels(const std::string& id) { return enqueue({Operation::Channels, m_session, id, {}}); }
  bool Client::channelDetails(const std::string& channel) {
    return validSnowflake(channel) && enqueue({Operation::ChannelDetails, m_session, channel, {}});
  }
  bool Client::applicationCommands(CommandIndexQuery query) {
    if (commandIndexPath(query).empty())
      return false;
    Command command{Operation::CommandIndex, m_session, query.channel, {}};
    command.commandIndex = std::move(query);
    return enqueue(std::move(command));
  }

  bool Client::inbox(InboxQuery query) {
    const auto route = inboxHttpRequest(query);
    if (!route)
      return false;
    Command command{
        route->method == "GET" ? Operation::InboxRead : Operation::InboxWrite, m_session, query.channel, {}
    };
    command.inboxQuery = std::move(query);
    return enqueue(std::move(command));
  }
  bool Client::streamPreview(StreamPreviewQuery query) {
    if (!query.generation)
      query.generation = m_session.load();
    if (query.generation != m_session || query.generation != m_authenticatedSession || streamPreviewRevoked(query))
      return false;
    if (query.kind == StreamPreviewKind::Upload) {
      std::lock_guard lock(m_mutex);
      if (query.key.owner != m_authenticatedUser)
        return false;
    }
    const auto version = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (version <= 0 || !streamPreviewHttpRequest(query,static_cast<std::uint64_t>(version)))
      return false;
    Command command{query.kind == StreamPreviewKind::Read ? Operation::StreamPreviewRead : Operation::StreamPreviewWrite,
        query.generation,query.key.value(),{}};
    command.streamPreview=std::move(query);
    return enqueue(std::move(command));
  }
  bool Client::channelPage(ChannelPageQuery query) {
    if (channelPagePath(query).empty())
      return false;
    Command command{Operation::ChannelPage, m_session, query.channel, {}};
    command.channelQuery = std::move(query);
    return enqueue(std::move(command));
  }
  bool Client::guildSettings(GuildSettingsRequest requestData) {
    if (!guildSettingsHttpRequest(requestData))
      return false;
    Command command{Operation::GuildSettings, m_session, requestData.guildId, {}};
    command.guildSettings = std::move(requestData);
    return enqueue(std::move(command));
  }
  bool Client::userSettings(UserSettingsRequest requestData) {
    const bool verification = requestData.mfaChallenge
        && (requestData.mfaType == "totp" || requestData.mfaType == "password")
        && !requestData.mfaCode.empty()
        && requestData.mfaCode.size() <= 1024;
    if (!verification && !userSettingsHttpRequest(requestData))
      return false;
    Command command{Operation::UserSettings, m_session, requestData.resourceId, {}};
    command.userSettings = std::move(requestData);
    return enqueue(std::move(command));
  }
  void Client::clearUserSettingsSecrets() {
    {
      std::lock_guard lock(m_mutex);
      ++m_userSettingsSecretsEpoch;
    }
    m_ready.notify_all();
  }
  bool Client::threads(ThreadQuery query) {
    if (threadListPath(query).empty())
      return false;
    Command command{Operation::Threads, m_session, query.guild, {}};
    command.threadQuery = std::move(query);
    return enqueue(std::move(command));
  }
  bool Client::privateChannels() { return enqueue({Operation::PrivateChannels, m_session, {}, {}}); }
  bool Client::messages(const std::string& id, HistoryQuery query) {
    if (messageHistoryPath(id, query).empty() || (query.preload && query.direction != HistoryDirection::Latest))
      return false;
    Command command{Operation::Messages, m_session, id, {}};
    command.history = std::move(query);
    return enqueue(std::move(command));
  }
  bool Client::promotePreload(const std::string& channel, const HistoryQuery& query) {
    if (!query.preload || query.direction != HistoryDirection::Latest || !validSnowflake(channel))
      return false;
    std::lock_guard lock(m_mutex);
    const auto session = m_session.load();
    if (!session || m_authenticatedSession != session || m_selectedChannel != channel)
      return false;
    for (const auto& event : m_events)
      if (event.session == session
          && event.operation == Operation::Messages
          && event.resource == channel
          && event.history == query)
        return event.error.empty() && !event.unauthorized;
    std::erase_if(m_preloadReads, [](const auto& pending) { return pending.intent.expired(); });
    const auto found = std::ranges::find_if(m_preloadReads, [&](const auto& pending) {
      return pending.session == session && pending.channel == channel && pending.query == query;
    });
    if (found == m_preloadReads.end())
      return false;
    const auto intent = found->intent.lock();
    if (!intent || intent->cancelled)
      return false;
    const auto navigation = m_historyRevision.load();
    intent->promote(navigation);
    for (auto& pending : m_commands)
      if (pending.readIntent == intent) {
        pending.background = false;
        pending.readRevision = navigation;
      }
    ++m_foregroundRevision;
    wakeRequests();
    return true;
  }
  bool Client::send(
      const std::string& id, std::string content, std::vector<Upload> uploads, std::optional<MessageReference> reply,
      std::vector<std::string> stickers, std::string nonce
  ) {
    if (!validSnowflake(id)
        || (!nonce.empty() && !validSnowflake(nonce))
        || !validStickerIds(stickers)
        || (content.empty() && uploads.empty() && stickers.empty())
        || content.size() > 8000
        || utf8Length(content) > 2000
        || !validUploads(uploads)
        || (reply && (!validSnowflake(reply->message) || reply->channel != id)))
      return false;
    Command command{Operation::Send, m_session, id, std::move(content), {}, std::move(uploads)};
    command.reply = std::move(reply);
    command.stickers = std::move(stickers);
    command.nonce = std::move(nonce);
    return enqueue(std::move(command));
  }
  bool Client::interact(api::Interaction action) {
    if (!interactionRequest(action))
      return false;
    if (action.kind == InteractionKind::ApplicationCommand
        && !validCommandAttachments(*action.command, action.values, action.commandUploads))
      return false;
    Command command{Operation::Interaction, m_session, action.channel, {}};
    if (!action.commandUploads.empty()) {
      if (action.kind != InteractionKind::ApplicationCommand || !validCommandUploads(action.commandUploads))
        return false;
      command.uploads = std::move(action.commandUploads);
      action.commandUploads.clear();
    }
    command.interaction = std::move(action);
    return enqueue(std::move(command));
  }

  bool Client::accountSettings() { return enqueue({Operation::AccountSettings, m_session, {}, {}}); }
  bool Client::searchGifs(const std::string& query) {
    if (query.size() > 256)
      return false;
    return enqueue({Operation::GifSearch, m_session, query, {}});
  }
  bool Client::gifFavorites() { return enqueue({Operation::GifFavorites, m_session, {}, {}}); }
  bool Client::guildStickers(const std::string& guild) {
    return validSnowflake(guild) && enqueue({Operation::GuildStickers, m_session, guild, {}});
  }
  bool Client::stickerPacks() { return enqueue({Operation::StickerPacks, m_session, {}, {}}); }
  bool Client::sticker(const std::string& id) {
    return validSnowflake(id) && enqueue({Operation::StickerDetails, m_session, id, {}});
  }
  bool Client::guildEmojis(const std::string& guild) {
    return validSnowflake(guild) && enqueue({Operation::GuildEmojis, m_session, guild, {}});
  }
  bool Client::emojiPreferences() { return enqueue({Operation::EmojiPreferences, m_session, {}, {}}); }
  bool Client::saveEmojiFavorite(const std::string& key, bool add) {
    return validEmojiKey(key) && enqueue({Operation::SaveEmojiFavorite, m_session, key, add ? "add" : "remove"});
  }
  bool Client::saveGifFavorite(const Gif& gif, bool add) {
    if (!validGif(gif))
      return false;
    auto value = gifJson(gif);
    value["add"] = add;
    return enqueue({Operation::SaveGifFavorite, m_session, gif.url, value.dump()});
  }
  bool Client::saveAccountSettings(SettingValues changes) {
    if (!validSettingChanges(changes))
      return false;
    return enqueue({Operation::SaveAccountSettings, m_session, {}, Json(changes).dump()});
  }
  bool Client::settingsList(const std::string& section) {
    if (section != "connections" && section != "authorized" && section != "devices")
      return false;
    return enqueue({Operation::SettingsList, m_session, section, {}});
  }
  bool Client::acknowledge(const std::string& channel, const std::string& message) {
    if (!validSnowflake(channel) || !validSnowflake(message))
      return false;
    return enqueue({Operation::Acknowledge, m_session, channel, message});
  }
  bool Client::profile() { return enqueue({Operation::Profile, m_session, {}, {}}); }
  bool Client::saveProfile(const Account& profile) {
    if (utf8Length(profile.displayName) > 32 || utf8Length(profile.bio) > 190 || utf8Length(profile.pronouns) > 40)
      return false;
    Json body{
        {"global_name", profile.displayName.empty() ? Json(nullptr) : Json(profile.displayName)},
        {"bio", profile.bio},
        {"pronouns", profile.pronouns}
    };
    return enqueue({Operation::SaveProfile, m_session, {}, body.dump()});
  }
  bool Client::setPresence(std::string status, std::optional<std::string> customText) {
    if (!validStatus(status) || (customText && utf8Length(*customText) > 128))
      return false;
    Command command{Operation::SavePresence, m_session, status, {}};
    command.customStatus = customText;
    command.presenceRevision = ++m_presenceRevision;
    if (!enqueue(std::move(command)))
      return false;
    m_gateway->setPresence(m_session, std::move(status), std::move(customText));
    return true;
  }

  void Client::subscribe(const std::string& guild, const std::string& channel, int first) {
    std::optional<MemberListIdentity> identity;
    {
      std::lock_guard lock(m_mutex);
      if (m_selectedGuild == guild && m_selectedChannel == channel)
        identity = m_selectedMemberIdentity;
    }
    subscribe(guild, channel, first, std::move(identity));
  }

  void Client::subscribe(
      const std::string& guild, const std::string& channel, int first, std::optional<MemberListIdentity> identity
  ) {
    if (m_authenticatedSession != m_session
        || (!guild.empty() && !validSnowflake(guild))
        || (channel.empty() ? !guild.empty() : !validSnowflake(channel)))
      return;
    const bool thread = identity && identity->thread;
    if (thread)
      first = 0;
    bool refreshIdentity = false;
    {
      std::lock_guard lock(m_mutex);
      const bool changed = m_selectedGuild != guild || m_selectedChannel != channel;
      refreshIdentity = !changed && identity != m_selectedMemberIdentity;
      if (changed)
        ++m_historyRevision;
      m_selectedMemberIdentity = std::move(identity);
      m_selectedGuild = guild;
      m_selectedChannel = channel;
      m_selectedMemberFirst = first;
    }
    wakeRequests();
    m_gateway->subscribe(guild, channel, first, refreshIdentity, thread);
  }

  void Client::requestMembers(const std::string& guild, const std::vector<std::string>& users) {
    if (m_authenticatedSession == m_session && !users.empty())
      m_gateway->requestMembers(m_session, guild, users);
  }

  bool Client::searchMembers(const std::string& guild, const std::string& query, const std::string& nonce) {
    return m_authenticatedSession == m_session && m_gateway->searchMembers(m_session, guild, query, nonce);
  }
  void Client::gatewayEvent(std::uint64_t session, const std::string& type, const Json& data) {
    if (session != m_session)
      return;
    if (type.starts_with("STREAM_")) {
      m_streams->event(session,type,data);
      return;
    }
    if (type=="READY" && data.is_object() && data.contains("streams") && data["streams"].is_array())
      for (const auto& stream : data["streams"])
        m_streams->event(session,"STREAM_CREATE",stream);
    if (type == "VOICE_STATE_UPDATE" || type == "VOICE_SERVER_UPDATE") {
      if (type == "VOICE_STATE_UPDATE")
        voiceRosterEvent(session, type, data);
      voiceEvent(session, type, data);
      return;
    }
    if (type == "RECONNECTING" || type == "CONNECTION_FAILED")
      leaveVoice(session);
    bool readyBatch = false;
    ScopeExit finishReady{[this, session, &readyBatch] {
      if (readyBatch && m_authenticatedSession == session) {
        queueBootstrap(session, m_guildsReadySession != session, m_bootstrapSettingsReadySession != session);
        // Also finish a partial batch when optional READY metadata is malformed;
        // missing snapshots then use the ordinary bounded REST fallbacks.
        publish(Event{.operation = Operation::Gateway, .session = session, .live = true});
      }
    }};
    try {
      if (type == "READY" || type == "USER_REQUIRED_ACTION_UPDATE") {
        const auto action = data.find("required_action");
        const bool required =
            action != data.end() && action->is_string() && !action->get_ref<const std::string&>().empty();
        if (required)
          accountLog.warn("Verification state received required_action_present=true");
        else if (
            type == "USER_REQUIRED_ACTION_UPDATE" && action != data.end() && (action->is_null() || action->is_string())
        )
          accountLog.info("Verification state updated required_action_present=false");
      }
      if (type == "READY") {
        if (auto notice = m_restRestrictions.requiredAction(session, data))
          publish(Event{.operation = Operation::AccountRestriction, .session = session, .error = std::move(*notice)});
      }
      if (type == "USER_REQUIRED_ACTION_UPDATE") {
        if (auto notice = m_restRestrictions.requiredAction(session, data)) {
          publish(Event{.operation = Operation::AccountRestriction, .session = session, .error = std::move(*notice)});
          wakeRequests();
        }
        return;
      }

      if (type == "SAVED_MESSAGE_CREATE" || type == "SAVED_MESSAGE_DELETE" || type == "RECENT_MENTION_DELETE") {
        Event event{.operation = Operation::InboxUpdate, .session = session};
        InboxQuery query;
        query.kind = type == "SAVED_MESSAGE_CREATE" ? InboxKind::Save
            : type == "SAVED_MESSAGE_DELETE"        ? InboxKind::Remove
                                                    : InboxKind::Dismiss;
        query.channel = data.value("channel_id", "");
        query.message = data.value("message_id", "");
        if (query.kind == InboxKind::Save) {
          // Creation is a single saved record, independently of its tab.
          event.inboxPage = InboxPage{};
          event.inboxPage->saved.push_back(parseSavedMessage(data));
          query.channel = event.inboxPage->saved.front().channel;
          query.message = event.inboxPage->saved.front().id;
        }
        if (!validSnowflake(query.message))
          return;
        event.inboxQuery = std::move(query);
        publish(std::move(event));
        return;
      }
      if (type == "READY") {
        Event connected{.operation = Operation::Connect, .session = session};
        if (!data.contains("user") || !data["user"].is_object())
          return;
        connected.account = parseAccount(data["user"]);
        if (!validSnowflake(connected.account.id))
          return;
        completeConnect(std::move(connected), BootstrapSource::Gateway);
        if (m_authenticatedSession != session)
          return;
        readyBatch = true;
        m_memberStream.clear();
        m_memberList = {};
      } else if (type == "RECONNECTING" || type == "CONNECTION_FAILED") {
        Event failed{.operation = Operation::Connect, .session = session};
        failed.unauthorized = type == "CONNECTION_FAILED" && data.value("authentication_failed", false);
        failed.error = failed.unauthorized ? "The account token was rejected. Reconnect with a valid token."
                                           : "Could not establish the live connection. Reconnect or try again.";
        if (completeConnect(std::move(failed), BootstrapSource::Gateway) && m_authenticatedSession != session)
          return;
      }
      if (m_authenticatedSession != session)
        return;
      if (type == "READY"
          || type == "READY_SUPPLEMENTAL"
          || type == "GUILD_CREATE"
          || type == "GUILD_DELETE"
          || type == "CHANNEL_DELETE"
          || type == "GUILD_MEMBER_REMOVE"
          || type == "GUILD_MEMBER_UPDATE")
        voiceRosterEvent(session, type, data);
      if (m_memberSession != session) {
        m_memberSession = session;
        m_memberStream.clear();
        m_memberList = {};
        m_roleNames.clear();
        m_users.clear();
        m_readState = {};
        m_userDirectory = {};
        m_self.clear();
      }
      std::string memberGuild, memberChannel;
      std::optional<MemberListIdentity> memberIdentity;
      std::optional<std::vector<MemberRow>> threadMembers;
      int memberFirst = 0;
      if (type == "GUILD_MEMBER_LIST_UPDATE" || type == "THREAD_MEMBER_LIST_UPDATE") {
        {
          std::lock_guard lock(m_mutex);
          memberGuild = m_selectedGuild;
          memberChannel = m_selectedChannel;
          memberIdentity = m_selectedMemberIdentity;
          memberFirst = m_selectedMemberFirst;
        }
        if (memberGuild.empty() || memberChannel.empty())
          return;
        const bool thread = memberIdentity && memberIdentity->thread;
        if (thread != (type == "THREAD_MEMBER_LIST_UPDATE"))
          return;
        if (thread) {
          threadMembers = threadMemberList(data, memberGuild, memberChannel);
          if (!threadMembers)
            return;
        } else {
          if (!m_memberStream.select(memberGuild, memberChannel, memberIdentity))
            m_memberList = {};
          if (!m_memberStream.accepts(data, memberFirst))
            return;
        }
      }
      if (type == "READY" && data.contains("user"))
        m_self = data["user"].value("id", "");
      if (type == "THREAD_MEMBERS_UPDATE") {
        std::lock_guard lock(m_mutex);
        if (m_selectedMemberIdentity
            && m_selectedMemberIdentity->thread
            && data.value("guild_id", "") == m_selectedGuild
            && data.value("id", "") == m_selectedChannel)
          m_gateway->subscribe(m_selectedGuild, m_selectedChannel, 0, true, true);
      }
      if (type == "READY") {
        const auto at = data.find("session_id");
        std::lock_guard lock(m_mutex);
        m_gatewaySessionId = at != data.end() && at->is_string() && at->get_ref<const std::string&>().size() <= 128
            ? at->get<std::string>()
            : "";
        m_gatewaySessionGeneration = session;
        m_authSessionIdHash.clear();
        m_authTotpEnabled.reset();
        if (data.contains("auth")
            && data["auth"].is_object()
            && data["auth"].contains("authenticator_types")
            && data["auth"]["authenticator_types"].is_array())
          m_authTotpEnabled =
              std::ranges::any_of(data["auth"]["authenticator_types"], [](const auto& type) { return type == 2; });
        if (data.contains("auth_session_id_hash")
            && data["auth_session_id_hash"].is_string()
            && data["auth_session_id_hash"].get_ref<const std::string&>().size() <= 256)
          m_authSessionIdHash = data["auth_session_id_hash"].get<std::string>();
      }
      if (type == "AUTH_SESSION_CHANGE"
          && data.contains("auth_session_id_hash")
          && data["auth_session_id_hash"].is_string()) {
        const auto hash = data["auth_session_id_hash"].get<std::string>();
        if (!hash.empty() && hash.size() <= 256) {
          std::lock_guard lock(m_mutex);
          m_authSessionIdHash = hash;
        }
      }
      if (type == "GUILD_BULK_BAN_UPDATE" || type == "GUILD_PRUNE_UPDATE") {
        const auto guild = data.value("guild_id", std::string{});
        const bool bans = type == "GUILD_BULK_BAN_UPDATE";
        const auto field = bans ? "bulk_ban" : "prune";
        if (validSnowflake(guild) && data.contains(field) && data[field].is_object()) {
          const auto& payload = data[field];
          Json result = Json::object();
          if (bans) {
            for (const auto key : {"banned_users", "failed_users"}) {
              if (!payload.contains(key) || !payload[key].is_array() || payload[key].size() > 2000)
                throw std::runtime_error("Invalid bulk moderation completion");
              result[key] = Json::array();
              for (const auto& user : payload[key]) {
                if (!user.is_string() || !validSnowflake(user.get<std::string>()))
                  throw std::runtime_error("Invalid bulk moderation member");
                result[key].push_back(user);
              }
            }
            result["complete"] = true;
          } else {
            for (const auto key : {"is_preview", "is_finished"}) {
              if (payload.contains(key) && payload[key].is_boolean())
                result[key] = payload[key];
            }
            for (const auto key : {"days", "prune_count"}) {
              if (payload.contains(key) && payload[key].is_number_integer() && payload[key].get<std::int64_t>() >= 0)
                result[key] = payload[key];
            }
            if (payload.contains("include_roles")
                && payload["include_roles"].is_array()
                && payload["include_roles"].size() <= 100) {
              result["include_roles"] = Json::array();
              for (const auto& role : payload["include_roles"])
                if (role.is_string() && validSnowflake(role.get<std::string>()))
                  result["include_roles"].push_back(role);
            }
            result["complete"] = result.value("is_finished", false);
          }
          Event event{.operation = Operation::GuildSettings, .session = session, .resource = guild, .live = true};
          event.guildSettingsRequest = GuildSettingsRequest{
              .section = bans ? GuildSettingsSection::BulkBans : GuildSettingsSection::Prune, .guildId = guild
          };
          event.guildSettings = std::move(result);
          publish(std::move(event));
        }
      }
      if (type == "INTERACTION_SUCCESS" || type == "INTERACTION_FAILURE" || type == "INTERACTION_MODAL_CREATE") {
        Event result{.operation = Operation::ComponentResult, .session = session};
        if (data.contains("nonce") && data["nonce"].is_string())
          result.nonce = data["nonce"].get<std::string>();
        if (type == "INTERACTION_FAILURE")
          result.error = "The application could not complete this action.";
        if (type == "INTERACTION_MODAL_CREATE") {
          try {
            result.modal = parseApplicationModal(data);
          } catch (const std::exception& error) {
            result.error = error.what();
          }
        }
        publish(std::move(result));
      }
      if (type == "APPLICATION_COMMAND_AUTOCOMPLETE_RESPONSE") {
        Event result{.operation = Operation::CommandChoices, .session = session};
        if (data.contains("nonce") && data["nonce"].is_string() && validSnowflake(data["nonce"].get<std::string>())) {
          result.nonce = data["nonce"].get<std::string>();
          try {
            result.commandChoices = parseCommandChoices(data.at("choices"));
          } catch (const std::exception&) {
            result.error = "The application returned invalid suggestions.";
          }
          publish(std::move(result));
        }
      }
      if (type == "GUILD_APPLICATION_COMMAND_INDEX_UPDATE"
          || type == "APPLICATION_COMMAND_PERMISSIONS_UPDATE"
          || type == "USER_APPLICATION_UPDATE"
          || type == "USER_APPLICATION_REMOVE") {
        const auto guild = data.value("guild_id", std::string{});
        if (guild.empty() || validSnowflake(guild))
          publish(Event{.operation = Operation::CommandIndex, .session = session, .resource = guild, .live = true});
      }
      if (type == "CHANNEL_DELETE" && data.contains("guild_id")) {
        const auto guild = data.value("guild_id", ""), channel = data.value("id", "");
        if (validSnowflake(guild) && validSnowflake(channel)) {
          Event event{.operation = Operation::InvalidateChannels, .session = session, .resource = guild};
          event.removed = {channel};
          publish(std::move(event));
        }
      }
      auto threads = parseThreadUpdates(type, data, m_self);
      if (threads.reset || !threads.changes.empty()) {
        Event event{.operation = Operation::ThreadUpdate, .session = session};
        event.threadUpdates = std::move(threads);
        publish(std::move(event));
      }
      auto guildUpdates = parseGuildUpdates(type, data);
      if (!guildUpdates.empty()) {
        Event event{.operation = Operation::GuildMetadata, .session = session};
        event.guildUpdates = std::move(guildUpdates);
        publish(std::move(event));
      }
      auto styles = parseRoleUpdates(type, data);
      if (type == "GUILD_MEMBERS_CHUNK" && data.value("nonce", std::string{}).starts_with("complete-")) {
        Event event{.operation = Operation::MemberSearch, .session = session, .resource = data.value("guild_id", "")};
        event.nonce = data.value("nonce", "");
        if (data.contains("members") && data["members"].is_array())
          for (const auto& member : data["members"]) {
            if (event.memberMatches.size() >= 25)
              break;
            if (!member.is_object() || !member.contains("user") || !member["user"].is_object())
              continue;
            auto user = parseAccount(member["user"]);
            if (!validSnowflake(user.id))
              continue;
            if (member.contains("nick") && member["nick"].is_string())
              user.name = member["nick"].get<std::string>();
            event.memberMatches.push_back(std::move(user));
          }
        publish(std::move(event));
      }
      if (styles.reset || !styles.guilds.empty()) {
        Event event{.operation = Operation::RoleStyles, .session = session};
        event.roleUpdates = std::move(styles);
        publish(std::move(event));
      }
      UserUpdates acknowledgedPreferences;
      {
        std::lock_guard lock(m_mutex);
        if (session == m_session) {
          if (type == "READY")
            m_guildPreferenceAcks.clear();
          else
            acknowledgedPreferences.guildNotifications.swap(m_guildPreferenceAcks);
        }
      }
      // Gateway-owned projections only change on this worker. REST successes
      // enter before the next dispatch, so suppression is current before a new
      // message is counted even when Discord delays its settings dispatch.
      m_userDirectory.apply(acknowledgedPreferences);
      for (const auto& [guild, preferences] : acknowledgedPreferences.guildNotifications) {
        Json settings{{"guild_id", guild}};
        if (preferences.suppressEveryone)
          settings["suppress_everyone"] = *preferences.suppressEveryone;
        if (preferences.suppressRoles)
          settings["suppress_roles"] = *preferences.suppressRoles;
        m_readState.context("USER_GUILD_SETTINGS_UPDATE", settings, m_self);
      }
      auto users = m_userDirectory.consume(type, data);
      if (type != "READY")
        m_readState.context(type, data, m_self);
      {
        std::lock_guard lock(m_mutex);
        if (session == m_session) {
          if (users.reset) {
            m_channelNotificationFlags.clear();
            m_guildNotificationFlags.clear();
            m_guildPreferenceRevisions.clear();
            const auto settings = data.find("user_guild_settings");
            m_channelNotificationFlagsReady = settings != data.end()
                && (settings->is_array()
                    || (settings->is_object() && settings->contains("entries") && (*settings)["entries"].is_array()));
            m_guildNotificationFlagsReady = m_channelNotificationFlagsReady;
          }
          for (const auto& [id, patch] : users.notifications)
            if (patch.flags && (m_channelNotificationFlags.contains(id) || m_channelNotificationFlags.size() < 100000))
              m_channelNotificationFlags[id] = *patch.flags;
          for (const auto& [id, patch] : users.guildNotifications) {
            if (m_guildPreferenceRevisions.contains(id) || m_guildPreferenceRevisions.size() < 1000)
              ++m_guildPreferenceRevisions[id];
            if (patch.flags && (m_guildNotificationFlags.contains(id) || m_guildNotificationFlags.size() < 1000))
              m_guildNotificationFlags[id] = *patch.flags;
          }
        }
      }
      if (users.reset
          || !users.users.empty()
          || !users.mutedChannels.empty()
          || !users.notifications.empty()
          || !users.guildNotifications.empty()) {
        Event event{.operation = Operation::UserState, .session = session};
        event.users = std::move(users);
        publish(std::move(event));
      }
      auto publishReads = [&](std::string rewind = {}) {
        Event state{.operation = Operation::ReadState, .session = session};
        state.resource = std::move(rewind);
        state.unread = m_readState.unread();
        state.unreadCursors = m_readState.unreadCursors();
        publish(std::move(state));
      };
      if (type == "THREAD_CREATE" || type == "THREAD_UPDATE")
        m_readState.channels(Json::array({data}), data.value("guild_id", ""));
      if (type == "THREAD_LIST_SYNC" && data.contains("threads"))
        m_readState.channels(data["threads"], data.value("guild_id", ""));
      if (type == "CHANNEL_DELETE"
          || type == "GUILD_DELETE"
          || type == "THREAD_DELETE"
          || type == "THREAD_CREATE"
          || type == "THREAD_LIST_SYNC")
        publishReads();
      if (type == "CHANNEL_UNREAD_UPDATE") {
        if (data.contains("channel_unread_updates"))
          m_readState.channels(data["channel_unread_updates"], data.value("guild_id", ""));
        publishReads();
      } else if (type == "MESSAGE_ACK") {
        m_readState.acknowledge(data);
        publishReads(data.value("manual", false) ? data.value("channel_id", "") : "");
      } else if (type == "PRESENCE_SENT") {
        Event event{.operation = Operation::PresenceChanged, .session = session};
        event.presence = Presence{data.value("status", "unknown"), data.value("text", "")};
        publish(std::move(event));
      } else if (type == "USER_UPDATE") {
        Event event{.operation = Operation::AccountUpdate, .session = session};
        event.account = parseAccount(data);
        publish(std::move(event));
      } else if (type == "READY" || type == "RESUMED") {
        if (type == "READY") {
          if (data.contains("user"))
            m_self = data["user"].value("id", "");
          m_readState.ready(data);
          publishReads("reset");
          if (data.contains("guilds") && data["guilds"].is_array() && data["guilds"].size() <= 10000) {
            Event event{.operation = Operation::Guilds, .session = session};
            event.guilds = parseGuilds(data["guilds"]);
            event.guildsComplete = true;
            event.live = true;
            m_guildsReadySession = session;
            wakeRequests();
            publish(std::move(event));
          }
        }
        if (data.contains("users"))
          for (const auto& user : data["users"]) {
            auto account = parseAccount(user);
            m_users[account.id] = std::move(account);
          }
        if (data.contains("guilds"))
          for (const auto& guild : data["guilds"]) {
            if (guild.contains("emojis") && guild["emojis"].is_array()) {
              Event event{.operation = Operation::GuildEmojis, .session = session, .resource = guild.value("id", "")};
              event.emojis = parseGuildEmojis(guild["emojis"], event.resource);
              publish(std::move(event));
            }
            if (guild.contains("stickers") && guild["stickers"].is_array()) {
              Event event{
                  .operation = Operation::GuildStickers,
                  .session = session,
                  .resource = guild.value("id", ""),
                  .live = true
              };
              event.stickers = parseStickers(guild["stickers"], event.resource);
              publish(std::move(event));
            }
            if (guild.contains("channels") && guild["channels"].is_array()) {
              Event snapshot{
                  .operation = Operation::ChannelSnapshot, .session = session, .resource = guild.value("id", "")
              };
              snapshot.channels = parseChannels(guild["channels"]);
              publish(std::move(snapshot));
            }
            if (guild.contains("roles"))
              for (const auto& role : guild["roles"])
                m_roleNames[role.at("id").get<std::string>()] = role.value("name", "Members");
          }
        if (data.contains("private_channels")) {
          Event event{.operation = Operation::PrivateChannels, .session = session, .live = true};
          event.channels = parsePrivateChannels(data["private_channels"], m_users);
          publish(std::move(event));
        }
        if (data.contains("user_settings_proto")) {
          Event event{.operation = Operation::Layout, .session = session, .live = true};
          const Json settings{{"proto", data["user_settings_proto"]}};
          event.layout = parseGuildLayout(settings);
          event.presence = parsePresenceSettings(settings);
          if (event.presence && !m_gateway->restorePresence(session, *event.presence))
            event.presence.reset();
          m_bootstrapSettingsReadySession = session;
          publish(std::move(event));
        } else if (data.contains("user_settings")) {
          Event event{.operation = Operation::Layout, .session = session, .live = true};
          event.layout = parseGuildLayout(data["user_settings"]);
          event.presence = parsePresenceSettings(data["user_settings"]);
          if (event.presence && !m_gateway->restorePresence(session, *event.presence))
            event.presence.reset();
          m_bootstrapSettingsReadySession = session;
          publish(std::move(event));
        }
        // The owning loop uses this as the end of the READY snapshot batch.
        if (type == "RESUMED")
          publish(Event{.operation = Operation::Gateway, .session = session, .live = true});
      } else if (type == "READY_SUPPLEMENTAL") {
        if (data.contains("lazy_private_channels") && data["lazy_private_channels"].is_array()) {
          Event event{.operation = Operation::PrivateChannelUpsert, .session = session};
          event.channels = parsePrivateChannels(data["lazy_private_channels"], m_users);
          publish(std::move(event));
        }
      } else if (type == "USER_SETTINGS_PROTO_UPDATE") {
        if (data.at("settings").value("type", 0) == 2) {
          Event changed{.operation = Operation::GifFavorites, .session = session, .resource = "changed"};
          publish(std::move(changed));
          publish(Event{.operation = Operation::EmojiPreferences, .session = session, .resource = "changed"});
        }
        if (data.at("settings").value("type", 0) == 1) {
          Event changed{.operation = Operation::AccountSettings, .session = session, .resource = "changed"};
          publish(std::move(changed));
          if (auto presence = parsePresenceSettings(data["settings"])) {
            Event event{.operation = Operation::PresenceChanged, .session = session};
            event.presence = presence;
            publish(std::move(event));
          }
          auto layout = parseGuildLayout(data["settings"]);
          // Partial updates unrelated to guild organization must not reset it.
          if (!layout.folders.empty() || !layout.positions.empty() || !data.value("partial", true)) {
            Event event{.operation = Operation::Layout, .session = session, .live = true};
            event.layout = std::move(layout);
            publish(std::move(event));
          }
        }
      } else if (type == "RECONNECTING" || type == "CONNECTION_FAILED") {
        Event event{.operation = Operation::Gateway, .session = session};
        if (type == "CONNECTION_FAILED" && data.value("authentication_failed", false)) {
          event.unauthorized = true;
          if (m_authenticatedSession == session || m_restoringSession == session)
            forgetLogin(session);
          m_authenticatedSession = 0;
        }
        event.error = type == "CONNECTION_FAILED" ? "Live connection was rejected. Sign out and reconnect."
                                                  : "Reconnecting… " + data.value("reason", std::string{});
        publish(std::move(event));
      } else if (type == "THREAD_MEMBER_LIST_UPDATE") {
        Event event{.operation = Operation::Members, .session = session, .resource = memberChannel};
        event.members = std::move(*threadMembers);
        event.memberIdentity = memberIdentity;
        event.memberCount = static_cast<int>(event.members.size());
        publish(std::move(event));
      } else if (type == "GUILD_MEMBER_LIST_UPDATE") {
        m_memberList.apply(data, m_roleNames, m_users);
        Event event{.operation = Operation::Members, .session = session, .resource = memberChannel};
        event.members = m_memberList.rows();
        event.memberIdentity = memberIdentity;
        event.memberCount = m_memberList.total;
        publish(std::move(event));
        if (data.contains("ops"))
          for (const auto& op : data["ops"]) {
            if (op.value("op", "") == "INVALIDATE"
                && op.contains("range")
                && op["range"].is_array()
                && op["range"].size() == 2
                && op["range"][0].get<int>() <= memberFirst + 199
                && op["range"][1].get<int>() >= memberFirst) {
              m_gateway->subscribe(memberGuild, memberChannel, memberFirst, true);
              break;
            }
          }
      } else if (
          type == "GUILD_EMOJIS_UPDATE"
          || type == "GUILD_STICKERS_UPDATE"
          || type == "GUILD_CREATE"
          || type == "GUILD_UPDATE"
          || type == "GUILD_DELETE"
      ) {
        const auto guild = data.value("guild_id", data.value("id", std::string{}));
        if (data.contains("emojis") || type == "GUILD_DELETE") {
          Event event{.operation = Operation::GuildEmojis, .session = session, .resource = guild};
          event.emojis = parseGuildEmojis(data.value("emojis", Json::array()), guild);
          publish(std::move(event));
        }
        if (data.contains("stickers") || type == "GUILD_DELETE") {
          Event event{.operation = Operation::GuildStickers, .session = session, .resource = guild, .live = true};
          event.stickers = parseStickers(data.value("stickers", Json::array()), guild);
          publish(std::move(event));
        }
      } else if ((type == "CHANNEL_CREATE" || type == "CHANNEL_UPDATE") && data.contains("guild_id")) {
        publish(
            Event{
                .operation = Operation::InvalidateChannels, .session = session, .resource = data.value("guild_id", "")
            }
        );
      } else if (type == "CHANNEL_CREATE" || type == "CHANNEL_UPDATE") {
        const auto channelType = data.value("type", 1);
        if (channelType == 1 || channelType == 3) {
          Event event{.operation = Operation::PrivateChannelUpsert, .session = session};
          event.channels = parsePrivateChannels(Json::array({data}), m_users);
          publish(std::move(event));
        }
      } else if (type == "CHANNEL_DELETE" && !data.contains("guild_id")) {
        Event event{.operation = Operation::PrivateChannelDelete, .session = session};
        event.resource = data.at("id");
        publish(std::move(event));
      } else if (auto reaction = parseReactionUpdate(type, data)) {
        Event event{.operation = Operation::Reaction, .session = session, .resource = reaction->channel};
        event.reaction = std::move(reaction);
        publish(std::move(event));
      } else if (type == "TYPING_START") {
        if (auto typing = parseTypingStart(data)) {
          if (typing->name.empty())
            if (const auto user = m_users.find(typing->user); user != m_users.end())
              typing->name = user->second.name;
          Event event{.operation = Operation::TypingStart, .session = session, .resource = typing->channel};
          event.typing = std::move(typing);
          publish(std::move(event));
        }
      } else if (type == "MESSAGE_CREATE") {
        const auto author = data.contains("author") ? data["author"].value("id", std::string{}) : std::string{};
        m_readState.message(
            data, m_self, m_userDirectory.relationship(author) == 2, m_userDirectory.muted(data.value("channel_id", ""))
        );
        publishReads();
        Event event{
            .operation = Operation::MessageCreate, .session = session, .resource = data.value("channel_id", "")
        };
        event.messages = parseMessages(Json::array({data}));
        publish(std::move(event));
      } else if (type == "MESSAGE_UPDATE") {
        Event event{
            .operation = Operation::MessageUpdate, .session = session, .resource = data.value("channel_id", "")
        };
        event.messageId = data.at("id");
        if (data.contains("content") && data["content"].is_string())
          event.content = data["content"];
        event.patch = parseMessagePatch(data);
        publish(std::move(event));
      } else if (type == "MESSAGE_DELETE" || type == "MESSAGE_DELETE_BULK") {
        m_readState.removed(data);
        publishReads();
        Event event{
            .operation = Operation::MessageDelete, .session = session, .resource = data.value("channel_id", "")
        };
        if (type == "MESSAGE_DELETE")
          event.removed.push_back(data.at("id"));
        else
          event.removed = data.at("ids").get<std::vector<std::string>>();
        publish(std::move(event));
      }
    } catch (...) { /* Ignore malformed optional events without exposing account data. */
    }
  }

  void Client::publish(Event event) {
    std::lock_guard lock(m_mutex);
    if (event.session != m_session)
      return;
    if (event.operation == Operation::Voice && event.voice && event.voice->revision != m_voiceRevision)
      return;
    if (m_events.size() >= 2048) {
      // Keep bounded UI delivery. REST history can resynchronize the selected channel.
      m_events.erase(m_events.begin());
    }
    m_events.push_back(std::move(event));
    (void)m_wake.signal();
  }

  std::vector<Event> Client::drain() {
    m_wake.drain();
    std::lock_guard lock(m_mutex);
    return std::exchange(m_events, {});
  }

  void Client::runReads(std::stop_token stop, std::size_t lane) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> connection(curl_easy_init(), curl_easy_cleanup);
    m_transportCache->attach(connection.get());
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
    const auto read = [](const auto& command) { return independentRead(command.operation); };
    const auto background = [](const auto& command) { return command.background; };
    while (!stop.stop_requested()) {
      Command command;
      std::string token;
      std::uint64_t foregroundRevision = 0, credentialRevision = 0;
      {
        std::unique_lock lock(m_mutex);
        m_ready.wait(lock, stop, [&] {
          return !m_readsPaused
              && !m_readToken.empty()
              && m_authenticatedSession == m_session
              && nextRestRead(m_commands, read, background, m_backgroundReads != 0) != m_commands.end();
        });
        if (stop.stop_requested())
          break;
        const auto next = nextRestRead(m_commands, read, background, m_backgroundReads != 0);
        if (next == m_commands.end())
          continue;
        command = std::move(*next);
        m_commands.erase(next);
        token = m_readToken;
        foregroundRevision = m_foregroundRevision;
        credentialRevision = m_readCredentialRevision;
        if (command.background)
          ++m_backgroundReads;
      }
      const bool optional = command.background;
      ScopeExit releaseSlot{[this, optional] {
        if (optional) {
          std::lock_guard lock(m_mutex);
          --m_backgroundReads;
        }
        m_ready.notify_all();
      }};
      if (command.session != m_session
          || command.session != m_authenticatedSession
          || (command.streamPreview && streamPreviewRevoked(*command.streamPreview))
          || (command.readRevision && command.readRevision != m_historyRevision))
        continue;
      Event event{.operation = command.operation, .session = command.session, .resource = command.resource};
      event.history = command.history;
      event.threadQuery = command.threadQuery;
      event.channelQuery = command.channelQuery;
      event.inboxQuery = command.inboxQuery;
      event.commandIndex = command.commandIndex;
      if (command.streamPreview)
        event.streamPreview=StreamPreviewResult{command.streamPreview->key,command.streamPreview->request,{},false};
      RestRequestScope scope{
          stop, &m_session, command.session, optional ? &m_foregroundRevision : nullptr, foregroundRevision
      };
      scope.readRevision = command.readRevision ? &m_historyRevision : nullptr;
      scope.expectedReadRevision = command.readRevision;
      scope.credentialRevision = &m_readCredentialRevision;
      scope.expectedCredentialRevision = credentialRevision;
      scope.intent = command.readIntent.get();
      scope.promotedNavigation = command.readIntent ? &m_historyRevision : nullptr;
      scope.revoked = command.streamPreview ? command.streamPreview->revoked.get() : nullptr;
      Transfer transfer{{}, scope, connection.get(), multi.get(), &m_readWake[lane]};
      transfer.restrictions = &m_restRestrictions;
      transfer.label = operationLabel(command.operation);
      network_profile::queued(transfer.label, command.queuedAt);
      try {
        std::string path;
        switch (command.operation) {
        case Operation::CommandIndex:
          path = commandIndexPath(*command.commandIndex);
          break;
        case Operation::InboxRead:
          path = inboxHttpRequest(*command.inboxQuery)->path;
          break;
        case Operation::StreamPreviewRead: {
          const auto version=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
          const auto route=streamPreviewHttpRequest(*command.streamPreview,static_cast<std::uint64_t>(std::max<std::int64_t>(version,0)));
          if (route) path=route->path;
          break;
        }
        case Operation::Messages:
          path = messageHistoryPath(command.resource, command.history.value_or(HistoryQuery{}));
          break;
        case Operation::Channels:
          path = "/guilds/" + command.resource + "/channels";
          break;
        case Operation::PrivateChannels:
          path = "/users/@me/channels";
          break;
        case Operation::ChannelDetails:
          path = "/channels/" + command.resource;
          break;
        case Operation::ChannelPage:
          path = channelPagePath(*command.channelQuery);
          break;
        case Operation::Threads:
          path = threadListPath(*command.threadQuery);
          break;
        case Operation::GuildStickers:
          path = "/guilds/" + command.resource + "/stickers";
          break;
        case Operation::StickerPacks:
          path = "/sticker-packs";
          break;
        case Operation::StickerDetails:
          path = "/stickers/" + command.resource;
          break;
        case Operation::GuildEmojis:
          path = "/guilds/" + command.resource + "/emojis";
          break;
        case Operation::GifSearch:
          path = gifSearchPath(command.resource);
          break;
        default:
          continue;
        }
        if (path.empty())
          throw std::runtime_error("Invalid read route");
        const auto response = request(
            command.operation == Operation::GifSearch ? std::string{} : token, path, nullptr, transfer, m_rateLimits
        );
        if (!response) {
          if (command.inboxQuery
              && inboxMissingCollection(*command.inboxQuery, response.error().httpStatus, response.error().serviceCode))
            event.inboxPage = InboxPage{};
          else {
            event.error = response.error().message;
            event.unauthorized = response.error().unauthorized;
          }
        } else {
          switch (command.operation) {
          case Operation::CommandIndex:
            event.commands = parseCommandIndex(*response, *command.commandIndex);
            break;
          case Operation::StreamPreviewRead:
            event.streamPreview->url = parseStreamPreview(*response);
            break;
          case Operation::Messages:
            event.messages = parseMessages(*response);
            break;
          case Operation::Channels:
            event.channels = parseChannels(*response);
            break;
          case Operation::PrivateChannels:
            event.channels = parsePrivateChannels(*response);
            break;
          case Operation::ChannelDetails:
            event.channels = parseChannels(Json::array({*response}));
            if (event.channels.size() != 1 || event.channels.front().id != command.resource)
              throw std::runtime_error("Unexpected channel identity");
            break;
          case Operation::InboxRead:
          case Operation::InboxWrite:
            event.inboxPage = parseInboxPage(*response, *command.inboxQuery);
            break;
          case Operation::ChannelPage:
            if (response->is_object() && response->value("code", 0) == 110000)
              event.error = "Search is being indexed. Please try again shortly.";
            else
              event.channelPage = parseChannelPage(*response, *command.channelQuery);
            break;
          case Operation::Threads:
            if (response->is_object() && response->value("code", 0) == 110000)
              event.error = "Search is being indexed. Please try again shortly.";
            else
              event.threadPage = parseThreadPage(*response, *command.threadQuery);
            break;
          case Operation::GuildStickers:
            event.stickers = parseStickers(*response, command.resource);
            break;
          case Operation::StickerPacks:
            event.stickers = parseStickerPacks(*response);
            break;
          case Operation::StickerDetails:
            event.stickers = parseStickers(Json::array({*response}));
            break;
          case Operation::GuildEmojis:
            event.emojis = parseGuildEmojis(*response, command.resource);
            break;
          case Operation::GifSearch:
            event.gifs = parseGifs(*response);
            break;
          default:
            break;
          }
        }
      } catch (const std::exception&) {
        restLog.warn(
            "Response processing failed id=req-{:x} action={} category=invalid-response", transfer.diagnosticId,
            request_diagnostics::action(transfer.label)
        );
        event.error = "Could not read Discord's response. Reconnect or try again.";
      }
      // All retry decisions below run under m_mutex. A credential change may
      // begin after the cancellation sample and before a 401 is classified.
      const auto retryRead = [&] {
        if (command.session != m_session
            || command.session != m_authenticatedSession
            || (command.readRevision && command.readRevision != m_historyRevision)
            || (command.readIntent
                && command.readIntent->foreground
                && command.readIntent->navigation != m_historyRevision)
            || std::ranges::any_of(m_commands, [&](const auto& pending) {
                 return pending.operation == command.operation
                     && pending.resource == command.resource
                     && pending.history == command.history
                     && pending.threadQuery == command.threadQuery
                     && pending.channelQuery == command.channelQuery;
               }))
          return;
        if (command.readIntent && command.readIntent->foreground) {
          command.background = false;
          command.readRevision = command.readIntent->navigation;
        }
        m_commands.push_back(std::move(command));
      };
      const auto cancelOrRetryRead = [&] {
        if (command.session != m_session || command.session != m_authenticatedSession)
          return;
        if (command.history && command.history->preload && (!command.readIntent || !command.readIntent->foreground)) {
          if (command.readIntent)
            command.readIntent->cancelled = true;
          event.messages.clear();
          event.unauthorized = false;
          event.error = "Chat preload cancelled for foreground work.";
          if (m_events.size() >= 2048)
            m_events.erase(m_events.begin());
          m_events.push_back(std::move(event));
          (void)m_wake.signal();
        } else
          retryRead();
      };
      const auto cancellation = scope.cancellation();
      const bool current = command.session == m_session
          && command.session == m_authenticatedSession
          && (!command.streamPreview || !streamPreviewRevoked(*command.streamPreview))
          && (!command.readRevision || command.readRevision == m_historyRevision)
          && (!command.readIntent
              || !command.readIntent->foreground
              || command.readIntent->navigation == m_historyRevision);
      if (!current || stop.stop_requested())
        continue;
      if (cancellation != RestCancellation::None || transfer.interrupted != RestCancellation::None) {
        std::lock_guard lock(m_mutex);
        // Only GETs enter these lanes. Suspend accepted visible reads through
        // credential changes; speculative reads still release their UI state.
        cancelOrRetryRead();
        continue;
      }
      if (event.unauthorized) {
        bool currentCredential;
        {
          std::lock_guard lock(m_mutex);
          currentCredential = credentialRevision == m_readCredentialRevision
              && !m_readsPaused
              && command.session == m_session
              && command.session == m_authenticatedSession;
          if (currentCredential) {
            m_authenticatedSession = 0;
            m_readToken.clear();
          } else if (command.session == m_session && command.session == m_authenticatedSession)
            cancelOrRetryRead();
        }
        if (!currentCredential)
          continue;
        forgetLogin(command.session);
        m_gateway->reset(command.session);
        wakeRequests();
      }
      publish(std::move(event));
    }
  }

  void Client::run(std::stop_token stop) {
    std::string token;
    std::string authenticatedUser;
    std::uint64_t activeSession = 0;
    struct CachedCosmetic {
      CosmeticProduct product;
      std::chrono::steady_clock::time_point retry{};
    };
    constexpr std::size_t maximumCosmeticProducts = 256;
    constexpr auto cosmeticRetryDelay = std::chrono::minutes(1);
    std::map<std::string, CachedCosmetic> cosmeticProducts;
    struct PendingGuildMfa {
      GuildSettingsRequest request;
      std::string ticket;
      std::vector<std::string> methods;
      std::chrono::steady_clock::time_point expires;
    };
    std::map<std::uint64_t, PendingGuildMfa> guildMfa;
    std::uint64_t nextGuildMfa = 0;
    struct PendingUserMfa {
      UserSettingsRequest request;
      std::string ticket;
      std::vector<std::string> methods;
      std::chrono::steady_clock::time_point expires;
    };
    struct UserProof {
      std::string value, binding;
      std::chrono::steady_clock::time_point expires{};
      bool valid() const { return !value.empty() && expires > std::chrono::steady_clock::now(); }
    };
    std::map<std::uint64_t, PendingUserMfa> userMfa;
    std::uint64_t nextUserMfa = 0, nextDeviceHandle = 0;
    std::map<std::string, std::string> deviceSessions;
    UserProof backupView, backupRegenerate, emailProof, phoneProof;
    std::uint64_t userSecretsEpoch = m_userSettingsSecretsEpoch;
    auto& cooldown = m_rateLimits;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> connection(curl_easy_init(), curl_easy_cleanup);
    m_transportCache->attach(connection.get());
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
    while (!stop.stop_requested()) {
      Command command;
      std::uint64_t foregroundRevision = 0;
      {
        std::unique_lock lock(m_mutex);
        m_ready.wait(lock, stop, [&] {
          return nextRestOwned(
                     m_commands, [](const auto& pending) { return independentRead(pending.operation); },
                     [](const auto& pending) { return pending.background; }
                 )
              != m_commands.end()
              || activeSession != m_session
              || userSecretsEpoch != m_userSettingsSecretsEpoch
              || (!token.empty() && m_bootstrap.resolved() == activeSession && m_authenticatedSession != activeSession);
        });
        if (stop.stop_requested())
          break;
        if (activeSession != m_session) {
          token.clear();
          m_channelNotificationFlags.clear();
          m_channelNotificationFlagsReady = false;
          m_guildNotificationFlags.clear();
          m_guildNotificationFlagsReady = false;
          m_guildPreferenceRevisions.clear();
          m_guildPreferenceAcks.clear();
          authenticatedUser.clear();
          guildMfa.clear();
          userMfa.clear();
          deviceSessions.clear();
          backupView = backupRegenerate = emailProof = phoneProof = {};
          activeSession = m_session;
          cooldown.clear();
        }
        if (m_bootstrap.resolved() == activeSession && m_authenticatedSession != activeSession) {
          token.clear();
          authenticatedUser.clear();
        } else if (m_authenticatedSession == activeSession)
          authenticatedUser = m_authenticatedUser;
        if (userSecretsEpoch != m_userSettingsSecretsEpoch) {
          // Dismissal invalidates proofs and original secret actions without
          // interrupting or automatically retrying an in-flight mutation.
          userMfa.clear();
          backupView = backupRegenerate = emailProof = phoneProof = {};
          userSecretsEpoch = m_userSettingsSecretsEpoch;
        }
        if (m_commands.empty())
          continue;
        const auto next = nextRestOwned(
            m_commands, [](const auto& pending) { return independentRead(pending.operation); },
            [](const auto& pending) { return pending.background; }
        );
        if (next == m_commands.end())
          continue;
        command = std::move(*next);
        m_commands.erase(next);
        foregroundRevision = m_foregroundRevision;
      }
      if (command.session != m_session
          || (command.operation != Operation::Connect && command.session != m_authenticatedSession)
          || (command.operation == Operation::Guilds && m_guildsReadySession == command.session)
          || (command.operation == Operation::BootstrapSettings && m_bootstrapSettingsReadySession == command.session)
          || (command.operation == Operation::SavePresence && command.presenceRevision != m_presenceRevision))
        continue;
      if (command.streamPreview && streamPreviewRevoked(*command.streamPreview))
        continue;
      const bool protectCredentials =
          command.operation == Operation::SaveProfile || (command.userSettings && [&] {
            // MFA retries carry a public, secret-cleared action. Use
            // the retained original to identify credential changes,
            // just as the verified retry does before sending HTTP.
            const auto* action = &*command.userSettings;
            if (action->mfaChallenge) {
              const auto pending = userMfa.find(action->mfaChallenge);
              if (pending != userMfa.end() && sameUserSettingsAction(pending->second.request, *action))
                action = &pending->second.request;
            }
            const auto route = userSettingsHttpRequest(*action);
            return route && (route->mayRotateToken || route->endsSession);
          }());
      if (protectCredentials) {
        std::lock_guard lock(m_mutex);
        m_readsPaused = true;
        ++m_readCredentialRevision;
        wakeRequests();
      }
      ScopeExit resumeReads{[this, protectCredentials, &token] {
        if (protectCredentials) {
          std::lock_guard lock(m_mutex);
          m_readToken = m_authenticatedSession == m_session ? token : std::string{};
          m_readsPaused = false;
          wakeRequests();
        }
      }};
      Event event{.operation = command.operation, .session = command.session, .resource = command.resource};
      if (command.operation == Operation::BootstrapSettings)
        event.operation = Operation::Layout;
      if (command.operation == Operation::Send)
        event.nonce = command.nonce;
      event.history = command.history;
      event.threadQuery = command.threadQuery;
      event.channelQuery = command.channelQuery;
      event.inboxQuery = command.inboxQuery;
      if (command.streamPreview)
        event.streamPreview=StreamPreviewResult{command.streamPreview->key,command.streamPreview->request,{},true};
      event.guildSettingsRequest = command.guildSettings;
      if (command.userSettings)
        event.userSettingsRequest = publicUserSettingsRequest(*command.userSettings);
      if (event.guildSettingsRequest)
        event.guildSettingsRequest->mfaCode.clear();
      if (command.operation == Operation::Acknowledge)
        event.messageId = command.content;
      Transfer transfer{
          {},
          {stop, &m_session, command.session, command.background && command.operation != Operation::StreamPreviewWrite ? &m_foregroundRevision : nullptr, foregroundRevision,
           command.operation == Operation::Guilds                  ? &m_guildsReadySession
               : command.operation == Operation::BootstrapSettings ? &m_bootstrapSettingsReadySession
                                                                   : nullptr},
          connection.get(),
          multi.get(),
          &m_requestWake
      };
      if (command.streamPreview && command.streamPreview->kind == StreamPreviewKind::Upload) {
        transfer.scope.revoked=command.streamPreview->revoked.get();
        transfer.retryRateLimits=false;
      }
      event.restoredLogin = command.restoredLogin;
      transfer.restrictions = &m_restRestrictions;
      transfer.label = operationLabel(command.operation);
      network_profile::queued(transfer.label, command.queuedAt);
      bool saveCredential = false;
      bool endAccountSession = false;
      std::optional<Command> continuation;
      std::uint64_t guildPreferenceRevision = 0;
      try {
        if (command.operation == Operation::Connect) {
          token = std::move(command.content);
          if (!auth::validCredential(token))
            throw std::runtime_error("Enter a valid account token.");
          if (command.handoffKey) {
            if (command.handoffKey->size() != 64 || !std::ranges::all_of(*command.handoffKey, [](unsigned char ch) {
                  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
                }))
              throw std::runtime_error("Invalid browser handoff key");
            const Json handoff{{"key", *command.handoffKey}, {"handoff_token", std::move(token)}};
            auto exchange = request({}, "/auth/handoff/exchange", &handoff, transfer, cooldown);
            if (!exchange) {
              event.error = exchange.error().message;
              publish(std::move(event));
              continue;
            }
            token = exchange->at("token").get<std::string>();
            if (!auth::validCredential(token))
              throw std::runtime_error("Invalid browser session");
          }
          {
            std::lock_guard lock(m_mutex);
            if (command.session != m_session)
              continue;
            m_bootstrap.begin(command.session, command.restoredLogin);
            m_readToken = token;
            m_authenticatedUser.clear();
          }
          // Identity verification and Identify race. READY already contains
          // the account identity and navigation; it must not wait for HTTP.
          m_gateway->authenticate(command.session, token);
          transfer.scope.authoritativeSession = &m_bootstrap.resolved();
        }
        std::string path;
        Json body;
        Json pendingReadStates;
        const char* method = nullptr;
        std::optional<InteractionRequest> interaction;
        bool autoModerationFeedbackSubmitted = false;
        std::optional<UserSettingsHttpRequest> accountRequest;
        std::string guildMfaAuthorization;
        Json accessGuildSnapshot;
        std::optional<GuildAccessPlan> accessPlan;
        bool accessFeatureMutation = false;
        std::optional<std::uint64_t> accessPendingCount;
        std::string accessPendingError;
        switch (command.operation) {
        case Operation::UserSettings: {
          auto& action = *command.userSettings;
          std::erase_if(userMfa, [](const auto& item) {
            return item.second.expires <= std::chrono::steady_clock::now();
          });
          if (action.mfaChallenge) {
            const auto pending = userMfa.find(action.mfaChallenge);
            if (pending == userMfa.end()
                || !sameUserSettingsAction(pending->second.request, action)
                || std::ranges::find(pending->second.methods, action.mfaType) == pending->second.methods.end()
                || action.mfaCode.empty()) {
              event.error = "Verification expired or this action changed. Submit the action again.";
              break;
            }
            event.userSettingsMfaChallenge = pending->first;
            event.userSettingsMfaMethods = pending->second.methods;
            const Json verification{
                {"ticket", pending->second.ticket}, {"mfa_type", action.mfaType}, {"data", action.mfaCode}
            };
            auto verified =
                request(token, "/mfa/finish", &verification, transfer, cooldown, {}, "POST", nullptr, {}, true);
            std::fill(action.mfaCode.begin(), action.mfaCode.end(), '\0');
            action.mfaCode.clear();
            if (!verified) {
              event.error = verified.error().unauthorized
                  ? verified.error().message
                  : "Discord could not verify that code or password. Try again.";
              event.unauthorized = verified.error().unauthorized;
              break;
            }
            if (!verified->is_object() || !verified->contains("token") || !(*verified)["token"].is_string())
              throw std::runtime_error("Invalid verification response");
            guildMfaAuthorization = (*verified)["token"].get<std::string>();
            if (guildMfaAuthorization.empty()
                || guildMfaAuthorization.size() > 8192
                || std::ranges::any_of(guildMfaAuthorization, [](unsigned char c) { return c <= 32 || c == 127; }))
              throw std::runtime_error("Invalid verification token");
            const auto requestId = action.request;
            action = std::move(pending->second.request);
            action.request = requestId;
            userMfa.erase(pending);
            event.userSettingsRequest = publicUserSettingsRequest(action);
            event.userSettingsMfaChallenge = 0;
            event.userSettingsMfaMethods.clear();
          }
          accountRequest = userSettingsHttpRequest(action);
          if (!accountRequest)
            throw std::runtime_error("Invalid account settings request");
          path = accountRequest->path;
          body = accountRequest->body;
          method = accountRequest->method.c_str();
          if (action.section == UserSettingsSection::Profile
              || action.section == UserSettingsSection::GuildProfile
              || action.section == UserSettingsSection::GuildMember) {
            if (!action.resourceId.empty() && action.resourceId != authenticatedUser) {
              event.error = "Only your own account profile can be edited in settings.";
              break;
            }
          }
          if (action.section == UserSettingsSection::Sessions && action.verb == UserSettingsVerb::Delete) {
            auto resolved = Json::array();
            std::string current;
            {
              std::lock_guard lock(m_mutex);
              if (m_gatewaySessionGeneration == command.session)
                current = m_authSessionIdHash;
            }
            for (const auto& handle : action.values["session_id_hashes"]) {
              const auto at = deviceSessions.find(handle.get<std::string>());
              if (at == deviceSessions.end()) {
                event.error = "This device list expired. Reload devices before signing them out.";
                break;
              }
              resolved.push_back(at->second);
              if (!current.empty() && at->second == current)
                endAccountSession = true;
            }
            body["session_id_hashes"] = std::move(resolved);
          }
          if (action.section == UserSettingsSection::BackupCodes && action.verb == UserSettingsVerb::Update) {
            const auto& proof = action.values["regenerate"] == true ? backupRegenerate : backupView;
            if (!proof.valid())
              event.error = "Backup code verification expired. Request a new email code.";
            else
              body["nonce"] = proof.value;
          }
          if (action.section == UserSettingsSection::Account && action.values.contains("email")) {
            if (emailProof.valid())
              body["email_token"] = emailProof.value;
            else
              body.erase("email_token");
          }
          if (action.section == UserSettingsSection::Phone && action.verb == UserSettingsVerb::Update) {
            if (!phoneProof.valid() || phoneProof.binding != action.values["phone"].get<std::string>())
              event.error = "Verify this phone number before adding it to your account.";
            else
              body["phone_token"] = phoneProof.value;
          }
          break;
        }
        case Operation::GuildSettings: {
          if (command.guildSettings->mfaChallenge) {
            std::erase_if(guildMfa, [](const auto& item) {
              return item.second.expires <= std::chrono::steady_clock::now();
            });
            auto pending = guildMfa.find(command.guildSettings->mfaChallenge);
            auto original = *command.guildSettings;
            original.mfaChallenge = 0;
            original.mfaCode.clear();
            original.mfaType.clear();
            if (pending != guildMfa.end())
              original.request = pending->second.request.request;
            if (pending == guildMfa.end()
                || original != pending->second.request
                || std::ranges::find(pending->second.methods, command.guildSettings->mfaType)
                    == pending->second.methods.end()
                || command.guildSettings->mfaCode.empty()) {
              event.error = "Verification expired or this action changed. Submit the action again.";
              break;
            }
            event.guildSettingsMfaChallenge = pending->first;
            event.guildSettingsMfaMethods = pending->second.methods;
            const Json verification{
                {"ticket", pending->second.ticket},
                {"mfa_type", command.guildSettings->mfaType},
                {"data", command.guildSettings->mfaCode}
            };
            auto verified =
                request(token, "/mfa/finish", &verification, transfer, cooldown, {}, "POST", &*command.guildSettings);
            std::fill(command.guildSettings->mfaCode.begin(), command.guildSettings->mfaCode.end(), '\0');
            command.guildSettings->mfaCode.clear();
            if (!verified) {
              event.error = verified.error().unauthorized
                  ? verified.error().message
                  : "Discord could not verify that code or password. Try again.";
              event.unauthorized = verified.error().unauthorized;
              break;
            }
            if (!verified->is_object() || !verified->contains("token") || !(*verified)["token"].is_string())
              throw std::runtime_error("Invalid verification response");
            guildMfaAuthorization = (*verified)["token"].get<std::string>();
            if (guildMfaAuthorization.empty()
                || guildMfaAuthorization.size() > 8192
                || std::ranges::any_of(guildMfaAuthorization, [](unsigned char c) { return c <= 32 || c == 127; }))
              throw std::runtime_error("Invalid verification token");
            guildMfa.erase(pending);
            event.guildSettingsMfaChallenge = 0;
            event.guildSettingsMfaMethods.clear();
          }
          auto routed = *command.guildSettings;
          if (routed.section == GuildSettingsSection::Members
              && routed.verb == GuildSettingsVerb::Update
              && routed.resourceId == authenticatedUser
              && routed.values.is_object()
              && routed.values.size() == 1
              && routed.values.contains("nick"))
            routed.resourceId = "@me";
          const auto settings = guildSettingsHttpRequest(routed);
          if (!settings)
            throw std::runtime_error("Invalid server settings request");
          path = settings->path;
          body = settings->body;
          if (command.guildSettings->section == GuildSettingsSection::Discovery
              && command.guildSettings->verb == GuildSettingsVerb::Update) {
            auto snapshot = request(token, settings->path, nullptr, transfer, cooldown);
            if (!snapshot) {
              event.error = snapshot.error().message;
              event.unauthorized = snapshot.error().unauthorized;
              break;
            }
            body = guildDiscoveryPatch(*snapshot, command.guildSettings->values);
          }
          if (command.guildSettings->section == GuildSettingsSection::Access) {
            auto guild = request(
                token, "/guilds/" + command.guildSettings->guildId + "?with_counts=true", nullptr, transfer, cooldown
            );
            if (!guild) {
              event.error = guild.error().message;
              event.unauthorized = guild.error().unauthorized;
              break;
            }
            accessGuildSnapshot = std::move(*guild);
            const bool applicationsActive = guildAccessMode(accessGuildSnapshot) == "apply";
            if (applicationsActive) {
              auto applications = request(
                  token, "/guilds/" + command.guildSettings->guildId + "/requests?status=SUBMITTED&limit=1", nullptr,
                  transfer, cooldown
              );
              if (applications
                  && applications->is_object()
                  && applications->contains("total")
                  && (*applications)["total"].is_number_integer()
                  && (*applications)["total"].get<std::int64_t>() >= 0)
                accessPendingCount = (*applications)["total"].get<std::uint64_t>();
              else if (!applications) {
                accessPendingError = applications.error().message;
                if (applications.error().unauthorized) {
                  event.error = accessPendingError;
                  event.unauthorized = true;
                  break;
                }
              } else
                accessPendingError = "Could not verify the pending application count.";
              if (command.guildSettings->verb == GuildSettingsVerb::Update
                  && command.guildSettings->values["mode"] != "apply") {
                if (!accessPendingCount)
                  throw std::runtime_error(
                      "Review access to pending applications before changing this access mode. " + accessPendingError
                  );
                if (*accessPendingCount > 0
                    && (!command.guildSettings->values.contains("bulk_action")
                        || command.guildSettings->values["bulk_action"].is_null()))
                  throw std::runtime_error(
                      "Choose whether to approve or reject pending applications before changing access mode."
                  );
                if (*accessPendingCount > 200
                    && command.guildSettings->values.contains("bulk_action")
                    && command.guildSettings->values["bulk_action"] == "APPROVED")
                  throw std::runtime_error(
                      "More than 200 pending applications must be reviewed individually or explicitly rejected."
                  );
              }
            }
            if (command.guildSettings->verb == GuildSettingsVerb::Update) {
              auto screening = request(token, settings->path, nullptr, transfer, cooldown);
              if (!screening) {
                event.error = screening.error().message;
                event.unauthorized = screening.error().unauthorized;
                break;
              }
              accessPlan = guildAccessPlan(accessGuildSnapshot, *screening, *command.guildSettings);
              if (!accessPlan->guildPatch.empty()) {
                path = "/guilds/" + command.guildSettings->guildId;
                body = accessPlan->guildPatch;
                accessFeatureMutation = true;
              } else
                body = accessPlan->verificationPatch;
            }
          }
          if (command.guildSettings->section == GuildSettingsSection::ServerGuide
              && command.guildSettings->verb == GuildSettingsVerb::Update) {
            auto snapshot = request(token, settings->path, nullptr, transfer, cooldown);
            if (!snapshot) {
              event.error = snapshot.error().message;
              event.unauthorized = snapshot.error().unauthorized;
              break;
            }
            body = guildGuidePatch(*snapshot, command.guildSettings->values);
            body["guild_id"] = command.guildSettings->guildId;
          }
          if (command.guildSettings->section == GuildSettingsSection::JoinInterview) {
            auto application =
                request(token, "/join-requests/" + command.guildSettings->resourceId, nullptr, transfer, cooldown);
            if (!application) {
              event.error = application.error().message;
              event.unauthorized = application.error().unauthorized;
              break;
            }
            if (!application->is_object()
                || application->value("guild_id", std::string{}) != command.guildSettings->guildId)
              throw std::runtime_error("The application does not belong to this server.");
          }
          // Retain the storage for CURLOPT_CUSTOMREQUEST until request() returns.
          command.content = accessFeatureMutation ? "PATCH" : settings->method;
          method = command.content.c_str();
          break;
        }
        case Operation::GuildStickers:
          path = "/guilds/" + command.resource + "/stickers";
          break;
        case Operation::StickerPacks:
          path = "/sticker-packs";
          break;
        case Operation::StickerDetails:
          path = "/stickers/" + command.resource;
          break;
        case Operation::GuildEmojis:
          path = "/guilds/" + command.resource + "/emojis";
          break;
        case Operation::EmojiPreferences:
          path = "/users/@me/settings-proto/2";
          break;
        case Operation::SaveEmojiFavorite: {
          path = "/users/@me/settings-proto/2";
          auto snapshot = request(token, path, nullptr, transfer, cooldown);
          if (!snapshot) {
            event.error = snapshot.error().message;
            event.unauthorized = snapshot.error().unauthorized;
            break;
          }
          body = emojiFavoritePatch(*snapshot, command.resource, command.content == "add");
          method = "PATCH";
          break;
        }
        case Operation::GifSearch:
          path = gifSearchPath(command.resource);
          break;
        case Operation::GifFavorites:
          path = "/users/@me/settings-proto/2";
          break;
        case Operation::SaveGifFavorite: {
          path = "/users/@me/settings-proto/2";
          auto snapshot = request(token, path, nullptr, transfer, cooldown);
          if (!snapshot) {
            event.error = snapshot.error().message;
            event.unauthorized = snapshot.error().unauthorized;
            break;
          }
          const auto value = Json::parse(command.content);
          body = gifFavoritePatch(*snapshot, gifFromJson(value), value.at("add").get<bool>());
          method = "PATCH";
          break;
        }
        case Operation::Interaction:
          if (!command.interaction || !(interaction = interactionRequest(*command.interaction)))
            continue;
          if (command.interaction->kind == InteractionKind::CreateDm
              && std::ranges::find(command.interaction->recipients, authenticatedUser)
                  != command.interaction->recipients.end()) {
            event.interaction = command.interaction;
            event.error = "Choose recipients other than yourself.";
            break;
          }
          if (command.interaction->kind == InteractionKind::PinDm
              || command.interaction->kind == InteractionKind::UnpinDm) {
            std::lock_guard lock(m_mutex);
            if (!m_channelNotificationFlagsReady || command.session != m_session) {
              event.interaction = command.interaction;
              event.error = "Wait for your conversation preferences to finish loading, then try again.";
              break;
            }
            const auto flags = m_channelNotificationFlags.find(command.resource);
            command.interaction->notificationFlags = flags == m_channelNotificationFlags.end() ? 0 : flags->second;
            interaction = interactionRequest(*command.interaction);
          }
          event.interaction = command.interaction;
          path = interaction->path;
          body = interaction->body;
          method = interaction->method.c_str();
          if (command.interaction->kind == InteractionKind::AutoModerationFeedback) {
            auto identity = request(token, "/users/@me?with_analytics_token=true", nullptr, transfer, cooldown);
            if (!identity) {
              event.error = identity.error().message;
              event.unauthorized = identity.error().unauthorized;
              break;
            }
            const auto analytics = identity->find("analytics_token");
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch()
            )
                                 .count();
            const auto feedback = analytics != identity->end() && analytics->is_string()
                ? autoModerationFeedbackBody(*command.interaction, analytics->get_ref<const std::string&>(), now)
                : std::nullopt;
            if (!feedback) {
              event.error = "Could not prepare AutoMod feedback. Try again.";
              break;
            }
            auto submitted = request(token, "/science", &*feedback, transfer, cooldown, {}, "POST");
            if (!submitted) {
              event.error = submitted.error().message;
              event.unauthorized = submitted.error().unauthorized;
              break;
            }
            autoModerationFeedbackSubmitted = true;
          }
          if (command.interaction->kind == InteractionKind::GuildNotifications) {
            {
              std::lock_guard lock(m_mutex);
              const auto& guild = command.interaction->guild;
              if (command.session != m_session
                  || (command.interaction->notificationFlagsMask && !m_guildNotificationFlagsReady)) {
                event.error = "Wait for your server preferences to finish loading, then try again.";
                break;
              }
              const auto revision = m_guildPreferenceRevisions.find(guild);
              guildPreferenceRevision = revision == m_guildPreferenceRevisions.end() ? 0 : revision->second;
              if (command.interaction->notificationFlagsMask) {
                const auto at = m_guildNotificationFlags.find(guild);
                const auto previous = at == m_guildNotificationFlags.end() ? 0 : at->second;
                command.interaction->notificationFlags = previous;
                event.interaction = command.interaction;
                body["flags"] = (previous & ~command.interaction->notificationFlagsMask)
                    | *command.interaction->guildNotification->flags;
              }
            }
            auto acknowledged = body;
            acknowledged["guild_id"] = command.interaction->guild;
            event.users.guildNotifications = guildNotifications(acknowledged);
          } else if (command.interaction->kind == InteractionKind::MarkGuildRead) {
            pendingReadStates = std::move(body["read_states"]);
            body["read_states"] = Json::array();
            const auto count = std::min<std::size_t>(100, pendingReadStates.size());
            for (std::size_t i = 0; i < count; ++i)
              body["read_states"].push_back(pendingReadStates[i]);
            pendingReadStates.erase(pendingReadStates.begin(), pendingReadStates.begin() + count);
          } else if (
              command.interaction->kind == InteractionKind::MoveGuild
              || command.interaction->kind == InteractionKind::EditGuildFolder
              || command.interaction->kind == InteractionKind::ReorderGuilds
              || (command.interaction->kind == InteractionKind::GuildPrivacy
                  && command.interaction->guildPrivacy
                  && !command.interaction->guildPrivacy->empty())
          ) {
            auto snapshot = request(token, path, nullptr, transfer, cooldown);
            if (!snapshot) {
              event.error = snapshot.error().message;
              event.unauthorized = snapshot.error().unauthorized;
              break;
            }
            if (command.interaction->kind == InteractionKind::MoveGuild)
              body = guildMovePatch(
                  *snapshot, command.interaction->guild, command.interaction->text,
                  command.interaction->content.empty() ? std::nullopt : std::make_optional(command.interaction->content)
              );
            else if (command.interaction->kind == InteractionKind::EditGuildFolder)
              body = guildFolderPatch(*snapshot, *command.interaction->folderChange);
            else if (command.interaction->kind == InteractionKind::ReorderGuilds)
              body = guildRailPatch(*snapshot, *command.interaction->railMove);
            else
              body = guildPrivacyPatch(*snapshot, command.interaction->guild, *command.interaction->guildPrivacy);
          }
          if (command.interaction->kind == InteractionKind::Mute
              || command.interaction->kind == InteractionKind::Unmute
              || command.interaction->kind == InteractionKind::ChannelNotifications) {
            auto overrideData = body["channel_overrides"][command.resource];
            overrideData["channel_id"] = command.resource;
            event.users.notifications = channelNotifications(Json{{"channel_overrides", Json::array({overrideData})}});
          }
          if (command.interaction->kind == InteractionKind::MessageButton
              || command.interaction->kind == InteractionKind::SubmitModal
              || command.interaction->kind == InteractionKind::ApplicationCommand
              || command.interaction->kind == InteractionKind::CommandAutocomplete) {
            {
              std::lock_guard lock(m_mutex);
              if (m_gatewaySessionGeneration != command.session || m_gatewaySessionId.empty())
                event.error = "Wait for the live connection before using applications.";
              else
                body["session_id"] = m_gatewaySessionId;
            }
            if (command.interaction->kind == InteractionKind::ApplicationCommand && !command.uploads.empty()) {
              if (!event.error.empty())
                break;
              const auto staging = commandUploadRequest(command.uploads);
              auto reserved =
                  request(token, "/channels/" + command.resource + "/attachments", &staging, transfer, cooldown);
              if (!reserved) {
                event.error = reserved.error().message;
                event.unauthorized = reserved.error().unauthorized;
                break;
              }
              const auto slots = commandUploadSlots(*reserved, command.uploads);
              for (std::size_t i = 0; i < slots.size(); ++i)
                if (!uploadCommandFile(slots[i], command.uploads[i], multi.get(), m_requestWake, transfer.scope)) {
                  event.error = "The attachment upload could not be confirmed. The command was not submitted.";
                  break;
                }
              if (!event.error.empty())
                break;
              body["data"]["attachments"] = commandUploadedAttachments(slots, command.uploads);
              command.uploads.clear();
            }
          } else if (command.interaction->kind == InteractionKind::CreateChannel) {
            std::optional<Json> parent;
            if (command.interaction->privateChannel
                && command.interaction->parentId
                && !command.interaction->parentId->empty()) {
              auto snapshot =
                  request(token, "/channels/" + *command.interaction->parentId, nullptr, transfer, cooldown);
              if (!snapshot) {
                event.error = snapshot.error().message;
                event.unauthorized = snapshot.error().unauthorized;
                break;
              }
              parent = std::move(*snapshot);
            }
            body = createChannelBody(*command.interaction, parent);
          } else if (
              command.interaction->kind == InteractionKind::MoveChannel && command.interaction->syncPermissions
          ) {
            auto parent = request(token, "/channels/" + *command.interaction->parentId, nullptr, transfer, cooldown);
            if (!parent) {
              event.error = parent.error().message;
              event.unauthorized = parent.error().unauthorized;
              break;
            }
            body = moveChannelBody(*parent, *command.interaction);
          } else if (command.interaction->kind == InteractionKind::SendInvite && command.interaction->channel.empty()) {
            auto dm = request(token, path, &body, transfer, cooldown, {}, "POST");
            if (!dm) {
              event.error = dm.error().message;
              event.unauthorized = dm.error().unauthorized;
              break;
            }
            const auto channel = dm->value("id", "");
            if (!validSnowflake(channel) || dm->value("type", -1) != 1)
              throw std::runtime_error("Unexpected direct message identity");
            const auto recipients = dm->find("recipients");
            if (recipients == dm->end()
                || !recipients->is_array()
                || std::ranges::none_of(*recipients, [&](const auto& recipient) {
                     return recipient.is_object() && recipient.value("id", "") == command.interaction->user;
                   }))
              throw std::runtime_error("Unexpected direct message recipient");
            path = "/channels/" + channel + "/messages";
            body = messageBody(command.interaction->text);
          } else if (command.interaction->kind == InteractionKind::DuplicateChannel) {
            auto snapshot = request(token, "/channels/" + command.resource, nullptr, transfer, cooldown);
            if (!snapshot) {
              event.error = snapshot.error().message;
              event.unauthorized = snapshot.error().unauthorized;
              break;
            }
            body = duplicateChannelBody(*snapshot, *command.interaction);
          } else if (
              command.interaction->kind == InteractionKind::GrantChannelAccess
              || command.interaction->kind == InteractionKind::EditChannelPermission
          ) {
            auto snapshot = request(token, "/channels/" + command.resource, nullptr, transfer, cooldown);
            if (!snapshot) {
              event.error = snapshot.error().message;
              event.unauthorized = snapshot.error().unauthorized;
              break;
            }
            if (command.interaction->kind == InteractionKind::EditChannelPermission) {
              body = channelPermissionBody(*snapshot, *command.interaction);
              auto projected = *snapshot;
              auto& entries = projected["permission_overwrites"];
              auto entry = std::ranges::find_if(entries, [&](const auto& value) {
                return value["id"] == command.interaction->user && value["type"] == command.interaction->overwriteType;
              });
              auto changed = body;
              changed["id"] = command.interaction->user;
              if (entry == entries.end())
                entries.push_back(std::move(changed));
              else
                *entry = std::move(changed);
              // Keep the acknowledged edit available even if the optional read-back fails.
              event.channels = parseChannels(Json::array({projected}));
              break;
            }
            Channel fresh;
            parseChannelDetails(fresh, *snapshot);
            if (!fresh.overwrites) {
              event.error = "Could not read the current channel permissions. Try again.";
              break;
            }
            std::string allow = "0", deny = "0";
            for (const auto& o : *fresh.overwrites)
              if (o.id == command.interaction->user && o.type == command.interaction->overwriteType) {
                allow = o.allow;
                deny = o.deny;
                break;
              }
            body["allow"] = withViewPermission(allow, true);
            body["deny"] = withViewPermission(deny, false);
          }
          break;
        case Operation::Acknowledge:
          path = "/channels/" + command.resource + "/messages/" + command.content + "/ack";
          body = {{"token", nullptr}, {"manual", false}};
          method = "POST";
          break;
        case Operation::Profile:
        case Operation::Connect:
          path = "/users/@me";
          break;
        case Operation::Guilds:
          path = "/users/@me/guilds?limit=200&with_counts=true"
              + (command.resource.empty() ? "" : "&after=" + command.resource);
          break;
        case Operation::BootstrapSettings:
        case Operation::AccountSettings:
          path = "/users/@me/settings-proto/1";
          break;
        case Operation::SettingsList:
          path = command.resource == "connections" ? "/users/@me/connections"
              : command.resource == "devices"      ? "/auth/sessions"
                                                   : "/oauth2/tokens";
          break;
        case Operation::SaveAccountSettings: {
          path = "/users/@me/settings-proto/1";
          auto snapshot = request(token, path, nullptr, transfer, cooldown);
          if (!snapshot) {
            event.error = snapshot.error().message;
            event.unauthorized = snapshot.error().unauthorized;
            publish(std::move(event));
            continue;
          }
          body = accountSettingsPatch(*snapshot, Json::parse(command.content).get<SettingValues>());
          method = "PATCH";
          break;
        }
        case Operation::SaveProfile:
          path = "/users/@me";
          body = Json::parse(command.content);
          method = "PATCH";
          break;
        case Operation::SavePresence: {
          path = "/users/@me/settings-proto/1";
          auto current = request(token, path, nullptr, transfer, cooldown);
          if (!current) {
            event.error = current.error().message;
            event.unauthorized = current.error().unauthorized;
            publish(std::move(event));
            continue;
          }
          body = presenceSettingsPatch(*current, command.resource, command.customStatus);
          method = "PATCH";
          break;
        }
        case Operation::ChannelDetails:
          path = "/channels/" + command.resource;
          break;
        case Operation::ChannelPage:
          path = channelPagePath(*command.channelQuery);
          break;
        case Operation::Threads:
          path = threadListPath(*command.threadQuery);
          break;
        case Operation::Channels:
          path = "/guilds/" + command.resource + "/channels";
          break;
        case Operation::PrivateChannels:
          path = "/users/@me/channels";
          break;
        case Operation::Messages:
          path = messageHistoryPath(command.resource, command.history.value_or(HistoryQuery{}));
          break;

        case Operation::InboxWrite: {
          auto route = inboxHttpRequest(*command.inboxQuery);
          path = route->path;
          method = command.inboxQuery->kind == InboxKind::Save ? "PUT" : "DELETE";
          body = std::move(route->body);
          break;
        }
        case Operation::StreamPreviewWrite: {
          const auto route=streamPreviewHttpRequest(*command.streamPreview,0);
          if (!route)
            throw std::runtime_error("The preview publication was revoked.");
          path=route->path;
          method="POST";
          body=route->body;
          break;
        }
        case Operation::Send:
          path = "/channels/" + command.resource + "/messages";
          body = messageBody(command.content, command.reply, command.stickers, command.nonce);
          if (!command.uploads.empty()) {
            body["attachments"] = Json::array();
            for (std::size_t i = 0; i < command.uploads.size(); ++i)
              body["attachments"].push_back({{"id", i}, {"filename", command.uploads[i].filename}});
          }
          break;
        default:
          continue;
        }
        auto response = autoModerationFeedbackSubmitted && !command.interaction->markAutoModerationFeedback
            ? std::expected<Json, Failure>(Json::object())
            : event.error.empty()
            ? request(
                  command.operation == Operation::GifSearch ? std::string{} : token, path,
                  ((interaction
                    || command.operation == Operation::GuildSettings
                    || command.operation == Operation::UserSettings
                    || command.operation == Operation::InboxWrite)
                       ? !body.is_null()
                       : command.operation == Operation::Send || method)
                      ? &body
                      : nullptr,
                  transfer, cooldown, command.uploads, method,
                  command.guildSettings ? &*command.guildSettings : nullptr, guildMfaAuthorization,
                  command.operation == Operation::UserSettings
              )
            : std::expected<Json, Failure>(std::unexpected(Failure{event.error, event.unauthorized}));
        // The issue itself is already delivered. The permission-dependent alert
        // marker is supplementary, like Discord's fire-and-forget marker update.
        if (autoModerationFeedbackSubmitted && !response)
          response = Json::object();
        while (response && pendingReadStates.is_array() && !pendingReadStates.empty()) {
          auto reads = Json::array();
          const auto count = std::min<std::size_t>(100, pendingReadStates.size());
          for (std::size_t i = 0; i < count; ++i)
            reads.push_back(pendingReadStates[i]);
          pendingReadStates.erase(pendingReadStates.begin(), pendingReadStates.begin() + count);
          const Json next{{"read_states", std::move(reads)}};
          response = request(token, path, &next, transfer, cooldown, {}, "POST");
        }
        if (response && accessFeatureMutation) {
          response = request(
              token, "/guilds/" + command.guildSettings->guildId + "/member-verification",
              &accessPlan->verificationPatch, transfer, cooldown, {}, "PATCH", &*command.guildSettings,
              guildMfaAuthorization
          );
          if (!response)
            response.error().message =
                "Server access changed, but the membership form could not be saved. " + response.error().message;
        }
        if (response && command.guildSettings && command.guildSettings->section == GuildSettingsSection::Access) {
          if (accessPlan && accessFeatureMutation)
            accessGuildSnapshot.update(accessPlan->guildPatch);
          const auto mode = accessPlan ? accessPlan->mode : guildAccessMode(accessGuildSnapshot);
          *response = Json{
              {"guild", std::move(accessGuildSnapshot)},
              {"screening", std::move(*response)},
              {"mode", mode},
              {"pending_application_count", accessPendingCount ? Json(*accessPendingCount) : Json{}},
              {"pending_application_error", accessPendingError}
          };
        }
        if (!response) {
          event.error = response.error().message;
          event.unauthorized = response.error().unauthorized;
          if (command.userSettings && response.error().mfa.is_object()) {
            const auto& challenge = response.error().mfa;
            if (challenge.contains("ticket")
                && challenge["ticket"].is_string()
                && !challenge["ticket"].get_ref<const std::string&>().empty()
                && challenge["ticket"].get_ref<const std::string&>().size() <= 4096
                && challenge.contains("methods")
                && challenge["methods"].is_array()) {
              PendingUserMfa pending;
              pending.request = *command.userSettings;
              pending.request.mfaChallenge = 0;
              pending.request.mfaCode.clear();
              pending.request.mfaType.clear();
              pending.ticket = challenge["ticket"].get<std::string>();
              pending.expires = std::chrono::steady_clock::now() + std::chrono::minutes(5);
              for (const auto& method : challenge["methods"])
                if (method.is_object() && method.contains("type") && method["type"].is_string()) {
                  const auto type = method["type"].get<std::string>();
                  if ((type == "totp" || type == "password")
                      && std::ranges::find(pending.methods, type) == pending.methods.end())
                    pending.methods.push_back(type);
                }
              if (!pending.methods.empty()) {
                if (userMfa.size() >= 8)
                  userMfa.erase(userMfa.begin());
                event.userSettingsMfaChallenge = ++nextUserMfa;
                event.userSettingsMfaMethods = pending.methods;
                userMfa.emplace(nextUserMfa, std::move(pending));
              } else
                event.error = "This action requires a verification method available in Discord's browser settings.";
            }
          }
          if (command.guildSettings && response.error().mfa.is_object()) {
            const auto& challenge = response.error().mfa;
            if (challenge.contains("ticket")
                && challenge["ticket"].is_string()
                && challenge["ticket"].get_ref<const std::string&>().size() <= 4096
                && !challenge["ticket"].get_ref<const std::string&>().empty()
                && challenge.contains("methods")
                && challenge["methods"].is_array()) {
              PendingGuildMfa pending;
              pending.request = *command.guildSettings;
              pending.request.mfaCode.clear();
              pending.request.mfaType.clear();
              pending.request.mfaChallenge = 0;
              pending.ticket = challenge["ticket"].get<std::string>();
              pending.expires = std::chrono::steady_clock::now() + std::chrono::minutes(5);
              for (const auto& entry : challenge["methods"])
                if (entry.is_object() && entry.contains("type") && entry["type"].is_string()) {
                  const auto type = entry["type"].get<std::string>();
                  if ((type == "totp" || type == "password")
                      && std::ranges::find(pending.methods, type) == pending.methods.end())
                    pending.methods.push_back(type);
                }
              if (!pending.methods.empty()) {
                if (guildMfa.size() >= 8)
                  guildMfa.erase(guildMfa.begin());
                event.guildSettingsMfaChallenge = ++nextGuildMfa;
                event.guildSettingsMfaMethods = pending.methods;
                guildMfa.emplace(nextGuildMfa, std::move(pending));
              }
            }
          }
        } else
          switch (command.operation) {
          case Operation::UserSettings: {
            const auto& action = *command.userSettings;
            const auto proof = [&](const char* field, std::string binding = {}) {
              if (!response->is_object() || !response->contains(field) || !(*response)[field].is_string())
                throw std::runtime_error("Missing verification proof");
              const auto value = (*response)[field].get<std::string>();
              if (value.empty() || value.size() > 8192 || std::ranges::any_of(value, [](unsigned char c) {
                    return c <= 32 || c == 127;
                  }))
                throw std::runtime_error("Invalid verification proof");
              return UserProof{value, std::move(binding), std::chrono::steady_clock::now() + std::chrono::minutes(5)};
            };
            if (action.section == UserSettingsSection::BackupCodes && action.verb == UserSettingsVerb::Create) {
              backupView = proof("nonce");
              backupRegenerate = proof("regenerate_nonce");
            }
            if (action.section == UserSettingsSection::EmailChange && action.verb == UserSettingsVerb::Update)
              emailProof = proof("token");
            if (action.section == UserSettingsSection::PhoneVerification)
              phoneProof = proof("token", action.values["phone"].get<std::string>());
            if (action.section == UserSettingsSection::Account && action.values.contains("email"))
              emailProof = {};
            if (action.section == UserSettingsSection::Phone && action.verb == UserSettingsVerb::Update)
              phoneProof = {};
            if (action.section == UserSettingsSection::Sessions && action.verb == UserSettingsVerb::Get) {
              std::string current;
              {
                std::lock_guard lock(m_mutex);
                if (m_gatewaySessionGeneration == command.session)
                  current = m_authSessionIdHash;
              }
              if (!response->is_object()
                  || !response->contains("user_sessions")
                  || !(*response)["user_sessions"].is_array()
                  || (*response)["user_sessions"].size() > 256)
                throw std::runtime_error("Invalid device list");
              auto safeSessions = Json::array();
              for (auto item : (*response)["user_sessions"]) {
                if (!item.is_object() || !item.contains("id_hash") || !item["id_hash"].is_string())
                  continue;
                const auto hash = item["id_hash"].get<std::string>();
                if (hash.empty() || hash.size() > 256)
                  continue;
                auto known =
                    std::ranges::find_if(deviceSessions, [&](const auto& entry) { return entry.second == hash; });
                if (known == deviceSessions.end()) {
                  if (deviceSessions.size() >= 512)
                    deviceSessions.erase(deviceSessions.begin());
                  const auto handle = "device-" + std::to_string(++nextDeviceHandle);
                  known = deviceSessions.emplace(handle, hash).first;
                }
                item["id_hash"] = known->first;
                item["current_session"] = !current.empty() && current == hash;
                safeSessions.push_back(std::move(item));
              }
              (*response)["user_sessions"] = std::move(safeSessions);
            }
            if (accountRequest->mayRotateToken && response->is_object() && response->contains("token")) {
              const auto replacement = (*response)["token"];
              if (!replacement.is_string() || !auth::validCredential(replacement.get<std::string>())) {
                event.error = "Discord updated the account, but the new session could not be restored. Sign in again.";
                event.unauthorized = true;
                break;
              }
              token = replacement.get<std::string>();
              saveCredential = true;
              m_gateway->authenticate(command.session, token);
            }
            const auto settings = parseUserSettings(*response, action);
            event.userSettings = settings.data;
            if (action.section == UserSettingsSection::Account && action.verb == UserSettingsVerb::Get) {
              std::lock_guard lock(m_mutex);
              if (m_gatewaySessionGeneration == command.session && m_authTotpEnabled)
                event.userSettings["mfa_totp"] = *m_authTotpEnabled;
            }
            if (action.section == UserSettingsSection::Authenticator) {
              std::lock_guard lock(m_mutex);
              if (m_gatewaySessionGeneration == command.session)
                m_authTotpEnabled = action.verb == UserSettingsVerb::Create;
            }
            event.userSettingsMore = settings.more;
            event.userSettingsCursor = settings.cursor;
            event.userSettingsSensitive = accountRequest->sensitiveResponse;
            event.userSettingsSessionEnded = accountRequest->endsSession || endAccountSession;
            if (response->is_object() && response->contains("id") && (*response)["id"] == authenticatedUser)
              event.account = parseAccount(*response);
            break;
          }
          case Operation::GuildSettings: {
            const auto settings = parseGuildSettings(*response, *command.guildSettings);
            event.guildSettings = settings.data;
            event.guildSettingsMore = settings.more;
            event.guildSettingsCursor = settings.cursor;
            if (command.guildSettings->section == GuildSettingsSection::JoinInterview) {
              if (!response->is_object() || response->value("type", -1) != 3)
                throw std::runtime_error("Invalid application interview channel.");
              event.channels = parsePrivateChannels(Json::array({*response}));
              if (event.channels.size() != 1)
                throw std::runtime_error("Invalid application interview channel.");
            }
            break;
          }
          case Operation::GuildStickers:
            event.stickers = parseStickers(*response, command.resource);
            break;
          case Operation::StickerPacks:
            event.stickers = parseStickerPacks(*response);
            break;
          case Operation::StickerDetails:
            event.stickers = parseStickers(Json::array({*response}));
            break;
          case Operation::GuildEmojis:
            event.emojis = parseGuildEmojis(*response, command.resource);
            break;
          case Operation::EmojiPreferences:
          case Operation::SaveEmojiFavorite:
            if (response->value("out_of_date", false))
              event.error = "Emoji favorites changed on another device. Please try again.";
            else
              event.emojiPreferences = parseEmojiPreferences(*response);
            break;
          case Operation::GifSearch:
            event.gifs = parseGifs(*response);
            break;
          case Operation::GifFavorites:
          case Operation::SaveGifFavorite:
            if (response->value("out_of_date", false))
              event.error = "Favorite GIFs changed on another device. Please try again.";
            else
              event.gifs = parseGifFavorites(*response);
            break;
          case Operation::Connect: {
            event.account = parseAccount(*response);
            if (!validSnowflake(event.account.id))
              throw std::runtime_error("Unexpected account identity");
            authenticatedUser = event.account.id;
            break;
          }
          case Operation::Guilds: {
            if (!response->is_array() || response->size() > 200)
              throw std::runtime_error("Unexpected server page");
            event.guilds = parseGuilds(*response);
            event.guildsComplete = response->size() < 200;
            if (!event.guildsComplete) {
              const auto& next = event.guilds.back().id;
              if (!validSnowflake(next)
                  || next == command.resource
                  || command.guildsLoaded + event.guilds.size() >= 10000)
                throw std::runtime_error("Could not finish loading the server list");
              continuation = command;
              continuation->resource = next;
              continuation->guildsLoaded += event.guilds.size();
            }
            break;
          }
          case Operation::BootstrapSettings:
            event.layout = parseGuildLayout(*response);
            if (command.presenceRevision == m_presenceRevision)
              event.presence = parsePresenceSettings(*response);
            break;
          case Operation::Interaction:
            if (command.interaction->kind == InteractionKind::GuildNotifications) {
              if (response->is_object()) {
                if (const auto id = response->find("guild_id");
                    id != response->end() && !id->is_null() && *id != command.interaction->guild)
                  throw std::runtime_error("Unexpected server preference identity");
                auto confirmed = *response;
                confirmed["guild_id"] = command.interaction->guild;
                const auto authoritative = guildNotifications(confirmed);
                if (const auto at = authoritative.find(command.interaction->guild); at != authoritative.end()) {
                  UserDirectory projection;
                  projection.apply(event.users);
                  UserUpdates patch;
                  patch.guildNotifications = authoritative;
                  projection.apply(patch);
                  event.users.guildNotifications[command.interaction->guild] =
                      projection.guildPreferences(command.interaction->guild);
                }
              }
              {
                std::lock_guard lock(m_mutex);
                const auto& guild = command.interaction->guild;
                const auto revision = m_guildPreferenceRevisions.find(guild);
                const auto current = revision == m_guildPreferenceRevisions.end() ? 0 : revision->second;
                if (command.session == m_session
                    && current == guildPreferenceRevision
                    && (revision != m_guildPreferenceRevisions.end() || m_guildPreferenceRevisions.size() < 1000)) {
                  const auto& confirmed = event.users.guildNotifications.at(guild);
                  if (confirmed.flags)
                    m_guildNotificationFlags[guild] = *confirmed.flags;
                  UserDirectory projection;
                  UserUpdates pending;
                  pending.guildNotifications = m_guildPreferenceAcks;
                  projection.apply(pending);
                  projection.apply(event.users);
                  m_guildPreferenceAcks[guild] = projection.guildPreferences(guild);
                  ++m_guildPreferenceRevisions[guild];
                }
              }
            } else if (
                command.interaction->kind == InteractionKind::MoveGuild
                || command.interaction->kind == InteractionKind::EditGuildFolder
                || command.interaction->kind == InteractionKind::ReorderGuilds
                || command.interaction->kind == InteractionKind::GuildPrivacy
            ) {
              if (response->value("out_of_date", false)) {
                event.error = "Settings changed on another device. Reopen this menu and try again.";
                break;
              }
              const auto& saved = response->contains("settings") || response->contains("proto") ? *response : body;
              if (command.interaction->kind == InteractionKind::MoveGuild
                  || command.interaction->kind == InteractionKind::EditGuildFolder
                  || command.interaction->kind == InteractionKind::ReorderGuilds)
                event.layout = parseGuildLayout(saved);
              else
                event.guildPrivacy = parseGuildPrivacy(saved, command.interaction->guild);
            } else if (command.interaction->kind == InteractionKind::CreateInvite) {
              const auto code = response->value("code", std::string{});
              if (code.empty() || code.size() > 128 || !std::ranges::all_of(code, [](unsigned char c) {
                    return std::isalnum(c) || c == '-' || c == '_';
                  }))
                throw std::runtime_error("Discord returned an invalid invite.");
              event.inviteUrl = "https://discord.gg/" + code;
            } else if (
                command.interaction->kind == InteractionKind::CreateChannel
                || command.interaction->kind == InteractionKind::DuplicateChannel
                || command.interaction->kind == InteractionKind::MoveChannel
            ) {
              event.channels = parseChannels(Json::array({*response}));
              if (event.channels.size() != 1
                  || response->value("guild_id", "") != command.interaction->guild
                  || (command.interaction->kind == InteractionKind::MoveChannel
                      && event.channels.front().id != command.resource))
                throw std::runtime_error("Unexpected channel identity");
            } else if (command.interaction->kind == InteractionKind::EditChannel) {
              event.channels = parseChannels(Json::array({*response}));
              if (event.channels.size() != 1 || event.channels[0].id != command.resource)
                throw std::runtime_error("Unexpected channel identity");
            } else if (
                command.interaction->kind == InteractionKind::GrantChannelAccess
                || command.interaction->kind == InteractionKind::EditChannelPermission
            ) {
              auto updated = request(token, "/channels/" + command.resource, nullptr, transfer, cooldown);
              if (updated)
                event.channels = parseChannels(Json::array({*updated}));
            } else if (
                command.interaction->kind == InteractionKind::CreateThread
                || command.interaction->kind == InteractionKind::CreateForumPost
                || command.interaction->kind == InteractionKind::UnarchiveThread
            ) {
              auto thread = parseThread(*response);
              if (!thread
                  || (command.interaction->kind == InteractionKind::UnarchiveThread
                      && thread->id != command.interaction->channel)
                  || ((command.interaction->kind == InteractionKind::CreateThread
                       || command.interaction->kind == InteractionKind::CreateForumPost)
                      && thread->parentId != command.interaction->channel))
                throw std::runtime_error("Unexpected thread identity");
              if (command.interaction->kind == InteractionKind::CreateThread
                  || command.interaction->kind == InteractionKind::CreateForumPost)
                thread->joined = true;
              event.channels.push_back(*thread);
            } else if (command.interaction->kind == InteractionKind::GuildProfile) {
              event.guildProfile = parseGuildProfile(*response);
              if (event.guildProfile->id != command.interaction->guild)
                throw std::runtime_error("Unexpected server profile identity");
            } else if (command.interaction->kind == InteractionKind::JoinGuild) {
              event.guildVerificationRequired =
                  response->contains("show_verification_form") && (*response)["show_verification_form"] == true;
              if (response->contains("id")
                  && (*response)["id"].is_string()
                  && (*response)["id"] == command.interaction->guild)
                event.guilds = parseGuilds(Json::array({*response}));
            } else if (command.interaction->kind == InteractionKind::ListReactions)
              event.reactors = parseReactors(*response);
            else if (command.interaction->kind == InteractionKind::RemoveEmbeds) {
              const auto patch = removedEmbedsPatch(*command.interaction, *response);
              if (!patch)
                throw std::runtime_error("Embed removal was not confirmed for this message");
              event.patch = *patch;
            } else if (command.interaction->kind == InteractionKind::EditMessage)
              event.messages = parseMessages(Json::array({*response}));
            else if (command.interaction->kind == InteractionKind::OpenDm)
              event.channels = parsePrivateChannels(Json::array({*response}));
            else if (command.interaction->kind == InteractionKind::CreateDm)
              event.channels = {createdDm(*response, command.interaction->recipients, authenticatedUser)};
            else if (
                command.interaction->kind == InteractionKind::PinDm
                || command.interaction->kind == InteractionKind::UnpinDm
            ) {
              event.users.notifications = channelNotifications(*response);
              const auto patch = event.users.notifications.find(command.resource);
              if (patch == event.users.notifications.end()
                  || !patch->second.flags
                  || ((*patch->second.flags & 2048) != 0) != (command.interaction->kind == InteractionKind::PinDm))
                throw std::runtime_error("Discord did not confirm the conversation pin. Try again.");
              std::lock_guard lock(m_mutex);
              if (command.session == m_session
                  && (m_channelNotificationFlags.contains(command.resource)
                      || m_channelNotificationFlags.size() < 100000)
                  && m_channelNotificationFlags[command.resource] == command.interaction->notificationFlags)
                m_channelNotificationFlags[command.resource] = *patch->second.flags;
            } else if (
                command.interaction->kind == InteractionKind::AcceptMessageRequest
                || command.interaction->kind == InteractionKind::IgnoreMessageRequest
            ) {
              if (response->contains("id")) {
                if ((*response)["id"] != command.interaction->channel)
                  throw std::runtime_error("Unexpected message request identity");
                event.channels = parsePrivateChannels(Json::array({*response}));
              }
            } else if (command.interaction->kind == InteractionKind::UserProfile) {
              event.profile = parseUserProfile(*response, command.interaction->guild);
              if (event.profile->user.id != command.interaction->user)
                throw std::runtime_error("Unexpected profile identity");
              event.account = event.profile->user;
              const auto hydrate = [&](const std::string& sku) -> CosmeticProduct {
                if (!validSnowflake(sku) || stop.stop_requested())
                  return {};
                auto at = cosmeticProducts.find(sku);
                if (at != cosmeticProducts.end() && at->second.retry > std::chrono::steady_clock::now())
                  return at->second.product;
                if (at == cosmeticProducts.end()) {
                  if (cosmeticProducts.size() >= maximumCosmeticProducts)
                    cosmeticProducts.erase(cosmeticProducts.begin());
                  at = cosmeticProducts.emplace(sku, CachedCosmetic{}).first;
                }
                at->second.retry = std::chrono::steady_clock::now() + cosmeticRetryDelay;
                auto product = request(token, "/collectibles-products/" + sku, nullptr, transfer, cooldown);
                if (product) {
                  at->second.product = parseCosmeticProduct(*product, sku);
                  at->second.retry = std::chrono::steady_clock::time_point::max();
                }
                return at->second.product;
              };
              if (event.profile->effect) {
                const auto product = hydrate(event.profile->effect->sku);
                if (product.effect)
                  event.profile->effect = product.effect;
              }
              if (event.profile->frame) {
                const auto product = hydrate(event.profile->frame->sku);
                if (product.frame)
                  event.profile->frame = product.frame;
              }
            }
            break;
          case Operation::AccountSettings:
          case Operation::SaveAccountSettings:
            if (response->value("out_of_date", false))
              event.error = "Settings changed on another device. Reload and try again.";
            else
              event.settings = parseAccountSettings(*response);
            break;
          case Operation::SettingsList:
            event.settingsEntries = parseSettingsEntries(command.resource, *response);
            break;
          case Operation::Profile:
          case Operation::SaveProfile:
            event.account = parseAccount(*response);
            if (command.operation == Operation::SaveProfile
                && response->contains("token")
                && (*response)["token"].is_string()) {
              const auto replacement = (*response)["token"].get<std::string>();
              if (auth::validCredential(replacement)) {
                token = replacement;
                saveCredential = true;
                m_gateway->authenticate(command.session, token);
              }
            }
            break;
          case Operation::SavePresence:
            if (response->value("out_of_date", false))
              event.error = "Status changed on another device. Please try again.";
            else
              event.presence = parsePresenceSettings(*response);
            break;
          case Operation::ChannelDetails:
            event.channels = parseChannels(Json::array({*response}));
            if (event.channels.size() != 1 || event.channels[0].id != command.resource)
              throw std::runtime_error("Unexpected channel identity");
            break;
          case Operation::InboxRead:
          case Operation::InboxWrite:
            event.inboxPage = parseInboxPage(*response, *command.inboxQuery);
            break;
          case Operation::ChannelPage:
            if (response->contains("code") && (*response)["code"] == 110000)
              event.error = "Search is being indexed. Please try again shortly.";
            else
              event.channelPage = parseChannelPage(*response, *command.channelQuery);
            break;
          case Operation::Threads:
            if (response->contains("code") && (*response)["code"] == 110000)
              event.error = "Search is being indexed. Please try again shortly.";
            else
              event.threadPage = parseThreadPage(*response, *command.threadQuery);
            break;
          case Operation::Channels:
            event.channels = parseChannels(*response);
            break;
          case Operation::PrivateChannels:
            event.channels = parsePrivateChannels(*response);
            break;
          case Operation::Messages:
            event.messages = parseMessages(*response);
            break;
          case Operation::Send:
            event.messages = parseMessages(Json::array({*response}));
            break;
          default:
            break;
          }
      } catch (const std::exception&) {
        // Do not expose arbitrary JSON/transport exceptions, which can contain account data.
        restLog.warn(
            "Response processing failed id=req-{:x} action={} category=invalid-response", transfer.diagnosticId,
            request_diagnostics::action(transfer.label)
        );
        event.error = "Could not read Discord's response. Reconnect or try again.";
      }
      if (command.operation == Operation::Connect && completeConnect(event, BootstrapSource::Rest))
        continue;
      if (transfer.interrupted == RestCancellation::ForegroundReady) {
        if (command.history && command.history->preload) {
          // Speculative reads yield permanently to actual user work. Never
          // requeue one behind the foreground load of the same conversation.
          event.error = "Chat preload cancelled for foreground work.";
          publish(std::move(event));
          continue;
        }
        std::lock_guard lock(m_mutex);
        if (command.session == m_session
            && command.session == m_authenticatedSession
            && !(command.operation == Operation::Guilds && m_guildsReadySession == command.session)
            && !std::ranges::any_of(m_commands, [&](const auto& pending) {
                 return pending.operation == command.operation && pending.resource == command.resource;
               })) {
          // One suspended read can be returned beside the bounded accepted
          // queue. It never duplicates itself or silently drops a UI completion.
          m_commands.push_back(std::move(command));
        }
        continue;
      }
      if (transfer.interrupted == RestCancellation::Superseded)
        continue;
      if (event.unauthorized
          && command.session == m_session
          && (command.restoredLogin || m_authenticatedSession == command.session))
        forgetLogin(command.session);
      if (event.unauthorized || (command.operation == Operation::Connect && !event.error.empty())) {
        token.clear();
        authenticatedUser.clear();
        m_authenticatedSession = 0;
        userMfa.clear();
        deviceSessions.clear();
        backupView = backupRegenerate = emailProof = phoneProof = {};
        m_gateway->reset(command.session);
      }
      if (event.userSettingsSessionEnded && event.error.empty() && command.session == m_session) {
        token.clear();
        authenticatedUser.clear();
        m_authenticatedSession = 0;
        guildMfa.clear();
        userMfa.clear();
        deviceSessions.clear();
        backupView = backupRegenerate = emailProof = phoneProof = {};
        m_gateway->reset(command.session);
        forgetLogin(command.session);
        saveCredential = false;
      }
      if (command.operation == Operation::SavePresence
          && command.presenceRevision != m_presenceRevision
          && !event.unauthorized)
        continue;
      const bool remember = saveCredential && event.error.empty();
      if (command.operation == Operation::BootstrapSettings && event.presence) {
        if (command.presenceRevision != m_presenceRevision
            || !m_gateway->restorePresence(command.session, *event.presence))
          event.presence.reset();
      }
      publish(std::move(event));
      if (continuation && m_guildsReadySession != command.session) {
        std::lock_guard lock(m_mutex);
        if (command.session == m_session && command.session == m_authenticatedSession)
          m_commands.push_back(std::move(*continuation));
      }
      if (remember)
        saveLogin(command.session, token);
    }
  }
} // namespace api
