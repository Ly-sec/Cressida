#include "app/client_application.h"
#include "app/theme.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace app {
  using namespace theme;

  float ClientApplication::drawGuildTag(
      Node& parent, float x, float y, float available, const std::optional<api::GuildTag>& tag, bool clickable,
      Color fill
  ) {
    if (!tag || tag->tag.empty() || available < 42)
      return 0;
    const float width = std::min(
        {available, 104.0F,
         std::ceil(m_window.target().renderer().measureText(tag->tag, Style::fontSizeMini, FontWeight::Medium).width)
             + 32}
    );
    float ox = 0, oy = 0;
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      ox += node->x();
      oy += node->y();
    }
    const bool over = clickable && hovered(ox + x, oy + y, width, 20);
    rectangle(parent, x, y, width, 20, over ? raised : fill, Style::radiusMd);
    if (!picture(
            parent, x + Style::spaceXs, y + Style::spaceXxs, 16, 16, tag->badge, 0, false, false, ImageFitMode::Contain
        ))
      glyph(
          parent, x + (Style::spaceXs + Style::borderWidth), y + (Style::spaceXxs + Style::borderWidth),
          Style::glyphSizeSmall, "shield", muted
      );
    label(
        parent, x + (Style::spaceXl + Style::borderWidth), y + (Style::spaceXxs + Style::borderWidth), width - 30,
        tag->tag, Style::fontSizeMini, foreground, FontWeight::Medium
    );
    if (clickable && api::validSnowflake(tag->guild)) {
      m_hits.push_back({x + ox, y + oy, width, 20, Action::GuildProfile, tag->guild});
      if (over) {
        m_tooltip = "View server · " + tag->tag;
        m_tooltipX = x + ox + width / 2;
        m_tooltipY = y + oy;
      }
    }
    return width + 6;
  }

  float ClientApplication::drawRoleIcon(
      Node& parent, float x, float y, const std::string& guild, const std::string& user,
      const std::optional<std::vector<std::string>>& roles
  ) {
    const auto role = m_roleDirectory.icon(guild, user, roles);
    if (!role)
      return 0;
    return drawRoleBadge(parent, x, y, *role);
  }
  float ClientApplication::drawRoleBadge(Node& parent, float x, float y, const api::ColoredRole& role) {
    if (!role.icon.empty())
      emojiImage(parent, x, y, 16, role.icon);
    else
      label(parent, x, y - Style::borderWidth, 20, role.unicodeEmoji, Style::fontSizeSubtitle, foreground);
    float ox = 0, oy = 0;
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      ox += node->x();
      oy += node->y();
    }
    if (hovered(x + ox, y + oy, 18, 18)) {
      m_tooltip = role.name;
      m_tooltipX = x + ox + 8;
      m_tooltipY = y + oy;
    }
    return 22;
  }
  void ClientApplication::prepareGuildTagDemo() {
    std::vector<unsigned char> badge(32 * 32 * 4);
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 32; ++x) {
        const int at = (y * 32 + x) * 4;
        badge[at] = 155;
        badge[at + 1] = 220;
        badge[at + 2] = 240;
        badge[at + 3] = std::abs(x - Style::spaceLg) + std::abs(y - Style::spaceLg) < 13 ? 255 : 0;
      }
    m_images["demo-profile-badge"].texture = m_window.context().textureManager().loadFromRgba(badge.data(), 32, 32);
    // Exercise protocol projection and the actual CDN cache key. Directly
    // constructing a role with a demo-only path concealed a broken URL before.
    using nlohmann::json;
    const std::string hash = "1234567890abcdef1234567890abcdef";
    const std::string rolePath = "/role-icons/501/" + hash + ".png?size=32";
    json members = json::array();
    for (auto& message : m_messages) {
      message.authorTag = api::GuildTag{"99", "MOON", "demo-profile-badge"};
      members.push_back({{"user_id", message.authorId}, {"roles", {message.authorId == "12" ? "502" : "501"}}});
    }
    m_roleDirectory.apply(
        api::parseRoleUpdates(
            "READY",
            json{
                {"guilds",
                 json::array(
                     {{{"id", m_guildId},
                       {"roles",
                        json::array(
                            {{{"id", "501"}, {"name", "Artist"}, {"position", 5}, {"color", 0xBE95EC}, {"icon", hash}},
                             {{"id", "502"},
                              {"name", "Moonwarden"},
                              {"position", 4},
                              {"color", 0x72BFB2},
                              {"unicode_emoji", "🌙"}}}
                        )}}}
                 )},
                {"merged_members", json::array({members})}
            }
        )
    );
    const auto icon = m_roleDirectory.icon(m_guildId, "11");
    if (!icon || icon->icon != rolePath || api::mediaUrl(icon->icon) != "https://cdn.discordapp.com" + rolePath)
      throw std::runtime_error("Role icons must use Discord's documented role-icons CDN route");
    m_images[rolePath].texture = m_images.at("demo-profile-badge").texture;
    if (m_demoOverlay == "guild-tags-hidden")
      m_preferences.showGuildTags = false;
  }

  void ClientApplication::demoGuildTagStep() {
    const auto require = [](bool ok, const char* message) {
      if (!ok)
        throw std::runtime_error(message);
    };
    const auto step = m_demoInteractionStep++;
    auto click = [&](Action kind) {
      const auto at = std::ranges::find(m_hits.rbegin(), m_hits.rend(), kind, &Hit::action);
      require(at != m_hits.rend(), "Server tag action must be visible");
      const auto hit = *at;
      m_window.click(
          hit.x + hit.width / 2, hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          272
      );
      m_window.released();
    };
    if (step == 0) {
      require(
          std::ranges::count(m_hits, Action::GuildProfile, &Hit::action) >= 3, "Chat authors have clickable server tags"
      );
      click(Action::GuildProfile);
    } else if (step == 1) {
      require(
          m_dialogKind == "guild-profile" && m_guildProfile.id == "99" && !m_guildPreviewJoined,
          "Clicking a tag opens the right server without joining it"
      );
      click(Action::JoinGuild);
    } else if (step == 2) {
      require(m_guildPreviewJoined && !m_dialogPending, "Join changes state only after its acknowledgement");
      require(
          std::ranges::find(m_guilds, "99", &api::Guild::id) != m_guilds.end(),
          "An acknowledged public join adds the server to the rail"
      );
      const auto old = *m_dialogAction;
      showGuildProfile("99");
      api::Event stale;
      stale.interaction = old;
      interactionResult(stale);
      require(!m_guildPreviewJoined, "Old join acknowledgements cannot replace a reopened card");
      closeDialog();
      activate({0, 0, 0, 0, Action::Preference, "guild_tags"});
    } else if (step == 3) {
      require(
          std::ranges::none_of(m_hits, [](const auto& hit) { return hit.action == Action::GuildProfile; }),
          "Disabling chat tags removes their text, images and hit regions"
      );
      int icons = 0;
      auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Image
            && static_cast<const ImageNode&>(node).textureId() == m_images.at("demo-profile-badge").texture.id)
          ++icons;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(icons > 0, "Role icons remain visible when chat tags are hidden");
      activate({0, 0, 0, 0, Action::Preference, "guild_tags"});
    } else if (step == 4) {
      require(
          std::ranges::count(m_hits, Action::GuildProfile, &Hit::action) >= 3,
          "Re-enabling chat tags restores their actions"
      );
      showProfile("11");
    } else if (step == 5) {
      require(m_dialogKind == "profile", "Profile fixture is open");
      click(Action::GuildProfile);
    } else if (step == 6) {
      require(
          m_dialogKind == "guild-profile" && m_guildProfile.id == "99",
          "Profile tags open the same server preview as chat tags"
      );
      closeDialog();
      openSettings();
      selectSettingsPage("cressida");
    } else if (step == 7 || step == 8) {
      require(m_settingsOpen && m_settingsPage == "cressida", "Client preferences share one General page");
      require(
          m_widgets.contains("nav:cressida") && m_widgets.at("nav:cressida")->parent(),
          "The consolidated client page is visible, including when searching its settings"
      );
      require(
          m_widgets.contains("toggle:emoji_image_links")
              && m_widgets.contains("toggle:role_colors")
              && m_widgets.contains("toggle:animate_role_colors"),
          "Expression and name controls share the same page"
      );
      const auto* toggle = m_widgets.at("toggle:guild_tags");
      float tx = 0, ty = 0;
      for (const auto* node = toggle; node && node != m_scene.get(); node = node->parent()) {
        tx += node->x();
        ty += node->y();
      }
      m_window.click(
          tx + Style::spaceRelaxed, ty + Style::spaceMd + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          272
      );
      m_window.released();
      require(
          m_preferences.showGuildTags == (step == 8),
          "Retained native settings toggles remain functional after consolidation"
      );
      m_settingsFields["search"] = step == 7 ? "image links" : "role";
    } else if (step == 9) {
      require(
          m_widgets.at("nav:cressida")->parent() && m_preferences.showGuildTags,
          "Search finds the General page by its role/image-link options without changing preferences"
      );
      std::clog << "Offline guild tag and settings replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
