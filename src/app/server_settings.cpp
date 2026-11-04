#include "api/channel_features.h"
#include "api/role_styles.h"
#include "app/client_application.h"
#include "app/settings_color_picker.h"
#include "app/settings_style.h"
#include "app/theme.h"
#include "core/input/key_modifiers.h"
#include "render/scene/rect_node.h"
#include "ui/controls/button.h"
#include "ui/controls/select.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;
  using Json = nlohmann::json;
  using Section = api::GuildSettingsSection;
  using Verb = api::GuildSettingsVerb;
  namespace {
    namespace p = api::permission;
    constexpr std::uint64_t administrator = p::administrator, manageGuild = p::manageGuild,
                            manageRoles = p::manageRoles;
    struct Page {
      const char* id;
      const char* name;
      const char* group;
      Section section;
      std::uint64_t permission;
      const char* feature = "";
    };
    constexpr Page pages[] = {
        {"profile", "Server Profile", "SERVER", Section::Profile, manageGuild},
        {"general", "Overview", "SERVER", Section::Overview, manageGuild},
        {"boosts", "Boost Status", "SERVER", Section::Boosts, manageGuild},
        {"roles", "Roles", "PEOPLE", Section::Roles, manageRoles},
        {"members", "Members", "PEOPLE", Section::Members,
         p::kickMembers
             | p::banMembers
             | manageRoles
             | p::manageNicknames
             | p::moderateMembers
             | p::muteMembers
             | p::deafenMembers
             | p::moveMembers},
        {"invites", "Invites", "PEOPLE", Section::Invites, manageGuild},
        {"applications", "Member Applications", "PEOPLE", Section::JoinRequests, p::kickMembers, "COMMUNITY"},
        {"emoji", "Emoji", "EXPRESSIONS", Section::Emoji, p::manageGuildExpressions | p::createGuildExpressions},
        {"stickers", "Stickers", "EXPRESSIONS", Section::Stickers,
         p::manageGuildExpressions | p::createGuildExpressions},
        {"soundboard", "Soundboard", "EXPRESSIONS", Section::Soundboard,
         p::manageGuildExpressions | p::createGuildExpressions},
        {"integrations", "Integrations", "APPS", Section::Integrations, manageGuild},
        {"commands", "App Permissions", "APPS", Section::InstalledApplications, manageGuild | manageRoles},
        {"webhooks", "Webhooks", "APPS", Section::Webhooks, p::manageWebhooks},
        {"safety", "Safety Setup", "MODERATION", Section::Overview, manageGuild},
        {"security", "Security Actions", "MODERATION", Section::IncidentActions, manageGuild},
        {"automod", "AutoMod", "MODERATION", Section::AutoModeration, manageGuild},
        {"audit", "Audit Log", "MODERATION", Section::AuditLog, p::viewAuditLog},
        {"bans", "Bans", "MODERATION", Section::Bans, p::banMembers},
        {"community", "Community", "COMMUNITY", Section::Overview, manageGuild},
        {"access", "Access", "COMMUNITY", Section::Access, manageGuild, "COMMUNITY"},
        {"discovery", "Discovery", "COMMUNITY", Section::Discovery, manageGuild, "COMMUNITY"},
        {"insights", "Insights", "COMMUNITY", Section::Insights, p::viewGuildInsights, "COMMUNITY"},
        {"onboarding", "Onboarding", "COMMUNITY", Section::Onboarding, manageGuild, "COMMUNITY"},
        {"guide", "Server Guide", "COMMUNITY", Section::ServerGuide, manageGuild, "COMMUNITY"},
        {"welcome", "Welcome Screen", "COMMUNITY", Section::Welcome, manageGuild, "COMMUNITY"},
        {"screening", "Rules Screening", "COMMUNITY", Section::MembershipScreening, manageGuild, "COMMUNITY"},
        {"events", "Scheduled Events", "COMMUNITY", Section::Events, p::manageEvents | p::createEvents},
        {"monetization", "Monetization", "MONETIZATION", Section::MonetizationRequirements, manageGuild, "COMMUNITY"},
        {"widget", "Widget", "ADVANCED", Section::Widget, manageGuild},
        {"vanity", "Custom Invite Link", "ADVANCED", Section::Vanity, manageGuild, "VANITY_URL"},
        {"templates", "Server Templates", "ADVANCED", Section::Templates, manageGuild},
        {"prune", "Prune Members", "DANGER ZONE", Section::Prune, manageGuild | p::kickMembers},
        {"ownership", "Transfer Ownership", "DANGER ZONE", Section::Overview, 0},
        {"delete", "Delete Server", "DANGER ZONE", Section::DeleteGuild, 0}
    };
    const Page* pageFor(const std::string& id) {
      const auto it = std::ranges::find_if(pages, [&](const auto& p) { return p.id == id; });
      return it == std::end(pages) ? nullptr : &*it;
    }
    std::string string(const Json& object, const char* key, std::string fallback = {}) {
      const auto at = object.find(key);
      if (at == object.end() || at->is_null())
        return fallback;
      if (at->is_string())
        return at->get<std::string>();
      if (at->is_number())
        return at->dump();
      return fallback;
    }
    std::string lower(std::string text) {
      std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    }
    std::string recordId(const Json& record) {
      for (const auto key : {"id", "code", "sound_id"})
        if (auto id = string(record, key); !id.empty())
          return id;
      if (record.contains("user") && record["user"].is_object())
        return string(record["user"], "id");
      return {};
    }
    std::string recordName(const Json& record) {
      if (auto name = string(record, "name"); !name.empty())
        return name;
      if (record.contains("user") && record["user"].is_object()) {
        const auto& user = record["user"];
        return string(record, "nick", string(user, "global_name", string(user, "username", "Unknown member")));
      }
      return string(record, "code", string(record, "id", "Unnamed"));
    }
    bool singleton(Section section) {
      return section == Section::Access
          || section == Section::ServerGuide
          || section == Section::IncidentActions
          || section == Section::Discovery
          || section == Section::Overview
          || section == Section::Profile
          || section == Section::Widget
          || section == Section::Welcome
          || section == Section::Onboarding
          || section == Section::Vanity
          || section == Section::MembershipScreening
          || section == Section::Prune
          || section == Section::DeleteGuild;
    }
    bool canCreate(Section section) {
      return section == Section::Roles
          || section == Section::Emoji
          || section == Section::Stickers
          || section == Section::Soundboard
          || section == Section::Webhooks
          || section == Section::Templates
          || section == Section::Events
          || section == Section::AutoModeration;
    }
    bool canDelete(Section section) {
      return canCreate(section)
          || section == Section::Bans
          || section == Section::Invites
          || section == Section::Integrations;
    }
    bool hasFeature(const ServerSettingsState& state, std::string_view name) {
      if (name.empty())
        return true;
      const auto at = state.overview.find("features");
      return at != state.overview.end() && at->is_array() && std::ranges::any_of(*at, [&](const auto& f) {
               return f.is_string() && f.template get<std::string>() == name;
             });
    }
    template <class Predicate> void eraseJson(Json& array, Predicate predicate) {
      for (auto it = array.begin(); it != array.end();)
        if (predicate(*it))
          it = array.erase(it);
        else
          ++it;
    }
    bool permissionEnabled(std::string number, unsigned bit) {
      bool remainder = false;
      for (unsigned i = 0; i <= bit; ++i) {
        unsigned carry = 0;
        std::string quotient;
        for (const char c : number) {
          const unsigned n = carry * 10 + static_cast<unsigned>(c - '0');
          if (!quotient.empty() || n / 2)
            quotient += static_cast<char>('0' + n / 2);
          carry = n % 2;
        }
        remainder = carry;
        number = quotient.empty() ? "0" : quotient;
      }
      return remainder;
    }
    std::string displayValue(const Json& value) {
      if (value.is_string())
        return value.get<std::string>();
      if (value.is_null())
        return "Unset";
      if (value.is_boolean())
        return value.get<bool>() ? "Enabled" : "Disabled";
      if (value.is_number())
        return value.dump();
      if (value.is_array())
        return std::to_string(value.size()) + " items";
      return "Updated settings";
    }
    std::string localTime(const std::string& value) {
      if (const auto time = api::muteDeadline(value)) {
        const auto epoch = std::chrono::system_clock::to_time_t(*time);
        std::tm local{};
        localtime_r(&epoch, &local);
        char text[64];
        std::strftime(text, sizeof(text), "%a %d %b, %H:%M", &local);
        return text;
      }
      return value.empty() ? "Not scheduled" : value;
    }
    std::string auditAction(int type) {
      switch (type) {
      case 1:
        return "Server updated";
      case 10:
        return "Channel created";
      case 11:
        return "Channel updated";
      case 12:
        return "Channel deleted";
      case 13:
        return "Channel permissions added";
      case 14:
        return "Channel permissions updated";
      case 15:
        return "Channel permissions removed";
      case 20:
        return "Member kicked";
      case 21:
        return "Members pruned";
      case 22:
        return "Member banned";
      case 23:
        return "Ban revoked";
      case 24:
        return "Member updated";
      case 25:
        return "Member roles updated";
      case 26:
        return "Member moved";
      case 27:
        return "Member disconnected";
      case 28:
        return "Bot added";
      case 30:
        return "Role created";
      case 31:
        return "Role updated";
      case 32:
        return "Role deleted";
      case 40:
        return "Invite created";
      case 41:
        return "Invite updated";
      case 42:
        return "Invite revoked";
      case 50:
        return "Webhook created";
      case 51:
        return "Webhook updated";
      case 52:
        return "Webhook deleted";
      case 60:
        return "Emoji created";
      case 61:
        return "Emoji updated";
      case 62:
        return "Emoji deleted";
      case 72:
        return "Message deleted";
      case 73:
        return "Messages deleted";
      case 74:
        return "Message pinned";
      case 75:
        return "Message unpinned";
      case 100:
        return "Event created";
      case 101:
        return "Event updated";
      case 102:
        return "Event deleted";
      case 140:
        return "AutoMod rule created";
      case 141:
        return "AutoMod rule updated";
      case 142:
        return "AutoMod rule deleted";
      default:
        return "Server activity";
      }
    }
    struct Permission {
      unsigned bit;
      const char* group;
      const char* name;
      const char* description;
    };
    constexpr Permission permissions[] = {
        {10, "General server permissions", "View channels", "See channels unless a channel overwrite denies access."},
        {4, "General server permissions", "Manage channels", "Create, edit and delete channels and categories."},
        {28, "General server permissions", "Manage roles", "Create and edit roles below this role in the hierarchy."},
        {30, "General server permissions", "Manage expressions",
         "Edit and remove server emoji, stickers and soundboard sounds."},
        {43, "General server permissions", "Create expressions", "Add emoji, stickers and soundboard sounds."},
        {7, "General server permissions", "View audit log", "See recorded administrative changes to this server."},
        {19, "General server permissions", "View server insights", "Read server activity and growth statistics."},
        {29, "General server permissions", "Manage webhooks", "Create, edit and delete channel webhooks."},
        {5, "General server permissions", "Manage server", "Change server settings and manage integrations."},
        {0, "Membership permissions", "Create invite", "Invite new members to this server."},
        {26, "Membership permissions", "Change nickname", "Change their own nickname in this server."},
        {27, "Membership permissions", "Manage nicknames", "Change nicknames of members below their highest role."},
        {1, "Membership permissions", "Kick members", "Remove members below their highest role."},
        {2, "Membership permissions", "Ban members", "Ban members below their highest role."},
        {40, "Membership permissions", "Moderate members",
         "Temporarily restrict a member's messages and voice participation."},
        {11, "Text channel permissions", "Send messages", "Send messages in text and announcement channels."},
        {38, "Text channel permissions", "Send messages in threads", "Participate in threads and forum posts."},
        {35, "Text channel permissions", "Create public threads", "Start public threads and forum posts."},
        {36, "Text channel permissions", "Create private threads", "Create invite-only private threads."},
        {14, "Text channel permissions", "Embed links", "Display previews for links sent in messages."},
        {15, "Text channel permissions", "Attach files", "Upload images and other files to messages."},
        {6, "Text channel permissions", "Add reactions", "Add new emoji reactions to messages."},
        {18, "Text channel permissions", "Use external emoji", "Use emoji from other servers."},
        {37, "Text channel permissions", "Use external stickers", "Use stickers from other servers."},
        {17, "Text channel permissions", "Mention everyone and roles",
         "Mention everyone, here and otherwise unmentionable roles."},
        {13, "Text channel permissions", "Manage messages", "Delete other members' messages."},
        {51, "Text channel permissions", "Pin messages", "Pin and unpin messages in channels."},
        {52, "Text channel permissions", "Bypass slowmode", "Send messages without the channel slowmode delay."},
        {53, "Text channel permissions", "Manage official messages", "Manage official server messages."},
        {34, "Text channel permissions", "Manage threads", "Edit, close and delete threads and forum posts."},
        {16, "Text channel permissions", "Read message history", "Read earlier messages in a channel."},
        {12, "Text channel permissions", "Send text-to-speech messages", "Use the /tts command."},
        {46, "Text channel permissions", "Send voice messages", "Record and send voice messages."},
        {49, "Text channel permissions", "Send polls", "Create polls in messages."},
        {20, "Voice channel permissions", "Connect", "Join voice channels."},
        {21, "Voice channel permissions", "Speak", "Speak in voice channels."},
        {9, "Voice channel permissions", "Video", "Share a camera or stream a screen."},
        {42, "Voice channel permissions", "Use soundboard", "Play server soundboard sounds in voice channels."},
        {45, "Voice channel permissions", "Use external sounds", "Play soundboard sounds from other servers."},
        {25, "Voice channel permissions", "Use voice activity", "Speak without push-to-talk."},
        {48, "Voice channel permissions", "Set voice channel status", "Set the status displayed on voice channels."},
        {8, "Voice channel permissions", "Priority speaker", "Be heard more clearly with priority push-to-talk."},
        {22, "Voice channel permissions", "Mute members", "Mute other members in voice channels."},
        {23, "Voice channel permissions", "Deafen members", "Stop other members from hearing a voice channel."},
        {24, "Voice channel permissions", "Move members", "Move other members between voice channels."},
        {32, "Voice channel permissions", "Request to speak", "Ask to speak in Stage channels."},
        {44, "Events permissions", "Create events", "Schedule events in this server."},
        {33, "Events permissions", "Manage events", "Edit and cancel other members' scheduled events."},
        {31, "Apps permissions", "Use application commands", "Use commands supplied by apps."},
        {39, "Apps permissions", "Use activities", "Start activities in supported channels."},
        {50, "Apps permissions", "Use external apps", "Allow apps to post public responses in channels."},
        {3, "Advanced permissions", "Administrator",
         "Grant every server permission and bypass channel permission overwrites."}
    };
    Json initialRecord(Section section) {
      if (section == Section::Roles)
        return {{"name", "new role"}, {"color", 0}, {"hoist", false}, {"mentionable", false}, {"permissions", "0"}};
      if (section == Section::Emoji)
        return {{"name", "new_emoji"}, {"roles", Json::array()}};
      if (section == Section::Stickers)
        return {{"name", "New sticker"}, {"description", ""}, {"tags", "🙂"}};
      if (section == Section::Soundboard)
        return {{"name", "New sound"}, {"volume", 1.0}};
      if (section == Section::Webhooks)
        return {{"name", "New webhook"}};
      if (section == Section::Events)
        return {{"name", "New event"}, {"entity_type", 3}, {"privacy_level", 2}, {"description", ""}};
      if (section == Section::AutoModeration)
        return {
            {"name", "New rule"},
            {"event_type", 1},
            {"trigger_type", 1},
            {"enabled", true},
            {"trigger_metadata", {{"keyword_filter", Json::array()}}},
            {"actions", Json::array({{{"type", 1}}})},
            {"exempt_roles", Json::array()},
            {"exempt_channels", Json::array()}
        };
      if (section == Section::Templates)
        return {{"name", "Server template"}, {"description", ""}};
      return Json::object();
    }
  } // namespace

  api::GuildSettingsAccess ClientApplication::serverSettingsAccess(const std::string& resource) const {
    api::GuildSettingsAccess access;
    access.self = m_account.id;
    access.owner = m_roleDirectory.owner(m_serverSettings.guild);
    api::Channel synthetic;
    synthetic.id = m_serverSettings.guild;
    synthetic.overwrites = std::vector<api::PermissionOverwrite>{};
    const auto roles = m_roleDirectory.allRoles(m_serverSettings.guild);
    const auto memberships = m_roleDirectory.memberships(m_serverSettings.guild, m_account.id);
    access.permissions =
        api::channelPermissions(m_serverSettings.guild, m_account.id, access.owner, roles, memberships, synthetic);
    if (memberships) {
      access.selfPosition = 0;
      for (const auto& role : roles)
        if (std::ranges::find(*memberships, role.id) != memberships->end())
          access.selfPosition = std::max(access.selfPosition, role.position);
    }
    if (!resource.empty()) {
      const Json* target = &m_serverSettings.record;
      if (m_serverSettings.resource != resource) {
        if (const auto at = m_serverSettings.lists.find(m_serverSettings.page);
            at != m_serverSettings.lists.end() && at->second.is_array())
          for (const auto& record : at->second)
            if (recordId(record) == resource) {
              target = &record;
              break;
            }
      }
      access.targetManaged = target->value("managed", false);
      if (target->contains("user") && (*target)["user"].is_object())
        access.targetCreator = string((*target)["user"], "id");
      if (access.targetCreator.empty())
        access.targetCreator = string(*target, "creator_id");
      if (access.targetCreator.empty() && target->contains("creator") && (*target)["creator"].is_object())
        access.targetCreator = string((*target)["creator"], "id");
      const auto selected = pageFor(m_serverSettings.page);
      if (selected && selected->section == Section::Roles) {
        for (const auto& role : roles)
          if (role.id == resource)
            access.targetPosition = role.position;
      } else {
        auto memberRoles = m_roleDirectory.memberships(m_serverSettings.guild, resource);
        if (!memberRoles && m_serverSettings.record.contains("roles") && m_serverSettings.record["roles"].is_array()) {
          memberRoles = std::vector<std::string>{};
          for (const auto& role : m_serverSettings.record["roles"])
            if (role.is_string())
              memberRoles->push_back(role.get<std::string>());
        }
        if (memberRoles) {
          access.targetPosition = 0;
          for (const auto& role : roles)
            if (std::ranges::find(*memberRoles, role.id) != memberRoles->end())
              access.targetPosition = std::max(access.targetPosition, role.position);
        }
      }
    }
    return access;
  }

  std::vector<std::pair<std::string, std::string>>
  ClientApplication::serverSettingsPages(const std::string& guild) const {
    std::vector<std::pair<std::string, std::string>> result;
    const auto found = std::ranges::find(m_guilds, guild, &api::Guild::id);
    if (found == m_guilds.end())
      return result;
    const auto permissions = serverGuildPermissions(guild).value_or(0);
    const bool owner = m_roleDirectory.owner(guild) == m_account.id;
    const bool admin = owner || (permissions & administrator);
    for (const auto& page : pages) {
      const bool all = std::string_view(page.id) == "prune" || std::string_view(page.id) == "commands";
      const bool allowed = page.permission
          ? admin || (all ? (permissions & page.permission) == page.permission : (permissions & page.permission) != 0)
          : owner;
      if (allowed && (!*page.feature || std::ranges::find(found->features, page.feature) != found->features.end()))
        result.emplace_back(page.id, page.name);
    }
    return result;
  }

  void ClientApplication::openServerSettings(const std::string& guild) {
    const auto id = guild.empty() ? m_guildId : guild;
    if (!api::validSnowflake(id))
      return;
    closeCompletion();
    cancelMessageEdit(false);
    closeContext();
    closeDialog();
    finishSidebarResize();
    closeChannelPanel();
    closeGifPicker();
    closeVideo();
    m_scrolling.stopAll();
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    resetServerAudit();
    resetServerCommands();
    if (m_serverSettings.imageCrop)
      discardImage(m_serverSettings.imageCrop->texture);
    discardImagesWithPrefix("server-emoji-upload:");
    discardImagesWithPrefix("server-upload:");
    discardImagesWithPrefix("server-guide-upload:");
    const auto serial = m_serverSettings.serial;
    const auto generation = m_serverSettings.generation + 1;
    m_serverSettings = {};
    m_serverSettings.serial = serial;
    m_serverSettings.generation = generation;
    m_serverSettings.open = true;
    m_serverSettings.guild = id;
    ++m_serverUploadSerial;
    m_filePicker.cancel();
    m_settingsOpen = false;
    m_presenceMenu = m_emojiMenu = false;
    const auto permissions = serverSettingsAccess();
    if (permissions.permissions
        && !(permissions.permissions.value() & (manageGuild | administrator))
        && permissions.owner != m_account.id) {
      for (const auto& page : pages)
        if (page.permission && (permissions.permissions.value() & page.permission)) {
          m_serverSettings.page = page.id;
          break;
        }
    }
    api::GuildSettingsRequest request;
    request.guildId = id;
    requestServerSettings(request);
    if (!m_guildChannels.contains(id)) {
      auto channels = request;
      channels.section = Section::Channels;
      requestServerSettings(channels);
    }
    if (pageFor(m_serverSettings.page)->section != Section::Overview)
      loadServerSettings();
    m_window.invalidate();
  }

  void ClientApplication::closeServerSettings(bool discard) {
    auto& state = m_serverSettings;
    if (!state.open)
      return;
    if (state.saving && !discard) {
      state.error = "Wait for the current server change to finish before closing settings.";
      m_window.invalidate();
      return;
    }
    if (state.dirty() && !discard) {
      m_scrolling.stop(state.scroll);
      state.scroll = 0;
      state.confirmDiscard = true;
      state.pendingPage = "close";
      m_window.invalidate();
      return;
    }
    m_scrolling.stop(state.scroll);
    m_scrolling.stop(state.navScroll);
    closeSelectDropdown();
    ++m_serverUploadSerial;
    m_filePicker.cancel();
    ++state.generation;
    state.gameTimer.stop();
    state.mfaRequest.reset();
    state.mfaChallenge = 0;
    state.mfaCode.clear();
    state.saveRemainder.reset();
    if (auto input = m_inputs.find("server-settings:mfa-code"); input != m_inputs.end())
      input->second->setValue("");
    resetServerAudit();
    resetServerCommands();
    stopServerSound();
    state.open = false;
    state.pending.clear();
    if (state.imageCrop)
      discardImage(state.imageCrop->texture);
    state.imageCrop.reset();
    state.expressionDrafts.clear();
    state.emojiUploads.clear();
    state.emojiUploading.clear();
    state.emojiUploadSaving = false;
    discardImagesWithPrefix("server-emoji-upload:");
    discardImagesWithPrefix("server-upload:");
    discardImagesWithPrefix("server-guide-upload:");
    state.guideIconDrafts.clear();
    state.expressionSaving = false;
    state.draft.clear();
    ++state.generation;
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_window.invalidate();
  }

  bool ClientApplication::requestServerSettings(api::GuildSettingsRequest request) {
    auto& state = m_serverSettings;
    request.guildId = state.guild;
    request.request = ++state.serial;
    const auto access = serverSettingsAccess(request.resourceId);
    // Read metadata is allowed before permissions arrive; mutation gates are
    // checked again against the newest role directory immediately before enqueue.
    if (request.section == Section::Members
        && request.verb == Verb::Update
        && request.values.contains("roles")
        && request.values["roles"].is_array()) {
      const auto original = state.record.value("roles", Json::array());
      const auto requested = request.values["roles"];
      auto latest = m_roleDirectory.memberships(state.guild, request.resourceId);
      auto merged = latest ? Json(*latest) : original;
      for (const auto& role : m_roleDirectory.allRoles(state.guild)) {
        const bool was = std::ranges::find(original, Json(role.id)) != original.end(),
                   wanted = std::ranges::find(requested, Json(role.id)) != requested.end();
        if (was == wanted)
          continue;
        const auto roleRecord = std::ranges::find_if(state.lists["roles"], [&](const auto& value) {
          return string(value, "id") == role.id;
        });
        const bool managed = roleRecord != state.lists["roles"].end() && roleRecord->value("managed", false);
        if (managed || (access.owner != access.self && role.position >= access.selfPosition)) {
          state.error = "You can only assign or remove unmanaged roles below your highest role.";
          m_window.invalidate();
          return false;
        }
        if (wanted && std::ranges::find(merged, Json(role.id)) == merged.end())
          merged.push_back(role.id);
        else if (!wanted)
          eraseJson(merged, [&](const auto& v) { return v == role.id; });
      }
      request.values["roles"] = std::move(merged);
    }
    if (request.verb != Verb::Get && !api::canManageGuildSettings(request, access)) {
      state.error = "Your permissions or role hierarchy no longer allow this change.";
      m_window.invalidate();
      return false;
    }
    if (request.verb != Verb::Get && !api::guildSettingsHttpRequest(request)) {
      state.error = "Check the required fields before saving. This change contains an invalid or missing value.";
      m_window.invalidate();
      return false;
    }
    if (m_demo || m_client.guildSettings(request)) {
      const auto tracks = [](const auto& request) {
        return request.section == Section::Overview
            || request.section == Section::Roles
            || request.section == Section::Members;
      };
      if (tracks(request)
          && std::ranges::none_of(state.pending, [&](const auto& item) { return tracks(item.second); })) {
        state.roleJournal.clear();
        state.roleJournalOverflow = false;
      }
      request.mfaCode.clear();
      state.pending.emplace(request.request, request);
      state.error.clear();
      state.status.clear();
      if (request.verb == Verb::Get)
        state.loading = true;
      else
        state.saving = true;
      return true;
    }
    state.error = "Could not send the request. Check the connection and try again.";
    m_window.invalidate();
    return false;
  }

  void ClientApplication::loadServerSettings(bool more) {
    auto& state = m_serverSettings;
    if (state.page == "audit") {
      loadServerAudit(more);
      return;
    }
    if (loadServerCommands(more))
      return;
    if (loadServerManagement(more))
      return;
    const auto page = pageFor(state.page);
    if (!page || state.saving)
      return;
    api::GuildSettingsRequest request;
    request.section = page->section;
    if (page->section == Section::DeleteGuild || state.page == "ownership")
      return;
    request.query = state.search;
    if (more)
      request.cursor = state.cursor;
    if (state.page == "members" && !state.resource.empty())
      request.resourceId = state.resource;
    requestServerSettings(std::move(request));
    if (page->section == Section::Discovery && !state.loaded.contains("discovery-categories")) {
      api::GuildSettingsRequest categories;
      categories.section = Section::Discovery;
      categories.resourceId = "categories";
      requestServerSettings(categories);
      categories.resourceId = "requirements";
      requestServerSettings(categories);
    }
  }

  void ClientApplication::selectServerSettingsPage(const std::string& page, bool discard) {
    auto& state = m_serverSettings;
    if (!pageFor(page) || state.saving)
      return;
    if (state.dirty() && !discard) {
      m_scrolling.stop(state.scroll);
      state.scroll = 0;
      state.confirmDiscard = true;
      state.pendingPage = page;
      return;
    }
    closeSelectDropdown();
    ++m_serverUploadSerial;
    m_filePicker.cancel();
    ++state.generation;
    state.gameTimer.stop();
    state.mfaRequest.reset();
    state.mfaChallenge = 0;
    state.mfaCode.clear();
    state.saveRemainder.reset();
    if (auto input = m_inputs.find("server-settings:mfa-code"); input != m_inputs.end())
      input->second->setValue("");
    if (state.page == "soundboard" && page != "soundboard")
      stopServerSound();
    if (state.page == "audit" && page != "audit")
      resetServerAudit();
    resetServerCommands();
    state.lists.erase("event-local-fields");
    state.profileColorPicker.clear();
    state.colorPicker = false;
    state.page = page;
    state.resource.clear();
    state.record = Json::object();
    state.draft = Json::object();
    if (state.imageCrop)
      discardImage(state.imageCrop->texture);
    state.imageCrop.reset();
    state.expressionDrafts.clear();
    state.emojiUploads.clear();
    state.emojiUploading.clear();
    state.emojiUploadSaving = false;
    discardImagesWithPrefix("server-emoji-upload:");
    discardImagesWithPrefix("server-upload:");
    discardImagesWithPrefix("server-guide-upload:");
    state.guideIconDrafts.clear();
    state.expressionSaving = false;
    state.creating = false;
    state.confirmDelete = state.confirmDiscard = false;
    state.error.clear();
    state.status.clear();
    state.search.clear();
    state.cursor.clear();
    state.more = false;
    m_scrolling.stop(state.scroll);
    state.scroll = 0;
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    if (page == "audit")
      loadServerAudit();
    else if (page == "commands")
      loadServerCommands();
    else if (page == "access" && !m_demo)
      loadServerSettings();
    else if (!state.loaded.contains(page) && pageFor(page)->section != Section::Overview)
      loadServerSettings();
    else if (pageFor(page)->section == Section::Overview)
      state.record = state.overview;
    else if (singleton(pageFor(page)->section) && state.lists.contains(page))
      state.record = state.lists.at(page);
    m_window.invalidate();
  }

  void ClientApplication::setServerSettingsValue(const std::string& key, Json value) {
    auto& state = m_serverSettings;
    if (!state.open || state.saving || state.mfaRequest)
      return;
    if (key == "trigger_type") {
      state.draft["trigger_metadata"] = value == 5
          ? Json{{"mention_total_limit", 5}, {"mention_raid_protection_enabled", true}}
          : value == 4 ? Json{{"presets", Json::array({1, 2, 3})}}
          : value == 1 ? Json{{"keyword_filter", Json::array()}}
                       : Json::object();
      state.draft["actions"] = Json::array({{{"type", 1}}});
    }
    if (state.page == "soundboard" && key == "emoji_name" && !value.is_null())
      state.draft["emoji_id"] = nullptr;
    if (state.page == "roles" && key == "unicode_emoji" && !value.is_null() && value != "")
      state.draft["icon"] = nullptr;
    if (state.page == "roles" && key == "icon" && !value.is_null())
      state.draft["unicode_emoji"] = nullptr;
    if (key == "colors")
      state.draft.erase("color");
    const auto original = state.record.find(key);
    if (original != state.record.end()
        && (*original == value
            || ((key == "brand_color_primary" || key == "badge_color_primary" || key == "badge_color_secondary")
                && original->is_string()
                && value.is_string()
                && lower(original->get<std::string>()) == lower(value.get<std::string>()))))
      state.draft.erase(key);
    else
      state.draft[key] = std::move(value);
    state.error.clear();
    state.status.clear();
    state.confirmDelete = false;
    m_window.invalidate();
  }

  void ClientApplication::serverSettingsRoleUpdate(const api::RoleUpdates& updates) {
    auto& state = m_serverSettings;
    if (!state.open)
      return;
    if (std::ranges::none_of(state.pending, [](const auto& item) {
          return item.second.section == Section::Overview
              || item.second.section == Section::Roles
              || item.second.section == Section::Members;
        }))
      return;
    api::RoleUpdates projected;
    for (const auto& update : updates.guilds)
      if (update.guild == state.guild)
        projected.guilds.push_back(update);
    if (projected.guilds.empty())
      return;
    if (state.roleJournal.size() < 128)
      state.roleJournal.push_back(std::move(projected));
    else
      state.roleJournalOverflow = true;
  }

  void ClientApplication::serverSettingsResult(const api::Event& event) {
    auto& state = m_serverSettings;
    if (!state.open || !event.guildSettingsRequest)
      return;
    if (serverAuditResult(event))
      return;
    if (serverCommandsResult(event))
      return;
    if (serverExtraResult(event))
      return;
    if (serverManagementResult(event))
      return;
    const auto& request = *event.guildSettingsRequest;
    const auto pending = state.pending.find(request.request);
    if (pending == state.pending.end() || pending->second != request || request.guildId != state.guild)
      return;
    state.pending.erase(pending);
    state.loading = std::ranges::any_of(state.pending, [](const auto& pair) { return pair.second.verb == Verb::Get; });
    state.saving = m_serverCommands.preparing.has_value()
        || std::ranges::any_of(state.pending, [](const auto& pair) { return pair.second.verb != Verb::Get; });
    if (event.guildSettingsMfaChallenge) {
      m_scrolling.stop(state.scroll);
      state.scroll = 0;
      m_focusedInput = "server-settings:mfa-code";
      state.mfaRequest = request;
      state.mfaChallenge = event.guildSettingsMfaChallenge;
      state.mfaMethods = event.guildSettingsMfaMethods;
      state.mfaType = state.mfaMethods.empty() ? "totp" : state.mfaMethods.front();
      state.mfaCode.clear();
      state.error = request.mfaChallenge ? event.error : std::string{};
      if (auto input = m_inputs.find("server-settings:mfa-code"); input != m_inputs.end())
        input->second->setValue("");
      m_window.invalidate();
      return;
    }
    if (!event.error.empty()) {
      state.saveRemainder.reset();
      state.expressionSaving = false;
      state.emojiUploadSaving = false;
      state.error = event.error;
      m_window.invalidate();
      return;
    }
    const auto applyRoles = [&](const api::RoleUpdates& updates) {
      if (state.roleJournalOverflow)
        return;
      m_roleDirectory.apply(updates);
      for (const auto& live : state.roleJournal)
        m_roleDirectory.apply(live);
    };
    auto data = event.guildSettings;
    if (request.section == Section::AuditLog && data.is_object() && data.contains("audit_log_entries")) {
      if (data.contains("users") && data["users"].is_array())
        state.lists["audit-users"] = data["users"];
      if (data.contains("users") && data["users"].is_array())
        for (const auto& user : data["users"])
          m_userDirectory.consume("USER_UPDATE", user);
      data = data["audit_log_entries"];
    }
    if (request.section == Section::Channels && data.is_array()) {
      m_guildChannels[state.guild] = api::parseChannels(data);
      m_window.invalidate();
      return;
    }
    if (request.section == Section::Discovery && !request.resourceId.empty() && request.verb == Verb::Get) {
      state.lists["discovery-" + request.resourceId] = data;
      state.loaded.insert("discovery-" + request.resourceId);
      m_window.invalidate();
      return;
    }
    if (request.section == Section::Applications) {
      if (request.query == state.gameQuery && state.page == "profile" && data.is_array()) {
        state.gameResults = data;
        for (const auto& game : data)
          state.gameNames[string(game, "id")] = string(game, "name");
      }
      m_window.invalidate();
      return;
    }
    if (request.verb == Verb::Get) {
      if (request.section == Section::Overview && data.is_object()) {
        state.overview = data;
        auto projection = data;
        projection["id"] = state.guild;
        applyRoles(api::parseRoleUpdates("GUILD_CREATE", projection));
        for (const auto& page : pages)
          if (page.section == Section::Overview)
            state.loaded.insert(page.id);
        if (pageFor(state.page)->section == Section::Overview)
          state.record = data;
      } else {
        for (const auto& page : pages)
          if (page.section == request.section) {
            if (!request.resourceId.empty() && page.section == Section::Members) {
              if (state.page == page.id && state.resource == request.resourceId)
                state.record = data;
              if (data.is_object()) {
                auto member = data;
                member["guild_id"] = state.guild;
                applyRoles(api::parseRoleUpdates("GUILD_MEMBER_UPDATE", member));
              }
            } else {
              if (!request.cursor.empty()
                  && state.lists.contains(page.id)
                  && state.lists[page.id].is_array()
                  && data.is_array()) {
                auto& list = state.lists[page.id];
                for (const auto& record : data) {
                  const auto id = recordId(record);
                  if (std::ranges::none_of(list, [&](const auto& existing) { return recordId(existing) == id; }))
                    list.push_back(record);
                }
              } else
                state.lists[page.id] = data;
              state.loaded.insert(page.id);
              if (state.page == page.id && singleton(page.section))
                state.record = data;
              if (page.section == Section::Profile
                  && data.is_object()
                  && data.contains("games")
                  && data["games"].is_array())
                for (const auto& game : data["games"])
                  state.gameNames[string(game, "id", string(game, "application_id"))] = string(game, "name");
              if (page.section == Section::Members && data.is_array())
                applyRoles(api::parseRoleUpdates("GUILD_CREATE", {{"id", state.guild}, {"members", data}}));
              if (page.section == Section::Roles && data.is_array())
                applyRoles(api::parseRoleUpdates("GUILD_CREATE", {{"id", state.guild}, {"roles", data}}));
            }
          }
      }
      if (pageFor(state.page)->section == request.section) {
        state.more = event.guildSettingsMore;
        state.cursor = event.guildSettingsCursor;
      }
    } else {
      if (request.section == Section::Emoji
          && request.verb == Verb::Create
          && state.emojiUploadSaving
          && !state.emojiUploading.empty()) {
        const auto found = std::ranges::find_if(state.emojiUploads, [&](const auto& file) {
          return string(file, "id") == state.emojiUploading;
        });
        if (found != state.emojiUploads.end()) {
          auto record = data.is_object() ? data : Json::object();
          if (!record.contains("name"))
            record["name"] = (*found)["name"];
          if (!record.contains("animated"))
            record["animated"] = (*found)["animated"];
          if (!record.contains("user"))
            record["user"] = {{"id", m_account.id}, {"username", m_account.username}, {"global_name", m_account.name}};
          if (!state.lists["emoji"].is_array())
            state.lists["emoji"] = Json::array();
          state.lists["emoji"].push_back(record);
          const auto texture = "server-emoji-upload:" + state.emojiUploading;
          if (const auto at = m_images.find(texture); at != m_images.end()) {
            if (const auto image = api::guildExpressionAsset(Section::Emoji, record);
                !image.empty() && !record.value("animated", false)) {
              const auto next = std::exchange(at->second.texture, TextureHandle{});
              replaceCachedImageTexture(m_images[image], m_window.context().textureManager(), next);
            }
            discardImage(texture);
          }
          state.emojiUploads.erase(found);
        }
        state.emojiUploading.clear();
        state.mfaRequest.reset();
        state.mfaChallenge = 0;
        state.mfaCode.clear();
        if (auto input = m_inputs.find("server-settings:mfa-code"); input != m_inputs.end())
          input->second->setValue("");
        if (!state.emojiUploads.empty() || !state.expressionDrafts.empty())
          serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "save"});
        else {
          state.emojiUploadSaving = false;
          state.expressionSaving = false;
          state.status = "Emoji uploaded.";
        }
        m_window.invalidate();
        return;
      }
      if (request.section == Section::Emoji
          && request.verb == Verb::Update
          && state.expressionDrafts.contains(request.resourceId)) {
        if (auto at = state.lists.find("emoji"); at != state.lists.end() && at->second.is_array())
          for (auto& record : at->second)
            if (recordId(record) == request.resourceId) {
              for (const auto& [key, entry] : request.values.items())
                record[key] = entry;
              if (data.is_object())
                for (const auto& [key, entry] : data.items())
                  record[key] = entry;
            }
        state.expressionDrafts.erase(request.resourceId);
        state.mfaRequest.reset();
        state.mfaChallenge = 0;
        state.mfaCode.clear();
        if (auto input = m_inputs.find("server-settings:mfa-code"); input != m_inputs.end())
          input->second->setValue("");
        if (state.expressionSaving && (!state.expressionDrafts.empty() || !state.emojiUploads.empty()))
          serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "save"});
        else {
          state.expressionSaving = false;
          state.status = "Emoji names saved.";
        }
        m_window.invalidate();
        return;
      }
      state.mfaRequest.reset();
      state.mfaChallenge = 0;
      state.mfaCode.clear();
      if (auto input = m_inputs.find("server-settings:mfa-code"); input != m_inputs.end())
        input->second->setValue("");
      if (request.verb == Verb::Delete)
        state.draft.clear();
      if (request.values.is_object())
        for (auto it = request.values.begin(); it != request.values.end(); ++it)
          if (state.draft.contains(it.key()) && (state.draft[it.key()] == it.value() || it.key() == "roles"))
            state.draft.erase(it.key());
      state.confirmDelete = false;
      state.status = "Changes saved.";
      if (request.section == Section::DeleteGuild) {
        const auto guild = state.guild;
        const bool selectedGuild = m_guildId == guild;
        std::set<std::string> removed;
        if (const auto channels = m_guildChannels.find(guild); channels != m_guildChannels.end())
          for (const auto& channel : channels->second)
            removed.insert(channel.id);
        for (const auto& thread : m_threads.list(guild))
          removed.insert(thread.id);
        for (const auto& [channel, owner] : m_unreadGuilds)
          if (owner == guild)
            removed.insert(channel);
        if (selectedGuild)
          for (const auto& channel : m_channels)
            removed.insert(channel.id);
        closeServerSettings(true);
        api::Event deletion;
        deletion.resource = guild;
        deletion.removed.assign(removed.begin(), removed.end());
        removeGuildChannels(deletion);
        std::erase_if(m_guilds, [&](const auto& item) { return item.id == guild; });
        m_guildUpdates.erase(guild);
        m_guildChannels.erase(guild);
        m_lastChannels.erase(guild);
        for (auto& folder : m_folders)
          std::erase(folder.guildIds, guild);
        std::erase_if(m_folders, [](const auto& folder) { return !folder.id.empty() && folder.guildIds.empty(); });
        m_roleDirectory.apply({false, {api::GuildRoleUpdate{.guild = guild, .removed = true}}});
        m_threads.apply({false, {api::ThreadChange{.kind = api::ThreadChange::Kind::RemoveGuild, .guild = guild}}});
        if (selectedGuild)
          showDirectMessages();
        m_window.invalidate();
        return;
      }
      if (request.section == Section::OwnershipCode) {
        state.status = "A verification code was sent to your account email address.";
        m_window.invalidate();
        return;
      }
      if (request.section == Section::Overview) {
        for (auto it = request.values.begin(); it != request.values.end(); ++it)
          if (it.key() != "code")
            state.overview[it.key()] = it.value();
        if (data.is_object())
          for (auto it = data.begin(); it != data.end(); ++it)
            state.overview[it.key()] = it.value();
        state.record = state.overview;
        if (request.values.contains("owner_id")) {
          state.overview.erase("code");
          state.record.erase("code");
          state.draft.erase("code");
          if (auto input = m_inputs.find("server-settings:ownership:code"); input != m_inputs.end())
            input->second->setValue("");
        }
        applyRoles(api::parseRoleUpdates("GUILD_UPDATE", state.overview));
        const auto updates = api::parseGuildUpdates("GUILD_UPDATE", state.overview);
        m_messageLayouts.clear();
        m_completionDirty = true;
        for (const auto& update : updates) {
          m_guildUpdates.try_emplace(update.id, api::GuildUpdate{.id = update.id}).first->second.merge(update);
          for (auto& guild : m_guilds)
            if (guild.id == update.id)
              update.apply(guild);
        }
      } else {
        if (request.section == Section::Roles) {
          if (request.verb == Verb::Delete)
            applyRoles(
                api::parseRoleUpdates("GUILD_ROLE_DELETE", {{"guild_id", state.guild}, {"role_id", request.resourceId}})
            );
          else if (data.is_object() && data.contains("id"))
            applyRoles(api::parseRoleUpdates("GUILD_ROLE_UPDATE", {{"guild_id", state.guild}, {"role", data}}));
          else if (data.is_array())
            applyRoles(api::parseRoleUpdates("GUILD_CREATE", {{"id", state.guild}, {"roles", data}}));
        }
        if (request.section == Section::Members) {
          if (request.verb == Verb::Delete)
            applyRoles(
                api::parseRoleUpdates(
                    "GUILD_MEMBER_REMOVE", {{"guild_id", state.guild}, {"user_id", request.resourceId}}
                )
            );
          else {
            auto member = data.is_object() && !data.empty() ? data : state.record;
            for (const auto& [key, entry] : request.values.items())
              member[key] = entry;
            member["guild_id"] = state.guild;
            member["user_id"] = request.resourceId;
            applyRoles(api::parseRoleUpdates("GUILD_MEMBER_UPDATE", member));
          }
        }
        if (request.section == Section::Profile) {
          for (auto& guild : m_guilds)
            if (guild.id == state.guild) {
              if (request.values.contains("name")) {
                guild.name = request.values["name"].get<std::string>();
                state.overview["name"] = guild.name;
              }
              if (request.values.contains("icon"))
                state.loaded.erase("profile");
            }
        }
        state.loaded.erase(state.page);
        if (request.verb == Verb::Update && request.values.is_object())
          for (const auto& [key, entry] : request.values.items())
            state.record[key] = entry;
        const auto createdId = data.is_object() ? recordId(data) : std::string{};
        if (request.verb == Verb::Delete) {
          state.resource.clear();
          state.record = Json::object();
          state.creating = false;
        } else if (data.is_object() && !data.empty()) {
          if (request.verb == Verb::Update) {
            for (const auto& [key, entry] : request.values.items())
              state.record[key] = entry;
            for (const auto& [key, entry] : data.items())
              state.record[key] = entry;
          } else
            state.record = data;
          state.resource = createdId.empty() ? request.resourceId : createdId;
          state.creating = false;
        }
        loadServerSettings();
      }
    }
    if (state.saveRemainder && request.verb != Verb::Get) {
      auto followup = std::move(*state.saveRemainder);
      state.saveRemainder.reset();
      requestServerSettings(std::move(followup));
    }
    m_window.invalidate();
  }

  bool ClientApplication::serverSettingsAction(const Hit& hit) {
    if (hit.action == Action::ServerSettings) {
      openServerSettings(hit.id);
      return true;
    }
    if (hit.action == Action::CloseServerSettings) {
      closeServerSettings();
      return true;
    }
    if (hit.action != Action::ServerSettingsAction)
      return false;
    auto& state = m_serverSettings;
    if (!state.open)
      return true;
    const auto cancelVerification = [&] {
      state.mfaRequest.reset();
      state.mfaChallenge = 0;
      state.mfaCode.clear();
      state.saveRemainder.reset();
      state.lists.erase("event-local-fields");
      if (auto input = m_inputs.find("server-settings:mfa-code"); input != m_inputs.end())
        input->second->setValue("");
    };
    const auto changeEditor = [&] {
      ++state.generation;
      ++m_serverUploadSerial;
      m_filePicker.cancel();
      state.gameTimer.stop();
      closeSelectDropdown();
      cancelVerification();
    };
    const auto page = pageFor(state.page);
    if (!page)
      return true;
    const auto& id = hit.id;
    if (serverAuditAction(id))
      return true;
    if (serverCommandsAction(id))
      return true;
    if (serverImageCropAction(id))
      return true;
    if (serverExtraAction(id))
      return true;
    if (serverManagementAction(id))
      return true;
    if (id == "send-ownership-code") {
      api::GuildSettingsRequest request;
      request.section = Section::OwnershipCode;
      request.verb = Verb::Create;
      requestServerSettings(request);
    } else if (id.starts_with("page:"))
      selectServerSettingsPage(id.substr(5));
    else if (id == "keep-editing") {
      state.confirmDiscard = state.confirmDelete = false;
      state.destructiveAction.clear();
      if (state.lists.contains("expression-delete")) {
        state.lists.erase("expression-delete");
        state.resource.clear();
        state.record = Json::object();
      }
    } else if (id == "discard") {
      state.draft.clear();
      if (state.imageCrop)
        discardImage(state.imageCrop->texture);
      state.imageCrop.reset();
      state.expressionDrafts.clear();
      state.emojiUploads.clear();
      state.emojiUploading.clear();
      state.emojiUploadSaving = false;
      discardImagesWithPrefix("server-emoji-upload:");
      discardImagesWithPrefix("server-upload:");
      discardImagesWithPrefix("server-guide-upload:");
      state.guideIconDrafts.clear();
      state.expressionSaving = false;
      state.confirmDiscard = false;
      if (state.pendingPage == "close")
        closeServerSettings(true);
      else if (state.pendingPage == "new") {
        state.pendingPage.clear();
        serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "new"});
      } else if (state.pendingPage == "expression-delete") {
        const auto target = state.pendingResource;
        state.pendingResource.clear();
        state.pendingPage.clear();
        serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "expression-delete:" + target});
      } else if (state.pendingPage == "record") {
        const auto target = state.pendingResource;
        state.pendingResource.clear();
        state.pendingPage.clear();
        serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, "record:" + target});
      } else
        selectServerSettingsPage(state.pendingPage, true);
    } else if (id == "reset") {
      changeEditor();
      ++m_serverUploadSerial;
      m_filePicker.cancel();
      state.draft.clear();
      if (state.imageCrop)
        discardImage(state.imageCrop->texture);
      state.imageCrop.reset();
      state.expressionDrafts.clear();
      state.emojiUploads.clear();
      state.emojiUploading.clear();
      state.emojiUploadSaving = false;
      discardImagesWithPrefix("server-emoji-upload:");
      discardImagesWithPrefix("server-upload:");
      discardImagesWithPrefix("server-guide-upload:");
      state.guideIconDrafts.clear();
      state.expressionSaving = false;
      state.error.clear();
      state.confirmDelete = false;
      if (state.creating) {
        state.creating = false;
        state.resource.clear();
        state.record = Json::object();
      }
    } else if (id == "retry" || id == "search") {
      state.cursor.clear();
      loadServerSettings();
    } else if (id == "more")
      loadServerSettings(true);
    else if (id.starts_with("upload:"))
      startServerSettingsUpload(id.substr(7));
    else if (id.starts_with("emoji-upload-remove:")) {
      const auto key = id.substr(20);
      eraseJson(state.emojiUploads, [&](const auto& file) { return string(file, "id") == key; });
      discardImage("server-emoji-upload:" + key);
    } else if (id.starts_with("clear-upload:"))
      setServerSettingsValue(id.substr(13), nullptr);
    else if (id == "back") {
      if (state.dirty()) {
        m_scrolling.stop(state.scroll);
        state.scroll = 0;
        state.confirmDiscard = true;
        state.pendingPage = state.page;
      } else {
        changeEditor();
        state.resource.clear();
        state.creating = false;
        state.record = Json::object();
      }
    } else if (id == "new" && canCreate(page->section)) {
      if (state.dirty()) {
        m_scrolling.stop(state.scroll);
        state.scroll = 0;
        state.confirmDiscard = true;
        state.pendingPage = "new";
      } else {
        changeEditor();
        state.creating = true;
        state.resource.clear();
        state.record = Json::object();
        state.draft = initialRecord(page->section);
        state.error.clear();
        state.scroll = 0;
        if (page->section == Section::Emoji)
          startServerSettingsUpload("image");
        else if (page->section == Section::Stickers)
          startServerSettingsUpload("file");
        else if (page->section == Section::Soundboard)
          startServerSettingsUpload("sound");
      }
    } else if (id.starts_with("expression-delete:")) {
      const auto resource = id.substr(18);
      if (state.dirty()) {
        m_scrolling.stop(state.scroll);
        state.scroll = 0;
        state.confirmDiscard = true;
        state.pendingPage = "expression-delete";
        state.pendingResource = resource;
      } else if (const auto at = state.lists.find(state.page); at != state.lists.end() && at->second.is_array()) {
        const auto record =
            std::ranges::find_if(at->second, [&](const auto& row) { return recordId(row) == resource; });
        if (record != at->second.end()) {
          changeEditor();
          state.resource = resource;
          state.record = *record;
          state.lists["expression-delete"] = true;
          state.confirmDelete = true;
          state.destructiveAction = "delete";
          m_scrolling.stop(state.scroll);
          state.scroll = 0;
        }
      }
    } else if (id.starts_with("role-members:")) {
      state.lists["member-role-filter"] = id.substr(13);
      state.loaded.erase("members");
      selectServerSettingsPage("members");
    } else if (id.starts_with("record:")) {
      const auto resource = id.substr(7);
      if (state.dirty()) {
        m_scrolling.stop(state.scroll);
        state.scroll = 0;
        state.confirmDiscard = true;
        state.pendingPage = "record";
        state.pendingResource = resource;
      } else if (const auto at = state.lists.find(state.page); at != state.lists.end() && at->second.is_array()) {
        const auto found =
            std::ranges::find_if(at->second, [&](const auto& record) { return recordId(record) == resource; });
        if (found != at->second.end()) {
          changeEditor();
          closeSelectDropdown();
          ++m_serverUploadSerial;
          m_filePicker.cancel();
          state.resource = resource;
          state.record = *found;
          state.creating = false;
          state.scroll = 0;
          state.error.clear();
          state.confirmDelete = false;
          state.reason.clear();
        }
      }
    } else if (id == "create-invite") {
      const auto& cached = m_guildChannels[state.guild];
      const auto channel = std::ranges::find_if(cached, [&](const auto& c) {
        const auto access = serverSettingsAccess();
        const auto permissions = api::channelPermissions(
            state.guild, m_account.id, access.owner, m_roleDirectory.allRoles(state.guild),
            m_roleDirectory.memberships(state.guild, m_account.id), c
        );
        return c.type != 4 && !c.isThread() && p::allows(permissions, p::createInstantInvite);
      });
      if (channel != cached.end()) {
        api::Interaction action;
        action.kind = api::InteractionKind::CreateInvite;
        action.guild = state.guild;
        action.channel = channel->id;
        closeServerSettings(true);
        openChannelInvite(action);
      } else
        state.error = "No available channel allows you to create an invite.";
    } else if (id == "copy-invite") {
      m_window.copy("https://discord.gg/" + state.resource);
    } else if (id == "copy-template") {
      m_window.copy("https://discord.new/" + state.resource);
    } else if (id == "sync-template") {
      api::GuildSettingsRequest request;
      request.section = Section::Templates;
      request.verb = Verb::Sync;
      request.resourceId = state.resource;
      requestServerSettings(request);
    } else if (id.starts_with("profile-color:")) {
      ++state.generation;
      const auto key = id.substr(14);
      state.profileColorPicker = state.profileColorPicker == key ? "" : key;
    } else if (id == "add-trait" || id.starts_with("remove-trait:")) {
      auto traits = state.draft.value("traits", state.record.value("traits", Json::array()));
      if (id == "add-trait" && traits.size() < 5)
        traits.push_back(
            {{"label", "New trait"},
             {"position", traits.size()},
             {"emoji_id", nullptr},
             {"emoji_name", "✨"},
             {"emoji_animated", false}}
        );
      else if (id.starts_with("remove-trait:")) {
        const auto index = std::stoul(id.substr(13));
        if (index < traits.size())
          traits.erase(traits.begin() + index);
      }
      for (std::size_t i = 0; i < traits.size(); ++i)
        traits[i]["position"] = i;
      setServerSettingsValue("traits", traits);
    } else if (id.starts_with("add-game:") || id.starts_with("remove-game:")) {
      auto games = state.draft.value("game_application_ids", state.record.value("game_application_ids", Json::array()));
      const auto key = id.substr(id.find(':') + 1);
      if (id.starts_with("add-game:") && games.size() < 20 && std::ranges::find(games, Json(key)) == games.end())
        games.push_back(key);
      else if (id.starts_with("remove-game:"))
        eraseJson(games, [&](const auto& game) { return game == key; });
      setServerSettingsValue("game_application_ids", games);
    } else if (id.starts_with("role-color-component:")) {
      ++state.generation;
      const auto key = id.substr(21);
      state.profileColorPicker = state.profileColorPicker == key ? "" : key;
    } else if (id == "default-role-color") {
      if (hasFeature(state, "ENHANCED_ROLE_COLORS") || state.record.contains("colors"))
        setServerSettingsValue(
            "colors", {{"primary_color", 0}, {"secondary_color", nullptr}, {"tertiary_color", nullptr}}
        );
      else
        setServerSettingsValue("color", 0);
    } else if (id == "role-color-picker") {
      state.colorPicker = !state.colorPicker;
    } else if (id == "copy-widget") {
      m_window.copy("https://discord.com/widget?id=" + state.guild);
    } else if (id == "mfa-verify") {
      if (state.mfaRequest && !state.mfaCode.empty()) {
        auto request = *state.mfaRequest;
        request.mfaChallenge = state.mfaChallenge;
        request.mfaType = state.mfaType;
        request.mfaCode = state.mfaCode;
        requestServerSettings(std::move(request));
        state.mfaCode.clear();
        if (auto input = m_inputs.find("server-settings:mfa-code"); input != m_inputs.end())
          input->second->setValue("");
      }
    } else if (id == "mfa-cancel") {
      cancelVerification();
      state.mfaRequest.reset();
      state.mfaCode.clear();
      state.mfaChallenge = 0;
      state.error.clear();
    } else if (id == "add-welcome") {
      auto channels = state.draft.value("welcome_channels", state.record.value("welcome_channels", Json::array()));
      const auto& cached = m_guildChannels[state.guild];
      const auto channel = std::ranges::find_if(cached, [](const auto& c) { return c.type == 0 || c.type == 5; });
      if (channels.size() < 5 && channel != cached.end()) {
        channels.push_back(
            {{"channel_id", channel->id}, {"description", "Start here"}, {"emoji_id", nullptr}, {"emoji_name", "👋"}}
        );
        setServerSettingsValue("welcome_channels", channels);
      }
    } else if (id.starts_with("remove-welcome:")) {
      auto channels = state.draft.value("welcome_channels", state.record.value("welcome_channels", Json::array()));
      const auto index = std::stoul(id.substr(15));
      if (index < channels.size()) {
        channels.erase(channels.begin() + index);
        setServerSettingsValue("welcome_channels", channels);
      }
    } else if (id == "add-rule") {
      auto fields = state.draft.value("form_fields", state.record.value("form_fields", Json::array()));
      auto found = std::ranges::find_if(fields, [](const auto& f) {
        return f.is_object() && f.value("field_type", "") == "TERMS";
      });
      if (found == fields.end())
        fields.push_back(
            {{"field_type", "TERMS"}, {"label", "Server rules"}, {"required", true}, {"values", Json::array({""})}}
        );
      else if ((*found)["values"].size() < 20)
        (*found)["values"].push_back("");
      setServerSettingsValue("form_fields", fields);
    } else if (id.starts_with("remove-rule:")) {
      const auto separator = id.find(':', 12);
      if (separator != std::string::npos) {
        const auto i = std::stoul(id.substr(12, separator - 12)), j = std::stoul(id.substr(separator + 1));
        auto fields = state.draft.value("form_fields", state.record.value("form_fields", Json::array()));
        if (i < fields.size() && j < fields[i]["values"].size()) {
          fields[i]["values"].erase(fields[i]["values"].begin() + j);
          setServerSettingsValue("form_fields", fields);
        }
      }
    } else if (
        id == "add-prompt"
        || id.starts_with("remove-prompt:")
        || id.starts_with("add-option:")
        || id.starts_with("remove-option:")
    ) {
      auto prompts = state.draft.value("prompts", state.record.value("prompts", Json::array()));
      const auto newId = [&] {
        const auto millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count()
            - 1420070400000LL;
        return std::to_string((static_cast<std::uint64_t>(millis) << 22) + (++state.serial & 0x3fffff));
      };
      const auto option = [&] {
        return Json{
            {"id", newId()},
            {"title", "New option"},
            {"description", ""},
            {"role_ids", Json::array()},
            {"channel_ids", Json::array()}
        };
      };
      if (id == "add-prompt" && prompts.size() < 5)
        prompts.push_back(
            {{"id", newId()},
             {"type", 0},
             {"title", "New question"},
             {"single_select", false},
             {"required", false},
             {"in_onboarding", true},
             {"options", Json::array({option()})}}
        );
      else if (id.starts_with("remove-prompt:")) {
        const auto i = std::stoul(id.substr(14));
        if (i < prompts.size())
          prompts.erase(prompts.begin() + i);
      } else if (id.starts_with("add-option:")) {
        const auto i = std::stoul(id.substr(11));
        if (i < prompts.size() && prompts[i]["options"].size() < 50)
          prompts[i]["options"].push_back(option());
      } else if (id.starts_with("remove-option:")) {
        const auto separator = id.find(':', 14);
        if (separator != std::string::npos) {
          const auto i = std::stoul(id.substr(14, separator - 14)), j = std::stoul(id.substr(separator + 1));
          if (i < prompts.size() && j < prompts[i]["options"].size())
            prompts[i]["options"].erase(prompts[i]["options"].begin() + j);
        }
      }
      setServerSettingsValue("prompts", prompts);
    } else if (id == "save") {
      if (state.saving || state.mfaRequest || state.imageCrop || !state.dirty())
        return true;
      if (page->section == Section::Emoji && !state.creating && !state.expressionDrafts.empty()) {
        state.expressionSaving = true;
        const auto first = state.expressionDrafts.begin();
        api::GuildSettingsRequest request;
        request.section = Section::Emoji;
        request.verb = Verb::Update;
        request.resourceId = first.key();
        request.values = first.value();
        if (!requestServerSettings(request))
          state.expressionSaving = false;
        return true;
      }
      if (page->section == Section::Emoji && !state.creating && !state.emojiUploads.empty()) {
        const auto& file = state.emojiUploads.front();
        api::GuildSettingsRequest request;
        request.section = Section::Emoji;
        request.verb = Verb::Create;
        request.values = {{"name", file["name"]}, {"image", file["image"]}, {"roles", Json::array()}};
        state.emojiUploading = string(file, "id");
        state.emojiUploadSaving = true;
        if (!requestServerSettings(request))
          state.emojiUploadSaving = false;
        return true;
      }
      api::GuildSettingsRequest request;
      request.section = page->section;
      request.verb = state.creating ? Verb::Create : Verb::Update;
      request.resourceId = state.resource;
      request.values = state.draft;
      request.reason = state.reason;
      if (request.section == Section::Events) {
        if (request.values.contains("scheduled_start_time")) {
          auto recurrence = request.values.value("recurrence_rule", state.record.value("recurrence_rule", Json()));
          if (recurrence.is_object()) {
            for (const auto key : {"count", "end", "by_year_day"})
              recurrence.erase(key);
            recurrence["start"] = request.values["scheduled_start_time"];
            request.values["recurrence_rule"] = recurrence;
          }
        }
        const auto type = request.values.value("entity_type", state.record.value("entity_type", 3));
        if (type == 3)
          request.values["channel_id"] = nullptr;
        else
          request.values["entity_metadata"] = nullptr;
      }
      if (state.creating && request.section == Section::Webhooks) {
        request.resourceId = string(request.values, "channel_id");
        request.values.erase("channel_id");
      }
      if (page->section == Section::Overview)
        request.resourceId.clear();
      if (state.page == "ownership") {
        state.destructiveAction = "transfer";
        m_focusedInput = "server-settings:confirmation";
        m_scrolling.stop(state.scroll);
        state.scroll = 0;
        state.confirmDelete = true;
        state.confirmation.clear();
      } else {
        if (request.section == Section::Overview && request.values.contains("mfa_level") && request.values.size() > 1) {
          auto remainder = request;
          remainder.values = Json{{"mfa_level", request.values["mfa_level"]}};
          state.saveRemainder = remainder;
          request.values.erase("mfa_level");
        }
        requestServerSettings(std::move(request));
      }
    } else if (id == "delete" || id == "kick" || id == "ban" || id == "prune" || id == "delete-server") {
      m_scrolling.stop(state.scroll);
      state.scroll = 0;
      state.confirmDelete = true;
      state.destructiveAction = id;
      if (id == "delete-server")
        m_focusedInput = "server-settings:confirmation";
      state.confirmation.clear();
      state.error.clear();
    } else if (id == "confirm-destructive") {
      api::GuildSettingsRequest request;
      request.section = page->section;
      request.verb = Verb::Delete;
      request.resourceId = state.resource;
      request.reason = state.reason;
      if (state.destructiveAction == "delete-server") {
        if (state.confirmation != string(state.overview, "name")) {
          state.error = "Enter the exact server name to confirm deletion.";
          m_window.invalidate();
          return true;
        }
        request.section = Section::DeleteGuild;
        request.resourceId.clear();
      } else if (state.destructiveAction == "transfer") {
        if (state.confirmation != string(state.overview, "name")) {
          state.error = "Enter the exact server name to confirm ownership transfer.";
          m_window.invalidate();
          return true;
        }
        request.section = Section::Overview;
        request.verb = Verb::Update;
        request.resourceId.clear();
        request.values = state.draft;
      } else if (state.destructiveAction == "kick")
        request.section = Section::Members;
      else if (state.destructiveAction == "ban") {
        request.section = Section::Bans;
        request.verb = Verb::Create;
        request.values = {{"delete_message_seconds", 0}};
      } else if (state.destructiveAction == "prune") {
        request.section = Section::Prune;
        request.verb = Verb::Create;
        request.resourceId.clear();
        request.values = {{"days", 7}, {"compute_prune_count", true}};
      }
      requestServerSettings(std::move(request));
    } else if (id.starts_with("role-up:") || id.starts_with("role-down:")) {
      const auto key = id.substr(id.find(':') + 1);
      const auto& list = state.lists[state.page];
      if (list.is_array()) {
        auto ordered = list;
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
          return a.value("position", 0) < b.value("position", 0);
        });
        const auto found = std::ranges::find_if(ordered, [&](const auto& role) { return recordId(role) == key; });
        if (found != ordered.end()) {
          const auto index = static_cast<std::ptrdiff_t>(std::distance(ordered.begin(), found));
          const auto other = index + (id.starts_with("role-up:") ? 1 : -1);
          if (other >= 0
              && other < static_cast<std::ptrdiff_t>(ordered.size())
              && string(ordered[other], "id") != state.guild) {
            const auto access = serverSettingsAccess(key);
            const auto otherPosition = ordered[other].value("position", 0);
            if (access.owner != access.self && otherPosition >= access.selfPosition)
              state.error = "You can only reorder roles below your highest role.";
            else {
              api::GuildSettingsRequest request;
              request.section = Section::Roles;
              request.verb = Verb::Reorder;
              request.resourceId = key;
              request.values = Json::array({{{"id", key}, {"position", otherPosition}}});
              requestServerSettings(request);
            }
          }
        }
      }
    }
    m_window.invalidate();
    return true;
  }

  void ClientApplication::serverSettingsUpload(
      const std::string& field, const std::string& mime, std::vector<std::uint8_t> bytes
  ) {
    auto& state = m_serverSettings;
    if (!state.open || state.saving || bytes.empty())
      return;
    if (state.page == "stickers" && field == "file") {
      if (mime == "application/json" && !hasFeature(state, "VERIFIED") && !hasFeature(state, "PARTNERED")) {
        state.error = "Lottie stickers are available to verified and partnered servers.";
        m_window.invalidate();
        return;
      }
      if ((mime != "image/png" && mime != "image/gif" && mime != "application/json") || bytes.size() > 512U * 1024) {
        state.error = "Choose a PNG, GIF or Lottie sticker no larger than 512 KiB.";
        m_window.invalidate();
        return;
      }
      setServerSettingsValue("file", Json::binary(std::move(bytes)));
      setServerSettingsValue(
          "filename",
          mime == "image/png"       ? "sticker.png"
              : mime == "image/gif" ? "sticker.gif"
                                    : "sticker.json"
      );
      setServerSettingsValue("content_type", mime);
      return;
    }
    const bool image = mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" || mime == "image/webp";
    const bool audio = mime == "audio/ogg" || mime == "audio/mpeg";
    const bool sound = field == "sound";
    const auto limit = sound                             ? 512U * 1024
        : state.page == "emoji" || state.page == "roles" ? 256U * 1024
        : state.page == "stickers"                       ? 512U * 1024
                                                         : 10U * 1024 * 1024;
    if ((!sound && !image) || (sound && !audio) || bytes.size() > limit) {
      state.error = sound                                  ? "Choose an Ogg or MP3 sound no larger than 512 KiB."
          : state.page == "emoji" || state.page == "roles" ? "Choose an image no larger than 256 KiB."
          : state.page == "stickers"                       ? "Choose a PNG or GIF sticker no larger than 512 KiB."
                                     : "Choose a PNG, JPEG, GIF or WebP image no larger than 10 MiB.";
      m_window.invalidate();
      return;
    }
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
      const unsigned a = bytes[i], b = i + 1 < bytes.size() ? bytes[i + 1] : 0,
                     c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
      encoded += alphabet[a >> 2];
      encoded += alphabet[((a & 3) << 4) | (b >> 4)];
      encoded += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
      encoded += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    if (field.starts_with("guide-action:") || field.starts_with("guide-resource:")) {
      state.guideIconDrafts[field] = {{"icon", "data:" + mime + ";base64," + encoded}};
      state.error.clear();
      state.status.clear();
      m_window.invalidate();
      return;
    }
    setServerSettingsValue(field, "data:" + mime + ";base64," + encoded);
  }

  void ClientApplication::drawServerSettings(Node& root, float w, float h) {
    auto& state = m_serverSettings;
    if (!state.open)
      return;
    const auto selected = pageFor(state.page);
    if (!selected)
      return;
    std::erase_if(m_hits, [](const auto& hit) {
      return hit.action != Action::Close && hit.action != Action::Minimize && hit.action != Action::Maximize;
    });
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, background);
    const auto frame = layout::settingsFrame(w);
    const float frameX = frame.x, navWidth = frame.navigationWidth;
    const float contentLeft = frame.contentLeft, contentWidth = frame.contentWidth;
    const float contentX = frame.contentX, width = frame.innerWidth;
    state.navRight = frameX + navWidth + Style::spaceXs;
    rectangle(root, 0, 36, state.navRight, h - layout::Window::titlebarHeight, sidebar);
    const auto retained = [&](const std::string& id) {
      std::unique_ptr<Node> saved;
      if (auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
        saved = std::move(at->second);
        m_detachedInputs.erase(at);
      }
      return saved;
    };
    const auto button = [&](Node& parent, float x, float y, float bw, const std::string& caption,
                            const std::string& action, ButtonVariant variant = ButtonVariant::Default,
                            bool enabled = true) {
      if (state.page == "onboarding"
          && (action == "save"
              || action == "add-prompt"
              || action.starts_with("remove-prompt:")
              || action.starts_with("add-option:")
              || action.starts_with("remove-option:")
              || action.starts_with("prompt-move:")
              || action.starts_with("option-move:"))) {
        api::GuildSettingsRequest request;
        request.guildId = state.guild;
        request.section = Section::Onboarding;
        request.verb = Verb::Update;
        enabled = enabled && api::canManageGuildSettings(request, serverSettingsAccess());
      }
      const auto controlId = "server-button:" + action;
      auto saved = retained(controlId);
      auto control =
          saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
      m_widgets[controlId] = control.get();
      control->setText(caption);
      control->setFontSize(settings::control.size);
      control->setVariant(variant);
      control->setPosition(x, y);
      control->setControlHeight(layout::Settings::actionHeight);
      control->setRadius(Style::scaledRadius(layout::Settings::actionRadius));
      control->setPadding(0, Style::spaceDense);
      control->setFrameSize(bw, layout::Settings::actionHeight);
      control->setEnabled(
          enabled && !state.saving && (!state.mfaRequest || action == "mfa-verify" || action == "mfa-cancel")
      );
      control->setOnClick([this, action] { serverSettingsAction({0, 0, 0, 0, Action::ServerSettingsAction, action}); });
      parent.addChild(std::move(control));
    };
    const auto registerPicker = [&](SettingsColorPicker& picker, const std::string& key) {
      const auto bind = [&](Input* input, const std::string& part) {
        if (!input)
          return;
        const auto id = "server-settings:picker:" + state.page + ":" + key + ":" + part;
        m_inputs[id] = input;
        input->setFontSize(Style::fontSizeDescription);
        input->setOnFocusGain([this, id] { m_focusedInput = id; });
        input->setOnKeyEvent([this, id](std::uint32_t key, std::uint32_t modifiers) {
          if (((modifiers & KeyMod::Ctrl) && (key == XKB_KEY_v || key == XKB_KEY_V))
              || ((modifiers & KeyMod::Shift) && key == XKB_KEY_Insert)) {
            m_pasteInput = id;
            m_pasteServerScope = serverSettingsScope();
            m_pasteDialogGeneration = m_dialogGeneration;
            m_pasteMessageEditGeneration = 0;
            m_pasteConnected = m_connected;
            m_pasteSession = m_session;
            m_pasteChannel = m_channelId;
            m_window.paste(false);
            return true;
          }
          return false;
        });
      };
      bind(picker.hexInput(), "hex");
      const auto rgb = picker.rgbInputs();
      for (std::size_t i = 0; i < rgb.size(); ++i)
        bind(rgb[i], std::to_string(i));
    };
    const auto textHeight = [&](const std::string& text, float size, float available,
                                FontWeight weight = FontWeight::Normal) {
      const auto metrics = m_window.target().renderer().measureText(text, size, weight, std::max(1.0F, available), 0);
      return std::max(size + 4, metrics.bottom - metrics.top);
    };
    const auto guildAt = std::ranges::find(m_guilds, state.guild, &api::Guild::id);
    const auto guildName =
        string(state.overview, "name", guildAt == m_guilds.end() ? "Server settings" : guildAt->name);
    label(
        root, frameX + Style::spaceMd, 56, navWidth - Style::spaceXl, guildName, settings::navigationTitle.size,
        foreground, settings::navigationTitle.weight
    );
    auto& search = inputControl(
        root, frameX + Style::spaceXs, 82, navWidth - Style::spaceSm, 32, "server-settings:nav-search", state.filter,
        "Search settings"
    );
    search.setFrameRadius(6);
    search.setFontSize(Style::fontSizeDescription);
    search.setHorizontalPadding(10);
    search.setOnChange([this](const std::string& text) {
      m_serverSettings.filter = text;
      m_serverSettings.navScroll = 0;
      m_window.invalidate();
    });
    search.setOnSubmit([](const std::string&) {});
    auto nav = std::make_unique<Node>();
    nav->setPosition(frameX, 124);
    const float navH = std::max(40.0F, h - 154);
    nav->setSize(frame.navigationContentWidth, navH);
    nav->setClipChildren(true);
    const auto access = serverSettingsAccess();
    const std::vector<api::Channel> noChannels;
    const auto targetChannels = m_guildChannels.find(state.guild);
    const auto& guildChannels = state.guild == m_guildId ? m_channels
        : targetChannels != m_guildChannels.end()        ? targetChannels->second
                                                         : noChannels;
    const auto actorName = [&](const std::string& id) {
      if (id == m_account.id)
        return m_account.name;
      const auto at = state.lists.find("audit-users");
      if (at != state.lists.end() && at->second.is_array())
        for (const auto& user : at->second)
          if (string(user, "id") == id)
            return string(user, "global_name", string(user, "username", "Unknown member"));
      return userName(id, "Unknown member");
    };
    const auto targetName = [&](const std::string& id) {
      if (id.empty() || id == state.guild)
        return guildName;
      for (const auto& channel : guildChannels)
        if (channel.id == id)
          return "# " + channel.name;
      for (const auto& role : m_roleDirectory.allRoles(state.guild))
        if (role.id == id)
          return role.name;
      const auto name = actorName(id);
      return name == "Unknown member" ? std::string("Unavailable target") : name;
    };
    const bool owner = access.owner == m_account.id,
               admin = owner || (access.permissions && (*access.permissions & administrator));
    std::string group;
    float navY = 0;
    for (const auto& page : pages) {
      const bool allowed = page.permission ? admin
              || (access.permissions
                  && (std::string_view(page.id) == "prune" || std::string_view(page.id) == "commands"
                          ? (*access.permissions & page.permission) == page.permission
                          : (*access.permissions & page.permission) != 0))
                                           : owner;
      if (!allowed
          || !hasFeature(state, page.feature)
          || (!state.filter.empty() && !lower(std::string(page.name) + " " + page.group).contains(lower(state.filter))))
        continue;
      if (group != page.group) {
        group = page.group;
        label(
            *nav, 12, navY - state.navScroll + Style::spaceSm, frame.navigationContentWidth - Style::spaceXl,
            page.group, settings::navigationHeading.size,
            std::string_view(page.group) == "DANGER ZONE" ? danger : muted, FontWeight::SemiBold
        );
        navY += (Style::spaceXl + Style::spaceXxs);
      }
      const auto controlId = "server-nav:" + std::string(page.id);
      auto saved = retained(controlId);
      auto control =
          saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
      m_widgets[controlId] = control.get();
      control->setText(page.name);
      control->setFontSize(settings::control.size);
      control->setVariant(state.page == page.id ? ButtonVariant::TabActive : ButtonVariant::Tab);
      control->setContentAlign(ButtonContentAlign::Start);
      control->setPosition(4, navY - state.navScroll);
      control->setControlHeight(layout::Settings::actionHeight);
      control->setRadius(Style::scaledRadius(layout::Settings::actionRadius));
      control->setPadding(0, 8);
      control->setFrameSize(frame.navigationContentWidth - Style::spaceSm, layout::Settings::actionHeight);
      control->setEnabled(!state.saving);
      control->setOnClick([this, id = std::string(page.id)] { selectServerSettingsPage(id); });
      nav->addChild(std::move(control));
      navY += (Style::space2Xl - Style::spaceXxs);
    }
    const auto previousNav = state.navScroll;
    m_scrolling.limit(state.navScroll, std::max(0.0F, navY - navH));
    nav->setTextScrollOffset(-state.navScroll);
    if (previousNav != state.navScroll)
      for (const auto& child : nav->children())
        child->setPosition(child->x(), child->y() + previousNav - state.navScroll);
    root.addChild(std::move(nav));
    settingsScrollbar(
        root, "server-settings-nav-scrollbar",
        {frame.navigationScrollbarX, layout::Settings::navigationTop, layout::Settings::scrollbarWidth, navH}, navY,
        state.navScroll
    );
    iconButton(root, {w - 56, 52, 30, 30, Action::CloseServerSettings, {}}, "x", "Close server settings (Esc)");
    label(
        root, contentLeft, 63, contentWidth, selected->name, settings::pageTitle.size, foreground,
        settings::pageTitle.weight
    );
    const bool dirty = state.dirty();
    const bool retryRead = !state.error.empty() && !state.dirty() && !state.saving && !state.mfaRequest;
    const auto feedback = !state.error.empty() && !state.mfaRequest ? state.error : state.status;
    const auto feedbackTextWidth = std::max(40.0F, width - 24 - (retryRead && width >= 420 ? 92 : 0));
    const float feedbackHeight = feedback.empty() ? 0
                                                  : textHeight(feedback, Style::fontSizeDescription, feedbackTextWidth)
            + 20
            + (retryRead && width < layout::Settings::stackThreshold ? 34 : 0);
    const float saveHeight = dirty ? (width < layout::Settings::stackThreshold ? layout::Settings::stackedSaveHeight
                                                                               : layout::Settings::saveHeight)
                                   : 0;
    const float viewportY = layout::Settings::viewportTop,
                viewportH = std::max(
                    40.0F,
                    h
                        - viewportY
                        - Style::spaceXl
                        - (saveHeight ? saveHeight + Style::spaceMd : 0)
                        - (feedbackHeight ? feedbackHeight + Style::spaceSm : 0)
                );
    auto pageNode = std::make_unique<Node>();
    pageNode->setPosition(contentLeft, viewportY);
    pageNode->setSize(contentWidth, viewportH);
    pageNode->setClipChildren(true);
    auto body = std::make_unique<Node>();
    auto* bodyNode = body.get();
    body->setPosition(layout::Settings::contentInset, -state.scroll);
    pageNode->addChild(std::move(body));
    auto& p = *bodyNode;
    const auto bodyHitsBegin = m_hits.size();
    const auto bodyScrollBefore = state.scroll;
    const auto tooltipBefore = m_tooltip;
    const auto tooltipXBefore = m_tooltipX, tooltipYBefore = m_tooltipY;
    float cursor = 0, categoryTop = 0;
    RectNode* category = nullptr;
    const auto finishCategory = [&] {
      if (category) {
        cursor += Style::spaceSm;
        category->setSize(contentWidth, cursor - categoryTop);
        category = nullptr;
      }
    };
    const auto section = [&](const std::string& caption) {
      finishCategory();
      if (cursor)
        cursor += Style::spaceLg;
      categoryTop = cursor;
      auto card = std::make_unique<RectNode>();
      card->setPosition(-layout::Settings::contentInset, cursor);
      card->setStyle({.fill = sidebar, .radius = Style::scaledRadius(Style::surfaceRadius)});
      card->setZIndex(-1);
      category = card.get();
      p.addChild(std::move(card));
      cursor += Style::spaceMd;
      if (!caption.empty()) {
        cursor += label(p, 0, cursor, width, caption, settings::section.size, muted, settings::section.weight, 0);
        cursor += Style::spaceSm;
      }
    };
    const auto ensure = [&] {
      if (!category)
        section("");
    };
    const auto value = [&](const std::string& key, Json fallback = Json()) {
      if (state.draft.contains(key))
        return state.draft.at(key);
      if (state.record.contains(key))
        return state.record.at(key);
      return fallback;
    };
    const auto detail = [&](const std::string& title, const std::string& description) {
      ensure();
      cursor += Style::spaceSm;
      cursor += label(p, 0, cursor, width, title, settings::rowTitle.size, foreground, settings::rowTitle.weight, 0);
      if (!description.empty()) {
        cursor += Style::spaceXs;
        cursor +=
            label(p, 0, cursor, width, description, settings::description.size, muted, settings::description.weight, 0);
      }
      cursor += Style::spaceDense;
    };
    const auto row = [&](const std::string& title, const std::string& description, float trailing = 0) {
      ensure();
      const auto top = cursor;
      const auto available = std::max(48.0F, width - trailing);
      const auto th = textHeight(title, settings::rowTitle.size, available, settings::rowTitle.weight);
      const auto dh = description.empty() ? 0 : textHeight(description, Style::fontSizeDescription, available) + 4;
      const auto height = std::max(46.0F, th + dh + 20);
      label(
          p, 0, top + (height - th - dh) / 2, available, title, settings::rowTitle.size, foreground,
          settings::rowTitle.weight, 0
      );
      if (!description.empty())
        label(
            p, 0, top + (height - th - dh) / 2 + th + Style::spaceXs, available, description,
            settings::description.size, muted, settings::description.weight, 0
        );
      cursor += height;
      return std::pair{top, height};
    };
    const auto fieldEditable = [&](const std::string& key) {
      if (state.page == "members" && !state.resource.empty()) {
        api::GuildSettingsRequest request;
        request.guildId = state.guild;
        request.section = Section::Members;
        request.verb = Verb::Update;
        request.resourceId = state.resource;
        request.values = {{key, nullptr}};
        return api::canManageGuildSettings(request, serverSettingsAccess(state.resource));
      }
      if (state.page == "roles" && !state.resource.empty()) {
        api::GuildSettingsRequest request;
        request.guildId = state.guild;
        request.section = Section::Roles;
        request.verb = Verb::Update;
        request.resourceId = state.resource;
        return api::canManageGuildSettings(request, serverSettingsAccess(state.resource));
      }
      if (state.page == "onboarding"
          || ((state.page == "emoji"
               || state.page == "stickers"
               || state.page == "soundboard"
               || state.page == "events")
              && !state.creating)) {
        api::GuildSettingsRequest request;
        request.guildId = state.guild;
        request.section = selected->section;
        request.verb = Verb::Update;
        request.resourceId = state.resource;
        return api::canManageGuildSettings(request, serverSettingsAccess(state.resource));
      }
      return true;
    };
    const auto field = [&](const std::string& title, const std::string& key, const std::string& description,
                           float height = 32.0F, int kind = 0) {
      detail(title, description);
      const auto current = value(key);
      const auto text = current.is_string() ? current.get<std::string>()
          : current.is_number()             ? current.dump()
                                            : std::string{};
      auto& input =
          inputControl(p, 0, cursor, width, height, "server-settings:" + state.page + ":" + key, text, {}, height > 40);
      input.setFontSize(settings::control.size);
      input.setFontWeight(settings::control.weight);
      input.setHorizontalPadding(10);
      input.setFrameRadius(5);
      input.setEnabled(!state.saving && !state.mfaRequest && fieldEditable(key));
      input.setOnChange([this, key, kind](const std::string& text) {
        if (kind == 1) {
          int number = 0;
          const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
          if (result.ec == std::errc{} && result.ptr == text.data() + text.size())
            setServerSettingsValue(key, number);
          else {
            m_serverSettings.error = "Enter a whole number.";
            m_window.invalidate();
          }
        } else if (kind == 2)
          setServerSettingsValue(key, text.empty() ? Json() : Json(text));
        else
          setServerSettingsValue(key, text);
      });
      input.setOnSubmit([](const std::string&) {});
      cursor += height + 12;
    };
    const auto toggleCallback = [&](const std::string& key, const std::string& title, const std::string& description,
                                    bool checked, bool enabled, std::function<void(bool)> callback) {
      const auto [top, height] = row(title, description, 50);
      const auto controlId = "server-toggle:" + state.page + ":" + key;
      auto saved = retained(controlId);
      auto control =
          saved ? std::unique_ptr<Toggle>(static_cast<Toggle*>(saved.release())) : std::make_unique<Toggle>();
      m_widgets[controlId] = control.get();
      control->setToggleSize(ToggleSize::Small);
      control->setCheckedImmediate(checked);
      control->setEnabled(enabled && !state.saving && (!state.mfaRequest || key == "mfa-method") && fieldEditable(key));
      control->setPosition(width - 30, top + (height - 18) / 2);
      control->setFrameSize(30, 18);
      control->setOnChange(std::move(callback));
      p.addChild(std::move(control));
    };
    const auto toggle = [&](const std::string& title, const std::string& key, const std::string& description) {
      const auto current = value(key, false);
      toggleCallback(
          key, title, description, current.is_boolean() && current.get<bool>(), true,
          [this, key](bool checked) { setServerSettingsValue(key, checked); }
      );
    };
    const auto chooseCallback = [&](const std::string& key, const std::string& title, const std::string& description,
                                    std::vector<std::string> names, std::size_t index,
                                    std::function<void(std::size_t)> callback, bool enabled = true) {
      detail(title, description);
      const auto controlId = "server-select:" + state.page + ":" + key;
      auto saved = retained(controlId);
      auto control =
          saved ? std::unique_ptr<Select>(static_cast<Select*>(saved.release())) : std::make_unique<Select>();
      m_widgets[controlId] = control.get();
      control->setOptions(std::move(names));
      control->setFontSize(settings::control.size);
      control->setControlHeight(30);
      control->setHorizontalPadding(10);
      control->setPosition(0, cursor);
      control->setFrameSize(width, 30);
      control->setSelectedIndexSilently(index);
      control->setEnabled(enabled && !state.saving && (!state.mfaRequest || key == "mfa-method") && fieldEditable(key));
      control->setOnSelectionChanged([callback = std::move(callback)](std::size_t index, std::string_view) {
        callback(index);
      });
      p.addChild(std::move(control));
      cursor += (Style::spaceSection + Style::spaceXxs);
    };
    const auto choose = [&](const std::string& title, const std::string& key, const std::string& description,
                            const std::vector<std::pair<std::string, Json>>& options) {
      std::vector<std::string> names;
      std::size_t index = 0;
      const auto current = value(key);
      for (std::size_t i = 0; i < options.size(); ++i) {
        names.push_back(options[i].first);
        if (current == options[i].second)
          index = i;
      }
      chooseCallback(key, title, description, std::move(names), index, [this, key, options](std::size_t at) {
        if (at < options.size())
          setServerSettingsValue(key, options[at].second);
      });
    };
    const auto channel = [&](const std::string& title, const std::string& key, const std::string& description,
                             int type = -1) {
      std::vector<std::pair<std::string, Json>> options = {{"No channel", nullptr}};
      const auto cache = m_guildChannels.find(state.guild);
      const auto& channels = state.guild == m_guildId ? m_channels
          : cache != m_guildChannels.end()            ? cache->second
                                                      : std::vector<api::Channel>{};
      for (const auto& c : channels)
        if (c.type != 4 && !c.isThread() && (type < 0 ? true : type == 2 ? c.type == 2 : c.type == 0 || c.type == 5))
          options.emplace_back((c.type == 2 ? "Voice: " : "# ") + c.name, c.id);
      choose(title, key, description, options);
    };
    const auto upload = [&](const std::string& title, const std::string& key, const std::string& description) {
      auto current = value(key);
      if (current.is_null() && state.page == "profile" && !state.draft.contains(key))
        current = value(key == "icon" ? "icon_hash" : key == "custom_banner" ? "custom_banner_hash" : key);
      const bool exists = !current.is_null() && (!current.is_string() || !current.get<std::string>().empty());
      detail(title, description);
      button(p, 0, cursor, 112, exists ? "Replace file" : "Choose file", "upload:" + key);
      if (exists)
        button(p, 122, cursor, 80, "Remove", "clear-upload:" + key);
      cursor += (Style::spaceSection - Style::spaceXxs);
    };
    const auto lineList = [&](const std::string& title, const std::string& key, const std::string& description) {
      auto items = value(key, Json::array());
      std::string text;
      if (items.is_array())
        for (const auto& item : items)
          if (item.is_string()) {
            if (!text.empty())
              text += '\n';
            text += item.get<std::string>();
          }
      detail(title, description);
      auto& input = inputControl(p, 0, cursor, width, 80, "server-settings:" + state.page + ":" + key, text, {}, true);
      input.setOnChange([this, key](const std::string& text) {
        Json values = Json::array();
        std::size_t start = 0;
        while (start < text.size()) {
          const auto end = text.find('\n', start);
          const auto part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
          if (!part.empty())
            values.push_back(part);
          if (end == std::string::npos)
            break;
          start = end + 1;
        }
        setServerSettingsValue(key, values);
      });
      input.setOnSubmit([](const std::string&) {});
      cursor += 92;
    };
    const auto dateTimeField = [&](const std::string& title, const std::string& key, const std::string& description,
                                   bool optional) {
      const auto current = value(key);
      const auto source = current.is_string() ? current.get<std::string>() : std::string{};
      auto& fields = state.lists["event-local-fields"];
      if (!fields.is_object())
        fields = Json::object();
      if (!fields.contains(key) || fields[key].value("source", "") != source) {
        std::string date, time;
        if (const auto parsed = api::muteDeadline(source)) {
          const auto epoch = std::chrono::system_clock::to_time_t(*parsed);
          std::tm local{};
          localtime_r(&epoch, &local);
          char d[16], t[8];
          std::strftime(d, sizeof(d), "%Y-%m-%d", &local);
          std::strftime(t, sizeof(t), "%H:%M", &local);
          date = d;
          time = t;
        }
        fields[key] = {{"source", source}, {"date", date}, {"time", time}};
      }
      detail(title, description + " Times use your computer's local time zone.");
      const auto gap = 12.0F, dateWidth = std::max(84.0F, (width - gap) * .6F),
                 timeWidth = std::max(50.0F, width - gap - dateWidth);
      const auto update = [this, key, optional](const std::string& part, const std::string& text) {
        auto& entry = m_serverSettings.lists["event-local-fields"][key];
        entry[part] = text;
        const auto date = entry.value("date", ""), clock = entry.value("time", "");
        if (optional && date.empty() && clock.empty()) {
          entry["source"] = "";
          setServerSettingsValue(key, nullptr);
          return;
        }
        std::tm local{};
        std::istringstream stream(date + " " + clock);
        stream >> std::get_time(&local, "%Y-%m-%d %H:%M");
        std::string serialized = date + " " + clock;
        if (!stream.fail() && date.size() == 10 && clock.size() == 5) {
          local.tm_isdst = -1;
          const auto expected = local;
          const auto epoch = std::mktime(&local);
          if (epoch != -1
              && local.tm_year == expected.tm_year
              && local.tm_mon == expected.tm_mon
              && local.tm_mday == expected.tm_mday
              && local.tm_hour == expected.tm_hour
              && local.tm_min == expected.tm_min) {
            std::tm utc{};
            gmtime_r(&epoch, &utc);
            char value[32];
            std::strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%SZ", &utc);
            serialized = value;
          }
        }
        entry["source"] = serialized;
        setServerSettingsValue(key, serialized);
      };
      auto& date = inputControl(
          p, 0, cursor, dateWidth, 32, "server-settings:event-date:" + key, fields[key].value("date", ""), "YYYY-MM-DD"
      );
      date.setEnabled(!state.saving && !state.mfaRequest);
      date.setOnChange([update](const std::string& text) { update("date", text); });
      date.setOnSubmit([](const std::string&) {});
      auto& time = inputControl(
          p, dateWidth + gap, cursor, timeWidth, 32, "server-settings:event-time:" + key, fields[key].value("time", ""),
          "HH:MM"
      );
      time.setEnabled(!state.saving && !state.mfaRequest);
      time.setOnChange([update](const std::string& text) { update("time", text); });
      time.setOnSubmit([](const std::string&) {});
      cursor += (Style::spaceSection + Style::spaceXs);
    };
    const auto colorField = [&](const std::string& title, const std::string& key, const std::string& description) {
      const auto colorValue = value(key, "#5865F2");
      auto hexValue = colorValue.is_string() ? colorValue.get<std::string>() : "#5865F2";
      if (!hexValue.starts_with('#'))
        hexValue = '#' + hexValue;
      Color current = accent;
      try {
        current = hex(hexValue);
      } catch (...) {
      }
      detail(title, description);
      rectangle(p, 0, cursor, 26, 26, current, Style::controlRadius);
      button(p, 36, cursor, 112, hexValue, "profile-color:" + key);
      cursor += (Style::spaceSection - Style::spaceXxs);
      if (state.profileColorPicker == key) {
        const auto controlId = "server-profile-color:" + key;
        auto saved = retained(controlId);
        auto picker = saved ? std::unique_ptr<SettingsColorPicker>(static_cast<SettingsColorPicker*>(saved.release()))
                            : std::make_unique<SettingsColorPicker>();
        m_widgets[controlId] = picker.get();
        const auto pickerWidth = std::min(280.0F, width);
        picker->setPickerWidth(pickerWidth);
        picker->setPosition(0, cursor);
        picker->setColor(current);
        picker->setEnabled(!state.saving && !state.mfaRequest);
        registerPicker(*picker, key);
        picker->setOnColorChanged([this, key](const Color& color) {
          const auto channel = [](float v) {
            return static_cast<unsigned>(std::round(std::clamp(v, 0.0F, 1.0F) * 255));
          };
          const auto packed = (channel(color.r) << 16) | (channel(color.g) << 8) | channel(color.b);
          std::ostringstream text;
          text << '#' << std::hex << std::setw(6) << std::setfill('0') << packed;
          setServerSettingsValue(key, text.str());
        });
        p.addChild(std::move(picker));
        cursor += SettingsColorPicker::intrinsicColumnHeight(pickerWidth, 1) + 12;
      }
    };
    const auto channelsAndRoles = [&](const std::string& title, const std::string& key, bool roles) {
      detail(
          title, roles ? "Choose roles that this setting applies to." : "Choose the channels included in this setting."
      );
      auto selectedIds = value(key, Json::array());
      if (!selectedIds.is_array())
        selectedIds = Json::array();
      if (roles) {
        for (const auto& role : m_roleDirectory.allRoles(state.guild)) {
          if (role.id == state.guild)
            continue;
          const bool checked = std::ranges::find(selectedIds, Json(role.id)) != selectedIds.end();
          toggleCallback(
              key + role.id, role.name, "", checked, true,
              [this, key, selectedIds, id = role.id](bool enabled) mutable {
                if (enabled)
                  selectedIds.push_back(id);
                else
                  eraseJson(selectedIds, [&](const auto& v) { return v == id; });
                setServerSettingsValue(key, selectedIds);
              }
          );
        }
      } else {
        for (const auto& c : guildChannels) {
          if (c.type == 4 || c.isThread())
            continue;
          const bool checked = std::ranges::find(selectedIds, Json(c.id)) != selectedIds.end();
          toggleCallback(
              key + c.id, "# " + c.name, "", checked, true, [this, key, selectedIds, id = c.id](bool enabled) mutable {
                if (enabled)
                  selectedIds.push_back(id);
                else
                  eraseJson(selectedIds, [&](const auto& v) { return v == id; });
                setServerSettingsValue(key, selectedIds);
              }
          );
        }
      }
    };
    if (state.confirmDiscard) {
      section("Unsaved changes");
      detail("Keep editing or discard your changes", "Your draft has not been sent to Discord.");
      button(p, 0, cursor, 116, "Keep editing", "keep-editing");
      button(p, 128, cursor, 112, "Discard", "discard", ButtonVariant::Destructive);
      cursor += (Style::spaceSection - Style::spaceXxs);
    }
    if (state.confirmDelete) {
      section(state.destructiveAction == "transfer" ? "Transfer ownership" : "Confirm action");
      const auto name = recordName(state.record);
      const auto description = state.destructiveAction == "delete-server"
          ? "Deleting this server permanently removes its channels, messages and membership. Enter the server name "
            "below to confirm."
          : state.destructiveAction == "transfer"
          ? "The new owner receives full control of this server. Enter the server name below to confirm."
          : state.destructiveAction == "kick"
          ? "Remove this member from the server. They can join again with a valid invite."
          : state.destructiveAction == "ban" ? "Ban this member from the server. Existing messages are retained."
          : state.destructiveAction == "prune"
          ? "Remove members who have been inactive for seven days and have no roles."
          : selected->section == Section::Bans ? "Lift this ban so the member can join again with a valid invite."
          : selected->section == Section::Invites
          ? "Revoke this invite link. New members will no longer be able to use it."
          : "Permanently remove " + name + " from this server.";
      detail(
          state.destructiveAction == "delete-server"  ? "Delete " + guildName + "?"
              : state.destructiveAction == "transfer" ? "Transfer " + guildName + "?"
                                                      : "Are you sure?",
          description
      );
      if (state.destructiveAction == "delete-server" || state.destructiveAction == "transfer") {
        auto& input =
            inputControl(p, 0, cursor, width, 32, "server-settings:confirmation", state.confirmation, guildName);
        input.setOnChange([this](const std::string& text) {
          m_serverSettings.confirmation = text;
          m_window.invalidate();
        });
        input.setOnSubmit([](const std::string&) {});
        cursor += (Style::spaceSection + Style::spaceXs);
      }
      button(p, 0, cursor, 90, "Cancel", "keep-editing");
      button(
          p, 102, cursor, std::min(180.0F, width - 102),
          state.destructiveAction == "transfer" ? "Transfer ownership" : "Confirm", "confirm-destructive",
          ButtonVariant::Destructive
      );
      cursor += (Style::spaceSection - Style::spaceXxs);
    }
    if (state.mfaRequest) {
      section("Verify your identity");
      detail("Confirm this server change", "Complete Discord's verification before this change can be applied.");
      std::vector<std::string> names;
      std::size_t index = 0;
      for (std::size_t i = 0; i < state.mfaMethods.size(); ++i) {
        const auto& method = state.mfaMethods[i];
        names.push_back(method == "totp" ? "Authenticator app" : method == "sms" ? "Text message" : "Account password");
        if (method == state.mfaType)
          index = i;
      }
      chooseCallback(
          "mfa-method", "Verification method", "Choose a method available on your account.", names, index,
          [this](std::size_t at) {
            if (at < m_serverSettings.mfaMethods.size()) {
              m_serverSettings.mfaType = m_serverSettings.mfaMethods[at];
              m_serverSettings.mfaCode.clear();
              m_window.invalidate();
            }
          }
      );
      detail(
          state.mfaType == "password" ? "Password" : "Verification code",
          state.mfaType == "totp"      ? "Enter the current code from your authenticator app."
              : state.mfaType == "sms" ? "Enter the code sent to your phone."
                                       : "Enter your Discord account password."
      );
      auto& input = inputControl(p, 0, cursor, width, 32, "server-settings:mfa-code", state.mfaCode, {}, false, true);
      input.setOnChange([this](const std::string& text) {
        if (text.size() <= 1024)
          m_serverSettings.mfaCode = text;
        m_window.invalidate();
      });
      input.setOnSubmit([](const std::string&) {});
      cursor += (Style::spaceSection + Style::spaceXs);
      if (!state.error.empty())
        row("Verification failed", state.error);
      button(p, 0, cursor, 90, "Cancel", "mfa-cancel");
      button(p, 102, cursor, 90, "Verify", "mfa-verify", ButtonVariant::Primary, !state.mfaCode.empty());
      cursor += (Style::spaceSection - Style::spaceXxs);
    }
    ServerSettingsForm form{
        p,
        cursor,
        width,
        section,
        detail,
        row,
        field,
        choose,
        chooseCallback,
        toggle,
        toggleCallback,
        channel,
        upload,
        lineList,
        channelsAndRoles,
        dateTimeField,
        colorField,
        button,
        value
    };
    const bool editable = admin
        || (access.permissions
            && (state.page == "prune" || state.page == "commands"
                    ? (*access.permissions & selected->permission) == selected->permission
                    : (*access.permissions & selected->permission) != 0));
    if ((!editable && selected->permission) || (!selected->permission && !owner)) {
      section("Access changed");
      detail(
          "You no longer have access to these settings",
          "Your server permissions changed. Choose an available page from the sidebar."
      );
    } else if (
        !state.loaded.contains(state.page)
        && selected->section != Section::Overview
        && state.page != "delete"
        && state.page != "ownership"
        && state.loading
    ) {
      section("Loading");
      row("Loading server settings", "The current server data is being requested.");
    } else if (selected->section == Section::Overview && state.overview.empty() && state.loading) {
      section("Loading");
      row("Loading server settings", "The current server data is being requested.");
    } else if (drawServerImageCrop(form)) {
    } else if (drawServerCommands(form)) {
    } else if (drawServerExpressions(form)) {
    } else if (drawServerExtraForms(form)) {
    } else if (drawServerManagementForms(form)) {
    } else if (state.page == "profile") {
      drawServerExtraForms(form, "profile-assets");
      section("Identity");
      field("Server name", "name", "The name shown in the server list, invites and server profile.");
      field("Description", "description", "Describe your community in up to 300 characters.", 80, 2);
      upload(
          "Server icon", "icon",
          "Choose a square image for the server list and invites. Animated icons require the server's animated icon "
          "perk."
      );
      if (hasFeature(state, "BANNER"))
        upload("Server banner", "custom_banner", "Choose the wide image displayed in your server profile.");
      colorField("Profile accent", "brand_color_primary", "Set the accent color used in the server profile.");
      choose(
          "Server profile visibility", "visibility", "Choose who can see this server's public profile.",
          {{"Default", 0}, {"Public", 1}, {"Private", 2}, {"Public with recruitment", 3}}
      );
      section("Server traits");
      auto traits = value("traits", Json::array());
      if (traits.is_array())
        for (std::size_t i = 0; i < traits.size(); ++i) {
          detail("Trait " + std::to_string(i + 1), "Describe a characteristic of your community.");
          auto& input = inputControl(
              p, 0, cursor, std::max(40.0F, width - 70), 32, "server-settings:trait-label:" + std::to_string(i),
              string(traits[i], "label"), "Trait"
          );
          input.setOnChange([this, traits, i](const std::string& text) mutable {
            traits[i]["label"] = text;
            setServerSettingsValue("traits", traits);
          });
          input.setOnSubmit([](const std::string&) {});
          auto& emoji = inputControl(
              p, width - 60, cursor, 60, 32, "server-settings:trait-emoji:" + std::to_string(i),
              string(traits[i], "emoji_name"), "Emoji"
          );
          emoji.setOnChange([this, traits, i](const std::string& text) mutable {
            traits[i]["emoji_id"] = nullptr;
            traits[i]["emoji_name"] = text.empty() ? Json() : Json(text);
            traits[i]["emoji_animated"] = false;
            setServerSettingsValue("traits", traits);
          });
          emoji.setOnSubmit([](const std::string&) {});
          cursor += (Style::spaceSection + Style::spaceXs);
          button(p, 0, cursor, 90, "Remove", "remove-trait:" + std::to_string(i));
          cursor += (Style::spaceSection - Style::spaceXxs);
        }
      if (traits.size() < 5) {
        button(p, 0, cursor, 100, "Add trait", "add-trait");
        cursor += (Style::spaceSection - Style::spaceXxs);
      }
      section("Games");
      auto games = value("game_application_ids", Json::array());
      if (games.is_array())
        for (const auto& game : games)
          if (game.is_string()) {
            const auto id = game.get<std::string>();
            const auto at = state.gameNames.find(id);
            const auto item =
                row(at == state.gameNames.end() ? "Selected game" : at->second, at == state.gameNames.end() ? id : "",
                    90);
            button(p, width - 80, item.first + (item.second - 26) / 2, 80, "Remove", "remove-game:" + id);
          }
      detail("Add games", "Search for games played by your community. You can select up to 20 games.");
      auto& gameSearch =
          inputControl(p, 0, cursor, width, 32, "server-settings:game-search", state.gameQuery, "Search games");
      gameSearch.setOnChange([this](const std::string& text) {
        auto& state = m_serverSettings;
        state.gameQuery = text;
        state.gameTimer.stop();
        state.gameResults = Json::array();
        if (text.size() >= 2 && !m_demo) {
          const auto generation = state.generation;
          state.gameTimer.start(std::chrono::milliseconds(200), [this, generation] {
            if (!m_serverSettings.open
                || m_serverSettings.generation != generation
                || m_serverSettings.page != "profile")
              return;
            api::GuildSettingsRequest request;
            request.section = Section::Applications;
            request.query = m_serverSettings.gameQuery;
            request.limit = 25;
            requestServerSettings(request);
          });
        }
        m_window.invalidate();
      });
      gameSearch.setOnSubmit([](const std::string&) {});
      cursor += (Style::spaceSection + Style::spaceXs);
      for (const auto& game : state.gameResults) {
        const auto id = string(game, "id");
        const auto item = row(string(game, "name"), "", 80);
        const bool present = std::ranges::find(games, Json(id)) != games.end();
        button(
            p, width - 70, item.first + (item.second - 26) / 2, 70, present ? "Added" : "Add", "add-game:" + id,
            ButtonVariant::Default, !present && games.size() < 20
        );
      }
      if (hasFeature(state, "GUILD_TAGS") || (!value("tag", "").is_null() && value("tag", "") != "")) {
        section("Server tag");
        field("Tag", "tag", "Choose the 2–4 character tag members can display beside their names.", 32, 2);
        std::vector<std::pair<std::string, Json>> badges = {{"Leaf", 6},     {"Sword", 0},      {"Heart", 7},
                                                            {"Fire", 8},     {"Water drop", 1}, {"Skull", 2},
                                                            {"Moon", 4},     {"Lightning", 5},  {"Compass", 9},
                                                            {"Toadstool", 3}};
        for (const auto& [feature, entries] :
             std::vector<std::pair<std::string, std::vector<std::pair<std::string, Json>>>>{
                 {"GUILD_TAGS_BADGE_PACK_PETS", {{"Bunny", 21}, {"Dog", 22}, {"Frog", 23}, {"Goat", 24}, {"Cat", 25}}},
                 {"GUILD_TAGS_BADGE_PACK_FLEX",
                  {{"Diamond", 26}, {"Crown", 27}, {"Trophy", 28}, {"Money bag", 29}, {"Dollar sign", 30}}},
                 {"GUILD_TAGS_BADGE_PACK_PLANT",
                  {{"Clover", 31}, {"Blossom", 32}, {"Potted plant", 33}, {"Maple", 34}, {"Wilted flower", 35}}},
                 {"GUILD_TAGS_BADGE_PACK_CREEPY_CRAWLIES",
                  {{"Butterfly", 36}, {"Snail", 37}, {"Caterpillar", 38}, {"Spider", 39}, {"Bee", 40}}}
             })
          if (hasFeature(state, feature))
            badges.insert(badges.end(), entries.begin(), entries.end());
        const auto badge = value("badge", 6);
        if (std::ranges::none_of(badges, [&](const auto& item) { return item.second == badge; }))
          badges.emplace_back("Current badge", badge);
        choose("Badge shape", "badge", "Choose the shape displayed beside your server tag.", badges);
        colorField("Badge color", "badge_color_primary", "Set the main color of your server tag badge.");
        const auto shape = badge.is_number_integer() ? badge.get<int>() : 6;
        const bool secondary = shape == 0
            || shape == 9
            || shape == 3
            || (shape >= 10 && shape <= 20)
            || shape == 22
            || shape == 23
            || shape == 27
            || shape == 33
            || shape == 35
            || shape == 36
            || shape == 37
            || shape == 38
            || shape == 39;
        if (secondary)
          colorField(
              "Secondary badge color", "badge_color_secondary", "Set the secondary color of your server tag badge."
          );
      }

    } else if (state.page == "general") {
      section("Conversation defaults");
      channel(
          "System messages channel", "system_channel_id",
          "Select the channel for member joins, boosts and other server notices.", 0
      );
      const auto flags = value("system_channel_flags", 0);
      const auto flagBits = flags.is_number_unsigned() ? flags.get<unsigned>()
          : flags.is_number_integer()                  ? static_cast<unsigned>(flags.get<int>())
                                                       : 0U;
      for (const auto& [bit, title, description] : std::vector<std::tuple<unsigned, std::string, std::string>>{
               {0, "Member welcome messages", "Send a notice when someone joins the server."},
               {1, "Server boost messages", "Send a notice when someone boosts the server."},
               {2, "Server setup tips", "Show Discord's server setup tips."},
               {3, "Welcome stickers", "Allow a sticker reply to member welcome messages."}
           })
        toggleCallback(
            "system-" + std::to_string(bit), title, description, !(flagBits & (1U << bit)), true,
            [this, flagBits, bit](bool enabled) {
              setServerSettingsValue(
                  "system_channel_flags", enabled ? flagBits & ~(1U << bit) : flagBits | (1U << bit)
              );
            }
        );
      choose(
          "Default notifications", "default_message_notifications",
          "Choose the starting notification preference for new members.", {{"All messages", 0}, {"Only mentions", 1}}
      );
      section("Inactive voice members");
      channel("Inactive channel", "afk_channel_id", "Move inactive voice members into this channel.", 2);
      choose(
          "Inactive timeout", "afk_timeout", "How long a member can be inactive before moving to the inactive channel.",
          {{"1 minute", 60}, {"5 minutes", 300}, {"15 minutes", 900}, {"30 minutes", 1800}, {"1 hour", 3600}}
      );
      section("Server display");
      toggle("Progress bar", "premium_progress_bar_enabled", "Display progress toward the next server boost level.");
      choose(
          "Server language", "preferred_locale", "The primary language used in server discovery and Discord notices.",
          {{"English, US", "en-US"},
           {"English, UK", "en-GB"},
           {"Deutsch", "de"},
           {"Français", "fr"},
           {"Español", "es-ES"},
           {"Italiano", "it"},
           {"Português, Brasil", "pt-BR"},
           {"日本語", "ja"},
           {"한국어", "ko"},
           {"简体中文", "zh-CN"},
           {"Русский", "ru"}}
      );
    } else if (state.page == "safety") {
      section("Member verification");
      choose(
          "Verification level", "verification_level",
          "Members must meet this requirement before sending messages. Members with roles are exempt.",
          {{"None — unrestricted", 0},
           {"Low — verified email", 1},
           {"Medium — account older than 5 minutes", 2},
           {"High — server member for 10 minutes", 3},
           {"Highest — verified phone number", 4}}
      );
      choose(
          "Explicit media filter", "explicit_content_filter",
          "Automatically scan and remove explicit media from the selected members.",
          {{"Do not scan", 0}, {"Scan members without a role", 1}, {"Scan all members", 2}}
      );
      if (owner) {
        section("Moderator account security");
        toggleCallback(
            "mfa", "Require two-factor authentication",
            "Require moderators to enable two-factor authentication before using moderation actions.",
            value("mfa_level", 0) == 1, true,
            [this](bool checked) { setServerSettingsValue("mfa_level", checked ? 1 : 0); }
        );
      }
    } else if (state.page == "boosts") {
      section("Server boost status");
      row("Boost level", string(state.overview, "premium_tier", "0"));
      row("Active boosts", string(state.overview, "premium_subscription_count", "0"));
      if (state.overview.contains("max_video_channel_users"))
        row("Video channel capacity", string(state.overview, "max_video_channel_users"));
      if (state.overview.contains("max_stage_video_channel_users"))
        row("Stage video capacity", string(state.overview, "max_stage_video_channel_users"));
      section("Unlocked perks");
      for (const auto& [feature, name] : std::vector<std::pair<std::string, std::string>>{
               {"ANIMATED_ICON", "Animated server icon"},
               {"BANNER", "Server banner"},
               {"INVITE_SPLASH", "Invite background"},
               {"VANITY_URL", "Custom invite link"},
               {"ROLE_ICONS", "Role icons"},
               {"ENHANCED_ROLE_COLORS", "Enhanced role colors"},
               {"GUILD_TAGS", "Server tags"}
           })
        if (hasFeature(state, feature))
          row(name, "Available for this server.");
    } else if (state.page == "discovery") {
      section("Server discovery");
      toggle(
          "Publish in discovery", "is_published",
          "Allow eligible members to find this server in Discord's server discovery."
      );
      toggle(
          "Discover server emoji", "emoji_discoverability_enabled",
          "Allow this server's emoji to appear in public discovery surfaces."
      );
      field("About this server", "about", "Describe this community for people browsing discovery.", 96, 2);
      lineList(
          "Search keywords", "keywords", "Enter up to 10 keywords, one per line, to help people find this server."
      );
      lineList("Social links", "social_links", "Add community links, one per line.");
      std::vector<std::pair<std::string, Json>> categories;
      const auto at = state.lists.find("discovery-categories");
      if (at != state.lists.end() && at->second.is_array())
        for (const auto& category : at->second)
          if (category.contains("id"))
            categories.emplace_back(string(category, "name", string(category, "localizations")), category["id"]);
      if (!categories.empty())
        choose(
            "Primary category", "primary_category_id", "Choose the category that best describes your server.",
            categories
        );
      const auto requirements = state.lists.find("discovery-requirements");
      if (requirements != state.lists.end() && requirements->second.is_object()) {
        section("Eligibility");
        for (const auto& [key, entry] : requirements->second.items())
          if (entry.is_boolean()) {
            auto title = key;
            std::ranges::replace(title, '_', ' ');
            row(title, entry.get<bool>() ? "Met" : "Not met");
          }
      }
    } else if (state.page == "insights") {
      section("Server insights");
      auto period = state.lists.contains("insights-period")
          ? state.lists.at("insights-period")
          : Json{{"days", 28}, {"interval", 1}, {"metric", "engagement"}};
      chooseCallback(
          "insights-days", "Date range", "Choose the period to include in these server metrics.",
          {"Last 7 days", "Last 28 days", "Last 90 days"},
          period.value("days", 28) == 7        ? 0
              : period.value("days", 28) == 90 ? 2
                                               : 1,
          [this, period](std::size_t index) mutable {
            period["days"] = index == 0 ? 7 : index == 2 ? 90 : 28;
            m_serverSettings.lists["insights-period"] = period;
            api::GuildSettingsRequest request;
            request.section = Section::Insights;
            request.resourceId = period.value("metric", "engagement");
            request.values = {{"days", period["days"]}, {"interval", period["interval"]}};
            requestServerSettings(request);
          }
      );
      chooseCallback(
          "insights-metric", "Metrics", "Choose which server activity to inspect.",
          {"Engagement", "Growth", "Retention"},
          period.value("metric", "") == "growth"          ? 1
              : period.value("metric", "") == "retention" ? 2
                                                          : 0,
          [this, period](std::size_t index) mutable {
            period["metric"] = index == 1 ? "growth" : index == 2 ? "retention" : "engagement";
            m_serverSettings.lists["insights-period"] = period;
            api::GuildSettingsRequest request;
            request.section = Section::Insights;
            request.resourceId = period["metric"].get<std::string>();
            request.values = {{"days", period["days"]}, {"interval", period["interval"]}};
            requestServerSettings(request);
          }
      );
      chooseCallback(
          "insights-interval", "Aggregation", "Group server activity into daily, weekly or monthly totals.",
          {"Daily", "Weekly", "Monthly"}, std::clamp(period.value("interval", 1), 1, 3) - 1,
          [this, period](std::size_t index) mutable {
            period["interval"] = index + 1;
            m_serverSettings.lists["insights-period"] = period;
            api::GuildSettingsRequest request;
            request.section = Section::Insights;
            request.resourceId = period.value("metric", "engagement");
            request.values = {{"days", period["days"]}, {"interval", period["interval"]}};
            requestServerSettings(request);
          }
      );
      const auto at = state.lists.find("insights");
      if (at != state.lists.end() && at->second.is_array() && !at->second.empty()) {
        const auto& series = at->second;
        for (const auto& [key, last] : series.back().items())
          if (last.is_number() && key != "guild_id") {
            auto title = key;
            std::ranges::replace(title, '_', ' ');
            section(title);
            row("Latest value", displayValue(last));
            double maximum = 1;
            for (const auto& sample : series)
              if (sample.contains(key) && sample[key].is_number())
                maximum = std::max(maximum, sample[key].get<double>());
            const auto step = width / std::max<std::size_t>(1, series.size());
            std::size_t index = 0;
            for (const auto& sample : series) {
              const auto amount = sample.contains(key) && sample[key].is_number() ? sample[key].get<double>() : 0;
              const auto bar = static_cast<float>(std::clamp(amount / maximum, 0.0, 1.0) * 60);
              rectangle(
                  p, index * step, cursor + 60 - bar, std::max(1.0F, step - Style::spaceXxs), bar, accent,
                  Style::radiusXs
              );
              ++index;
            }
            cursor += 76;
          }
      } else if (!state.loading)
        row("No insights available", "Discord returned no metrics for this period. Try a different date range.");
    } else if (state.page == "community") {
      section("Community status");
      const auto communityFeatures = value("features", state.overview.value("features", Json::array()));
      const bool community = communityFeatures.is_array()
          && std::ranges::find(communityFeatures, Json("COMMUNITY")) != communityFeatures.end();
      toggleCallback(
          "community", "Enable Community",
          "Require verified email and media scanning, designate rules and moderator update channels, and unlock "
          "Community features.",
          community, admin, [this](bool enabled) {
            auto features = m_serverSettings.overview.value("features", Json::array());
            eraseJson(features, [](const auto& f) { return f == "COMMUNITY"; });
            if (enabled) {
              features.push_back("COMMUNITY");
              setServerSettingsValue("verification_level", 1);
              setServerSettingsValue("explicit_content_filter", 2);
            }
            setServerSettingsValue("features", features);
          }
      );
      channel("Rules channel", "rules_channel_id", "Choose the channel where members can read the server rules.", 0);
      channel(
          "Community updates", "public_updates_channel_id",
          "Choose a private channel where Discord sends Community moderator updates.", 0
      );
      channel(
          "Safety alerts", "safety_alerts_channel_id", "Choose a private moderator channel for safety notifications.", 0
      );
    } else if (state.page == "ownership") {
      section("Transfer server");
      field(
          "New owner's user ID", "owner_id",
          "Transfer full server ownership to an existing member. Only the current owner can perform this action."
      );
      detail("Verify ownership transfer", "Request a verification code at the email address on your Discord account.");
      button(p, 0, cursor, 128, "Send email code", "send-ownership-code");
      cursor += (Style::spaceSection - Style::spaceXxs);
      field("Email verification code", "code", "Enter the code sent by Discord before transferring ownership.");
    } else if (state.page == "delete") {
      section("Delete server");
      detail(
          "Delete " + guildName,
          "Permanently remove this server and all its channels and messages. Only the owner can delete the server."
      );
      button(p, 0, cursor, 128, "Delete Server", "delete-server", ButtonVariant::Destructive);
      cursor += (Style::spaceSection - Style::spaceXxs);
    } else if (state.page == "widget") {
      section("Server widget");
      toggle(
          "Enable server widget", "enabled",
          "Allow a public widget to display the server's online members and an invite link."
      );
      channel("Widget invite channel", "channel_id", "Choose the channel used by the widget's invite link.", 0);
      if (value("enabled", false) == true) {
        const auto item = row("Public widget URL", "https://discord.com/widget?id=" + state.guild, 90);
        button(p, width - 80, item.first + (item.second - 26) / 2, 80, "Copy URL", "copy-widget");
      }
    } else if (state.page == "vanity") {
      section("Custom invite link");
      field(
          "Invite code", "code",
          "Choose the code used after discord.gg/. Changing it makes the previous custom invite unavailable."
      );
      row("Current invite uses", string(state.record, "uses", "0"));
    } else if (state.page == "onboarding") {
      section("New member setup");
      toggle(
          "Enable onboarding", "enabled",
          "Let new members choose their roles and channels before they enter the server."
      );
      choose(
          "Onboarding mode", "mode", "Advanced mode supports additional customization of the new member experience.",
          {{"Default", 0}, {"Advanced", 1}}
      );
      channelsAndRoles("Default channels", "default_channel_ids", false);
      section("Questions");
      auto prompts = value("prompts", Json::array());
      if (prompts.is_array())
        for (std::size_t i = 0; i < prompts.size(); ++i) {
          const auto prompt = prompts[i];
          detail(
              "Question " + std::to_string(i + 1),
              "Ask new members about their interests and assign the matching roles and channels."
          );
          auto& title = inputControl(
              p, 0, cursor, width, 32, "server-settings:prompt-title:" + std::to_string(i), string(prompt, "title"),
              "Question"
          );
          title.setEnabled(fieldEditable("prompts"));
          title.setOnChange([this, prompts, i](const std::string& text) mutable {
            prompts[i]["title"] = text;
            setServerSettingsValue("prompts", prompts);
          });
          title.setOnSubmit([](const std::string&) {});
          cursor += (Style::spaceSection + Style::spaceXs);
          chooseCallback(
              "prompt-type-" + std::to_string(i), "Question format", "Show choices as buttons or as a dropdown.",
              {"Buttons", "Dropdown"}, prompt.value("type", 0), [this, prompts, i](std::size_t selected) mutable {
                prompts[i]["type"] = selected;
                setServerSettingsValue("prompts", prompts);
              }
          );
          for (const auto& [key, title, description] : std::vector<std::tuple<std::string, std::string, std::string>>{
                   {"required", "Required question", "Members must answer this question before continuing."},
                   {"single_select", "Allow only one answer", "Members can select only one option."},
                   {"in_onboarding", "Ask before joining", "Include this question in the initial onboarding flow."}
               })
            toggleCallback(
                "prompt-" + std::to_string(i) + key, title, description, prompt.value(key, false), true,
                [this, prompts, i, key](bool checked) mutable {
                  prompts[i][key] = checked;
                  setServerSettingsValue("prompts", prompts);
                }
            );
          if (prompt.contains("options") && prompt["options"].is_array())
            for (std::size_t j = 0; j < prompt["options"].size(); ++j) {
              const auto option = prompt["options"][j];
              detail(
                  "Answer " + std::to_string(j + 1),
                  "Set an answer and choose what members receive when they select it."
              );
              auto& input = inputControl(
                  p, 0, cursor, width, 32,
                  "server-settings:prompt-option:" + std::to_string(i) + ":" + std::to_string(j),
                  string(option, "title"), "Answer title"
              );
              input.setEnabled(fieldEditable("prompts"));
              input.setOnChange([this, prompts, i, j](const std::string& text) mutable {
                prompts[i]["options"][j]["title"] = text;
                setServerSettingsValue("prompts", prompts);
              });
              input.setOnSubmit([](const std::string&) {});
              cursor += (Style::spaceSection + Style::spaceXxs);
              auto& description = inputControl(
                  p, 0, cursor, width, 32,
                  "server-settings:prompt-description:" + std::to_string(i) + ":" + std::to_string(j),
                  string(option, "description"), "Optional description"
              );
              description.setEnabled(fieldEditable("prompts"));
              description.setOnChange([this, prompts, i, j](const std::string& text) mutable {
                prompts[i]["options"][j]["description"] = text;
                setServerSettingsValue("prompts", prompts);
              });
              description.setOnSubmit([](const std::string&) {});
              cursor += (Style::spaceSection + Style::spaceXxs);
              auto roleIds = option.value("role_ids", Json::array());
              for (const auto& role : m_roleDirectory.allRoles(state.guild)) {
                if (role.id == state.guild)
                  continue;
                const bool checked = std::ranges::find(roleIds, Json(role.id)) != roleIds.end();
                toggleCallback(
                    "prompt-role-" + std::to_string(i) + ":" + std::to_string(j) + role.id, role.name, "", checked,
                    owner || role.position < access.selfPosition,
                    [this, prompts, i, j, roleIds, id = role.id](bool enabled) mutable {
                      if (enabled)
                        roleIds.push_back(id);
                      else
                        eraseJson(roleIds, [&](const auto& v) { return v == id; });
                      prompts[i]["options"][j]["role_ids"] = roleIds;
                      setServerSettingsValue("prompts", prompts);
                    }
                );
              }
              auto channelIds = option.value("channel_ids", Json::array());
              for (const auto& c : guildChannels) {
                if (c.type == 4 || c.isThread())
                  continue;
                const bool checked = std::ranges::find(channelIds, Json(c.id)) != channelIds.end();
                toggleCallback(
                    "prompt-channel-" + std::to_string(i) + ":" + std::to_string(j) + c.id, "# " + c.name, "", checked,
                    true, [this, prompts, i, j, channelIds, id = c.id](bool enabled) mutable {
                      if (enabled)
                        channelIds.push_back(id);
                      else
                        eraseJson(channelIds, [&](const auto& v) { return v == id; });
                      prompts[i]["options"][j]["channel_ids"] = channelIds;
                      setServerSettingsValue("prompts", prompts);
                    }
                );
              }
              button(
                  p, 0, cursor, 82, "Move up", "option-move:up:" + std::to_string(i) + ":" + std::to_string(j),
                  ButtonVariant::Default, j > 0
              );
              button(
                  p, 94, cursor, 92, "Move down", "option-move:down:" + std::to_string(i) + ":" + std::to_string(j),
                  ButtonVariant::Default, j + 1 < prompt["options"].size()
              );
              cursor += (Style::spaceSection - Style::spaceXxs);
              button(
                  p, 0, cursor, 116, "Remove answer", "remove-option:" + std::to_string(i) + ":" + std::to_string(j)
              );
              cursor += (Style::spaceSection - Style::spaceXxs);
            }
          button(p, 0, cursor, 82, "Move up", "prompt-move:up:" + std::to_string(i), ButtonVariant::Default, i > 0);
          button(
              p, 94, cursor, 92, "Move down", "prompt-move:down:" + std::to_string(i), ButtonVariant::Default,
              i + 1 < prompts.size()
          );
          cursor += (Style::spaceSection - Style::spaceXxs);
          button(p, 0, cursor, 104, "Add answer", "add-option:" + std::to_string(i));
          cursor += (Style::spaceSection - Style::spaceXxs);
          button(p, 0, cursor, 126, "Remove question", "remove-prompt:" + std::to_string(i));
          cursor += (Style::spaceSection - Style::spaceXxs);
        }
      if (prompts.empty())
        row("No onboarding questions", "Add questions that let members choose roles and channels.");
      button(p, 0, cursor, 128, "Add question", "add-prompt");
      cursor += (Style::spaceSection - Style::spaceXxs);
    } else if (state.page == "welcome") {
      section("Welcome screen");
      toggle(
          "Enable welcome screen", "enabled",
          "Show a welcome screen with a description and recommended channels to new members."
      );
      field("Welcome description", "description", "Introduce your community in up to 140 characters.", 64, 2);
      auto channels = value("welcome_channels", Json::array());
      if (channels.is_array())
        for (std::size_t i = 0; i < channels.size(); ++i) {
          const auto entry = channels[i];
          detail(
              "Recommended channel " + std::to_string(i + 1), "Choose a channel and explain what members can do there."
          );
          std::vector<std::string> names;
          std::vector<std::string> ids;
          std::size_t at = 0;
          for (const auto& c : guildChannels)
            if (c.type == 0 || c.type == 5) {
              if (string(entry, "channel_id") == c.id)
                at = ids.size();
              ids.push_back(c.id);
              names.push_back("# " + c.name);
            }
          chooseCallback(
              "welcome-channel-" + std::to_string(i), "Channel", "", names, at,
              [this, channels, i, ids](std::size_t selected) mutable {
                if (selected < ids.size()) {
                  channels[i]["channel_id"] = ids[selected];
                  setServerSettingsValue("welcome_channels", channels);
                }
              }
          );
          auto& input = inputControl(
              p, 0, cursor, width, 32, "server-settings:welcome-description:" + std::to_string(i),
              string(entry, "description"), "Why should members visit this channel?"
          );
          input.setOnChange([this, channels, i](const std::string& text) mutable {
            channels[i]["description"] = text;
            setServerSettingsValue("welcome_channels", channels);
          });
          input.setOnSubmit([](const std::string&) {});
          cursor += (Style::spaceSection + Style::spaceXxs);
          button(p, 0, cursor, 90, "Remove", "remove-welcome:" + std::to_string(i));
          cursor += (Style::spaceSection - Style::spaceXxs);
        }
      if (channels.size() < 5) {
        button(p, 0, cursor, 128, "Add channel", "add-welcome");
        cursor += (Style::spaceSection - Style::spaceXxs);
      }
    } else if (state.page == "screening") {
      section("Rules screening");
      toggle(
          "Require rules acceptance", "enabled",
          "Require new members to accept the server rules before they can talk or send direct messages to members."
      );
      field("Introduction", "description", "Introduce the rules shown to new members.", 64, 2);
      auto fields = value("form_fields", Json::array());
      if (fields.is_array())
        for (std::size_t i = 0; i < fields.size(); ++i) {
          if (!fields[i].is_object() || fields[i].value("field_type", "") != "TERMS")
            continue;
          const auto rules = fields[i].value("values", Json::array());
          for (std::size_t j = 0; j < rules.size(); ++j) {
            detail("Rule " + std::to_string(j + 1), "State a rule that members must agree to follow.");
            auto& input = inputControl(
                p, 0, cursor, width, 64, "server-settings:rule:" + std::to_string(i) + ":" + std::to_string(j),
                rules[j].is_string() ? rules[j].get<std::string>() : "", {}, true
            );
            input.setOnChange([this, fields, i, j](const std::string& text) mutable {
              fields[i]["values"][j] = text;
              setServerSettingsValue("form_fields", fields);
            });
            input.setOnSubmit([](const std::string&) {});
            cursor += 76;
            button(p, 0, cursor, 100, "Remove rule", "remove-rule:" + std::to_string(i) + ":" + std::to_string(j));
            cursor += (Style::spaceSection - Style::spaceXxs);
          }
        }
      button(p, 0, cursor, 100, "Add rule", "add-rule");
      cursor += (Style::spaceSection - Style::spaceXxs);
    } else if (state.page == "audit") {
      finishCategory();
      cursor = drawServerAudit(p, width);
    } else {
      const bool editing = state.creating || !state.resource.empty();
      if (!editing) {
        section(selected->name);
        auto& input = inputControl(
            p, 0, cursor, std::max(60.0F, width - 88), 32, "server-settings:list-search", state.search,
            state.page == "members" ? "Search members by name" : "Filter this list"
        );
        input.setOnChange([this](const std::string& text) {
          m_serverSettings.search = text;
          m_window.invalidate();
        });
        input.setOnSubmit([this](const std::string&) { loadServerSettings(); });
        button(p, width - 80, cursor + (Style::spaceXxs + Style::borderWidth), 80, "Search", "search");
        cursor += (Style::spaceSection + Style::spaceXs);
        if (canCreate(selected->section)) {
          button(
              p, 0, cursor, 128,
              state.page == "roles"         ? "Create role"
                  : state.page == "events"  ? "Create event"
                  : state.page == "automod" ? "Create rule"
                                            : "Add " + std::string(selected->name),
              "new", ButtonVariant::Primary
          );
          cursor += (Style::spaceSection - Style::spaceXxs);
        }
        if (state.page == "invites") {
          button(p, 0, cursor, 128, "Create invite", "create-invite", ButtonVariant::Primary);
          cursor += (Style::spaceSection - Style::spaceXxs);
        }
        const auto at = state.lists.find(state.page);
        if (at != state.lists.end() && at->second.is_array()) {
          std::vector<Json> records;
          for (const auto& record : at->second)
            if (state.search.empty()
                || lower(recordName(record) + " " + recordId(record)).contains(lower(state.search)))
              records.push_back(record);
          if (state.page == "roles")
            std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
              return a.value("position", 0) > b.value("position", 0);
            });
          for (const auto& record : records) {
            const auto id = recordId(record);
            std::string description;
            if (state.page == "members")
              description = string(record["user"], "username") + " · " + id;
            else if (state.page == "bans")
              description = string(record, "reason", "No reason provided");
            else if (state.page == "invites")
              description = string(record, "uses", "0")
                  + " uses · "
                  + (string(record, "max_uses", "0") == "0" ? "unlimited uses"
                                                            : string(record, "max_uses") + " maximum")
                  + " · "
                  + (string(record, "expires_at").empty() ? "Never expires"
                                                          : "Expires " + localTime(string(record, "expires_at")));
            else if (state.page == "roles")
              description = record.value("managed", false) ? "Managed by an integration"
                  : id == state.guild                      ? "Default permissions for every member"
                                                           : "Position " + string(record, "position", "0");
            else if (state.page == "events")
              description = localTime(string(record, "scheduled_start_time")) + " · " + string(record, "description");
            else if (state.page == "audit")
              description =
                  "Action " + string(record, "action_type") + " · " + string(record, "reason", "No reason provided");
            else
              description = string(record, "description", string(record, "type", id));
            const auto item = row(recordName(record), description, state.page == "roles" ? 170 : 90);
            button(
                p, width - (state.page == "roles" ? 164 : 80), item.first + (item.second - 26) / 2, 80, "Manage",
                "record:" + id
            );
            if (state.page == "roles") {
              api::GuildSettingsRequest reorder;
              reorder.guildId = state.guild;
              reorder.section = Section::Roles;
              reorder.verb = Verb::Reorder;
              reorder.resourceId = id;
              const auto roleAccess = serverSettingsAccess(id);
              const bool canReorder = id != state.guild
                  && !record.value("managed", false)
                  && api::canManageGuildSettings(reorder, roleAccess);
              if (canReorder) {
                iconButton(
                    p,
                    {width - 74, item.first + (item.second - 26) / 2, 26, 26, Action::ServerSettingsAction,
                     "role-up:" + id},
                    "chevron-up", "Move role up"
                );
                iconButton(
                    p,
                    {width - 34, item.first + (item.second - 26) / 2, 26, 26, Action::ServerSettingsAction,
                     "role-down:" + id},
                    "chevron-down", "Move role down"
                );
              }
            }
          }
          if (records.empty())
            row(state.search.empty() ? "Nothing here yet" : "No matches",
                state.search.empty() ? "No records were returned for this server." : "Try another name or identifier.");
        } else if (!state.loading)
          row("Nothing here yet", "No records were returned for this server.");
        if (state.more) {
          button(p, 0, cursor, 112, "Load more", "more");
          cursor += (Style::spaceSection - Style::spaceXxs);
        }
        if (state.page == "members" && (admin || p::allows(access.permissions, p::kickMembers))) {
          finishCategory();
          section("Inactive members");
          detail("Prune inactive members", "Remove members who have been inactive for seven days and have no roles.");
          button(p, 0, cursor, 128, "Prune members", "prune", ButtonVariant::Destructive);
          cursor += (Style::spaceSection - Style::spaceXxs);
        }
      } else {
        button(p, 0, cursor, 100, "Back to list", "back");
        cursor += (Style::spaceSection - Style::spaceXxs);
        section(state.creating ? "Create " + std::string(selected->name) : recordName(state.record));
        if (state.page == "roles") {
          field("Role name", "name", "The name displayed in member profiles and the member list.");
          auto colors = value("colors", Json::object());
          if (!colors.is_object())
            colors = Json::object();
          const auto legacy = value("color", 0);
          const auto primary = colors.value("primary_color", legacy.is_number_integer() ? legacy.get<unsigned>() : 0U);
          const bool enhanced = hasFeature(state, "ENHANCED_ROLE_COLORS") || !colors.empty();
          if (hasFeature(state, "ENHANCED_ROLE_COLORS")) {
            const auto style = colors.contains("tertiary_color") && !colors["tertiary_color"].is_null() ? 2
                : colors.contains("secondary_color") && !colors["secondary_color"].is_null()            ? 1
                                                                                                        : 0;
            chooseCallback(
                "role-color-style", "Name color style",
                "Use a solid color, a two-color gradient or Discord's holographic preset.",
                {"Solid color", "Two-color gradient", "Holographic"}, style,
                [this, colors, primary](std::size_t selected) mutable {
                  if (selected == 2) {
                    setServerSettingsValue(
                        "colors",
                        {{"primary_color", 11127295}, {"secondary_color", 16759788}, {"tertiary_color", 16761760}}
                    );
                    return;
                  }
                  colors["primary_color"] = primary;
                  colors["secondary_color"] = selected >= 1 ? colors.value("secondary_color", Json(0xb4befe)) : Json();
                  if (selected >= 1 && colors["secondary_color"].is_null())
                    colors["secondary_color"] = 0xb4befe;
                  colors["tertiary_color"] = selected >= 2 ? colors.value("tertiary_color", Json(0xf5c2e7)) : Json();
                  if (selected >= 2 && colors["tertiary_color"].is_null())
                    colors["tertiary_color"] = 0xf5c2e7;
                  setServerSettingsValue("colors", colors);
                }
            );
          }
          const auto roleColor = [&](const std::string& title, const std::string& component, unsigned packed) {
            std::ostringstream colorLabel;
            colorLabel << '#' << std::hex << std::uppercase << std::setw(6) << std::setfill('0') << packed;
            detail(
                title,
                component == "primary_color" ? "Set the color used for this role's member names. The default color "
                                               "inherits the normal text color."
                                             : "Set another color in this role's name gradient."
            );
            const bool main = component == "primary_color";
            button(
                p, 0, cursor, 112, colorLabel.str(), main ? "role-color-picker" : "role-color-component:" + component,
                ButtonVariant::Default, fieldEditable("color")
            );
            if (main)
              button(
                  p, 124, cursor, 90, "Default", "default-role-color", ButtonVariant::Default, fieldEditable("color")
              );
            cursor += (Style::spaceSection - Style::spaceXxs);
            if (main ? state.colorPicker : state.profileColorPicker == component) {
              const auto controlId =
                  main ? std::string("server-role-color-picker") : "server-role-color-picker:" + component;
              auto saved = retained(controlId);
              auto picker = saved
                  ? std::unique_ptr<SettingsColorPicker>(static_cast<SettingsColorPicker*>(saved.release()))
                  : std::make_unique<SettingsColorPicker>();
              m_widgets[controlId] = picker.get();
              const auto pickerWidth = std::min(280.0F, width);
              picker->setPickerWidth(pickerWidth);
              picker->setPosition(0, cursor);
              picker->setColor(rgbHex(packed));
              picker->setEnabled(!state.saving && !state.mfaRequest && fieldEditable("color"));
              registerPicker(*picker, "role-color-" + component);
              picker->setOnColorChanged([this, colors, component, enhanced](const Color& color) mutable {
                const auto channel = [](float v) {
                  return static_cast<unsigned>(std::round(std::clamp(v, 0.0F, 1.0F) * 255));
                };
                const auto packed = (channel(color.r) << 16) | (channel(color.g) << 8) | channel(color.b);
                if (enhanced) {
                  colors[component] = packed;
                  setServerSettingsValue("colors", colors);
                } else
                  setServerSettingsValue("color", packed);
              });
              p.addChild(std::move(picker));
              cursor += SettingsColorPicker::intrinsicColumnHeight(pickerWidth, 1) + 12;
            }
          };
          const bool holographic = colors.contains("tertiary_color") && !colors["tertiary_color"].is_null();
          if (holographic) {
            detail("Holographic colors", "Discord uses a fixed blue, pink and peach palette for this style.");
            rectangle(p, 0, cursor, width / 3, 26, rgbHex(11127295), 4);
            rectangle(p, width / 3, cursor, width / 3, 26, rgbHex(16759788), 0);
            rectangle(p, 2 * width / 3, cursor, width / 3, 26, rgbHex(16761760), 4);
            cursor += (Style::spaceSection - Style::spaceXxs);
          }
          if (!holographic)
            roleColor("Role color", "primary_color", primary);
          if (!holographic && colors.contains("secondary_color") && colors["secondary_color"].is_number_integer())
            roleColor("Second color", "secondary_color", colors["secondary_color"].get<unsigned>());
          if (!holographic && colors.contains("tertiary_color") && colors["tertiary_color"].is_number_integer())
            roleColor("Third color", "tertiary_color", colors["tertiary_color"].get<unsigned>());
          toggle("Display role members separately", "hoist", "Show this role as a heading in the member list.");
          toggle(
              "Allow anyone to mention this role", "mentionable",
              "Let members mention this role even without Mention Everyone permission."
          );
          if (hasFeature(state, "ROLE_ICONS")) {
            upload(
                "Role icon", "icon", "Show an icon beside members with this role. Unicode emoji can be used instead."
            );
            field("Role emoji", "unicode_emoji", "Use a Unicode emoji as this role's icon.", 32, 2);
          }
          std::string group;
          auto decimal = value("permissions", "0");
          auto bits = decimal.is_string() ? decimal.get<std::string>() : std::string("0");
          const auto roleAccess = serverSettingsAccess(state.resource);
          const bool managed = state.record.value("managed", false);
          const bool hierarchy = owner
              || state.resource == state.guild
              || (!managed && roleAccess.targetPosition >= 0 && roleAccess.targetPosition < roleAccess.selfPosition);
          for (const auto& permission : permissions) {
            if (group != permission.group) {
              group = permission.group;
              section(group);
            }
            const bool checked = permissionEnabled(bits, permission.bit);
            const bool allowed =
                hierarchy && (admin || (access.permissions && (*access.permissions & (1ULL << permission.bit))));
            toggleCallback(
                "permission-" + std::to_string(permission.bit), permission.name, permission.description, checked,
                allowed, [this, bits, bit = permission.bit](bool enabled) {
                  setServerSettingsValue("permissions", api::guildRolePermission(bits, bit, enabled));
                }
            );
          }
          if (!state.creating) {
            section("Role members");
            detail(
                "Manage members with this role",
                "View the members who hold this role and assign or remove their server roles."
            );
            button(p, 0, cursor, 136, "Manage members", "role-members:" + state.resource);
            cursor += (Style::spaceSection - Style::spaceXxs);
          }
          if (!state.creating && state.resource != state.guild && !managed) {
            section("Role position");
            row("Position " + string(state.record, "position", "0"),
                "Members can only manage roles and members below their highest role.");
            button(p, 0, cursor, 86, "Move up", "role-up:" + state.resource);
            button(p, 98, cursor, 100, "Move down", "role-down:" + state.resource);
            cursor += (Style::spaceSection - Style::spaceXxs);
          }
        } else if (state.page == "members") {
          field(
              "Nickname", "nick", "Change this member's name in this server. Leave blank to use their profile name.",
              32, 2
          );
          section("Member roles");
          auto memberRoles = value("roles", Json::array());
          if (!memberRoles.is_array())
            memberRoles = Json::array();
          const auto targetAccess = serverSettingsAccess(state.resource);
          for (const auto& role : m_roleDirectory.allRoles(state.guild)) {
            if (role.id == state.guild)
              continue;
            const bool checked = std::ranges::find(memberRoles, Json(role.id)) != memberRoles.end();
            const bool managed = std::ranges::any_of(state.lists["roles"], [&](const auto& r) {
              return string(r, "id") == role.id && r.value("managed", false);
            });
            const bool allowed = fieldEditable("roles")
                && !managed
                && (owner
                    || (role.position < access.selfPosition && targetAccess.targetPosition < access.selfPosition));
            toggleCallback(
                "member-role-" + role.id, role.name, managed ? "Managed by an integration" : "", checked, allowed,
                [this, memberRoles, id = role.id](bool enabled) mutable {
                  if (enabled)
                    memberRoles.push_back(id);
                  else
                    eraseJson(memberRoles, [&](const auto& v) { return v == id; });
                  setServerSettingsValue("roles", memberRoles);
                }
            );
          }
          section("Moderation");
          std::vector<std::pair<std::string, Json>> timeouts = {{"No timeout", nullptr}};
          for (const auto& [seconds, name] : std::vector<std::pair<int, std::string>>{
                   {60, "1 minute"},
                   {300, "5 minutes"},
                   {600, "10 minutes"},
                   {3600, "1 hour"},
                   {86400, "1 day"},
                   {604800, "1 week"}
               }) {
            const auto time =
                std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() + std::chrono::seconds(seconds));
            std::tm utc{};
            gmtime_r(&time, &utc);
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
            timeouts.emplace_back(name, std::string(text));
          }
          if (!value("communication_disabled_until").is_null())
            timeouts.insert(timeouts.begin(), {"Keep current timeout", value("communication_disabled_until")});
          choose(
              "Timeout", "communication_disabled_until",
              "Temporarily restrict this member's messages, reactions and voice participation.", timeouts
          );
          toggle("Server mute", "mute", "Prevent this member from speaking in voice channels.");
          toggle("Server deafen", "deaf", "Prevent this member from hearing voice channels.");
          row("Remove member", "Kick permits rejoining with an invite. A ban prevents rejoining.");
          api::GuildSettingsRequest kickRequest;
          kickRequest.guildId = state.guild;
          kickRequest.section = Section::Members;
          kickRequest.verb = Verb::Delete;
          kickRequest.resourceId = state.resource;
          auto banRequest = kickRequest;
          banRequest.section = Section::Bans;
          banRequest.verb = Verb::Create;
          button(
              p, 0, cursor, 90, "Kick", "kick", ButtonVariant::Destructive,
              api::canManageGuildSettings(kickRequest, targetAccess)
          );
          button(
              p, 102, cursor, 90, "Ban", "ban", ButtonVariant::Destructive,
              api::canManageGuildSettings(banRequest, targetAccess)
          );
          cursor += (Style::spaceSection - Style::spaceXxs);
        } else if (state.page == "emoji") {
          if (!state.creating) {
            const auto image = api::guildExpressionAsset(Section::Emoji, state.record);
            picture(p, 0, cursor, 64, 64, image, Style::controlRadius, true);
            cursor += 76;
          }
          field("Emoji name", "name", "Use 2–32 letters, numbers or underscores.");
          if (state.creating)
            upload(
                "Emoji image", "image",
                "Choose an image no larger than 256 KiB. Animated GIF emoji require available animated emoji slots."
            );
          channelsAndRoles("Allowed roles", "roles", true);
        } else if (state.page == "stickers") {
          if (!state.creating) {
            const auto image = api::guildExpressionAsset(Section::Stickers, state.record);
            picture(
                p, (width - 160) / 2, cursor, 160, 160, image, Style::controlRadius, true, false, ImageFitMode::Contain
            );
            cursor += 172;
          }
          field("Sticker name", "name", "Choose a name between 2 and 30 characters.");
          field("Description", "description", "Describe this sticker for accessibility.", 64, 2);
          field("Related emoji", "tags", "Choose the emoji used to suggest this sticker.");
          if (state.creating)
            upload(
                "Sticker image", "file",
                "Choose a 320×320 PNG, APNG or GIF sticker up to 512 KiB. Verified and partnered servers can also use "
                "Lottie JSON."
            );
        } else if (state.page == "soundboard") {
          field("Sound name", "name", "Choose a name between 2 and 32 characters.");
          if (state.creating)
            upload(
                "Sound", "sound", "Choose an Ogg or MP3 sound no longer than 5.2 seconds and no larger than 512 KiB."
            );
          drawServerExtraForms(form, "sound-volume");
          field("Emoji", "emoji_name", "Use a Unicode emoji to identify this sound.", 32, 2);
          if (const auto at = state.lists.find("emoji"); at != state.lists.end() && at->second.is_array()) {
            std::vector<std::string> names = {"No custom emoji"}, ids = {""};
            std::size_t selected = 0;
            const auto current = value("emoji_id");
            for (const auto& emoji : at->second) {
              const auto id = recordId(emoji);
              if (current == id)
                selected = ids.size();
              names.push_back(":" + recordName(emoji) + ":");
              ids.push_back(id);
            }
            chooseCallback(
                "sound-custom-emoji", "Server emoji", "Choose a custom server emoji instead of a Unicode emoji.", names,
                selected, [this, ids](std::size_t index) {
                  if (index < ids.size()) {
                    setServerSettingsValue("emoji_id", ids[index].empty() ? Json() : Json(ids[index]));
                    if (!ids[index].empty())
                      setServerSettingsValue("emoji_name", nullptr);
                  }
                }
            );
          }
        } else if (state.page == "webhooks") {
          field("Webhook name", "name", "The name shown on messages posted by this webhook.");
          upload("Webhook avatar", "avatar", "Choose an image used by messages from this webhook.");
          channel("Channel", "channel_id", "Choose where this webhook posts messages.", 0);
          row("Webhook ID", state.resource);
        } else if (state.page == "templates") {
          drawServerExtraForms(form, "template-summary");
          field("Template name", "name", "The name shown when someone previews this template.");
          field("Description", "description", "Explain what this server template provides.", 64, 2);
          if (!state.creating) {
            button(p, 0, cursor, 112, "Copy link", "copy-template");
            button(p, 124, cursor, 120, "Sync template", "sync-template");
            cursor += (Style::spaceSection - Style::spaceXxs);
          }
        } else if (state.page == "events") {
          field("Event name", "name", "Choose a name for the event.");
          field("Description", "description", "Tell members what to expect.", 80, 2);
          upload("Cover image", "image", "Choose an image displayed on the event card.");
          choose(
              "Event location", "entity_type", "Host this event in a voice channel, Stage channel or another location.",
              {{"Stage channel", 1}, {"Voice channel", 2}, {"Somewhere else", 3}}
          );
          const auto entityType = value("entity_type", 3);
          if (entityType != 3) {
            std::vector<std::pair<std::string, Json>> choices;
            for (const auto& c : guildChannels)
              if (c.type == (entityType == 1 ? 13 : 2))
                choices.emplace_back(c.name, c.id);
            choose("Event channel", "channel_id", "Choose the channel hosting this event.", choices);
          }
          dateTimeField("Starts", "scheduled_start_time", "Choose when the event starts.", false);
          dateTimeField(
              "Ends", "scheduled_end_time",
              entityType == 3 ? "Choose when this external event ends." : "Optionally choose when the event ends.", true
          );
          const auto metadata = value("entity_metadata", Json::object());
          if (entityType == 3) {
            detail("External location", "Provide a link or address when hosting somewhere else.");
            auto& input = inputControl(
                p, 0, cursor, width, 32, "server-settings:event-location", string(metadata, "location"),
                "Where is the event?"
            );
            input.setOnChange([this](const std::string& text) {
              setServerSettingsValue("entity_metadata", {{"location", text}});
            });
            input.setOnSubmit([](const std::string&) {});
            cursor += (Style::spaceSection + Style::spaceXs);
          }
          drawServerExtraForms(form, "event-recurrence");
          if (!state.creating) {
            const auto currentStatus = state.record.value("status", 1);
            std::vector<std::pair<std::string, Json>> statuses = currentStatus == 1
                ? std::vector<std::pair<std::string, Json>>{{"Scheduled", 1}, {"Active", 2}, {"Canceled", 4}}
                : currentStatus == 2 ? std::vector<std::pair<std::string, Json>>{{"Active", 2}, {"Completed", 3}}
                                     : std::vector<std::pair<std::string, Json>>{
                                           {currentStatus == 3 ? "Completed" : "Canceled", currentStatus}
                                       };
            choose(
                "Event status", "status", "Start a scheduled event, finish an active event or cancel before it starts.",
                statuses
            );
          }
        } else if (state.page == "automod") {
          field("Rule name", "name", "Identify this automatic moderation rule.");
          toggle("Enabled", "enabled", "Apply this rule when a matching message is sent.");
          if (state.creating)
            choose(
                "Trigger", "trigger_type", "Choose the type of content that activates this rule.",
                {{"Custom keywords", 1}, {"Spam content", 3}, {"Commonly flagged words", 4}, {"Mention spam", 5}}
            );
          auto metadata = value("trigger_metadata", Json::object());
          const auto trigger = value("trigger_type", 1);
          const auto metadataLines = [&](const std::string& title, const std::string& key,
                                         const std::string& description) {
            std::string text;
            if (metadata.contains(key) && metadata[key].is_array())
              for (const auto& item : metadata[key])
                if (item.is_string()) {
                  if (!text.empty())
                    text += '\n';
                  text += item.get<std::string>();
                }
            detail(title, description);
            auto& input = inputControl(p, 0, cursor, width, 80, "server-settings:automod-" + key, text, {}, true);
            input.setOnChange([this, metadata, key](const std::string& text) mutable {
              Json words = Json::array();
              std::size_t start = 0;
              while (start < text.size()) {
                const auto end = text.find('\n', start);
                const auto word = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!word.empty())
                  words.push_back(word);
                if (end == std::string::npos)
                  break;
                start = end + 1;
              }
              metadata[key] = words;
              setServerSettingsValue("trigger_metadata", metadata);
            });
            input.setOnSubmit([](const std::string&) {});
            cursor += 92;
          };
          if (trigger == 1) {
            metadataLines(
                "Keywords", "keyword_filter",
                "Enter one keyword per line. Use * at the start or end to match parts of words."
            );
            metadataLines(
                "Regular expressions", "regex_patterns",
                "Enter one Rust-compatible regular expression per line, up to ten expressions."
            );
            metadataLines("Allowed words", "allow_list", "Enter exceptions to this filter, one per line.");
          }
          if (trigger == 4) {
            for (const auto& [preset, title] :
                 std::vector<std::pair<int, std::string>>{{1, "Profanity"}, {2, "Sexual content"}, {3, "Slurs"}}) {
              auto presets = metadata.value("presets", Json::array());
              toggleCallback(
                  "preset-" + std::to_string(preset), title, "Filter Discord's predefined word list.",
                  std::ranges::find(presets, Json(preset)) != presets.end(), true,
                  [this, metadata, presets, preset](bool checked) mutable {
                    if (checked)
                      presets.push_back(preset);
                    else
                      eraseJson(presets, [&](const auto& v) { return v == preset; });
                    metadata["presets"] = presets;
                    setServerSettingsValue("trigger_metadata", metadata);
                  }
              );
            }
            metadataLines("Allowed words", "allow_list", "Enter exceptions to the predefined filters, one per line.");
          }
          if (trigger == 5) {
            chooseCallback(
                "mention-limit", "Mention limit", "Block messages that mention more than this many members or roles.",
                {"3 mentions", "5 mentions", "10 mentions", "20 mentions", "50 mentions"},
                metadata.value("mention_total_limit", 5) == 3        ? 0
                    : metadata.value("mention_total_limit", 5) == 10 ? 2
                    : metadata.value("mention_total_limit", 5) == 20 ? 3
                    : metadata.value("mention_total_limit", 5) == 50 ? 4
                                                                     : 1,
                [this, metadata](std::size_t i) mutable {
                  static constexpr int limits[] = {3, 5, 10, 20, 50};
                  metadata["mention_total_limit"] = limits[std::min<std::size_t>(i, 4)];
                  setServerSettingsValue("trigger_metadata", metadata);
                }
            );
            toggleCallback(
                "mention-raid", "Mention raid protection", "Detect and block repeated mention spam across messages.",
                metadata.value("mention_raid_protection_enabled", true), true, [this, metadata](bool checked) mutable {
                  metadata["mention_raid_protection_enabled"] = checked;
                  setServerSettingsValue("trigger_metadata", metadata);
                }
            );
          }
          section("Rule actions");
          auto actions = value("actions", Json::array());
          const auto alertDefault = std::ranges::find_if(guildChannels, [](const auto& channel) {
            return channel.type == 0 || channel.type == 5;
          });
          const auto alertChannel = alertDefault == guildChannels.end() ? std::string{} : alertDefault->id;
          for (const auto& [type, title, description] : std::vector<std::tuple<int, std::string, std::string>>{
                   {1, "Block message", "Prevent the matching message from being sent."},
                   {2, "Send alert", "Notify moderators in a selected channel."},
                   {3, "Timeout member", "Temporarily restrict the member when this rule matches."}
               }) {
            if (type == 3 && trigger != 1 && trigger != 5)
              continue;
            const auto found =
                std::ranges::find_if(actions, [&](const auto& action) { return action.value("type", 0) == type; });
            const bool checked = found != actions.end();
            toggleCallback(
                "automod-action-" + std::to_string(type), title, description, checked,
                type != 3 || admin || p::allows(access.permissions, p::moderateMembers),
                [this, actions, type, alertChannel](bool enabled) mutable {
                  eraseJson(actions, [&](const auto& action) { return action.value("type", 0) == type; });
                  if (enabled) {
                    Json action = {{"type", type}};
                    if (type == 2)
                      action["metadata"] = {{"channel_id", alertChannel}};
                    if (type == 3)
                      action["metadata"] = {{"duration_seconds", 600}};
                    actions.push_back(action);
                  }
                  setServerSettingsValue("actions", actions);
                }
            );
            if (checked && type == 2) {
              std::vector<std::string> names, ids;
              std::size_t index = 0;
              for (const auto& c : guildChannels)
                if (c.type == 0 || c.type == 5) {
                  if (found->value("metadata", Json::object()).value("channel_id", "") == c.id)
                    index = ids.size();
                  ids.push_back(c.id);
                  names.push_back("# " + c.name);
                }
              chooseCallback(
                  "alert-channel", "Alert channel", "Send moderation alerts to this channel.", names, index,
                  [this, actions, ids](std::size_t selected) mutable {
                    if (selected < ids.size()) {
                      for (auto& action : actions)
                        if (action.value("type", 0) == 2)
                          action["metadata"] = {{"channel_id", ids[selected]}};
                      setServerSettingsValue("actions", actions);
                    }
                  }
              );
            }
            if (checked && type == 3) {
              const auto duration = found->value("metadata", Json::object()).value("duration_seconds", 600);
              chooseCallback(
                  "timeout-duration", "Timeout duration", "How long to restrict a member after a match.",
                  {"1 minute", "10 minutes", "1 hour", "1 day", "1 week"},
                  duration == 60           ? 0
                      : duration == 3600   ? 2
                      : duration == 86400  ? 3
                      : duration == 604800 ? 4
                                           : 1,
                  [this, actions](std::size_t selected) mutable {
                    static constexpr int seconds[] = {60, 600, 3600, 86400, 604800};
                    for (auto& action : actions)
                      if (action.value("type", 0) == 3)
                        action["metadata"] = {{"duration_seconds", seconds[std::min<std::size_t>(selected, 4)]}};
                    setServerSettingsValue("actions", actions);
                  }
              );
            }
          }
          section("Exceptions");
          channelsAndRoles("Exempt roles", "exempt_roles", true);
          channelsAndRoles("Exempt channels", "exempt_channels", false);
        } else if (state.page == "integrations") {
          row("Integration type", string(state.record, "type"));
          row("Connected account",
              state.record.contains("account") ? string(state.record["account"], "name") : "Unknown");
          toggle("Enable emoticons", "enable_emoticons", "Allow this integration's subscriber emoji in the server.");
          choose(
              "Expired subscription action", "expire_behavior",
              "Choose what happens when a member's subscription expires.", {{"Remove role", 0}, {"Kick", 1}}
          );
          choose(
              "Grace period", "expire_grace_period",
              "Wait this many days after a subscription expires before taking action.",
              {{"1 day", 1}, {"3 days", 3}, {"7 days", 7}, {"14 days", 14}, {"30 days", 30}}
          );
        } else if (state.page == "invites") {
          row("Invite URL", "https://discord.gg/" + state.resource);
          row("Uses", string(state.record, "uses", "0"));
          row("Maximum uses",
              string(state.record, "max_uses", "0") == "0" ? "Unlimited" : string(state.record, "max_uses"));
          row("Expires",
              string(state.record, "expires_at").empty() ? "Never" : localTime(string(state.record, "expires_at")));
          row("Temporary membership", state.record.value("temporary", false) ? "Enabled" : "Disabled");
          button(p, 0, cursor, 112, "Copy invite", "copy-invite");
          cursor += (Style::spaceSection - Style::spaceXxs);
        } else if (state.page == "bans") {
          row("User", recordName(state.record));
          row("Reason", string(state.record, "reason", "No reason provided"));
        } else if (state.page == "audit") {
          row("Action", auditAction(state.record.value("action_type", 0)));
          row("Performed by", actorName(string(state.record, "user_id")));
          row("Target", targetName(string(state.record, "target_id")));
          row("Reason", string(state.record, "reason", "No reason provided"));
          if (state.record.contains("changes") && state.record["changes"].is_array())
            for (const auto& change : state.record["changes"])
              row(string(change, "key", "Changed value"),
                  (change.contains("old_value") ? displayValue(change["old_value"]) : "Unset")
                      + " → "
                      + (change.contains("new_value") ? displayValue(change["new_value"]) : "Unset"));
        }
        api::GuildSettingsRequest deleteRequest;
        deleteRequest.guildId = state.guild;
        deleteRequest.section = selected->section;
        deleteRequest.verb = Verb::Delete;
        deleteRequest.resourceId = state.resource;
        if (canDelete(selected->section)
            && api::canManageGuildSettings(deleteRequest, serverSettingsAccess(state.resource))
            && !state.creating
            && !(state.page == "roles" && (state.resource == state.guild || state.record.value("managed", false)))) {
          section("Remove");
          detail(
              state.page == "bans"               ? "Revoke ban"
                  : state.page == "invites"      ? "Revoke invite"
                  : state.page == "integrations" ? "Remove integration"
                                                 : "Delete " + recordName(state.record),
              state.page == "bans"          ? "Allow this member to rejoin with a valid invite."
                  : state.page == "invites" ? "Prevent this invite link from being used again."
                                            : "This action removes the selected item from the server."
          );
          button(
              p, 0, cursor, 128,
              state.page == "bans"          ? "Revoke ban"
                  : state.page == "invites" ? "Revoke invite"
                                            : "Delete",
              "delete", ButtonVariant::Destructive
          );
          cursor += (Style::spaceSection - Style::spaceXxs);
        }
      }
    }
    finishCategory();
    const auto fullHeight = cursor + Style::spaceSm;
    m_scrolling.limit(state.scroll, std::max(0.0F, fullHeight - viewportH));
    pageNode->setTextScrollOffset(-state.scroll);
    const auto hitShift = bodyScrollBefore - state.scroll;
    const bool bodyTooltip = m_tooltip != tooltipBefore || m_tooltipX != tooltipXBefore || m_tooltipY != tooltipYBefore;
    bool tooltipVisible = false;
    for (auto at = bodyHitsBegin; at < m_hits.size(); ++at) {
      auto& hit = m_hits[at];
      const bool ownsTooltip =
          bodyTooltip && std::abs(hit.x + hit.width / 2 - m_tooltipX) < 0.5F && std::abs(hit.y - m_tooltipY) < 0.5F;
      const auto left = std::max(hit.x, contentLeft), right = std::min(hit.x + hit.width, contentLeft + contentWidth);
      const auto top = std::max(hit.y + hitShift, viewportY),
                 bottom = std::min(hit.y + hit.height + hitShift, viewportY + viewportH);
      hit.x = left;
      hit.y = top;
      hit.width = std::max(0.0F, right - left);
      hit.height = std::max(0.0F, bottom - top);
      if (ownsTooltip && hit.width > 0 && hit.height > 0 && hovered(hit.x, hit.y, hit.width, hit.height)) {
        tooltipVisible = true;
        m_tooltipX = hit.x + hit.width / 2;
        m_tooltipY = hit.y;
      }
    }
    m_hits.erase(
        std::remove_if(
            m_hits.begin() + bodyHitsBegin, m_hits.end(),
            [](const auto& hit) { return hit.width <= 0 || hit.height <= 0; }
        ),
        m_hits.end()
    );
    if (bodyTooltip && !tooltipVisible) {
      m_tooltip = tooltipBefore;
      m_tooltipX = tooltipXBefore;
      m_tooltipY = tooltipYBefore;
    }
    bodyNode->setPosition(layout::Settings::contentInset, -state.scroll);
    root.addChild(std::move(pageNode));
    settingsScrollbar(
        root, "server-settings-scrollbar",
        {frame.contentScrollbarX, viewportY, layout::Settings::scrollbarWidth, viewportH}, fullHeight, state.scroll
    );
    if (dirty) {
      const float y = h - Style::spaceXl - saveHeight - (feedbackHeight ? feedbackHeight + Style::spaceSm : 0);
      rectangle(root, contentX, y, width, saveHeight, panel, Style::hoverRadius);
      label(
          root, contentX + Style::spaceMd, y + Style::cardPadding,
          width < layout::Settings::stackThreshold ? width - Style::spaceXl : width - 236, "Unsaved changes",
          Style::fontSizeDescription, foreground
      );
      const auto by = y + saveHeight - (Style::space2Xl + Style::spaceXxs);
      const auto saveWidth = std::min(122.0F, width * .55F), resetWidth = std::min(76.0F, width * .3F);
      button(root, contentX + width - saveWidth - resetWidth - Style::spaceXl, by, resetWidth, "Reset", "reset");
      button(
          root, contentX + width - saveWidth - Style::spaceMd, by, saveWidth,
          state.saving         ? "Saving…"
              : state.creating ? "Create"
                               : "Save Changes",
          "save", ButtonVariant::Primary, !state.imageCrop
      );
    }
    if (feedbackHeight) {
      rectangle(root, contentX, h - Style::spaceXl - feedbackHeight, width, feedbackHeight, panel, Style::hoverRadius);
      label(
          root, contentX + Style::spaceMd, h - Style::cardPadding - feedbackHeight, feedbackTextWidth, feedback,
          Style::fontSizeDescription, state.error.empty() ? muted : danger, FontWeight::Normal, 0
      );
      if (retryRead)
        button(
            root, contentX + width - 92,
            width < layout::Settings::stackThreshold
                ? h - 56
                : h - Style::spaceXl - feedbackHeight + (feedbackHeight - (Style::spaceXl + Style::spaceXxs)) / 2,
            80, "Retry", "retry"
        );
    }
  }
} // namespace app
