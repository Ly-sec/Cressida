#include "app/client_application.h"
#include "app/settings_color_picker.h"
#include "app/settings_style.h"
#include "app/theme.h"
#include "core/input/key_modifiers.h"
#include "render/scene/rect_node.h"
#include "ui/controls/input.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;
  namespace {
    bool serverDialog(const std::string& kind) {
      return kind == "server-notifications"
          || kind == "server-privacy"
          || kind == "server-leave"
          || kind == "server-folder"
          || kind == "server-folder-settings";
    }
    std::string dialogTitle(const std::string& kind) {
      return kind == "server-folder-settings" ? "Folder Settings"
          : kind == "server-notifications"    ? "Notification Settings"
          : kind == "server-privacy"          ? "Privacy Settings"
          : kind == "server-leave"            ? "Leave Server"
                                              : "Create Server Folder";
    }
  } // namespace

  void ClientApplication::openServerMenuDialog(const std::string& kind, const std::string& guild) {
    if (!serverDialog(kind) || std::ranges::find(m_guilds, guild, &api::Guild::id) == m_guilds.end())
      return;
    closeContext();
    closeDialog();
    closeCompletion();
    closeGifPicker();
    closeEmojiPicker();
    m_scrolling.stopAll();
    m_serverDialogGuild = guild;
    m_serverDialogScroll = 0;
    m_serverPrivacyLoaded = false;
    m_serverPrivacyDraft = {};
    m_serverNotificationOriginal = m_userDirectory.guildPreferences(guild);
    m_serverNotificationOriginal.muted = m_userDirectory.muted(guild);
    m_serverNotificationDraft = m_serverNotificationOriginal;
    m_dialogKind = kind;
    m_dialogTitle = dialogTitle(kind);
    m_dialogError.clear();
    m_dialogText.clear();
    m_dialogPending = false;
    if (kind == "server-folder" || kind == "server-folder-settings")
      m_focusedInput = "dialog";
    else if (kind == "server-privacy") {
      api::Interaction action{
          .kind = api::InteractionKind::GuildPrivacy, .guild = guild, .revision = m_dialogGeneration
      };
      m_dialogAction = action;
      if (m_demo || startInteraction(action))
        m_dialogPending = true;
      else
        m_dialogError = m_status;
    }
    if (kind == "server-leave" && m_roleDirectory.owner(guild) == m_account.id)
      m_dialogError = "Transfer ownership of this server before leaving it.";
    m_window.invalidate();
  }

  void ClientApplication::serverMenuDialogAction(const std::string& id) {
    if (!serverDialog(m_dialogKind) || m_dialogPending)
      return;
    if (id == "privacy-retry" && m_dialogKind == "server-privacy" && !m_serverPrivacyLoaded) {
      const auto guild = m_serverDialogGuild;
      openServerMenuDialog("server-privacy", guild);
      return;
    }
    if (m_dialogKind == "server-notifications") {
      auto& draft = m_serverNotificationDraft;
      if (id == "mute")
        draft.muted = !draft.muted.value_or(false);
      else if (id == "suppress-everyone")
        draft.suppressEveryone = !draft.suppressEveryone.value_or(false);
      else if (id == "suppress-roles")
        draft.suppressRoles = !draft.suppressRoles.value_or(false);
      else if (id == "mobile-push")
        draft.mobilePush = !draft.mobilePush.value_or(true);
      else if (id == "mute-events")
        draft.muteScheduledEvents = !draft.muteScheduledEvents.value_or(false);
      else if (id.starts_with("level:") || id.starts_with("highlights:")) {
        const auto colon = id.find(':');
        int value = -1;
        const auto result = std::from_chars(id.data() + colon + 1, id.data() + id.size(), value);
        if (result.ec == std::errc{} && result.ptr == id.data() + id.size() && value >= 0 && value <= 2) {
          if (id.starts_with("level:"))
            draft.level = value;
          else
            draft.notifyHighlights = value;
        }
      }
    } else if (m_dialogKind == "server-privacy" && m_serverPrivacyLoaded) {
      auto& draft = m_serverPrivacyDraft;
      if (id == "direct-messages")
        draft.directMessages = !draft.directMessages.value_or(true);
      else if (id == "activity")
        draft.activity = !draft.activity.value_or(true);
      else if (id == "activity-joining")
        draft.activityJoining = !draft.activityJoining.value_or(true);
      else if (id == "message-requests")
        draft.messageRequests = !draft.messageRequests.value_or(true);
    }
    m_dialogError.clear();
    m_window.invalidate();
  }

  void ClientApplication::submitServerMenuDialog() {
    if (!serverDialog(m_dialogKind) || m_dialogPending)
      return;
    if (std::ranges::find(m_guilds, m_serverDialogGuild, &api::Guild::id) == m_guilds.end()) {
      m_dialogError = "This server is no longer available.";
      m_window.invalidate();
      return;
    }
    api::Interaction action{.guild = m_serverDialogGuild, .revision = m_dialogGeneration};
    if (m_dialogKind == "server-notifications") {
      action.kind = api::InteractionKind::GuildNotifications;
      const auto& previous = m_serverNotificationOriginal;
      api::GuildNotification patch;
      const auto changed = [](const auto& desired, const auto& before, auto& target, auto fallback) {
        if (desired && *desired != before.value_or(fallback))
          target = desired;
      };
      changed(m_serverNotificationDraft.muted, previous.muted, patch.muted, false);
      changed(m_serverNotificationDraft.suppressEveryone, previous.suppressEveryone, patch.suppressEveryone, false);
      changed(m_serverNotificationDraft.suppressRoles, previous.suppressRoles, patch.suppressRoles, false);
      changed(m_serverNotificationDraft.mobilePush, previous.mobilePush, patch.mobilePush, true);
      changed(
          m_serverNotificationDraft.muteScheduledEvents, previous.muteScheduledEvents, patch.muteScheduledEvents, false
      );
      changed(m_serverNotificationDraft.notifyHighlights, previous.notifyHighlights, patch.notifyHighlights, 0);
      if (m_serverNotificationDraft.level.value_or(3) != 3)
        changed(m_serverNotificationDraft.level, previous.level, patch.level, 3);
      if (patch.empty()) {
        closeDialog();
        return;
      }
      action.guildNotification = std::move(patch);
      action.stateRevision = m_userDirectory.guildNotificationRevision(action.guild);
    } else if (m_dialogKind == "server-privacy") {
      if (!m_serverPrivacyLoaded)
        return;
      action.kind = api::InteractionKind::GuildPrivacy;
      api::GuildPrivacy patch;
      const auto changed = [](const auto& desired, const auto& before, auto& target) {
        if (desired && *desired != before.value_or(true))
          target = desired;
      };
      changed(m_serverPrivacyDraft.directMessages, m_serverPrivacyOriginal.directMessages, patch.directMessages);
      changed(m_serverPrivacyDraft.activity, m_serverPrivacyOriginal.activity, patch.activity);
      changed(m_serverPrivacyDraft.activityJoining, m_serverPrivacyOriginal.activityJoining, patch.activityJoining);
      changed(m_serverPrivacyDraft.messageRequests, m_serverPrivacyOriginal.messageRequests, patch.messageRequests);
      if (patch.empty()) {
        closeDialog();
        return;
      }
      action.guildPrivacy = std::move(patch);
    } else if (m_dialogKind == "server-leave") {
      if (m_roleDirectory.owner(action.guild) == m_account.id) {
        m_dialogError = "Transfer ownership of this server before leaving it.";
        m_window.invalidate();
        return;
      }
      action.kind = api::InteractionKind::LeaveGuild;
    } else if (m_dialogKind == "server-folder-settings") {
      const auto folder = std::ranges::find(m_folders, m_serverDialogFolder, &api::GuildFolder::id);
      const auto input = m_inputs.find("server-menu:folder-color");
      const auto text = input == m_inputs.end() ? m_serverFolderColor : input->second->value();
      std::string_view digits = text;
      if (digits.starts_with('#'))
        digits.remove_prefix(1);
      Color selected;
      if (folder == m_folders.end()
          || (digits.size() != 3 && digits.size() != 6)
          || !tryParseHexColor(text, selected)) {
        m_dialogError = folder == m_folders.end() ? "This folder is no longer available." : "Enter a color as #RRGGBB.";
        m_window.invalidate();
        return;
      }
      const auto byte = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255));
      };
      const auto color = (byte(selected.r) << 16) | (byte(selected.g) << 8) | byte(selected.b);
      action.kind = api::InteractionKind::EditGuildFolder;
      action.folderChange = api::GuildFolderChange{.folder = folder->id, .name = m_dialogText, .color = color};
      if (!api::validFolderChange(*action.folderChange)) {
        m_dialogError = "Use a folder name of at most 100 characters and a color as #RRGGBB.";
        m_window.invalidate();
        return;
      }
      if (m_dialogText == folder->name && color == folder->color) {
        closeDialog();
        return;
      }
    } else {
      action.kind = api::InteractionKind::MoveGuild;
      action.content = m_dialogText;
      if (action.content.find_first_not_of(" \t\r\n") == std::string::npos
          || action.content.size() > 400
          || api::utf8Length(action.content) > 100
          || std::ranges::any_of(action.content, [](unsigned char c) { return c < 32 || c == 127; })) {
        m_dialogError = "Enter a folder name with 1–100 characters.";
        m_window.invalidate();
        return;
      }
    }
    if (!api::interactionRequest(action)) {
      m_dialogError = "Check your selections and try again.";
      m_window.invalidate();
      return;
    }
    if (action.kind == api::InteractionKind::MoveGuild || action.kind == api::InteractionKind::EditGuildFolder)
      action.stateRevision = m_guildLayoutRevision;
    action.revision = ++m_dialogGeneration;
    if (m_demo || startInteraction(action)) {
      m_dialogAction = std::move(action);
      m_dialogPending = true;
      m_dialogError.clear();
    } else
      m_dialogError = m_status;
    m_window.invalidate();
  }

  bool ClientApplication::serverMenuDialogResult(const api::Event& event) {
    if (!event.interaction || !serverDialog(m_dialogKind) || !m_dialogAction || !m_dialogPending)
      return false;
    const auto& action = *event.interaction;
    if (action.guild != m_serverDialogGuild
        || action.guild != m_dialogAction->guild
        || action.kind != m_dialogAction->kind
        || action.revision != m_dialogGeneration
        || action.revision != m_dialogAction->revision)
      return false;
    m_dialogPending = false;
    if (!event.error.empty())
      m_dialogError = event.error;
    else if (m_dialogKind == "server-privacy" && (!action.guildPrivacy || action.guildPrivacy->empty())) {
      if (event.guildPrivacy) {
        m_serverPrivacyDraft = *event.guildPrivacy;
        m_serverPrivacyOriginal = m_serverPrivacyDraft;
        m_serverPrivacyLoaded = true;
        m_dialogError.clear();
      } else
        m_dialogError = "Your server privacy settings could not be loaded. Try again.";
    } else
      closeDialog();
    m_window.invalidate();
    return true;
  }

  void ClientApplication::openServerProfile(const std::string& guild) {
    if (std::ranges::find(m_guilds, guild, &api::Guild::id) == m_guilds.end())
      return;
    if (m_userAccount.busy || !m_userAccount.draft.empty() || m_userAccount.crop) {
      m_userAccount.error = "Save or reset your current profile changes before opening another server profile.";
      m_status = m_userAccount.error;
      m_window.invalidate();
      return;
    }
    if (m_serverSettings.open) {
      closeServerSettings();
      if (m_serverSettings.open)
        return;
    }
    closeContext();
    closeDialog();
    ++m_userAccount.generation;
    m_userAccount.pending.clear();
    m_userAccount.loading.clear();
    m_userAccount.profile = nlohmann::json::object();
    m_userAccount.guild = guild;
    ++m_dialogGeneration;
    openSettings();
    if (m_settingsOpen) {
      selectSettingsPage("profile");
      loadUserAccountSettings("profile", true);
    }
    m_window.invalidate();
  }

  void ClientApplication::drawServerMenuDialog(Node& root, float w, float h) {
    if (!serverDialog(m_dialogKind))
      return;
    const bool privacy = m_dialogKind == "server-privacy", notifications = m_dialogKind == "server-notifications",
               leaving = m_dialogKind == "server-leave", folder = m_dialogKind == "server-folder",
               folderSettings = m_dialogKind == "server-folder-settings";
    const auto guild = std::ranges::find(m_guilds, m_serverDialogGuild, &api::Guild::id);
    const std::string guildName = guild == m_guilds.end() ? "Unavailable server" : guild->name;
    const float width = std::min(500.0F, w - Style::space2Xl), inner = width - 40;
    struct Row {
      enum Kind { Heading, Paragraph, Toggle, Radio, Field, ColorField } kind;
      std::string title, description, id;
      bool checked = false;
      float height = 0;
    };
    std::vector<Row> rows;
    const auto toggle = [&](const std::string& title, const std::string& description, const std::string& id,
                            bool checked) { rows.push_back({Row::Toggle, title, description, id, checked}); };
    const auto radio = [&](const std::string& title, const std::string& id, bool checked) {
      rows.push_back({Row::Radio, title, {}, id, checked});
    };
    if (notifications) {
      const auto& draft = m_serverNotificationDraft;
      toggle("Mute server", "Suppress notifications from this server.", "mute", draft.muted.value_or(false));
      rows.push_back({Row::Heading, "Server notifications"});
      radio("All messages", "level:0", draft.level.value_or(3) == 0);
      radio("Only @mentions", "level:1", draft.level.value_or(3) == 1);
      radio("Nothing", "level:2", draft.level.value_or(3) == 2);
      rows.push_back({Row::Heading, "Notification preferences"});
      toggle(
          "Suppress @everyone and @here", "Ignore these mentions from this server.", "suppress-everyone",
          draft.suppressEveryone.value_or(false)
      );
      toggle(
          "Suppress all role mentions", "Ignore mentions of roles you belong to in this server.", "suppress-roles",
          draft.suppressRoles.value_or(false)
      );
      toggle(
          "Mobile push notifications", "Send notifications from this server to your mobile devices.", "mobile-push",
          draft.mobilePush.value_or(true)
      );
      toggle(
          "Mute scheduled events", "Suppress notifications about scheduled events in this server.", "mute-events",
          draft.muteScheduledEvents.value_or(false)
      );
      rows.push_back({Row::Heading, "Highlights"});
      radio("Use default", "highlights:0", draft.notifyHighlights.value_or(0) == 0);
      radio("Enabled", "highlights:2", draft.notifyHighlights.value_or(0) == 2);
      radio("Disabled", "highlights:1", draft.notifyHighlights.value_or(0) == 1);
    } else if (privacy && m_serverPrivacyLoaded) {
      const auto& draft = m_serverPrivacyDraft;
      toggle(
          "Direct messages", "Allow direct messages from members of this server.", "direct-messages",
          draft.directMessages.value_or(true)
      );
      toggle(
          "Message requests", "Allow new conversations from server members to appear in Message Requests.",
          "message-requests", draft.messageRequests.value_or(true)
      );
      toggle(
          "Activity status", "Share your activity with members of this server.", "activity",
          draft.activity.value_or(true)
      );
      toggle(
          "Activity joining", "Allow server members to join activities that support joining.", "activity-joining",
          draft.activityJoining.value_or(true)
      );
    } else if (privacy)
      rows.push_back(
          {Row::Paragraph,
           m_dialogPending ? "Loading your server privacy settings…"
                           : "Your current privacy settings must be loaded before editing."}
      );
    else if (leaving)
      rows.push_back(
          {Row::Paragraph,
           "Are you sure you want to leave " + guildName + "? You will need another invitation to rejoin this server."}
      );
    else if (folderSettings) {
      rows.push_back({Row::Field, "Folder name"});
      rows.push_back({Row::ColorField, "Folder color"});
    } else {
      rows.push_back(
          {Row::Paragraph, "Create a folder for " + guildName + ". You can move other servers into it later."}
      );
      rows.push_back({Row::Field, "Folder name"});
    }
    auto& renderer = m_window.target().renderer();
    const auto measured = [&](const std::string& value, float size, FontWeight weight, float space) {
      if (value.empty())
        return 0.0F;
      const auto metrics = renderer.measureText(value, size, weight, std::max(1.0F, space), 0);
      return std::max(size, metrics.bottom - metrics.top);
    };
    float contentHeight = 0;
    for (auto& row : rows) {
      if (row.kind == Row::Heading)
        row.height = 30;
      else if (row.kind == Row::Radio)
        row.height = 28;
      else if (row.kind == Row::ColorField)
        row.height = Style::spaceXl + SettingsColorPicker::intrinsicColumnHeight(inner, 1) + Style::spaceSm;
      else if (row.kind == Row::Field)
        row.height = 64;
      else if (row.kind == Row::Toggle)
        row.height =
            std::max(
                30.0F,
                measured(row.title, 13, FontWeight::SemiBold, inner - (Style::spaceSection + Style::spaceXs))
                    + 4
                    + measured(row.description, 12, FontWeight::Normal, inner - (Style::spaceSection + Style::spaceXs))
            )
            + 16;
      else
        row.height = measured(row.title, 12, FontWeight::Normal, inner) + 18;
      contentHeight += row.height;
    }
    const float errorHeight = m_dialogError.empty() ? 0 : measured(m_dialogError, 12, FontWeight::Normal, inner) + 12;
    const float height = std::min(contentHeight + errorHeight + 136, std::max(160.0F, h - 70));
    const float x = (w - width) / 2, y = std::max(42.0F, (h - height + 36) / 2);
    const float bodyTop = y + 72, viewport = std::max(12.0F, height - 136 - errorHeight);
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, Color{0, 0, 0, .65F});
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::None, {}});
    auto surface = std::make_unique<RectNode>();
    surface->setPosition(x, y);
    surface->setFrameSize(width, height);
    surface->setStyle({.fill = background, .border = raised, .radius = Style::scaledRadiusXl(), .borderWidth = 1});
    root.addChild(std::move(surface));
    m_dialogBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    label(
        root, x + Style::spaceRelaxed, y + (Style::spaceLg + Style::spaceXxs),
        inner - (Style::space2Xl - Style::spaceXxs), m_dialogTitle, Style::fontSizeDialog, foreground,
        FontWeight::SemiBold
    );
    label(
        root, x + Style::spaceRelaxed, y + ((Style::spaceSection + Style::spaceXs) + Style::borderWidth), inner,
        guildName, Style::fontSizeDescription, muted
    );
    iconButton(root, {x + width - 40, y + Style::cardPadding, 26, 26, Action::CloseDialog, {}}, "x", "Close");
    auto clip = std::make_unique<Node>();
    clip->setPosition(x + Style::spaceRelaxed, bodyTop);
    clip->setSize(inner, viewport);
    clip->setClipChildren(true);
    auto body = std::make_unique<Node>();
    m_scrolling.limit(m_serverDialogScroll, std::max(0.0F, contentHeight - viewport));
    clip->setTextScrollOffset(-m_serverDialogScroll);
    body->setPosition(0, -m_serverDialogScroll);
    auto& page = *body;
    const auto firstHit = m_hits.size();
    float cursor = 0;
    for (const auto& row : rows) {
      if (row.kind == Row::Heading)
        label(
            page, 0, cursor + Style::spaceSm, inner, row.title, Style::fontSizeDescription, muted, FontWeight::SemiBold
        );
      else if (row.kind == Row::Radio) {
        Hit hit{
            x + Style::spaceRelaxed,
            bodyTop + cursor - m_serverDialogScroll,
            inner,
            28,
            m_dialogPending ? Action::None : Action::ServerMenuDialogAction,
            row.id
        };
        if (hit.contains(m_pointerX, m_pointerY))
          rectangle(page, 0, cursor, inner, 28, sidebar, Style::controlRadius);
        glyph(
            page, 3, cursor + (Style::spaceXs + Style::borderWidth), 17, row.checked ? "circle-dot" : "circle",
            row.checked ? accent : muted
        );
        label(
            page, 29, cursor + Style::spaceCompact, inner - Style::space2Xl, row.title, Style::fontSizeDescription,
            foreground
        );
        m_hits.push_back(std::move(hit));
      } else if (row.kind == Row::Toggle) {
        const float titleHeight = label(
            page, 0, cursor + Style::borderWidth, inner - (Style::spaceSection + Style::spaceXs), row.title,
            Style::fontSizeCaption, foreground, FontWeight::SemiBold, 0
        );
        label(
            page, 0, cursor + titleHeight + (Style::spaceXs + Style::borderWidth),
            inner - (Style::spaceSection + Style::spaceXs), row.description, Style::fontSizeDescription, muted,
            FontWeight::Normal, 0
        );
        const std::string key = "toggle:server-menu:" + row.id;
        std::unique_ptr<Node> saved;
        if (auto old = m_detachedInputs.find(key); old != m_detachedInputs.end()) {
          saved = std::move(old->second);
          m_detachedInputs.erase(old);
        }
        auto control =
            saved ? std::unique_ptr<Toggle>(static_cast<Toggle*>(saved.release())) : std::make_unique<Toggle>();
        m_widgets[key] = control.get();
        control->setToggleSize(ToggleSize::Small);
        control->setFrameSize(30, 18);
        control->setPosition(inner - (Style::space2Xl - Style::spaceXxs), cursor + Style::borderWidth);
        control->setCheckedImmediate(row.checked);
        control->setEnabled(!m_dialogPending);
        control->setOnChange([this, id = row.id](bool) { serverMenuDialogAction(id); });
        page.addChild(std::move(control));
      } else if (row.kind == Row::ColorField) {
        label(page, 0, cursor, inner, row.title, Style::fontSizeCaption, foreground, FontWeight::SemiBold);
        const std::string key = "server-menu:folder-picker";
        std::unique_ptr<Node> saved;
        if (auto old = m_detachedInputs.find(key); old != m_detachedInputs.end()) {
          saved = std::move(old->second);
          m_detachedInputs.erase(old);
        }
        auto picker = saved ? std::unique_ptr<SettingsColorPicker>(static_cast<SettingsColorPicker*>(saved.release()))
                            : std::make_unique<SettingsColorPicker>();
        m_widgets[key] = picker.get();
        picker->setPickerWidth(inner);
        picker->setPosition(0, cursor + Style::spaceXl);
        Color current;
        if (tryParseHexColor(m_serverFolderColor, current))
          picker->setColor(current);
        picker->setEnabled(!m_dialogPending);
        const auto bind = [&](Input* input, const std::string& id) {
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
        bind(picker->hexInput(), "server-menu:folder-color");
        const auto rgb = picker->rgbInputs();
        for (std::size_t i = 0; i < rgb.size(); ++i)
          bind(rgb[i], "server-menu:folder-color:" + std::to_string(i));
        picker->setOnColorChanged([this](const Color& color) {
          const auto byte = [](float value) {
            return static_cast<unsigned>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255));
          };
          const auto packed = (byte(color.r) << 16) | (byte(color.g) << 8) | byte(color.b);
          std::ostringstream text;
          text << '#' << std::hex << std::setw(6) << std::setfill('0') << packed;
          m_serverFolderColor = text.str();
          m_dialogError.clear();
          m_window.invalidate();
        });
        page.addChild(std::move(picker));
      } else if (row.kind == Row::Field) {
        label(page, 0, cursor, inner, row.title, Style::fontSizeCaption, foreground, FontWeight::SemiBold);
        auto& input = inputControl(
            page, 0, cursor + Style::spaceXl, inner, Style::space2Xl, "dialog", m_dialogText, "Folder name"
        );
        input.setEnabled(!m_dialogPending);
      } else
        label(page, 0, cursor, inner, row.title, Style::fontSizeDescription, foreground, FontWeight::Normal, 0);
      cursor += row.height;
    }
    for (std::size_t i = firstHit; i < m_hits.size(); ++i) {
      auto& hit = m_hits[i];
      const float top = std::max(bodyTop, hit.y), bottom = std::min(bodyTop + viewport, hit.y + hit.height);
      hit.y = top;
      hit.height = std::max(0.0F, bottom - top);
    }
    clip->addChild(std::move(body));
    root.addChild(std::move(clip));
    settingsScrollbar(
        root, "server-menu:scrollbar", {x + width - 13, bodyTop, 6, viewport, Action::None, {}}, contentHeight,
        m_serverDialogScroll
    );
    if (!m_dialogError.empty())
      label(
          root, x + Style::spaceRelaxed, y + height - 57 - errorHeight, inner, m_dialogError,
          Style::fontSizeDescription, danger, FontWeight::Normal, 0
      );
    const auto actionButton = [&](Hit hit, const std::string& text, bool primary, bool enabled) {
      const bool hover = enabled && hit.contains(m_pointerX, m_pointerY);
      const Color fill = primary ? (leaving ? destructive : accent) : sidebar;
      auto shape = std::make_unique<RectNode>();
      shape->setPosition(hit.x, hit.y);
      shape->setSize(hit.width, hit.height);
      shape->setStyle(
          {.fill = !enabled ? lerpColor(background, fill, .4F)
               : hover      ? lerpColor(fill, foreground, .1F)
                            : fill,
           .border = primary ? fill : raised,
           .radius = Style::scaledRadius(Style::controlRadius),
           .borderWidth = primary ? 0.0F : 1.0F}
      );
      root.addChild(std::move(shape));
      const auto metrics = renderer.measureText(text, Style::fontSizeDescription, FontWeight::Normal);
      label(
          root, hit.x + (hit.width - metrics.width) / 2, hit.y + (hit.height - (metrics.bottom - metrics.top)) / 2,
          metrics.width + Style::spaceXxs, text, Style::fontSizeDescription,
          !enabled      ? muted
              : primary ? (leaving ? rgbHex(0xFFFFFF) : ink)
                        : foreground
      );
      if (!enabled)
        hit.action = Action::None;
      m_hits.push_back(std::move(hit));
    };
    const float buttonWidth = (inner - Style::spaceSm) / 2, buttonY = y + height - 44;
    actionButton({x + Style::spaceRelaxed, buttonY, buttonWidth, 26, Action::CloseDialog, {}}, "Cancel", false, true);
    const bool retry = privacy && !m_serverPrivacyLoaded && !m_dialogPending;
    const bool valid = guild != m_guilds.end()
        && !m_dialogPending
        && (!privacy || m_serverPrivacyLoaded || retry)
        && (!leaving || m_roleDirectory.owner(m_serverDialogGuild) != m_account.id)
        && (!folder || m_dialogText.find_first_not_of(" \t\r\n") != std::string::npos);
    actionButton(
        {x + Style::spaceWide + buttonWidth, buttonY, buttonWidth, 26,
         retry ? Action::ServerMenuDialogAction : Action::SubmitDialog, retry ? "privacy-retry" : ""},
        m_dialogPending ? (privacy && !m_serverPrivacyLoaded ? "Loading…" : "Saving…")
            : retry     ? "Retry"
            : leaving   ? "Leave Server"
            : folder    ? "Create Folder"
                        : "Save",
        true, valid
    );
    m_tooltip.clear();
  }
} // namespace app
