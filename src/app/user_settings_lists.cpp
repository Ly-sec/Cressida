#include "app/billing_amount.h"
#include "app/client_application.h"
#include "app/theme.h"
#include "app/user_settings_form.h"
#include "ui/controls/input.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    using Json = nlohmann::json;
    using Section = api::UserSettingsSection;
    using Verb = api::UserSettingsVerb;
    constexpr std::uint64_t requestNamespace = 1ULL << 60;
    constexpr std::pair<Section, const char*> resources[] = {{Section::Account, "account"},
                                                             {Section::Relationships, "relationships"},
                                                             {Section::Subscriptions, "subscriptions"},
                                                             {Section::Payments, "payments"},
                                                             {Section::PurchaseHistory, "history"},
                                                             {Section::PaymentInvoice, "invoice"},
                                                             {Section::GiftCode, "gift"},
                                                             {Section::PaymentSources, "sources"},
                                                             {Section::Entitlements, "entitlements"},
                                                             {Section::DataPackage, "data"}};
    constexpr const char* packageBackends[] = {"Account",  "Analytics", "Activities", "Ads",
                                               "Messages", "Servers",   "Zendesk"};
    bool owned(const std::string& page) {
      return page == "subscriptions"
          || page == "payments"
          || page == "gifts"
          || page == "data"
          || page == "relationships";
    }
    std::string key(Section section) {
      for (const auto& [candidate, name] : resources)
        if (candidate == section)
          return name;
      return {};
    }
    std::string string(const Json& value, const char* field, std::string fallback = {}) {
      const auto at = value.find(field);
      return at != value.end() && at->is_string() ? at->get<std::string>() : fallback;
    }
    int integer(const Json& value, const char* field, int fallback = 0) {
      const auto at = value.find(field);
      return at != value.end() && at->is_number_integer() ? at->get<int>() : fallback;
    }
    bool flag(const Json& value, const char* field, bool fallback = false) {
      const auto at = value.find(field);
      return at != value.end() && at->is_boolean() ? at->get<bool>() : fallback;
    }
    Json rows(const Json& value, const char* field = "") {
      if (value.is_array())
        return value;
      const auto at = value.find(field);
      return at != value.end() && at->is_array() ? *at : Json::array();
    }
    std::string folded(std::string value) {
      for (auto& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return value;
    }
    std::optional<std::time_t> timestamp(const std::string& value) {
      if (value.size() < 19)
        return {};
      std::tm time{};
      std::istringstream stream(value.substr(0, 19));
      stream >> std::get_time(&time, "%Y-%m-%dT%H:%M:%S");
      if (stream.fail())
        return {};
      return timegm(&time);
    }
    std::string date(const std::string& value) {
      const auto parsed = timestamp(value);
      if (!parsed)
        return value.empty() ? "Not provided" : value;
      std::tm local{};
      localtime_r(&*parsed, &local);
      std::ostringstream stream;
      stream << std::put_time(&local, "%d %b %Y, %H:%M");
      return stream.str();
    }
    std::string money(const Json& value) {
      const auto amount = value.find("amount");
      if (amount == value.end()
          || !amount->is_number_integer()
          || (amount->is_number_unsigned() && amount->get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)))
        return "Amount unavailable";
      const auto result = billingAmount(amount->get<std::int64_t>(), string(value, "currency"));
      return result.value_or("Amount unavailable");
    }
    std::string name(const Json& relation) {
      const auto user = relation.value("user", Json::object());
      return string(user, "global_name", string(user, "username", "Discord user"));
    }
    std::string giftCode(std::string value) {
      const auto first = value.find_first_not_of(" \t\r\n"), last = value.find_last_not_of(" \t\r\n");
      if (first == std::string::npos)
        return {};
      value = value.substr(first, last - first + 1);
      for (const auto* prefix :
           {"https://discord.gift/", "http://discord.gift/", "discord.gift/", "https://discord.com/gifts/"})
        if (value.starts_with(prefix)) {
          value = value.substr(std::char_traits<char>::length(prefix));
          break;
        }
      if (const auto query = value.find_first_of("?#"); query != std::string::npos)
        value.resize(query);
      return api::userGiftCodeBrowserUrl(value).empty() ? "" : value;
    }
    std::string relationship(int type) {
      switch (type) {
      case 1:
        return "Friend";
      case 2:
        return "Blocked";
      case 3:
        return "Incoming request";
      case 4:
        return "Outgoing request";
      case 5:
        return "Implicit relationship";
      case 6:
        return "Suggested friend";
      default:
        return "Relationship";
      }
    }
    Json record(const UserSettingsListsState& state, const std::string& resource, const std::string& id) {
      const auto at = state.records.find(resource);
      if (at == state.records.end())
        return Json::object();
      for (const auto& value : rows(at->second, at->second.is_object() ? "receipts" : ""))
        if (string(value, "id") == id)
          return value;
      return Json::object();
    }
  } // namespace

  void ClientApplication::resetUserSettingsLists() {
    auto& state = m_userSettingsLists;
    const auto serial = state.serial, generation = state.generation + 1;
    if (state.mfaRequest && !m_demo)
      m_client.clearUserSettingsSecrets();
    state = {};
    state.serial = serial;
    state.generation = generation;
    for (const auto& [id, input] : m_inputs)
      if (id.starts_with("user-account:lists:"))
        input->setValue("");
  }

  bool ClientApplication::requestUserSettingsList(api::UserSettingsRequest request) {
    auto& state = m_userSettingsLists;
    if (!m_settingsOpen || !owned(m_settingsPage) || (!m_demo && !m_connected))
      return false;
    const auto cache =
        key(request.section) + (request.section == Section::PaymentInvoice ? ":" + request.resourceId : "");
    if (request.verb == Verb::Get && state.loading.contains(cache))
      return false;
    if (cache.empty() || (!request.mfaChallenge && !api::userSettingsHttpRequest(request))) {
      state.error = "This action could not be validated. Refresh the page and try again.";
      m_window.invalidate();
      return false;
    }
    request.request = requestNamespace | ++state.serial;
    if (!m_demo && !m_client.userSettings(request)) {
      state.error = "This action could not be started. Check the connection and try again.";
      if (request.verb == Verb::Get)
        state.errors[cache] = state.error;
      m_window.invalidate();
      return false;
    }
    state.pending.emplace(
        request.request,
        UserSettingsListPending{
            api::publicUserSettingsRequest(request), state.generation, cache, m_userDirectory.relationshipRevision(),
            state.giftVersion
        }
    );
    if (request.verb == Verb::Get)
      state.loading.insert(cache);
    else
      state.busy = true;
    state.error.clear();
    state.errors.erase(cache);
    state.status.clear();
    m_window.invalidate();
    return true;
  }

  void ClientApplication::loadUserSettingsLists() {
    auto& state = m_userSettingsLists;
    if (state.page != m_settingsPage) {
      if (state.mfaRequest && !m_demo)
        m_client.clearUserSettingsSecrets();
      for (const auto& [id, input] : m_inputs)
        if (id.starts_with("user-account:lists:"))
          input->setValue("");
      ++state.generation;
      state.page = m_settingsPage;
      state.pending.clear();
      state.loading.clear();
      state.search.clear();
      state.nicknameTarget.clear();
      state.nickname.clear();
      state.requestUsername.clear();
      state.giftCode.clear();
      state.invoicePayment.clear();
      ++state.giftVersion;
      state.giftPreviewVersion = 0;
      state.giftRedeemed = false;
      state.confirmationRequest.reset();
      state.mfaRequest.reset();
      state.mfaChallenge = 0;
      state.mfaCode.clear();
      state.mfaMethods.clear();
      state.mfaMethod.clear();
      state.confirmation.clear();
      state.status.clear();
      state.error.clear();
      state.busy = false;
      state.relationshipFilter = 0;
    }
    if (!owned(state.page))
      return;
    if (m_demo) {
      prepareUserSettingsListsDemo();
      return;
    }
    const auto load = [&](Section section) {
      if (state.loading.contains(key(section)))
        return;
      api::UserSettingsRequest request;
      request.section = section;
      requestUserSettingsList(std::move(request));
    };
    if (state.page == "subscriptions") {
      load(Section::Subscriptions);
      load(Section::Entitlements);
    } else if (state.page == "payments") {
      load(Section::PurchaseHistory);
      load(Section::PaymentSources);
    } else if (state.page == "gifts")
      load(Section::Entitlements);
    else if (state.page == "relationships")
      load(Section::Relationships);
    else {
      load(Section::Account);
      load(Section::DataPackage);
    }
  }

  bool ClientApplication::userSettingsListsResult(const api::Event& event) {
    if (!event.userSettingsRequest || !(event.userSettingsRequest->request & requestNamespace))
      return false;
    auto& state = m_userSettingsLists;
    const auto& request = *event.userSettingsRequest;
    const auto at = state.pending.find(request.request);
    if (at == state.pending.end())
      return true;
    const auto pending = at->second;
    if (!api::sameUserSettingsAction(request, pending.request))
      return true;
    state.pending.erase(at);
    if (pending.generation != state.generation || !m_settingsOpen || state.page != m_settingsPage)
      return true;
    state.loading.erase(pending.cache);
    if (request.section == Section::GiftCode && request.verb == Verb::Get && pending.giftVersion != state.giftVersion)
      return true;
    state.busy =
        std::ranges::any_of(state.pending, [](const auto& entry) { return entry.second.request.verb != Verb::Get; });
    if (!state.busy && state.status == "Wait for the current account operation to finish before changing pages.")
      state.status.clear();
    if (event.userSettingsMfaChallenge) {
      const bool again = state.mfaChallenge != 0;
      ++state.generation;
      std::erase_if(state.pending, [](const auto& item) { return item.second.request.verb == Verb::Get; });
      state.loading.clear();
      state.mfaRequest = pending.request;
      state.mfaChallenge = event.userSettingsMfaChallenge;
      state.mfaMethods = event.userSettingsMfaMethods;
      state.mfaMethod = std::ranges::find(state.mfaMethods, "totp") != state.mfaMethods.end() ? "totp"
          : std::ranges::find(state.mfaMethods, "password") != state.mfaMethods.end()         ? "password"
                                                                                              : "";
      state.mfaCode.clear();
      for (const auto& [id, input] : m_inputs)
        if (id.starts_with("user-account:lists:") && id.ends_with(":verification"))
          input->setValue("");
      state.error = again ? event.error : "";
      m_settingsScroll = 0;
      m_scrolling.stop(m_settingsScroll);
      m_window.invalidate();
      return true;
    }
    if (!event.error.empty()) {
      state.error = event.error;
      if (request.verb == Verb::Get)
        state.errors[pending.cache] = event.error;
      m_window.invalidate();
      return true;
    }
    if (request.section == Section::Relationships) {
      api::UserUpdates updates;
      if (request.verb == Verb::Get && pending.directoryRevision == m_userDirectory.relationshipRevision()) {
        updates.relationshipsReset = true;
        for (const auto& row : rows(event.userSettings)) {
          const auto id = string(row, "id");
          if (!api::validSnowflake(id))
            continue;
          auto& user = updates.users[id];
          user.relationship = integer(row, "type");
          user.nickname = string(row, "nickname");
          user.ignored = flag(row, "user_ignored");
        }
      } else if (request.verb == Verb::Delete) {
        auto& user = updates.users[request.resourceId];
        user.relationship = 0;
        user.nickname = "";
        user.ignored = false;
      } else if (request.verb == Verb::Update) {
        auto& user = updates.users[request.resourceId];
        if (request.values.contains("nickname"))
          user.nickname = string(request.values, "nickname");
        if (request.values.contains("type")) {
          user.relationship = integer(request.values, "type");
          if (*user.relationship == 2)
            user.nickname = "";
        }
        if (request.values.contains("user_ignored"))
          user.ignored = flag(request.values, "user_ignored");
      }
      m_userDirectory.apply(updates);
      m_messageLayouts.clear();
    }
    if (request.verb == Verb::Get) {
      if (request.section == Section::PurchaseHistory && !request.cursor.empty()) {
        auto& receipts = state.records[pending.cache]["receipts"];
        if (!receipts.is_array())
          receipts = Json::array();
        for (const auto& next : rows(event.userSettings, "receipts"))
          if (std::ranges::none_of(receipts, [&](const auto& item) {
                return string(item, "id") == string(next, "id");
              }))
            receipts.push_back(next);
        state.records[pending.cache]["cursor"] = event.userSettings.value("cursor", Json());
      } else if (!request.cursor.empty() && state.records.contains(pending.cache) && event.userSettings.is_array()) {
        auto& records = state.records[pending.cache];
        if (!records.is_array())
          records = Json::array();
        for (const auto& next : event.userSettings) {
          const auto id = string(next, "id");
          if (id.empty() || std::ranges::none_of(records, [&](const auto& item) { return string(item, "id") == id; }))
            records.push_back(next);
        }
      } else
        state.records[pending.cache] = event.userSettings;
      if (request.section == Section::GiftCode) {
        state.giftPreviewVersion = pending.giftVersion;
        state.giftRedeemed = false;
      }
      state.loaded.insert(pending.cache);
      state.more[pending.cache] = event.userSettingsMore;
      state.cursors[pending.cache] = event.userSettingsCursor;
    } else {
      state.confirmationRequest.reset();
      state.confirmation.clear();
      state.mfaRequest.reset();
      state.mfaChallenge = 0;
      state.mfaCode.clear();
      for (const auto& [id, input] : m_inputs)
        if (id.starts_with("user-account:lists:") && id.ends_with(":verification"))
          input->setValue("");
      state.status = request.section == Section::GiftCode ? "Gift redeemed." : "Changes saved.";
      if (request.section == Section::GiftCode) {
        state.giftRedeemed = true;
        state.giftCode.clear();
        ++state.giftVersion;
        for (const auto& [id, input] : m_inputs)
          if (id.starts_with("user-account:lists:") && id.ends_with(":gift-code"))
            input->setValue("");
      }
      state.nicknameTarget.clear();
      state.nickname.clear();
      state.requestUsername.clear();
      api::UserSettingsRequest refresh;
      refresh.section = request.section == Section::GiftCode ? Section::Entitlements : request.section;
      if (requestUserSettingsList(std::move(refresh)))
        state.status = request.section == Section::GiftCode ? "Gift redeemed." : "Changes saved.";
      // Keep relationship and privacy presentation consistent with the acknowledged account state.
      if (request.section == Section::Relationships)
        m_messageLayouts.clear();
    }
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::userSettingsListsAction(const std::string& action) {
    if (!action.starts_with("lists:"))
      return false;
    auto& state = m_userSettingsLists;
    if (!m_settingsOpen || !owned(m_settingsPage) || state.page != m_settingsPage)
      return true;
    const auto id = action.substr(6);
    if (id == "cancel") {
      if (state.busy)
        return true;
      ++state.generation;
      if (state.mfaRequest && !m_demo)
        m_client.clearUserSettingsSecrets();
      state.confirmationRequest.reset();
      state.mfaRequest.reset();
      state.mfaChallenge = 0;
      state.confirmation.clear();
      state.mfaCode.clear();
      state.error.clear();
      for (const auto& [field, input] : m_inputs)
        if (field.starts_with("user-account:lists:") && field.ends_with(":verification"))
          input->setValue("");
      return true;
    }
    if (state.busy)
      return true;
    if (id.starts_with("browser:")) {
      const auto page = id.substr(8);
      const auto section = page == "subscriptions" ? Section::Subscriptions
          : page == "gifts"                        ? Section::Entitlements
                                                   : Section::PaymentSources;
      const auto url = api::userSettingsBrowserUrl(section);
      if (!url.empty())
        activate({0, 0, 0, 0, Action::OpenUrl, url});
      return true;
    }
    if ((state.mfaRequest || state.confirmationRequest) && id != "verify" && id != "confirm")
      return true;
    if (id == "verify") {
      if (!state.mfaRequest || state.mfaCode.empty() || state.mfaMethod.empty())
        return true;
      auto request = *state.mfaRequest;
      request.mfaChallenge = state.mfaChallenge;
      request.mfaType = state.mfaMethod;
      request.mfaCode = state.mfaCode;
      state.mfaCode.clear();
      for (const auto& [field, input] : m_inputs)
        if (field.starts_with("user-account:lists:") && field.ends_with(":verification"))
          input->setValue("");
      requestUserSettingsList(std::move(request));
      return true;
    }
    if (id == "confirm") {
      if (state.confirmationRequest) {
        auto request = *state.confirmationRequest;
        if (request.section == Section::GiftCode) {
          if (state.confirmationGiftVersion != state.giftVersion || state.giftPreviewVersion != state.giftVersion)
            return true;
          request.giftCode = giftCode(state.giftCode);
        }
        requestUserSettingsList(std::move(request));
      }
      return true;
    }
    if (id == "reload") {
      loadUserSettingsLists();
      return true;
    }
    const auto queueConfirmation = [&](api::UserSettingsRequest request, const std::string& title,
                                       const std::string& label) {
      state.confirmationRequest = api::publicUserSettingsRequest(std::move(request));
      state.confirmationGiftVersion = state.giftVersion;
      state.confirmation = title;
      state.confirmLabel = label;
      state.error.clear();
      m_settingsScroll = 0;
      m_scrolling.stop(m_settingsScroll);
      m_window.invalidate();
    };
    if (id == "gift-preview") {
      api::UserSettingsRequest request;
      request.section = Section::GiftCode;
      request.giftCode = giftCode(state.giftCode);
      requestUserSettingsList(std::move(request));
    } else if (id == "gift-redeem") {
      if (state.giftPreviewVersion != state.giftVersion || state.giftRedeemed || !state.records.contains("gift"))
        return true;
      const auto& gift = state.records.at("gift");
      const auto expires = timestamp(string(gift, "expires_at"));
      if (flag(gift, "redeemed")
          || flag(gift, "requires_payment_source")
          || (expires && *expires < std::time(nullptr))
          || (integer(gift, "max_uses") > 0 && integer(gift, "uses") >= integer(gift, "max_uses")))
        return true;
      api::UserSettingsRequest request;
      request.section = Section::GiftCode;
      request.verb = Verb::Create;
      request.values["confirm"] = true;
      queueConfirmation(
          std::move(request),
          "Redeem this gift on your current Discord account? Review the gift name and duration before continuing.",
          "Redeem gift"
      );
    } else if (id == "gift-browser") {
      const auto url = api::userGiftCodeBrowserUrl(giftCode(state.giftCode));
      if (!url.empty())
        activate({0, 0, 0, 0, Action::OpenUrl, url});
    } else if (id == "invoice-close") {
      state.invoicePayment.clear();
    } else if (id.starts_with("invoice:")) {
      state.invoicePayment = id.substr(8);
      if (record(state, "history", state.invoicePayment).empty())
        return true;
      api::UserSettingsRequest request;
      request.section = Section::PaymentInvoice;
      request.resourceId = state.invoicePayment;
      requestUserSettingsList(std::move(request));
      m_settingsScroll = 0;
      m_scrolling.stop(m_settingsScroll);
    } else if (id.starts_with("invoice-link:")) {
      const auto n = id.substr(13);
      const auto& invoice = state.records["invoice:" + state.invoicePayment];
      const auto links = rows(invoice, "refundInvoiceLinks");
      const auto url = n == "original" ? string(invoice, "invoiceLink")
          : n.starts_with("refund-")
              && n.size() > 7
              && std::ranges::all_of(n.substr(7), [](unsigned char c) { return c >= '0' && c <= '9'; })
              && n.size() < 12
          ? ([&]() {
              const auto index = std::stoul(n.substr(7));
              return index < links.size() && links[index].is_string() ? links[index].get<std::string>() : "";
            })()
          : "";
      if (!url.empty())
        activate({0, 0, 0, 0, Action::OpenUrl, url});
    } else if (id.starts_with("refund:")) {
      const auto receipt = record(state, "history", id.substr(7));
      if (receipt.contains("refund") && flag(receipt["refund"], "eligible"))
        activate({0, 0, 0, 0, Action::OpenUrl, api::userPaymentRefundUrl()});
    } else if (id == "request-data") {
      api::UserSettingsRequest request;
      request.section = Section::DataPackage;
      request.verb = Verb::Create;
      request.values["backends"] = Json::array();
      for (const auto* backend : packageBackends)
        if (!state.excludedBackends.contains(backend))
          request.values["backends"].push_back(backend);
      queueConfirmation(
          std::move(request),
          "Request your Discord data? Discord sends the download link to your verified email address. Preparation can "
          "take up to 30 days; the download link expires after 30 days.",
          "Request data"
      );
    } else if (id == "friend-request") {
      api::UserSettingsRequest request;
      request.section = Section::Relationships;
      request.verb = Verb::Create;
      request.values["username"] = state.requestUsername;
      requestUserSettingsList(std::move(request));
    } else if (id == "nickname-save") {
      api::UserSettingsRequest request;
      request.section = Section::Relationships;
      request.verb = Verb::Update;
      request.resourceId = state.nicknameTarget;
      request.values["nickname"] = state.nickname.empty() ? Json(nullptr) : Json(state.nickname);
      requestUserSettingsList(std::move(request));
    } else if (id == "nickname-cancel") {
      state.nicknameTarget.clear();
      state.nickname.clear();
    } else if (id.starts_with("more:")) {
      const auto cache = id.substr(5);
      for (const auto& [section, name] : resources)
        if (cache == name && state.more[cache] && !state.loading.contains(cache)) {
          api::UserSettingsRequest request;
          request.section = section;
          request.cursor = state.cursors[cache];
          requestUserSettingsList(std::move(request));
          break;
        }
    } else {
      const auto colon = id.find(':');
      if (colon == std::string::npos)
        return true;
      const auto verb = id.substr(0, colon), resource = id.substr(colon + 1);
      api::UserSettingsRequest request;
      request.resourceId = resource;
      if (verb == "cancel-subscription") {
        if (record(state, "subscriptions", resource).empty())
          return true;
        request.section = Section::Subscriptions;
        request.verb = Verb::Delete;
        queueConfirmation(
            std::move(request),
            "Cancel this subscription? Its remaining paid benefits last until the end of the billing period. Recurring "
            "renewal will be stopped.",
            "Cancel subscription"
        );
      } else if (verb == "source-default") {
        if (record(state, "sources", resource).empty())
          return true;
        request.section = Section::PaymentSources;
        request.verb = Verb::Update;
        request.values["default"] = true;
        requestUserSettingsList(std::move(request));
      } else if (verb == "source-remove") {
        if (record(state, "sources", resource).empty())
          return true;
        request.section = Section::PaymentSources;
        request.verb = Verb::Delete;
        queueConfirmation(
            std::move(request),
            "Remove this payment method from your Discord account? A method used by an active subscription may need to "
            "be changed on that subscription first.",
            "Remove method"
        );
      } else if (verb == "nickname") {
        const auto relation = record(state, "relationships", resource);
        if (relation.empty())
          return true;
        state.nicknameTarget = resource;
        state.nickname = string(relation, "nickname");
        m_settingsScroll = 0;
        m_scrolling.stop(m_settingsScroll);
      } else {
        const auto relation = record(state, "relationships", resource);
        if (relation.empty())
          return true;
        request.section = Section::Relationships;
        const auto type = integer(relation, "type");
        if (verb == "ignore" || verb == "unignore") {
          request.verb = Verb::Update;
          request.values["user_ignored"] = verb == "ignore";
          requestUserSettingsList(std::move(request));
        } else if (verb == "accept" && type == 3) {
          request.verb = Verb::Update;
          request.values["type"] = 1;
          requestUserSettingsList(std::move(request));
        } else if (verb == "block" && type != 2) {
          request.verb = Verb::Update;
          request.values["type"] = 2;
          queueConfirmation(
              std::move(request),
              "Block " + name(relation) + "? This removes your friendship and stops direct messages from this account.",
              "Block"
          );
        } else if (verb == "remove") {
          request.verb = Verb::Delete;
          queueConfirmation(
              std::move(request),
              type == 2 ? "Unblock "
                      + name(relation)
                      + "? They will be able to send requests according to your privacy settings."
                  : type == 1 ? "Remove " + name(relation) + " from your friends?"
                  : type == 3 ? "Decline this friend request?"
                              : "Cancel this friend request?",
              type == 2       ? "Unblock"
                  : type == 3 ? "Decline"
                              : "Remove"
          );
        }
      }
    }
    return true;
  }

  bool ClientApplication::drawUserSettingsLists(UserSettingsForm& form) {
    auto& state = m_userSettingsLists;
    if (!owned(m_settingsPage))
      return false;
    const auto fieldId = [&](const std::string& field) {
      return "user-account:lists:" + std::to_string(state.generation) + ':' + field;
    };
    const auto action = [&](const std::string& title, const std::string& description, const std::string& caption,
                            const std::string& id, ButtonVariant variant = ButtonVariant::Default,
                            bool enabled = true) {
      form.rowButton(form.actionRow(title, description), caption, "lists:" + id, variant, 128, enabled && !state.busy);
    };
    struct ListButton {
      std::string caption, id;
      ButtonVariant variant = ButtonVariant::Default;
    };
    const auto buttons = [&](const std::vector<ListButton>& items) {
      float x = 0, y = form.y;
      for (const auto& item : items) {
        const auto measured = m_window.target()
                                  .renderer()
                                  .measureText(item.caption, Style::fontSizeDescription, FontWeight::Medium)
                                  .width;
        const auto w = std::min(form.width, std::max(76.0F, std::ceil(measured) + 20));
        if (x && x + w > form.width) {
          x = 0;
          y += (Style::space2Xl + Style::spaceXs);
        }
        form.buttonControl(
            form.node, x, y, w, item.caption, "lists:" + item.id, item.variant,
            !state.busy && !state.mfaRequest && !state.confirmationRequest
        );
        x += w + Style::spaceSm;
      }
      if (!items.empty())
        form.y = y + (Style::spaceSection - Style::spaceXxs);
    };
    if (state.mfaRequest) {
      form.section("Verify your identity");
      std::vector<std::string> methods, names;
      for (const auto& method : state.mfaMethods)
        if (method == "totp" || method == "password") {
          methods.push_back(method);
          names.push_back(method == "totp" ? "Authenticator code" : "Account password");
        }
      if (methods.empty())
        form.row(
            "Verification unavailable",
            "This action requires a verification method that is not supported in this Cressida build."
        );
      else {
        const auto at = std::ranges::find(methods, state.mfaMethod);
        form.choose(
            fieldId("method"), "Verification method", "Choose how to confirm this exact account action.", names,
            at == methods.end() ? 0 : at - methods.begin(),
            [this, methods](std::size_t index) {
              ++m_userSettingsLists.generation;
              m_userSettingsLists.mfaMethod = methods[index];
              m_userSettingsLists.mfaCode.clear();
              for (const auto& [id, input] : m_inputs)
                if (id.starts_with("user-account:lists:") && id.ends_with(":verification"))
                  input->setValue("");
              m_window.invalidate();
            },
            !state.busy
        );
        form.field(
            fieldId("verification"), state.mfaMethod == "password" ? "Password" : "One-time code",
            "Verification inputs are cleared after submission or cancellation.", state.mfaCode,
            [this](const std::string& value) { m_userSettingsLists.mfaCode = value; }, 32, true, !state.busy
        );
        form.button("Verify", "lists:verify", ButtonVariant::Primary, 112, !state.busy && !state.mfaCode.empty());
      }
      form.button("Cancel", "lists:cancel", ButtonVariant::Default, 88, !state.busy);
    } else if (state.confirmationRequest) {
      form.section("Confirm action");
      form.row("Review this change", state.confirmation);
      form.button(
          state.busy ? "Saving…" : state.confirmLabel, "lists:confirm",
          (state.confirmationRequest->section == Section::DataPackage
           || state.confirmationRequest->section == Section::GiftCode)
              ? ButtonVariant::Primary
              : ButtonVariant::Destructive,
          160, !state.busy
      );
      form.button("Cancel", "lists:cancel", ButtonVariant::Default, 88, !state.busy);
    }
    if (!state.error.empty()
        && std::ranges::none_of(state.errors, [&](const auto& item) { return item.second == state.error; }))
      form.row("Could not complete the action", state.error);
    if (!state.status.empty())
      form.row(state.busy ? "Saving" : "Saved", state.status);
    for (const auto& cache : state.loading)
      if (cache != "gift" && !cache.starts_with("invoice:"))
        form.row("Loading " + cache, "Your current account data is being loaded.");
    for (const auto& [cache, error] : state.errors)
      if (cache != "gift" && !cache.starts_with("invoice:"))
        action("Could not load " + cache, error, "Retry", "reload");
    if (m_settingsPage == "payments" && !state.invoicePayment.empty()) {
      form.section("Invoice documents");
      const auto cache = "invoice:" + state.invoicePayment;
      if (state.loading.contains(cache))
        form.row("Loading invoices", "Checking available invoice and refund documents for this purchase.");
      else if (state.errors.contains(cache))
        action("Could not load invoices", state.errors.at(cache), "Retry", "invoice:" + state.invoicePayment);
      else {
        const auto invoice = state.records.contains(cache) ? state.records.at(cache) : Json::object();
        const auto links = rows(invoice, "refundInvoiceLinks");
        if (string(invoice, "invoiceLink").empty() && links.empty())
          form.row("No documents available", "Discord has no downloadable invoice documents for this purchase.");
        if (!string(invoice, "invoiceLink").empty())
          form.button("Open invoice", "lists:invoice-link:original", ButtonVariant::Default, 132);
        for (std::size_t n = 0; n < links.size(); ++n)
          form.button(
              "Refund invoice " + std::to_string(n + 1), "lists:invoice-link:refund-" + std::to_string(n),
              ButtonVariant::Default, 156
          );
      }
      form.button("Close documents", "lists:invoice-close", ButtonVariant::Default, 144);
    }
    if (m_settingsPage == "data") {
      form.section("How Discord uses your data");
      for (const auto& [setting, title, description] :
           std::initializer_list<std::tuple<const char*, const char*, const char*>>{
               {"non_spam_retraining", "Help improve message safety",
                "Allow Discord to use eligible non-spam messages to improve its spam detection systems."},
               {"quests_data_opted_out", "Limit personalized quests",
                "Opt out of using account data to personalize Discord quests."},
               {"drops_opted_out", "Opt out of drops", "Stop participating in Discord activity-based drops."},
               {"message_promotions_opted_out", "Hide message promotions",
                "Opt out of promotional messages on supported Discord surfaces."}
           }) {
        const bool checked = m_settingsDraft.contains(setting) && m_settingsDraft.at(setting) != 0;
        form.toggle(
            fieldId(setting), title, description, checked,
            [this, setting = std::string(setting)](bool) {
              activate({0, 0, 0, 0, Action::AccountPreference, setting});
              m_window.invalidate();
            },
            m_settingsLoaded && !m_loadingSettings && !m_savingSettings
        );
      }
      form.section("Request a data package");
      const auto harvest = state.records["data"];
      const auto created = string(harvest, "created_at"), completed = string(harvest, "completed_at");
      if (!created.empty())
        form.row(
            "Latest request",
            "Requested "
                + date(created)
                + (completed.empty() ? ". Your package is still being prepared."
                                     : ". Completed " + date(completed) + '.')
        );
      for (const auto* backend : packageBackends) {
        const auto title = std::string(backend) == "Zendesk" ? "Support tickets"
            : std::string(backend) == "Ads"                  ? "Advertising data"
                                                             : std::string(backend);
        form.toggle(
            fieldId("backend:" + std::string(backend)), title,
            "Include this category in the downloadable data package.", !state.excludedBackends.contains(backend),
            [this, backend = std::string(backend)](bool value) {
              if (value)
                m_userSettingsLists.excludedBackends.erase(backend);
              else
                m_userSettingsLists.excludedBackends.insert(backend);
              m_window.invalidate();
            },
            !state.busy
        );
      }
      const auto account = state.records["account"];
      const bool verified = flag(account, "verified");
      const auto requested = timestamp(created);
      const bool cooling = requested && std::time(nullptr) - *requested < 30 * 24 * 60 * 60;
      const auto detail = !verified ? "Verify your email address in My Account before requesting data."
          : cooling
          ? "Discord permits one data request every 30 days. Your previous request was made on " + date(created) + '.'
          : "Discord emails your selected data within 30 days. The download is available for 30 days after delivery.";
      action(
          "Download your Discord data", detail, "Request data", "request-data", ButtonVariant::Primary,
          verified && !cooling && state.excludedBackends.size() < std::size(packageBackends)
      );
    } else if (m_settingsPage == "relationships") {
      form.section("Friends and requests");
      form.field(
          fieldId("request-username"), "Add a friend", "Enter their exact Discord username.", state.requestUsername,
          [this](const std::string& value) { m_userSettingsLists.requestUsername = value; }, 32, false, !state.busy
      );
      form.button(
          "Send friend request", "lists:friend-request", ButtonVariant::Primary, 156,
          !state.busy && !state.requestUsername.empty()
      );
      if (!state.nicknameTarget.empty()) {
        form.section("Friend nickname");
        form.field(
            fieldId("nickname"), "Nickname",
            "This name is visible only to you. Leave it empty to restore their profile name.", state.nickname,
            [this](const std::string& value) { m_userSettingsLists.nickname = value; }, 32, false, !state.busy
        );
        form.button("Save nickname", "lists:nickname-save", ButtonVariant::Primary, 132, !state.busy);
        form.button("Cancel", "lists:nickname-cancel", ButtonVariant::Default, 88, !state.busy);
      }
      form.section("Your relationships");
      form.choose(
          fieldId("filter"), "Show", "Filter friends, incoming and outgoing requests, or blocked accounts.",
          {"All", "Friends", "Incoming requests", "Outgoing requests", "Blocked", "Ignored"}, state.relationshipFilter,
          [this](std::size_t index) {
            m_userSettingsLists.relationshipFilter = index;
            m_window.invalidate();
          }
      );
      form.field(
          fieldId("search"), "Search people", "Match profile names, usernames or your private nicknames.", state.search,
          [this](const std::string& value) {
            m_userSettingsLists.search = value;
            m_window.invalidate();
          }
      );
      std::size_t visible = 0;
      for (const auto& relation : rows(state.records["relationships"])) {
        const auto id = string(relation, "id");
        const auto type = integer(relation, "type");
        constexpr int filters[] = {0, 1, 3, 4, 2};
        if (state.relationshipFilter == 5 ? !flag(relation, "user_ignored")
                                          : state.relationshipFilter && type != filters[state.relationshipFilter])
          continue;
        const auto user = relation.value("user", Json::object());
        const auto title = name(relation), nickname = string(relation, "nickname");
        if (!state.search.empty()
            && !folded(title + ' ' + string(user, "username") + ' ' + nickname).contains(folded(state.search)))
          continue;
        ++visible;
        form.row(
            nickname.empty() ? title : nickname,
            '@'
                + string(user, "username", title)
                + " · "
                + relationship(type)
                + (nickname.empty() ? "" : " · Profile name: " + title)
        );
        std::vector<ListButton> actions;
        if (type == 3)
          actions.push_back({"Accept", "accept:" + id, ButtonVariant::Primary});
        if (type == 1)
          actions.push_back({"Edit nickname", "nickname:" + id});
        actions.push_back(
            {type == 2       ? "Unblock"
                 : type == 3 ? "Decline"
                 : type == 4 ? "Cancel request"
                             : "Remove friend",
             "remove:" + id, ButtonVariant::Destructive}
        );
        actions.push_back(
            {flag(relation, "user_ignored") ? "Stop ignoring" : "Ignore",
             std::string(flag(relation, "user_ignored") ? "unignore:" : "ignore:") + id}
        );
        if (type != 2)
          actions.push_back({"Block", "block:" + id, ButtonVariant::Destructive});
        buttons(actions);
      }

      if (!visible && !state.loading.contains("relationships"))
        form.row(
            "No matching people",
            state.search.empty() ? "Friends, friend requests and blocked accounts will appear here."
                                 : "Try a different search or filter."
        );
    } else if (m_settingsPage == "subscriptions") {
      form.section("Plans and purchases");
      action(
          "Manage plans",
          "Choose paid plans, authorize purchases or resume a cancelled subscription in Discord's billing flow.",
          "Open billing", "browser:subscriptions"
      );
      form.section("Your subscriptions");
      const auto subscriptions = rows(state.records["subscriptions"]);
      if (subscriptions.empty() && !state.loading.contains("subscriptions"))
        form.row("No subscriptions", "This account has no subscriptions returned by Discord.");
      for (const auto& subscription : subscriptions) {
        const auto id = string(subscription, "id");
        const bool cancelled = !string(subscription, "canceled_at").empty() || integer(subscription, "status") == 3;
        const auto plan = subscription.value("plan", Json::object());
        static const char* states[] = {"Not started", "Active", "Payment overdue", "Cancelled", "Unpaid", "Paused"};
        const auto status = integer(subscription, "status");
        const auto description = std::string(status >= 0 && status < 6 ? states[status] : "Subscription")
            + " · Current billing period ends "
            + date(string(subscription, "current_period_end"))
            + '.';
        const auto title = string(subscription, "name", string(plan, "name", "Discord subscription"));
        if (cancelled)
          form.row(title, description);
        else
          action(
              title, description + " Cancel to stop future renewal and retain the remaining paid period.", "Cancel",
              "cancel-subscription:" + id, ButtonVariant::Destructive
          );
      }
    } else if (m_settingsPage == "payments") {
      form.section("Payment setup");
      action(
          "Add or update a method",
          "Authorize a new payment method or update processor-managed details in Discord's secure billing flow.",
          "Open billing", "browser:payments"
      );
      form.section("Payment methods");
      const auto sources = rows(state.records["sources"]);
      if (sources.empty() && !state.loading.contains("sources"))
        form.row("No saved methods", "This account has no saved payment methods returned by Discord.");
      for (const auto& source : sources) {
        const auto id = string(source, "id"), brand = string(source, "brand", "Payment method"),
                   last = string(source, "last_4");
        std::string description = flag(source, "default") ? "Default for new purchases." : "Saved payment method.";
        if (integer(source, "expires_month") && integer(source, "expires_year"))
          description += " Expires "
              + std::to_string(integer(source, "expires_month"))
              + '/'
              + std::to_string(integer(source, "expires_year"))
              + '.';
        if (flag(source, "invalid"))
          description += " This payment method needs to be updated before it can be used.";
        if (!string(source, "email").empty())
          description += ' ' + string(source, "email");
        form.row(brand + (last.empty() ? "" : " ···· " + last), description);
        std::vector<ListButton> actions;
        if (!flag(source, "default") && !flag(source, "invalid"))
          actions.push_back({"Make default", "source-default:" + id});
        actions.push_back({"Remove", "source-remove:" + id, ButtonVariant::Destructive});
        buttons(actions);
      }
      form.section("Payment history");
      const auto history = state.records.contains("history") ? state.records.at("history") : Json::object();
      const auto payments = rows(history, "receipts");
      if (payments.empty() && !state.loading.contains("history"))
        form.row("No payments", "Completed and pending Discord purchases will appear here.");
      for (const auto& receipt : payments) {
        const auto status = integer(receipt, "status");
        const std::string states[] = {"Processing",         "Pending",  "Paid",    "Failed", "Cancelled",
                                      "Partially refunded", "Refunded", "Reversed"};
        const auto sku = receipt.value("sku", Json::object());
        const auto total = receipt.contains("total") ? receipt["total"] : Json();
        const auto amount = money(Json{{"amount", total}, {"currency", string(receipt, "currency")}});
        const auto id = string(receipt, "id");
        form.row(
            string(receipt, "description", string(sku, "name", "Discord purchase")),
            amount
                + " · "
                + (status >= 0 && status < 8 ? states[status] : "Processing")
                + " · "
                + date(string(receipt, "created_at"))
        );
        std::vector<ListButton> actions;
        if (flag(receipt, "has_invoice_url") || flag(receipt, "has_refund_invoice_urls"))
          actions.push_back({"Invoices", "invoice:" + id});
        if (receipt.contains("refund") && flag(receipt["refund"], "eligible"))
          actions.push_back({"Request refund", "refund:" + id});
        buttons(actions);
      }
      if (state.more["history"])
        form.button(
            "Load more purchases", "lists:more:history", ButtonVariant::Default, 156, !state.loading.contains("history")
        );
    } else {
      form.section("Redeem a gift");
      form.field(
          fieldId("gift-code"), "Gift code",
          "Paste a gift code or Discord gift link. Review the gift before adding it to your account.", state.giftCode,
          [this](const std::string& value) {
            auto& state = m_userSettingsLists;
            if (value.size() > 2048) {
              state.error = "Enter a gift code or a Discord gift link up to 2048 characters.";
              return;
            }
            state.giftCode = value;
            ++state.giftVersion;
            state.giftPreviewVersion = 0;
            state.giftRedeemed = false;
            state.records.erase("gift");
            state.errors.erase("gift");
            state.error.clear();
            state.status.clear();
            m_window.invalidate();
          },
          32, false, !state.busy && !state.mfaRequest && !state.confirmationRequest
      );
      form.button(
          state.loading.contains("gift") ? "Checking…" : "Preview gift", "lists:gift-preview", ButtonVariant::Primary,
          132,
          !state.busy
              && !state.loading.contains("gift")
              && !giftCode(state.giftCode).empty()
              && !state.confirmationRequest
              && !state.mfaRequest
      );
      if (state.errors.contains("gift"))
        form.row("Could not preview the gift", state.errors.at("gift"));
      if (state.giftPreviewVersion == state.giftVersion && state.records.contains("gift")) {
        const auto gift = state.records.at("gift");
        const auto listing = gift.value("store_listing", Json::object());
        const auto sku = listing.value("sku", Json::object());
        const auto plan = gift.value("subscription_plan", Json::object());
        const auto application = gift.value("application", Json::object());
        const auto giver = gift.value("user", Json::object());
        const auto expires = timestamp(string(gift, "expires_at"));
        const bool expired = expires && *expires < std::time(nullptr);
        const bool exhausted = integer(gift, "max_uses") > 0 && integer(gift, "uses") >= integer(gift, "max_uses");
        const bool redeemed = flag(gift, "redeemed") || state.giftRedeemed;
        const bool paymentRequired = flag(gift, "requires_payment_source");
        std::string description = string(plan, "name");
        const auto count = integer(plan, "interval_count");
        const auto interval = integer(plan, "interval");
        if (count > 0 && interval >= 1 && interval <= 3) {
          const auto unit = interval == 1 ? "month" : interval == 2 ? "year" : "day";
          description +=
              (description.empty() ? "" : " · ") + std::to_string(count) + ' ' + unit + (count == 1 ? "" : "s");
        }
        if (integer(gift, "max_uses") > 0)
          description += (description.empty() ? "" : " · ")
              + std::to_string(std::max(0, integer(gift, "max_uses") - integer(gift, "uses")))
              + (integer(gift, "max_uses") - integer(gift, "uses") == 1 ? " redemption remaining"
                                                                        : " redemptions remaining");
        if (!string(giver, "username").empty())
          description += (description.empty() ? "" : " · ")
              + std::string("From ")
              + string(giver, "global_name", string(giver, "username"));
        if (!string(gift, "expires_at").empty())
          description +=
              (description.empty() ? "" : " · ") + std::string("Expires ") + date(string(gift, "expires_at"));
        form.row(string(sku, "name", string(application, "name", "Discord gift")), description);
        if (redeemed)
          form.row("Already redeemed", "This gift has already been redeemed on this account.");
        else if (expired || exhausted)
          form.row(
              "Gift unavailable", expired ? "This gift link has expired." : "This gift has no remaining redemptions."
          );
        else if (paymentRequired)
          action(
              "Payment authorization required",
              "Continue in Discord to review the terms and authorize a payment method before redeeming this gift.",
              "Continue", "gift-browser"
          );
        else
          form.button(
              "Redeem gift", "lists:gift-redeem", ButtonVariant::Primary, 132,
              !state.busy && !state.confirmationRequest && !state.mfaRequest
          );
      }
      form.section("Shop for gifts");
      action("Gift purchases", "Choose gifts and authorize purchases in Discord's shop.", "Open shop", "browser:gifts");
      form.section("Gifts and entitlements");
      const auto entitlements = rows(state.records["entitlements"]);
      if (entitlements.empty() && !state.loading.contains("entitlements"))
        form.row("No gifts or rewards", "Gifts and other entitlements associated with your account will appear here.");
      for (const auto& entitlement : entitlements) {
        const auto sku = entitlement.value("sku", Json::object());
        const auto title = string(sku, "name", string(entitlement, "name", "Discord entitlement"));
        std::string description =
            flag(entitlement, "consumed") ? "Already redeemed." : "Available on your Discord account.";
        if (!string(entitlement, "ends_at").empty())
          description += " Expires " + date(string(entitlement, "ends_at")) + '.';
        form.row(title, description);
      }
    }
    return true;
  }
} // namespace app

