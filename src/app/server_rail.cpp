#include "app/client_application.h"
#include "app/theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace app {
  std::string ClientApplication::initial(const std::string& name) {
    if (name.empty())
      return "?";
    std::size_t end = 1;
    while (end < name.size() && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80)
      ++end;
    auto value = name.substr(0, end);
    if (end == 1)
      value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
    return value;
  }
  FolderMotion::Clock::time_point ClientApplication::folderMotionTime() const {
    if (m_demo && m_demoOverlay.starts_with("folder-animation"))
      return FolderMotion::Clock::time_point{} + FolderMotion::duration * m_demoInteractionStep / 4;
    return FolderMotion::Clock::now();
  }
  void ClientApplication::drawServerRail(Node& root, float bottom) {
    using R = layout::ServerRail;
    m_railTargets.clear();
    m_railBottom = bottom;
    std::map<std::string, int> mentions;
    for (const auto& [channel, guild] : m_unreadGuilds)
      if (const auto count = m_unreadTimeline.mentions(channel);
          !guild.empty() && count > 0 && channelActivityVisible(guild, channel))
        mentions[guild] = std::min(1000000, mentions[guild] + count);
    struct Row {
      const api::Guild* guild = nullptr;
      const api::GuildFolder* folder = nullptr;
      const api::Channel* dm = nullptr;
      float top, height, clipTop, clipBottom, progress = 0;
    };
    struct Group {
      const api::GuildFolder* folder;
      float top, height, header, progress;
    };
    std::vector<Row> rows;
    std::vector<Group> groups;
    float cursor = R::top;
    for (const auto& dm : m_privateChannels)
      if (!dm.isMessageRequest()
          && m_unreadDMs.contains(dm.id)
          && (!m_userDirectory.muted(dm.id) || m_unreadTimeline.mentions(dm.id) > 0)) {
        rows.push_back(
            {.dm = &dm, .top = cursor, .height = R::step, .clipTop = cursor, .clipBottom = cursor + R::step}
        );
        cursor += R::step;
      }
    const auto unreadDMRows = rows.size();
    m_folderMotion.begin();
    for (const auto& folder : m_folders) {
      std::vector<const api::Guild*> children;
      for (const auto& id : folder.guildIds)
        if (const auto at = std::ranges::find(m_guilds, id, &api::Guild::id); at != m_guilds.end())
          children.push_back(&*at);
      if (folder.id.empty()) {
        for (const auto* guild : children) {
          rows.push_back(
              {.guild = guild,
               .top = cursor,
               .height = R::rowHeight,
               .clipTop = cursor,
               .clipBottom = cursor + R::rowHeight}
          );
          cursor += R::step;
        }
        continue;
      }
      const float p = m_folderMotion.set(
          folder.id, m_openFolders.contains(folder.id), !m_preferences.reducedMotion, folderMotionTime()
      );
      const auto frame = R::folderFrame(children.size(), p);
      groups.push_back({&folder, cursor, frame.height, frame.headerHeight, p});
      rows.push_back(
          {.folder = &folder,
           .top = cursor,
           .height = frame.headerHeight,
           .clipTop = cursor,
           .clipBottom = cursor + frame.headerHeight,
           .progress = p}
      );
      const float childTop = cursor + frame.headerHeight;
      for (std::size_t i = 0; i < children.size(); ++i)
        rows.push_back(
            {.guild = children[i],
             .folder = &folder,
             .top = childTop + i * R::folderChildStep,
             .height = R::rowHeight,
             .clipTop = childTop,
             .clipBottom = childTop + frame.childrenHeight}
        );
      cursor += frame.height + R::folderGap;
    }
    const float addTop = cursor;
    cursor += R::step;
    m_folderMotion.end();
    if (m_folderMotion.active())
      m_window.invalidate();
    if (m_revealGuild) {
      m_revealGuild = false;
      for (const auto& row : rows)
        if (row.guild && row.guild->id == m_guildId && row.clipBottom > row.top) {
          const float end = std::min(row.top + row.height, row.clipBottom);
          if (row.top - m_guildScroll < R::visibleTop)
            m_guildScroll = row.top - R::visibleTop;
          if (end - m_guildScroll > bottom)
            m_guildScroll = end - bottom;
          break;
        }
    }
    m_scrolling.limit(m_guildScroll, std::max(0.0F, cursor - bottom));
    const auto clip = [&](float top, float end) -> Node* {
      top = std::max(top - m_guildScroll, R::visibleTop);
      end = std::min(end - m_guildScroll, bottom);
      if (end <= top)
        return nullptr;
      auto node = std::make_unique<Node>();
      node->setPosition(0, top);
      node->setSize(layout::Sidebar::railWidth, end - top);
      node->setClipChildren(true);
      node->setTextScrollOffset(-m_guildScroll);
      return root.addChild(std::move(node));
    };
    for (const auto& group : groups)
      if (auto* node = clip(group.top, group.top + group.height)) {
        const bool over = hovered(R::folderX, group.top - m_guildScroll, R::folderSize, group.header);
        const auto tint = group.folder->color ? rgbHex(group.folder->color) : theme::accent;
        const auto closed = lerpColor(over ? theme::raised : theme::panel, tint, Style::serverFolderTintMix);
        rectangle(
            *node, R::folderX, group.top - m_guildScroll - node->y(), R::folderSize, group.height,
            lerpColor(closed, theme::panel, group.progress), Style::radiusXl
        );
      }
    for (std::size_t index = 0; index < rows.size(); ++index) {
      const auto& row = rows[index];
      const float rowTop = std::max(row.top, row.clipTop);
      const float rowBottom = std::min(row.top + row.height, row.clipBottom);
      auto* node = clip(rowTop, rowBottom);
      if (!node)
        continue;
      const float y = row.top - m_guildScroll, py = y - node->y();
      const auto addHit = [&](Hit hit, bool context = false) {
        const auto end = std::min(hit.y + hit.height, node->y() + node->height());
        hit.y = std::max(hit.y, node->y());
        hit.height = end - hit.y;
        if (hit.height > 0) {
          if (context)
            m_contextTargets.push_back(std::move(hit));
          else {
            if (hit.action == Action::Guild || hit.action == Action::Folder)
              m_railTargets.push_back(hit);
            m_hits.push_back(std::move(hit));
          }
        }
      };
      if (row.dm) {
        const auto& dm = *row.dm;
        avatar(
            *node, R::iconX, py, layout::Avatar::rail, dm.icon, channelName(dm),
            dm.type == 1 && !dm.recipients.empty() ? dm.recipients.front().id : ""
        );
        if (dm.type == 1 && !dm.recipients.empty() && m_unreadTimeline.mentions(dm.id) == 0)
          statusBadge(
              *node, R::iconX + layout::Avatar::rail - Style::spaceMd - Style::spaceXxs,
              py + layout::Avatar::rail - Style::spaceMd - Style::spaceXxs, userStatus(dm.recipients.front().id),
              theme::railBackground, dm.recipients.front().id, dm.id
          );
        rectangle(
            *node, R::markerX, py + (layout::Avatar::rail - Style::spaceLg) / 2, Style::spaceXs, Style::spaceLg,
            theme::foreground, Style::radiusXs
        );
        if (hovered(
                R::iconX, std::max(y, node->y()), layout::Avatar::rail,
                std::min(y + layout::Avatar::rail, node->y() + node->height()) - std::max(y, node->y())
            ))
          rectangle(
              *node, layout::Sidebar::railWidth - R::markerX - Style::spaceXs,
              py + (layout::Avatar::rail - Style::spaceRelaxed) / 2, Style::spaceXs, Style::spaceRelaxed, theme::accent,
              Style::radiusXs
          );
        countBadge(*node, R::badgeRight, py + R::badgeTop(R::rowHeight), m_unreadTimeline.mentions(dm.id), true);
        addHit({R::iconX, y, layout::Avatar::rail, layout::Avatar::rail, Action::DirectMessage, dm.id});
        if (index + 1 == unreadDMRows)
          rectangle(
              *node, (layout::Sidebar::railWidth - Style::space2Xl) / 2,
              py + layout::Avatar::rail + Style::spaceCompact, Style::space2Xl, Style::borderWidth, theme::raised
          );
        continue;
      }
      if (!row.guild) {
        const auto& folder = *row.folder;
        if (row.progress < 1) {
          auto miniatures = std::make_unique<Node>();
          miniatures->setOpacity(1 - row.progress);
          auto* mini = node->addChild(std::move(miniatures));
          for (std::size_t j = 0; j < std::min<std::size_t>(4, folder.guildIds.size()); ++j)
            if (const auto at = std::ranges::find(m_guilds, folder.guildIds[j], &api::Guild::id); at != m_guilds.end())
              avatar(
                  *mini, R::folderX + R::folderInset + (j % 2) * R::folderAvatarStep,
                  py + R::folderInset + (j / 2) * R::folderAvatarStep, R::folderAvatarSize, at->icon, at->name
              );
        }
        if (row.progress > 0) {
          auto symbol = std::make_unique<Node>();
          symbol->setOpacity(row.progress);
          auto* icon = node->addChild(std::move(symbol));
          glyph(
              *icon, (layout::Sidebar::railWidth - R::folderGlyphSize) / 2, py + (row.height - R::folderGlyphSize) / 2,
              R::folderGlyphSize, "folder-filled", folder.color ? rgbHex(folder.color) : theme::accent
          );
        }
        int count = 0;
        for (const auto& guild : folder.guildIds)
          count = std::min(1000000, count + mentions[guild]);
        countBadge(*node, R::badgeRight, py + R::badgeTop(row.height), count, true);
        addHit({R::folderX, y, R::folderSize, row.height, Action::Folder, folder.id});
        addHit({R::folderX, y, R::folderSize, row.height, Action::Folder, folder.id}, true);
        continue;
      }
      const auto& guild = *row.guild;
      const bool selected = guild.id == m_guildId;
      const float hitTop = std::max(y, node->y());
      const bool over = hovered(
          R::iconX, hitTop, layout::Avatar::rail,
          std::min(y + layout::Avatar::rail, node->y() + node->height()) - hitTop
      );
      if (!row.folder && !selected)
        rectangle(
            *node, R::iconX, py, layout::Avatar::rail, layout::Avatar::rail, over ? theme::panel : theme::raised,
            over ? Style::radiusXl : layout::Avatar::rail / 2
        );
      if (!image(
              *node, R::iconX, py, layout::Avatar::rail, guild.icon,
              selected || over ? Style::radiusXl : layout::Avatar::rail / 2
          )) {
        if (row.folder)
          rectangle(*node, R::iconX, py, layout::Avatar::rail, layout::Avatar::rail, theme::raised, Style::radiusXl);
        const auto text = initial(guild.name);
        const auto metrics =
            m_window.target().renderer().measureText(text, Style::fontSizeDialog, FontWeight::SemiBold);
        label(
            *node, R::iconX + (layout::Avatar::rail - metrics.width) / 2,
            py + (layout::Avatar::rail - (metrics.bottom - metrics.top)) / 2, metrics.width + Style::borderWidth, text,
            Style::fontSizeDialog, selected ? theme::accent : theme::foreground, FontWeight::SemiBold
        );
      }
      const bool unread =
          !m_userDirectory.muted(guild.id) && std::ranges::any_of(m_unreadGuilds, [&](const auto& state) {
            return state.second == guild.id
                && m_unreadChannels.contains(state.first)
                && !channelMuted(state.first)
                && unreadDotVisible(guild.id, state.first);
          });
      if (selected || over || unread)
        rectangle(
            *node, R::markerX, py + (layout::Avatar::rail - (selected ? Style::spaceRelaxed : Style::spaceDense)) / 2,
            Style::spaceXs, selected ? Style::spaceRelaxed : Style::spaceDense,
            selected || over ? theme::accent : theme::foreground, Style::radiusXs
        );
      countBadge(*node, R::badgeRight, py + R::badgeTop(R::rowHeight), mentions[guild.id], true);
      addHit({R::iconX, y, layout::Avatar::rail, layout::Avatar::rail, Action::Guild, guild.id});
      addHit({R::iconX, y, layout::Avatar::rail, layout::Avatar::rail, Action::GuildMenu, guild.id}, true);
    }
    if (auto* node = clip(addTop, addTop + layout::Avatar::rail)) {
      const float py = addTop - m_guildScroll - node->y();
      rectangle(*node, R::iconX, py, layout::Avatar::rail, layout::Avatar::rail, theme::panel, Style::radiusXl);
      iconButton(
          *node, {R::iconX, py, layout::Avatar::rail, layout::Avatar::rail, Action::ServerAccess, "open"}, "plus",
          "Add a server"
      );
    }
    drawRailDrag(root);
  }
} // namespace app
