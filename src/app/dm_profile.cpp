#include "app/client_application.h"
#include "app/reply_preview.h"
#include "app/theme.h"
#include "ui/controls/scrollbar.h"

#include <algorithm>
#include <cmath>

namespace app {
  namespace {
    constexpr std::uint64_t dmProfileRequest = std::uint64_t{1} << 62;
  }
  const api::Account* ClientApplication::dmRecipient() const {
    if (!m_dmMode || !m_dmPage.empty())
      return nullptr;
    const auto* channel = selectedChannel();
    if (!channel || channel->kind() != api::ChannelType::DirectMessage)
      return nullptr;
    const auto recipient = std::ranges::find_if(channel->recipients, [&](const auto& user) {
      return user.id != m_account.id && api::validSnowflake(user.id);
    });
    return recipient == channel->recipients.end() ? nullptr : &*recipient;
  }
  float ClientApplication::memberPaneWidth() const {
    if (voiceStageVisible())
      return 0;
    const float available = m_window.width() - chatLeft();
    if (!m_connected
        || (m_dmMode && !m_dmPage.empty())
        || forumView()
        || !m_channelPanel.empty()
        || !m_showMembers
        || m_window.width() < layout::Members::minimumWindowWidth
        || available < layout::Members::minimumAvailableWidth)
      return 0;
    return dmRecipient() ? std::min(layout::DmProfile::width, available - layout::Sidebar::minimumConversation)
                         : layout::Members::width;
  }
  void ClientApplication::refreshDmProfile(bool retry) {
    const auto* user = dmRecipient();
    if (!user || m_dmProfile.user != user->id || m_dmProfile.channel != m_channelId) {
      m_scrolling.stop(m_dmProfile.scroll);
      m_dmProfile = {};
      if (!user)
        return;
      m_dmProfile.user = user->id;
      m_dmProfile.channel = m_channelId;
    }
    if (memberPaneWidth() <= 0 || m_dmProfile.data || m_dmProfile.request || (!retry && !m_dmProfile.error.empty()))
      return;
    api::Interaction action{
        .kind = api::InteractionKind::UserProfile,
        .channel = m_channelId,
        .user = user->id,
        .revision = dmProfileRequest | ++m_dmProfileSerial
    };
    m_dmProfile.error.clear();
    if (m_demo || startInteraction(action))
      m_dmProfile.request = std::move(action);
    else
      m_dmProfile.error = m_status.empty() ? "Could not load this profile." : m_status;
  }
  bool ClientApplication::dmProfileResult(const api::Event& event) {
    if (!event.interaction
        || event.interaction->kind != api::InteractionKind::UserProfile
        || !(event.interaction->revision & dmProfileRequest))
      return false;
    const auto& action = *event.interaction;
    if (!m_dmProfile.request
        || m_dmProfile.request->revision != action.revision
        || m_dmProfile.user != action.user
        || m_dmProfile.channel != action.channel)
      return true;
    m_dmProfile.request.reset();
    m_dmProfile.error = event.error;
    if (event.error.empty()) {
      if (event.profile && event.profile->user.id == action.user && event.profile->guild.empty()) {
        m_dmProfile.data = event.profile;
        api::UserUpdates update;
        update.users[action.user].cosmetics = event.profile->user.cosmetics;
        m_userDirectory.apply(update);
      } else
        m_dmProfile.error = "This profile is unavailable.";
    }
    m_window.invalidate();
    return true;
  }
  void ClientApplication::drawCustomStatus(
      Node& root, const layout::Frame& frame, const api::UserCustomStatus& status, float size
  ) {
    if (status.empty() || frame.width <= 0 || frame.height <= 0)
      return;
    auto clip = std::make_unique<Node>();
    clip->setPosition(frame.x, frame.y);
    clip->setSize(frame.width, frame.height);
    clip->setClipChildren(true);
    clip->setHitTestVisible(false);
    auto* content = root.addChild(std::move(clip));
    float x = 0;
    const float symbolSize = std::min(frame.height, size + Style::spaceXxs);
    auto& renderer = m_window.target().renderer();
    if (!status.emojiId.empty()) {
      if (!emojiImage(
              *content, 0, (frame.height - symbolSize) / 2, symbolSize, api::emojiPath(status.emojiId, status.animated)
          ))
        glyph(*content, 0, (frame.height - symbolSize) / 2, symbolSize, "mood-smile", theme::muted);
      x = symbolSize + Style::spaceXs;
    } else if (!status.emojiName.empty()) {
      const auto metrics = renderer.measureText(status.emojiName, symbolSize, FontWeight::Normal, 0, 1);
      const float center = metrics.inkBottom > metrics.inkTop ? (metrics.inkTop + metrics.inkBottom) / 2
                                                              : (metrics.top + metrics.bottom) / 2;
      label(
          *content, 0, frame.height / 2 + metrics.top - center, std::min(frame.width, metrics.width + Style::spaceXxs),
          status.emojiName, symbolSize, theme::muted
      );
      x = metrics.width + Style::spaceXs;
    }
    const auto text = oneLineReplyText(status.text);
    const float width = std::max(0.0F, frame.width - x);
    if (!text.empty() && width > 0) {
      const auto metrics = renderer.measureText(text, size, FontWeight::Normal, width, 1);
      label(*content, x, (frame.height - (metrics.bottom - metrics.top)) / 2, width, text, size, theme::muted);
    }
    float ox = frame.x, oy = frame.y;
    for (const auto* node = &root; node && node != m_scene.get(); node = node->parent()) {
      ox += node->x();
      oy += node->y();
    }
    bool visible = true;
    float nx = ox - frame.x, ny = oy - frame.y;
    for (const auto* node = &root; node && node != m_scene.get(); node = node->parent()) {
      if (node->clipChildren())
        visible &=
            m_pointerX >= nx && m_pointerX < nx + node->width() && m_pointerY >= ny && m_pointerY < ny + node->height();
      nx -= node->x();
      ny -= node->y();
    }
    if (visible && hovered(ox, oy, frame.width, frame.height)) {
      m_tooltip = status.emojiName;
      if (!m_tooltip.empty() && !text.empty())
        m_tooltip += ' ';
      m_tooltip += text;
      m_tooltipX = ox + frame.width / 2;
      m_tooltipY = oy;
    }
  }
  void ClientApplication::drawDmProfile(Node& root, const layout::Frame& frame) {
    refreshDmProfile();
    const auto* recipient = dmRecipient();
    if (!recipient)
      return;
    using P = layout::DmProfile;
    const api::UserProfile fallback{.user = *recipient};
    const auto& profile = m_dmProfile.data ? *m_dmProfile.data : fallback;
    const auto& user = profile.user;
    const auto name = userName(user.id, user.name);
    const float inner = std::max(0.0F, frame.width - P::inset * 2);
    const float viewport = std::max(0.0F, frame.height - P::footerHeight);
    const auto custom = m_userDirectory.customStatus(user.id);
    const auto measure = [&](std::string_view text, const MessageStyle& style) {
      return m_window.target()
          .renderer()
          .measureText(
              messageMarkup(text, style), style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
              TextEllipsize::End, true
          )
          .width;
    };
    const auto bio = profile.privateProfile ? MessageLayout{}
                                            : layoutMessage(
                                                  user.bio, inner, measure, Style::fontSizeCaption, {},
                                                  {.compact = true, .unicodeEmoji = [this](std::string_view text) {
                                                     return m_emojiCatalog.isUnicode(text);
                                                   }}
                                              );
    const float pronouns = user.pronouns.empty() ? 0 : P::metaHeight;
    const float customHeight = custom.empty() ? 0 : P::statusHeight + P::gap;
    float bodyY = P::identityTop + P::nameHeight + P::metaHeight + pronouns + P::sectionGap + customHeight;
    const bool mutuals = !profile.privateProfile && (profile.mutualFriendCount || profile.mutualGuildCount);
    if (mutuals)
      bodyY += P::metaHeight + P::sectionGap;
    const float summaryY = bodyY;
    const auto created = api::accountCreationDate(user.id);
    const float biographyY = summaryY + (created.empty() ? 0 : P::metaHeight * 2 + P::sectionGap);
    const bool notice = m_dmProfile.request || !m_dmProfile.error.empty() || profile.privateProfile;
    const float bodyHeight = std::max(
        P::identityTop + P::nameHeight,
        biographyY
            + (notice                 ? P::nameHeight * 2
                   : user.bio.empty() ? 0
                                      : P::metaHeight + P::gap + bio.height)
            + P::inset
    );
    m_scrolling.limit(m_dmProfile.scroll, std::max(0.0F, bodyHeight - viewport));
    drawProfileCosmetics(root, frame, profile, false);
    floatingPanel(
        root, frame,
        m_preferences.showProfileThemes && profile.theme
            ? lerpColor(theme::background, rgbHex((*profile.theme)[1]), Style::profileThemeBlend)
            : theme::background
    );
    auto clip = std::make_unique<Node>();
    clip->setPosition(frame.x, frame.y);
    clip->setSize(frame.width, viewport);
    clip->setClipChildren(true);
    auto content = std::make_unique<Node>();
    content->setPosition(0, -m_dmProfile.scroll);
    content->setSize(frame.width, bodyHeight);
    auto* body = content.get();
    clip->addChild(std::move(content));
    root.addChild(std::move(clip));
    const auto hit = [&](float x, float y, float width, float height, Action action, const std::string& id,
                         bool context = false) {
      const float top = std::max(frame.y, frame.y + y - m_dmProfile.scroll);
      const float bottom = std::min(frame.y + viewport, frame.y + y + height - m_dmProfile.scroll);
      if (bottom <= top)
        return;
      Hit target{frame.x + x, top, width, bottom - top, action, id};
      if (context)
        m_contextTargets.push_back(std::move(target));
      else
        m_hits.push_back(std::move(target));
    };
    const auto bannerColor = profile.accent ? rgbHex(*profile.accent) : theme::accent;
    if (!m_preferences.showProfileBanners
        || !picture(
            *body, 0, 0, frame.width, P::bannerHeight, profile.banner, 0, false, false, ImageFitMode::Cover, {},
            ImageUse::ProfileBanner
        ))
      rectangle(*body, 0, 0, frame.width, P::bannerHeight, bannerColor);
    rectangle(
        *body, P::inset - P::gap / 2, P::avatarTop - P::gap / 2, P::avatarSize + P::gap, P::avatarSize + P::gap,
        theme::background, (P::avatarSize + P::gap) / 2
    );
    avatar(
        *body, P::inset, P::avatarTop, P::avatarSize, user.avatar, name, user.id, profile.decoration,
        layout::Frame{0, 0, frame.width, bodyHeight}
    );
    auto presence = std::make_unique<Node>();
    presence->setPosition(
        P::inset + P::avatarSize - Style::spaceMd * P::statusScale,
        P::avatarTop + P::avatarSize - Style::spaceMd * P::statusScale
    );
    presence->setScale(P::statusScale);
    statusBadge(*presence, 0, 0, userStatus(user.id), theme::background, user.id, m_channelId);
    body->addChild(std::move(presence));
    hit(P::inset, P::avatarTop, P::avatarSize, P::avatarSize, Action::UserProfile, user.id);
    hit(P::inset, P::avatarTop, P::avatarSize, P::avatarSize, Action::UserMenu, user.id, true);
    label(
        *body, P::inset, P::identityTop, inner, name, Style::fontSizeHeading, theme::foreground, FontWeight::SemiBold
    );
    const auto username = user.username.empty() ? recipient->username : user.username;
    const auto tag = m_userDirectory.guildTag(user.id, user.guildTag);
    const float usernameY = P::identityTop + P::nameHeight;
    const auto usernameWidth =
        std::min(inner, m_window.target().renderer().measureText(username, Style::fontSizeCaption).width);
    label(*body, P::inset, usernameY, inner, username, Style::fontSizeCaption, theme::foreground);
    const float tagWidth = drawGuildTag(
        *body, P::inset + usernameWidth + P::gap, usernameY - Style::spaceXxs, inner - usernameWidth - P::gap, tag,
        false
    );
    if (tag && tagWidth > 0 && api::validSnowflake(tag->guild))
      hit(P::inset + usernameWidth + P::gap, usernameY - Style::spaceXxs, tagWidth - Style::spaceCompact,
          Style::spaceRelaxed, Action::GuildProfile, tag->guild);
    if (!user.pronouns.empty())
      label(*body, P::inset, usernameY + P::metaHeight, inner, user.pronouns, Style::fontSizeDescription, theme::muted);
    const float customY = usernameY + P::metaHeight + pronouns + P::sectionGap;
    if (!custom.empty())
      drawCustomStatus(*body, {P::inset, customY, inner, P::statusHeight}, custom, Style::fontSizeCaption);
    if (mutuals) {
      const auto text = std::to_string(profile.mutualFriendCount.value_or(0))
          + " mutual friends · "
          + std::to_string(profile.mutualGuildCount.value_or(0))
          + " mutual servers";
      label(*body, P::inset, customY + customHeight, inner, text, Style::fontSizeMini, theme::muted);
    }
    if (!created.empty()) {
      label(
          *body, P::inset, summaryY, inner, "Member since", Style::fontSizeDescription, theme::foreground,
          FontWeight::SemiBold
      );
      label(*body, P::inset, summaryY + P::metaHeight, inner, created, Style::fontSizeDescription, theme::muted);
    }
    if (m_dmProfile.request)
      label(*body, P::inset, biographyY, inner, "Loading profile…", Style::fontSizeCaption, theme::muted);
    else if (!m_dmProfile.error.empty()) {
      label(*body, P::inset, biographyY, inner, "Could not load this profile.", Style::fontSizeCaption, theme::muted);
      label(*body, P::inset, biographyY + P::nameHeight, inner, "Try again", Style::fontSizeCaption, theme::accent);
      hit(P::inset, biographyY + P::nameHeight, inner, P::nameHeight, Action::DmProfileRetry, user.id);
    } else if (profile.privateProfile)
      label(*body, P::inset, biographyY, inner, "This profile is private.", Style::fontSizeCaption, theme::muted);
    else if (!user.bio.empty()) {
      label(
          *body, P::inset, biographyY, inner, "About me", Style::fontSizeDescription, theme::foreground,
          FontWeight::SemiBold
      );
      const float bioY = biographyY + P::metaHeight + P::gap;
      for (const auto& box : bio.boxes)
        rectangle(
            *body, P::inset + box.x, bioY + box.y, box.width, box.height, box.quote ? theme::muted : theme::panel,
            box.quote ? 0 : Style::radiusSm
        );
      for (const auto& area : messageSpoilerAreas(bio))
        rectangle(*body, P::inset + area.x, bioY + area.y, area.width, area.height, theme::raised, Style::radiusSm);
      const auto links = messageLinkAreas(bio);
      const bool bodyHovered = hovered(frame.x, frame.y, frame.width, viewport);
      std::size_t runIndex = 0;
      for (const auto& run : bio.runs) {
        auto style = run.style;
        style.linkHovered =
            bodyHovered
            && messageLinkHovered(
                links, runIndex, m_pointerX - frame.x - P::inset, m_pointerY - frame.y + m_dmProfile.scroll - bioY
            );
        ++runIndex;
        if (run.style.spoiler)
          continue;
        if (!run.image.empty())
          emojiImage(*body, P::inset + run.x, bioY + run.y, run.width, run.image);
        else
          label(
              *body, P::inset + run.x, bioY + run.y, run.width + Style::spaceXxs, messageMarkup(run.text, style, false),
              run.style.size,
              run.style.muted              ? theme::muted
                  : run.style.link.empty() ? theme::foreground
                                           : theme::accent,
              FontWeight::Normal, 1, true
          );
      }
      drawMessageStrikes(*body, bio, P::inset, bioY, theme::foreground);
      for (const auto& link : links)
        if (!bio.runs[link.first].style.spoiler)
          hit(P::inset + link.x, bioY + link.y, link.width, link.height, Action::OpenUrl, link.url);
    }
    const float footerY = frame.y + frame.height - P::footerHeight;
    button(
        root, {frame.x + P::inset, footerY + P::inset, inner, Style::controlHeight, Action::UserProfile, user.id},
        "View Full Profile", false, false, true
    );
    if (bodyHeight > viewport) {
      const std::string id = "dm-profile-scrollbar";
      std::unique_ptr<Node> owned;
      if (const auto stored = m_detachedInputs.find(id); stored != m_detachedInputs.end()) {
        owned = std::move(stored->second);
        m_detachedInputs.erase(stored);
      } else {
        auto bar = std::make_unique<Scrollbar>();
        bar->setTrackInset(Style::spaceXs);
        bar->setOnScrollChanged([this](float offset) {
          m_scrolling.stop(m_dmProfile.scroll);
          m_dmProfile.scroll = offset;
          m_window.invalidate();
        });
        owned = std::move(bar);
      }
      auto* bar = static_cast<Scrollbar*>(owned.get());
      m_widgets[id] = bar;
      bar->setPosition(frame.x + frame.width - Style::spaceMd, frame.y);
      bar->setSize(bar->reservedThickness(), viewport);
      bar->update(viewport, bodyHeight, m_dmProfile.scroll);
      root.addChild(std::move(owned));
    }
    drawProfileCosmetics(root, frame, profile, true);
  }
} // namespace app
