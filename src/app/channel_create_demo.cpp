#include "app/client_application.h"
#include "ui/controls/input.h"
#include "ui/controls/scrollbar.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::demoChannelCreateStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto click = [&](Action action, const std::string& id = "") {
      const auto found = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == action && (id.empty() || hit.id == id) && hit.height > 0;
      });
      require(found != m_hits.end(), "The channel creation action must be visible and enabled");
      const auto hit = *found;
      const float x = hit.x + hit.width / 2,
                  y = hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.released();
    };
    const auto clickControl = [&](Node* control) {
      require(control && control->parent(), "The retained native creation control must be mounted");
      float x = 0, y = 0;
      Node::mapToScene(control, control->width() / 2, control->height() / 2, x, y);
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.released();
    };
    const auto rolePermissions = [&](const std::string& permissions) {
      auto roles = m_roleDirectory.allRoles(m_guildId);
      const auto role = std::ranges::find(roles, "51", &api::ColoredRole::id);
      require(role != roles.end(), "The creation fixture needs its existing management role");
      role->permissions = permissions;
      api::GuildRoleUpdate changed{.guild = m_guildId};
      changed.roles = std::move(roles);
      m_roleDirectory.apply({false, {changed}});
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(
          m_dialogKind == "sidebar-create"
              && m_dialogAction
              && m_dialogAction->kind == api::InteractionKind::CreateChannel
              && m_dialogAction->channelType == 0
              && !m_dialogAction->privateChannel
              && m_dialogText.empty(),
          "Channel creation opens on Text with a blank name and no account mutation"
      );
      require(
          std::ranges::none_of(m_hits, [](const auto& hit) { return hit.action == Action::SidebarSubmit; }),
          "An empty channel name cannot advance or create a channel"
      );
      m_members = {{.user = {.id = "41", .name = "Create witness", .username = "create-witness"}}};
      click(Action::ChannelCreateType, "2");
      clickControl(m_inputs.at("dialog"));
      pasteControl("dialog", "study-room");
      require(
          m_dialogAction->channelType == 2 && m_dialogText == "study-room" && m_pendingInteractions.empty(),
          "Voice selection and native name editing only change the local draft"
      );
    } else if (step == 1) {
      m_window.key({.symbol = XKB_KEY_z, .control = true});
      require(m_dialogText.empty(), "Pasting a channel name is one native undoable edit");
      pasteControl("dialog", "study-room");
      click(Action::ChannelCreateType, "15");
      require(
          m_dialogAction->channelType == 15 && m_dialogText == "study-room",
          "Community servers offer a working Forum radio without clearing the name"
      );
    } else if (step == 2) {
      click(Action::ChannelCreateType, "0");
      const auto toggle = m_widgets.find("toggle:channel-create-private");
      require(toggle != m_widgets.end(), "The privacy option uses a retained native toggle");
      auto* control = static_cast<Toggle*>(toggle->second);
      require(
          control->enabled() && control->width() == 40 && control->height() == 24,
          "The enabled privacy toggle retains its actual native geometry"
      );
      clickControl(control);
      require(
          m_dialogAction->channelType == 0 && m_dialogAction->privateChannel && !m_channelCreateAccessStep,
          "The real native privacy toggle changes the draft before Next"
      );
    } else if (step == 3) {
      click(Action::SidebarSubmit);
      require(
          m_channelCreateAccessStep
              && !m_dialogPending
              && m_pendingInteractions.empty()
              && m_dialogText == "study-room",
          "Next opens member and role access selection without sending a creation request"
      );
    } else if (step == 4) {
      require(
          m_inputs.contains("channel-create-access") && m_inputs.at("channel-create-access")->parent(),
          "Private access selection mounts its own retained native search input"
      );
      click(Action::ChannelCreateAccess, "0:50");
      click(Action::ChannelCreateAccess, "1:41");
      auto action = *m_dialogAction;
      action.user = m_account.id;
      action.text = m_dialogText;
      const auto body = api::createChannelBody(action);
      require(
          action.channelAccess.size() == 2 && body == nlohmann::json{
              {"name", "study-room"}, {"type", 0},
              {"permission_overwrites", nlohmann::json::array({
                  {{"id", m_guildId}, {"type", 0}, {"allow", "0"}, {"deny", "1024"}},
                  {{"id", m_account.id}, {"type", 1}, {"allow", "1024"}, {"deny", "0"}},
                  {{"id", "50"}, {"type", 0}, {"allow", "1024"}, {"deny", "0"}},
                  {{"id", "41"}, {"type", 1}, {"allow", "1024"}, {"deny", "0"}}
              })}
          },
          "Native access choices build exact private everyone, creator, role and member overwrites"
      );
      require(m_pendingInteractions.empty(), "Selecting access targets must not change the server");
    } else if (step == 5) {
      click(Action::ChannelCreateBack);
      require(
          !m_channelCreateAccessStep && m_dialogText == "study-room" && m_dialogAction->channelAccess.size() == 2,
          "Back preserves the channel name and selected private access targets"
      );
    } else if (step == 6) {
      click(Action::ChannelCreateType, "2");
      click(Action::SidebarSubmit);
      require(
          m_channelCreateAccessStep
              && m_dialogAction->channelType == 2
              && m_dialogAction->channelAccess.size() == 2
              && m_dialogText == "study-room",
          "Changing the type and returning to access selection preserves the complete private draft"
      );
    } else if (step == 7) {
      rolePermissions("16");
      click(Action::SidebarSubmit);
      require(
          !m_dialogPending
              && m_pendingInteractions.empty()
              && m_dialogError == "You need Manage Permissions to create a private channel."
              && m_channelCreateAccessStep
              && m_dialogAction->channelAccess.size() == 2,
          "Losing Manage Permissions blocks an already-open private creation form and retains its draft"
      );
    } else if (step == 8) {
      rolePermissions("268435472");
      click(Action::SidebarSubmit);
      require(
          !m_dialogPending
              && m_pendingInteractions.empty()
              && m_dialogError == "Demo mode is read-only."
              && m_channelCreateAccessStep
              && m_dialogText == "study-room",
          "Explicit submission uses the normal guarded request path without network activity in the replay"
      );
    } else if (step == 9) {
      m_window.setUiScale(1.8F);
    } else if (step == 10) {
      const float logicalBottom = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
      require(
          m_dialogBounds.x >= 0 && m_dialogBounds.y >= 36 && m_dialogBounds.y + m_dialogBounds.height <= logicalBottom,
          "The creation form and footer remain within the zoomed small window"
      );
      const auto scrollbar = m_widgets.find("channel-create-scrollbar");
      require(scrollbar != m_widgets.end(), "Private access has a retained native scrollbar");
      auto* control = static_cast<Scrollbar*>(scrollbar->second);
      require(control->visible() && control->children().size() > 1, "A short access viewport exposes its scrollbar");
      auto* thumb = control->children()[1].get();
      float x = 0, y = 0;
      Node::mapToScene(thumb, thumb->width() / 2, thumb->height() / 2, x, y);
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.motion(x, y + ((Style::spaceSection + Style::spaceXs) + Style::borderWidth));
      require(
          m_controlInput.pointerCaptured() && m_channelCreateScroll > 0,
          "Dragging the actual native scrollbar scrolls private access and captures the pointer"
      );
    } else if (step == 11) {
      require(m_controlInput.pointerCaptured(), "Creation scrollbar capture survives scene rebuilding");
      m_window.released();
      click(Action::CloseDialog);
      require(
          m_dialogKind.empty()
              && !m_dialogAction
              && !m_channelCreateAccessStep
              && m_dialogText.empty()
              && m_channelCreateFilter.empty()
              && m_channelCreateScroll == 0
              && m_inputs.at("dialog")->value().empty()
              && m_pendingInteractions.empty()
              && m_channelId == "101"
              && m_channels.size() == 5,
          "Closing creation releases native capture and all draft state while leaving the server and chat unchanged"
      );
    }
    std::clog << "Offline channel creation replay step " << step + 1 << " passed\n";
    m_window.invalidate();
  }
} // namespace app
