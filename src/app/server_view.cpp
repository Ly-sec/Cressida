#include "app/client_application.h"
#include "app/theme.h"
#include "render/scene/image_node.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace app {
  using namespace theme;
  namespace {
    std::string countText(int value) {
      auto text = std::to_string(value);
      for (int at = static_cast<int>(text.size()) - 3; at > 0; at -= 3)
        text.insert(at, ",");
      return text;
    }
  } // namespace
  float ClientApplication::drawGuildBadge(Node& root, float x, float y, api::GuildBadge badge, bool tooltip) {
    if (badge == api::GuildBadge::None)
      return 0;
    const auto icon = badge == api::GuildBadge::Verified ? "rosette-discount-check-filled"
        : badge == api::GuildBadge::Partnered            ? "affiliate-filled"
                                                         : "building-community";
    const auto title = badge == api::GuildBadge::Verified ? "Verified server"
        : badge == api::GuildBadge::Partnered             ? "Partnered server"
                                                          : "Community server";
    glyph(root, x, y, Style::glyphSizeMedium, icon, badge == api::GuildBadge::Verified ? success : accent);
    if (tooltip && hovered(x, y, 18, 18) && sidebarResizeAvailable()) {
      m_tooltip = title;
      m_tooltipX = x + (Style::spaceSm + Style::borderWidth);
      m_tooltipY = y;
    }
    return 24;
  }
  void ClientApplication::drawGuildTooltip(Node& root, float w, float h) {
    m_guildTooltipBounds = {};
    if (!sidebarResizeAvailable()
        || m_resizingSidebar
        || (m_railDrag && m_railDrag->active)
        || m_dragDraft
        || m_dragMessage
        || (!m_window.active() && !m_demo))
      return;
    const auto at = std::ranges::find_if(m_hits, [&](const auto& hit) {
      return hit.action == Action::Guild && hit.contains(m_pointerX, m_pointerY);
    });
    if (at == m_hits.end())
      return;
    const auto found = std::ranges::find(m_guilds, at->id, &api::Guild::id);
    if (found == m_guilds.end())
      return;
    const auto& guild = *found;
    std::string activity, latest;
    bool unread = false;
    const bool guildMuted = m_userDirectory.muted(guild.id);
    for (const auto& [channel, server] : m_unreadGuilds) {
      if (server != guild.id || !m_unreadChannels.contains(channel))
        continue;
      if ((guildMuted || channelMuted(channel)) && m_unreadTimeline.mentions(channel) == 0)
        continue;
      if (!channelActivityVisible(guild.id, channel)
          || (!unreadDotVisible(guild.id, channel) && m_unreadTimeline.mentions(channel) == 0))
        continue;
      unread = true;
      const auto* row = serverChannel(guild.id, channel);
      if (!row)
        row = m_threads.find(guild.id, channel);
      const auto cursor = m_unreadTimeline.cursor(channel);
      const auto message = cursor ? cursor->latest : std::string{};
      if (activity.empty() || message.size() > latest.size() || (message.size() == latest.size() && message > latest)) {
        activity = row ? "New activity in #" + row->name : "New activity";
        latest = message;
      }
    }
    if (activity.empty() && unread)
      activity = "New activity";
    const auto badge = api::guildBadge(guild);
    const float badgeWidth = badge == api::GuildBadge::None ? 0 : 24;
    auto& renderer = m_window.target().renderer();
    const auto titleWidth =
        renderer.measureText(guild.name, Style::fontSizeCaption, FontWeight::SemiBold).width + badgeWidth;
    const auto activityWidth =
        std::ceil(renderer.measureText(activity, Style::fontSizeMini).width) + Style::borderWidth;
    const std::string online = guild.online ? countText(*guild.online) + " online" : "";
    const std::string members = guild.members ? countText(*guild.members) + " members" : "";
    const float onlineWidth = online.empty() ? 0 : renderer.measureText(online, Style::fontSizeDescription).width + 20;
    const float membersWidth =
        members.empty() ? 0 : renderer.measureText(members, Style::fontSizeDescription).width + 18;
    const bool counts = guild.online || guild.members;
    const float width = std::min(
        std::clamp(std::max({titleWidth, activityWidth, onlineWidth + membersWidth}) + 24, 200.0F, 340.0F), w - 94
    );
    const auto activityLines = [&](int lines) {
      const auto metrics =
          renderer.measureText(activity, Style::fontSizeMini, FontWeight::Normal, width - Style::spaceXl, lines);
      return metrics.bottom - metrics.top;
    };
    const float activityExtra = activity.empty() ? 0 : std::max(0.0F, activityLines(2) - activityLines(1));
    const float height = 40 + (activity.empty() ? 0 : 20 + activityExtra) + (counts ? 24 : 0);
    const float x = 84, y = std::clamp(at->y + at->height / 2 - height / 2, 44.0F, std::max(44.0F, h - height - 8));
    rectangle(
        root, x, y + (Style::spaceXxs + Style::borderWidth), width, height, Color{0, 0, 0, .25F}, Style::surfaceRadius
    );
    rectangle(root, x, y, width, height, lerpColor(sidebar, muted, .3F), Style::surfaceRadius);
    rectangle(
        root, x + Style::borderWidth, y + Style::borderWidth, width - Style::spaceXxs, height - Style::spaceXxs,
        sidebar, Style::hoverRadius
    );
    glyph(
        root, x - (Style::spaceMd + Style::borderWidth),
        std::clamp(at->y + (Style::cardPadding + Style::borderWidth), y + Style::spaceSm, y + height - Style::spaceXl),
        Style::glyphSizeLarge, "caret-left-filled", sidebar
    );
    const auto offset = drawGuildBadge(root, x + Style::spaceMd, y + Style::spaceDense, badge, false);
    label(
        root, x + Style::spaceMd + offset, y + (Style::spaceDense + Style::borderWidth),
        width - Style::spaceXl - offset, guild.name, Style::fontSizeCaption, foreground, FontWeight::SemiBold
    );
    if (!activity.empty())
      label(
          root, x + Style::spaceMd, y + (Style::space2Xl + Style::borderWidth), width - Style::spaceXl, activity,
          Style::fontSizeMini, muted, FontWeight::Normal, 2
      );
    if (counts) {
      const float cy = y + height - 25;
      float cx = x + Style::spaceMd;
      if (guild.online) {
        rectangle(root, cx, cy + (Style::spaceXxs + Style::borderWidth), 9, 9, success, Style::controlRadius);
        label(
            root, cx + (Style::cardPadding + Style::borderWidth), cy,
            onlineWidth - (Style::cardPadding + Style::borderWidth), online, Style::fontSizeDescription, foreground
        );
        cx += onlineWidth;
      }
      if (guild.members) {
        glyph(root, cx, cy + Style::borderWidth, Style::glyphSizeTiny, "circle", muted);
        label(
            root, cx + Style::spaceLg, cy, std::max(0.0F, x + width - Style::spaceMd - cx - Style::spaceLg), members,
            Style::fontSizeDescription, muted
        );
      }
    }
    m_guildTooltipBounds = {x, y, width, height, Action::None, guild.id};
    m_tooltip.clear();
  }
  void ClientApplication::demoGuildHoverStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto move = [&](const std::string& guild) {
      const auto at =
          std::ranges::find_if(m_hits, [&](const auto& hit) { return hit.action == Action::Guild && hit.id == guild; });
      require(at != m_hits.end(), "Hover replay server must be visible");
      m_window.motion(
          at->x + Style::spaceXl, at->y + Style::spaceXl + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
    };
    const auto step = m_demoInteractionStep++;
    for (std::size_t i = 0; i < 3; ++i) {
      const auto& guild = m_guilds[i];
      const auto path = "/icons/" + guild.id + "/a_hover.gif?size=64";
      const auto& moving = m_images.at(path);
      const bool playing = (step < 3 && i == step) || (step == 5 && i == 0);
      require(
          (moving.playingDraw == m_animationDraw) == playing,
          "Only the hovered server icon may schedule animation, including the selected server"
      );
      if (step != 3) {
        const auto texture = playing ? moving.texture.id : m_images.at(guild.icon).texture.id;
        const auto target = std::ranges::find_if(m_hits, [&](const auto& hit) {
          return hit.action == Action::Guild && hit.id == guild.id;
        });
        require(target != m_hits.end(), "Hover replay has a measured server hit");
        const auto rendered = [&](auto&& self, const Node& node, float x, float y) -> bool {
          x += node.x();
          y += node.y();
          if (node.type() == NodeType::Image
              && x == target->x
              && y == target->y
              && node.width() == layout::Avatar::rail
              && static_cast<const ImageNode&>(node).textureId() == texture)
            return true;
          return std::ranges::any_of(node.children(), [&](const auto& child) { return self(self, *child, x, y); });
        };
        require(
            std::ranges::any_of(m_scene->children(), [&](const auto& node) { return rendered(rendered, *node, 0, 0); }),
            "Idle server icons use their still image and switch back immediately when the pointer leaves"
        );
      }
    }
    if (step < 3) {
      require(
          m_guildTooltipBounds.width > 0 && m_guildTooltipBounds.id == std::to_string(step + 1),
          "Hover card follows the actual server pointer target"
      );
      if (step == 0) {
        const auto hasText = [&](auto&& self, const Node& node, std::string_view value) -> bool {
          if (node.type() == NodeType::Text
              && static_cast<const TextNode&>(node).text().find(value) != std::string::npos)
            return true;
          return std::ranges::any_of(node.children(), [&](const auto& child) { return self(self, *child, value); });
        };
        const auto rendered = [&](std::string_view value) {
          return std::ranges::any_of(m_scene->children(), [&](const auto& node) {
            return hasText(hasText, *node, value);
          });
        };
        require(
            rendered("New activity in #" + m_channels.front().name) && !rendered("staff-only"),
            "Hover card activity omits channels the account cannot view"
        );
      }
      require(
          m_guildTooltipBounds.x >= 78 && m_guildTooltipBounds.y >= 44,
          "Hover card stays clear of the server icon and titlebar"
      );
      if (step < 2)
        move(std::to_string(step + 2));
      else
        openSettings();
    } else if (step == 3) {
      require(m_guildTooltipBounds.width == 0, "Settings suppresses server hover cards");
      m_settingsOpen = false;
      m_window.motion(107, 65 + m_window.titlebarHeight() - layout::Window::titlebarHeight);
    } else if (step == 4) {
      require(
          m_guildTooltipBounds.width == 0 && m_tooltip == "Community server",
          "The server title badge has its own tooltip"
      );
      for (const auto& update : api::parseGuildUpdates(
               "GUILD_UPDATE",
               {{"id", "1"},
                {"features", nlohmann::json::array()},
                {"member_count", nullptr},
                {"online_count", nullptr}}
           ))
        update.apply(m_guilds.front());
      move("1");
    } else if (step == 5) {
      require(
          m_guildTooltipBounds.height == 60 && api::guildBadge(m_guilds.front()) == api::GuildBadge::None,
          "Live cleared counts and badges disappear without invented zero values"
      );
      m_window.motion(chatLeft() + 100, 180 + m_window.titlebarHeight() - layout::Window::titlebarHeight);
    } else if (step == 6)
      require(m_guildTooltipBounds.width == 0, "Leaving the server rail dismisses its card");
    std::clog << "Offline server hover replay step " << step + 1 << " passed\n";
    m_window.invalidate();
  }
} // namespace app
