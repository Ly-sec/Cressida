#include "app/client_application.h"
#include "app/theme.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <iostream>
#include <linux/input-event-codes.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    api::UserProfile fixture(const std::string& id, bool longBio = false, bool privateProfile = false) {
      const auto name = id == "11" ? "Maya" : "Noah";
      std::string bio = "Native UI enthusiast. **Making things** and sharing ideas.";
      if (longBio)
        for (int i = 0; i < 40; ++i)
          bio += "\nA longer profile description with another paragraph.";
      return api::parseUserProfile(
          nlohmann::json{
              {"user",
               {{"id", id},
                {"username", id == "11" ? "maya" : "noah"},
                {"global_name", name},
                {"avatar", "11111111111111111111111111111111"}}},
              {"user_profile",
               {{"bio", bio},
                {"pronouns", "they/them"},
                {"banner", "22222222222222222222222222222222"},
                {"accent_color", 0x629EC0}}},
              {"private", privateProfile},
              {"mutual_friends_count", 3},
              {"mutual_guilds", nlohmann::json::array({{{"id", "1"}}, {{"id", "2"}}})}
          }
      );
    }
  } // namespace
  void ClientApplication::prepareDmProfileDemo(const std::string& view) {
    if (view.ends_with("light"))
      m_preferences.theme = "light";
    m_privateChannels = api::parsePrivateChannels(nlohmann::json::parse(R"([
      {"id":"9001","type":1,"recipients":[{"id":"11","username":"maya","global_name":"Maya","avatar":"11111111111111111111111111111111","primary_guild":{"identity_guild_id":"1","identity_enabled":true,"tag":"NATV","badge":"33333333333333333333333333333333"}}]},
      {"id":"9002","type":1,"recipients":[{"id":"12","username":"noah","global_name":"Noah","avatar":"11111111111111111111111111111111"}]},
      {"id":"9003","type":3,"name":"Study room","recipients":[{"id":"11","username":"maya","global_name":"Maya"},{"id":"12","username":"noah","global_name":"Noah"}]}
    ])"));
    m_privateReady = true;
    m_showMembers = true;
    m_showDmProfile = true;
    m_userDirectory.consume("READY_SUPPLEMENTAL", nlohmann::json::parse(R"({
      "merged_presences":{"friends":[
        {"user_id":"11","status":"online","activities":[{"type":4,"state":"Building native things","emoji":{"id":null,"name":"👀"}}]},
        {"user_id":"12","status":"idle","activities":[]}
      ]}
    })"));
    const int size = static_cast<int>(layout::Avatar::large);
    for (const auto id : {"11", "12"}) {
      const auto profile = fixture(id);
      std::vector<std::uint8_t> pixels(size * size * 4);
      const auto color =
          rgbHex(id == std::string_view("11") ? defaultThemePalette(false).accent : defaultThemePalette(false).success);
      for (std::size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
        pixels[pixel] = static_cast<std::uint8_t>(color.r * 255);
        pixels[pixel + 1] = static_cast<std::uint8_t>(color.g * 255);
        pixels[pixel + 2] = static_cast<std::uint8_t>(color.b * 255);
        pixels[pixel + 3] = 255;
      }
      m_images[profile.user.avatar].texture =
          m_window.context().textureManager().loadFromRgba(pixels.data(), size, size);
      m_images[profile.banner].texture = m_window.context().textureManager().loadFromRgba(pixels.data(), size, size);
    }
    showDirectMessages();
    selectChannel("9001");
    m_drafts["9001"] = "Keep this draft";
    m_messages = {{"9901", "9001", "Maya", "A direct conversation with a profile beside it.", "2026-10-20T19:00:00Z"}};
    m_messages.front().authorId = "11";
    m_messageLayouts.clear();
    if (view.starts_with("dm-profile-cycle")) {
      m_demoOverlay = view;
      m_demoInteractionStep = 0;
    } else {
      m_dmProfile.data = fixture("11");
      m_dmProfile.request.reset();
    }
    if (view.ends_with("narrow"))
      m_window.setUiScale(1.6F);
  }
  void ClientApplication::demoDmProfileStep() {
    using Json = nlohmann::json;
    static api::Interaction firstRequest;
    const auto require = [](bool ok, const char* reason) {
      if (!ok)
        throw std::runtime_error(reason);
    };
    const auto click = [&](Action action, const std::string& id = std::string{}) {
      const auto at = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == action && (id.empty() || hit.id == id);
      });
      require(at != m_hits.end(), "DM profile replay has a native click target");
      m_window.motion(
          at->x + (action == Action::Channel ? Style::spaceXs : at->width / 2),
          at->y + at->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      m_window.click(m_pointerX, m_pointerY + m_window.titlebarHeight() - layout::Window::titlebarHeight, BTN_LEFT);
      m_window.released();
    };
    const auto textCount = [&](const std::string& expected) {
      int found = 0;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text && static_cast<const TextNode&>(node).text() == expected)
          ++found;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      return found;
    };
    const auto profileButton = [&] {
      return std::ranges::any_of(m_hits, [&](const auto& hit) {
        return hit.action == Action::UserProfile
            && hit.id == m_dmProfile.user
            && hit.x >= m_window.width() - memberPaneWidth()
            && hit.width > layout::Avatar::profile;
      });
    };
    const auto response = [&](const std::string& error = std::string{}, bool longBio = false, bool hidden = false) {
      if (!m_dmProfile.request)
        throw std::runtime_error(
            "Profile result without a request at replay step " + std::to_string(m_demoInteractionStep)
        );
      api::Event event;
      event.interaction = m_dmProfile.request;
      event.error = error;
      if (error.empty()) {
        event.profile = fixture(m_dmProfile.user, longBio, hidden);
        event.account = event.profile->user;
      }
      interactionResult(event);
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(
          dmRecipient() && memberPaneWidth() > layout::Members::width && m_dmProfile.request && profileButton(),
          "A one-to-one DM replaces participants with a floating profile and cached identity while loading"
      );
      require(textCount("Building native things") == 3, "Custom status appears in header, DM list and profile");
      const auto row = std::ranges::find_if(m_hits, [](const auto& hit) {
        return hit.action == Action::Channel && hit.id == "9001";
      });
      const auto tag = std::ranges::find_if(m_hits, [](const auto& hit) {
        return hit.action == Action::GuildProfile && hit.id == "1";
      });
      require(
          row != m_hits.end()
              && tag != m_hits.end()
              && tag->y >= row->y + layout::DmSidebar::contentInset
              && tag->y + tag->height <= row->y + row->height - layout::DmSidebar::contentInset,
          "DM selection and hover bounds retain vertical breathing room around identity tags"
      );
      const auto profile = fixture("11");
      const auto texture = m_images.at(profile.user.avatar).texture.id;
      bool headerAvatar = false;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Image
            && node.width() == layout::DmProfile::headerAvatar
            && static_cast<const ImageNode&>(node).textureId() == texture)
          headerAvatar = true;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(headerAvatar, "The actual DM header paints the recipient avatar");
      firstRequest = *m_dmProfile.request;
      response();
    } else if (step == 1) {
      require(
          m_dmProfile.data && textCount("Member since") == 1 && textCount("About me") == 1,
          "A matching global profile fills the identity and biography sections"
      );
      const auto footer = std::ranges::find_if(m_hits, [&](const auto& h) {
        return h.action == Action::UserProfile
            && h.id == "11"
            && h.width > layout::Avatar::profile
            && h.x >= m_window.width() - memberPaneWidth();
      });
      require(footer != m_hits.end(), "The profile footer is mounted");
      const auto hit = *footer;
      m_window.motion(
          hit.x + hit.width / 2, hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      m_window.click(m_pointerX, m_pointerY + m_window.titlebarHeight() - layout::Window::titlebarHeight, BTN_LEFT);
      m_window.released();
      require(
          m_dialogKind == "profile" && m_dialogAccount.id == "11" && !m_dialogPending,
          "View Full Profile reuses the loaded global identity without a duplicate request"
      );
      require(m_drafts["9001"] == "Keep this draft", "Opening the profile preserves the DM draft");
    } else if (step == 2) {
      closeDialog();
    } else if (step == 3) {
      m_window.key({.symbol = XKB_KEY_u, .control = true});
    } else if (step == 4) {
      require(memberPaneWidth() == 0 && !profileButton(), "The header toggle hides the profile surface and hits");
      require(m_showMembers && !m_showDmProfile, "Hiding the DM profile preserves member-list visibility");
      click(Action::ToggleMembers);
    } else if (step == 5) {
      require(profileButton() && m_dmProfile.data && !m_dmProfile.request, "Reopening reuses the current profile");
      click(Action::Channel, "9002");
      api::Event stale;
      stale.interaction = firstRequest;
      stale.profile = fixture("11");
      interactionResult(stale);
      require(
          m_dmProfile.user == "12" && !m_dmProfile.data && m_dmProfile.request,
          "A previous conversation's late response cannot replace the selected profile"
      );
      response("Temporary profile failure");
    } else if (step == 6) {
      require(
          !m_dmProfile.error.empty() && !m_dmProfile.request,
          "Failures retain the cached basic identity and explicit retry"
      );
      click(Action::DmProfileRetry);
      response({}, true);
    } else if (step == 7) {
      require(
          m_dmProfile.data && m_dmProfile.user == "12" && m_widgets.contains("dm-profile-scrollbar"),
          "Long biographies use a retained scrollable profile viewport"
      );
      m_window.motion(
          m_window.width() - layout::DmProfile::inset,
          layout::Members::top
              + layout::DmProfile::identityTop
              + m_window.titlebarHeight()
              - layout::Window::titlebarHeight
      );
      m_window.scroll(Style::spaceGroup);
      require(m_scrolling.active(m_dmProfile.scroll), "Wheel input over the profile uses the profile scroll owner");
    } else if (step == 8) {
      click(Action::ToggleMembers);
      click(Action::Channel, "9003");
      require(!dmRecipient() && m_dmProfile.user.empty(), "Group conversations keep participant mode");
    } else if (step == 9) {
      require(
          memberPaneWidth() == layout::Members::width && !profileButton(), "Groups retain the ordinary member list"
      );
      click(Action::ToggleMembers);
      require(!m_showMembers && !m_showDmProfile, "Group members toggle independently of the hidden DM profile");
      click(Action::Channel, "9001");
      require(memberPaneWidth() == 0, "Returning to a DM preserves its independently hidden profile");
      click(Action::ToggleMembers);
      require(
          memberPaneWidth() > 0 && !m_showMembers, "Showing a DM profile leaves group and server member lists hidden"
      );
      response({}, false, true);
    } else if (step == 10) {
      require(
          m_dmProfile.data
              && m_dmProfile.data->privateProfile
              && textCount("This profile is private.") == 1
              && textCount("About me") == 0,
          "Private profiles do not disclose their biography"
      );
      m_userDirectory.consume(
          "PRESENCE_UPDATE", Json{{"user_id", "11"}, {"status", "dnd"}, {"activities", Json::array()}}
      );
      m_window.setUiScale(1.6F);
    } else if (step == 11) {
      require(
          memberPaneWidth() == 0 && textCount("Building native things") == 0,
          "Narrow windows keep the conversation usable, and cleared live statuses disappear"
      );
      m_window.setUiScale(1);
      m_userDirectory.consume(
          "PRESENCE_UPDATE",
          Json{
              {"user_id", "11"},
              {"activities", Json::array({{{"type", 4}, {"emoji", {{"name", "👀"}, {"id", nullptr}}}}})}
          }
      );
    } else if (step == 12) {
      require(
          memberPaneWidth() > 0
              && m_userDirectory.customStatus("11").text.empty()
              && !m_userDirectory.customStatus("11").emojiName.empty(),
          "Emoji-only live statuses remain visible after restoring width"
      );
      clearInteractions();
      require(
          m_dmProfile.user.empty() && !m_dmProfile.data && !m_dmProfile.request,
          "Account reset drops profile data and pending UI generations"
      );
      std::clog << "Offline DM profile, header avatar, custom statuses, retry, stale results, groups and responsive "
                   "layout passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
