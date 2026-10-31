#include "app/client_application.h"
#include "app/theme.h"

#include <algorithm>
#include <chrono>

namespace app {
  namespace {
    constexpr auto previewFreshness = std::chrono::minutes{5};
    constexpr auto failedPreviewFreshness = std::chrono::seconds{10};
    constexpr int blockedRelationship = 2;
  } // namespace

  void ClientApplication::hoverStreamPreview(const std::string& key, const layout::Frame& anchor) {
    if (!anchor.contains(m_pointerX, m_pointerY)
        || anchor.width <= 0
        || anchor.height <= 0
        || !m_connected
        || !m_preferences.showImages
        || startupCovered()
        || settingsVisible()
        || m_shareOpen
        || m_completionQuery
        || m_fontPickerOpen
        || !m_searchSuggestions.empty()
        || m_contextOpen
        || m_inboxOpen
        || m_selectRequest
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || !m_dialogKind.empty()
        || userAccountDialogOpen()
        || !m_viewImage.empty()
        || m_videoExpanded
        || m_resizingSidebar
        || m_railDrag
        || m_dragDraft
        || m_dragMessage
        || m_dragImage
        || m_controlInput.pointerCaptured()
        || (!m_window.active() && !m_demo))
      return;
    const auto parsed = api::parseStreamKey(key);
    if (!parsed
        || parsed->owner == m_account.id
        || m_userDirectory.relationship(parsed->owner) == blockedRelationship
        || m_userDirectory.ignored(parsed->owner)
        || std::ranges::none_of(m_streamSnapshot.available, [&](const auto& stream) { return stream.key == *parsed; }))
      return;

    std::erase_if(m_streamPreviewCache, [&](const auto& entry) {
      const auto owner = api::parseStreamKey(entry.first);
      const bool stale = !owner
          || m_userDirectory.relationship(owner->owner) == blockedRelationship
          || m_userDirectory.ignored(owner->owner)
          || std::ranges::none_of(m_streamSnapshot.available, [&](const auto& stream) { return stream.key == *owner; });
      if (stale && entry.second.revoked)
        entry.second.revoked->store(true);
      return stale;
    });
    if (!m_streamPreviewCache.contains(key) && m_streamPreviewCache.size() >= api::maximumStreams) {
      const auto oldest =
          std::ranges::min_element(m_streamPreviewCache, {}, [](const auto& entry) { return entry.second.checked; });
      if (oldest != m_streamPreviewCache.end()) {
        if (oldest->second.revoked)
          oldest->second.revoked->store(true);
        m_streamPreviewCache.erase(oldest);
      }
    }

    m_streamPreviewHovered = key;
    m_streamPreviewAnchor = anchor;
    m_tooltip.clear();
    auto& cached = m_streamPreviewCache[key];
    const auto now = std::chrono::steady_clock::now();
    if (cached.pending) {
      if (now - cached.checked >= api::streamSetupDeadline) {
        if (cached.revoked)
          cached.revoked->store(true);
        cached.pending = false;
        cached.error = "Preview unavailable";
        cached.checked = now;
      }
      return;
    }
    const auto freshness = cached.error.empty() && !cached.url.empty() ? previewFreshness : failedPreviewFreshness;
    if (cached.checked != std::chrono::steady_clock::time_point{} && now - cached.checked < freshness)
      return;
    if (cached.revoked)
      cached.revoked->store(true);
    cached.revoked = std::make_shared<std::atomic_bool>(false);
    cached.request = ++m_streamPreviewRequest;
    if (!cached.request)
      cached.request = ++m_streamPreviewRequest;
    cached.checked = now;
    cached.error.clear();
    cached.pending = !m_demo
        && m_client.streamPreview(
            {.key = *parsed, .request = cached.request, .generation = m_session, .revoked = cached.revoked}
        );
    if (!cached.pending) {
      cached.revoked->store(true);
      cached.error = "Preview unavailable";
    }
  }