namespace app {
  void ClientApplication::prepareUserSettingsListsDemo() {
    auto& state = m_userSettingsLists;
    if (state.loaded.contains("demo"))
      return;
    state.records["account"] = {{"id", m_account.id}, {"verified", true}};
    state.records["relationships"] = Json::array(
        {{{"id", "1001"},
          {"type", 1},
          {"nickname", "Owl"},
          {"user", {{"id", "1001"}, {"username", "serein"}, {"global_name", "Serein"}}}},
         {{"id", "1002"}, {"type", 3}, {"user", {{"id", "1002"}, {"username", "aditya"}, {"global_name", "Aditya"}}}},
         {{"id", "1003"}, {"type", 2}, {"user", {{"id", "1003"}, {"username", "sample"}, {"global_name", "Sample"}}}},
         {{"id", "1004"}, {"type", 4}, {"user", {{"id", "1004"}, {"username", "yocraft"}, {"global_name", "Yocraft"}}}}}
    );
    state.records["subscriptions"] = Json::array(
        {{{"id", "2001"},
          {"type", 1},
          {"status", 1},
          {"currency", "eur"},
          {"current_period_end", "2026-11-18T09:30:00Z"},
          {"items", Json::array({{{"id", "2101"}, {"plan_id", "511651880837840896"}, {"quantity", 1}}})}}}
    );
    state.records["sources"] = Json::array(
        {{{"id", "3001"},
          {"type", 1},
          {"brand", "Visa"},
          {"last_4", "4242"},
          {"expires_month", 12},
          {"expires_year", 2028},
          {"default", false},
          {"invalid", false}},
         {{"id", "3002"}, {"type", 2}, {"email", "lysec@example.invalid"}, {"default", true}, {"invalid", false}}}
    );
    state.records["payments"] = Json::array(
        {{{"id", "4001"},
          {"amount", 999},
          {"currency", "eur"},
          {"status", 1},
          {"created_at", "2026-10-18T09:30:00Z"},
          {"description", "Nitro monthly subscription"}},
         {{"id", "4002"},
          {"amount", 499},
          {"currency", "eur"},
          {"status", 1},
          {"created_at", "2026-09-18T09:30:00Z"},
          {"description", "Nitro Basic gift"}}}
    );
    state.records["history"] = {
        {"receipts",
         Json::array(
             {{{"id", "4001"},
               {"total", 999},
               {"currency", "eur"},
               {"status", 2},
               {"created_at", "2026-10-18T09:30:00Z"},
               {"description", "Nitro monthly subscription"},
               {"has_invoice_url", true},
               {"has_refund_invoice_urls", true},
               {"refund", {{"eligible", true}, {"window", {{"days", 5}, {"days_since_purchase", 1}}}}},
               {"payments", Json::array({{{"id", "4001"}}})}},
              {{"id", "4002"},
               {"total", 499},
               {"currency", "eur"},
               {"status", 6},
               {"created_at", "2026-09-18T09:30:00Z"},
               {"description", "Nitro Basic gift"},
               {"has_invoice_url", true},
               {"refund", {{"eligible", false}}}}}
         )},
        {"cursor", "older-page+receipt="}
    };
    state.more["history"] = true;
    state.cursors["history"] = "older-page+receipt=";
    state.records["invoice:4001"] = {
        {"invoiceLink", "https://discord.com/invoice/demo"},
        {"refundInvoiceLinks", Json::array({"https://discord.com/invoice/demo-refund"})}
    };
    state.records["entitlements"] = Json::array(
        {{{"id", "5001"}, {"type", 1}, {"consumed", false}, {"sku", {{"id", "5101"}, {"name", "Nitro gift"}}}},
         {{"id", "5002"}, {"type", 1}, {"consumed", true}, {"sku", {{"id", "5102"}, {"name", "Avatar decoration"}}}}}
    );
    state.records["data"] = {
        {"id", "6001"}, {"created_at", "2026-01-18T10:30:00Z"}, {"completed_at", "2026-01-19T10:30:00Z"}
    };
    for (const auto& [section, name] : resources)
      state.loaded.insert(name);
    state.loaded.insert("demo");
    if (m_demoOverlay.find("gifts-preview") != std::string::npos) {
      state.giftCode = "DEMO-GIFT";
      state.records["gift"] = {
          {"uses", 0},
          {"max_uses", 1},
          {"redeemed", false},
          {"flags", 0},
          {"requires_payment_source", false},
          {"store_listing", {{"sku", {{"name", "Nitro gift"}}}}},
          {"subscription_plan", {{"name", "Nitro"}, {"interval", 1}, {"interval_count", 1}}},
          {"user", {{"username", "serein"}, {"global_name", "Serein"}}}
      };
      state.giftPreviewVersion = state.giftVersion;
    }
    if (m_demoOverlay.find("invoice") != std::string::npos)
      state.invoicePayment = "4001";
    if (m_demoOverlay.find("lists-error") != std::string::npos)
      state.errors["relationships"] = "The connection was interrupted. Retry to reload this list.";
    if (m_demoOverlay.find("lists-empty") != std::string::npos)
      state.records["relationships"] = Json::array();
  }

