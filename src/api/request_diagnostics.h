#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace api::request_diagnostics {
  // These templates are the only route text permitted in diagnostic output.
  // Unknown endpoints remain useful as an action/method pair without disclosing
  // opaque path segments, invitation codes, identities or query values.
  inline constexpr auto routes = std::to_array<std::string_view>(
      {"/users/@me",
       "/users/@me/channels",
       "/users/@me/guilds",
       "/users/@me/guilds/:id",
       "/users/@me/mentions",
       "/users/@me/mentions/:id",
       "/users/@me/saved-messages",
       "/users/@me/saved-messages/bookmarks",
       "/users/@me/saved-messages/:id",
       "/users/@me/bookmarks",
       "/users/@me/bookmarks/:id",
       "/users/@me/relationships",
       "/users/@me/relationships/:id",
       "/users/@me/relationships/:id/ignore",
       "/users/@me/settings",
       "/users/@me/settings-proto/:id",
       "/users/@me/profile",
       "/users/@me/connections",
       "/users/@me/connections/:type/:id",
       "/users/@me/connections/:type/:id/refresh",
       "/users/@me/billing/subscriptions",
       "/users/@me/billing/subscriptions/:id",
       "/users/@me/billing/payments",
       "/users/@me/billing/history",
       "/users/@me/billing/invoice/breakdown",
       "/users/@me/billing/payment-sources",
       "/users/@me/entitlements",
       "/users/@me/collectibles-purchases",
       "/users/@me/harvest",
       "/users/@me/email",
       "/users/@me/email/verify-code",
       "/users/@me/phone",
       "/users/@me/delete",
       "/users/@me/disable",
       "/users/@me/mfa/codes-verification",
       "/users/@me/mfa/totp/enable",
       "/users/@me/mfa/totp/disable",
       "/users/@me/mfa/sms/enable",
       "/users/@me/mfa/sms/disable",
       "/users/@me/mfa/webauthn/credentials",
       "/users/@me/guilds/premium/subscription-slots",
       "/users/:id",
       "/users/:id/profile",
       "/channels/:id",
       "/channels/:id/application-command-index",
       "/channels/:id/attachments",
       "/users/@me/application-command-index",
       "/channels/:id/messages",
       "/channels/:id/messages/search",
       "/channels/:id/messages/:id",
       "/channels/:id/messages/:id/ack",
       "/channels/:id/messages/:id/bookmarks",
       "/channels/:id/messages/:id/reactions/:emoji/@me",
       "/channels/:id/messages/:id/reactions/:emoji/:id",
       "/channels/:id/messages/:id/reactions/:emoji",
       "/channels/:id/messages/:id/reactions",
       "/channels/:id/messages/:id/threads",
       "/channels/:id/threads",
       "/channels/:id/threads/archived/public",
       "/channels/:id/threads/archived/private",
       "/channels/:id/users/@me/threads/archived/private",
       "/channels/:id/thread-members/@me",
       "/channels/:id/thread-members/:id",
       "/channels/:id/thread-members",
       "/channels/:id/pins",
       "/channels/:id/pins/:id",
       "/channels/:id/typing",
       "/channels/:id/recipients/:id",
       "/channels/:id/invites",
       "/channels/:id/webhooks",
       "/channels/:id/permissions/:id",
       "/guilds/:id",
       "/guilds/:id/channels",
       "/guilds/:id/messages/search",
       "/guilds/:id/threads/active",
       "/guilds/:id/roles",
       "/guilds/:id/roles/:id",
       "/guilds/:id/emojis",
       "/guilds/:id/emojis/:id",
       "/guilds/:id/stickers",
       "/guilds/:id/stickers/:id",
       "/guilds/:id/members",
       "/guilds/:id/members/@me",
       "/guilds/:id/members/:id",
       "/guilds/:id/members-search",
       "/guilds/:id/profile/@me",
       "/guilds/:id/profile",
       "/guilds/:id/bans",
       "/guilds/:id/bans/:id",
       "/guilds/:id/invites",
       "/guilds/:id/audit-logs",
       "/guilds/:id/soundboard-sounds",
       "/guilds/:id/integrations",
       "/guilds/:id/integrations/:id",
       "/guilds/:id/integrations/:id/sync",
       "/guilds/:id/webhooks",
       "/guilds/:id/widget",
       "/guilds/:id/welcome-screen",
       "/guilds/:id/onboarding",
       "/guilds/:id/vanity-url",
       "/guilds/:id/templates",
       "/guilds/:id/templates/:code",
       "/guilds/:id/scheduled-events",
       "/guilds/:id/scheduled-events/:id",
       "/guilds/:id/auto-moderation/rules",
       "/guilds/:id/auto-moderation/rules/:id",
       "/guilds/:id/prune",
       "/guilds/:id/delete",
       "/guilds/:id/mfa",
       "/guilds/:id/visibility",
       "/guilds/:id/pincode",
       "/guilds/:id/member",
       "/guilds/:id/discovery-requirements",
       "/guilds/:id/discovery-metadata",
       "/guilds/:id/discovery-categories/:id",
       "/guilds/:id/incident-actions",
       "/guilds/:id/bulk-ban/v2",
       "/guilds/:id/new-member-welcome",
       "/guilds/:id/new-member-actions/:id",
       "/guilds/:id/resource-channels/:id",
       "/guilds/:id/requests",
       "/guilds/:id/requests/:id",
       "/guilds/:id/applications",
       "/guilds/:id/application-command-index",
       "/guilds/:id/premium/subscriptions",
       "/guilds/:id/premium/subscriptions/:id",
       "/guilds/:id/member-verification",
       "/guilds/:id/creator-monetization/requirements",
       "/guilds/:id/creator-monetization/restrictions",
       "/guilds/:id/analytics/growth-activation/overview",
       "/guilds/:id/analytics/growth-activation/retention",
       "/guilds/:id/analytics/engagement/overview",
       "/invites/:code",
       "/join-requests/:id",
       "/join-requests/:id/interview",
       "/webhooks/:id",
       "/webhooks/:id/:token",
       "/stickers/:id",
       "/sticker-packs",
       "/collectibles-products/:id",
       "/gifs/search",
       "/gifs/suggest",
       "/gifs/trending",
       "/auth/handoff/exchange",
       "/auth/sessions",
       "/auth/sessions/logout",
       "/auth/forgot",
       "/auth/verify/resend",
       "/auth/verify/view-backup-codes-challenge",
       "/mfa/finish",
       "/phone-verifications/verify",
       "/phone-verifications/resend",
       "/oauth2/tokens",
       "/oauth2/tokens/:id",
       "/connections/:type/authorize",
       "/entitlements/gift-codes/:code/redeem",
       "/discovery/categories",
       "/games/autocomplete",
       "/applications/:id/guilds/:id/commands/:id/permissions",
       "/science"}
  );

  inline bool matches(std::string_view path, std::string_view pattern) {
    while (!pattern.empty() && !path.empty()) {
      const auto patternEnd = pattern.find('/', 1), pathEnd = path.find('/', 1);
      const auto expected = pattern.substr(1, patternEnd == pattern.npos ? pattern.size() - 1 : patternEnd - 1);
      const auto actual = path.substr(1, pathEnd == path.npos ? path.size() - 1 : pathEnd - 1);
      if (actual.empty() || (expected.front() != ':' && expected != actual))
        return false;
      pattern = patternEnd == pattern.npos ? std::string_view{} : pattern.substr(patternEnd);
      path = pathEnd == path.npos ? std::string_view{} : path.substr(pathEnd);
    }
    return pattern.empty() && path.empty();
  }
  inline std::string_view route(std::string_view path) {
    constexpr std::size_t maximumPathBytes = 4096;
    if (!path.starts_with('/') || path.size() > maximumPathBytes)
      return "/unclassified";
    path = path.substr(0, path.find_first_of("?#"));
    for (const auto pattern : routes)
      if (matches(path, pattern))
        return pattern;
    return "/unclassified";
  }
  inline std::string_view action(std::string_view value) {
    constexpr auto allowed = std::to_array<std::string_view>(
        {"connect",
         "messages",
         "channels",
         "direct-messages",
         "servers",
         "channel-details",
         "channel-page",
         "inbox",
         "threads",
         "stickers",
         "sticker-packs",
         "sticker",
         "emojis",
         "gifs",
         "server-settings",
         "user-settings",
         "startup-settings",
         "account",
         "gateway",
         "gateway-connect",
         "asset",
         "send-message",
         "read-receipt",
         "reaction",
         "interaction",
         "application-commands",
         "profile",
         "save-profile",
         "save-presence",
         "account-settings",
         "save-account-settings",
         "settings-list",
         "gif-favorites",
         "emoji-preferences"}
    );
    for (const auto label : allowed)
      if (value == label)
        return label;
    return "other";
  }
  inline std::string_view method(std::string_view value) {
    for (const auto label : {"GET", "POST", "PATCH", "PUT", "DELETE", "HEAD", "OPTIONS"})
      if (value == label)
        return label;
    return "OTHER";
  }
  inline bool expectedEmptyCollection(
      std::string_view verb, std::string_view path, long status, int serviceCode, std::size_t pageSize
  ) {
    constexpr int missingSavedCollectionCode = 10133;
    if (verb != "GET" || status != 404 || serviceCode != missingSavedCollectionCode)
      return false;
    return path == "/users/@me/saved-messages?bookmark_ids=true"
        || path == std::format("/users/@me/saved-messages/bookmarks?limit={}", pageSize);
  }
  enum class Outcome {
    Success,
    ExpectedEmpty,
    Cancelled,
    Restricted,
    RateLimited,
    Transport,
    Authentication,
    Permission,
    Http,
    InvalidResponse,
    Setup
  };
  inline std::string_view category(Outcome outcome) {
    switch (outcome) {
    case Outcome::Success:
      return "success";
    case Outcome::ExpectedEmpty:
      return "expected-empty";
    case Outcome::Cancelled:
      return "cancelled";
    case Outcome::Restricted:
      return "restricted";
    case Outcome::RateLimited:
      return "rate-limited";
    case Outcome::Transport:
      return "transport";
    case Outcome::Authentication:
      return "authentication";
    case Outcome::Permission:
      return "permission";
    case Outcome::Http:
      return "http";
    case Outcome::InvalidResponse:
      return "invalid-response";
    case Outcome::Setup:
      return "setup";
    }
    return "other";
  }
  inline std::uint64_t nextId() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  struct Record {
    std::uint64_t id;
    std::string_view action, method, path;
    long status = 0;
    int serviceCode = 0;
    std::int64_t durationMs = 0;
    int attempt = 1;
    bool retry = false;
    Outcome outcome = Outcome::Setup;
    int transportCode = 0;
  };
  inline std::string format(const Record& record) {
    constexpr int maximumServiceCode = 999999999, maximumTransportCode = 999, maximumAttempts = 3;
    constexpr std::int64_t maximumDurationMs = 24 * 60 * 60 * 1000;
    return std::format(
        "id=req-{:x} action={} method={} route={} status={} service_code={} category={} duration_ms={} "
        "attempt={}/{} retry={} transport_code={}",
        record.id, action(record.action), method(record.method), route(record.path),
        record.status >= 100 && record.status <= 599 ? record.status : 0,
        record.serviceCode >= 0 && record.serviceCode <= maximumServiceCode ? record.serviceCode : 0,
        category(record.outcome), std::clamp<std::int64_t>(record.durationMs, 0, maximumDurationMs),
        std::clamp(record.attempt, 1, maximumAttempts), maximumAttempts, record.retry ? "yes" : "no",
        record.transportCode >= 0 && record.transportCode <= maximumTransportCode ? record.transportCode : 0
    );
  }
} // namespace api::request_diagnostics
