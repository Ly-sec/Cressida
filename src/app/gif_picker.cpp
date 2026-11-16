#include "app/client_application.h"
#include "app/theme.h"
#include "ui/controls/input.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;
  void ClientApplication::closeGifPicker() {
    m_scrolling.stop(m_gifScroll);
    m_gifMenu = false;
    m_gifSearchTimer.stop();
    if (m_focusedInput == "gif-search") {
      m_controlInput.setFocus(nullptr);
      m_focusedInput.clear();
    }
    m_window.invalidate();
  }
  void ClientApplication::clearGifs() {
    closeGifPicker();
    m_gifs.clear();
    m_gifFavorites.clear();
    m_gifTargets.clear();
    m_pendingGifFavorites.clear();
    m_gifQuery.clear();
    m_gifRequestedQuery.clear();
    m_gifError.clear();
    m_gifFavoriteError.clear();
    m_loadingGifs = m_loadingGifFavorites = m_gifFavoritesLoaded = m_gifFavoritesDirty = false;
    m_gifFavoritesTab = false;
    m_gifScroll = 0;
  }
  void ClientApplication::searchGifs() {
    m_scrolling.stop(m_gifScroll);
    m_gifSearchTimer.stop();
    if (!m_gifMenu || m_gifFavoritesTab || m_demo)
      return;
    m_gifRequestedQuery = m_gifQuery;
    m_gifScroll = 0;
    m_loadingGifs = m_client.searchGifs(m_gifQuery);
    m_gifError = m_loadingGifs ? "" : "Could not request GIFs. Try again.";
    m_window.invalidate();
  }
  void ClientApplication::toggleGifFavorite(const api::Gif& gif) {
    if (m_pendingGifFavorites.contains(gif.url) || !api::validGif(gif))
      return;
    const bool add = std::ranges::find(m_gifFavorites, gif.url, &api::Gif::url) == m_gifFavorites.end();
    if (m_demo) {
      if (add)
        m_gifFavorites.insert(m_gifFavorites.begin(), gif);
      else
        std::erase_if(m_gifFavorites, [&](const auto& entry) { return entry.url == gif.url; });
    } else if (m_client.saveGifFavorite(gif, add)) {
      m_pendingGifFavorites.insert(gif.url);
      m_gifFavoriteError.clear();
    } else
      m_gifFavoriteError = "Could not update favorites. Try again.";
    m_window.invalidate();
  }
  void ClientApplication::gifEvent(api::Event& event) {
    if (event.operation == api::Operation::GifSearch) {
      if (!m_gifMenu || m_gifFavoritesTab || event.resource != m_gifQuery || event.resource != m_gifRequestedQuery)
        return;
      m_loadingGifs = false;
      m_gifError = event.error;
      if (event.error.empty())
        m_gifs = std::move(event.gifs);
    } else if (event.resource == "changed") {
      m_gifFavoritesDirty = true;
    } else {
      if (event.operation == api::Operation::SaveGifFavorite)
        m_pendingGifFavorites.erase(event.resource);
      else
        m_loadingGifFavorites = false;
      m_gifFavoriteError = event.error;
      if (event.error.empty()) {
        m_gifFavorites = std::move(event.gifs);
        m_gifFavoritesLoaded = true;
      }
    }
    if (event.error.empty()
        && m_gifFavoritesDirty
        && m_gifFavoritesLoaded
        && !m_loadingGifFavorites
        && m_pendingGifFavorites.empty()) {
      m_loadingGifFavorites = m_client.gifFavorites();
      if (m_loadingGifFavorites)
        m_gifFavoritesDirty = false;
    }
  }
  void ClientApplication::drawGifPicker(Node& root, float w, float h) {
    const auto frame = layout::pickerFrame(
        w - memberPaneWidth(), m_composerTop, false, layout::Picker::gifWidth, layout::Picker::minimumHeight
    );
    const auto [x, y, width, height] = frame;
    m_gifBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::CloseGif, {}});
    rectangle(root, x, y, width, height, sidebar, Style::radiusXl);
    m_hits.push_back(m_gifBounds);
    button(
        root, {x + Style::spaceMd, y + Style::spaceDense, 66, 32, Action::GifTab, "search"}, "GIFs", false,
        !m_gifFavoritesTab
    );
    button(
        root, {x + 86, y + Style::spaceDense, 100, 32, Action::GifTab, "favorites"}, "Favorites", false,
        m_gifFavoritesTab
    );
    button(root, {x + 194, y + Style::spaceDense, 66, 32, Action::ToggleEmoji, {}}, "Emoji");
    button(root, {x + 268, y + Style::spaceDense, 88, 32, Action::ToggleStickers, {}}, "Stickers");
    iconButton(root, {x + width - 44, y + Style::spaceDense, 32, 32, Action::CloseGif, {}}, "x", "Close GIF picker");
    inputControl(
        root, x + Style::spaceMd, y + layout::Picker::searchTop, width - 24, 38, "gif-search", m_gifQuery,
        m_gifFavoritesTab ? "Search favorites" : "Search GIFs"
    );
    const auto& error = m_gifFavoritesTab ? m_gifFavoriteError : m_gifError;
    const bool loading = m_gifFavoritesTab ? m_loadingGifFavorites : m_loadingGifs;
    const float top = y + layout::Picker::contentTop, available = height - 116;
    if (!error.empty()) {
      label(
          root, x + (Style::spaceLg + Style::spaceXxs), top + Style::cardPadding, width - 36, error,
          Style::fontSizeCaption, muted, FontWeight::Normal, 4
      );
      button(root, {x + (Style::spaceLg + Style::spaceXxs), top + 94, 90, 32, Action::GifRetry, {}}, "Try again");
      return;
    }
    if (loading || m_gifSearchTimer.active()) {
      label(
          root, x + (Style::spaceLg + Style::spaceXxs), top + Style::spaceRelaxed, width - 36,
          m_gifFavoritesTab ? "Loading favorites…" : "Finding GIFs…", Style::fontSizeBody, muted
      );
      return;
    }
    auto body = std::make_unique<Node>();
    body->setPosition(x + Style::spaceMd, top);
    body->setSize(width - Style::spaceXl, available);
    body->setClipChildren(true);
    const float paintedScroll = m_gifScroll;
    const float tileWidth = (width - 32) / 2;
    float columnY[2] = {0, 0};
    std::size_t count = 0;
    auto lower = [](std::string text) {
      for (auto& c : text)
        c = std::tolower(static_cast<unsigned char>(c));
      return text;
    };
    const auto query = lower(m_gifQuery);
    const auto& items = m_gifFavoritesTab ? m_gifFavorites : m_gifs;
    for (const auto& gif : items) {
      if (m_gifFavoritesTab && !query.empty() && !lower(gif.title + " " + gif.url).contains(query))
        continue;
      const auto column = columnY[0] <= columnY[1] ? 0 : 1;
      const float tileHeight = std::clamp(
          gif.width > 0 ? tileWidth * gif.height / gif.width : layout::Picker::gifFallbackTileHeight,
          layout::Picker::gifMinimumTileHeight, layout::Picker::gifMaximumTileHeight
      );
      const float tx = column * (tileWidth + Style::spaceSm), ty = columnY[column] - m_gifScroll;
      columnY[column] += tileHeight + Style::spaceSm;
      ++count;
      if (ty + tileHeight <= 0 || ty >= available)
        continue;
      const float gx = x + Style::spaceMd + tx, gy = top + ty;
      rectangle(*body, tx, ty, tileWidth, tileHeight, panel, Style::hoverRadius);
      const auto source = gif.src.empty() ? gif.preview : gif.src;
      picture(*body, tx, ty, tileWidth, tileHeight, source, Style::hoverRadius, true);
      m_gifTargets[gif.url] = gif;
      const float clippedTop = std::max(top, gy), clippedBottom = std::min(top + available, gy + tileHeight);
      m_hits.push_back({gx, clippedTop, tileWidth, clippedBottom - clippedTop, Action::SendGif, gif.url});
      const bool favorite = std::ranges::find(m_gifFavorites, gif.url, &api::Gif::url) != m_gifFavorites.end();
      if ((hovered(gx, clippedTop, tileWidth, clippedBottom - clippedTop) || favorite)
          && ty >= 0
          && ty + Style::space2Xl <= available) {
        rectangle(
            *body, tx + tileWidth - layout::Window::titlebarHeight, ty + Style::spaceXs, 30, 30, sidebar,
            Style::hoverRadius
        );
        iconButton(
            *body,
            {tx + tileWidth - layout::Window::titlebarHeight, ty + Style::spaceXs, 30, 30,
             m_pendingGifFavorites.contains(gif.url) ? Action::None : Action::FavoriteGif, gif.url},
            favorite ? "star-filled" : "star", favorite ? "Remove from favorites" : "Add to favorites", favorite
        );
      }
    }
    m_gifMaxScroll = std::max(0.0F, std::max(columnY[0], columnY[1]) - available);
    m_scrolling.limit(m_gifScroll, m_gifMaxScroll);
    body->setTextScrollOffset(-paintedScroll);
    if (!count)
      label(
          *body, 6, 20, width - 36,
          m_gifFavoritesTab ? "No favorites yet. Star a GIF to keep it here." : "No GIFs found. Try another search.",
          Style::fontSizeBody, muted, FontWeight::Normal, 3
      );
    root.addChild(std::move(body));
  }
  void ClientApplication::demoGifStep() {
    const auto step = m_demoInteractionStep++ % 8;
    const auto titleOffset = m_window.titlebarHeight() - layout::Window::titlebarHeight;
    auto require = [](bool condition, const char* text) {
      if (!condition)
        throw std::runtime_error(text);
    };
    auto click = [&](Action action, const std::string& id = std::string{}) {
      const auto it = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == action && (id.empty() || hit.id == id);
      });
      require(it != m_hits.end(), "GIF replay needs a visible control");
      const auto hit = *it;
      m_window.motion(hit.x + Style::spaceSm, hit.y + Style::spaceSm + titleOffset);
      m_window.click(hit.x + Style::spaceSm, hit.y + Style::spaceSm + titleOffset, 272);
      m_window.released();
    };
    if (step == 0) {
      require(m_gifMenu, "GIF picker survives repaint");
      if (m_gifFavoritesTab)
        click(Action::GifTab, "search");
    } else if (step == 1) {
      focusControl("gif-search");
      onKey({.symbol = XKB_KEY_c, .text = "c"});
      require(m_gifQuery == "c", "Native GIF search accepts keyboard input");
      onKey({.symbol = XKB_KEY_a, .control = true});
      onKey({.symbol = XKB_KEY_BackSpace});
      require(m_gifQuery.empty(), "Native GIF search supports Ctrl+A and deletion");
    } else if (step == 2) {
      const auto it = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == Action::SendGif && hit.id == m_gifs[1].url;
      });
      require(it != m_hits.end(), "GIF replay needs a visible preview");
      m_window.motion(it->x + Style::spaceRelaxed, it->y + Style::spaceRelaxed + titleOffset);
    } else if (step == 3) {
      click(Action::FavoriteGif, m_gifs[1].url);
      require(
          std::ranges::find(m_gifFavorites, m_gifs[1].url, &api::Gif::url) != m_gifFavorites.end(),
          "Star adds a demo favorite without network access"
      );
    } else if (step == 4) {
      click(Action::GifTab, "favorites");
      require(m_gifFavoritesTab, "Favorites tab opens");
    } else if (step == 5) {
      click(Action::FavoriteGif, m_gifs[1].url);
      require(
          std::ranges::find(m_gifFavorites, m_gifs[1].url, &api::Gif::url) == m_gifFavorites.end(),
          "Star removes a demo favorite"
      );
    } else if (step == 6) {
      onKey({.symbol = XKB_KEY_Escape});
      require(!m_gifMenu && !m_gifSearchTimer.active(), "Escape closes the GIF picker and cancels search");
    } else {
      click(Action::ToggleGif);
      require(m_gifMenu, "Composer reopens the GIF picker after dismissal");
    }
    std::clog << "Offline GIF replay step " << m_demoInteractionStep << " passed\n";
    m_window.invalidate();
  }
} // namespace app
