#include "app/client_application.h"
#include "app/font_catalog.h"
#include "app/theme.h"
#include "render/core/texture_manager.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;

  void ClientApplication::clearStickers() {
    m_pendingWaveChannel.clear();
    m_stickerTab = m_stickerInfo = false;
    m_stickers.clear();
    m_loadedStickerSources.clear();
    m_loadingStickerSources.clear();
    m_stickerErrors.clear();
    m_stickerGroups.clear();
    m_stickerTargets.clear();
    m_recentStickers.clear();
    m_inspectedSticker.reset();
    m_stickerSection = "all";
    m_stickerFilter.clear();
    m_draftStickers.clear();
    ++m_stickerRevision;
  }

  void ClientApplication::prepareStickers() {
    if (m_demo) {
      if (!m_stickers.empty())
        return;
      for (std::size_t g = 0; g < m_guilds.size(); ++g) {
        auto& stickers = m_stickers[m_guilds[g].id];
        for (int i = 0; i < 20; ++i) {
          api::Sticker sticker;
          sticker.id = std::to_string(810000000000000000ULL + g * 100 + i);
          sticker.name = i % 3 == 0 ? "Hello" : "Celebration " + std::to_string(i);
          sticker.description = "A friendly sticker.";
          sticker.tags = i % 3 == 0 ? "wave, hello" : "party, celebration";
          sticker.guild = m_guilds[g].id;
          sticker.format = 1 + i % 4;
          sticker.available = i != 19;
          auto animation = std::make_shared<media::Animation>();
          animation->width = animation->height = 64;
          for (int f = 0; f < (sticker.format == 1 ? 1 : 2); ++f) {
            media::Frame frame;
            frame.duration = std::chrono::milliseconds(250);
            frame.rgba.resize(64 * 64 * 4);
            for (int y = 0; y < 64; ++y)
              for (int x = 0; x < 64; ++x) {
                const auto at = (y * 64 + x) * 4;
                if ((x - Style::space2Xl) * (x - Style::space2Xl) + (y - Style::space2Xl) * (y - Style::space2Xl)
                    < 780) {
                  const bool face = ((x > 20 && x < 25) || (x > 39 && x < 44)) && y > 20 && y < 29;
                  frame.rgba[at] = face ? 35 : 180 + i % 6 * 10;
                  frame.rgba[at + 1] = face ? 35 : 130 + f * 50;
                  frame.rgba[at + 2] = face ? 35 : 240;
                  frame.rgba[at + 3] = 255;
                }
              }
            animation->frames.push_back(std::move(frame));
          }
          animation->duration = std::chrono::milliseconds(250 * animation->frames.size());
          auto& cached = m_images[api::stickerAsset(sticker)];
          cached.texture = m_window.context().textureManager().loadFromRgba(animation->frames[0].rgba.data(), 64, 64);
          if (animation->frames.size() > 1)
            cached.animation = animation;
          stickers.push_back(std::move(sticker));
        }
        m_loadedStickerSources.insert(m_guilds[g].id);
      }
      if (!m_guilds.empty()) {
        auto standard = m_stickers[m_guilds.front().id].front();
        standard.type = 1;
        standard.name = "Wave";
        standard.guild.clear();
        standard.pack = "820000000000000000";
        standard.packName = "Friendly faces";
        m_stickers["packs"] = {standard};
      }
      m_loadedStickerSources.insert("packs");
      ++m_stickerRevision;
      return;
    }
    if (!m_loadingStickerSources.empty())
      return;
    std::vector<std::string> sources;
    if (!m_dmMode && !m_guildId.empty())
      sources.push_back(m_guildId);
    sources.push_back("packs");
    for (const auto& guild : m_guilds)
      if (std::ranges::find(sources, guild.id) == sources.end())
        sources.push_back(guild.id);
    for (const auto& id : sources) {
      if (m_loadedStickerSources.contains(id) || m_stickerErrors.contains(id))
        continue;
      if (id == "packs" ? m_client.stickerPacks() : m_client.guildStickers(id))
        m_loadingStickerSources.insert(id);
      else
        m_stickerErrors[id] = "Could not load stickers. Check the connection.";
      break;
    }
  }

  void ClientApplication::stickerEvent(api::Event& event) {
    if (event.operation == api::Operation::StickerDetails) {
      if (m_inspectedSticker && m_inspectedSticker->id == event.resource) {
        if (!event.error.empty())
          m_stickerErrors["details"] = event.error;
        else if (event.stickers.size() == 1 && event.stickers.front().id == event.resource) {
          auto sticker = event.stickers.front();
          if (sticker.pack == m_inspectedSticker->pack && sticker.packName.empty())
            sticker.packName = m_inspectedSticker->packName;
          m_inspectedSticker = std::move(sticker);
        }
      }
      return;
    }
    const auto source = event.operation == api::Operation::StickerPacks ? "packs" : event.resource;
    if (!event.live && !m_loadingStickerSources.contains(source))
      return;
    m_loadingStickerSources.erase(source);
    if (event.error.empty()) {
      m_stickerErrors.erase(source);
      m_loadedStickerSources.insert(source);
      std::size_t resident = 0;
      for (const auto& [id, values] : m_stickers)
        if (id != source)
          resident += values.size();
      if (event.stickers.size() > 32000 - std::min<std::size_t>(resident, 32000))
        event.stickers.resize(32000 - std::min<std::size_t>(resident, 32000));
      m_stickers[source] = std::move(event.stickers);
      ++m_stickerRevision;
    } else
      m_stickerErrors[source] = event.error;
    if (source == "packs" && !m_pendingWaveChannel.empty()) {
      const auto channel = std::exchange(m_pendingWaveChannel, {});
      if (channel == m_channelId && m_dmMode && m_messages.empty()) {
        if (event.error.empty())
          stickerAction({.action = Action::SendWave});
        else
          m_status = "Could not load wave stickers. Choose Wave to say hi! to try again.";
      }
    }
  }

  std::string ClientApplication::stickerRestriction(const api::Sticker& sticker) const {
    if (const auto restriction = selectedChannelWriteRestriction(); !restriction.empty())
      return restriction;
    const bool external = m_demo || selectedCan(can::useExternalStickers);
    const auto reason =
        api::stickerUnavailable(sticker, m_dmMode ? "" : m_guildId, m_account.premiumType.value_or(0), external);
    return !reason.empty() && m_preferences.emojiImageLinks && !api::stickerImageLink(sticker).empty() ? "" : reason;
  }

  void ClientApplication::stickerAction(const Hit& hit) {
    if (hit.action == Action::SendWave) {
      if (!m_dmMode
          || m_channelId.empty()
          || !m_messages.empty()
          || m_loadingMessages
          || m_sending
          || !canSendToThread()
          || !m_pendingWaveChannel.empty())
        return;
      if (!m_loadedStickerSources.contains("packs")) {
        if (m_demo)
          prepareStickers();
        else {
          if (m_loadingStickerSources.contains("packs") || m_client.stickerPacks()) {
            m_pendingWaveChannel = m_channelId;
            m_loadingStickerSources.insert("packs");
          } else
            m_status = "Could not load wave stickers. Try again when connected.";
          return;
        }
      }
      std::vector<api::Sticker> candidates;
      for (const auto& sticker : m_stickers["packs"])
        if (api::isWaveSticker(sticker) && stickerRestriction(sticker).empty())
          candidates.push_back(sticker);
      if (candidates.empty()) {
        m_status = "No wave stickers are available. Open Stickers to choose another greeting.";
        return;
      }
      static std::mt19937 random(std::random_device{}());
      const auto& sticker = candidates[std::uniform_int_distribution<std::size_t>(0, candidates.size() - 1)(random)];
      auto reply = m_replies.extract(m_channelId);
      const bool queued = queueOutgoing("", {}, {sticker.id}, {sticker}, false);
      if (!reply.empty())
        m_replies.insert(std::move(reply));
      if (!queued && !m_demo)
        m_status = "Could not send the wave sticker. Try again when connected.";
    } else if (hit.action == Action::ToggleStickers) {
      const bool wasOpen = m_emojiMenu && m_stickerTab && !m_stickerInfo;
      closeGifPicker();
      closeEmojiPicker();
      closeContext();
      closeCompletion();
      m_reactionMessage.clear();
      m_reactionChannel.clear();
      m_scrolling.stopAll();
      if (!wasOpen) {
        m_emojiMenu = m_stickerTab = true;
        m_emojiQuery.clear();
        m_emojiScroll = m_emojiRailScroll = 0;
      }
    } else if (hit.action == Action::StickerSection) {
      m_stickerSection = hit.id;
      m_emojiQuery.clear();
      m_scrolling.stop(m_emojiScroll);
      m_emojiScroll = 0;
    } else if (hit.action == Action::StickerRetry) {
      if (hit.id == "details" && m_inspectedSticker) {
        m_stickerErrors.erase("details");
        if (!m_demo && !m_client.sticker(m_inspectedSticker->id))
          m_stickerErrors["details"] = "Could not load sticker details. Check the connection.";
      } else {
        m_stickerErrors.clear();
        prepareStickers();
      }
    } else if (hit.action == Action::StickerInfo) {
      const auto target = m_stickerTargets.find(hit.id);
      if (target == m_stickerTargets.end())
        return;
      auto sticker = target->second;
      for (const auto& [_, values] : m_stickers)
        if (const auto found = std::ranges::find(values, sticker.id, &api::Sticker::id); found != values.end()) {
          sticker = *found;
          break;
        }
      closeGifPicker();
      closeEmojiPicker();
      closeContext();
      m_inspectedSticker = std::move(sticker);
      closeCompletion();
      m_stickerErrors.erase("details");
      m_emojiMenu = m_stickerTab = m_stickerInfo = true;
      m_emojiOpened = true;
      m_controlInput.setFocus(nullptr);
      m_focusedInput.clear();
      if (!m_demo && !m_client.sticker(m_inspectedSticker->id))
        m_stickerErrors["details"] = "Could not load sticker details. Check the connection.";
    } else if (hit.action == Action::RemoveSticker) {
      std::erase_if(m_draftStickers[m_channelId], [&](const auto& sticker) { return sticker.id == hit.id; });
    } else if (hit.action == Action::SendSticker) {
      const auto target = m_stickerTargets.find(hit.id);
      if (target == m_stickerTargets.end() || !canSendToThread() || m_channelId.empty())
        return;
      const auto sticker = target->second;
      const auto reason = stickerRestriction(sticker);
      if (!reason.empty()) {
        m_stickerErrors["send"] = reason;
        return;
      }
      if (!m_drafts[m_channelId].empty()
          || !m_attachments[m_channelId].empty()
          || !m_draftStickers[m_channelId].empty()) {
        auto& draft = m_draftStickers[m_channelId];
        if (std::ranges::find(draft, sticker.id, &api::Sticker::id) == draft.end()) {
          if (draft.size() >= 3) {
            m_stickerErrors["send"] = "A message can contain up to three stickers.";
            return;
          }
          draft.push_back(sticker);
        }
        closeEmojiPicker();
        focusControl("composer");
        return;
      }
      const bool external = m_demo || selectedCan(can::useExternalStickers);
      const bool link =
          !api::stickerUnavailable(sticker, m_dmMode ? "" : m_guildId, m_account.premiumType.value_or(0), external)
               .empty();
      const auto reply = m_replies.contains(m_channelId) ? std::optional(m_replies.at(m_channelId)) : std::nullopt;
      if (m_demo)
        return;
      if (queueOutgoing(
              link ? api::stickerImageLink(sticker) : "", {},
              link ? std::vector<std::string>{} : std::vector{sticker.id}, {sticker}, false
          ))
        closeEmojiPicker();
      else
        m_stickerErrors["send"] = "Could not send this sticker. Check the connection.";
    }
  }

  void ClientApplication::drawStickerPicker(Node& root, float w, float h) {
    if (!m_stickerInfo)
      prepareStickers();
    if (!m_emojiOpened) {
      m_emojiOpened = true;
      m_controlInput.setFocus(nullptr);
      m_focusedInput = "emoji-search";
    }
    const auto frame = layout::pickerFrame(
        w - memberPaneWidth(), m_composerTop, false, layout::Picker::emojiWidth, layout::Picker::minimumHeight,
        m_stickerInfo ? layout::Picker::stickerInfoHeight : layout::Picker::maximumHeight
    );
    const auto [x, y, width, height] = frame;
    m_emojiBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back(
        {0, layout::Window::titlebarHeight, w, h - layout::Window::titlebarHeight, Action::CloseEmoji, {}}
    );
    rectangle(root, x, y, width, height, sidebar, Style::radiusXl);
    m_hits.push_back(m_emojiBounds);
    iconButton(
        root, {x + width - 44, y + (Style::spaceSm + Style::borderWidth), 32, 32, Action::CloseEmoji, {}}, "x",
        "Close stickers"
    );
    if (m_stickerInfo && m_inspectedSticker) {
      const auto& sticker = *m_inspectedSticker;
      m_stickerTargets[sticker.id] = sticker;
      label(
          root, x + (Style::spaceLg + Style::spaceXxs), y + Style::spaceLg, width - 70, sticker.name,
          Style::fontSizeTitle, foreground, FontWeight::SemiBold
      );
      const float size = std::min(160.0F, height - 184);
      picture(
          root, x + (width - size) / 2, y + 50, size, size, api::stickerAsset(sticker), 0, false, false,
          ImageFitMode::Contain
      );
      const auto guild = std::ranges::find(m_guilds, sticker.guild, &api::Guild::id);
      const std::string source = sticker.type == 1
          ? (sticker.packName.empty() ? "Discord sticker pack" : sticker.packName)
          : guild != m_guilds.end() ? guild->name
                                    : "Server sticker";
      label(
          root, x + (Style::spaceLg + Style::spaceXxs), y + 60 + size, width - layout::Picker::railItemSize, source,
          Style::fontSizeCaption, foreground, FontWeight::SemiBold
      );
      label(
          root, x + (Style::spaceLg + Style::spaceXxs), y + 84 + size, width - layout::Picker::railItemSize,
          sticker.description.empty() ? sticker.tags : sticker.description, Style::fontSizeDescription, muted,
          FontWeight::Normal, 2
      );
      const auto reason = stickerRestriction(sticker);
      if (!reason.empty())
        label(
            root, x + (Style::spaceLg + Style::spaceXxs), y + height - 86, width - layout::Picker::railItemSize, reason,
            Style::fontSizeDescription, muted, FontWeight::Normal, 2
        );
      if (const auto error = m_stickerErrors.find("details"); error != m_stickerErrors.end()) {
        label(
            root, x + (Style::spaceLg + Style::spaceXxs), y + height - 86, width - 108, error->second,
            Style::fontSizeDescription, muted, FontWeight::Normal, 2
        );
        button(root, {x + width - 82, y + height - 87, 64, 26, Action::StickerRetry, "details"}, "Retry");
      }
      button(
          root,
          {x + (Style::spaceLg + Style::spaceXxs), y + height - 44, 112, 28,
           reason.empty() ? Action::SendSticker : Action::None, sticker.id},
          "Send Sticker"
      );
      if (guild != m_guilds.end())
        button(root, {x + 142, y + height - 44, 112, 28, Action::Guild, guild->id}, "View Server");
      return;
    }
    button(root, {x + Style::spaceMd, y + Style::spaceDense, 66, 30, Action::ToggleGif, {}}, "GIFs");
    button(root, {x + 86, y + Style::spaceDense, 66, 30, Action::ToggleEmoji, {}}, "Emoji");
    label(
        root, x + 166, y + (Style::spaceLg + Style::borderWidth), width - 220, "Stickers", Style::fontSizeTitle,
        foreground, FontWeight::SemiBold
    );
    inputControl(
        root, x + Style::spaceMd, y + layout::Picker::searchTop, width - 24, 38, "emoji-search", m_emojiQuery,
        "Search stickers"
    );
    const float top = y + layout::Picker::contentTop, available = height - 164, gridX = x + layout::Picker::railWidth,
                gridWidth = width - 84;
    std::vector<std::string> sources;
    if (!m_dmMode && !m_guildId.empty())
      sources.push_back(m_guildId);
    for (const auto& folder : m_folders)
      for (const auto& id : folder.guildIds)
        if (std::ranges::find(sources, id) == sources.end())
          sources.push_back(id);
    for (const auto& guild : m_guilds)
      if (std::ranges::find(sources, guild.id) == sources.end())
        sources.push_back(guild.id);
    sources.push_back("packs");
    auto title = [&](const std::string& id) -> std::string {
      if (id == "packs")
        return "Discord stickers";
      if (id == "recent")
        return "Frequently used";
      const auto guild = std::ranges::find(m_guilds, id, &api::Guild::id);
      return guild == m_guilds.end() ? "Server stickers" : guild->name;
    };
    auto rail = std::make_unique<Node>();
    rail->setPosition(x + Style::spaceSm, top);
    rail->setSize(44, available);
    rail->setClipChildren(true);
    const auto railCount = 2 + std::ranges::count_if(sources, [&](const auto& id) {
                             return !m_stickers[id].empty() || id == "packs" || id == m_guildId;
                           });
    m_scrolling.limit(m_emojiRailScroll, std::max(0.0F, railCount * 44.0F - available));
    rail->setTextScrollOffset(-m_emojiRailScroll);
    float railHeight = 0;
    auto railItem = [&](const std::string& id, const std::string& icon) {
      const float ry = railHeight - m_emojiRailScroll;
      railHeight += layout::Picker::railStep;
      if (ry + layout::Picker::railItemSize <= 0 || ry >= available)
        return;
      const auto guild = std::ranges::find(m_guilds, id, &api::Guild::id);
      const float a = std::max(0.0F, ry), b = std::min(available, ry + layout::Picker::railItemSize);
      const bool over = hovered(x + Style::spaceSm, top + a, layout::Picker::railItemSize, b - a);
      if (over || m_stickerSection == id)
        rectangle(
            *rail, 0, ry, layout::Picker::railItemSize, layout::Picker::railItemSize, raised, Style::surfaceRadius
        );
      if (guild != m_guilds.end())
        avatar(*rail, 4, ry + Style::spaceXs, layout::Avatar::dm, guild->icon, guild->name);
      else
        glyph(*rail, 8, ry + Style::spaceSm, Style::glyphSizeLarge, icon, muted);
      m_hits.push_back({x + Style::spaceSm, top + a, layout::Picker::railItemSize, b - a, Action::StickerSection, id});
      if (over) {
        m_tooltip = id == "all" ? "All stickers" : title(id);
        m_tooltipX = x + (Style::spaceXl + Style::spaceXxs);
        m_tooltipY = top + ry;
      }
    };
    railItem("all", "sticker");
    railItem("recent", "clock");
    for (const auto& id : sources)
      if (!m_stickers[id].empty() || id == "packs" || id == m_guildId)
        railItem(id, "sticker");
    m_scrolling.limit(m_emojiRailScroll, std::max(0.0F, railHeight - available));
    root.addChild(std::move(rail));
    settingsScrollbar(
        root, "sticker-rail-scroll", {x + 52, top, 5, available, Action::None, {}}, railHeight, m_emojiRailScroll
    );
    const auto filter = std::to_string(m_stickerRevision) + ":" + m_stickerSection + ":" + m_emojiQuery;
    if (m_stickerFilter != filter) {
      m_stickerFilter = filter;
      m_stickerGroups.clear();
      if (m_stickerSection == "recent" && m_emojiQuery.empty()) {
        std::vector<api::Sticker> recent;
        for (const auto& id : m_recentStickers)
          for (const auto& source : sources) {
            const auto& values = m_stickers[source];
            if (const auto at = std::ranges::find(values, id, &api::Sticker::id); at != values.end()) {
              recent.push_back(*at);
              break;
            }
          }
        if (!recent.empty())
          m_stickerGroups.emplace_back("Frequently used", std::move(recent));
      } else {
        for (const auto& id : sources) {
          if (m_emojiQuery.empty() && m_stickerSection != "all" && m_stickerSection != id)
            continue;
          std::vector<api::Sticker> matches;
          for (const auto& sticker : m_stickers[id])
            if (fontFamilyMatches(
                    sticker.name + " " + sticker.tags + " " + sticker.description + " " + title(id), m_emojiQuery
                ))
              matches.push_back(sticker);
          if (!matches.empty())
            m_stickerGroups.emplace_back(title(id), std::move(matches));
        }
      }
    }
    const int columns = std::max(1, static_cast<int>(gridWidth / 116));
    const float cell = gridWidth / columns, step = cell + 8;
    float contentHeight = 0;
    for (const auto& [_, values] : m_stickerGroups)
      contentHeight += Style::spaceWide + std::ceil(static_cast<float>(values.size()) / columns) * step + 8;
    m_scrolling.limit(m_emojiScroll, std::max(0.0F, contentHeight - available));
    auto grid = std::make_unique<Node>();
    grid->setPosition(gridX, top);
    grid->setSize(gridWidth, available);
    grid->setClipChildren(true);
    grid->setTextScrollOffset(-m_emojiScroll);
    float cursor = -m_emojiScroll;
    std::optional<api::Sticker> hoveredSticker;
    for (const auto& [name, values] : m_stickerGroups) {
      if (cursor + Style::spaceXl > 0 && cursor < available)
        label(*grid, 0, cursor, gridWidth, name, Style::fontSizeDescription, muted, FontWeight::SemiBold);
      cursor += Style::spaceWide;
      for (std::size_t i = 0; i < values.size(); ++i) {
        const float sx = (i % columns) * cell, sy = cursor + (i / columns) * step;
        if (sy + cell <= 0 || sy >= available)
          continue;
        const auto& sticker = values[i];
        const float a = std::max(0.0F, sy), b = std::min(available, sy + cell);
        const bool over = hovered(gridX + sx, top + a, cell, b - a);
        if (over) {
          rectangle(*grid, sx, sy, cell, cell, raised, Style::surfaceRadius);
          hoveredSticker = sticker;
        }
        const bool loaded = picture(
            *grid, sx + Style::spaceCompact, sy + Style::spaceCompact, cell - Style::spaceMd, cell - Style::spaceMd,
            api::stickerAsset(sticker), 0, false, false, ImageFitMode::Contain
        );
        if (!loaded)
          glyph(
              *grid, sx + (cell - Style::spaceXl) / 2, sy + (cell - Style::spaceXl) / 2, Style::glyphSizeFeature,
              "sticker", muted
          );
        m_stickerTargets[sticker.id] = sticker;
        const auto reason = stickerRestriction(sticker);
        m_hits.push_back(
            {gridX + sx, top + a, cell, b - a, reason.empty() ? Action::SendSticker : Action::StickerInfo, sticker.id}
        );
        if (!reason.empty())
          glyph(*grid, sx + Style::spaceSm, sy + cell - Style::spaceXl, Style::baseGlyphSize, "lock", muted);
        if (over && sy >= 0 && sy + (Style::space2Xl - Style::spaceXxs) <= available)
          iconButton(
              *grid, {sx + cell - 28, sy + Style::spaceXxs, 26, 26, Action::StickerInfo, sticker.id}, "info-circle",
              "About this sticker"
          );
      }
      cursor += std::ceil(static_cast<float>(values.size()) / columns) * step + 8;
    }
    root.addChild(std::move(grid));
    settingsScrollbar(
        root, "sticker-scroll", {x + width - 12, top, 6, available, Action::None, {}}, contentHeight, m_emojiScroll
    );
    if (m_stickerGroups.empty())
      label(
          root, gridX + Style::spaceCompact, top + (Style::spaceLg + Style::spaceXxs), gridWidth - Style::spaceMd,
          !m_loadingStickerSources.empty() ? "Loading stickers…" : "No matching stickers", Style::fontSizeCaption, muted
      );
    if (!m_stickerErrors.empty()) {
      label(
          root, gridX, y + height - Style::spaceGroup, gridWidth - 72, m_stickerErrors.begin()->second,
          Style::fontSizeMini, muted, FontWeight::Normal, 2
      );
      button(root, {x + width - 82, y + height - 45, 64, 28, Action::StickerRetry, {}}, "Retry");
    } else if (hoveredSticker) {
      const auto& sticker = *hoveredSticker;
      label(
          root, gridX, y + height - Style::spaceGroup, gridWidth, sticker.name, Style::fontSizeCaption, foreground,
          FontWeight::SemiBold
      );
      const bool external = m_demo || selectedCan(can::useExternalStickers);
      const auto native =
          api::stickerUnavailable(sticker, m_dmMode ? "" : m_guildId, m_account.premiumType.value_or(0), external);
      const auto reason = stickerRestriction(sticker);
      label(
          root, gridX, y + height - 27, gridWidth,
          !reason.empty()       ? reason
              : !native.empty() ? "Sends as an image link"
                                : title(sticker.type == 1 ? "packs" : sticker.guild),
          Style::fontSizeMini, muted
      );
    }
  }
  void ClientApplication::demoStickerStep() {
    const auto step = m_demoInteractionStep++;
    auto require = [](bool condition, const char* message) {
      if (!condition)
        throw std::runtime_error(message);
    };
    auto click = [&](Action action) {
      const auto it =
          std::find_if(m_hits.rbegin(), m_hits.rend(), [&](const auto& hit) { return hit.action == action; });
      require(it != m_hits.rend(), "Sticker replay requires a visible control");
      const auto hit = *it;
      const auto chrome = m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(hit.x + hit.width / 2, hit.y + hit.height / 2 + chrome);
      m_window.click(hit.x + hit.width / 2, hit.y + hit.height / 2 + chrome, 272);
      m_window.released();
    };
    if (step == 0) {
      require(
          m_emojiMenu && m_stickerTab && !m_stickerGroups.empty(), "Sticker picker opens and retains server sections"
      );
      m_drafts[m_channelId] = "Keep my draft";
      focusControl("emoji-search");
      onKey({.symbol = XKB_KEY_1, .text = "1"});
      onKey({.symbol = XKB_KEY_7, .text = "7"});
      require(m_emojiQuery == "17", "Sticker search uses retained native input");
    } else if (step == 1) {
      require(!m_stickerGroups.empty(), "Sticker search keeps matching server sections");
      for (const auto& [_, values] : m_stickerGroups)
        for (const auto& value : values)
          require(value.name == "Celebration 17", "Sticker search filters by name and tags");
      onKey({.symbol = XKB_KEY_a, .control = true});
      onKey({.symbol = XKB_KEY_BackSpace});
    } else if (step == 2) {
      m_scrolling.stop(m_emojiScroll);
      m_emojiScroll = 73;
      m_window.motion(
          m_emojiBounds.x + 110, m_emojiBounds.y + 274 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
    } else if (step == 3) {
      require(m_emojiScroll == 73, "Sticker scrolling survives scene rebuilds");
      click(Action::StickerInfo);
    } else if (step == 4) {
      require(m_stickerInfo && m_inspectedSticker.has_value(), "Clicking sticker info opens a native detail card");
      onKey({.symbol = XKB_KEY_Escape});
      require(!m_emojiMenu, "Escape closes sticker details");
    } else if (step == 5) {
      click(Action::ToggleStickers);
      require(m_emojiMenu && m_stickerTab, "Composer sticker control reopens the picker");
    } else if (step == 6) {
      auto sticker = m_stickers.at(m_guilds.front().id).front();
      sticker.guild = "999999999999999999";
      m_account.premiumType = 0;
      m_preferences.emojiImageLinks = false;
      require(!stickerRestriction(sticker).empty(), "External stickers require Nitro when image links are disabled");
      m_preferences.emojiImageLinks = true;
      require(
          stickerRestriction(sticker).empty() && !api::stickerImageLink(sticker).empty(),
          "Emoji image links also enable sticker image links"
      );
      require(
          m_drafts[m_channelId] == "Keep my draft" && !m_sending,
          "Sticker search and details preserve composer drafts without requests"
      );
      click(Action::SendSticker);
      require(
          !m_emojiMenu && m_draftStickers[m_channelId].size() == 1 && m_drafts[m_channelId] == "Keep my draft",
          "Choosing a sticker attaches it to a nonempty draft instead of sending early"
      );
    } else if (step == 7) {
      click(Action::RemoveSticker);
      require(
          m_draftStickers[m_channelId].empty() && m_drafts[m_channelId] == "Keep my draft",
          "Draft sticker removal preserves the caption"
      );
      click(Action::ToggleEmoji);
      require(m_emojiMenu && !m_stickerTab, "Emoji tab switches out of the shared sticker picker");
      auto messages = std::exchange(m_messages, {});
      const auto dm = std::exchange(m_dmMode, true);
      const auto loading = std::exchange(m_loadingMessages, false);
      const auto overlay = std::exchange(m_demoOverlay, "outgoing-cycle");
      const auto pending = m_pendingSends.size();
      stickerAction({.action = Action::SendWave});
      require(
          m_pendingSends.size() == pending + 1 && m_drafts[m_channelId] == "Keep my draft",
          "A wave queues one native sticker without consuming the composer draft"
      );
      stickerAction({.action = Action::SendWave});
      require(m_pendingSends.size() == pending + 1, "A pending wave cannot be submitted twice");
      m_demoOverlay = overlay;
      m_dmMode = dm;
      m_loadingMessages = loading;
      m_messages = std::move(messages);
      std::clog << "Offline sticker replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
