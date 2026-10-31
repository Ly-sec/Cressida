#include "app/stream_popout.h"

#include "app/layout_metrics.h"
#include "app/theme.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/rect_node.h"
#include "render/scene/text_node.h"

#include <array>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  StreamPopout::StreamPopout(
      std::string key, std::string title, std::shared_ptr<voice::VideoPipeline> video, Action action, float scale
  )
      : m_key(std::move(key)), m_title(std::move(title)), m_video(std::move(video)), m_action(std::move(action)) {
    m_window.initialize();
    m_window.setUiScale(scale);
    m_window.draw = [this] { draw(); };
    m_window.key = [this](const Key& key) {
      if (key.symbol == XKB_KEY_Escape) {
        if (m_window.fullscreen())
          m_window.setFullscreen(false);
        else
          m_window.close();
      } else if (key.symbol == XKB_KEY_f || key.symbol == XKB_KEY_F11)
        m_window.setFullscreen(!m_window.fullscreen());
      else if (key.symbol == XKB_KEY_plus || key.symbol == XKB_KEY_equal)
        m_action("volume-up:" + m_key);
      else if (key.symbol == XKB_KEY_minus)
        m_action("volume-down:" + m_key);
    };
    m_window.click = [this](float x, float y, std::uint32_t) {
      const auto controls=layout::Sharing::popoutControls(m_window.width(),m_window.height());
      const auto target=std::ranges::find_if(controls,[&](const auto& frame) { return x>=frame.x && x<frame.x+frame.width && y>=frame.y && y<frame.y+frame.height; });
      if (target==controls.end()) return;
      const auto index=target-controls.begin();
      if (index == 0)
        m_action("volume-down:" + m_key);
      else if (index == 1)
        m_action("volume-up:" + m_key);
      else if (index == 2)
        m_window.setFullscreen(!m_window.fullscreen());
      else {
        m_action("unwatch:" + m_key);
        m_window.close();
      }
    };
    m_window.closeRequested = [this] { m_window.close(); };
  }
  StreamPopout::~StreamPopout() {
    if (m_texture.valid())
      m_window.context().textureManager().unload(m_texture);
  }
  int StreamPopout::eventFd() const { return m_window.eventFd(); }
  bool StreamPopout::advance() {
    const auto revision = m_video->statistics().frameRevision;
    if (revision != m_revision)
      m_window.invalidate();
    return m_window.dispatch();
  }
  void StreamPopout::draw() {
    using namespace theme;
    auto& textures = m_window.context().textureManager();
    const auto snapshot=m_video->snapshot(); const auto& frame=snapshot.frame;
    const auto revision=snapshot.statistics.frameRevision;
    if (frame && frame->valid() && (!m_texture.valid() || revision != m_revision)) {
      if (m_texture.valid() && m_texture.width == frame->width && m_texture.height == frame->height)
        textures.updateSubImage(
            m_texture, frame->rgba.data(), 0, 0, frame->width, frame->height, TextureDataFormat::Rgba
        );
      else
        textures.replace(m_texture, frame->rgba.data(), frame->width, frame->height, TextureDataFormat::Rgba);
      m_revision = revision;
    }
    m_scene = std::make_unique<Node>();
    const float width = m_window.width(), height = m_window.height();
    auto base = std::make_unique<RectNode>();
    base->setSize(width, height);
    base->setStyle({.fill = background});
    m_scene->addChild(std::move(base));
    const auto label = [&](float x, float y, float w, const std::string& text) {
      auto node = std::make_unique<TextNode>();
      node->setPosition(x, y);
      node->setText(text);
      node->setFontSize(Style::fontSizeBody);
      node->setColor(foreground);
      node->setMaxWidth(w);
      node->setMaxLines(1);
      m_scene->addChild(std::move(node));
    };
    const float top = m_window.titlebarHeight() + Style::spaceLg;
    label(Style::spaceLg, top, width - Style::spaceLg * 2, m_title);
    if (m_texture.valid()) {
      auto image = std::make_unique<ImageNode>();
      image->setPosition(Style::spaceLg, top + layout::Sharing::row);
      image->setSize(
          width - Style::spaceLg * 2, std::max(0.0F, height - top - layout::Sharing::row - layout::Sharing::footer)
      );
      image->setTextureId(m_texture.id);
      image->setTextureSize(m_texture.width, m_texture.height);
      image->setFitMode(ImageFitMode::Contain);
      m_scene->addChild(std::move(image));
    }
    const std::array<std::string, 4> captions{
        "Volume −", std::to_string(static_cast<int>(m_volume * 100)) + "% +", "Fullscreen", "Stop watching"
    };
    const auto controls=layout::Sharing::popoutControls(width,height);
    for (std::size_t i = 0; i < captions.size(); ++i) {
      auto button = std::make_unique<RectNode>();
      const auto& frame=controls[i];
      button->setPosition(frame.x,frame.y);
      button->setSize(frame.width,frame.height);
      button->setStyle({.fill = panel, .radius = Style::scaledRadiusMd()});
      m_scene->addChild(std::move(button));
      label(
          frame.x+Style::spaceSm,frame.y+Style::spaceXs,
          frame.width-Style::spaceLg, captions[i]
      );
    }
    m_window.context().renderScene(m_window.target(), m_scene.get());
  }
} // namespace app
