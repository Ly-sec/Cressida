#include "app/client_application.h"
#include "render/core/texture_manager.h"

namespace app {
  std::string ClientApplication::serverSettingsScope() const {
    if (!m_serverSettings.open)
      return {};
    return std::to_string(m_serverSettings.generation)
        + ":"
        + m_serverSettings.guild
        + ":"
        + m_serverSettings.page
        + ":"
        + m_serverSettings.resource;
  }
  void ClientApplication::startServerSettingsUpload(const std::string& field) {
    if (!m_serverSettings.open || m_serverSettings.saving)
      return;
    if (m_demo) {
      m_serverSettings.error = "The desktop file chooser is unavailable in this offline preview.";
      m_window.invalidate();
      return;
    }
    m_serverUploadField = field;
    m_serverUploadGuild = m_serverSettings.guild;
    m_serverUploadPage = m_serverSettings.page;
    m_serverUploadResource = m_serverSettings.resource;
    m_serverUploadGeneration = m_serverSettings.generation;
    m_serverUploadSession = m_session;
    std::vector<std::string> types{"image/png", "image/jpeg", "image/gif", "image/webp"};
    std::size_t maximum = 10 * 1024 * 1024;
    if (m_serverSettings.page == "emoji" || m_serverSettings.page == "roles")
      maximum = 256 * 1024;
    if (m_serverSettings.page == "stickers") {
      types = {"image/png", "image/gif", "application/json"};
      maximum = 512 * 1024;
    }
    if (m_serverSettings.page == "soundboard") {
      types = {"audio/ogg", "audio/mpeg"};
      maximum = 512 * 1024;
    }
    m_serverSettings.error.clear();
    m_filePicker.open(
        ++m_serverUploadSerial, "Choose a server upload", std::move(types), maximum,
        m_serverSettings.page == "emoji" && m_serverSettings.creating && field == "image"
    );
  }
  void ClientApplication::drainServerSettingsUploads() {
    while (auto result = m_filePicker.poll()) {
      if (slashAttachmentResult(*result))
        continue;
      if (imageSaved(*result))
        continue;
      if (userAccountPickedFile(*result))
        continue;
      if (result->request != m_serverUploadSerial
          || result->cancelled
          || !m_serverSettings.open
          || m_session != m_serverUploadSession
          || m_serverSettings.guild != m_serverUploadGuild
          || m_serverSettings.page != m_serverUploadPage
          || m_serverSettings.resource != m_serverUploadResource
          || m_serverSettings.generation != m_serverUploadGeneration)
        continue;
      if (!result->error.empty())
        m_serverSettings.error = result->error;
      else if (!result->files.empty() && m_serverSettings.page == "emoji" && m_serverSettings.creating)
        stageServerEmojiUploads(std::move(result->files));
      else if (m_serverSettings.page == "guide" && result->raster) {
        const auto& raster = *result->raster;
        replaceCachedImageTexture(
            m_images["server-guide-upload:" + m_serverSettings.guild + ":" + m_serverUploadField],
            m_window.context().textureManager(),
            m_window.context().textureManager().loadFromRgba(raster.pixels.data(), raster.width, raster.height)
        );
        serverSettingsUpload(m_serverUploadField, result->mime, std::move(result->bytes));
      } else if (
          result->raster
          && (m_serverSettings.page == "profile"
              || m_serverSettings.page == "general"
              || m_serverSettings.page == "events"
              || m_serverSettings.page == "roles"
              || m_serverSettings.page == "webhooks")
      )
        beginServerImageCrop(m_serverUploadField, result->mime, std::move(result->bytes), std::move(*result->raster));
      else
        serverSettingsUpload(m_serverUploadField, result->mime, std::move(result->bytes));
      m_window.invalidate();
    }
  }
} // namespace app
