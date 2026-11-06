#include "media/video_player.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
  void require(bool value, const char* text) {
    if (!value)
      throw std::runtime_error(text);
  }
  struct Graphics {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint texture = 0, fbo = 0;
    bool initialize() {
      display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
      if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr))
        return false;
      eglBindAPI(EGL_OPENGL_ES_API);
      const EGLint attributes[] = {
          EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE
      };
      EGLConfig config{};
      EGLint count = 0;
      if (!eglChooseConfig(display, attributes, &config, 1, &count) || !count)
        return false;
      const EGLint options[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
      context = eglCreateContext(display, config, EGL_NO_CONTEXT, options);
      if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        return false;
      glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_2D, texture);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1280, 720, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
      return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    ~Graphics() {
      if (context != EGL_NO_CONTEXT) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
      }
      if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT)
          eglDestroyContext(display, context);
        eglTerminate(display);
      }
    }
  };
} // namespace
int main(int argc, char** argv) {
  Graphics graphics;
  if (!graphics.initialize()) {
    std::cerr << "No headless EGL/GLES3 context available\n";
    return 77;
  }
  try {
    require(argc == 2, "Video fixture required");
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    media::VideoPlayer player;
    auto getProc = +[](void*, const char* name) { return reinterpret_cast<void*>(eglGetProcAddress(name)); };
    require(player.open(media::memoryVideoInput(bytes), getProc, true), "Initialize embedded video playback");
    auto pump = [&](auto condition) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < deadline) {
        player.update();
        player.drain();
        if (player.state().width > 0 && player.state().height > 0)
          player.render(graphics.fbo, 1280, 720);
        player.presented();
        if (!player.state().error.empty())
          throw std::runtime_error(player.state().error);
        if (condition())
          return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return false;
    };
    require(
        pump([&] { return !player.state().loading && player.state().width > 0; }),
        "Load a paused video without waiting for a renderable frame"
    );
    require(
        !player.state().playing && player.state().position < .1 && player.state().volume == 25,
        "Videos never autoplay and begin at 25 percent volume"
    );
    player.pause(false);
    require(pump([&] { return player.state().position > .1; }), "Explicit Play starts playback");
    player.render(graphics.fbo, 1280, 720, true);
    require(
        pump([&] {
          // Playback time can advance before the asynchronous renderer supplies its first frame.
          glBindFramebuffer(GL_FRAMEBUFFER, graphics.fbo);
          std::uint8_t top[4]{}, bottom[4]{};
          glReadPixels(640, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top);
          glReadPixels(640, 704, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bottom);
          return top[0] > 200 && top[2] < 40 && bottom[2] > 200 && bottom[0] < 40;
        }),
        "Render upright for ImageNode: texture row zero contains the video's red top half"
    );
    require(player.state().width == 1280 && player.state().height == 720, "Preserve original HD video dimensions");
    require(std::abs(player.state().duration - 3) < .2 && player.state().hasAudio, "Expose duration and audio stream");
    player.pause(true);
    require(pump([&] { return !player.state().playing; }), "Pause playback");
    const auto paused = player.state().position;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    player.drain();
    require(std::abs(player.state().position - paused) < .15, "Paused playback stops advancing");
    player.seek(1.8);
    require(pump([&] { return std::abs(player.state().position - 1.8) < .15; }), "Seek while paused");
    player.volume(35);
    player.speed(1.5);
    player.mute(true);
    require(
        pump([&] { return player.state().muted && player.state().volume == 35 && player.state().speed == 1.5; }),
        "Volume, mute and playback speed controls"
    );
    player.speed(1);
    player.pause(false);
    require(pump([&] { return player.state().position > 2; }), "Resume at the seek position");
    require(pump([&] { return player.state().ended; }), "Videos end without looping as GIFs");
    player.pause(false);
    require(pump([&] { return !player.state().ended && player.state().position < 1; }), "Replay from the beginning");
    player.close();
    require(player.state().width == 0 && !player.state().playing, "Close clears playback state");
    require(player.open(media::memoryVideoInput(bytes), getProc, true), "Start a fresh video session");
    player.close();
    std::vector<std::uint8_t> invalid(128, 0);
    require(player.open(media::memoryVideoInput(invalid), getProc, true), "Malformed media fails asynchronously");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (player.state().error.empty() && std::chrono::steady_clock::now() < deadline) {
      player.drain();
      player.render(graphics.fbo, 1280, 720);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(!player.state().error.empty(), "Malformed media produces a visible error");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
