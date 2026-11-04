#include "app/server_audit.h"

#include "app/client_application.h"
#include "app/theme.h"
#include "render/scene/rect_node.h"
#include "ui/controls/button.h"
#include "ui/controls/select.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace app {
  using namespace theme;
  using Json = nlohmann::json;
  namespace {
    std::string string(const Json& object, std::string_view key, std::string fallback = {}) {
      if (!object.is_object())
        return fallback;
      const auto at = object.find(key);
      return at == object.end() || !at->is_string() ? fallback : at->get<std::string>();
    }
    std::string lower(std::string text) {
      std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    }
    bool snowflake(const std::string& value) {
      std::uint64_t id = 0;
      const auto result = std::from_chars(value.data(), value.data() + value.size(), id);
      return !value.empty() && id && result.ec == std::errc{} && result.ptr == value.data() + value.size();
    }
  } // namespace

  void ClientApplication::resetServerAudit() { m_serverAudit = {}; }

  void ClientApplication::loadServerAudit(bool more) {
    auto& view = m_serverAudit;
    auto& state = m_serverSettings;
    if (!state.open || state.page != "audit")
      return;
    if (view.guild != state.guild) {
      resetServerAudit();
      view.guild = state.guild;
    }
    const auto access = serverSettingsAccess();
    if (access.self != access.owner && !api::permission::allows(access.permissions, api::permission::viewAuditLog)) {
      view.error = "You need View Audit Log permission to view this server's administrative history.";
      return;
    }
    if (more && (!view.more || view.loading))
      return;
    if (m_demo) {
      if (!view.loaded)
        prepareServerAuditDemo();
      view.error.clear();
      view.loading = false;
      view.loaded = true;
      view.more = false;
      return;
    }
    api::GuildSettingsRequest request;
    request.section = api::GuildSettingsSection::AuditLog;
    request.limit = 50;
    if (!view.actor.empty())
      request.values["user_id"] = view.actor;
    if (view.action)
      request.values["action_type"] = view.action;
    if (more)
      request.cursor = view.cursor;
    if (!requestServerSettings(request)) {
      view.error = state.error;
      view.failed = request;
      state.error.clear();
      view.loading = false;
      return;
    }
    view.pending = state.pending.at(state.serial);
    view.failed.reset();
    view.loading = true;
    view.error.clear();
  }

  bool ClientApplication::serverAuditResult(const api::Event& event) {
    if (!event.guildSettingsRequest || event.guildSettingsRequest->section != api::GuildSettingsSection::AuditLog)
      return false;
    auto& state = m_serverSettings;
    auto& view = m_serverAudit;
    const auto& request = *event.guildSettingsRequest;
    const auto pending = state.pending.find(request.request);
    if (pending == state.pending.end() || pending->second != request || request.guildId != state.guild)
      return true;
    state.pending.erase(pending);
    state.loading = std::ranges::any_of(state.pending, [](const auto& item) {
      return item.second.verb == api::GuildSettingsVerb::Get;
    });
    if (!view.pending || *view.pending != request || view.guild != request.guildId)
      return true;
    view.pending.reset();
    view.loading = false;
    if (!event.error.empty()) {
      view.error = event.error;
      view.failed = request;
      m_window.invalidate();
      return true;
    }
    if (!event.guildSettings.is_object()
        || !event.guildSettings.contains("audit_log_entries")
        || !event.guildSettings["audit_log_entries"].is_array()) {
      view.error = "The server returned an invalid audit log. Try again.";
      view.failed = request;
      m_window.invalidate();
      return true;
    }
    view.replace(event.guildSettings, !request.cursor.empty());
    view.loaded = true;
    view.more = event.guildSettingsMore;
    view.cursor = event.guildSettingsCursor;
    view.error.clear();
    state.loaded.insert("audit");
    if (event.guildSettings.contains("users") && event.guildSettings["users"].is_array())
      for (const auto& user : event.guildSettings["users"])
        m_userDirectory.consume("USER_UPDATE", user);
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::serverAuditAction(const std::string& action) {
    if (!action.starts_with("audit:"))
      return false;
    if (!m_serverSettings.open || m_serverSettings.page != "audit")
      return true;
    auto& view = m_serverAudit;
    if (action.starts_with("audit:expand:")) {
      const auto id = action.substr(13);
      if (!view.expanded.erase(id))
        view.expanded.insert(id);
    } else if (action == "audit:more")
      loadServerAudit(true);
    else if (action == "audit:retry") {
      const auto cursor = view.cursor;
      const auto retryCursor = view.failed ? view.failed->cursor : std::string{};
      view.cursor = retryCursor;
      loadServerAudit(!retryCursor.empty());
      view.cursor = cursor;
    } else if (action == "audit:refresh")
      loadServerAudit();
    else if (action == "audit:reset") {
      view.actor.clear();
      view.actorQuery.clear();
      view.action = 0;
      view.cursor.clear();
      view.entries = Json::array();
      view.expanded.clear();
      view.pending.reset();
      view.failed.reset();
      view.loaded = false;
      m_scrolling.stop(m_serverSettings.scroll);
      m_serverSettings.scroll = 0;
      loadServerAudit();
    }
    m_window.invalidate();
    return true;
  }

  void ClientApplication::prepareServerAuditDemo() {
    auto& view = m_serverAudit;
    view.guild = m_serverSettings.guild;
    const auto id = [](unsigned index) {
      // 18 October 2026, with a different minute for each administrative action.
      return std::to_string(((1792317600000ULL - index * 60000ULL - 1420070400000ULL) << 22) + index);
    };
    Json response = {
        {"users",
         Json::array(
             {{{"id", "42"}, {"username", "lysec"}, {"global_name", "Lysec"}},
              {{"id", "43"}, {"username", "mira"}, {"global_name", "Mira"}},
              {{"id", "44"}, {"username", "helper"}, {"global_name", "Community Helper"}, {"bot", true}}}
         )},
        {"threads", Json::array({{{"id", "700"}, {"name", "October desktops"}}})},
        {"webhooks", Json::array({{{"id", "800"}, {"name", "Release updates"}}})},
        {"auto_moderation_rules", Json::array({{{"id", "900"}, {"name", "Block invite spam"}}})},
        {"audit_log_entries",
         Json::array(
             {{{"id", id(0)},
               {"user_id", "42"},
               {"target_id", "101"},
               {"action_type", 11},
               {"reason", "Make the channel topic clearer for new members"},
               {"changes",
                Json::array(
                    {{{"key", "topic"},
                      {"old_value", "General chat"},
                      {"new_value", "Talk about your desktop and share what you are working on"}},
                     {{"key", "rate_limit_per_user"}, {"old_value", 0}, {"new_value", 5}}}
                )}},
              {{"id", id(1)},
               {"user_id", "43"},
               {"target_id", "50"},
               {"action_type", 31},
               {"changes",
                Json::array(
                    {{{"key", "name"}, {"old_value", "Helpers"}, {"new_value", "Community team"}},
                     {{"key", "permissions"}, {"old_value", "1024"}, {"new_value", "1099511628800"}}}
                )}},
              {{"id", id(2)},
               {"user_id", "42"},
               {"target_id", "101"},
               {"action_type", 14},
               {"options", {{"id", "50"}, {"type", "0"}, {"role_name", "Community team"}}},
               {"changes",
                Json::array(
                    {{{"key", "allow"}, {"old_value", "1024"}, {"new_value", "3072"}},
                     {{"key", "deny"}, {"old_value", "2048"}, {"new_value", "0"}}}
                )}},
              {{"id", id(3)},
               {"user_id", "43"},
               {"target_id", "44"},
               {"action_type", 25},
               {"changes",
                Json::array(
                    {{{"key", "$add"}, {"new_value", Json::array({{{"id", "50"}, {"name", "Community team"}}})}}}
                )}},
              {{"id", id(4)},
               {"user_id", "44"},
               {"target_id", "700"},
               {"action_type", 110},
               {"changes",
                Json::array(
                    {{{"key", "name"}, {"new_value", "October desktops"}}, {{"key", "archived"}, {"new_value", false}}}
                )}},
              {{"id", id(5)},
               {"user_id", "42"},
               {"target_id", "800"},
               {"action_type", 51},
               {"changes", Json::array({{{"key", "channel_id"}, {"old_value", "101"}, {"new_value", "102"}}})}},
              {{"id", id(6)},
               {"user_id", "43"},
               {"target_id", "45"},
               {"action_type", 22},
               {"reason", "Repeated spam after several warnings"}},
              {{"id", id(7)},
               {"user_id", "42"},
               {"target_id", nullptr},
               {"action_type", 73},
               {"options", {{"channel_id", "101"}, {"count", "12"}}}},
              {{"id", id(8)},
               {"user_id", "42"},
               {"target_id", "900"},
               {"action_type", 141},
               {"changes", Json::array({{{"key", "enabled"}, {"old_value", false}, {"new_value", true}}})}}}
         )}
    };
    view.replace(response, false);
    view.loaded = true;
    view.loading = false;
    view.more = false;
    view.pending.reset();
    if (m_demoOverlay.find("audit-expanded") != std::string::npos) {
      view.expanded.insert(id(2));
      // Put the expanded overwrite changes in the captured viewport rather than
      // below the filters and the two newer audit entries.
      m_scrolling.stop(m_serverSettings.scroll);
      m_serverSettings.scroll = 320;
    }
    if (m_demoOverlay.find("audit-error") != std::string::npos)
      view.error = "The audit log could not be loaded. Try again when the connection is restored.";
    if (m_demoOverlay.find("audit-empty") != std::string::npos)
      view.entries = Json::array();
  }

  float ClientApplication::drawServerAudit(Node& parent, float width) {
    auto& view = m_serverAudit;
    if (view.guild != m_serverSettings.guild) {
      resetServerAudit();
      view.guild = m_serverSettings.guild;
    }
    if (m_demo && !view.loaded)
      prepareServerAuditDemo();
    const auto access = serverSettingsAccess();
    const bool permitted =
        access.self == access.owner || api::permission::allows(access.permissions, api::permission::viewAuditLog);
    float cursor = 0;
    const auto retained = [&](const std::string& id) {
      std::unique_ptr<Node> saved;
      if (auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
        saved = std::move(at->second);
        m_detachedInputs.erase(at);
      }
      return saved;
    };
    const auto button = [&](float x, float y, float size, const std::string& caption, const std::string& action,
                            bool enabled = true) {
      const auto id = "server-button:" + action;
      auto saved = retained(id);
      auto control =
          saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
      m_widgets[id] = control.get();
      control->setText(caption);
      control->setFontSize(Style::fontSizeDescription);
      control->setPosition(x, y);
      control->setControlHeight(26);
      control->setRadius(Style::scaledRadius(Style::controlRadius));
      control->setPadding(0, 10);
      control->setFrameSize(size, 26);
      control->setEnabled(enabled);
      control->setOnClick([this, action] { serverAuditAction(action); });
      parent.addChild(std::move(control));
    };
    const auto card = [&](float top, float height) {
      auto node = std::make_unique<RectNode>();
      node->setPosition(-14, top);
      node->setSize(width + Style::spaceWide, height);
      node->setStyle({.fill = sidebar, .radius = Style::scaledRadius(Style::surfaceRadius)});
      node->setZIndex(-1);
      parent.addChild(std::move(node));
    };
    const auto description = [&](const std::string& title, const std::string& text) {
      cursor += label(parent, 0, cursor, width, title, Style::fontSizeCaption, foreground, FontWeight::SemiBold, 0);
      if (!text.empty()) {
        cursor += Style::spaceXs;
        cursor += label(parent, 0, cursor, width, text, Style::fontSizeDescription, muted, FontWeight::Normal, 0);
      }
      cursor += Style::spaceDense;
    };
    if (!permitted) {
      cursor = 16;
      description("Audit log unavailable", "Your current roles do not allow you to view this server's audit log.");
      card(0, cursor + Style::spaceXs);
      return cursor + Style::spaceXs;
    }
    const auto select = [&](const std::string& name, std::vector<std::string> options, std::size_t index,
                            std::function<void(std::size_t)> callback) {
      const auto id = "server-select:audit:" + name;
      auto saved = retained(id);
      auto control =
          saved ? std::unique_ptr<Select>(static_cast<Select*>(saved.release())) : std::make_unique<Select>();
      m_widgets[id] = control.get();
      control->setOptions(std::move(options));
      control->setFontSize(Style::fontSizeDescription);
      control->setControlHeight(30);
      control->setHorizontalPadding(10);
      control->setPosition(0, cursor);
      control->setFrameSize(width, 30);
      control->setSelectedIndexSilently(index);
      control->setOnSelectionChanged([callback = std::move(callback)](std::size_t at, std::string_view) {
        callback(at);
      });
      parent.addChild(std::move(control));
      cursor += (Style::spaceSection + Style::spaceXxs);
    };
    const auto changeFilter = [this] {
      auto& view = m_serverAudit;
      view.cursor.clear();
      view.entries = Json::array();
      view.expanded.clear();
      view.pending.reset();
      view.failed.reset();
      view.loaded = false;
      m_scrolling.stop(m_serverSettings.scroll);
      m_serverSettings.scroll = 0;
      loadServerAudit();
      m_window.invalidate();
    };
    cursor = 14;
    description(
        "Filters", "View administrative actions from the past 45 days, filtered by the person or action you choose."
    );
    cursor +=
        label(parent, 0, cursor, width, "Performed by", Style::fontSizeDescription, foreground, FontWeight::SemiBold, 0)
        + 8;
    auto& input = inputControl(
        parent, 0, cursor, width, 30, "server-settings:audit-user-search", view.actorQuery,
        "Find a user or enter their user ID"
    );
    input.setFontSize(Style::fontSizeDescription);
    input.setOnChange([this](const std::string& value) {
      m_serverAudit.actorQuery = value;
      m_window.invalidate();
    });
    input.setOnSubmit([this, changeFilter](const std::string& value) {
      if (snowflake(value)) {
        m_serverAudit.actor = value;
        changeFilter();
      }
    });
    cursor += (Style::spaceSection - Style::spaceXxs);
    std::map<std::string, std::string> actors;
    for (const auto& [id, user] : view.references.users)
      actors[id] = view.references.user(id);
    actors[m_account.id] = m_account.name;
    if (!view.actor.empty() && !actors.contains(view.actor))
      actors[view.actor] = view.actor;
    if (snowflake(view.actorQuery))
      actors.try_emplace(view.actorQuery, view.actorQuery);
    std::vector<std::pair<std::string, std::string>> ranked(actors.begin(), actors.end());
    std::ranges::sort(ranked, [](const auto& a, const auto& b) { return lower(a.second) < lower(b.second); });
    std::vector<std::string> actorIds{""}, actorNames{"All users"};
    std::size_t selectedActor = 0;
    for (const auto& [id, name] : ranked) {
      if (!view.actorQuery.empty() && id != view.actor && !lower(name + " " + id).contains(lower(view.actorQuery)))
        continue;
      if (id == view.actor)
        selectedActor = actorIds.size();
      actorIds.push_back(id);
      actorNames.push_back(name);
    }
    select("actor", std::move(actorNames), selectedActor, [this, actorIds, changeFilter](std::size_t index) {
      if (index < actorIds.size() && m_serverAudit.actor != actorIds[index]) {
        m_serverAudit.actor = actorIds[index];
        changeFilter();
      }
    });
    cursor +=
        label(parent, 0, cursor, width, "Action", Style::fontSizeDescription, foreground, FontWeight::SemiBold, 0) + 8;
    std::vector<int> actionIds{0};
    std::vector<std::string> actionNames{"All actions"};
    std::size_t selectedAction = 0;
    for (const auto& action : api::guildAuditActions()) {
      if (view.action == action.type)
        selectedAction = actionIds.size();
      actionIds.push_back(action.type);
      actionNames.push_back(std::string(action.group) + " · " + std::string(action.name));
    }
    select("action", std::move(actionNames), selectedAction, [this, actionIds, changeFilter](std::size_t index) {
      if (index < actionIds.size() && m_serverAudit.action != actionIds[index]) {
        m_serverAudit.action = actionIds[index];
        changeFilter();
      }
    });
    button(0, cursor, 92, "Refresh", "audit:refresh", !view.loading);
    if (!view.actor.empty() || view.action)
      button(102, cursor, 112, "Reset filters", "audit:reset");
    cursor += (Style::spaceSection - Style::spaceXxs);
    card(0, cursor);
    cursor += Style::spaceLg;
    const AuditLookup lookup = [&](const std::string& id) -> std::string {
      if (id.empty())
        return {};
      if (id == m_serverSettings.guild)
        return string(m_serverSettings.overview, "name", "this server");
      if (const auto user = view.references.user(id); !user.empty())
        return user;
      if (const auto resource = view.references.resource(id); !resource.empty())
        return resource;
      if (const auto channels = m_guildChannels.find(view.guild); channels != m_guildChannels.end())
        for (const auto& channel : channels->second)
          if (channel.id == id)
            return "#" + channel.name;
      if (view.guild == m_guildId)
        for (const auto& channel : m_channels)
          if (channel.id == id)
            return "#" + channel.name;
      for (const auto& role : m_roleDirectory.allRoles(view.guild))
        if (role.id == id)
          return role.name;
      return userName(id, id);
    };
    if (!view.error.empty()) {
      const auto top = cursor;
      cursor += Style::cardPadding;
      description("Could not load audit log", view.error);
      button(0, cursor, 80, "Retry", "audit:retry", !view.loading);
      cursor += (Style::spaceSection - Style::spaceXxs);
      card(top, cursor - top);
      cursor += Style::spaceLg;
    }
    if (view.loading && view.entries.empty()) {
      const auto top = cursor;
      cursor += Style::cardPadding;
      description("Loading audit log", "The server's recent administrative actions are loading.");
      card(top, cursor - top);
      return cursor;
    }
    std::string date;
    float dateTop = cursor;
    std::size_t visible = 0;
    for (const auto& entry : view.entries) {
      if ((!view.actor.empty() && string(entry, "user_id") != view.actor)
          || (view.action && entry.value("action_type", 0) != view.action))
        continue;
      ++visible;
      const auto day = auditEntryTime(entry, true);
      if (date != day) {
        if (!date.empty()) {
          card(dateTop, cursor - dateTop + Style::spaceXs);
          cursor += Style::spaceRelaxed;
        }
        date = day;
        dateTop = cursor;
        cursor += Style::cardPadding;
        cursor +=
            label(parent, 0, cursor, width, date, Style::fontSizeDescription, muted, FontWeight::SemiBold, 0) + 12;
      }
      const auto entryId = auditEntryId(entry);
      const auto actor = string(entry, "user_id");
      std::string path;
      if (const auto user = view.references.users.find(actor); user != view.references.users.end()) {
        const auto hash = string(user->second, "avatar");
        if (!hash.empty())
          path = "https://cdn.discordapp.com/avatars/" + actor + "/" + hash + ".png?size=64";
      }
      avatar(parent, 0, cursor + Style::spaceXxs, layout::Avatar::dm, path, actor.empty() ? "Discord" : lookup(actor));
      const auto bodyX = 38.0F, bodyWidth = std::max(40.0F, width - bodyX);
      const auto summary = auditEntrySummary(entry, lookup);
      const auto details = auditEntryDetails(entry, lookup);
      const auto top = cursor;
      cursor +=
          label(parent, bodyX, cursor, bodyWidth, summary, Style::fontSizeCaption, foreground, FontWeight::SemiBold, 0);
      cursor += Style::spaceXs;
      cursor += label(
          parent, bodyX, cursor, bodyWidth, auditEntryTime(entry), Style::fontSizeMini, muted, FontWeight::Normal, 0
      );
      if (const auto reason = string(entry, "reason"); !reason.empty()) {
        cursor += Style::spaceCompact;
        cursor += label(
            parent, bodyX, cursor, bodyWidth, "Reason: " + reason, Style::fontSizeDescription, foreground,
            FontWeight::Normal, 0
        );
      }
      cursor = std::max(cursor, top + Style::space2Xl);
      if (!details.empty()) {
        cursor += Style::spaceSm;
        const bool expanded = view.expanded.contains(entryId);
        button(bodyX, cursor, 128, expanded ? "Hide details" : "Show details", "audit:expand:" + entryId);
        cursor += (Style::space2Xl + Style::spaceXxs);
        if (expanded) {
          const bool columns = bodyWidth >= 390;
          const float cellWidth = columns ? (bodyWidth - (Style::spaceLg + Style::spaceXxs)) / 2 : bodyWidth;
          for (const auto& detail : details) {
            cursor += Style::spaceSm;
            cursor += label(
                          parent, bodyX, cursor, bodyWidth, detail.title, Style::fontSizeDescription, foreground,
                          FontWeight::SemiBold, 0
                      )
                + 6;
            if (!detail.hadBefore && detail.hasAfter) {
              cursor += label(
                  parent, bodyX, cursor, bodyWidth, detail.after, Style::fontSizeDescription, foreground,
                  FontWeight::Normal, 0
              );
            } else if (columns) {
              const auto oldHeight = label(
                  parent, bodyX, cursor, cellWidth, "Before\n" + detail.before, Style::fontSizeDescription, muted,
                  FontWeight::Normal, 0
              );
              const auto newHeight = label(
                  parent, bodyX + cellWidth + (Style::spaceLg + Style::spaceXxs), cursor, cellWidth,
                  "After\n" + detail.after, Style::fontSizeDescription, foreground, FontWeight::Normal, 0
              );
              cursor += std::max(oldHeight, newHeight);
            } else {
              cursor += label(
                            parent, bodyX, cursor, cellWidth, "Before: " + detail.before, Style::fontSizeDescription,
                            muted, FontWeight::Normal, 0
                        )
                  + 4;
              cursor += label(
                  parent, bodyX, cursor, cellWidth, "After: " + detail.after, Style::fontSizeDescription, foreground,
                  FontWeight::Normal, 0
              );
            }
          }
          cursor += Style::spaceDense;
        }
      }
      cursor += (Style::spaceLg + Style::spaceXxs);
    }
    if (!date.empty())
      card(dateTop, cursor - dateTop + Style::spaceXs);
    if (!visible && view.error.empty()) {
      const auto top = cursor;
      cursor += Style::cardPadding;
      description(
          view.actor.empty() && !view.action ? "No recent administrative actions" : "No matching actions",
          view.actor.empty() && !view.action ? "This server has no audit log entries from the past 45 days."
                                             : "No recent audit log entries match these user and action filters."
      );
      card(top, cursor - top);
    }
    if (view.loading && visible) {
      cursor += Style::spaceLg;
      cursor += label(
          parent, 0, cursor, width, "Loading older actions…", Style::fontSizeDescription, muted, FontWeight::Normal, 0
      );
    } else if (view.more) {
      cursor += Style::spaceLg;
      button(0, cursor, 144, "Load older actions", "audit:more", view.error.empty());
      cursor += (Style::space2Xl + Style::spaceXs);
    }
    return cursor + Style::spaceSm;
  }
} // namespace app
