#include "app/client_application.h"
#include "app/theme.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>

namespace app {
  namespace {
    constexpr std::size_t maximumInvitePreviews = 256, maximumPendingInvitePreviews = 4;
    constexpr std::array<std::pair<const char*, const char*>, 6> presets{
        {{"custom", "Create My Own"},
         {"gaming", "Gaming"},
         {"friends", "Friends"},
         {"study", "Study Group"},
         {"school", "School Club"},
         {"community", "Local Community"}}
    };
  } // namespace
  void ClientApplication::requestInvitePreview(const std::string& code, bool retry) {
    if (!api::serverInviteCode(code) || api::serverInviteCode(code) != code)
      return;
    if (const auto found = m_invitePreviews.find(code); found != m_invitePreviews.end()) {
      if (found->second.pending || !retry)
        return;
      m_invitePreviews.erase(found);
    }
    if (m_demo) {
      auto& cached = m_invitePreviews[code];
      cached.error = "Invite preview unavailable in this offline view.";
      return;
    }
    if (!m_connected
        || static_cast<std::size_t>(
               std::ranges::count_if(m_invitePreviews, [](const auto& item) { return item.second.pending; })
           ) >= maximumPendingInvitePreviews)
      return;
    if (m_invitePreviews.size() >= maximumInvitePreviews) {
      const auto old = std::ranges::find_if(m_invitePreviews, [](const auto& item) { return !item.second.pending; });
      if (old == m_invitePreviews.end())
        return;
      m_invitePreviews.erase(old);
    }
    auto& cached = m_invitePreviews[code];
    cached.revision = ++m_inviteSerial;
    api::Interaction action{.kind = api::InteractionKind::InvitePreview, .text = code, .revision = cached.revision};
    cached.pending = startInteraction(action);
    if (!cached.pending)
      cached.error = "The invite preview could not be queued. Open it to try again.";
  }
  MessageOptions::Invite ClientApplication::inviteCardData(const std::string& code) const {
    MessageOptions::Invite result;
    if (const auto found = m_invitePreviews.find(code); found != m_invitePreviews.end()) {
      result.data = found->second.data;
      result.failed = !found->second.error.empty();
      result.joined =
          result.data && std::ranges::find(m_guilds, result.data->guild.id, &api::Guild::id) != m_guilds.end();
    }
    return result;
  }
  void ClientApplication::openServerAccess(const std::string& invite) {
    if (!m_connected && !m_demo)
      return;
    closeContext();
    closeDialog();
    closeCompletion();
    closeGifPicker();
    closeEmojiPicker();
    m_scrolling.stopAll();
    m_serverAccessStage = invite.empty() ? "choose" : "preview";
    m_serverAccessCode = invite;
    m_serverAccessInput = invite;
    m_serverAccessName.clear();
    m_serverAccessPreset = "custom";
    m_serverAccessVerification = false;
    m_serverAccessScroll = 0;
    m_dialogKind = "guild-access";
    m_fieldFocused = false;
    if (!invite.empty())
      requestInvitePreview(invite);
    m_window.invalidate();
  }
  void ClientApplication::serverAccessAction(const std::string& command) {
    if (command == "open") {
      openServerAccess();
      return;
    }
    if (command.starts_with("invite:")) {
      if (const auto code = api::serverInviteCode(command.substr(7))) {
        const auto card = inviteCardData(*code);
        if (card.joined && card.data) {
          closeDialog();
          selectGuild(card.data->guild.id);
        } else
          openServerAccess(*code);
      }
      return;
    }
    if (m_dialogKind != "guild-access" || m_dialogPending)
      return;
    if (command == "join") {
      m_serverAccessStage = "join";
      m_dialogError.clear();
      m_focusedInput = "server-access:invite";
    } else if (command == "back") {
      m_serverAccessStage = "choose";
      m_dialogError.clear();
      m_focusedInput.clear();
    } else if (command.starts_with("preset:")) {
      const auto preset = command.substr(7);
      if (!std::ranges::any_of(presets, [&](const auto& item) { return item.first == preset; }))
        return;
      m_serverAccessPreset = preset;
      m_serverAccessStage = "create";
      m_focusedInput = "server-access:name";
      m_dialogError.clear();
    } else if (command == "preview") {
      const auto code = api::serverInviteCode(m_serverAccessInput);
      if (!code) {
        m_dialogError = "Enter a valid server invite link or code.";
      } else {
        m_serverAccessCode = *code;
        m_serverAccessStage = "preview";
        m_focusedInput.clear();
        m_dialogError.clear();
        requestInvitePreview(*code, true);
      }
    } else if (command == "retry") {
      m_dialogError.clear();
      requestInvitePreview(m_serverAccessCode, true);
    } else if (command == "submit") {
      if (m_serverAccessStage == "create") {
        api::Interaction action{
            .kind = api::InteractionKind::CreateGuild,
            .text = m_serverAccessName,
            .revision = m_dialogGeneration,
            .content = m_serverAccessPreset
        };
        if (!api::interactionRequest(action))
          m_dialogError = "Enter a server name between 2 and 100 characters without surrounding spaces.";
        else if (m_demo) {
          m_dialogAction = action;
          m_dialogPending = true;
        } else if (startInteraction(action)) {
          m_dialogAction = action;
          m_dialogPending = true;
          m_dialogError.clear();
        } else
          m_dialogError = m_status;
      } else if (m_serverAccessStage == "preview") {
        const auto card = inviteCardData(m_serverAccessCode);
        if (!card.data || card.failed)
          return;
        if (m_serverAccessVerification) {
          if (!m_demo)
            auth::openExternalUrl("https://discord.com/channels/" + card.data->guild.id);
        } else if (card.joined) {
          const auto guild = card.data->guild.id;
          closeDialog();
          selectGuild(guild);
        } else {
          api::Interaction action{
              .kind = api::InteractionKind::AcceptInvite,
              .text = m_serverAccessCode,
              .guild = card.data->guild.id,
              .revision = m_dialogGeneration
          };
          if (m_demo || startInteraction(action)) {
            m_dialogAction = action;
            m_dialogPending = true;
            m_dialogError.clear();
          } else
            m_dialogError = m_status;
        }
      }
    }
    m_window.invalidate();
  }
  bool ClientApplication::serverAccessResult(const api::Event& event) {
    const auto& action = *event.interaction;
    if (action.kind == api::InteractionKind::InvitePreview) {
      const auto found = m_invitePreviews.find(action.text);
      if (found == m_invitePreviews.end() || found->second.revision != action.revision)
        return true;
      found->second.pending = false;
      found->second.error = event.error;
      found->second.data = event.serverInvite;
      if (!event.serverInvite && event.error.empty())
        found->second.error = "This server invite is unavailable or expired.";
      m_messageLayouts.clear();
      m_window.invalidate();
      return true;
    }
    if (action.kind != api::InteractionKind::AcceptInvite && action.kind != api::InteractionKind::CreateGuild)
      return false;
    const bool selected = m_dialogKind == "guild-access"
        && m_dialogAction
        && m_dialogAction->kind == action.kind
        && m_dialogAction->revision == action.revision;
    if (event.error.empty()) {
      api::Event membership{.operation = api::Operation::Guilds};
      membership.live = true;
      membership.guilds = event.guilds;
      guildListEvent(membership);
    }
    if (selected) {
      m_dialogPending = false;
      m_dialogError = event.error;
      if (event.error.empty()) {
        if (action.kind == api::InteractionKind::CreateGuild && !event.guilds.empty()) {
          const auto guild = event.guilds.front().id;
          closeDialog();
          selectGuild(guild);
        } else {
          m_serverAccessVerification = event.guildVerificationRequired;
          m_messageLayouts.clear();
        }
      }
    }
    m_window.invalidate();
    return true;
  }
  void ClientApplication::drawInviteCard(
      Node& root, const MessageLayout::InviteCard& card, float x, float y, float absoluteX, float absoluteY
  ) {
    using I = layout::ServerInvite;
    const auto& value = card.value;
    const float width = card.width;
    rectangle(root, x, y, width, I::height, theme::sidebar, Style::radiusXl);
    if (!value.data
        || !m_preferences.showImages
        || !picture(root, x, y, width, I::bannerHeight, value.data->banner, Style::radiusXl))
      rectangle(root, x, y, width, I::bannerHeight, theme::raised, Style::radiusXl);
    const float iconTop = y + I::bannerHeight - I::avatarSize / 2;
    rectangle(
        root, x + I::padding - Style::spaceXs, iconTop - Style::spaceXs, I::avatarSize + Style::spaceXs * 2,
        I::avatarSize + Style::spaceXs * 2, theme::sidebar, Style::radiusXl
    );
    const auto name = value.data ? value.data->guild.name : value.failed ? "Invite unavailable" : "Loading server…";
    avatar(
        root, x + I::padding, iconTop, I::avatarSize,
        value.data && m_preferences.showImages ? value.data->guild.icon : "", name
    );
    label(
        root, x + I::padding, y + I::nameTop, width - I::padding * 2, name, Style::fontSizeBody, theme::foreground,
        FontWeight::SemiBold
    );
    std::string detail;
    if (value.data) {
      if (value.data->guild.online)
        detail = std::to_string(*value.data->guild.online) + " online";
      if (value.data->guild.members)
        detail += (detail.empty() ? "" : " · ") + std::to_string(*value.data->guild.members) + " members";
    } else
      detail = value.failed ? "Open invite to retry" : "Fetching invite details";
    label(
        root, x + I::padding, y + I::detailTop, width - I::padding * 2, detail, Style::fontSizeDescription, theme::muted
    );
    if (m_dialogKind == "guild-access")
      return;
    Hit hit{absoluteX + I::padding, absoluteY + I::buttonTop, width - I::padding * 2,
            Style::controlHeight,   Action::ServerAccess,     "invite:" + card.code};
    const bool over = hovered(hit.x, hit.y, hit.width, hit.height);
    rectangle(
        root, x + I::padding, y + I::buttonTop, hit.width, hit.height, over ? theme::hover : theme::accent,
        Style::controlRadius
    );
    const std::string caption = value.joined ? "Go to Server" : "View Invite";
    const auto metrics =
        m_window.target().renderer().measureText(caption, Style::fontSizeCaption, FontWeight::SemiBold);
    label(
        root, x + (width - metrics.width) / 2,
        y + I::buttonTop + (Style::controlHeight - (metrics.bottom - metrics.top)) / 2, metrics.width + Style::spaceXxs,
        caption, Style::fontSizeCaption, over ? theme::onHover : theme::ink, FontWeight::SemiBold
    );
    m_hits.push_back(std::move(hit));
  }
  void ClientApplication::drawServerAccess(Node& root, float w, float h) {
    using A = layout::ServerAccess;
    const auto stage = m_serverAccessStage;
    const bool form = stage == "join" || stage == "create";
    float errorHeight = 0;
    if (form && !m_dialogError.empty()) {
      const float contentWidth = std::min(Style::embedMaximumWidth, w - A::padding * 2) - A::padding * 2;
      const auto error = m_window.target().renderer().measureText(
          m_dialogError, Style::fontSizeDescription, FontWeight::Normal, contentWidth, 3
      );
      errorHeight = error.bottom - error.top;
    }
    const auto frame = form ? A::dialog(w, h, A::formHeight(errorHeight)) : A::dialog(w, h);
    const float x = frame.x, y = frame.y, width = frame.width, height = frame.height;
    const float inner = width - A::padding * 2, left = x + A::padding;
    rectangle(
        root, 0, layout::Window::titlebarHeight, w, h - layout::Window::titlebarHeight,
        Color{0, 0, 0, Style::modalBackdropOpacity}
    );
    m_hits.push_back({0, layout::Window::titlebarHeight, w, h - layout::Window::titlebarHeight, Action::None, {}});
    m_dialogBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    rectangle(root, x, y, width, height, theme::background, Style::radiusXl);
    const std::string title = stage == "choose" ? "Add a Server"
        : stage == "create"                     ? "Create Your Server"
                                                : "Join a Server";
    label(
        root, left, y + A::padding, inner - Style::space2Xl, title, Style::fontSizeDialog, theme::foreground,
        FontWeight::SemiBold
    );
    iconButton(
        root,
        {x + width - A::padding - Style::spaceXl,
         y + A::padding,
         Style::spaceXl,
         Style::spaceXl,
         m_dialogPending ? Action::None : Action::CloseDialog,
         {}},
        "x", "Close"
    );
    const float footer = y + height - A::padding - Style::controlHeight;
    const auto control = [&](float atY, const std::string& caption, const std::string& command, bool enabled = true,
                             bool primary = false) {
      button(
          root,
          {left, atY, inner, Style::controlHeight, enabled && !m_dialogPending ? Action::ServerAccess : Action::None,
           command},
          caption, primary && enabled && !m_dialogPending, false, true, true
      );
    };
    if (stage == "choose") {
      label(
          root, left, y + A::introTop, inner, "Create a place for your community, or join with an invite.",
          Style::fontSizeBody, theme::muted, FontWeight::Normal, 2
      );
      const float top = y + A::listTop, bottom = footer - Style::spaceLg;
      m_serverAccessList = {left, top, inner, std::max(0.0F, bottom - top)};
      m_scrolling.limit(m_serverAccessScroll, std::max(0.0F, presets.size() * A::rowStep - m_serverAccessList.height));
      auto list = std::make_unique<Node>();
      list->setPosition(left, top);
      list->setSize(inner, m_serverAccessList.height);
      list->setClipChildren(true);
      const auto firstHit = m_hits.size();
      for (std::size_t i = 0; i < presets.size(); ++i) {
        const float at = i * A::rowStep - m_serverAccessScroll;
        if (at + A::rowHeight <= 0 || at >= m_serverAccessList.height)
          continue;
        button(
            *list, {0, at, inner, A::rowHeight, Action::ServerAccess, "preset:" + std::string(presets[i].first)},
            presets[i].second, false, false, true
        );
      }
      for (auto i = firstHit; i < m_hits.size(); ++i) {
        auto& hit = m_hits[i];
        const auto end = std::min(hit.y + hit.height, top + m_serverAccessList.height);
        hit.y = std::max(hit.y, top);
        hit.height = std::max(0.0F, end - hit.y);
      }
      root.addChild(std::move(list));
      control(footer, "Join a Server", "join");
    } else if (stage == "create" || stage == "join") {
      const bool create = stage == "create";
      label(
          root, left, y + A::introTop, inner,
          create ? "Give your server a name. You can customize it afterward."
                 : "Paste a server invite link or enter its code.",
          Style::fontSizeBody, theme::muted, FontWeight::Normal, 2
      );
      inputControl(
          root, left, y + A::listTop, inner, Style::controlHeight,
          create ? "server-access:name" : "server-access:invite", create ? m_serverAccessName : m_serverAccessInput,
          create ? "Server name" : "Invite link or code"
      );
      label(
          root, left, y + A::listTop + Style::controlHeight + Style::spaceMd, inner, m_dialogError,
          Style::fontSizeDescription, theme::danger, FontWeight::Normal, 3
      );
      control(footer - Style::controlHeight - Style::spaceSm, "Back", "back");
      control(
          footer,
          m_dialogPending ? "Working…"
              : create    ? "Create Server"
                          : "Preview Invite",
          create ? "submit" : "preview", true, true
      );
    } else {
      const auto cached = m_invitePreviews.find(m_serverAccessCode);
      const auto card = inviteCardData(m_serverAccessCode);
      if (card.data) {
        const float cardWidth = std::min(inner, layout::ServerInvite::width);
        MessageLayout::InviteCard preview{m_serverAccessCode, 0, 0, cardWidth, card};
        drawInviteCard(
            root, preview, left + (inner - cardWidth) / 2, y + A::introTop, left + (inner - cardWidth) / 2,
            y + A::introTop
        );
      } else {
        label(
            root, left, y + A::listTop, inner,
            card.failed ? "This invite is unavailable or expired." : "Loading invite preview…", Style::fontSizeBody,
            theme::foreground, FontWeight::Normal, 2
        );
      }
      const std::string error = !m_dialogError.empty() ? m_dialogError
          : cached != m_invitePreviews.end()           ? cached->second.error
                                                       : "";
      label(
          root, left, footer - Style::controlHeight - Style::spaceMd, inner,
          m_serverAccessVerification ? "Joined. Continue server verification in the official app." : error,
          Style::fontSizeDescription, theme::muted, FontWeight::Normal, 2
      );
      if (card.failed)
        control(footer, "Retry Preview", "retry");
      else
        control(
            footer,
            m_dialogPending                  ? "Joining…"
                : m_serverAccessVerification ? "Continue in official app"
                : card.joined                ? "Go to Server"
                                             : "Join Server",
            "submit", card.data.has_value(), true
        );
    }
    m_tooltip.clear();
  }
  void ClientApplication::demoServerAccessStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto click = [&](const std::string& command) {
      const auto at = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == Action::ServerAccess && hit.id == command && hit.height > 0;
      });
      require(at != m_hits.end(), "The expected server action is visible and actionable");
      const auto hit = *at;
      const auto x = hit.x + hit.width / 2;
      const auto y = hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      m_window.released();
    };
    const auto member = [&](const std::string& id) {
      return std::ranges::find(m_guilds, id, &api::Guild::id) != m_guilds.end();
    };
    const auto reply = [&](const std::string& error) {
      api::Event event{.operation = api::Operation::Interaction};
      event.interaction = m_dialogAction;
      event.error = error;
      if (error.empty()) {
        if (event.interaction->kind == api::InteractionKind::AcceptInvite) {
          event.serverInvite = m_invitePreviews.at("moonInvite").data;
          event.guilds = {event.serverInvite->guild};
        } else
          event.guilds = {{.id = "99902", .name = "My Study Room"}};
      }
      interactionResult(event);
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(m_messageLayouts.at("89001").layout.invites.size() == 1, "A chat invite renders a native server card");
      m_scrolling.stop(m_guildScroll);
      m_guildScroll = Style::scrollWheelStep * m_guilds.size();
    } else if (step == 1)
      click("open");
    else if (step == 2) {
      require(
          m_dialogKind == "guild-access" && m_serverAccessStage == "choose",
          "The rail add button opens join/create choices"
      );
      click("join");
    } else if (step == 3) {
      m_serverAccessInput = "https://discord.gg/moonInvite";
      click("preview");
      require(m_serverAccessStage == "preview" && !member("99901"), "Previewing never joins a server");
    } else if (step == 4) {
      auto& cached = m_invitePreviews.at("moonInvite");
      cached.revision = ++m_inviteSerial;
      api::Event event{.operation = api::Operation::Interaction};
      event.interaction = api::Interaction{
          .kind = api::InteractionKind::InvitePreview, .text = "moonInvite", .revision = cached.revision
      };
      event.serverInvite = api::ServerInvite{
          "moonInvite", {.id = "99901", .name = "Moon Cafe", .members = 21, .online = 13}, "demo-preview"
      };
      interactionResult(event);
    } else if (step == 5) {
      click("submit");
      require(m_dialogPending && !member("99901"), "Queuing a join does not claim membership");
    } else if (step == 6) {
      reply("Synthetic join failure");
      require(
          !member("99901") && !m_dialogPending && !m_dialogError.empty(),
          "Failed joins retain the preview and explicit error"
      );
    } else if (step == 7) {
      click("submit");
      reply("");
      require(member("99901"), "A validated join acknowledgement immediately updates the server rail");
    } else if (step == 8) {
      api::Event event{.operation = api::Operation::Guilds};
      event.live = true;
      event.guilds = {{.id = "99901", .name = "Moon Cafe"}};
      guildListEvent(event);
      require(
          std::ranges::count(m_guilds, std::string("99901"), &api::Guild::id) == 1,
          "A later live join does not duplicate the rail entry"
      );
      event.guilds.clear();
      event.removed = {"99901"};
      guildListEvent(event);
      require(!member("99901") && !m_guilds.empty(), "A live departure removes only its server");
      closeDialog();
      openServerAccess();
    } else if (step == 9)
      click("preset:gaming");
    else if (step == 10) {
      m_serverAccessName = "x";
      click("submit");
      require(!m_dialogPending && !m_dialogError.empty(), "Invalid names never queue server creation");
    } else if (step == 11) {
      m_serverAccessName = "My Study Room";
      click("submit");
      require(
          m_dialogPending && m_dialogAction->kind == api::InteractionKind::CreateGuild,
          "Creation requires an explicit submit"
      );
      const auto request = api::interactionRequest(*m_dialogAction);
      require(
          request && request->body.at("channels").size() == 4,
          "The selected starter preset creates its declared channels"
      );
    } else if (step == 12) {
      reply("");
      require(
          member("99902") && m_guildId == "99902" && m_dialogKind.empty(), "Successful creation opens the new server"
      );
    } else if (step == 13) {
      api::Event event{.operation = api::Operation::Guilds};
      event.live = true;
      event.removed = {"99902"};
      guildListEvent(event);
      require(!member("99902") && m_dmMode, "Losing access to the selected server returns to direct messages");
    } else if (step == 14) {
      openServerAccess("moonInvite");
      const auto before = m_invitePreviews.at("moonInvite").revision;
      api::Event stale{.operation = api::Operation::Interaction};
      stale.interaction =
          api::Interaction{.kind = api::InteractionKind::InvitePreview, .text = "moonInvite", .revision = before + 1};
      stale.error = "Wrong request";
      interactionResult(stale);
      require(
          m_invitePreviews.at("moonInvite").error != "Wrong request",
          "Stale invite replies cannot replace a current preview"
      );
    } else {
      closeDialog();
      std::clog << "Offline server previews, explicit joins, creation and live membership passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
