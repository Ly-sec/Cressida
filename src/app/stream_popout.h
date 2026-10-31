#pragma once

#include "app/window.h"
#include "voice/video_codec.h"

namespace app {
  class StreamPopout {
  public:
    using Action = std::function<void(std::string)>;
    StreamPopout(
        std::string key, std::string title, std::shared_ptr<voice::VideoPipeline> video, Action action, float scale
    );
    ~StreamPopout();
    bool advance();
    int eventFd() const;
    const std::string& key() const { return m_key; }
    void volume(float volume) {
      m_volume = volume;
      m_window.invalidate();
    }

  private:
    void draw();
    Window m_window;
    std::string m_key, m_title;
    std::shared_ptr<voice::VideoPipeline> m_video;
    Action m_action;
    TextureHandle m_texture;
    std::uint64_t m_revision = 0;
    float m_volume = 1;
    std::unique_ptr<Node> m_scene;
  };
} // namespace app