  void ClientApplication::demoUserSettingsListsStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    auto& state = m_userSettingsLists;
    const auto click = [&](const std::string& id) {
      const auto at = m_widgets.find(id);
      require(at != m_widgets.end(), "User settings list native control must exist");
      const auto* node = at->second;
      float x = node->width() / 2, y = node->height() / 2;
      for (const auto* parent = node; parent; parent = parent->parent()) {
        x += parent->x();
        y += parent->y();
      }
      m_window.click(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272);
      m_window.released();
    };
    const auto request = [&]() -> api::UserSettingsRequest {
      const auto at =
          std::ranges::find_if(state.pending, [](const auto& entry) { return entry.second.request.verb != Verb::Get; });
      require(at != state.pending.end(), "Account list action queues a typed mutation only after confirmation");
      return at->second.request;
    };
    const auto acknowledge = [&](Json data = Json::object(), std::string error = {}, bool mfa = false,
                                 bool reading = false) {
      api::Event event;
      event.operation = api::Operation::UserSettings;
      event.userSettingsRequest = reading ? state.pending.begin()->second.request : request();
      event.userSettings = std::move(data);
      if (event.userSettings.is_object()
          && event.userSettings.contains("cursor")
          && event.userSettings["cursor"].is_string()) {
        event.userSettingsMore = true;
        event.userSettingsCursor = event.userSettings["cursor"].get<std::string>();
      }
      event.error = std::move(error);
      if (!mfa && event.userSettingsRequest->mfaChallenge) {
        event.userSettingsRequest->mfaChallenge = 0;
        event.userSettingsRequest->mfaType.clear();
      }
      if (mfa) {
        event.userSettingsMfaChallenge = 9001;
        event.userSettingsMfaMethods = {"totp", "password"};
      }
      userSettingsListsResult(event);
      std::erase_if(state.pending, [](const auto& entry) { return entry.second.request.verb == Verb::Get; });
      state.loading.clear();
    };
    const auto field = [&](const std::string& suffix) {
      return "user-account:lists:" + std::to_string(state.generation) + ':' + suffix;
    };
    const auto step = m_demoInteractionStep++;
    std::cerr << "user-settings-lists-cycle step " << step << '\n';
    if (step == 0)
      selectSettingsPage("relationships");
    else if (step == 1) {
      require(m_inputs.contains(field("request-username")), "Friend request uses a retained native field");
      focusControl(field("request-username"));
      pasteControl(field("request-username"), "new.friend");
      require(
          state.requestUsername == "new.friend", "Native friend request field updates its draft before enabling Send"
      );
    } else if (step == 2) {
      click("user-account-button:lists:friend-request");
      require(request().values.value("username", "") == "new.friend", "Friend request sends the exact username");
      acknowledge({}, "The username was not found.");
      require(
          state.requestUsername == "new.friend" && !state.error.empty(),
          "Failed friend request retains the draft and explicit error"
      );
    } else if (step == 3) {
      click("user-account-button:lists:friend-request");
      acknowledge();
    } else if (step == 4) {
      click("user-account-select:" + field("filter"));
      require(m_selectRequest.has_value(), "Native relationship picker opens its popup");
    } else if (step == 5) {
      require(m_selectControl && !m_selectDirty, "Relationship popup is realized before keyboard selection");
      m_window.key({XKB_KEY_Home, "", false, false});
      for (int n = 0; n < 4; ++n)
        m_window.key({XKB_KEY_Down, "", false, false});
      m_window.key({XKB_KEY_Return, "", false, false});
      require(state.relationshipFilter == 4, "Native relationship picker selects blocked users");
      userSettingsListsAction("lists:remove:1003");
      require(state.confirmationRequest.has_value() && !state.busy, "Unblocking waits for explicit confirmation");
    } else if (step == 6) {
      click("user-account-button:lists:confirm");
      acknowledge({}, "Verification is required.", true);
    } else if (step == 7) {
      require(state.mfaRequest && state.mfaMethod == "totp", "Verification retains the exact original mutation");
      focusControl(field("verification"));
      pasteControl(field("verification"), "123456");
    } else if (step == 8) {
      click("user-account-button:lists:verify");
      require(
          state.mfaCode.empty() && m_inputs.at(field("verification"))->value().empty(),
          "Verification submission scrubs the native input and its undo history"
      );
    } else if (step == 9) {
      acknowledge({}, "The verification code is incorrect.", true);
      require(!state.error.empty(), "Incorrect verification feedback remains visible");
      // Retry successfully using the worker's restored original request identity.
      state.mfaCode = "654321";
      userSettingsListsAction("lists:verify");
      acknowledge();
      require(!state.busy && !state.mfaRequest, "Successful MFA ACK matches the restored original action");
      selectSettingsPage("payments");
    } else if (step == 10)
      click("user-account-button:lists:source-default:3001");
    else if (step == 11) {
      require(
          request().section == Section::PaymentSources && request().values.value("default", false),
          "Default payment method uses the verified typed update"
      );
      acknowledge();
      userSettingsListsAction("lists:source-remove:3001");
    } else if (step == 12) {
      click("user-account-button:lists:cancel");
      require(!state.confirmationRequest && !state.busy, "Cancelling removal submits no mutation");
      selectSettingsPage("data");
    } else if (step == 13) {
      const auto previous = m_settingsDraft["non_spam_retraining"];
      click("user-account-toggle:" + field("non_spam_retraining"));
      require(
          previous != m_settingsDraft["non_spam_retraining"], "Native data preference updates the staged remote setting"
      );
    } else if (step == 14) {
      state.excludedBackends.insert("Ads");
      userSettingsListsAction("lists:request-data");
    } else if (step == 15)
      click("user-account-button:lists:confirm");
    else if (step == 16) {
      const auto expected = request();
      require(
          expected.section == Section::DataPackage && expected.values.at("backends").size() == 6,
          "Data request contains exactly the selected verified backend identifiers"
      );
      acknowledge();
      selectSettingsPage("relationships");
      state.nicknameTarget = "1001";
      state.nickname = "Owl";
    } else if (step == 17) {
      focusControl(field("nickname"));
      m_window.key({XKB_KEY_a, "", true, false});
      pasteControl(field("nickname"), "Friendly owl");
      click("user-account-button:lists:nickname-save");
    } else if (step == 18) {
      require(
          request().values.value("nickname", "") == "Friendly owl",
          "Private friend nickname remains a real explicit update"
      );
      acknowledge();
      require(
          m_userDirectory.name("1001", "") == "Friendly owl",
          "Acknowledged nickname updates the live owning-loop directory immediately"
      );
      selectSettingsPage("accessibility");
    } else if (step == 19) {
      const auto previous = m_preferences.reducedMotion;
      click("toggle:reduced_motion");
      require(previous != m_preferences.reducedMotion, "Reduced motion uses an actual retained native toggle");
      click("user-account-select:local:role_color_mode");
      require(m_selectRequest.has_value(), "Native role color picker opens its popup");
    } else if (step == 20) {
      require(m_selectControl && !m_selectDirty, "Role color popup is realized before keyboard selection");
      m_window.key({XKB_KEY_End, "", false, false});
      m_window.key({XKB_KEY_Return, "", false, false});
      require(m_preferences.roleColorMode == 2, "Native role color picker updates the runtime preference");
    } else if (step == 22) {
      selectSettingsPage("gifts");
    } else if (step == 23) {
      focusControl(field("gift-code"));
      pasteControl(field("gift-code"), "https://discord.gift/DEMO-GIFT");
    } else if (step == 24) {
      click("user-account-button:lists:gift-preview");
      require(
          state.pending.begin()->second.request.giftCode.empty(), "Gift codes never enter public pending identities"
      );
      acknowledge(
          Json{
              {"uses", 0},
              {"max_uses", 1},
              {"redeemed", false},
              {"requires_payment_source", false},
              {"store_listing", {{"sku", {{"name", "Nitro gift"}}}}},
              {"subscription_plan", {{"name", "Nitro"}, {"interval", 1}, {"interval_count", 1}}}
          },
          "", false, true
      );
    } else if (step == 25) {
      click("user-account-button:lists:gift-redeem");
      require(
          state.confirmationRequest && state.confirmationRequest->giftCode.empty() && !state.busy,
          "Gift redemption stages an explicit confirmation without retaining a raw request code"
      );
    } else if (step == 26) {
      click("user-account-button:lists:confirm");
      require(
          request().giftCode.empty() && request().values.value("confirm", false),
          "Confirmed gift code remains request-only"
      );
      acknowledge(Json{{"redeemed", true}});
      require(state.giftCode.empty() && state.giftRedeemed, "Gift redemption scrubs native and retained code fields");
      selectSettingsPage("payments");
    } else if (step == 27) {
      userSettingsListsAction("lists:invoice:4001");
      acknowledge(
          Json{
              {"invoiceLink", "https://discord.com/invoice/demo"},
              {"refundInvoiceLinks", Json::array({"https://discord.com/invoice/demo-refund"})}
          },
          "", false, true
      );
    } else if (step == 28) {
      require(
          m_widgets.contains("user-account-button:lists:invoice-link:original")
              && m_widgets.contains("user-account-button:lists:invoice-link:refund-0"),
          "Invoice breakdown renders real original and refund document actions"
      );
      userSettingsListsAction("lists:invoice-close");
      userSettingsListsAction("lists:more:history");
    } else if (step == 29) {
      require(
          state.pending.begin()->second.request.cursor == "older-page+receipt=",
          "Purchase history preserves the opaque server cursor"
      );
      acknowledge(
          Json{
              {"receipts",
               Json::array(
                   {{{"id", "4003"},
                     {"total", 9900},
                     {"currency", "jpy"},
                     {"status", 2},
                     {"description", "Nitro"},
                     {"created_at", "2026-08-18T09:30:00Z"}}}
               )},
              {"cursor", nullptr}
          },
          "", false, true
      );
      require(
          rows(state.records["history"], "receipts").size() == 3 && !state.more["history"],
          "Purchase receipts append once and stop at the authoritative end cursor"
      );
    } else if (step == 21) {
      resetUserSettingsLists();
      require(
          state.mfaCode.empty() && !state.mfaRequest && state.pending.empty(),
          "Closing account lists clears secrets and in-flight identities"
      );
      selectSettingsPage("subscriptions");
    }
    m_window.invalidate();
  }
} // namespace app