  void ClientApplication::streamPreviewResult(const api::Event& event) {
    if (!event.streamPreview || event.streamPreview->uploaded || event.session != m_session)
      return;
    const auto key = event.streamPreview->key.value();
    const auto entry = m_streamPreviewCache.find(key);
    if (key.empty()
        || event.resource != key
        || entry == m_streamPreviewCache.end()
        || !entry->second.pending
        || entry->second.request != event.streamPreview->request
        || !entry->second.revoked
        || entry->second.revoked->load())
      return;
    const auto& owner = event.streamPreview->key.owner;
    if (owner == m_account.id
        || m_userDirectory.relationship(owner) == blockedRelationship
        || m_userDirectory.ignored(owner)
        || std::ranges::none_of(m_streamSnapshot.available, [&](const auto& stream) {
             return stream.key == event.streamPreview->key;
           })) {
      entry->second.revoked->store(true);
      m_streamPreviewCache.erase(entry);
      return;
    }
    auto& cached = entry->second;
    cached.pending = false;
    cached.checked = std::chrono::steady_clock::now();
    cached.error = event.error;
    if (event.error.empty())
      cached.url = event.streamPreview->url;
    if (cached.url.empty() && cached.error.empty())
      cached.error = "Preview unavailable";
    m_window.invalidate();
  }

  void ClientApplication::drawStreamPreviewTooltip(Node& root, float width, float height) {
    if (m_streamPreviewHovered.empty()
        || !m_preferences.showImages
        || !m_connected
        || startupCovered()
        || settingsVisible()
        || m_shareOpen
        || m_completionQuery
        || m_fontPickerOpen
        || !m_searchSuggestions.empty()
        || m_contextOpen
        || m_inboxOpen
        || m_selectRequest
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || !m_dialogKind.empty()
        || userAccountDialogOpen()
        || !m_viewImage.empty()
        || m_videoExpanded
        || m_resizingSidebar
        || m_railDrag
        || m_dragDraft
        || m_dragMessage
        || m_dragImage
        || m_controlInput.pointerCaptured()
        || (!m_window.active() && !m_demo)
        || !m_streamPreviewAnchor.contains(m_pointerX, m_pointerY))
      return;
    const auto key = api::parseStreamKey(m_streamPreviewHovered);
    if (!key
        || key->owner == m_account.id
        || m_userDirectory.relationship(key->owner) == blockedRelationship
        || m_userDirectory.ignored(key->owner)
        || std::ranges::none_of(m_streamSnapshot.available, [&](const auto& stream) { return stream.key == *key; }))
      return;
    const auto entry = m_streamPreviewCache.find(m_streamPreviewHovered);
    if (entry == m_streamPreviewCache.end())
      return;
    m_tooltip.clear();
    const auto frame = layout::Sharing::previewTooltip(width, height, m_streamPreviewAnchor);
    if (frame.image.width <= 0 || frame.image.height <= 0)
      return;
    rectangle(
        root, frame.panel.x, frame.panel.y, frame.panel.width, frame.panel.height, theme::raised, Style::radiusMd
    );
    const auto border = Style::borderWidth;
    rectangle(
        root, frame.panel.x + border, frame.panel.y + border, frame.panel.width - border * 2,
        frame.panel.height - border * 2, theme::sidebar, Style::radiusMd
    );
    label(
        root, frame.title.x, frame.title.y, frame.title.width, userName(key->owner, "Participant") + "'s screen",
        Style::fontSizeBody, theme::foreground, FontWeight::SemiBold
    );
    rectangle(root, frame.image.x, frame.image.y, frame.image.width, frame.image.height, theme::panel, Style::radiusSm);
    const auto& cached = entry->second;
    bool imageFailed = false;
    if (!cached.url.empty()) {
      if (const auto image = m_images.find(cached.url); image != m_images.end())
        imageFailed = (image->second.animation && image->second.animation->frames.size() > 1)
            || (!image->second.texture.valid()
                && image->second.retry != std::chrono::steady_clock::time_point{}
                && !m_pendingImages.contains(cached.url));
      if (!imageFailed
          && picture(
              root, frame.image.x, frame.image.y, frame.image.width, frame.image.height, cached.url, Style::radiusSm,
              false, false, ImageFitMode::Contain
          ))
        return;
    }
    const std::string caption = imageFailed || (!cached.pending && (cached.url.empty() || !cached.error.empty()))
        ? "Preview unavailable"
        : "Loading preview…";
    const float icon = Style::glyphSizeFeature;
    glyph(
        root, frame.image.x + (frame.image.width - icon) / 2, frame.image.y + frame.image.height / 2 - icon, icon,
        imageFailed ? "photo-off" : "screen-share", theme::muted
    );
    const auto metrics = m_window.target().renderer().measureText(caption, Style::fontSizeCaption);
    const float captionWidth = std::max(0.0F, std::min(metrics.width, frame.image.width - Style::spaceMd * 2));
    label(
        root, frame.image.x + (frame.image.width - captionWidth) / 2,
        frame.image.y + frame.image.height / 2 + Style::spaceSm, captionWidth, caption, Style::fontSizeCaption,
        theme::muted
    );
  }
} // namespace app
