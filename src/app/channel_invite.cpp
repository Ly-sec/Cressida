#include "app/client_application.h"
#include "app/theme.h"
#include "render/scene/rect_node.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glib.h>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;
  namespace {
    struct InviteOption {
      int value;
      const char* label;
    };
    constexpr std::array ageOptions{InviteOption{1800, "30 minutes"}, InviteOption{3600, "1 hour"},
                                    InviteOption{21600, "6 hours"},   InviteOption{43200, "12 hours"},
                                    InviteOption{86400, "1 day"},     InviteOption{604800, "7 days"},
                                    InviteOption{0, "Never"}};
    constexpr std::array useOptions{InviteOption{0, "No limit"},  InviteOption{1, "1 use"},
                                    InviteOption{5, "5 uses"},    InviteOption{10, "10 uses"},
                                    InviteOption{25, "25 uses"},  InviteOption{50, "50 uses"},
                                    InviteOption{100, "100 uses"}};
    std::string optionLabel(const auto& options, int value) {
      const auto at = std::ranges::find(options, value, &InviteOption::value);
      return at == options.end() ? "Select" : at->label;
    }
    bool inviteMatches(const std::string& value, const std::string& query) {
      auto* a = g_utf8_casefold(value.c_str(), value.size());
      auto* b = g_utf8_casefold(query.c_str(), query.size());
      const bool result = a && b && std::string_view(a).contains(b);
      g_free(a);
      g_free(b);
      return result;
    }
    void
    inviteSurface(Node& parent, float x, float y, float width, float height, Color fill, Color border, float radius) {
      auto shape = std::make_unique<RectNode>();
      shape->setPosition(x, y);
      shape->setFrameSize(width, height);
      shape->setStyle({.fill = fill, .border = border, .radius = Style::scaledRadius(radius), .borderWidth = 1});
      parent.addChild(std::move(shape));
    }
    std::string recipientKey(const api::Interaction& action) {
      return action.channel.empty() ? "user:" + action.user : "channel:" + action.channel;
    }
  } // namespace

  void ClientApplication::openChannelInvite(const api::Interaction& source) {
    closeDialog();
    m_channelInvite = {};
    m_channelInvite.generation = m_dialogGeneration;
    if (const auto cached = m_channelInviteSettings.find(source.guild + ":" + source.channel);
        cached != m_channelInviteSettings.end()) {
      m_channelInvite.maxAge = cached->second.inviteMaxAge;
      m_channelInvite.maxUses = cached->second.inviteMaxUses;
      m_channelInvite.temporary = cached->second.inviteTemporary;
    }
    if (m_channelInvite.maxAge == 0) {
      const auto guild = std::ranges::find(m_guilds, source.guild, &api::Guild::id);
      if (guild == m_guilds.end() || std::ranges::find(guild->features, "COMMUNITY") == guild->features.end())
        m_channelInvite.maxAge = 604800;
    }
    m_dialogKind = "sidebar-invite";
    m_dialogTitle = "Invite friends";
    m_dialogAction = source;
    m_dialogText.clear();
    m_dialogError.clear();
    m_sidebarInviteUrl.clear();
    m_focusedInput = "dialog";
    std::set<std::string> users;
    const auto addUser = [&](const api::Account& user, const std::string& channel) {
      if (user.id.empty()
          || user.id == m_account.id
          || m_userDirectory.relationship(user.id) == 2
          || !users.insert(user.id).second
          || m_channelInvite.recipients.size() >= 200)
        return;
      m_channelInvite.recipients.push_back(
          {channel.empty() ? "user:" + user.id : "channel:" + channel, channel, user.id, userName(user.id, user.name),
           user.username, user.avatar}
      );
    };
    for (const auto& channel : m_privateChannels) {
      if (channel.type == 1) {
        for (const auto& user : channel.recipients)
          addUser(user, channel.id);
      } else if (channel.type == 3 && m_channelInvite.recipients.size() < 200)
        m_channelInvite.recipients.push_back({"channel:" + channel.id, channel.id, {}, channel.name, {}, channel.icon});
    }
    for (const auto& member : m_members)
      if (m_userDirectory.relationship(member.user.id) == 1)
        addUser(member.user, {});
    for (const auto& message : m_messages)
      if (m_userDirectory.relationship(message.authorId) == 1)
        addUser(lookupUser(message.authorId), {});
    for (std::size_t i = 0; i < m_channelInvite.recipients.size(); ++i)
      m_channelInvite.matches.push_back(i);
    generateChannelInvite();
  }

  void ClientApplication::generateChannelInvite() {
    if (m_dialogKind != "sidebar-invite" || !m_dialogAction || m_dialogPending)
      return;
    const auto* channel = serverChannel(m_dialogAction->guild, m_dialogAction->channel);
    const auto permissions = serverChannelPermissions(m_dialogAction->guild, m_dialogAction->channel);
    if (!channel || !permissions || !api::permission::allows(permissions, api::permission::createInstantInvite)) {
      m_dialogError = !channel
          ? "This server's channels are not available yet. Reopen the invite after they finish loading."
          : !permissions
          ? "Server permissions are not available yet. Reopen the invite after the server finishes loading."
          : "You no longer have permission to invite people to this channel.";
      m_window.invalidate();
      return;
    }
    auto action = *m_dialogAction;
    action.kind = api::InteractionKind::CreateInvite;
    action.revision = m_channelInvite.generation;
    action.inviteMaxAge = m_channelInvite.editing ? m_channelInvite.draftMaxAge : m_channelInvite.maxAge;
    action.inviteMaxUses = m_channelInvite.editing ? m_channelInvite.draftMaxUses : m_channelInvite.maxUses;
    action.inviteTemporary = m_channelInvite.editing ? m_channelInvite.draftTemporary : m_channelInvite.temporary;
    action.inviteUnique = m_channelInvite.editing;
    if (action.inviteMaxAge == 0) {
      const auto guild = std::ranges::find(m_guilds, action.guild, &api::Guild::id);
      if (guild == m_guilds.end() || std::ranges::find(guild->features, "COMMUNITY") == guild->features.end()) {
        m_dialogError = "Only Community servers support invite links that never expire.";
        m_window.invalidate();
        return;
      }
    }
    if (!api::interactionRequest(action)) {
      m_dialogError = "Choose valid expiry and usage limits for this invite.";
      m_window.invalidate();
      return;
    }
    if (m_demo || startInteraction(action)) {
      m_dialogAction = action;
      m_dialogPending = true;
      m_dialogError.clear();
      m_channelInvite.dropdown = 0;
    } else
      m_dialogError = m_status;
    m_window.invalidate();
  }

  bool ClientApplication::channelInviteAction(const Hit& hit) {
    if (m_dialogKind != "sidebar-invite")
      return false;
    auto& state = m_channelInvite;
    switch (hit.action) {
    case Action::InviteEditLink:
      if (!m_dialogPending) {
        state.draftMaxAge = state.maxAge;
        state.draftMaxUses = state.maxUses;
        state.draftTemporary = state.temporary;
        state.editing = true;
        state.dropdown = 0;
        m_scrolling.stop(state.formScroll);
        state.formScroll = 0;
        m_dialogError.clear();
        m_focusedInput.clear();
        m_controlInput.setFocus(nullptr);
      }
      break;
    case Action::InviteBack:
      state.editing = false;
      state.dropdown = 0;
      m_dialogError.clear();
      m_focusedInput = "dialog";
      break;
    case Action::InviteGenerate:
      generateChannelInvite();
      break;
    case Action::SidebarCopyInvite:
    case Action::InviteCopy:
      if (!m_sidebarInviteUrl.empty()) {
        m_window.copy(m_sidebarInviteUrl);
        state.copied = true;
      }
      break;
    case Action::InviteTemporary:
      if (!m_dialogPending)
        state.draftTemporary = !state.draftTemporary;
      break;
    case Action::InviteDropdown:
      if (!m_dialogPending) {
        const int value = hit.id == "age" ? 1 : hit.id == "uses" ? 2 : 0;
        state.dropdown = value == state.dropdown ? 0 : value;
      }
      break;
    case Action::InviteOption:
      if (!m_dialogPending) {
        const auto colon = hit.id.find(':');
        if (colon != std::string::npos) {
          const auto& options = hit.id.starts_with("age:") ? ageOptions : useOptions;
          const auto at = std::ranges::find_if(options, [&](const auto& option) {
            return hit.id.substr(colon + 1) == std::to_string(option.value);
          });
          if (at != options.end()) {
            if (hit.id.starts_with("age:"))
              state.draftMaxAge = at->value;
            else
              state.draftMaxUses = at->value;
          }
        }
        state.dropdown = 0;
      }
      break;
    case Action::InviteRecipient: {
      if (m_sidebarInviteUrl.empty() || state.pending.contains(hit.id) || state.sent.contains(hit.id))
        break;
      const auto recipient = std::ranges::find(state.recipients, hit.id, &ChannelInviteRecipient::key);
      if (recipient == state.recipients.end()
          || (!recipient->user.empty() && m_userDirectory.relationship(recipient->user) == 2))
        break;
      api::Interaction action;
      action.kind = api::InteractionKind::SendInvite;
      action.channel = recipient->channel;
      action.user = recipient->channel.empty() ? recipient->user : "";
      action.guild = m_dialogAction ? m_dialogAction->guild : "";
      action.text = m_sidebarInviteUrl;
      action.revision = state.generation;
      if (m_demo || startInteraction(action)) {
        state.pending[hit.id] = action;
        state.errors.erase(hit.id);
      } else
        state.errors[hit.id] = m_status;
      break;
    }
    default:
      return false;
    }
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::channelInviteKey(const Key& key) {
    if (m_dialogKind != "sidebar-invite")
      return false;
    if (m_channelInvite.dropdown) {
      if (key.symbol == XKB_KEY_Escape || key.symbol == XKB_KEY_Tab) {
        m_channelInvite.dropdown = 0;
        m_window.invalidate();
        return key.symbol == XKB_KEY_Escape;
      }
      const auto widget = m_widgets.find("invite:options");
      if (widget != m_widgets.end()) {
        auto* menu = static_cast<ContextMenuControl*>(widget->second);
        if (key.symbol == XKB_KEY_Up || key.symbol == XKB_KEY_Down) {
          (void)menu->moveHighlight(key.symbol == XKB_KEY_Down ? 1 : -1);
          m_window.invalidate();
        } else if (key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter)
          (void)menu->activateHighlighted();
      }
      return !key.control || (key.symbol != XKB_KEY_q && key.symbol != XKB_KEY_Q);
    }
    if (key.symbol == XKB_KEY_Escape) {
      closeDialog();
      return true;
    }
    return false;
  }

  bool ClientApplication::channelInviteResult(const api::Event& event) {
    if (!event.interaction)
      return false;
    const auto& action = *event.interaction;
    const bool send = action.kind == api::InteractionKind::SendInvite;
    if (!send && action.kind != api::InteractionKind::CreateInvite)
      return false;
    if (m_dialogKind != "sidebar-invite" || action.revision != m_channelInvite.generation)
      return true;
    if (send) {
      const auto key = recipientKey(action);
      const auto pending = m_channelInvite.pending.find(key);
      if (pending == m_channelInvite.pending.end() || pending->second.text != action.text)
        return true;
      m_channelInvite.pending.erase(pending);
      if (event.error.empty()) {
        m_channelInvite.sent.insert(key);
        m_channelInvite.errors.erase(key);
      } else
        m_channelInvite.errors[key] = event.error;
    } else {
      if (!m_dialogAction || m_dialogAction->channel != action.channel || m_dialogAction->guild != action.guild)
        return true;
      m_dialogPending = false;
      m_dialogError = event.error;
      if (event.error.empty()) {
        m_sidebarInviteUrl = event.inviteUrl;
        if (m_channelInviteSettings.size() >= 256)
          m_channelInviteSettings.erase(m_channelInviteSettings.begin());
        m_channelInviteSettings[action.guild + ":" + action.channel] = action;
        m_channelInvite.maxAge = action.inviteMaxAge;
        m_channelInvite.maxUses = action.inviteMaxUses;
        m_channelInvite.temporary = action.inviteTemporary;
        m_channelInvite.copied = false;
        m_channelInvite.editing = false;
        m_channelInvite.dropdown = 0;
        m_focusedInput = "dialog";
      }
    }
    m_window.invalidate();
    return true;
  }

  void ClientApplication::drawChannelInvite(Node& root, float w, float h) {
    auto& state = m_channelInvite;
    const bool edit = state.editing;
    const float width = std::min(520.0F, w - Style::space2Xl);
    const float height = std::min(edit ? 408.0F + (m_dialogError.empty() ? 0 : 40) : 524.0F, h - 64);
    const float x = (w - width) / 2, y = 36 + (h - layout::Window::titlebarHeight - height) / 2;
    const float inner = width - 48;
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, Color{0, 0, 0, .65F});
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::None, {}});
    inviteSurface(root, x, y, width, height, background, raised, 18);
    m_dialogBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    const auto drawButton = [&](Node& parent, Hit hit, const std::string& caption, bool primary = false,
                                bool enabled = true, bool left = false) {
      float offsetX = 0, offsetY = 0;
      for (auto* p = &parent; p && p != m_scene.get(); p = p->parent()) {
        offsetX += p->x();
        offsetY += p->y();
      }
      const bool over = enabled && hit.contains(m_pointerX - offsetX, m_pointerY - offsetY);
      inviteSurface(
          parent, hit.x, hit.y, hit.width, hit.height,
          primary    ? enabled ? over ? lerpColor(accent, foreground, .12F) : accent : lerpColor(sidebar, accent, .35F)
              : over ? raised
                     : sidebar,
          primary ? Color{0, 0, 0, 0} : raised, 7
      );
      const auto metrics =
          m_window.target().renderer().measureText(caption, Style::fontSizeCaption, FontWeight::Medium);
      label(
          parent, hit.x + (left ? 12 : (hit.width - metrics.width) / 2),
          hit.y + (hit.height - (metrics.bottom - metrics.top)) / 2, metrics.width + Style::spaceXxs, caption,
          Style::fontSizeCaption,
          primary && enabled ? ink
              : enabled      ? foreground
                             : muted,
          FontWeight::Medium
      );
      hit.x += offsetX;
      hit.y += offsetY;
      if (!enabled)
        hit.action = Action::None;
      m_hits.push_back(std::move(hit));
    };
    std::string guildName = "your server", channelName;
    if (m_dialogAction) {
      if (const auto guild = std::ranges::find(m_guilds, m_dialogAction->guild, &api::Guild::id);
          guild != m_guilds.end())
        guildName = guild->name;
      if (const auto* channel = serverChannel(m_dialogAction->guild, m_dialogAction->channel))
        channelName = channel->name;
    }
    label(
        root, x + Style::spaceXl, y + ((Style::spaceXl - Style::spaceXxs) + Style::borderWidth), width - 82,
        edit ? "Server invite link settings" : "Invite friends to " + guildName, Style::fontSizeDialog, foreground,
        FontWeight::Medium
    );
    iconButton(
        root,
        {x + width - 46,
         y + ((Style::spaceLg + Style::spaceXxs) + Style::borderWidth),
         28,
         28,
         Action::CloseDialog,
         {}},
        "x", "Close"
    );
    if (!channelName.empty()) {
      glyph(root, x + Style::spaceXl, y + 56, 15, "hash", muted);
      label(
          root, x + ((Style::spaceSection + Style::spaceXs) + Style::borderWidth), y + 55,
          inner - (Style::spaceRelaxed + Style::borderWidth), channelName, Style::fontSizeDescription, muted
      );
    }
    if (edit) {
      const float selectWidth = inner;
      const float formTop = y + 87;
      const float viewport = std::max(36.0F, height - 171 - (m_dialogError.empty() ? 0 : 40));
      const std::string description = "Members are removed when they disconnect unless you assign them a role.";
      const auto titleMetrics = m_window.target().renderer().measureText(
          "Grant temporary membership", Style::fontSizeBody, FontWeight::Medium, inner - Style::spaceGroup, 2
      );
      const auto descriptionMetrics = m_window.target().renderer().measureText(
          description, Style::fontSizeDescription, FontWeight::Normal, inner - Style::spaceGroup, 3
      );
      const float descriptionTop = 171 + titleMetrics.bottom - titleMetrics.top + Style::spaceDense;
      const float content = descriptionTop + descriptionMetrics.bottom - descriptionMetrics.top + Style::spaceCompact;
      state.formBounds = {x + Style::spaceXl, formTop, inner, viewport};
      m_scrolling.limit(state.formScroll, std::max(0.0F, content - viewport));
      auto formClip = std::make_unique<Node>();
      formClip->setPosition(x + Style::spaceXl, formTop);
      formClip->setFrameSize(inner, viewport);
      formClip->setClipChildren(true);
      auto contentNode = std::make_unique<Node>();
      contentNode->setPosition(0, -state.formScroll);
      contentNode->setFrameSize(inner, content);
      auto* form = contentNode.get();
      formClip->addChild(std::move(contentNode));
      root.addChild(std::move(formClip));
      const auto clipTarget = [&] {
        auto& hit = m_hits.back();
        const float bottom = std::min(hit.y + hit.height, formTop + viewport);
        hit.y = std::max(hit.y, formTop);
        hit.height = std::max(0.0F, bottom - hit.y);
      };
      label(*form, 0, 0, selectWidth, "Expire After", Style::fontSizeCaption, foreground, FontWeight::Medium);
      label(*form, 0, 78, selectWidth, "Max Number of Uses", Style::fontSizeCaption, foreground, FontWeight::Medium);
      drawButton(
          *form, {0, 25, selectWidth, 36, Action::InviteDropdown, "age"}, optionLabel(ageOptions, state.draftMaxAge),
          false, !m_dialogPending, true
      );
      clipTarget();
      drawButton(
          *form, {0, 103, selectWidth, 36, Action::InviteDropdown, "uses"}, optionLabel(useOptions, state.draftMaxUses),
          false, !m_dialogPending, true
      );
      clipTarget();
      glyph(*form, selectWidth - Style::spaceXl, 36, Style::glyphSizeSmall, "chevron-down", muted);
      glyph(*form, selectWidth - Style::spaceXl, 114, Style::glyphSizeSmall, "chevron-down", muted);
      label(
          *form, 0, 171, inner - Style::spaceGroup, "Grant temporary membership", Style::fontSizeBody, foreground,
          FontWeight::Medium, 2
      );
      label(
          *form, 0, descriptionTop, inner - Style::spaceGroup, description, Style::fontSizeDescription, muted,
          FontWeight::Normal, 3
      );
      const std::string toggleId = "invite:temporary";
      std::unique_ptr<Node> saved;
      if (auto at = m_detachedInputs.find(toggleId); at != m_detachedInputs.end()) {
        saved = std::move(at->second);
        m_detachedInputs.erase(at);
      }
      auto toggle = saved ? std::unique_ptr<Toggle>(static_cast<Toggle*>(saved.release())) : std::make_unique<Toggle>();
      m_widgets[toggleId] = toggle.get();
      toggle->setToggleSize(ToggleSize::Small);
      toggle->setCheckedImmediate(state.draftTemporary);
      toggle->setEnabled(!m_dialogPending);
      toggle->setPosition(inner - (content > viewport ? 42 : 30), 173);
      toggle->setFrameSize(30, 18);
      toggle->setOnChange([this](bool) { channelInviteAction({0, 0, 0, 0, Action::InviteTemporary, {}}); });
      form->addChild(std::move(toggle));
      if (content > viewport)
        settingsScrollbar(
            root, "invite:form-scrollbar", {x + width - 30, formTop, 6, viewport, Action::None, {}}, content,
            state.formScroll
        );
      if (!m_dialogError.empty())
        label(
            root, x + Style::spaceXl, y + height - 111, inner, m_dialogError, Style::fontSizeDescription, danger,
            FontWeight::Normal, 2
        );
      const float buttonWidth = (inner - Style::spaceDense) / 2;
      drawButton(
          root, {x + Style::spaceXl, y + height - 60, buttonWidth, 36, Action::InviteBack, {}}, "Cancel", false,
          !m_dialogPending
      );
      drawButton(
          root,
          {x + (Style::space2Xl + Style::spaceXxs) + buttonWidth,
           y + height - 60,
           buttonWidth,
           36,
           Action::InviteGenerate,
           {}},
          m_dialogPending ? "Generating…" : "Generate a New Link", true, !m_dialogPending
      );
      if (state.dropdown) {
        const bool age = state.dropdown == 1;
        const auto& options = age ? ageOptions : useOptions;
        const auto guild = std::ranges::find(m_guilds, m_dialogAction->guild, &api::Guild::id);
        const bool community =
            guild != m_guilds.end() && std::ranges::find(guild->features, "COMMUNITY") != guild->features.end();
        std::vector<ContextMenuControlEntry> entries;
        for (const auto& option : options) {
          if (age && option.value == 0 && !community)
            continue;
          entries.push_back(
              {.id = option.value,
               .label = option.label,
               .radio = true,
               .toggleState = (age ? state.draftMaxAge : state.draftMaxUses) == option.value ? 1 : 0}
          );
        }
        const float menuHeight = ContextMenuControl::preferredHeight(entries, 8);
        const float menuX = x + Style::spaceXl;
        const float anchorY = formTop + (age ? 25 : 103) - state.formScroll;
        const float menuY = std::clamp(
            anchorY + Style::spaceSection + menuHeight < h - Style::spaceMd ? anchorY + Style::spaceSection
                                                                            : anchorY - menuHeight - Style::spaceXs,
            44.0F, std::max(44.0F, h - menuHeight - Style::spaceMd)
        );
        state.dropdownBounds = {menuX, menuY, selectWidth, menuHeight};
        std::unique_ptr<Node> menu;
        const std::string id = "invite:options";
        if (auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
          menu = std::move(at->second);
          m_detachedInputs.erase(at);
        } else
          menu = std::make_unique<ContextMenuControl>();
        auto* control = static_cast<ContextMenuControl*>(menu.get());
        const auto highlighted = control->highlightedIndex();
        std::size_t selected = 0;
        for (std::size_t i = 0; i < entries.size(); ++i)
          if (entries[i].toggleState == 1)
            selected = i;
        m_widgets[id] = control;
        control->setPosition(menuX, menuY);
        control->setMenuWidth(selectWidth);
        control->setMaxVisible(8);
        control->setEntries(std::move(entries));
        control->setHighlightedIndex(state.renderedDropdown == state.dropdown ? highlighted : selected);
        state.renderedDropdown = state.dropdown;
        control->setOnActivate([this, age](const ContextMenuControlEntry& entry) {
          channelInviteAction(
              {0, 0, 0, 0, Action::InviteOption, std::string(age ? "age:" : "uses:") + std::to_string(entry.id)}
          );
        });
        control->setRedrawCallback([this] { m_window.invalidate(); });
        m_hits.push_back({menuX, menuY, selectWidth, menuHeight, Action::None, {}});
        inviteSurface(root, menuX, menuY, selectWidth, menuHeight, panel, raised, 7);
        root.addChild(std::move(menu));
      }
    } else {
      if (state.query != m_dialogText) {
        state.query = m_dialogText;
        state.matches.clear();
        for (std::size_t i = 0; i < state.recipients.size(); ++i) {
          const auto& recipient = state.recipients[i];
          if (state.query.empty()
              || inviteMatches(recipient.name, state.query)
              || inviteMatches(recipient.username, state.query))
            state.matches.push_back(i);
        }
        m_scrolling.stop(state.scroll);
        state.scroll = 0;
      }
      inviteSurface(root, x + Style::spaceXl, y + 87, inner, 36, sidebar, raised, 7);
      auto& search = inputControl(
          root, x + Style::spaceXl, y + 87, inner - Style::space2Xl, 36, "dialog", m_dialogText, "Search for friends"
      );
      search.setFrameVisible(false);
      search.setHorizontalPadding(12);
      search.setFontSize(Style::fontSizeCaption);
      search.setEnabled(true);
      glyph(root, x + width - 50, y + 97, Style::baseGlyphSize, "search", muted);
      const float listTop = y + 137, listHeight = std::max(48.0F, height - 288);
      state.listBounds = {x + Style::spaceXl, listTop, inner, listHeight};
      const float contentHeight = state.matches.size() * 48.0F;
      m_scrolling.limit(state.scroll, std::max(0.0F, contentHeight - listHeight));
      auto clip = std::make_unique<Node>();
      clip->setPosition(x + Style::spaceXl, listTop);
      clip->setFrameSize(inner, listHeight);
      clip->setClipChildren(true);
      auto* list = clip.get();
      root.addChild(std::move(clip));
      if (state.matches.empty()) {
        label(
            *list, 8, 24, inner - Style::spaceLg,
            state.query.empty() ? "No recent conversations to invite." : "No friends found.", Style::fontSizeCaption,
            muted
        );
      } else {
        for (std::size_t row = 0; row < state.matches.size(); ++row) {
          const float rowY = row * 48.0F - state.scroll;
          if (rowY + Style::spaceGroup < 0 || rowY > listHeight)
            continue;
          const auto& recipient = state.recipients[state.matches[row]];
          const bool pending = state.pending.contains(recipient.key), sent = state.sent.contains(recipient.key);
          const bool failed = state.errors.contains(recipient.key);
          const float rowWidth = inner - Style::spaceMd;
          const bool over =
              ChannelInviteBounds{x + Style::spaceXl, listTop + rowY, rowWidth, 46}.contains(m_pointerX, m_pointerY);
          if (over)
            rectangle(*list, 0, rowY, rowWidth, 46, sidebar, Style::radiusMd);
          avatar(
              *list, 4, rowY + (Style::spaceCompact + Style::borderWidth), layout::Avatar::small, recipient.avatar,
              recipient.name, recipient.user
          );
          if (!recipient.user.empty())
            statusBadge(
                *list, 27, rowY + (Style::spaceWide + Style::borderWidth), userStatus(recipient.user),
                over ? sidebar : background
            );
          label(
              *list, 47, rowY + (failed || !recipient.username.empty() ? 6 : 14), rowWidth - 141, recipient.name,
              Style::fontSizeCaption, foreground, FontWeight::Medium
          );
          if (failed || !recipient.username.empty())
            label(
                *list, 47, rowY + (Style::spaceXl + Style::spaceXxs), rowWidth - 141,
                failed ? "Couldn't send invite. Try again." : recipient.username, Style::fontSizeMini,
                failed ? danger : muted
            );
          const Hit hit{x + Style::spaceXl + rowWidth - 82,
                        listTop + rowY + Style::spaceSm,
                        82,
                        30,
                        Action::InviteRecipient,
                        recipient.key};
          // Draw in local coordinates but clip the manual target to the same visible row.
          drawButton(
              *list, {rowWidth - 82, rowY + Style::spaceSm, 82, 30, hit.action, hit.id},
              pending      ? "Sending…"
                  : sent   ? "Sent"
                  : failed ? "Retry"
                           : "Invite",
              false,
              !m_sidebarInviteUrl.empty()
                  && !pending
                  && !sent
                  && (recipient.user.empty() || m_userDirectory.relationship(recipient.user) != 2)
          );
          auto& target = m_hits.back();
          const float bottom = std::min(target.y + target.height, listTop + listHeight);
          target.y = std::max(target.y, listTop);
          target.height = std::max(0.0F, bottom - target.y);
        }
      }
      if (contentHeight > listHeight)
        settingsScrollbar(
            root, "invite:scrollbar", {x + width - 30, listTop, 6, listHeight, Action::None, {}}, contentHeight,
            state.scroll
        );
      const float footerTop = y + height - 136;
      label(
          root, x + Style::spaceXl, footerTop, inner, "Or, send a server invite link to a friend",
          Style::fontSizeCaption, foreground, FontWeight::Medium
      );
      inviteSurface(root, x + Style::spaceXl, footerTop + Style::spaceWide, inner - 92, 36, sidebar, raised, 7);
      label(
          root, x + (Style::space2Xl + Style::spaceXs), footerTop + (Style::spaceSection - Style::spaceXxs),
          inner - 116,
          !m_sidebarInviteUrl.empty() ? m_sidebarInviteUrl
              : m_dialogPending       ? "Generating invite link…"
                                      : "Invite link unavailable",
          Style::fontSizeDescription, m_sidebarInviteUrl.empty() ? muted : foreground
      );
      drawButton(
          root, {x + width - 108, footerTop + Style::spaceWide, 84, 36, Action::InviteCopy, {}},
          state.copied ? "Copied" : "Copy", true, !m_sidebarInviteUrl.empty()
      );
      const std::string expiry = state.maxAge == 0
          ? "This invite link never expires."
          : "This invite link expires in " + optionLabel(ageOptions, state.maxAge) + ".";
      label(
          root, x + Style::spaceXl, footerTop + 76, inner - 126, expiry, Style::fontSizeMini, muted, FontWeight::Normal,
          2
      );
      const Hit editHit{
          x + width - 142, footerTop + 71, 118, 25, m_dialogPending ? Action::None : Action::InviteEditLink, {}
      };
      label(
          root, editHit.x, editHit.y + (Style::spaceXs + Style::borderWidth), editHit.width, "Edit invite link",
          Style::fontSizeDescription, m_dialogPending ? muted : accent, FontWeight::Medium
      );
      m_hits.push_back(editHit);
      if (!m_dialogError.empty()) {
        label(root, x + Style::spaceXl, footerTop + 101, inner - 84, m_dialogError, Style::fontSizeMini, danger);
        const Hit retry{x + width - 86, footerTop + 98, 62, 23, Action::InviteGenerate, {}};
        label(
            root, retry.x, retry.y + Style::spaceXs, retry.width, "Retry", Style::fontSizeDescription, accent,
            FontWeight::Medium
        );
        m_hits.push_back(retry);
      }
    }
    m_tooltip.clear();
  }

  void ClientApplication::prepareChannelInviteDemo(const std::string& view) {
    prepareChannelMenuDemo(view);
    m_privateChannels.clear();
    api::UserUpdates users;
    const std::array names{"Aditya", "anchoby", "Lysec", "Sample", "Zero", "Yocraft", "ne", "Preacher of Decaratology"};
    for (std::size_t i = 0; i < names.size(); ++i) {
      api::Account user{std::to_string(600 + i), names[i]};
      user.username = std::array{"adityakr15",         "anchovyaddict", "lysec",   "_sample.7",
                                 "zero_likes_nothing", "yocraft_2000",  "ne.yfua", "preacher"}[i];
      api::Channel channel;
      channel.id = std::to_string(700 + i);
      channel.type = 1;
      channel.name = user.name;
      channel.recipients = {user};
      m_privateChannels.push_back(channel);
      users.users[user.id].relationship = 1;
      users.users[user.id].status = i % 3 == 0 ? "online" : i % 3 == 1 ? "idle" : "offline";
    }
    m_userDirectory.apply(users);
    auto guild = std::ranges::find(m_guilds, m_guildId, &api::Guild::id);
    if (guild != m_guilds.end()) {
      guild->name = "Noctalia";
      guild->features.push_back("COMMUNITY");
    }
    api::Interaction action;
    action.kind = api::InteractionKind::CreateInvite;
    action.channel = "103";
    action.guild = m_guildId;
    openChannelInvite(action);
    if (view.find("loading") == std::string::npos) {
      auto request = *m_dialogAction;
      channelInviteResult(
          {.operation = api::Operation::Interaction, .interaction = request, .inviteUrl = "https://discord.gg/noctalia"}
      );
    }
    if (view.find("settings") != std::string::npos)
      channelInviteAction({0, 0, 0, 0, Action::InviteEditLink, {}});
    if (view.find("expiry") != std::string::npos)
      channelInviteAction({0, 0, 0, 0, Action::InviteDropdown, "age"});
    if (view.find("uses") != std::string::npos)
      channelInviteAction({0, 0, 0, 0, Action::InviteDropdown, "uses"});
    if (view.find("search") != std::string::npos)
      m_dialogText = "ly";
    if (view.find("error") != std::string::npos)
      m_dialogError = "You no longer have permission to create invites.";
    if (view.find("light") != std::string::npos) {
      m_preferences.theme = "light";
    }
    if (view.find("narrow") != std::string::npos)
      m_window.setUiScale(1.8F);
  }

  void ClientApplication::demoChannelInviteStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto click = [&](Action action, const std::string& id = std::string{}) {
      const auto at = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == action && (id.empty() || hit.id == id);
      });
      require(at != m_hits.end(), "Expected invite action must be visible and enabled");
      m_window.motion(
          at->x + at->width / 2, at->y + at->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      m_window.click(
          at->x + at->width / 2, at->y + at->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          272
      );
      m_window.released();
    };
    const auto ack = [&](const api::Interaction& action, const std::string& error = std::string{}) {
      api::Event event;
      event.operation = api::Operation::Interaction;
      event.interaction = action;
      event.error = error;
      event.inviteUrl = "https://discord.gg/native-test";
      interactionResult(event);
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(
          !m_sidebarInviteUrl.empty() && m_channelInvite.recipients.size() == 8,
          "Invite opening generates a link and displays cached recipients"
      );
      click(Action::InviteEditLink);
    } else if (step == 1) {
      require(m_channelInvite.editing, "Edit invite link opens the settings panel");
      click(Action::InviteDropdown, "age");
    } else if (step == 2) {
      require(m_channelInvite.dropdown == 1, "Expiry uses a native options menu");
      m_window.key({.symbol = XKB_KEY_Up});
      m_window.key({.symbol = XKB_KEY_Return});
      require(
          m_channelInvite.draftMaxAge == 86400 && !m_channelInvite.dropdown,
          "Keyboard selection changes only the expiry draft"
      );
    } else if (step == 3)
      click(Action::InviteDropdown, "uses");
    else if (step == 4) {
      m_window.key({.symbol = XKB_KEY_Down});
      m_window.key({.symbol = XKB_KEY_Return});
      require(m_channelInvite.draftMaxUses == 1, "Usage dropdown supports keyboard selection");
    } else if (step == 5) {
      const auto toggle = m_widgets.find("invite:temporary");
      require(toggle != m_widgets.end(), "Temporary membership uses a retained native toggle");
      const float x = m_channelInvite.formBounds.x + toggle->second->x() + 15;
      const float y = m_channelInvite.formBounds.y
          + toggle->second->y()
          + 9
          - m_channelInvite.formScroll
          + m_window.titlebarHeight()
          - (Style::space2Xl + Style::spaceXs);
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.released();
    } else if (step == 6) {
      require(m_channelInvite.draftTemporary, "Clicking the temporary-membership switch updates its draft");
      click(Action::InviteGenerate);
      require(
          m_dialogPending
              && m_dialogAction->inviteMaxAge == 86400
              && m_dialogAction->inviteMaxUses == 1
              && m_dialogAction->inviteTemporary,
          "Only Generate submits the selected invite settings"
      );
    } else if (step == 7)
      ack(*m_dialogAction, "Could not generate this invite. Try again.");
    else if (step == 8) {
      require(
          m_channelInvite.editing && !m_dialogError.empty() && !m_sidebarInviteUrl.empty(),
          "A failed generation preserves the old link and settings draft"
      );
      click(Action::InviteGenerate);
    } else if (step == 9) {
      ack(*m_dialogAction);
      require(
          !m_channelInvite.editing
              && m_channelInvite.maxAge == 86400
              && m_channelInvite.maxUses == 1
              && m_channelInvite.temporary,
          "A confirmed invite becomes the displayed link"
      );
    } else if (step == 10) {
      click(Action::InviteRecipient, "channel:700");
      require(m_channelInvite.pending.contains("channel:700"), "Invite sends only to the explicitly clicked recipient");
    } else if (step == 11) {
      ack(m_channelInvite.pending.at("channel:700"), "Message delivery failed.");
      require(m_channelInvite.errors.contains("channel:700"), "Recipient send failures retain an explicit retry");
    } else if (step == 12)
      click(Action::InviteRecipient, "channel:700");
    else if (step == 13) {
      ack(m_channelInvite.pending.at("channel:700"));
      require(m_channelInvite.sent.contains("channel:700"), "Only an acknowledged direct send displays Sent");
      focusControl("dialog");
      TextInputEdit edit;
      edit.hasCommitText = true;
      edit.commitText = "ly";
      m_inputs.at("dialog")->textInputApplyEdit(edit);
    } else if (step == 14) {
      require(
          m_channelInvite.matches.size() == 1
              && m_channelInvite.recipients[m_channelInvite.matches.front()].name == "Lysec",
          "The retained native search input filters cached recipients"
      );
      click(Action::InviteCopy);
      require(m_channelInvite.copied, "Copy is available only after a link is acknowledged");
    } else if (step == 15) {
      const auto old = *m_dialogAction;
      closeDialog();
      openChannelInvite(old);
      ack(old, "This response belongs to the dismissed dialog.");
    } else if (step == 16) {
      require(
          m_dialogPending
              && m_dialogError.empty()
              && m_dialogAction->inviteMaxAge == 86400
              && m_dialogAction->inviteMaxUses == 1
              && m_dialogAction->inviteTemporary,
          "Reopening reuses channel settings and ignores a dismissed dialog's response"
      );
      ack(*m_dialogAction);
      channelInviteAction({0, 0, 0, 0, Action::InviteEditLink, {}});
      channelInviteAction({0, 0, 0, 0, Action::InviteDropdown, "age"});
    } else if (step == 17) {
      const auto at = m_widgets.find("invite:options");
      require(at != m_widgets.end(), "The invite dropdown retains its native menu");
      auto* menu = static_cast<ContextMenuControl*>(at->second);
      const auto last = menu->entryCount() - 1;
      const float px = m_channelInvite.dropdownBounds.x + m_channelInvite.dropdownBounds.width / 2;
      const float py = m_channelInvite.dropdownBounds.y
          + (menu->rowTop(last) + menu->rowBottom(last)) / 2
          + m_window.titlebarHeight()
          - (Style::space2Xl + Style::spaceXs);
      m_window.motion(px, py);
      m_window.click(px, py, 272);
      m_window.released();
      require(
          m_channelInvite.draftMaxAge == 0 && !m_channelInvite.dropdown && m_dialogKind == "sidebar-invite",
          "Clicking a native option outside the modal selects it without dismissing the dialog"
      );
      channelInviteAction({0, 0, 0, 0, Action::InviteDropdown, "age"});
      m_window.key({.symbol = XKB_KEY_Escape});
      require(
          !m_channelInvite.dropdown && m_dialogKind == "sidebar-invite",
          "Escape dismisses the invite dropdown before the modal"
      );
      m_window.key({.symbol = XKB_KEY_Escape});
      require(m_dialogKind.empty(), "Escape then closes the invite dialog");
    }
    std::clog << "Offline channel invite frame " << step << '\n';
    m_window.invalidate();
  }
} // namespace app
