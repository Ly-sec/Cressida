#include "app/client_application.h"
#include "app/theme.h"
#include "ui/controls/input.h"

#include <algorithm>
#include <cmath>
#include <glib.h>

namespace app {
  namespace {
    constexpr std::uint64_t homeRequest = std::uint64_t{1} << 63;
    std::string fold(const std::string& text) {
      auto* value = g_utf8_casefold(text.c_str(), text.size());
      std::string result = value ? value : "";
      g_free(value);
      return result;
    }
  } // namespace
  void ClientApplication::pinDm(const std::string& id, bool pin) {
    const auto channel = std::ranges::find(m_privateChannels, id, &api::Channel::id);
    if (channel == m_privateChannels.end()
        || channel->isMessageRequest()
        || m_dmPinRequests.contains(id)
        || pin == m_userDirectory.pinnedDm(id))
      return;
    m_status.clear();
    m_dmStatus.clear();
    api::Interaction action;
    action.kind = pin ? api::InteractionKind::PinDm : api::InteractionKind::UnpinDm;
    action.channel = id;
    action.revision = homeRequest | ++m_dmSerial;
    action.stateRevision = m_userDirectory.notificationRevision(id);
    action.notificationFlags = m_userDirectory.notificationFlags(id);
    const bool replay = m_demo && m_demoOverlay == "dm-compose-cycle";
    if (replay)
      m_dmDemoAction = action;
    if (replay || startInteraction(action))
      m_dmPinRequests[id] = action.revision;
    m_window.invalidate();
  }
  void ClientApplication::openDmCreate() {
    if (!m_connected && !m_demo)
      return;
    closeContext();
    closeDialog();
    closeCompletion();
    m_scrolling.stopAll();
    m_emojiMenu = m_gifMenu = m_presenceMenu = false;
    std::map<std::string, api::Account> candidates;
    const auto add = [&](const api::Account& user) {
      if (api::validSnowflake(user.id)
          && user.id != m_account.id
          && m_userDirectory.relationship(user.id) != 2
          && !m_userDirectory.ignored(user.id))
        candidates.try_emplace(user.id, user);
    };
    for (const auto& user : m_userDirectory.relatedUsers())
      if (m_userDirectory.relationship(user.id) == 1)
        add(user);
    for (const auto& channel : m_privateChannels)
      if (!channel.isMessageRequest())
        for (const auto& user : channel.recipients)
          add(user);
    for (const auto& member : m_members)
      if (member.count < 0)
        add(member.user);
    for (const auto& [id, snapshot] : m_channelMembers)
      for (const auto& member : snapshot.rows)
        if (member.count < 0)
          add(member.user);
    for (auto& [id, user] : candidates)
      m_dmCreateCandidates.push_back(std::move(user));
    std::ranges::sort(m_dmCreateCandidates, [&](const auto& a, const auto& b) {
      const bool af = m_userDirectory.relationship(a.id) == 1, bf = m_userDirectory.relationship(b.id) == 1;
      if (af != bf)
        return af;
      const auto an = fold(m_userDirectory.name(a.id, a.name)), bn = fold(m_userDirectory.name(b.id, b.name));
      return an != bn ? an < bn : a.id < b.id;
    });
    m_dialogKind = "dm-create";
    m_focusedInput = "dm-create-search";
    m_window.invalidate();
  }
  void ClientApplication::dmCreateAction(const std::string& command) {
    if (m_dialogKind != "dm-create" || m_dmCreateRequest)
      return;
    if (command.starts_with("recipient:")) {
      const auto id = command.substr(10);
      if (std::ranges::find(m_dmCreateCandidates, id, &api::Account::id) == m_dmCreateCandidates.end()
          || m_userDirectory.relationship(id) == 2
          || m_userDirectory.ignored(id))
        return;
      const auto at = std::ranges::find(m_dmCreateRecipients, id);
      if (at != m_dmCreateRecipients.end())
        m_dmCreateRecipients.erase(at);
      else if (m_dmCreateRecipients.size() < 9)
        m_dmCreateRecipients.push_back(id);
      else
        m_dmCreateError = "A group DM can have up to 10 members, including you.";
      if (m_dmCreateRecipients.size() < 9)
        m_dmCreateError.clear();
    } else if (command == "create-submit" && !m_dmCreateRecipients.empty()) {
      for (const auto& id : m_dmCreateRecipients)
        if (id == m_account.id || m_userDirectory.relationship(id) == 2 || m_userDirectory.ignored(id)) {
          m_dmCreateError = "A selected recipient is no longer available. Remove them and try again.";
          m_window.invalidate();
          return;
        }
      if (m_dmCreateRecipients.size() == 1) {
        const auto channel = std::ranges::find_if(m_privateChannels, [&](const auto& value) {
          return value.type == 1
              && !value.isMessageRequest()
              && value.recipients.size() == 1
              && value.recipients.front().id == m_dmCreateRecipients.front();
        });
        if (channel != m_privateChannels.end()) {
          const auto id = channel->id;
          closeDialog();
          m_restoreChannel = id;
          showDirectMessages();
          if (m_channelId != id)
            selectChannel(id);
          return;
        }
      }
      api::Interaction action;
      action.kind = api::InteractionKind::CreateDm;
      action.recipients = m_dmCreateRecipients;
      action.revision = homeRequest | ++m_dmSerial;
      const bool replay = m_demo && m_demoOverlay == "dm-compose-cycle";
      if (replay)
        m_dmDemoAction = action;
      if (replay || startInteraction(action)) {
        m_dmCreateRequest = action.revision;
        m_dialogPending = true;
        m_dmCreateError.clear();
      } else
        m_dmCreateError = m_status;
    }
    m_window.invalidate();
  }
  void ClientApplication::drawDmCreate(Node& root, float w, float h) {
    using namespace theme;
    const float width = std::min(480.0F, w - Style::space2Xl), height = std::min(660.0F, h - 72);
    const float x = (w - width) / 2, y = std::max(44.0F, (h - height) / 2);
    const float inner = width - 40;
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, Color{0, 0, 0, .65F});
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::None, {}});
    rectangle(
        root, x - Style::borderWidth, y - Style::borderWidth, width + Style::spaceXxs, height + Style::spaceXxs, raised,
        13
    );
    rectangle(root, x, y, width, height, background, Style::radiusXl);
    m_dialogBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    label(
        root, x + Style::spaceRelaxed, y + Style::spaceRelaxed, inner - (Style::space2Xl - Style::spaceXxs),
        "New message", Style::fontSizeDialog, foreground, FontWeight::SemiBold
    );
    iconButton(
        root,
        {x + width - 40, y + Style::cardPadding, 26, 26, m_dialogPending ? Action::None : Action::CloseDialog, {}}, "x",
        "Close"
    );
    label(
        root, x + Style::spaceRelaxed, y + 51, inner,
        "Search friends and server members. Group DMs can have up to 10 members, including you.",
        Style::fontSizeDescription, muted, FontWeight::Normal, 2
    );
    inputControl(
        root, x + Style::spaceRelaxed, y + 94, inner, 34, "dm-create-search", m_dmCreateSearch, "Search people"
    );
    std::string selection = "Select people to start a conversation";
    if (!m_dmCreateRecipients.empty()) {
      selection = std::to_string(m_dmCreateRecipients.size()) + "/9 selected · ";
      for (const auto& id : m_dmCreateRecipients) {
        const auto at = std::ranges::find(m_dmCreateCandidates, id, &api::Account::id);
        if (at != m_dmCreateCandidates.end()) {
          if (!selection.ends_with(" · "))
            selection += ", ";
          selection += m_userDirectory.name(id, at->name);
        }
      }
    }
    label(root, x + Style::spaceRelaxed, y + 139, inner, selection, Style::fontSizeMini, muted);
    const float top = y + 164, footer = y + height - 86,
                viewportHeight = std::max(24.0F, footer - top - Style::spaceSm);
    m_dmCreateBounds = {x + Style::spaceMd, top, width - 24, viewportHeight, Action::None, {}};
    const auto query = fold(m_dmCreateSearch);
    std::vector<const api::Account*> matches;
    for (const auto& user : m_dmCreateCandidates)
      if (query.empty() || fold(m_userDirectory.name(user.id, user.name) + " " + user.username).contains(query))
        matches.push_back(&user);
    const float content = matches.size() * 48.0F;
    m_scrolling.limit(m_dmCreateScroll, std::max(0.0F, content - viewportHeight));
    auto viewport = std::make_unique<Node>();
    viewport->setPosition(x + Style::spaceMd, top);
    viewport->setSize(width - Style::spaceXl, viewportHeight);
    viewport->setClipChildren(true);
    viewport->setTextScrollOffset(-m_dmCreateScroll);
    if (matches.empty())
      label(*viewport, 8, 12, inner, "No matching people.", Style::fontSizeCaption, muted);
    for (std::size_t i = 0; i < matches.size(); ++i) {
      const float row = i * 48.0F - m_dmCreateScroll;
      if (row + 48 <= 0 || row >= viewportHeight)
        continue;
      const auto& user = *matches[i];
      const bool selected = std::ranges::find(m_dmCreateRecipients, user.id) != m_dmCreateRecipients.end();
      const bool enabled = !m_dialogPending && (selected || m_dmCreateRecipients.size() < 9);
      if (selected || hovered(x + Style::spaceMd, top + row, width - 30, 46))
        rectangle(*viewport, 0, row, width - 30, 46, selected ? raised : panel, Style::radiusMd);
      avatar(*viewport, 8, row + 7, layout::Avatar::member, user.avatar, user.name, user.id);
      statusBadge(*viewport, 30, row + 28, userStatus(user.id), selected ? raised : background);
      label(
          *viewport, 48, row + 7, width - 104, m_userDirectory.name(user.id, user.name), Style::fontSizeCaption,
          enabled ? foreground : muted, FontWeight::Medium
      );
      label(*viewport, 48, row + 26, width - 104, user.username, Style::fontSizeMini, muted);
      glyph(
          *viewport, width - 58, row + 14, Style::glyphSizeMedium, selected ? "checkbox" : "square",
          selected ? accent : muted
      );
      const float hitTop = std::max(top, top + row), hitBottom = std::min(top + viewportHeight, top + row + 46);
      if (enabled && hitBottom > hitTop)
        m_hits.push_back(
            {x + Style::spaceMd, hitTop, width - 30, hitBottom - hitTop, Action::DmAction, "recipient:" + user.id}
        );
    }
    root.addChild(std::move(viewport));
    if (content > viewportHeight)
      settingsScrollbar(
          root, "popup:dm-create-scrollbar", {x + width - 17, top, 5, viewportHeight, Action::None, {}}, content,
          m_dmCreateScroll
      );
    if (!m_dmCreateError.empty())
      label(
          root, x + Style::spaceRelaxed, footer + 1, inner, m_dmCreateError, Style::fontSizeMini, danger,
          FontWeight::Normal, 2
      );
    const float buttonWidth = (inner - Style::spaceSm) / 2, by = y + height - 48;
    dmButton(
        root, {x + Style::spaceRelaxed, by, buttonWidth, 30, m_dialogPending ? Action::None : Action::CloseDialog, {}},
        "Cancel"
    );
    const bool ready = !m_dialogPending && !m_dmCreateRecipients.empty();
    dmButton(
        root,
        {x + Style::spaceWide + buttonWidth, by, buttonWidth, 30, ready ? Action::DmAction : Action::None,
         "create-submit"},
        m_dialogPending                       ? "Creating…"
            : m_dmCreateRecipients.size() > 1 ? "Create group DM"
                                              : "Open message",
        ready
    );
  }
} // namespace app
