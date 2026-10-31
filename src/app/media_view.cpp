#include "app/client_application.h"
#include "app/theme.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/image_encoder.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/input_area.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <gio/gio.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    constexpr std::string_view draftImagePrefix = "draft-attachment:";
    std::string imageMime(std::span<const std::uint8_t> bytes) {
      if (bytes.empty())
        return {};
      auto* type = g_content_type_guess(nullptr, bytes.data(), bytes.size(), nullptr);
      auto* mime = type ? g_content_type_get_mime_type(type) : nullptr;
      std::string result = mime ? mime : "";
      g_free(mime);
      g_free(type);
      return result.starts_with("image/") ? result : "";
    }
    std::string imageFilename(const std::string& link, const std::string& mime) {
      std::string name = "image";
      if (auto* uri = g_uri_parse(link.c_str(), G_URI_FLAGS_NONE, nullptr)) {
        if (const auto* path = g_uri_get_path(uri)) {
          auto* base = g_path_get_basename(path);
          if (base && *base && std::string_view(base) != "." && std::string_view(base) != "/")
            name = base;
          g_free(base);
        }
        g_uri_unref(uri);
      }
      std::erase_if(name, [](unsigned char c) { return c < ' ' || c == 127 || c == '/' || c == '\\'; });
      const std::string_view extension = mime == "image/png" ? ".png"
          : mime == "image/jpeg"                             ? ".jpg"
          : mime == "image/gif"                              ? ".gif"
          : mime == "image/webp"                             ? ".webp"
          : mime == "image/avif"                             ? ".avif"
                                                             : "";
      if (!extension.empty()) {
        if (const auto dot = name.rfind('.'); dot != std::string::npos)
          name.resize(dot);
        name += extension;
      }
      return name;
    }
  } // namespace
  std::string ClientApplication::viewerMediaLink() const {
    if (draftImage(m_viewImage))
      return {};
    auto link = api::mediaUrl(m_viewUrl);
    return link.empty() ? api::mediaUrl(m_viewImage) : link;
  }
  void ClientApplication::closeImageViewer() {
    if (!m_imageContext.empty())
      closeContext();
    if (m_imageSaveRequest)
      m_filePicker.cancel();
    m_imageSaveRequest = 0;
    m_imageActionSource.clear();
    m_imageActionViewer.clear();
    m_pendingImageAction = Action::None;
    m_imageStatus.clear();
    m_viewImage.clear();
    m_dragImage = false;
  }
  void ClientApplication::enterMediaViewer() {
    finishRailDrag(true);
    finishSidebarResize();
    closeCompletion();
    closeGifPicker();
    closeEmojiPicker();
    closeContext();
    m_presenceMenu = false;
    m_scrolling.stopAll();
    m_controlInput.pointerLeave();
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_dragDraft = m_dragMessage = false;
    m_tooltip.clear();
    m_contextTargets.clear();
    m_textAreas.clear();
    std::erase_if(m_hits, [](const auto& hit) {
      return hit.action != Action::Close && hit.action != Action::Minimize && hit.action != Action::Maximize;
    });
  }
  void ClientApplication::openImageContext(float x, float y) {
    closeContext();
    const auto image = std::ranges::find_if(m_hits, [&](const auto& hit) {
      return hit.action == Action::ImagePan && hit.contains(x, y);
    });
    if (image == m_hits.end())
      return;
    m_contextMessage.clear();
    m_contextUser.clear();
    m_contextGuild.clear();
    m_contextChannel.clear();
    m_contextQuickIndex = -1;
    m_contextEntries.clear();
    const auto add = [&](Action action, std::string title, std::string icon, bool enabled) {
      ContextMenuControlEntry entry{static_cast<int>(action), std::move(title)};
      entry.icon = std::move(icon);
      entry.enabled = enabled;
      m_contextEntries.push_back(std::move(entry));
    };
    const auto* draft = draftImage(m_viewImage);
    const auto source = viewerMediaLink();
    const auto cached = m_images.find(m_viewImage);
    const bool available = (draft && !draft->upload.bytes.empty())
        || (cached != m_images.end() && !cached->second.bytes.empty())
        || !source.empty();
    const bool idle = m_pendingImageAction == Action::None && !m_imageSaveRequest;
    add(Action::CopyImage, "Copy Image", "photo", available && idle);
    add(Action::SaveImage, "Save Image", "download", available && idle);
    ContextMenuControlEntry separator;
    separator.separator = true;
    m_contextEntries.push_back(std::move(separator));
    add(Action::CopyMediaLink, "Copy Media Link", "link", !source.empty());
    add(Action::OpenMediaLink, "Open Media Link", "external-link", !source.empty());
    m_imageContext = m_viewImage;
    m_contextX = x;
    m_contextY = y;
    m_contextScroll = 0;
    m_contextOpen = m_contextDirty = true;
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_dragImage = false;
    m_window.invalidate();
  }
  void ClientApplication::imageViewerAction(Action action) {
    if (m_viewImage.empty())
      return;
    const auto link = viewerMediaLink();
    if (action == Action::CopyMediaLink) {
      if (!link.empty()) {
        m_window.copy(link);
        m_imageStatus = "Media link copied.";
      }
    } else if (action == Action::OpenMediaLink) {
      if (!link.empty())
        m_imageStatus =
            m_demo || auth::openExternalUrl(link) ? "Opening media link…" : "Could not open the media link.";
    } else if (action == Action::CopyImage || action == Action::SaveImage) {
      if (m_imageSaveRequest || m_pendingImageAction != Action::None)
        return;
      std::span<const std::uint8_t> bytes;
      std::string mime, name;
      if (const auto* draft = draftImage(m_viewImage)) {
        bytes = draft->upload.bytes;
        mime = draft->upload.mime;
        name = draft->upload.filename;
      } else {
        const auto source = link.empty() ? m_viewImage : link;
        auto& cached = m_images[source];
        cached.uses |= imageUseMask(ImageUse::Content);
        if (cached.bytes.empty()) {
          if (!m_demo && (m_pendingImages.contains(source) || m_assets.request(source))) {
            m_pendingImages[source] |= imageUseMask(ImageUse::Content);
            m_imageActionSource = source;
            m_imageActionViewer = m_viewImage;
            m_pendingImageAction = action;
            m_imageStatus = "Loading original image…";
          } else
            m_imageStatus = "Could not load the original image. Try again.";
          m_window.invalidate();
          return;
        }
        bytes = cached.bytes;
        mime = imageMime(bytes);
        name = imageFilename(source, mime);
      }
      if (bytes.empty() || !mime.starts_with("image/"))
        m_imageStatus = "This image could not be copied or saved.";
      else if (action == Action::CopyImage) {
        std::vector<std::uint8_t> png;
        if (mime != "image/png") {
          if (const auto decoded = decodeRasterImage(bytes.data(), bytes.size()))
            png = encodePng(decoded->pixels.data(), decoded->width, decoded->height);
          bytes = png;
        }
        if (bytes.empty() || bytes.size() > api::maxUploadBytes)
          m_imageStatus = "This image could not be copied to the clipboard.";
        else {
          m_window.copy({bytes.begin(), bytes.end()}, "image/png");
          m_imageStatus = "Image copied.";
        }
      } else if (m_demo)
        m_imageStatus = "The save dialog is unavailable in this offline preview.";
      else {
        m_imageSaveRequest = ++m_imageSaveSerial;
        m_filePicker.save(m_imageSaveRequest, std::move(name), std::move(mime), {bytes.begin(), bytes.end()});
        m_imageStatus = "Choose where to save the image…";
      }
    }
    m_window.invalidate();
  }
  bool ClientApplication::imageSaved(const PickedFile& result) {
    if (!result.saving)
      return false;
    if (!m_imageSaveRequest || result.request != m_imageSaveRequest)
      return true;
    m_imageSaveRequest = 0;
    m_imageStatus = result.cancelled ? "" : result.error.empty() ? "Image saved." : result.error;
    m_window.invalidate();
    return true;
  }
  const ClientApplication::DraftImage* ClientApplication::draftImage(std::string_view path) const {
    if (path.starts_with("pending-attachment:")) {
      const auto suffix = path.substr(std::string_view("pending-attachment:").size());
      const auto separator = suffix.find(':');
      if (separator == std::string_view::npos)
        return nullptr;
      const auto pending = m_pendingSends.find(std::string(suffix.substr(0, separator)));
      if (pending == m_pendingSends.end() || pending->second.preview.channelId != m_channelId)
        return nullptr;
      const auto& images = pending->second.images;
      const auto image = std::ranges::find(images, suffix.substr(separator + 1), &DraftImage::key);
      return image == images.end() ? nullptr : &*image;
    }
    if (!path.starts_with(draftImagePrefix))
      return nullptr;
    const auto draft = m_attachments.find(m_channelId);
    if (draft == m_attachments.end())
      return nullptr;
    const auto image = std::ranges::find(draft->second, path.substr(draftImagePrefix.size()), &DraftImage::key);
    return image == draft->second.end() ? nullptr : &*image;
  }
  void ClientApplication::openDraftAttachment(const std::string& key) {
    const auto path = std::string(draftImagePrefix) + key;
    const auto* selected = draftImage(path);
    if (!selected || !selected->texture.valid())
      return;
    m_viewerImages.clear();
    for (const auto& image : m_attachments.at(m_channelId))
      if (image.texture.valid())
        m_viewerImages.push_back(
            {std::string(draftImagePrefix) + image.key,
             {},
             static_cast<float>(image.texture.width),
             static_cast<float>(image.texture.height)}
        );
    activate({.action = Action::ViewImage, .id = path});
  }
  bool ClientApplication::hovered(float x, float y, float width, float height) const {
    return m_pointerX >= x && m_pointerY >= y && m_pointerX < x + width && m_pointerY < y + height;
  }
  void ClientApplication::updateCursor() {
    const auto preview = sharingPreviewFrame();
    if (m_shareOpen || m_sharingPreviewDragging || (preview && preview->card.contains(m_pointerX, m_pointerY))) {
      auto shape = m_sharingPreviewDragging ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING
          : m_shareOpen                     ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
                                            : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
      if (!m_sharingPreviewDragging)
        for (const auto& hit :
             (m_shareOpen && (m_sharingAdvanced || m_sharingAudioExpanded)) ? m_sharingMenuHits : m_shareHits)
          if (hit.contains(m_pointerX, m_pointerY)) {
            shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
            break;
          }
      if (m_shareOpen && (m_sharingAdvanced || m_sharingAudioExpanded))
        for (const auto& hit : m_shareHits)
          if ((hit.id == "advanced" || hit.id == "audio-expand") && hit.contains(m_pointerX, m_pointerY))
            shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
      m_window.setCursorShape(shape);
      return;
    }
    std::uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    if (m_railDrag && m_railDrag->active)
      shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING;
    else if (m_resizingSidebar)
      shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE;
    else if (m_dragImage)
      shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING;
    else if (m_dragDraft || m_dragMessage)
      shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
    else if (m_controlInput.pointerCaptured())
      shape = m_controlCursor;
    else if (
        userAccountDialogOpen()
        && !(m_selectRequest && m_selectBounds.contains(m_pointerX, m_pointerY))
        && (m_pointerX < m_userAccount.dialogX
            || m_pointerY < m_userAccount.dialogY
            || m_pointerX >= m_userAccount.dialogX + m_userAccount.dialogWidth
            || m_pointerY >= m_userAccount.dialogY + m_userAccount.dialogHeight)
    )
      shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    else if (
        !(m_inboxOpen && m_inboxBounds.contains(m_pointerX, m_pointerY)) && sidebarResizeAt(m_pointerX, m_pointerY)
    )
      shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE;
    else {
      for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it)
        if (it->contains(m_pointerX, m_pointerY)) {
          if (m_inboxOpen
              && m_inboxBounds.contains(m_pointerX, m_pointerY)
              && it->action != Action::InboxAction
              && it->action != Action::Inbox)
            continue;
          switch (it->action) {
          case Action::None:
            break;
          case Action::Composer:
          case Action::Token:
          case Action::SelectMessage:
            shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
            break;
          case Action::ImagePan:
            shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
            break;
          default:
            shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
            break;
          }
          break;
        }
      auto* area = m_controlInput.inputAreaAt(
          m_pointerX, m_pointerY + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      // Covered native controls remain in the scene. Only the active overlay
      // may supply a cursor; InputAreas are children of retained controls.
      const auto allowed = [&](const std::string& id) {
        if (!m_viewImage.empty())
          return m_contextOpen && id.starts_with("popup:context");
        if (m_videoExpanded || m_presenceMenu)
          return false;
        if (m_selectRequest)
          return id.starts_with("select-popup") && m_selectBounds.contains(m_pointerX, m_pointerY);
        if (userAccountDialogOpen())
          return id.starts_with("user-account:secret:")
              || id.starts_with("user-account-dialog-button:")
              || id == "user-account-provider-select"
              || id == "user-account-dialog-scrollbar";
        if (m_serverSettings.open)
          return id.starts_with("server-");
        if (m_contextOpen)
          return id.starts_with("popup:")
              && (m_contextBounds.contains(m_pointerX, m_pointerY)
                  || (m_reactionSubmenu && m_reactionSubmenuBounds.contains(m_pointerX, m_pointerY)));
        if (!m_dialogKind.empty())
          return (id == "invite:options"
                  && m_channelInvite.dropdown
                  && m_channelInvite.dropdownBounds.contains(m_pointerX, m_pointerY))
              || (m_dialogBounds.contains(m_pointerX, m_pointerY)
                  && (id == "dialog"
                      || id.starts_with("server-menu:")
                      || id.starts_with("toggle:server-menu:")
                      || id == "channel-name"
                      || id == "channel-topic"
                      || id == "channel-access-filter"
                      || id.starts_with("channel-create")
                      || id.starts_with("toggle:channel-create")
                      || id.starts_with("channel-edit:")
                      || id.starts_with("invite:")
                      || id.starts_with("application-")
                      || id.starts_with("forum-")
                      || id == "profile-scrollbar"));
        if (m_emojiMenu)
          return (id == "emoji-search" || (m_stickerTab && (id == "sticker-scroll" || id == "sticker-rail-scroll")))
              && m_emojiBounds.contains(m_pointerX, m_pointerY);
        if (m_gifMenu)
          return id == "gif-search" && m_gifBounds.contains(m_pointerX, m_pointerY);
        if (m_inboxOpen && m_inboxBounds.contains(m_pointerX, m_pointerY))
          return id == "inbox-scrollbar";
        if (m_completionQuery && m_completionBounds.contains(m_pointerX, m_pointerY))
          return id == "completion-scrollbar";
        if (m_settingsOpen && m_fontPickerOpen)
          return m_fontPickerBounds.contains(m_pointerX, m_pointerY)
              && (id == "settings:font_search" || id == "font-picker-scrollbar");
        return true;
      };
      bool nativeAllowed = false;
      for (auto* node = static_cast<Node*>(area); node && !nativeAllowed; node = node->parent()) {
        for (const auto& [id, control] : m_inputs)
          if (node == control && allowed(id))
            nativeAllowed = true;
        for (const auto& [id, control] : m_widgets)
          if (node == control && allowed(id))
            nativeAllowed = true;
      }
      if (nativeAllowed)
        shape = area->cursorShape() ? area->cursorShape() : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }
    m_window.setCursorShape(shape);
  }

  void ClientApplication::changeImage(int direction) {
    if (m_viewerImages.empty())
      return;
    auto it = std::ranges::find(m_viewerImages, m_viewImage, &ViewerImage::path);
    const int current = it == m_viewerImages.end() ? 0 : it - m_viewerImages.begin();
    const auto& next = m_viewerImages[(current + direction + m_viewerImages.size()) % m_viewerImages.size()];
    const auto path = next.path, url = next.url;
    closeImageViewer();
    m_viewImage = path;
    m_viewUrl = url;
    m_viewZoom = 1;
    m_viewPanX = m_viewPanY = 0;
  }
  void ClientApplication::drawImageViewer(Node& root, float w, float h) {
    const auto* draft = draftImage(m_viewImage);
    if ((m_viewImage.starts_with(draftImagePrefix) || m_viewImage.starts_with("pending-attachment:")) && !draft) {
      // A send acknowledgement or removal can release this texture while the
      // viewer is open. Resolve it from the draft on each frame; never own it twice.
      closeImageViewer();
      m_viewerImages.clear();
      m_window.invalidate();
      return;
    }
    std::erase_if(m_hits, [](const auto& hit) {
      return hit.action != Action::Close && hit.action != Action::Minimize && hit.action != Action::Maximize;
    });
    Color backdrop = theme::background;
    backdrop.a = 0.96F;
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, backdrop);
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::CloseImage, {}});
    const float left = 60, top = 94, availableW = w - 120, availableH = h - 174;
    const auto found = m_images.find(m_viewImage);
    const auto texture = draft ? draft->texture : found == m_images.end() ? TextureHandle{} : found->second.texture;
    float iw = texture.valid() ? texture.width : 640, ih = texture.valid() ? texture.height : 480;
    const auto metadata = std::ranges::find(m_viewerImages, m_viewImage, &ViewerImage::path);
    if (metadata != m_viewerImages.end() && metadata->width > 0 && metadata->height > 0) {
      iw = metadata->width;
      ih = metadata->height;
    }
    const float scale = std::min({availableW / iw, availableH / ih, 1.0F}) * m_viewZoom;
    iw *= scale;
    ih *= scale;
    m_viewPanX = std::clamp(m_viewPanX, -std::max(0.0F, (iw - availableW) / 2), std::max(0.0F, (iw - availableW) / 2));
    m_viewPanY = std::clamp(m_viewPanY, -std::max(0.0F, (ih - availableH) / 2), std::max(0.0F, (ih - availableH) / 2));
    auto imageLayer = std::make_unique<Node>();
    imageLayer->setPosition(left, top);
    imageLayer->setSize(availableW, availableH);
    imageLayer->setClipChildren(true);
    const float x = (availableW - iw) / 2 + m_viewPanX, y = (availableH - ih) / 2 + m_viewPanY;
    if (draft) {
      auto image = std::make_unique<ImageNode>();
      image->setPosition(x, y);
      image->setSize(iw, ih);
      image->setTextureId(texture.id);
      image->setTextureSize(texture.width, texture.height);
      image->setFitMode(ImageFitMode::Contain);
      image->setRadius(Style::scaledRadius(Style::surfaceRadius));
      imageLayer->addChild(std::move(image));
    } else
      picture(*imageLayer, x, y, iw, ih, m_viewImage, Style::surfaceRadius, true, false, ImageFitMode::Contain);
    root.addChild(std::move(imageLayer));
    const float hitX = std::max(left, left + x), hitY = std::max(top, top + y);
    const float hitRight = std::min(left + availableW, left + x + iw),
                hitBottom = std::min(top + availableH, top + y + ih);
    m_hits.push_back(
        {hitX, hitY, std::max(0.0F, hitRight - hitX), std::max(0.0F, hitBottom - hitY), Action::ImagePan, {}}
    );
    label(
        root, 28, 53, w - 140, draft ? draft->upload.filename : "Image", Style::fontSizeSection, theme::foreground,
        FontWeight::SemiBold
    );
    iconButton(root, {w - 64, 46, 32, 32, Action::CloseImage, {}}, "x", "Close image");
    if (m_viewerImages.size() > 1) {
      iconButton(
          root, {12, top + availableH / 2 - 20, 32, 32, Action::ImagePrevious, {}}, "chevron-left", "Previous image"
      );
      iconButton(
          root, {w - 54, top + availableH / 2 - 20, 32, 32, Action::ImageNext, {}}, "chevron-right", "Next image"
      );
    }
    iconButton(root, {w / 2 - 118, h - 62, 32, 32, Action::ImageZoom, "out"}, "zoom-out", "Zoom out");
    button(
        root, {w / 2 - 68, h - 62, 80, 34, Action::ImageZoom, "fit"},
        std::to_string(static_cast<int>(m_viewZoom * 100)) + "%"
    );
    iconButton(root, {w / 2 + 20, h - 62, 32, 32, Action::ImageZoom, "in"}, "zoom-in", "Zoom in");
    if (!m_imageStatus.empty())
      label(
          root, Style::spaceXl, h - Style::spaceGroup, std::max(0.0F, w / 2 - Style::spaceGroup * 2), m_imageStatus,
          Style::fontSizeDescription, theme::foreground
      );
  }
  void ClientApplication::prepareMediaGalleryDemo(int count, bool portrait) {
    // Source sizes and pixel patterns are offline fixture data, not layout values.
    constexpr std::array<std::pair<int, int>, 5> sizes{{{480, 280}, {280, 480}, {360, 360}, {640, 240}, {240, 640}}};
    auto attachments = nlohmann::json::array();
    for (int i = 0; i < count; ++i) {
      auto [width, height] = sizes[i % sizes.size()];
      if (i == 0 && portrait)
        std::swap(width, height);
      const auto url = "https://cdn.discordapp.com/attachments/100/"
          + std::to_string(9700 + i)
          + (portrait ? "/portrait.png" : "/landscape.png");
      attachments.push_back(
          {{"id", std::to_string(9700 + i)},
           {"filename", "image.png"},
           {"url", url},
           {"proxy_url", url},
           {"content_type", "image/png"},
           {"width", width},
           {"height", height}}
      );
      if (!m_images[api::mediaUrl(url)].texture.valid()) {
        std::vector<std::uint8_t> pixels(width * height * 4);
        for (int y = 0; y < height; ++y)
          for (int x = 0; x < width; ++x) {
            const auto at = (y * width + x) * 4;
            const bool stripe = (x / 32 + y / 32) % 2;
            pixels[at] = 45 + (i * 37 + x * 100 / width) % 150;
            pixels[at + 1] = 55 + (i * 19 + y * 120 / height) % 150;
            pixels[at + 2] = stripe ? 210 : 130;
            pixels[at + 3] = 255;
          }
        m_images[api::mediaUrl(url)].texture =
            m_window.context().textureManager().loadFromRgba(pixels.data(), width, height);
        m_images[api::mediaUrl(url)].bytes = encodePng(pixels.data(), width, height);
      }
    }
    m_messages = api::parseMessages(
        nlohmann::json::array(
            {{{"id", "9700"},
              {"channel_id", m_channelId},
              {"author", {{"id", "11"}, {"username", "Maya"}}},
              {"timestamp", "2026-10-20T12:35:00Z"},
              {"content", std::to_string(count) + " pictures in one message"},
              {"attachments", attachments}}}
        )
    );
    m_showMembers = false;
    m_unreadTimeline.snapshot({});
    m_unreadTimeline.positioned();
    m_messageLayouts.clear();
    m_messageScroll = 0;
  }
  void ClientApplication::demoMediaGalleryStep() {
    const auto step = m_demoInteractionStep++;
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto& message = m_messages.front();
    const auto& pictures = m_messageLayouts.at(message.id).layout.pictures;
    require(pictures.size() == message.attachments.size(), "Every parsed attachment needs a mosaic tile");
    for (std::size_t i = 0; i < pictures.size(); ++i) {
      require(
          pictures[i].media == message.attachments[i].image && pictures[i].link == message.attachments[i].url,
          "Mosaic tiles preserve original sources, dimensions and order"
      );
      require(
          pictures[i].width > 0 && pictures[i].height > 0 && std::isfinite(pictures[i].height),
          "Mosaic tiles remain usable through narrow layout changes"
      );
    }
    if (step == 0) {
      require(pictures[0].y == pictures[1].y, "Two pictures share one gallery row");
      const auto hit = std::ranges::find_if(m_hits, [&](const auto& item) {
        return item.action == Action::ViewImage && item.id == pictures[0].media.url;
      });
      require(hit != m_hits.end(), "The first mosaic tile needs a native image-viewer target");
      const float x = hit->x + hit->width / 2;
      const float y = hit->y + hit->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.released();
      require(
          m_viewImage == pictures[0].media.url && m_viewerImages.size() == pictures.size(),
          "Clicking a cropped tile opens its original image and the full gallery"
      );
    } else if (step == 1) {
      const auto texture = m_images.at(m_viewImage).texture;
      bool fullImage = false;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Image) {
          const auto& image = static_cast<const ImageNode&>(node);
          fullImage |= image.textureId() == texture.id && image.fitMode() == ImageFitMode::Contain;
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(fullImage, "The viewer contains the whole original image rather than its cropped tile");
      onKey({.symbol = XKB_KEY_Right});
      require(
          m_viewImage == message.attachments[1].image.url && m_viewUrl == message.attachments[1].url,
          "Gallery navigation follows attachment order and retains the original link"
      );
      onKey({.symbol = XKB_KEY_Escape});
      require(m_viewImage.empty(), "Escape closes the gallery viewer");
      prepareMediaGalleryDemo(3, true);
    } else if (step == 2) {
      require(
          pictures[0].height > pictures[1].height && pictures[1].x == pictures[2].x,
          "A portrait lead sits beside two stacked images"
      );
      prepareMediaGalleryDemo(3);
    } else if (step == 3) {
      require(
          pictures[0].width > pictures[1].width && pictures[1].y == pictures[2].y,
          "A landscape lead sits above two images"
      );
      prepareMediaGalleryDemo(4);
    } else if (step < 10) {
      require(pictures.size() == static_cast<std::size_t>(step), "The gallery replay covers every attachment count");
      prepareMediaGalleryDemo(step + 1);
    } else if (step == 10) {
      require(pictures.size() == 10, "The largest supported gallery retains all ten images");
      m_window.setUiScale(1.7F);
    } else if (step == 11) {
      m_window.setUiScale(1);
      prepareMediaGalleryDemo(1);
    } else {
      require(
          std::abs(
              pictures[0].width / pictures[0].height
              - static_cast<float>(pictures[0].media.width) / pictures[0].media.height
          ) < .001F,
          "Single-image messages keep their uncropped aspect ratio"
      );
      require(m_pendingImages.empty(), "Offline gallery previews never fetch attachment bytes");
      std::clog << "Offline adaptive media gallery, viewer, source order and narrow layout replay passed\n";
    }
    m_window.invalidate();
  }
  void ClientApplication::demoImageMenuStep() {
    const auto step = m_demoInteractionStep++;
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const float titleOffset = m_window.titlebarHeight() - layout::Window::titlebarHeight;
    const auto openMenu = [&] {
      const auto image = std::ranges::find(m_hits, Action::ImagePan, &Hit::action);
      require(image != m_hits.end(), "The image viewer needs a visible native image target");
      m_window.contextClick(
          image->x + image->width - Style::borderWidth, image->y + image->height - Style::borderWidth + titleOffset
      );
      require(m_contextOpen && m_imageContext == m_viewImage, "Right-click opens the current image's menu");
    };
    const auto clickEntry = [&](Action action) {
      const auto found = std::ranges::find(m_contextEntries, static_cast<int>(action), &ContextMenuControlEntry::id);
      require(
          found != m_contextEntries.end() && found->enabled && m_contextControl,
          "The requested image menu action is enabled"
      );
      const auto index = static_cast<std::size_t>(found - m_contextEntries.begin());
      const float x = m_contextBounds.x + m_contextBounds.width / 2;
      const float y = m_contextBounds.y + (m_contextControl->rowTop(index) + m_contextControl->rowBottom(index)) / 2;
      m_window.motion(x, y + titleOffset);
      require(
          m_window.cursorShape() == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
          "Image menu entries keep their native pointer cursor above the viewer"
      );
      m_window.click(x, y + titleOffset, 272);
      m_window.released();
      require(!m_contextOpen && !m_viewImage.empty(), "Image actions dismiss only their menu");
    };
    if (step == 0) {
      const auto tile = std::ranges::find(m_hits, Action::ViewImage, &Hit::action);
      require(tile != m_hits.end(), "The image-menu fixture starts from a real gallery tile");
      const float x = tile->x + tile->width / 2, y = tile->y + tile->height / 2 + titleOffset;
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.released();
      m_window.motion(chatLeft() + Style::spaceSection, m_composerTop + Style::spaceXl + titleOffset);
      require(!m_controlInput.hoveredArea(), "Opening an image clears covered native hover and pointer capture");
    } else if (step == 1) {
      require(
          m_tooltip.empty() && m_contextTargets.empty() && m_textAreas.empty(),
          "Covered chat controls contribute no tooltip, profile targets or text selection to the viewer"
      );
      openMenu();
    } else if (step == 2) {
      require(
          m_contextEntries.size() == 5 && m_contextEntries[2].separator,
          "The image menu has Copy Image, Save Image, a separator and both media-link actions"
      );
      require(
          m_contextBounds.x >= 0
              && m_contextBounds.y >= 0
              && m_contextBounds.x + m_contextBounds.width <= m_window.width()
              && m_contextBounds.y + m_contextBounds.height <= m_window.height(),
          "The image menu remains inside the window near the image edge"
      );
      require(
          std::ranges::none_of(m_hits, [](const auto& hit) { return hit.action == Action::OpenUrl; }),
          "The viewer no longer has a detached Open original button"
      );
      clickEntry(Action::CopyImage);
    } else if (step == 3) {
      require(m_imageStatus == "Image copied.", "Copy Image uses the cached original bytes");
      onKey({.symbol = XKB_KEY_c, .control = true});
      require(m_imageStatus == "Image copied.", "Ctrl+C uses the same image-copy path");
      auto& original = m_images.at(viewerMediaLink()).bytes;
      const auto previous = original;
      original = {'G', 'I', 'F', '8', '9', 'a',  1, 0, 1, 0, 0x80, 0, 0, 0, 0, 0, 255, 255,  255, 0x21, 0xf9, 4,
                  1,   0,   0,   0,   0,   0x2c, 0, 0, 0, 0, 1,    0, 1, 0, 0, 2, 2,   0x44, 1,   0,    0x3b};
      require(imageMime(original) == "image/gif", "The clipboard fixture contains real GIF bytes");
      imageViewerAction(Action::CopyImage);
      require(
          m_imageStatus == "Image copied." && imageMime(original) == "image/gif",
          "Copying a GIF produces a PNG clipboard image without rewriting the original animation"
      );
      original = previous;
      openMenu();
    } else if (step == 4)
      clickEntry(Action::SaveImage);
    else if (step == 5) {
      require(
          m_imageStatus == "The save dialog is unavailable in this offline preview.",
          "The actual Save Image action does not open the desktop portal in offline replays"
      );
      m_imageSaveRequest = ++m_imageSaveSerial;
      imageSaved({.request = m_imageSaveRequest, .error = "Synthetic save failure", .saving = true});
      require(m_imageStatus == "Synthetic save failure", "Image saving exposes errors in the viewer");
      m_imageSaveRequest = ++m_imageSaveSerial;
      imageSaved({.request = m_imageSaveRequest - 1, .saving = true});
      require(m_imageSaveRequest != 0, "A stale save acknowledgement cannot complete a newer operation");
      imageSaved({.request = m_imageSaveRequest, .saving = true});
      require(m_imageStatus == "Image saved.", "A successful save has explicit completion feedback");
      openMenu();
    } else if (step == 6)
      clickEntry(Action::CopyMediaLink);
    else if (step == 7) {
      require(
          m_imageStatus == "Media link copied." && viewerMediaLink() == m_messages.front().attachments[0].url,
          "Copy Media Link retains the original attachment URL"
      );
      openMenu();
    } else if (step == 8)
      clickEntry(Action::OpenMediaLink);
    else if (step == 9) {
      require(
          m_imageStatus == "Opening media link…",
          "Open Media Link selects the browser action without launching it offline"
      );
      openMenu();
    } else if (step == 10) {
      onKey({.symbol = XKB_KEY_Escape});
      require(!m_contextOpen && !m_viewImage.empty(), "Escape dismisses the menu before the viewer");
      contextAction(Action::CopyImage);
      require(!m_viewImage.empty(), "Stale menu actions do not dismiss the image");
      closeImageViewer();
      prepareAttachmentsDemo();
      openDraftAttachment("1");
    } else if (step == 11)
      openMenu();
    else {
      require(
          m_contextEntries[0].enabled
              && m_contextEntries[1].enabled
              && !m_contextEntries[3].enabled
              && !m_contextEntries[4].enabled,
          "Unsent images support copying/saving and disable unavailable media links"
      );
      onKey({.symbol = XKB_KEY_Escape});
      onKey({.symbol = XKB_KEY_Escape});
      require(
          m_viewImage.empty() && !m_contextOpen && m_attachments.at(m_channelId).size() == 10,
          "Dismissing a draft image's menu and viewer preserves every attachment"
      );
      require(
          m_pendingImages.empty() && m_imageSaveRequest == 0,
          "Image-menu replays never request network or desktop state"
      );
      std::clog
          << "Offline image context menu, native actions, keyboard priority, save feedback and draft replay passed\n";
    }
    m_window.invalidate();
  }
  void ClientApplication::prepareAttachmentsDemo() {
    constexpr int width = 160, height = 320, count = 10;
    std::vector<std::uint8_t> pixels(width * height * 4);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        const auto at = (y * width + x) * 4;
        pixels[at] = x % 2 ? 180 : 70;
        pixels[at + 1] = y % 2 ? 150 : 80;
        pixels[at + 2] = 210;
        pixels[at + 3] = 255;
      }
    const auto bytes = encodePng(pixels.data(), width, height);
    for (int index = 0; index < count; ++index) {
      const auto key = std::to_string(++m_attachmentSerial);
      const auto texture = m_window.context().textureManager().loadFromRgba(pixels.data(), width, height);
      m_attachments[m_channelId].push_back({{"draft-" + key + ".png", "image/png", bytes}, texture, key});
    }
    m_drafts[m_channelId] = "Unsent draft";
  }
  void ClientApplication::demoAttachmentsStep() {
    const auto step = m_demoInteractionStep++;
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto click = [&](Action action, const std::string& key = {}) {
      const auto hit = std::ranges::find_if(m_hits, [&](const auto& value) {
        return value.action == action && (key.empty() || value.id == key) && value.width > 0;
      });
      require(hit != m_hits.end(), "Draft attachment replay needs a visible control");
      const auto x = hit->x + hit->width / 2;
      const auto y = hit->y + hit->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.released();
    };
    if (step == 0) {
      click(Action::ViewAttachment, "1");
      require(draftImage(m_viewImage) != nullptr && m_viewUrl.empty(), "Draft images open locally without a URL");
      require(m_viewerImages.size() == 10, "The draft viewer contains the draft image gallery");
    } else if (step == 1) {
      click(Action::ImageZoom, "in");
      require(m_viewZoom > 1, "Draft image zoom uses the normal viewer controls");
      click(Action::ImageNext);
      require(draftImage(m_viewImage)->key == "2" && m_viewZoom == 1, "Gallery navigation resets image zoom");
    } else if (step == 2) {
      click(Action::ImagePrevious);
      onKey({.symbol = XKB_KEY_Left});
      require(draftImage(m_viewImage)->key == "10", "Draft image gallery wraps to the last image");
      onKey({.symbol = XKB_KEY_Escape});
      require(m_viewImage.empty(), "Escape returns to the draft without removing attachments");
      m_attachmentScroll = m_attachments.at(m_channelId).size() * layout::DraftAttachment::step;
    } else if (step == 3) {
      for (const auto& hit : m_hits)
        if (hit.action == Action::ViewAttachment)
          require(
              hit.x >= m_attachmentX && hit.x + hit.width <= m_attachmentX + m_attachmentWidth,
              "Scrolled attachment preview targets remain inside their viewport"
          );
      click(Action::RemoveAttachment, "10");
      require(m_viewImage.empty() && m_attachments.at(m_channelId).size() == 9, "Remove stays separate from preview");
    } else if (step == 4) {
      click(Action::ViewAttachment, "9");
      require(draftImage(m_viewImage)->key == "9", "A scrolled draft thumbnail opens the correct image");
      // A stale removal/send acknowledgement can arrive after opening the viewer.
      activate({.action = Action::RemoveAttachment, .id = "9"});
    } else if (step == 5) {
      require(m_viewImage.empty(), "Releasing the selected draft closes its viewer without a stale texture");
      require(m_attachments.at(m_channelId).size() == 8, "Previewing preserves the remaining draft images");
      activate({.action = Action::ViewAttachment, .id = "9"});
      require(m_viewImage.empty(), "A stale attachment target cannot open a removed draft image");
    } else if (step == 6) {
      openSettings();
      selectSettingsPage("cressida");
    } else {
      const auto at = m_widgets.find("toggle:randomize_media_filenames");
      require(at != m_widgets.end() && at->second->parent(), "Media naming uses a mounted native settings toggle");
      auto* node = at->second;
      float x = node->width() / 2, y = node->height() / 2;
      for (; node && node != m_scene.get(); node = node->parent()) {
        x += node->x();
        y += node->y();
      }
      require(
          m_preferences.randomizeMediaFilenames == (step == 8),
          "The native toggle retains its account value across redraws"
      );
      m_window.motion(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight);
      m_window.click(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272);
      m_window.released();
      require(
          m_preferences.randomizeMediaFilenames == (step == 7),
          "Native clicks enable and disable random media filenames"
      );
      require(m_drafts.at(m_channelId) == "Unsent draft" && !m_sending, "Previewing never changes or sends the draft");
      require(m_pendingImages.empty(), "Local draft previews never request media from the network");
      if (step == 8)
        std::clog << "Offline draft attachment and native random filename toggle replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
