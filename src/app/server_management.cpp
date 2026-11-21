#include "api/channel_features.h"
#include "app/client_application.h"
#include "app/message_group.h"
#include "auth/browser_login.h"
#include "ui/controls/input.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <iostream>
#include <stdexcept>

namespace app {
  namespace {
    using Json = nlohmann::json;
    using Section = api::GuildSettingsSection;
    using Verb = api::GuildSettingsVerb;
    std::string str(const Json& v, std::string_view key, std::string fallback = {}) {
      if (!v.is_object() || !v.contains(key) || v[key].is_null())
        return fallback;
      return v[key].is_string() ? v[key].get<std::string>() : v[key].dump();
    }
    std::string managementUserName(const Json& v) {
      const auto& user = v.contains("user") && v["user"].is_object() ? v["user"] : v;
      return str(v, "nick", str(user, "global_name", str(user, "username", str(user, "id", "Unknown member"))));
    }
    std::optional<Section> managementSection(std::string_view page) {
      if (page == "guide")
        return Section::ServerGuide;
      if (page == "security")
        return Section::IncidentActions;
      if (page == "applications")
        return Section::JoinRequests;
      if (page == "access")
        return Section::Access;
      if (page == "monetization")
        return Section::MonetizationRequirements;
      if (page == "prune")
        return Section::Prune;
      if (page == "boosts")
        return Section::Boosts;
      return {};
    }
    std::string managementPage(Section section) {
      switch (section) {
      case Section::ServerGuide:
        return "guide";
      case Section::IncidentActions:
        return "security";
      case Section::JoinRequests:
      case Section::JoinInterview:
        return "applications";
      case Section::Access:
        return "access";
      case Section::MonetizationRequirements:
      case Section::MonetizationRestrictions:
        return "monetization";
      case Section::Prune:
        return "prune";
      case Section::Boosts:
      case Section::BoostSlots:
        return "boosts";
      default:
        return {};
      }
    }
    std::string until(unsigned minutes) {
      const auto epoch =
          std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() + std::chrono::minutes(minutes));
      std::tm utc{};
      gmtime_r(&epoch, &utc);
      char text[32]{};
      std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
      return text;
    }
    bool activeUntil(const Json& value) {
      if (!value.is_string())
        return false;
      const auto time = messageTime(value.get<std::string>());
      return time && *time > std::chrono::system_clock::now();
    }
    std::string dateLabel(const Json& value) {
      if (!value.is_string())
        return "Unavailable";
      const auto parsed = messageTime(value.get<std::string>());
      if (!parsed)
        return "Unavailable";
      const auto epoch = std::chrono::system_clock::to_time_t(*parsed);
      std::tm local{};
      localtime_r(&epoch, &local);
      char out[64]{};
      std::strftime(out, sizeof(out), "%d %b %Y, %H:%M", &local);
      return out;
    }
    std::optional<std::string> memberDate(const std::string& value, bool end) {
      if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        return {};
      const auto number = [&](std::size_t start, std::size_t length) {
        unsigned n = 0;
        const auto parsed = std::from_chars(value.data() + start, value.data() + start + length, n);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + start + length ? n : 0;
      };
      const auto year = number(0, 4), month = number(5, 2), day = number(8, 2);
      if (year < 2015
          || year > 2200
          || !std::chrono::year_month_day(std::chrono::year(year), std::chrono::month(month), std::chrono::day(day))
                  .ok())
        return {};
      std::tm local{};
      local.tm_year = year - 1900;
      local.tm_mon = month - 1;
      local.tm_mday = day;
      local.tm_isdst = -1;
      if (end) {
        local.tm_hour = 23;
        local.tm_min = 59;
        local.tm_sec = 59;
      }
      const auto epoch = std::mktime(&local);
      if (epoch == -1)
        return {};
      std::tm utc{};
      gmtime_r(&epoch, &utc);
      char out[32]{};
      std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &utc);
      return out;
    }
    int pendingApplications(const Json& record) {
      const auto value = record.value("pending_application_count", Json());
      if (!value.is_number_integer())
        return -1;
      if (value.is_number_unsigned() && value.get<std::uint64_t>() > 2000000000)
        return -1;
      const auto number = value.get<std::int64_t>();
      return number < 0 || number > 2000000000 ? -1 : static_cast<int>(number);
    }
    bool sameIds(Json a, Json b) {
      if (!a.is_array() || !b.is_array())
        return false;
      const auto normalize = [](Json& values) {
        std::sort(values.begin(), values.end());
        auto& array = values.get_ref<Json::array_t&>();
        array.erase(std::unique(array.begin(), array.end()), array.end());
      };
      normalize(a);
      normalize(b);
      return a == b;
    }
    bool matchingPrune(const Json& payload, const Json& requested) {
      return payload.value("days", -1) == requested.value("days", 7)
          && sameIds(payload.value("include_roles", Json::array()), requested.value("include_roles", Json::array()));
    }
  } // namespace

  void ClientApplication::prepareServerManagementDemo() {
    auto& s = m_serverSettings;
    s.lists["guide"] = {
        {"enabled", true},
        {"welcome_message",
         {{"author_ids", Json::array({"42"})}, {"message", "Welcome to Noctalia. Make yourself at home!"}}},
        {"new_member_actions",
         Json::array(
             {{{"channel_id", "102"},
               {"action_type", 0},
               {"title", "Read our rules"},
               {"description", "Meet the community and learn our guidelines."}},
              {{"channel_id", "101"},
               {"action_type", 1},
               {"title", "Say hello"},
               {"description", "Introduce yourself to the community."}},
              {{"channel_id", "103"},
               {"action_type", 0},
               {"title", "Find support"},
               {"description", "Learn where to ask for help."}}}
         )},
        {"resource_channels",
         Json::array(
             {{{"channel_id", "102"},
               {"title", "Community handbook"},
               {"description", "Helpful information for new members."}}}
         )}
    };
    s.lists["security"] = {{"invites_disabled_until", nullptr}, {"dms_disabled_until", nullptr}};
    s.lists["access"] = s.lists["screening"];
    s.lists["access"]["mode"] = "invite";
    s.lists["access"]["pending_application_count"] = 0;
    s.lists["monetization"] = {
        {"sufficient", false},
        {"size", true},
        {"mfa", true},
        {"age", true},
        {"owner_age", true},
        {"safe_environment", true},
        {"engagement_healthy", true},
        {"retention_healthy", false},
        {"can_apply", false}
    };
    s.lists["management:monetization-restrictions"] = Json::array();
    s.lists["prune"] = {{"pruned", 12}};
    s.lists["applications"] = Json::array(
        {{{"join_request_id", "501"},
          {"guild_id", s.guild},
          {"user_id", "43"},
          {"user", {{"id", "43"}, {"username", "barullus"}, {"global_name", "Barullus"}}},
          {"created_at", "2026-10-18T09:20:00Z"},
          {"application_status", "SUBMITTED"},
          {"form_responses",
           Json::array({{{"label", "What brings you here?"}, {"response", "I'd like to share my desktop setup."}}})}}}
    );
    s.lists["boosts"] = Json::array(
        {{{"id", "601"}, {"guild_id", s.guild}, {"user", {{"id", "42"}, {"username", "lysec"}}}, {"ended", false}}}
    );
    s.lists["management:boost-slots"] = Json::array(
        {{{"id", "602"}, {"premium_guild_subscription", nullptr}, {"cooldown_ends_at", nullptr}, {"canceled", false}}}
    );
    for (const auto* page : {"guide", "security", "prune", "applications", "boosts", "access", "monetization"})
      s.loaded.insert(page);
    if (s.page == "guide" || s.page == "security")
      s.record = s.lists[s.page];
  }

  void ClientApplication::demoServerManagementStep() {
    const auto require = [](bool condition, const char* message) {
      if (!condition)
        throw std::runtime_error(message);
    };
    auto& s = m_serverSettings;
    const auto action = [&](const std::string& value) {
      require(serverManagementAction(value), "Management replay action must be handled");
    };
    const auto result = [&](Json data) {
      require(!s.pending.empty(), "Management replay must retain the exact pending request");
      api::Event event;
      event.operation = api::Operation::GuildSettings;
      event.session = m_session;
      event.guildSettingsRequest = s.pending.begin()->second;
      event.guildSettings = std::move(data);
      serverSettingsResult(event);
    };
    const auto completion = [&](Section section, Json data) {
      api::Event event;
      event.operation = api::Operation::GuildSettings;
      event.session = m_session;
      event.live = true;
      api::GuildSettingsRequest request;
      request.guildId = "100";
      request.section = section;
      event.guildSettingsRequest = request;
      event.guildSettings = std::move(data);
      require(
          serverManagementResult(event), "Live moderation completion must be handled independently of the open page"
      );
    };
    const auto step = m_demoInteractionStep++;
    std::clog << "Server management replay step " << step << '\n';
    if (step == 0) {
      selectServerSettingsPage("applications", true);
      require(s.loaded.contains("applications"), "Member applications must have native fixtures");
    } else if (step == 1) {
      action("management:application:501");
      action("management:approve");
      require(
          s.pending.begin()->second.values["action"] == "APPROVED", "Approve must send an explicit review decision"
      );
      result(Json::object());
      require(
          s.lists["applications"].empty(), "Approved applications leave the pending queue only after acknowledgement"
      );
      selectServerSettingsPage("guide", true);
    } else if (step == 2) {
      auto welcome = s.record["welcome_message"];
      welcome["message"] = "Welcome! Enjoy your time in our community.";
      setServerSettingsValue("welcome_message", welcome);
      action("save");
      require(s.pending.begin()->second.section == Section::ServerGuide, "Guide saves must use the guide resource");
      result(Json::object());
      require(!s.dirty(), "Guide acknowledgement clears only the saved draft");
      selectServerSettingsPage("members", true);
    } else if (step == 3) {
      s.lists["management:member-filters"] = {
          {"sort", 4}, {"role_ids", Json::array({"51"})}, {"require_timeout", true}
      };
      action("management:apply-members");
      require(
          s.pending.begin()->second.values["sort"] == 4, "Member sort and role filters must reach the search request"
      );
      result(s.lists["members"]);
      s.lists["management:member-selected"] = Json::array({"43"});
      action("management:bulk-ban");
      action("management:confirm-bulk");
    } else if (step == 4) {
      completion(
          Section::BulkBans,
          {{"complete", true}, {"banned_users", Json::array({"43"})}, {"failed_users", Json::array()}}
      );
      result(Json::object());
      require(
          m_serverManagementJobs["100:bans"]["complete"] == true,
          "An early Gateway completion must survive a later HTTP acknowledgement"
      );
      action("management:bulk-ban");
      action("management:confirm-bulk");
    } else if (step == 5) {
      result(Json::object());
      require(
          m_serverManagementJobs["100:bans"]["complete"] == false, "HTTP acceptance must not claim bulk bans completed"
      );
      closeServerSettings(true);
      completion(
          Section::BulkBans,
          {{"complete", true}, {"banned_users", Json::array()}, {"failed_users", Json::array({"43"})}}
      );
      require(
          m_serverManagementJobs["100:bans"]["failed_users"].size() == 1,
          "Closed settings must retain actual bulk-ban failures"
      );
      prepareServerSettingsDemo();
      selectServerSettingsPage("prune", true);
    } else if (step == 6) {
      s.lists["management:prune-options"] = {{"days", 30}, {"include_roles", Json::array({"51"})}};
      action("management:estimate-prune");
      result({{"pruned", 8}});
      action("management:prune");
      require(s.pending.empty(), "Showing a destructive confirmation must not mutate the server");
      action("management:confirm-prune");
      result({{"pruned", nullptr}});
    } else if (step == 7) {
      completion(
          Section::Prune,
          {{"complete", true},
           {"is_finished", true},
           {"prune_count", 7},
           {"days", 30},
           {"include_roles", Json::array({"51"})}}
      );
      require(
          m_serverManagementJobs["100:prune"]["prune_count"] == 7,
          "Pruning must report the final count, not the estimate"
      );
      selectServerSettingsPage("boosts", true);
    } else if (step == 8) {
      s.lists["management:boost-selected"] = Json::array({"602"});
      action("management:apply-boosts");
      require(
          s.pending.begin()->second.values["user_premium_guild_subscription_slot_ids"] == Json::array({"602"}),
          "Boost allocation must use the selected account-owned slot"
      );
      result(Json::array());
      while (!s.pending.empty()) {
        const auto section = s.pending.begin()->second.section;
        result(section == Section::BoostSlots ? s.lists["management:boost-slots"] : s.lists["boosts"]);
      }
      selectServerSettingsPage("screening", true);
    } else if (step == 9) {
      action("management:add-question:MULTIPLE_CHOICE");
      require(
          s.draft["form_fields"].back()["field_type"] == "MULTIPLE_CHOICE",
          "Screening must support choice questions, not only terms"
      );
      require(s.pending.empty(), "Editing screening questions must remain local until Save");
      s.draft.clear();
      selectServerSettingsPage("security", true);
    } else if (step == 10) {
      setServerSettingsValue("invites_disabled_until", until(15));
      action("save");
      require(
          s.pending.begin()->second.section == Section::IncidentActions,
          "Temporary safety actions must use the incident endpoint"
      );
      result(Json::object());
      require(!s.dirty(), "Safety acknowledgement must clear the saved value");
    } else if (step == 11) {
      require(s.pending.empty() && !s.saving, "Management replay must finish without pending mutations");
      std::clog << "Offline server management replay passed\n";
    }
    if (step < 11)
      m_window.invalidate();
  }

  bool ClientApplication::loadServerManagement(bool more) {
    auto& s = m_serverSettings;
    auto section = managementSection(s.page);
    if (s.page == "members" && s.resource.empty())
      section = Section::Members;
    if (!section)
      return false;
    if (s.saving)
      return true;
    api::GuildSettingsRequest r;
    r.section = *section;
    if (more)
      r.cursor = s.cursor;
    if (*section == Section::JoinRequests)
      r.values = {
          {"status",
           s.lists.contains("management:application-status") ? s.lists["management:application-status"]
                                                             : Json("SUBMITTED")}
      };
    if (*section == Section::Members) {
      r.query = s.search;
      r.values = s.lists.contains("management:member-filters") ? s.lists["management:member-filters"] : Json::object();
      if (s.lists.contains("member-role-filter") && s.lists["member-role-filter"].is_string()) {
        r.values["role_ids"] = Json::array({s.lists["member-role-filter"]});
        s.lists.erase("member-role-filter");
        s.lists["management:member-filters"] = r.values;
      }
      s.lists["management:member-applied"] = r.values;
      s.lists["management:member-search"] = r.query;
    }
    if (*section == Section::Prune) {
      r.values = s.lists.contains("management:prune-options") ? s.lists["management:prune-options"]
                                                              : Json{{"days", 7}, {"include_roles", Json::array()}};
    }
    requestServerSettings(r);
    if (*section == Section::Boosts) {
      r.section = Section::BoostSlots;
      r.cursor.clear();
      r.values = Json::object();
      requestServerSettings(r);
    }
    if (*section == Section::MonetizationRequirements) {
      r.section = Section::MonetizationRestrictions;
      r.cursor.clear();
      r.values = Json::object();
      requestServerSettings(r);
    }
    return true;
  }

  bool ClientApplication::serverManagementResult(const api::Event& event) {
    if (!event.guildSettingsRequest)
      return false;
    auto& s = m_serverSettings;
    const auto& r = *event.guildSettingsRequest;
    if (m_serverManagementSession != m_session) {
      m_serverManagementJobs.clear();
      m_serverManagementSession = m_session;
    }
    const auto jobKey = r.guildId + ":" + (r.section == Section::BulkBans ? "bans" : "prune");
    if (event.live && (r.section == Section::BulkBans || r.section == Section::Prune)) {
      if (r.section == Section::Prune && event.guildSettings.value("is_preview", false)) {
        const auto options = s.lists.contains("management:prune-options")
            ? s.lists["management:prune-options"]
            : Json{{"days", 7}, {"include_roles", Json::array()}};
        if (s.open && s.guild == r.guildId && matchingPrune(event.guildSettings, options)) {
          s.lists["prune"] = {{"pruned", event.guildSettings.value("prune_count", Json())}};
          m_window.invalidate();
        }
        return true;
      }
      // Gateway completion may precede the REST acknowledgement. Never replace it
      // with an HTTP acceptance, and retain it when this settings view is closed.
      const auto existing = m_serverManagementJobs.find(jobKey);
      if (existing == m_serverManagementJobs.end() || existing->second.value("complete", false))
        return true;
      const auto requested = existing->second.value("requested", Json::object());
      if (r.section == Section::BulkBans) {
        auto users = event.guildSettings.value("banned_users", Json::array());
        const auto failures = event.guildSettings.value("failed_users", Json::array());
        for (const auto& user : failures)
          users.push_back(user);
        if (!sameIds(users, requested.value("user_ids", Json::array())))
          return true;
      } else if (!matchingPrune(event.guildSettings, requested))
        return true;
      existing->second.update(event.guildSettings);
      if (s.open && s.guild == r.guildId)
        m_window.invalidate();
      return true;
    }
    const auto page = managementPage(r.section);
    const bool bulk = r.section == Section::BulkBans;
    if (page.empty() && !bulk) {
      if (r.section == Section::Members
          && r.verb == Verb::Get
          && r.resourceId.empty()
          && (s.lists["management:member-applied"] != r.values || s.lists["management:member-search"] != r.query)) {
        const auto pending = s.pending.find(r.request);
        if (pending == s.pending.end() || pending->second != r || r.guildId != s.guild)
          return true;
        s.pending.erase(pending);
        s.loading = std::ranges::any_of(s.pending, [](const auto& p) { return p.second.verb == Verb::Get; });
        return true;
      }
      return false;
    }
    if (!s.open || r.guildId != s.guild)
      return true;
    const auto pending = s.pending.find(r.request);
    if (pending == s.pending.end() || pending->second != r)
      return true;
    if (event.guildSettingsMfaChallenge)
      return false;
    s.pending.erase(pending);
    s.loading = std::ranges::any_of(s.pending, [](const auto& p) { return p.second.verb == Verb::Get; });
    s.saving = m_serverCommands.preparing.has_value()
        || std::ranges::any_of(s.pending, [](const auto& p) { return p.second.verb != Verb::Get; });
    if (!event.error.empty()) {
      if ((bulk || (r.section == Section::Prune && r.verb != Verb::Get))
          && m_serverManagementJobs.contains(jobKey)
          && !m_serverManagementJobs[jobKey].value("complete", false))
        m_serverManagementJobs.erase(jobKey);
      s.error = event.error;
      m_window.invalidate();
      return true;
    }
    if (r.verb != Verb::Get) {
      s.mfaRequest.reset();
      s.mfaChallenge = 0;
      s.mfaCode.clear();
      if (auto input = m_inputs.find("server-settings:mfa-code"); input != m_inputs.end())
        input->second->setValue("");
    }
    if (r.section == Section::Prune && r.verb == Verb::Get) {
      const auto options = s.lists.contains("management:prune-options")
          ? s.lists["management:prune-options"]
          : Json{{"days", 7}, {"include_roles", Json::array()}};
      if (r.values != options) {
        m_window.invalidate();
        return true;
      }
    }
    if (r.verb == Verb::Get) {
      const auto key = r.section == Section::BoostSlots    ? "management:boost-slots"
          : r.section == Section::MonetizationRestrictions ? "management:monetization-restrictions"
                                                           : page;
      auto data = event.guildSettings;
      if (r.section == Section::Access) {
        if (data.contains("guild") && data["guild"].is_object())
          s.overview.update(data["guild"]);
        const auto mode = data.value("mode", std::string("invite"));
        const auto pending = data.value("pending_application_count", Json());
        const auto pendingError = data.value("pending_application_error", std::string{});
        data = data.value("screening", Json::object());
        if (!data.is_object())
          data = Json::object();
        data["mode"] = mode;
        data["pending_application_count"] = pending;
        data["pending_application_error"] = pendingError;
      }
      if (r.section == Section::JoinRequests) {
        if (r.values.value("status", std::string("SUBMITTED"))
            != (s.lists.contains("management:application-status")
                    ? s.lists["management:application-status"].get<std::string>()
                    : "SUBMITTED"))
          return true;
        if (data.is_object()) {
          s.lists["management:application-total"] = data.value("total", 0);
          data = data.value("guild_join_requests", Json::array());
        }
      }
      if (!r.cursor.empty() && data.is_array() && s.lists[key].is_array()) {
        for (const auto& v : data)
          if (std::ranges::find(s.lists[key], v) == s.lists[key].end())
            s.lists[key].push_back(v);
      } else
        s.lists[key] = data;
      s.loaded.insert(key);
      if (s.page == page) {
        if (r.section == Section::ServerGuide || r.section == Section::IncidentActions || r.section == Section::Access)
          s.record = data.is_object() ? data : Json::object();
        s.more = event.guildSettingsMore;
        s.cursor = event.guildSettingsCursor;
      }
    } else if (bulk || r.section == Section::Prune) {
      auto& job = m_serverManagementJobs[jobKey];
      if (!job.is_object())
        job = Json::object();
      job["started"] = true;
      if (!job.contains("complete"))
        job["complete"] = false;
      if (!job.contains("requested"))
        job["requested"] = r.values;
      s.lists.erase("management:bulk-confirm");
      s.lists.erase("management:prune-confirm");
    } else if (r.section == Section::JoinInterview) {
      if (!event.channels.empty()) {
        const auto channel = event.channels.front();
        closeServerSettings(true);
        m_privateChannels.insert(m_privateChannels.begin(), channel);
        selectChannel(channel.id);
      }
    } else if (r.section == Section::JoinRequests) {
      if (s.lists["applications"].is_array())
        std::erase_if(s.lists["applications"].get_ref<Json::array_t&>(), [&](const Json& v) {
          return str(v, "join_request_id", str(v, "id")) == r.resourceId;
        });
      s.lists.erase("management:application-open");
      s.status =
          r.values.value("action", std::string{}) == "APPROVED" ? "Application approved." : "Application rejected.";
    } else {
      if (s.page == page) {
        s.draft.clear();
        s.status = "Changes saved.";
        if (r.section == Section::ServerGuide
            || r.section == Section::IncidentActions
            || r.section == Section::Access) {
          s.record.update(r.values);
          if (r.section == Section::Access) {
            if (event.guildSettings.contains("guild") && event.guildSettings["guild"].is_object())
              s.overview.update(event.guildSettings["guild"]);
            if (event.guildSettings.contains("screening") && event.guildSettings["screening"].is_object())
              s.record.update(event.guildSettings["screening"]);
            s.record["mode"] = event.guildSettings.value("mode", r.values.value("mode", std::string("invite")));
            s.record["pending_application_count"] = event.guildSettings.value("pending_application_count", Json());
            s.record["pending_application_error"] =
                event.guildSettings.value("pending_application_error", std::string{});
          } else if (event.guildSettings.is_object())
            s.record.update(event.guildSettings);
          s.lists[page] = s.record;
          if (r.section == Section::ServerGuide && !s.guideIconDrafts.empty())
            serverExtraAction("save");
        } else {
          s.loaded.erase(page);
          s.lists.erase("management:boost-remove");
          s.lists.erase("management:boost-selected");
          loadServerManagement();
        }
      }
    }
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::serverManagementAction(const std::string& id) {
    auto& s = m_serverSettings;
    const auto send = [&](Section section, Verb verb, Json values = Json::object(), std::string resource = {}) {
      api::GuildSettingsRequest r;
      r.section = section;
      r.verb = verb;
      r.values = std::move(values);
      r.resourceId = std::move(resource);
      r.reason = s.reason;
      return requestServerSettings(std::move(r));
    };
    const auto startJob = [&](const std::string& key) {
      if (m_serverManagementSession != m_session) {
        m_serverManagementJobs.clear();
        m_serverManagementSession = m_session;
      }
      if (m_serverManagementJobs.contains(key) && !m_serverManagementJobs[key].value("complete", false)) {
        s.error = "A matching moderation action is already running. Wait for its result.";
        return false;
      }
      if (!m_serverManagementJobs.contains(key) && m_serverManagementJobs.size() >= 32) {
        const auto finished = std::ranges::find_if(m_serverManagementJobs, [](const auto& job) {
          return job.second.value("complete", false);
        });
        if (finished == m_serverManagementJobs.end()) {
          s.error = "Wait for the current moderation actions to finish before starting another.";
          return false;
        }
        m_serverManagementJobs.erase(finished);
      }
      return true;
    };
    if (id.starts_with("sound-preview:")) {
      playServerSound(id.substr(14));
      return true;
    }
    if (id == "sound-stop") {
      stopServerSound();
      return true;
    }
    if (id == "mfa-cancel"
        && s.mfaRequest
        && (s.mfaRequest->section == Section::BulkBans || s.mfaRequest->section == Section::Prune)) {
      m_serverManagementJobs.erase(s.guild + ":" + (s.mfaRequest->section == Section::BulkBans ? "bans" : "prune"));
      return false;
    }
    if (id == "save" && (s.page == "guide" || s.page == "security")) {
      if (s.saving || s.mfaRequest)
        return true;
      if (s.page == "guide" && s.draft.empty())
        return false;
      send(*managementSection(s.page), Verb::Update, s.draft);
      return true;
    }
    if (id == "save" && s.page == "access") {
      if (s.saving || s.mfaRequest)
        return true;
      Json values = {{"mode", s.record.value("mode", std::string("invite"))}};
      for (const auto* key : {"mode", "enabled", "description", "form_fields", "bulk_action"}) {
        if (s.draft.contains(key))
          values[key] = s.draft[key];
      }
      if (s.record.value("mode", std::string("invite")) == "apply"
          && values.value("mode", std::string("invite")) != "apply"
          && pendingApplications(s.record) < 0) {
        s.error = "Refresh the pending application count before changing the server access mode.";
        m_window.invalidate();
        return true;
      }
      if (s.record.value("mode", std::string("invite")) == "apply"
          && values.value("mode", std::string("invite")) != "apply"
          && pendingApplications(s.record) > 0
          && (!values.contains("bulk_action") || values["bulk_action"].is_null())) {
        s.error = "Choose whether to approve or reject pending applications before changing the server access mode.";
        m_window.invalidate();
        return true;
      }
      send(Section::Access, Verb::Update, values);
      return true;
    }
    if (s.saving || s.mfaRequest)
      return id.starts_with("management:");
    if (id == "management:access-refresh") {
      loadServerManagement();
    } else if (id == "management:monetization-browser") {
      if (!m_demo && !openExternalUrl("https://discord.com/guilds/" + s.guild + "/settings/ROLE_SUBSCRIPTIONS"))
        s.error = "Could not open the monetization settings in your browser.";
    } else if (id == "management:apply-members") {
      if (s.lists.contains("management:member-date-invalid") && !s.lists["management:member-date-invalid"].empty())
        s.error = "Enter a valid date in YYYY-MM-DD format before applying the filters.";
      else {
        s.cursor.clear();
        loadServerManagement();
      }
    } else if (id == "management:toggle-member-filters") {
      s.lists["management:member-filters-open"] =
          !(s.lists.contains("management:member-filters-open") && s.lists["management:member-filters-open"] == true);
    } else if (id.starts_with("management:remove-filter-role:")) {
      auto& roles = s.lists["management:member-filters"]["role_ids"];
      if (roles.is_array())
        std::erase(roles.get_ref<Json::array_t&>(), Json(id.substr(30)));
    } else if (id == "management:clear-members") {
      s.search.clear();
      s.lists["management:member-filters"] = Json::object();
      s.lists.erase("management:member-selected");
      s.lists.erase("management:member-date-invalid");
      std::erase_if(s.lists, [](const auto& entry) {
        return entry.first.starts_with("management:member-date:")
            || entry.first.starts_with("management:range:")
            || entry.first == "management:joined-range";
      });
      s.cursor.clear();
      loadServerManagement();
    } else if (id == "management:bulk-ban")
      s.lists["management:bulk-confirm"] = true;
    else if (id == "management:cancel-bulk")
      s.lists.erase("management:bulk-confirm");
    else if (id == "management:confirm-bulk") {
      const auto key = s.guild + ":bans";
      if (!startJob(key)) {
        m_window.invalidate();
        return true;
      }
      const Json values = {
          {"user_ids", s.lists["management:member-selected"]},
          {"delete_message_seconds",
           s.lists.contains("management:ban-seconds") ? s.lists["management:ban-seconds"] : Json(0)}
      };
      if (send(Section::BulkBans, Verb::Create, values))
        m_serverManagementJobs[key] = {{"complete", false}, {"started", false}, {"requested", values}};
    } else if (id == "management:estimate-prune")
      loadServerManagement();
    else if (id == "management:prune")
      s.lists["management:prune-confirm"] = true;
    else if (id == "management:cancel-prune")
      s.lists.erase("management:prune-confirm");
    else if (id == "management:confirm-prune") {
      const auto key = s.guild + ":prune";
      if (!startJob(key)) {
        m_window.invalidate();
        return true;
      }
      auto values = s.lists.contains("management:prune-options") ? s.lists["management:prune-options"]
                                                                 : Json{{"days", 7}, {"include_roles", Json::array()}};
      values["compute_prune_count"] = false;
      if (send(Section::Prune, Verb::Create, values))
        m_serverManagementJobs[key] = {{"complete", false}, {"started", false}, {"requested", values}};
    } else if (id.starts_with("management:application:"))
      s.lists["management:application-open"] = id.substr(23);
    else if (id == "management:application-back")
      s.lists.erase("management:application-open");
    else if (id == "management:approve" || id == "management:reject" || id == "management:interview") {
      const auto resource = s.lists["management:application-open"].get<std::string>();
      if (id == "management:interview")
        send(Section::JoinInterview, Verb::Create, Json::object(), resource);
      else {
        Json values = {{"action", id == "management:approve" ? "APPROVED" : "REJECTED"}};
        if (id == "management:reject")
          values["rejection_reason"] =
              s.lists.contains("management:rejection") ? s.lists["management:rejection"] : Json("");
        send(Section::JoinRequests, Verb::Update, values, resource);
      }
    } else if (id == "management:apply-boosts")
      send(
          Section::Boosts, Verb::Create,
          {{"user_premium_guild_subscription_slot_ids", s.lists["management:boost-selected"]},
           {"disable_powerup_auto_apply", false}}
      );
    else if (id.starts_with("management:remove-boost:")) {
      s.lists["management:boost-remove"] = id.substr(24);
    } else if (id == "management:cancel-boost")
      s.lists.erase("management:boost-remove");
    else if (id == "management:confirm-boost")
      send(Section::Boosts, Verb::Delete, Json::object(), s.lists["management:boost-remove"].get<std::string>());
    else if (id.starts_with("management:sync:"))
      send(Section::Integrations, Verb::Sync, Json::object(), id.substr(16));
    else if (id.starts_with("management:add-question:")) {
      const auto type = id.substr(24);
      auto fields = s.draft.value("form_fields", s.record.value("form_fields", Json::array()));
      if (fields.size() < 5) {
        Json field = {
            {"field_type", type}, {"label", type == "TERMS" ? "Server rules" : "New question"}, {"required", true}
        };
        if (type == "TERMS")
          field["values"] = Json::array({"Be respectful."});
        if (type == "MULTIPLE_CHOICE")
          field["choices"] = Json::array({"First option"});
        fields.push_back(field);
        setServerSettingsValue("form_fields", fields);
      }
    } else if (id.starts_with("management:guide-add:")) {
      const auto key = id.substr(21);
      auto rows = s.draft.value(key, s.record.value(key, Json::array()));
      rows.push_back(
          key == "new_member_actions" ? Json{{"channel_id", ""}, {"action_type", 0}, {"title", ""}, {"description", ""}}
                                      : Json{{"channel_id", ""}, {"title", ""}, {"description", ""}}
      );
      setServerSettingsValue(key, rows);
    } else
      return false;
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::drawServerManagementForms(ServerSettingsForm& f) {
    auto& s = m_serverSettings;
    const auto busy = s.saving || s.loading || s.mfaRequest.has_value();
    const auto temporary = [&](const std::string& key, Json fallback) {
      return s.lists.contains(key) ? s.lists[key] : fallback;
    };
    const auto text = [&](const std::string& title, const std::string& description, const std::string& key,
                          Json snapshot, const std::function<void(Json, const std::string&)>& changed,
                          float height = 32) {
      f.detail(title, description);
      auto& input = inputControl(
          f.node, 0, f.y, f.width, height, "server-settings:management:" + key,
          snapshot.is_string() ? snapshot.get<std::string>() : "", {}, height > 32
      );
      input.setOnChange([snapshot, changed](const std::string& value) { changed(snapshot, value); });
      input.setOnSubmit([](const std::string&) {});
      input.setEnabled(!busy);
      f.y += height + 12;
    };
    const auto showJob = [&](const std::string& type) {
      const auto it = m_serverManagementJobs.find(s.guild + ":" + type);
      if (it == m_serverManagementJobs.end())
        return;
      const auto& result = it->second;
      if (result.value("complete", false)) {
        if (type == "bans")
          f.row(
              "Bulk ban complete",
              std::to_string(result.value("banned_users", Json::array()).size())
                  + " banned · "
                  + std::to_string(result.value("failed_users", Json::array()).size())
                  + " failed"
          );
        else
          f.row("Prune complete", str(result, "prune_count", "0") + " members removed.");
      } else
        f.row(
            type == "bans" ? "Banning members…" : "Pruning members…",
            result.value("started", false) ? "Discord accepted the request. Waiting for the operation result."
                                           : "Sending the request to Discord."
        );
    };
    if (s.page == "screening" || s.page == "access") {
      if (s.page == "access") {
        f.section("Server access");
        const std::vector<std::string> modes = {"invite", "apply", "discoverable"};
        const auto mode = f.value("mode", "invite").get<std::string>();
        const auto at = std::ranges::find(modes, mode);
        f.chooseCallback(
            "access-mode", "How members join",
            "Invite Only uses invite links. Apply to Join requires an approved application. Discoverable allows "
            "eligible servers to appear in Server Discovery.",
            {"Invite Only", "Apply to Join", "Discoverable"}, at == modes.end() ? 0 : at - modes.begin(),
            [this, modes](std::size_t choice) {
              auto fields = m_serverSettings.draft.value(
                  "form_fields", m_serverSettings.record.value("form_fields", Json::array())
              );
              if (!fields.is_array())
                fields = Json::array();
              if (choice != 1)
                std::erase_if(fields.get_ref<Json::array_t&>(), [](const Json& field) {
                  return str(field, "field_type") != "TERMS";
                });
              else if (std::ranges::none_of(fields, [](const Json& field) {
                         return str(field, "field_type") != "TERMS";
                       }))
                fields.push_back(
                    {{"field_type", "PARAGRAPH"}, {"label", "What brings you to our community?"}, {"required", true}}
                );
              setServerSettingsValue("mode", modes.at(choice));
              setServerSettingsValue("form_fields", fields);
              setServerSettingsValue("enabled", choice == 1 || !fields.empty());
            }
        );
        if (s.record.value("mode", std::string("invite")) == "apply"
            && mode != "apply"
            && pendingApplications(s.record) > 0) {
          ServerSettingsForm::Choices decisions = {{"Choose an action", nullptr}};
          if (pendingApplications(s.record) <= 200)
            decisions.emplace_back("Approve all pending applications", "APPROVED");
          decisions.emplace_back("Reject all pending applications", "REJECTED");
          f.choose(
              "Pending applications", "bulk_action",
              "Choose what happens to pending applicants when you stop requiring applications.", decisions
          );
        }
        if (s.record.value("mode", std::string("invite")) == "apply") {
          f.row(
              "Pending applications",
              pendingApplications(s.record) < 0 ? "Count unavailable. Refresh before changing the access mode."
                                                : std::to_string(pendingApplications(s.record)) + " awaiting review"
          );
          f.button("Refresh applications", "management:access-refresh", ButtonVariant::Default, 176, !busy);
        }
      }
      f.section("Membership screening");
      f.toggle(
          "Enable screening", "enabled", "Require new members to complete the membership form before participating."
      );
      f.field("Introduction", "description", "Introduce your rules and application questions.", 64, 2);
      auto fields = f.value("form_fields", Json::array());
      for (std::size_t i = 0; i < fields.size(); ++i) {
        auto field = fields[i];
        const auto type = str(field, "field_type");
        f.section(type == "TERMS" ? "Server rules" : "Question " + std::to_string(i + 1));
        for (const auto& [key, title] :
             std::vector<std::pair<std::string, std::string>>{{"label", "Title"}, {"description", "Description"}})
          text(
              title,
              key == "label" ? "Ask a clear question or name this rules section."
                             : "Add context within the question description.",
              "screening:" + std::to_string(i) + key, field.value(key, Json("")),
              [this, fields, i, key](Json, const std::string& v) mutable {
                fields[i][key] = v;
                setServerSettingsValue("form_fields", fields);
              }
          );
        f.toggleCallback(
            "question-required:" + std::to_string(i), "Required",
            "Members must answer this question before submitting.", field.value("required", false), !busy,
            [this, fields, i](bool checked) mutable {
              fields[i]["required"] = checked;
              setServerSettingsValue("form_fields", fields);
            }
        );
        if (type == "TERMS" || type == "MULTIPLE_CHOICE") {
          const auto key = type == "TERMS" ? "values" : "choices";
          std::string lines;
          for (const auto& v : field.value(key, Json::array()))
            if (v.is_string()) {
              if (!lines.empty())
                lines += '\n';
              lines += v.get<std::string>();
            }
          text(
              type == "TERMS" ? "Rules" : "Choices",
              type == "TERMS" ? "Write one rule per line, up to 16 rules."
                              : "Write one answer per line, up to eight choices.",
              "question-lines:" + std::to_string(i), lines,
              [this, fields, i, key = std::string(key)](Json, const std::string& v) mutable {
                Json values = Json::array();
                std::size_t at = 0;
                while (at < v.size()) {
                  auto end = v.find('\n', at);
                  auto line = v.substr(at, end == std::string::npos ? end : end - at);
                  if (!line.empty())
                    values.push_back(line);
                  if (end == std::string::npos)
                    break;
                  at = end + 1;
                }
                fields[i][key] = values;
                setServerSettingsValue("form_fields", fields);
              },
              96
          );
        } else if (type == "TEXT_INPUT" || type == "PARAGRAPH")
          text(
              "Answer placeholder", "Show an example before the member types a response.",
              "question-placeholder:" + std::to_string(i), field.value("placeholder", Json("")),
              [this, fields, i](Json, const std::string& v) mutable {
                fields[i]["placeholder"] = v;
                setServerSettingsValue("form_fields", fields);
              }
          );
        f.chooseCallback(
            "question-position:" + std::to_string(i), "Position", "Reorder or remove this question.",
            {"Keep position", "Move up", "Move down", "Remove"}, 0, [this, fields, i](std::size_t choice) mutable {
              if (choice == 1 && i)
                std::swap(fields[i], fields[i - 1]);
              else if (choice == 2 && i + 1 < fields.size())
                std::swap(fields[i], fields[i + 1]);
              else if (choice == 3)
                fields.erase(i);
              else
                return;
              setServerSettingsValue("form_fields", fields);
            }
        );
      }
      if (fields.size() < 5) {
        f.section("Add a question");
        for (const auto& [type, title] : std::vector<std::pair<std::string, std::string>>{
                 {"TERMS", "Rules agreement"},
                 {"TEXT_INPUT", "Short answer"},
                 {"PARAGRAPH", "Long answer"},
                 {"MULTIPLE_CHOICE", "Multiple choice"},
                 {"VERIFICATION", "Verification"}
             })
          f.button(title, "management:add-question:" + type, ButtonVariant::Default, 160, !busy);
      }
      return true;
    }
    if (s.page == "members" && s.resource.empty()) {
      f.section("Find members");
      text(
          "Name", "Search server nicknames, display names or usernames.", "member-search", s.search,
          [this](Json, const std::string& v) {
            m_serverSettings.search = v;
            m_window.invalidate();
          }
      );
      auto filters = temporary("management:member-filters", Json::object());
      const auto changeFilter = [this](std::string key, Json value) {
        auto& filters = m_serverSettings.lists["management:member-filters"];
        if (!filters.is_object())
          filters = Json::object();
        if (value.is_null())
          filters.erase(key);
        else
          filters[key] = std::move(value);
        m_window.invalidate();
      };
      f.chooseCallback(
          "members-sort", "Sort", "Order the results by server join date or account creation date.",
          {"Newest members", "Oldest members", "Newest accounts", "Oldest accounts"}, filters.value("sort", 1) - 1,
          [changeFilter](std::size_t i) { changeFilter("sort", i + 1); }
      );
      f.button("Filter members", "management:toggle-member-filters", ButtonVariant::Default, 128, !busy);
      if (temporary("management:member-filters-open", false) == true) {
        std::vector<std::string> roleNames = {"All roles"}, roleIds = {""};
        for (const auto& role : m_roleDirectory.allRoles(s.guild)) {
          roleNames.push_back(role.name);
          roleIds.push_back(role.id);
        }
        const auto selected = filters.value("role_ids", Json::array());
        f.chooseCallback(
            "members-role", "Roles", "Show members who have all the selected server roles.", roleNames, 0,
            [changeFilter, roleIds, selected = Json(selected)](std::size_t i) mutable {
              if (!i) {
                changeFilter("role_ids", Json());
                return;
              }
              if (std::ranges::find(selected, Json(roleIds.at(i))) == selected.end())
                selected.push_back(roleIds.at(i));
              changeFilter("role_ids", selected);
            }
        );
        for (const auto& id : selected)
          if (id.is_string()) {
            const auto at = std::ranges::find(roleIds, id.get<std::string>());
            f.row(
                at == roleIds.end() ? "Unknown role" : roleNames[at - roleIds.begin()], "Members must have this role."
            );
            f.button(
                "Remove role", "management:remove-filter-role:" + id.get<std::string>(), ButtonVariant::Default, 112,
                !busy
            );
          }
        const auto range = [&](const std::string& prefix, const std::string& title) {
          f.chooseCallback(
              "members-range:" + prefix, title, "Choose a recent time range, or include all dates.",
              {"Any time", "Past 24 hours", "Past 7 days", "Past 30 days"},
              temporary("management:range:" + prefix, 0).get<unsigned>(), [this, changeFilter, prefix](std::size_t i) {
                static constexpr unsigned hours[] = {0, 24, 168, 720};
                m_serverSettings.lists["management:range:" + prefix] = i;
                if (!i) {
                  changeFilter(prefix + "_after", Json());
                  return;
                }
                const auto epoch = std::chrono::system_clock::to_time_t(
                    std::chrono::system_clock::now() - std::chrono::hours(hours[i])
                );
                std::tm utc{};
                gmtime_r(&epoch, &utc);
                char out[32]{};
                std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &utc);
                changeFilter(prefix + "_after", out);
              }
          );
        };
        range("account", "Account created");
        text(
            "Invite code", "Show members who joined using a particular invite link.", "members-invite",
            filters.value("source_invite_code", Json("")), [changeFilter](Json, const std::string& v) {
              changeFilter("source_invite_code", v.empty() ? Json() : Json(v));
            }
        );
        const std::vector<std::string> sourceNames = {"Any join method",    "Bot",
                                                      "Integration",        "Discovery",
                                                      "Student Hub",        "Invite link",
                                                      "Custom invite link", "Approved application",
                                                      "Linked game channel"};
        const auto source = filters.contains("join_source_type") ? filters["join_source_type"].get<unsigned>() : 0;
        f.chooseCallback(
            "members-source", "Join method", "Filter how members originally joined this server.", sourceNames,
            std::min(source, 8u),
            [changeFilter](std::size_t i) { changeFilter("join_source_type", i ? Json(i) : Json()); }
        );
        for (const auto& [key, title] : std::vector<std::pair<std::string, std::string>>{
                 {"joined_after", "Joined on or after"},
                 {"joined_before", "Joined on or before"},
                 {"account_after", "Account created on or after"},
                 {"account_before", "Account created on or before"}
             }) {
          const auto rawKey = "management:member-date:" + key;
          text(
              title, "Enter YYYY-MM-DD in your local time zone, or leave blank for no limit.", "member-date:" + key,
              temporary(rawKey, str(filters, key).substr(0, 10)),
              [this, changeFilter, key, rawKey](Json, const std::string& v) {
                m_serverSettings.lists[rawKey] = v;
                auto& invalid = m_serverSettings.lists["management:member-date-invalid"];
                if (!invalid.is_object())
                  invalid = Json::object();
                if (v.empty()) {
                  invalid.erase(key);
                  changeFilter(key, Json());
                } else if (const auto value = memberDate(v, key.ends_with("before"))) {
                  invalid.erase(key);
                  changeFilter(key, *value);
                } else {
                  invalid[key] = true;
                  m_serverSettings.error = "Enter a valid date in YYYY-MM-DD format.";
                  m_window.invalidate();
                }
              }
          );
        }
        f.chooseCallback(
            "members-joined", "Joined server", "Restrict results to members who joined recently.",
            {"Any time", "Past 24 hours", "Past 7 days", "Past 30 days"},
            temporary("management:joined-range", 0).get<unsigned>(), [this, changeFilter](std::size_t i) {
              static constexpr unsigned hours[] = {0, 24, 168, 720};
              m_serverSettings.lists["management:joined-range"] = i;
              changeFilter("joined_after", i ? Json(until(0)) : Json());
              if (i) {
                auto epoch = std::chrono::system_clock::to_time_t(
                    std::chrono::system_clock::now() - std::chrono::hours(hours[i])
                );
                std::tm utc{};
                gmtime_r(&epoch, &utc);
                char out[32]{};
                std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &utc);
                changeFilter("joined_after", out);
              }
            }
        );
        for (const auto& [key, title] : std::vector<std::pair<std::string, std::string>>{
                 {"require_unusual_dm_activity", "Unusual DM activity"},
                 {"require_unusual_account_activity", "Unusual account activity"},
                 {"require_timeout", "Timed out"},
                 {"require_quarantined_username", "Quarantined profiles"}
             })
          f.toggleCallback(
              "member-filter:" + key, title, "Show only members matching this safety signal.",
              filters.value(key, false), !busy,
              [changeFilter, key](bool v) { changeFilter(key, v ? Json(true) : Json()); }
          );
      }
      f.button("Apply filters", "management:apply-members", ButtonVariant::Primary, 128, !busy);
      f.button("Clear filters", "management:clear-members", ButtonVariant::Default, 128, !busy);
      f.section("Members");
      auto selectedIds = temporary("management:member-selected", Json::array());
      api::GuildSettingsRequest bulk;
      bulk.section = Section::BulkBans;
      bulk.verb = Verb::Create;
      const auto canBulk = api::canManageGuildSettings(bulk, serverSettingsAccess());
      const auto list = s.lists.find("members");
      if (list != s.lists.end() && list->second.is_array())
        for (const auto& member : list->second) {
          const auto uid = member.contains("user") ? str(member["user"], "id") : str(member, "user_id");
          api::GuildSettingsRequest ban;
          ban.section = Section::Bans;
          ban.verb = Verb::Create;
          ban.resourceId = uid;
          const auto canBan = api::canManageGuildSettings(ban, serverSettingsAccess(uid));
          if (canBulk && canBan && uid != m_account.id && uid != serverSettingsAccess().owner)
            f.toggleCallback(
                "member-select:" + uid, managementUserName(member), dateLabel(member.value("joined_at", Json())),
                std::ranges::find(selectedIds, Json(uid)) != selectedIds.end(), !busy, [this, uid](bool checked) {
                  auto& ids = m_serverSettings.lists["management:member-selected"];
                  if (!ids.is_array())
                    ids = Json::array();
                  if (checked) {
                    if (ids.size() < 200 && std::ranges::find(ids, Json(uid)) == ids.end())
                      ids.push_back(uid);
                  } else
                    std::erase(ids.get_ref<Json::array_t&>(), Json(uid));
                  m_window.invalidate();
                }
            );
          else
            f.row(managementUserName(member), dateLabel(member.value("joined_at", Json())));
          f.button("Manage member", "record:" + uid, ButtonVariant::Default, 128, !busy);
        }
      if (list == s.lists.end() || list->second.empty())
        f.row(s.loading ? "Loading members…" : "No members found", "Try a different name, role or safety filter.");
      if (s.more)
        f.button("Load more", "more", ButtonVariant::Default, 112, !busy);
      if (canBulk && !selectedIds.empty()) {
        f.section("Selected members");
        f.row(
            std::to_string(selectedIds.size()) + " selected",
            "Bulk bans permanently remove these accounts and prevent them from rejoining."
        );
        f.chooseCallback(
            "ban-seconds", "Delete message history", "Remove recent messages from the selected members.",
            {"Keep messages", "Past hour", "Past 24 hours", "Past 7 days"},
            temporary("management:ban-choice", 0).get<unsigned>(), [this](std::size_t i) {
              static constexpr unsigned seconds[] = {0, 3600, 86400, 604800};
              m_serverSettings.lists["management:ban-choice"] = i;
              m_serverSettings.lists["management:ban-seconds"] = seconds[i];
              m_window.invalidate();
            }
        );
        if (temporary("management:bulk-confirm", false).get<bool>()) {
          f.row(
              "Ban selected members?",
              "This immediately removes all selected accounts. Review the selection before confirming."
          );
          f.button("Confirm bans", "management:confirm-bulk", ButtonVariant::Destructive, 128, !busy);
          f.button("Cancel", "management:cancel-bulk");
        } else
          f.button("Ban selected", "management:bulk-ban", ButtonVariant::Destructive, 128, !busy);
      }
      showJob("bans");
      return true;
    }
    if (s.page == "guide") {
      f.section("Server Guide");
      f.toggle(
          "Enable Server Guide", "enabled",
          "Give new members a welcome message, useful tasks and resource pages after onboarding."
      );
      auto welcome = f.value("welcome_message", Json{{"author_ids", Json::array()}, {"message", ""}});
      text(
          "Welcome message", "Welcome members in 7–300 characters.", "guide-welcome",
          welcome.value("message", Json("")),
          [this, welcome](Json, const std::string& v) mutable {
            welcome["message"] = v;
            setServerSettingsValue("welcome_message", welcome);
          },
          76
      );
      std::vector<std::string> authorNames, authorIds;
      const auto addAuthor = [&](std::string id, std::string name) {
        if (!id.empty() && std::ranges::find(authorIds, id) == authorIds.end()) {
          authorIds.push_back(id);
          authorNames.push_back(name.empty() ? userName(id, "Member " + id) : name);
        }
      };
      addAuthor(m_account.id, m_account.name);
      if (s.lists.contains("members") && s.lists["members"].is_array())
        for (const auto& member : s.lists["members"])
          if (member.contains("user"))
            addAuthor(str(member["user"], "id"), managementUserName(member));
      const auto authors = welcome.value("author_ids", Json::array());
      for (const auto& id : authors)
        if (id.is_string())
          addAuthor(id.get<std::string>(), {});
      const auto currentAuthor = authors.empty() ? m_account.id : authors.front().get<std::string>();
      const auto authorAt = std::ranges::find(authorIds, currentAuthor);
      f.chooseCallback(
          "guide-author", "Welcome author", "Show this member's name and avatar above the welcome message.",
          authorNames, authorAt == authorIds.end() ? 0 : authorAt - authorIds.begin(),
          [this, welcome, authorIds](std::size_t i) mutable {
            welcome["author_ids"] = Json::array({authorIds.at(i)});
            setServerSettingsValue("welcome_message", welcome);
          }
      );
      for (const std::string key : {"new_member_actions", "resource_channels"}) {
        const bool task = key == "new_member_actions";
        f.section(task ? "New member tasks" : "Resource pages");
        auto rows = f.value(key, Json::array());
        for (std::size_t i = 0; i < rows.size(); ++i) {
          f.row(
              (task ? "Task " : "Resource ") + std::to_string(i + 1),
              task ? "Choose a channel to visit or participate in."
                   : "Choose a read-only channel to present as a resource page."
          );
          std::vector<std::string> names = {"Choose a channel"}, ids = {""};
          if (const auto channels = m_guildChannels.find(s.guild); channels != m_guildChannels.end())
            for (const auto& channel : channels->second)
              if (channel.type == 0 || channel.type == 5) {
                names.push_back("#" + channel.name);
                ids.push_back(channel.id);
              }
          const auto at = std::ranges::find(ids, str(rows[i], "channel_id"));
          f.chooseCallback(
              "guide-channel:" + key + std::to_string(i), "Channel", "Select a server text channel.", names,
              at == ids.end() ? 0 : at - ids.begin(), [this, rows, key, i, ids](std::size_t choice) mutable {
                rows[i]["channel_id"] = ids.at(choice);
                setServerSettingsValue(key, rows);
              }
          );
          if (task)
            f.chooseCallback(
                "guide-action:" + std::to_string(i), "Action", "Ask members to view a channel or send a message.",
                {"View channel", "Send a message"}, rows[i].value("action_type", 0),
                [this, rows, key, i](std::size_t choice) mutable {
                  rows[i]["action_type"] = choice;
                  setServerSettingsValue(key, rows);
                }
            );
          for (const char* field : {"title", "description"})
            text(
                field == std::string_view("title") ? "Title" : "Description", "Describe this channel for new members.",
                key + field + std::to_string(i), rows[i].value(field, Json("")),
                [this, rows, key, i, field = std::string(field)](Json, const std::string& v) mutable {
                  rows[i][field] = v;
                  setServerSettingsValue(key, rows);
                }
            );
          const auto iconChannel = str(rows[i], "channel_id");
          if (api::validSnowflake(iconChannel))
            drawServerExtraForms(f, (task ? "guide-action:" : "guide-resource:") + iconChannel);
          f.chooseCallback(
              "guide-order:" + key + std::to_string(i), "Position", "Move this item in the guide, or remove it.",
              {"Keep position", "Move up", "Move down", "Remove"}, 0, [this, rows, key, i](std::size_t choice) mutable {
                if (choice == 1 && i)
                  std::swap(rows[i], rows[i - 1]);
                else if (choice == 2 && i + 1 < rows.size())
                  std::swap(rows[i], rows[i + 1]);
                else if (choice == 3)
                  rows.erase(i);
                else
                  return;
                setServerSettingsValue(key, rows);
              }
          );
        }
        if (rows.size() < (task ? 5u : 7u))
          f.button(
              task ? "Add task" : "Add resource", "management:guide-add:" + key, ButtonVariant::Default, 128, !busy
          );
      }
      return true;
    }
    if (s.page == "security") {
      f.section("Security actions");
      const auto minutes = temporary("management:security-duration", 60).get<unsigned>();
      const std::vector<unsigned> durations = {15, 30, 60, 120, 240, 480, 1440};
      const auto duration = std::ranges::find(durations, minutes);
      f.chooseCallback(
          "security-duration", "Pause duration", "Choose how long to pause invites or direct messages, up to 24 hours.",
          {"15 minutes", "30 minutes", "1 hour", "2 hours", "4 hours", "8 hours", "24 hours"},
          duration == durations.end() ? 2 : duration - durations.begin(), [this, durations](std::size_t i) {
            m_serverSettings.lists["management:security-duration"] = durations.at(i);
            m_window.invalidate();
          }
      );
      for (const auto& [key, title] : std::vector<std::pair<std::string, std::string>>{
               {"invites_disabled_until", "Pause invites"}, {"dms_disabled_until", "Pause direct messages"}
           }) {
        const auto value = f.value(key);
        f.toggleCallback(
            "incident:" + key, title,
            key == "invites_disabled_until"
                ? "Temporarily prevent new members from joining with invite links."
                : "Temporarily stop direct messages between server members who are not friends.",
            activeUntil(value), !busy,
            [this, key, minutes](bool enabled) { setServerSettingsValue(key, enabled ? Json(until(minutes)) : Json()); }
        );
        if (activeUntil(value))
          f.row("Paused until", dateLabel(value));
      }
      return true;
    }
    if (s.page == "prune") {
      f.section("Prune inactive members");
      auto options = temporary("management:prune-options", Json{{"days", 7}, {"include_roles", Json::array()}});
      f.chooseCallback(
          "prune-days", "Inactivity", "Remove members who have not been active for this many days.",
          {"7 days", "30 days"}, options.value("days", 7) == 30 ? 1 : 0, [this, options](std::size_t i) mutable {
            options["days"] = i ? 30 : 7;
            m_serverSettings.lists["management:prune-options"] = options;
            m_serverSettings.lists.erase("prune");
            m_window.invalidate();
          }
      );
      for (const auto& role : m_roleDirectory.allRoles(s.guild))
        if (role.id != s.guild) {
          const auto ids = options.value("include_roles", Json::array());
          f.toggleCallback(
              "prune-role:" + role.id, role.name,
              "Include members who only have selected roles; members without roles are always included.",
              std::ranges::find(ids, Json(role.id)) != ids.end(), !busy,
              [this, options, id = role.id](bool checked) mutable {
                auto& values = options["include_roles"];
                if (checked && std::ranges::find(values, Json(id)) == values.end())
                  values.push_back(id);
                else if (!checked)
                  std::erase(values.get_ref<Json::array_t&>(), Json(id));
                m_serverSettings.lists["management:prune-options"] = options;
                m_serverSettings.lists.erase("prune");
                m_window.invalidate();
              }
          );
        }
      f.button("Estimate members", "management:estimate-prune", ButtonVariant::Default, 144, !busy);
      if (s.lists.contains("prune"))
        f.row("Estimated removals", str(s.lists["prune"], "pruned", "Unavailable"));
      if (temporary("management:prune-confirm", false).get<bool>()) {
        f.row(
            "Remove inactive members?", "Removed members can rejoin with a valid invite. This action cannot be undone."
        );
        f.button("Confirm prune", "management:confirm-prune", ButtonVariant::Destructive, 128, !busy);
        f.button("Cancel", "management:cancel-prune");
      } else
        f.button("Prune members", "management:prune", ButtonVariant::Destructive, 128, !busy);
      showJob("prune");
      return true;
    }
    if (s.page == "applications") {
      f.section("Member applications");
      const std::vector<std::string> statuses = {"SUBMITTED", "APPROVED", "REJECTED"};
      const auto status = temporary("management:application-status", "SUBMITTED").get<std::string>();
      const auto at = std::ranges::find(statuses, status);
      f.chooseCallback(
          "application-status", "Status", "Review pending applications or revisit previous decisions.",
          {"Pending", "Approved", "Rejected"}, at == statuses.end() ? 0 : at - statuses.begin(),
          [this, statuses](std::size_t i) {
            m_serverSettings.lists["management:application-status"] = statuses.at(i);
            m_serverSettings.lists.erase("management:application-open");
            m_serverSettings.cursor.clear();
            loadServerManagement();
            m_window.invalidate();
          }
      );
      const auto open = temporary("management:application-open", "").get<std::string>();
      const auto list = temporary("applications", Json::array());
      bool found = false;
      if (list.is_array())
        for (const auto& application : list) {
          const auto id = str(application, "join_request_id", str(application, "id"));
          if (!open.empty() && id != open)
            continue;
          found = true;
          f.row(managementUserName(application), dateLabel(application.value("created_at", Json())));
          if (open.empty())
            f.button("Review application", "management:application:" + id, ButtonVariant::Default, 160, !busy);
          else {
            f.button("Back to applications", "management:application-back", ButtonVariant::Default, 176, !busy);
            const auto responses = application.value("form_responses", Json::array());
            if (responses.is_array())
              for (const auto& response : responses)
                f.row(
                    str(response, "label", "Response"), str(response, "response", str(response, "value", "No response"))
                );
            if (status == "SUBMITTED") {
              text(
                  "Rejection reason", "Optionally explain why the application was rejected.", "application-reason",
                  temporary("management:rejection", ""),
                  [this](Json, const std::string& v) { m_serverSettings.lists["management:rejection"] = v; }, 64
              );
              f.button("Approve", "management:approve", ButtonVariant::Primary, 112, !busy);
              f.button("Reject", "management:reject", ButtonVariant::Destructive, 112, !busy);
              f.button("Start interview", "management:interview", ButtonVariant::Default, 144, !busy);
            }
          }
        }
      if (!found)
        f.row(
            s.loading ? "Loading applications…" : "No applications", "Applications with this status will appear here."
        );
      if (s.more && open.empty())
        f.button("Load more", "more", ButtonVariant::Default, 112, !busy);
      return true;
    }
    if (s.page == "boosts") {
      f.section("Server boosts");
      f.row(
          "Boost level",
          str(s.overview, "premium_tier", "0") + " · " + str(s.overview, "premium_subscription_count", "0") + " boosts"
      );
      const auto slots = temporary("management:boost-slots", Json::array());
      const auto selected = temporary("management:boost-selected", Json::array());
      for (const auto& slot : slots) {
        const auto id = str(slot, "id");
        if (slot.value("canceled", false))
          continue;
        const auto subscription = slot.value("premium_guild_subscription", Json());
        const bool applied = subscription.is_object() && !subscription.value("ended", false);
        const bool cooldown = activeUntil(slot.value("cooldown_ends_at", Json()));
        f.toggleCallback(
            "boost-slot:" + id, "Boost slot " + id,
            applied        ? "Already assigned to a server."
                : cooldown ? "Available after " + str(slot, "cooldown_ends_at")
                           : "Available to boost this server.",
            std::ranges::find(selected, Json(id)) != selected.end(), !busy && !applied && !cooldown,
            [this, id](bool checked) {
              auto& ids = m_serverSettings.lists["management:boost-selected"];
              if (!ids.is_array())
                ids = Json::array();
              if (checked && std::ranges::find(ids, Json(id)) == ids.end())
                ids.push_back(id);
              else if (!checked)
                std::erase(ids.get_ref<Json::array_t&>(), Json(id));
              m_window.invalidate();
            }
        );
      }
      if (slots.empty())
        f.row("No available boost slots", "Your existing subscription's boost slots will appear here when available.");
      f.button(
          "Apply selected boosts", "management:apply-boosts", ButtonVariant::Primary, 176, !busy && !selected.empty()
      );
      f.section("Applied boosts");
      const auto boosts = temporary("boosts", Json::array());
      for (const auto& boost : boosts)
        if (boost.is_object()) {
          f.row(
              managementUserName(boost), boost.value("ended", false) ? "Ended" : str(boost, "ends_at", "Active boost")
          );
          if (boost.contains("user") && str(boost["user"], "id") == m_account.id && !boost.value("ended", false))
            f.button(
                "Remove my boost", "management:remove-boost:" + str(boost, "id"), ButtonVariant::Destructive, 144, !busy
            );
        }
      if (s.lists.contains("management:boost-remove")) {
        f.row(
            "Remove this boost?", "This removes the selected boost from the server. Your subscription remains active."
        );
        f.button("Confirm removal", "management:confirm-boost", ButtonVariant::Destructive, 144, !busy);
        f.button("Cancel", "management:cancel-boost");
      }
      return true;
    }
    if (s.page == "monetization") {
      f.section("Eligibility");
      const auto requirements = temporary("monetization", Json::object());
      const auto eligible = requirements.value("sufficient", Json());
      f.row(
          "Monetization eligibility",
          eligible.is_boolean()
              ? (eligible.get<bool>()
                     ? "This server meets Discord's eligibility requirements."
                     : "This server does not currently meet all of Discord's eligibility requirements.")
              : "Discord has not supplied an eligibility result."
      );
      for (const auto& [key, title] : std::vector<std::pair<std::string, std::string>>{
               {"size", "Server size"},
               {"mfa", "Two-factor authentication"},
               {"age", "Server age"},
               {"owner_age", "Owner age"},
               {"safe_environment", "Server safety"},
               {"engagement_healthy", "Healthy engagement"},
               {"retention_healthy", "Member retention"}
           }) {
        const auto value = requirements.value(key, Json());
        f.row(
            title,
            value.is_boolean() ? (value.get<bool>() ? "Requirement met" : "Requirement not met")
                               : "Discord has not supplied a result."
        );
      }
      f.section("Server Subscriptions & Shop");
      f.row(
          "Monetization setup",
          "Manage subscription tiers, server products, payouts and legal agreements in Discord's browser settings."
      );
      if (requirements.contains("latest_request") && requirements["latest_request"].is_object())
        f.row(
            "Latest application", "Review your monetization application and its status in Discord's browser settings."
        );
      f.button("Open monetization", "management:monetization-browser", ButtonVariant::Primary, 176, !busy);
      return true;
    }
    if (s.page == "integrations" && !s.resource.empty()) {
      f.section("Integration details");
      if (s.record.contains("application")) {
        const auto& application = s.record["application"];
        f.row("Application", str(application, "name", str(application, "id")));
        f.row("Description", str(application, "description", "No description"));
        if (api::validSnowflake(str(application, "id")))
          f.button(
              "Command permissions", "commands:open:" + str(application, "id"), ButtonVariant::Default, 176, !busy
          );
        if (application.contains("bot"))
          f.row("Bot", managementUserName(application["bot"]));
      }
      if (s.record.contains("user"))
        f.row("Added by", managementUserName(s.record["user"]));
      f.row("Subscribers", str(s.record, "subscriber_count", "Not available"));
      f.row("Last sync", str(s.record, "synced_at", "Not available"));
      if (str(s.record, "type") == "twitch" || str(s.record, "type") == "youtube")
        f.button("Sync integration", "management:sync:" + s.resource, ButtonVariant::Default, 144, !busy);
      return false;
    }
    return false;
  }
} // namespace app
