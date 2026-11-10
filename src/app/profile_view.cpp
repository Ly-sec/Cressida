#include "app/client_application.h"
#include "app/theme.h"
#include "render/core/texture_manager.h"
#include "ui/controls/scrollbar.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;

  void ClientApplication::drawProfileCard(Node& root, float w, float h) {
    m_tooltip.clear();
    const auto& profile = m_profileCard;
    const auto& user = m_dialogAccount;
    const bool own = user.id == m_account.id;
    const int relation = m_userDirectory.relationship(user.id);
    const auto* frame =
        m_preferences.showProfileFrames && !profile.privateProfile && profile.frame && profile.frame->innerWidth > 0
        ? &*profile.frame
        : nullptr;
    const float sideRatio = frame ? frame->overflowHorizontal / frame->innerWidth : 0;
    const float width = std::min(layout::Profile::width, (w - Style::spaceXl) / (1 + sideRatio * 2)),
                inset = layout::Profile::inset, inner = width - inset * 2;
    const float frameScale = frame ? width / frame->innerWidth : 0;
    const float frameTop = frame ? frame->overflowTop * frameScale : 0;
    const float frameBottom = frame ? frame->overflowBottom * frameScale : 0;
    const float frameSide = width * sideRatio;
    const auto measure = [&](std::string_view text, const MessageStyle& style) {
      return m_window.target()
          .renderer()
          .measureText(
              messageMarkup(text, style), style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
              TextEllipsize::End, true
          )
          .width;
    };
    const auto bio = layoutMessage(
        user.bio, inner - Style::spaceSm, measure, Style::fontSizeCaption, {},
        {.compact = true,
         .unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); },
         .discordTimestamps = true}
    );
    const auto roles = m_roleDirectory.roles(profile.guild, user.id, profile.roles);
    const bool bioCut = !m_profileExpanded && bio.height > 72;
    const float bioHeight = bioCut ? layout::Profile::maximumBioHeight : bio.height;
    const float bannerHeight =
        h < layout::Profile::compactHeight ? layout::Profile::compactBannerHeight : layout::Profile::bannerHeight;
    const float bodyStart = bannerHeight
        + (user.pronouns.empty() ? layout::Profile::identityHeight : layout::Profile::pronounIdentityHeight);
    const float bioSection = m_dialogPending ? 64
        : !m_dialogError.empty()             ? 84
        : profile.privateProfile             ? 38
        : user.bio.empty()                   ? 0
                                             : bioHeight + (bio.height > 72 ? 32 : 14);
    const bool mutuals = !own && (profile.mutualFriendCount || profile.mutualGuildCount);
    const float roleTop = bioSection + (mutuals ? 46 : 0);
    struct Chip {
      const api::ColoredRole* role;
      float x, y, width;
    };
    std::vector<Chip> chips;
    float chipX = 0, chipY = roleTop + (Style::spaceXl - Style::spaceXxs);
    const auto shown = m_profileExpanded ? roles.size() : std::min<std::size_t>(roles.size(), 6);
    for (std::size_t i = 0; i < shown; ++i) {
      const bool icon = !roles[i].icon.empty() || !roles[i].unicodeEmoji.empty();
      const auto size = std::min(
          inner - Style::spaceSm,
          m_window.target().renderer().measureText(roles[i].name, Style::fontSizeDescription).width
              + 29
              + (icon ? 20 : 0)
      );
      if (chipX > 0 && chipX + size > inner - Style::spaceSm) {
        chipX = 0;
        chipY += Style::spaceWide;
      }
      chips.push_back({&roles[i], chipX, chipY, size});
      chipX += size + 6;
    }
    const float roleHeight =
        chips.empty() ? 0 : chipY + (Style::spaceXl + Style::spaceXxs) - roleTop + (shown < roles.size() ? 30 : 0);
    const float activityTop = std::max(20.0F, roleTop + roleHeight);
    const float activityHeight = profile.privateProfile
        ? 0
        : drawActivities(nullptr, {0, activityTop, inner - Style::spaceSm, 0}, user.id, {}, "profile");
    const float bodyHeight = activityTop + activityHeight;
    const float height = std::min(
        h - layout::Profile::viewportReserve - frameTop - frameBottom,
        bodyStart + bodyHeight + layout::Profile::footerHeight
    );
    const float x = std::clamp(
        m_profileAnchorX < 0           ? (w - width) / 2
            : m_profileAnchorX < w / 2 ? m_profileAnchorX + Style::spaceLg
                                       : m_profileAnchorX - width - 16,
        Style::spaceMd + frameSide, std::max(Style::spaceMd + frameSide, w - width - Style::spaceMd - frameSide)
    );
    const float y = std::clamp(
        m_profileAnchorY < 0 ? (h - height) / 2 : m_profileAnchorY - layout::Profile::anchorOffset,
        Style::controlHeightLg + frameTop,
        std::max(Style::controlHeightLg + frameTop, h - height - Style::spaceMd - frameBottom)
    );
    const float viewport = std::max(1.0F, height - bodyStart - layout::Profile::footerHeight);
    m_scrolling.limit(m_profileScroll, bodyHeight - viewport);
    m_dialogBounds = {x, y, width, height, Action::None, {}};
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, Color{0, 0, 0, 0.24F});
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::None, {}});
    rectangle(
        root, x - Style::spaceXxs, y + Style::spaceXs, width + Style::spaceXs, height + Style::spaceXxs,
        Color{0, 0, 0, 0.3F}, 17
    );
    drawProfileCosmetics(root, {x, y, width, height}, profile, false, layout::Profile::radius);
    const auto bodyColor = m_preferences.showProfileThemes && profile.theme
        ? lerpColor(sidebar, rgbHex((*profile.theme)[1]), Style::profileThemeBlend)
        : sidebar;
    rectangle(root, x, y, width, height, lerpColor(bodyColor, muted, 0.2F), layout::Profile::radius);
    rectangle(
        root, x + Style::borderWidth, y + Style::borderWidth, width - Style::spaceXxs, height - Style::spaceXxs,
        bodyColor, layout::Profile::radius - Style::borderWidth
    );
    m_hits.push_back(m_dialogBounds);
    const auto bannerColor = profile.accent                ? rgbHex(*profile.accent)
        : m_preferences.showProfileThemes && profile.theme ? rgbHex((*profile.theme)[0])
                                                           : lerpColor(accent, panel, 0.4F);
    // Clip the banner in its own rounded frame; the avatar remains outside it.
    picture(
        root, x + Style::borderWidth, y + Style::borderWidth, width - Style::spaceXxs, bannerHeight + Style::spaceMd,
        m_preferences.showProfileBanners ? profile.banner : "", 14, false, false, ImageFitMode::Cover, {},
        ImageUse::ProfileBanner
    );
    if (!m_preferences.showProfileBanners
        || profile.banner.empty()
        || !m_images.contains(profile.banner)
        || !m_images.at(profile.banner).texture.valid())
      rectangle(
          root, x + Style::borderWidth, y + Style::borderWidth, width - Style::spaceXxs, bannerHeight + Style::spaceMd,
          bannerColor, 14
      );
    rectangle(root, x + Style::borderWidth, y + bannerHeight, width - Style::spaceXxs, 16, bodyColor);
    rectangle(root, x + Style::cardPadding, y + bannerHeight - Style::spaceSection, 96, 96, bodyColor, 48);
    const auto name = userName(user.id, user.name);
    avatar(
        root, x + Style::spaceRelaxed, y + bannerHeight - (Style::space2Xl + Style::spaceXxs), layout::Avatar::profile,
        user.avatar, name, user.id, profile.decoration, layout::Frame{x, y, width, height}
    );
    auto presence = std::make_unique<Node>();
    presence->setPosition(x + 87, y + bannerHeight + (Style::space2Xl - Style::spaceXxs));
    presence->setScale(1.8F);
    statusBadge(*presence, 0, 0, user.id == m_account.id ? m_presence.status : userStatus(user.id), bodyColor);
    root.addChild(std::move(presence));
    rectangle(root, x + width - 42, y + Style::spaceDense, 30, 30, Color{0, 0, 0, 0.42F}, 15);
    iconButton(root, {x + width - 42, y + Style::spaceDense, 30, 30, Action::CloseDialog, {}}, "x", "Close profile");
    if (!own && relation != 2 && relation != 4) {
      rectangle(root, x + width - 80, y + Style::spaceDense, 30, 30, Color{0, 0, 0, 0.42F}, 15);
      iconButton(
          root, {x + width - 80, y + Style::spaceDense, 30, 30, Action::ProfileFriend, {}},
          relation == 1 ? "user-check" : "user-plus",
          relation == 1       ? "Remove friend"
              : relation == 3 ? "Accept friend request"
                              : "Add friend"
      );
    }
    for (std::size_t i = 0; i < profile.badges.size(); ++i) {
      const int columns = std::max(1, static_cast<int>((width - 138) / 24));
      const float bx = x + 124 + (i % columns) * 24, by = y + bannerHeight + Style::spaceMd + (i / columns) * 24;
      if (by + (Style::spaceXl - Style::spaceXxs) > y + bannerHeight + 58)
        break;
      picture(root, bx, by, 20, 20, profile.badges[i].image, 0, false, false, ImageFitMode::Contain);
      if (hovered(bx, by, 20, 20)) {
        m_tooltip = profile.badges[i].description;
        m_tooltipX = bx;
        m_tooltipY = by;
      }
    }
    roleLabel(
        root, x + inset, y + bannerHeight + 64, inner, name, Style::fontSizeHeader, foreground, FontWeight::Bold,
        user.id, "profile:" + user.id, false, profile.roles
    );
    const auto username = user.username.empty() ? user.name : user.username;
    const auto userTag = m_userDirectory.guildTag(user.id, user.guildTag);
    const float usernameWidth = std::min(
        inner - (userTag ? 100 : 0),
        m_window.target().renderer().measureText(username, Style::fontSizeCaption).width + 2
    );
    label(root, x + inset, y + bannerHeight + 92, usernameWidth, username, Style::fontSizeCaption, foreground);
    const auto tagWidth = drawGuildTag(
        root, x + inset + usernameWidth + Style::spaceSm, y + bannerHeight + 89, inner - usernameWidth - Style::spaceSm,
        userTag, true, panel
    );
    if (usernameWidth + tagWidth + (Style::space2Xl - Style::spaceXxs) < inner)
      drawRoleIcon(
          root, x + inset + usernameWidth + tagWidth + Style::spaceSm, y + bannerHeight + 91, profile.guild, user.id,
          profile.roles
      );
    if (!user.pronouns.empty())
      label(root, x + inset, y + bannerHeight + 115, inner, user.pronouns, Style::fontSizeDescription, muted);

    auto clip = std::make_unique<Node>();
    clip->setPosition(x + inset, y + bodyStart);
    clip->setSize(inner, viewport);
    clip->setClipChildren(true);
    auto content = std::make_unique<Node>();
    content->setPosition(0, -m_profileScroll);
    content->setSize(inner, bodyHeight);
    auto* body = content.get();
    clip->addChild(std::move(content));
    root.addChild(std::move(clip));
    if (!profile.privateProfile)
      drawActivities(
          body, {0, activityTop, inner - Style::spaceSm, activityHeight}, user.id,
          {x + inset, y + bodyStart, inner, viewport}, "profile"
      );
    auto bodyHit = [&](float bx, float by, float bw, float bh, Action action, std::string id = {}) {
      const float top = std::max(y + bodyStart, y + bodyStart + by - m_profileScroll);
      const float bottom = std::min(y + bodyStart + viewport, y + bodyStart + by - m_profileScroll + bh);
      if (bottom > top)
        m_hits.push_back({x + inset + bx, top, bw, bottom - top, action, std::move(id)});
    };
    if (m_dialogPending) {
      rectangle(*body, 0, 3, inner - Style::spaceSm, 11, lerpColor(bodyColor, muted, 0.16F), Style::controlRadius);
      rectangle(*body, 0, 24, inner * 0.76F, 11, lerpColor(bodyColor, muted, 0.12F), Style::controlRadius);
      rectangle(*body, 0, 45, inner * 0.45F, 11, lerpColor(bodyColor, muted, 0.1F), Style::controlRadius);
    } else if (!m_dialogError.empty()) {
      label(*body, 0, 0, inner - Style::spaceSm, "Could not load this profile.", Style::fontSizeCaption, muted);
      label(*body, 0, 21, inner - Style::spaceSm, m_dialogError, Style::fontSizeMini, muted, FontWeight::Normal, 2);
      label(*body, 0, 62, inner, "Try again", Style::fontSizeDescription, accent, FontWeight::Medium);
      bodyHit(0, 57, inner, 26, Action::ProfileRetry);
    } else if (profile.privateProfile) {
      label(*body, 0, 0, inner - Style::spaceSm, "This profile is private.", Style::fontSizeCaption, muted);
    } else if (!user.bio.empty()) {
      auto bioClip = std::make_unique<Node>();
      bioClip->setSize(inner - Style::spaceSm, bioHeight);
      bioClip->setClipChildren(true);
      auto* text = bioClip.get();
      body->addChild(std::move(bioClip));
      for (const auto& box : bio.boxes) {
        if (box.inlineCode) {
          rectangle(*text, box.x, box.y, box.width, box.height, withAlpha(muted, .2F), Style::radiusSm);
          rectangle(
              *text, box.x + Style::borderWidth, box.y + Style::borderWidth, box.width - Style::spaceXxs,
              box.height - Style::spaceXxs, panel, Style::radiusXs
          );
        } else
          rectangle(
              *text, box.x, box.y, box.width, box.height, box.quote ? muted : panel,
              box.quote ? 0 : Style::controlRadius
          );
      }
      if (!m_profileRevealSpoilers)
        for (const auto& area : messageSpoilerAreas(bio)) {
          rectangle(*text, area.x, area.y, area.width, area.height, raised, Style::radiusSm);
          if (area.y < bioHeight)
            bodyHit(area.x, area.y, area.width, std::min(area.height, bioHeight - area.y), Action::ProfileSpoiler);
        }
      const auto links = messageLinkAreas(bio);
      const bool bodyHovered = hovered(x + inset, y + bodyStart, inner, viewport);
      std::size_t runIndex = 0;
      for (const auto& run : bio.runs) {
        auto style = run.style;
        style.linkHovered = bodyHovered
            && messageLinkHovered(
                                links, runIndex, m_pointerX - x - inset, m_pointerY - y - bodyStart + m_profileScroll
            );
        ++runIndex;
        const bool hidden = run.style.spoiler && !m_profileRevealSpoilers;
        if (hidden)
          continue;
        messageTimestampHover(*text, run, run.x, run.y);
        if (!run.image.empty())
          emojiImage(*text, run.x, run.y, run.width, run.image);
        else
          label(
              *text, run.x, run.y, run.width + Style::spaceXxs, messageMarkup(run.text, style, false), run.style.size,
              run.style.syntax != CodeToken::Plain ? syntaxColor(run.style.syntax)
                  : run.style.muted                ? muted
                  : run.style.link.empty()         ? foreground
                                                   : accent,
              FontWeight::Normal, 1, true
          );
      }
      drawMessageStrikes(*text, bio, 0, 0, foreground, m_profileRevealSpoilers);
      for (const auto& area : links)
        if ((!bio.runs[area.first].style.spoiler || m_profileRevealSpoilers) && area.y < bioHeight)
          bodyHit(area.x, area.y, area.width, std::min(area.height, bioHeight - area.y), Action::OpenUrl, area.url);
      if (bio.height > 72) {
        label(
            *body, 0, bioHeight + Style::spaceCompact, inner, m_profileExpanded ? "Show Less" : "View Full Bio",
            Style::fontSizeDescription, accent, FontWeight::Medium
        );
        bodyHit(0, bioHeight, inner, 30, Action::ProfileExpand);
      }
    }
    if (mutuals) {
      const float my = bioSection + 6;
      float textX = 0;
      for (const auto& friendUser : profile.mutualFriends) {
        rectangle(*body, textX - Style::spaceXxs, my - Style::spaceXxs, 26, 26, bodyColor, 13);
        avatar(*body, textX, my, 22, friendUser.avatar, friendUser.name, friendUser.id);
        textX += Style::spaceLg;
      }
      if (textX)
        textX += Style::spaceMd;
      std::string summary;
      if (profile.mutualFriendCount)
        summary = std::to_string(*profile.mutualFriendCount)
            + " mutual "
            + (*profile.mutualFriendCount == 1 ? "friend" : "friends");
      if (profile.mutualGuildCount) {
        if (!summary.empty())
          summary += " · ";
        summary += std::to_string(*profile.mutualGuildCount)
            + " mutual "
            + (*profile.mutualGuildCount == 1 ? "server" : "servers");
      }
      label(*body, textX, my + Style::spaceXs, inner - textX - Style::spaceSm, summary, Style::fontSizeMini, muted);
    }
    if (!chips.empty()) {
      label(
          *body, 0, roleTop + Style::spaceXxs, inner, "SERVER ROLES", Style::fontSizeDetail, muted, FontWeight::SemiBold
      );
      for (const auto& chip : chips) {
        rectangle(*body, chip.x, chip.y, chip.width, 23, lerpColor(bodyColor, muted, 0.14F), Style::hoverRadius);
        rectangle(
            *body, chip.x + (Style::spaceCompact + Style::borderWidth),
            chip.y + (Style::spaceCompact + Style::borderWidth), 9, 9,
            chip.role->colors.primary ? rgbHex(chip.role->colors.primary) : muted, Style::controlRadius
        );
        const bool icon = !chip.role->icon.empty() || !chip.role->unicodeEmoji.empty();
        label(
            *body, chip.x + (Style::spaceXl - Style::spaceXxs), chip.y + Style::spaceXs,
            chip.width - Style::spaceWide - (icon ? 20 : 0), chip.role->name, Style::fontSizeDescription, foreground
        );
        if (!chip.role->icon.empty())
          emojiImage(*body, chip.x + chip.width - 22, chip.y + Style::spaceXs, 15, chip.role->icon);
        else if (!chip.role->unicodeEmoji.empty())
          label(
              *body, chip.x + chip.width - 22, chip.y + (Style::spaceXxs + Style::borderWidth), 19,
              chip.role->unicodeEmoji, Style::fontSizeBody, foreground
          );
      }
      if (shown < roles.size()) {
        const float moreY = chipY + Style::spaceWide;
        label(
            *body, 0, moreY + (Style::spaceXxs + Style::borderWidth), inner,
            "Show all " + std::to_string(roles.size()) + " roles", Style::fontSizeDescription, accent
        );
        bodyHit(0, moreY, inner, 28, Action::ProfileExpand);
      }
    }
    if (bodyHeight > viewport) {
      const std::string id = "profile-scrollbar";
      std::unique_ptr<Node> owned;
      if (auto stored = m_detachedInputs.find(id); stored != m_detachedInputs.end()) {
        owned = std::move(stored->second);
        m_detachedInputs.erase(stored);
      } else {
        auto bar = std::make_unique<Scrollbar>();
        bar->setTrackInset(4);
        bar->setOnScrollChanged([this](float offset) {
          m_scrolling.stop(m_profileScroll);
          m_profileScroll = offset;
          m_window.invalidate();
        });
        owned = std::move(bar);
      }
      auto* bar = static_cast<Scrollbar*>(owned.get());
      m_widgets[id] = bar;
      bar->setPosition(x + width - Style::spaceMd, y + bodyStart);
      bar->setSize(bar->reservedThickness(), viewport);
      bar->update(viewport, bodyHeight, m_profileScroll);
      root.addChild(std::move(owned));
    }
    rectangle(root, x + inset, y + height - 58, inner, 1, lerpColor(bodyColor, muted, 0.16F));
    const bool blocked = relation == 2;
    button(
        root,
        {x + inset,
         y + height - 46,
         inner,
         34,
         own ? Action::ProfileEdit
             : blocked ? Action::ProfileUnblock
                       : Action::ProfileMessage,
         {}},
        own ? "Edit Profile"
            : blocked ? "Unblock"
                      : "Message",
        true, false, true
    );
    drawProfileCosmetics(root, {x, y, width, height}, profile, true, layout::Profile::radius);
  }

  bool ClientApplication::profileAction(const Hit& hit) {
    if (hit.action != Action::ProfileExpand
        && hit.action != Action::ProfileEdit
        && hit.action != Action::ProfileMessage
        && hit.action != Action::ProfileFriend
        && hit.action != Action::ProfileUnblock
        && hit.action != Action::ProfileRetry
        && hit.action != Action::ProfileSpoiler)
      return false;
    if (m_dialogKind != "profile")
      return true;
    if (hit.action == Action::ProfileExpand) {
      m_profileExpanded = !m_profileExpanded;
      m_scrolling.stop(m_profileScroll);
      m_profileScroll = 0;
    } else if (hit.action == Action::ProfileSpoiler) {
      m_profileRevealSpoilers = true;
    } else if (hit.action == Action::ProfileRetry) {
      const auto user = m_dialogAccount.id;
      showProfile(user);
    } else if (hit.action == Action::ProfileEdit) {
      if (m_dialogAccount.id == m_account.id) {
        closeDialog();
        openSettings();
        selectSettingsPage("profile");
      }
    } else {
      const auto user = m_dialogAccount.id;
      const auto relation = m_userDirectory.relationship(user);
      closeDialog();
      m_contextUser = user;
      m_contextMessage.clear();
      m_contextChannel = m_channelId;
      contextAction(
          hit.action == Action::ProfileMessage       ? Action::OpenDm
              : hit.action == Action::ProfileUnblock ? Action::UnblockUser
              : relation == 1                        ? Action::RemoveFriend
                                                     : Action::AddFriend
      );
    }
    m_window.invalidate();
    return true;
  }
  void ClientApplication::prepareProfileDemo() {
    const auto id = m_dialogAccount.id;
    const bool own = id == m_account.id;
    auto& profile = m_profileCard;
    profile.user = {id, own ? "Alex" : "Maya", "", own ? "alex" : "maya", own ? "Alex" : "Maya"};
    profile.user.pronouns = "they/them";
    profile.user.bio = "Building little things for the **native desktop**.\n\nMy midnight is your <t:1754172000:t>.\n\n"
                       "Always making room for a new idea. 🌙\n\n[Say hello](https://discord.com).";
    if (m_demoOverlay == "profile-card-long" || m_demoOverlay == "profile-card-cycle")
      for (int i = 0; i < 12; ++i)
        profile.user.bio +=
            "\n\nAnother quiet orbit. A longer biography stays inside the card, with its own scroll area.";
    profile.guild = m_guildId;
    profile.banner = "demo-preview";
    profile.accent = 0xBD78B1;
    profile.theme = std::array<std::uint32_t, 2>{0xBD78B1, 0x40508B};
    profile.guildTag = "MOON";
    profile.guildBadge = "demo-profile-badge";
    profile.user.guildTag = api::GuildTag{"99", "MOON", "demo-profile-badge"};
    std::vector<unsigned char> badge(32 * 32 * 4);
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 32; ++x) {
        const int at = (y * 32 + x) * 4;
        badge[at] = 220;
        badge[at + 1] = 200;
        badge[at + 2] = 255;
        badge[at + 3] = std::abs(x - Style::spaceLg) + std::abs(y - Style::spaceLg) < 13 ? 255 : 0;
      }
    m_images["demo-profile-badge"].texture = m_window.context().textureManager().loadFromRgba(badge.data(), 32, 32);
    profile.badges = {
        {"Synthetic supporter badge", "demo-profile-badge"}, {"Synthetic community badge", "demo-profile-badge"}
    };
    profile.mutualFriendCount = 2;
    profile.mutualGuildCount = 4;
    profile.mutualFriends = {{"12", "Noah"}, {"13", "Sam"}};
    const std::vector<std::string> names = {"Moonwarden", "Nightlight", "Artists",   "Developers",
                                            "Stargazer",  "Friends",    "Designers", "Community"};
    const std::vector<std::uint32_t> colors = {0xB498F0, 0x6DBBC7, 0xD986AF, 0x78B591, 0xD6AA6A, 0, 0x7A92CE, 0};
    api::GuildRoleUpdate roles;
    roles.guild = m_guildId;
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < names.size(); ++i) {
      const auto roleId = std::to_string(50 + i);
      roles.roles.push_back({roleId, static_cast<int>(names.size() - i), {colors[i]}, names[i]});
      if (i == 0)
        roles.roles.back().icon = "demo-profile-badge";
      if (i == 1)
        roles.roles.back().unicodeEmoji = "🌙";
      ids.push_back(roleId);
    }
    roles.members[id] = ids;
    m_roleDirectory.apply({false, {std::move(roles)}});
    profile.roles = ids;
    m_dialogAccount = profile.user;
    api::UserUpdates users;
    users.users[id].relationship = own ? 0 : 1;
    users.users[id].status = "online";
    m_userDirectory.apply(users);
    m_profileAnchorX = m_profileAnchorY = -1;
    if (m_demoOverlay == "profile-card-long")
      m_profileExpanded = true;
    if (m_demoOverlay == "profile-card-loading")
      m_dialogPending = true;
    if (m_demoOverlay == "profile-card-error")
      m_dialogError = "The connection was interrupted.";
    if (!own) {
      api::Channel dm;
      dm.id = "900";
      dm.name = "Maya";
      dm.type = 1;
      dm.recipients = {profile.user};
      m_privateChannels = {dm};
    }
  }

  void ClientApplication::demoProfileStep() {
    const auto require = [](bool ok, const char* message) {
      if (!ok)
        throw std::runtime_error(message);
    };
    const auto step = m_demoInteractionStep++;
    auto action = [&](Action kind) {
      const auto at = std::ranges::find(m_hits, kind, &Hit::action);
      require(at != m_hits.end(), "Profile action must have a visible hit region");
      const auto hit = *at;
      activate(hit);
    };
    if (step == 0) {
      require(
          m_dialogKind == "profile" && !m_dialogPending && m_dialogBounds.width <= 370 && !m_profileExpanded,
          "Profiles open as a compact card with bounded geometry"
      );
      action(Action::ProfileExpand);
    } else if (step == 1) {
      require(
          m_profileExpanded && m_widgets.contains("profile-scrollbar"),
          "Long biographies expand inside a retained scrolling viewport"
      );
      m_scrolling.scroll(m_profileScroll, 600, true);
    } else if (step == 2) {
      require(m_profileScroll > 0, "Expanded profiles can scroll without moving the conversation");
      const auto old = *m_dialogAction;
      showProfile(m_account.id);
      api::Event stale;
      stale.interaction = old;
      stale.account = {"11", "Stale result"};
      interactionResult(stale);
      require(m_dialogAccount.id == m_account.id, "Late responses cannot replace a different open profile");
    } else if (step == 3) {
      action(Action::ProfileEdit);
    } else if (step == 4) {
      require(
          m_settingsOpen && m_settingsPage == "profile" && m_dialogKind.empty(),
          "Edit Profile opens the existing native profile settings"
      );
      activate({0, 0, 0, 0, Action::CloseSettings, {}});
      showProfile("11");
    } else if (step == 5) {
      action(Action::ProfileFriend);
      require(
          m_dialogKind == "confirm" && m_dialogAction->kind == api::InteractionKind::RemoveFriend,
          "Removing a friend retains the existing explicit confirmation"
      );
      closeDialog();
      showProfile("11");
      m_dialogPending = true;
      overlayKey({.symbol = XKB_KEY_Escape});
      require(m_dialogKind.empty(), "Escape can dismiss a profile while it is loading");
      showProfile("11");
      const auto old = *m_dialogAction;
      showProfile("11");
      api::Event stale;
      stale.interaction = old;
      stale.account = {"11", "Stale result"};
      interactionResult(stale);
      require(m_dialogAccount.name == "Maya", "Reopening the same user rejects the previous request revision");
      m_dialogError = "Synthetic offline error";
    } else if (step == 6) {
      action(Action::ProfileRetry);
      require(m_dialogError.empty() && m_dialogAccount.id == "11", "Profile errors offer an explicit retry");
    } else if (step == 7) {
      action(Action::ProfileMessage);
      require(
          m_dmMode && m_channelId == "900" && m_dialogKind.empty(),
          "Message reuses an existing DM without sending anything"
      );
      std::clog << "Offline profile card replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
