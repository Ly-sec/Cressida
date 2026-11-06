#include "api/video_source.h"
#include "app/client_application.h"
#include "app/theme.h"
#include "core/files/resource_paths.h"
#include "render/gl_shared_context.h"
#include "render/scene/image_node.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;
  namespace {
    std::string videoTime(double seconds) {
      const auto value = std::clamp<int>(std::isfinite(seconds) ? int(seconds) : 0, 0, 24 * 60 * 60);
      return std::to_string(value / 60) + ":" + (value % 60 < 10 ? "0" : "") + std::to_string(value % 60);
    }
  } // namespace
  void ClientApplication::closeVideo() {
    if (m_videoExpanded)
      setVideoExpanded(false);
    if (m_videoContext != EGL_NO_CONTEXT) {
      eglMakeCurrent(m_window.graphics().display(), EGL_NO_SURFACE, EGL_NO_SURFACE, m_videoContext);
      m_video.close();
      m_videoTarget.destroy();
      eglMakeCurrent(m_window.graphics().display(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext(m_window.graphics().display(), m_videoContext);
      m_videoContext = EGL_NO_CONTEXT;
      m_window.context().makeCurrent(m_window.target());
    } else
      m_video.close();
    m_videoSource.clear();
    m_videoPlayRequested = false;
    m_videoExpanded = false;
    m_videoDrag = 0;
  }
  void ClientApplication::openVideo(const std::string& source) {
    if (source == m_videoSource && m_video.state().error.empty()) {
      toggleVideo();
      return;
    }
    closeVideo();
    std::shared_ptr<media::VideoInput> input;
    if (m_demo) {
      if ((m_demoOverlay != "video" && m_demoOverlay != "video-cycle") || source != "demo-video")
        return;
      std::ifstream file(paths::assetPath("demo-video.mp4"), std::ios::binary | std::ios::ate);
      const auto size = file.tellg();
      if (size <= 0 || size > 20 * 1024 * 1024)
        return;
      std::vector<std::uint8_t> bytes(size);
      file.seekg(0);
      if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        return;
      input = media::memoryVideoInput(bytes);
    } else
      input = api::videoSource(source);
    if (!input)
      return;
    m_videoSource = source;
    m_videoContext = m_window.graphics().createContext(m_window.graphics().rootContext(), "video");
    if (m_videoContext == EGL_NO_CONTEXT)
      return;
    eglMakeCurrent(m_window.graphics().display(), EGL_NO_SURFACE, EGL_NO_SURFACE, m_videoContext);
    m_video.open(
        std::move(input), [](void*, const char* name) { return reinterpret_cast<void*>(eglGetProcAddress(name)); },
        m_demo
    );
    m_videoPlayRequested = true; // Only reached from the explicit Play action.
    m_window.context().makeCurrent(m_window.target());
  }
  void ClientApplication::drawVideo(
      Node& parent, float x, float y, float width, float height, const std::string& source, const std::string& poster,
      bool expanded
  ) {
    float ox = 0, oy = 0;
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      ox += node->x();
      oy += node->y();
    }
    const float gx = ox + x, gy = oy + y;
    bool visible = true;
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent())
      if (node->clipChildren() && (y + height <= 0 || y >= node->height()))
        visible = false;
    if (!visible)
      return;
    const bool selected = source == m_videoSource;
    const auto& state = m_video.state();
    rectangle(parent, x, y, width, height, rgbHex(0x08090D), Style::hoverRadius);
    bool frame = false;
    if (selected && (expanded || !m_videoExpanded)) {
      m_videoVisible = true;
      m_videoBounds = {gx, gy, width, height, Action::PauseVideo, source};
      if (m_videoContext != EGL_NO_CONTEXT && state.error.empty() && state.width > 0 && state.height > 0) {
        // This getter binds the UI context. Obtain it BEFORE selecting the
        // video's context: FBO names are not shared between EGL contexts.
        auto& textures = m_window.context().textureManager();
        eglMakeCurrent(m_window.graphics().display(), EGL_NO_SURFACE, EGL_NO_SURFACE, m_videoContext);
        const bool resize =
            m_videoTarget.width() != unsigned(state.width) || m_videoTarget.height() != unsigned(state.height);
        if (resize && !m_videoTarget.create(textures, state.width, state.height)) {
          m_video.pause(true);
          m_status = "Could not allocate the video surface.";
        }
        if (m_videoTarget.valid()) {
          m_video.render(m_videoTarget.id(), state.width, state.height, resize);
          glFlush();
          frame = true;
        }
        m_window.context().makeCurrent(m_window.target());
        if (frame) {
          auto image = std::make_unique<ImageNode>();
          image->setPosition(x, y);
          image->setSize(width, height);
          image->setTextureId(m_videoTarget.colorTexture());
          image->setTextureSize(state.width, state.height);
          image->setFitMode(ImageFitMode::Contain);
          image->setRadius(Style::scaledRadius(Style::hoverRadius));
          parent.addChild(std::move(image));
        }
      }
    }
    if (!frame && !poster.empty())
      picture(parent, x, y, width, height, poster, Style::hoverRadius, false, true);
    m_hits.push_back(
        {gx, gy, width, height, selected && state.error.empty() ? Action::PauseVideo : Action::PlayVideo, source}
    );
    if (!selected || !state.playing || state.loading || !state.error.empty()) {
      rectangle(parent, x + width / 2 - Style::spaceXl, y + height / 2 - Style::spaceXl, 48, 48, panel, 24);
      glyph(
          parent, x + width / 2 - Style::spaceMd, y + height / 2 - Style::spaceMd, Style::glyphSizeFeature,
          selected && (state.loading || state.buffering) ? "loader" : "player-play-filled", foreground
      );
    }
    if (selected && !state.error.empty()) {
      label(
          parent, x + Style::spaceMd, y + height / 2 + 30, width - Style::spaceXl, state.error,
          Style::fontSizeDescription, foreground, FontWeight::Normal, 2
      );
    }
    if (selected && (expanded || !m_videoExpanded)) {
      const float controlsY = y + height - 38;
      Color scrim = rgbHex(0x08090D);
      scrim.a = .9F;
      rectangle(parent, x, controlsY - Style::spaceSm, width, 46, scrim, Style::radiusMd);
      const float progress = state.duration > 0 ? std::clamp(float(state.position / state.duration), 0.0F, 1.0F) : 0;
      const auto shown = m_videoDrag == 1 ? m_videoScrub : progress;
      const float trackWidth = width - 24;
      rectangle(parent, x + Style::spaceMd, controlsY - Style::spaceXs, trackWidth, 8, raised, Style::radiusXs);
      rectangle(parent, x + Style::spaceMd, controlsY - Style::spaceXs, trackWidth * shown, 8, accent, Style::radiusXs);
      rectangle(parent, x + Style::spaceSm + trackWidth * shown, controlsY - Style::spaceCompact, 14, 14, accent, 4);
      m_videoSeek = {
          gx + Style::spaceDense,
          oy + controlsY - (Style::spaceDense + Style::borderWidth),
          width - 20,
          16,
          Action::SeekVideo,
          source
      };
      m_hits.push_back(m_videoSeek);
      iconButton(
          parent, {x + Style::spaceCompact, controlsY + Style::spaceXs, 34, 34, Action::PauseVideo, source},
          state.playing ? "player-pause-filled" : "player-play-filled", state.playing ? "Pause" : "Play"
      );
      label(
          parent, x + (Style::spaceSection + Style::spaceXxs), controlsY + (Style::spaceDense + Style::borderWidth),
          width - 185,
          videoTime((m_videoDrag == 1 ? m_videoScrub * state.duration : state.position))
              + " / "
              + videoTime(state.duration),
          Style::fontSizeDescription, foreground
      );
      if (width >= 310) {
        iconButton(
            parent, {x + width - 140, controlsY + Style::spaceXs, 34, 34, Action::MuteVideo, source},
            state.muted || state.volume == 0 ? "volume-off" : "volume", state.muted ? "Unmute" : "Mute"
        );
        rectangle(parent, x + width - 104, controlsY + Style::spaceLg, 54, 6, raised, 1.5);
        rectangle(
            parent, x + width - 104, controlsY + Style::spaceLg, 54 * (state.muted ? 0 : state.volume / 100), 8, accent,
            1.5
        );
        m_videoVolume = {gx + width - 107, oy + controlsY + Style::spaceCompact, 60, 24, Action::VolumeVideo, source};
        m_hits.push_back(m_videoVolume);
      }
      iconButton(
          parent, {x + width - 38, controlsY + Style::spaceXs, 34, 34, Action::ExpandVideo, source},
          expanded ? "arrows-minimize" : "arrows-maximize",
          m_videoFullscreen ? "Exit fullscreen" : expanded ? "Exit expanded player" : "Expand video"
      );
      if (state.loading || state.buffering)
        label(
            parent, x + Style::spaceMd, y + Style::spaceMd, width - Style::spaceXl,
            state.loading ? "Loading video…" : "Buffering…", Style::fontSizeDescription, foreground
        );
    } else {
      glyph(parent, x + width - Style::spaceWide, y + height - 27, Style::glyphSizeMedium, "video", foreground);
    }
  }
  void ClientApplication::drawVideoViewer(Node& root, float width, float height) {
    std::erase_if(m_hits, [](const auto& hit) {
      return hit.action != Action::Close && hit.action != Action::Minimize && hit.action != Action::Maximize;
    });
    if (m_videoFullscreen) {
      drawVideo(root, 0, 36, width, height - 36, m_videoSource, m_videoPosters[m_videoSource], true);
      return;
    }
    Color backdrop = rgbHex(0x08090D);
    backdrop.a = .97F;
    rectangle(root, 0, 36, width, height - 36, backdrop);
    m_hits.push_back({0, 36, width, height - 36, Action::ExpandVideo, m_videoSource});
    const auto& state = m_video.state();
    const float ratio = state.width && state.height ? float(state.width) / state.height : 16.0F / 9;
    const float w = std::min(width - 100, (height - 180) * ratio);
    const float h = std::max(80.0F, w / ratio);
    drawVideo(
        root, (width - w) / 2, 90 + (height - 180 - h) / 2, w, h, m_videoSource, m_videoPosters[m_videoSource], true
    );
    label(root, 28, 52, width - 140, "Video", Style::fontSizeSection, foreground, FontWeight::SemiBold);
    iconButton(root, {width - 64, 46, 32, 32, Action::ExpandVideo, m_videoSource}, "x", "Close expanded player");
    button(root, {width - 182, height - 62, 156, 34, Action::OpenUrl, m_videoSource}, "Open original");
  }
  bool ClientApplication::videoAction(const Hit& hit) {
    switch (hit.action) {
    case Action::PlayVideo:
      openVideo(hit.id);
      break;
    case Action::PauseVideo:
      toggleVideo();
      break;
    case Action::ExpandVideo:
      m_scrolling.stopAll();
      closeContext();
      closeGifPicker();
      closeEmojiPicker();
      m_controlInput.setFocus(nullptr);
      m_focusedInput.clear();
      m_fieldFocused = false;
      m_videoFullscreen ? setVideoFullscreen(false) : setVideoExpanded(!m_videoExpanded);
      break;
    case Action::CloseVideo:
      closeVideo();
      break;
    case Action::MuteVideo:
      m_video.mute(!m_video.state().muted);
      break;
    case Action::SeekVideo:
      m_videoWasPlaying = m_video.state().playing;
      m_video.pause(true);
      m_videoDrag = 1;
      videoMotion(m_pointerX, m_pointerY);
      break;
    case Action::VolumeVideo:
      m_videoDrag = 2;
      videoMotion(m_pointerX, m_pointerY);
      break;
    default:
      return false;
    }
    m_window.invalidate();
    return true;
  }
  void ClientApplication::videoMotion(float x, float) {
    if (m_videoDrag == 1)
      m_videoScrub = std::clamp((x - m_videoSeek.x) / m_videoSeek.width, 0.0F, 1.0F);
    if (m_videoDrag == 2) {
      m_video.volume(std::clamp((x - m_videoVolume.x) / m_videoVolume.width, 0.0F, 1.0F) * 100);
      m_video.mute(false);
    }
  }
  void ClientApplication::videoRelease() {
    if (m_videoDrag == 1) {
      m_video.seek(m_videoScrub * m_video.state().duration);
      if (m_videoWasPlaying)
        m_video.pause(false);
    }
    m_videoDrag = 0;
  }
  bool ClientApplication::videoKey(const Key& key) {
    if (m_videoSource.empty()
        || (!m_videoExpanded && (!m_focusedInput.empty() || !m_videoBounds.contains(m_pointerX, m_pointerY))))
      return false;
    if (key.symbol == XKB_KEY_Escape)
      m_videoFullscreen ? setVideoFullscreen(false) : setVideoExpanded(false);
    else if (key.symbol == XKB_KEY_f || key.symbol == XKB_KEY_F)
      m_videoExpanded ? setVideoFullscreen(!m_videoFullscreen) : setVideoExpanded(true);
    else if (key.symbol == XKB_KEY_space)
      toggleVideo();
    else if (key.symbol == XKB_KEY_Left)
      m_video.seek(m_video.state().position - 5);
    else if (key.symbol == XKB_KEY_Right)
      m_video.seek(m_video.state().position + 5);
    else if (key.symbol == XKB_KEY_m || key.symbol == XKB_KEY_M)
      m_video.mute(!m_video.state().muted);
    else if (key.control && (key.symbol == XKB_KEY_q || key.symbol == XKB_KEY_Q))
      m_window.close();
    m_window.invalidate();
    return true;
  }
  void ClientApplication::toggleVideo() {
    if (m_videoPlayRequested) {
      m_videoPlayRequested = false;
      m_video.pause(true);
    } else if (m_video.state().loading) {
      m_videoPlayRequested = true;
    } else {
      m_video.pause(m_video.state().playing);
    }
  }
  void ClientApplication::setVideoExpanded(bool enabled) {
    if (enabled == m_videoExpanded)
      return;
    if (enabled) {
      m_videoPreviousFullscreen = m_window.fullscreen();
      enterMediaViewer();
    }
    m_videoFullscreen = false;
    m_videoExpanded = enabled;
    m_window.setFullscreen(enabled || m_videoPreviousFullscreen);
    m_videoDrag = 0;
    m_window.invalidate();
  }
  void ClientApplication::setVideoFullscreen(bool enabled) {
    if (enabled == m_videoFullscreen || !m_videoExpanded)
      return;
    m_videoFullscreen = enabled;
    m_videoDrag = 0;
    m_window.invalidate();
  }
  void ClientApplication::demoVideoStep() {
    auto require = [](bool value, const char* text) {
      if (!value)
        throw std::runtime_error(text);
    };
    auto click = [&](Action action) {
      const auto at = std::ranges::find_if(m_hits, [&](const Hit& hit) {
        return hit.action == action && (action != Action::PauseVideo || hit.width <= 34);
      });
      require(at != m_hits.end(), "Video control must have a hit region");
      const auto hit = *at;
      m_window.motion(
          hit.x + hit.width / 2, hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      const auto over = std::find_if(m_hits.rbegin(), m_hits.rend(), [&](const Hit& other) {
        return other.contains(hit.x + hit.width / 2, hit.y + hit.height / 2);
      });
      require(
          over != m_hits.rend() && over->action == action, "Video control must not be covered by another hit region"
      );
      m_window.click(
          hit.x + hit.width / 2, hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight,
          272
      );
      if (action != Action::SeekVideo && action != Action::VolumeVideo)
        m_window.released();
    };
    const auto& state = m_video.state();
    if (m_demoInteractionStep == 0) {
      click(Action::PlayVideo);
      require(!m_videoSource.empty(), "Clicking play must open the video");
    } else if (m_demoInteractionStep == 1) {
      require(state.error.empty(), "Native video must load successfully");
      if (state.loading || state.width == 0) {
        m_window.invalidate();
        return;
      }
      require(state.width == 1280 && state.height == 720, "Native UI preserves HD video dimensions");
      click(Action::PauseVideo);
      require(!state.playing, "Native pause click must reach the player");
    } else if (m_demoInteractionStep == 2) {
      require(!state.playing, "Native pause button must pause");
      click(Action::SeekVideo);
      require(m_videoDrag == 1, "Timeline receives pointer capture");
    } else if (m_demoInteractionStep == 3) {
      require(m_videoDrag == 1, "Timeline drag survives repaint");
      m_window.motion(
          m_videoSeek.x + m_videoSeek.width * .75F,
          m_videoSeek.y + Style::spaceCompact + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      m_window.released();
    } else if (m_demoInteractionStep == 4) {
      if (std::abs(state.position - state.duration * .75) > .2) {
        m_window.invalidate();
        return;
      }
      require(!state.playing, "Seeking a paused video keeps it paused");
      click(Action::ExpandVideo);
      require(!m_controlInput.hoveredArea(), "Expanding video clears covered native hover and captures");
    } else if (m_demoInteractionStep == 5) {
      require(m_videoExpanded, "Expanded player opens");
      if (!m_window.fullscreen()) {
        m_window.invalidate();
        return;
      }
      require(m_contextTargets.empty() && m_textAreas.empty(), "Expanded video excludes covered chat hit targets");
      const float previousScroll = m_messageScroll;
      m_window.scroll(Style::spaceGroup);
      require(m_messageScroll == previousScroll, "Expanded video blocks scrolling the covered conversation");
      click(Action::PauseVideo);
    } else if (m_demoInteractionStep == 6) {
      require(state.playing, "Expanded play button resumes");
      click(Action::VolumeVideo);
    } else if (m_demoInteractionStep == 7) {
      require(m_videoDrag == 2, "Volume drag survives repaint");
      m_window.motion(
          m_videoVolume.x + m_videoVolume.width * .4F,
          m_videoVolume.y + Style::spaceSm + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      m_window.released();
      require(std::abs(state.volume - 40) < 1, "Dragging volume changes the actual playback volume");
    } else if (m_demoInteractionStep == 8) {
      click(Action::MuteVideo);
    } else if (m_demoInteractionStep == 9) {
      require(state.muted, "Native mute button works");
      m_window.key({XKB_KEY_Escape});
    } else {
      require(!m_videoExpanded && !m_videoSource.empty(), "Collapse retains the playing video");
      if (m_window.fullscreen() != m_videoPreviousFullscreen) {
        m_window.invalidate();
        return;
      }
      closeVideo();
      require(!m_video.state().playing && m_videoSource.empty(), "Close releases playback state");
      std::clog << "Offline native video replay passed\n";
      m_window.close();
    }
    ++m_demoInteractionStep;
    m_window.invalidate();
  }
} // namespace app
