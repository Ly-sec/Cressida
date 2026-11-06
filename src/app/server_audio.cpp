#include "api/video_source.h"
#include "app/client_application.h"
#include "core/files/resource_paths.h"

#include <fstream>

namespace app {
  void ClientApplication::stopServerSound() {
    m_soundPlayRequested = false;
    m_soundPreview.close();
    m_soundPreviewSource.clear();
    m_window.invalidate();
  }
  void ClientApplication::playServerSound(const std::string& sound) {
    if (!m_serverSettings.open || m_serverSettings.page != "soundboard" || !api::validSnowflake(sound))
      return;
    if (m_soundPreviewSource == sound) {
      stopServerSound();
      return;
    }
    stopServerSound();
    std::shared_ptr<media::VideoInput> input;
    if (m_demo) {
      std::ifstream file(paths::assetPath("demo-sound.ogg"), std::ios::binary | std::ios::ate);
      const auto size = file.tellg();
      if (size <= 0 || size > 512 * 1024)
        return;
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
      file.seekg(0);
      if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        return;
      input = media::memoryVideoInput(bytes);
    } else
      input = api::videoSource("https://cdn.discordapp.com/soundboard-sounds/" + sound, api::MediaSourceKind::OggAudio);
    if (!input)
      return;
    m_soundPreviewSource = sound;
    if (!m_soundPreview.openAudio(std::move(input), m_demo)) {
      m_serverSettings.error = "This sound could not be previewed.";
      m_soundPreviewSource.clear();
    } else
      m_soundPlayRequested = true;
    m_window.invalidate();
  }
} // namespace app
