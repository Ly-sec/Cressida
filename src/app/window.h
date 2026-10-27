#pragma once

#include "app/window_controls.h"
#include "render/render_context.h"
#include "render/render_target.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class InputDispatcher;

namespace app {
  struct Key {
    std::uint32_t symbol = 0;
    std::string text;
    bool control = false;
    bool shift = false;
    bool alt = false, super = false;
  };

  // A normal xdg-shell desktop window using the project's EGL/GLES renderer.
  class Window {
  public:
    Window();
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    void initialize();
    // Detach before destroying the owning dispatcher.
    void setTextInputDispatcher(InputDispatcher* dispatcher);
    void updateTextInput();
    void
    run(int wakeFd, std::function<void()> tick, int frameLimit = 0, int assetFd = -1, int videoFd = -1,
        int themeFd = -1, int pickerFd = -1, int audioFd = -1, int notificationFd = -1, int trayFd = -1,
        int iconFd = -1, int instanceFd = -1, int voiceFd = -1);
    void close();
    void requestClose();
    void hide();
    void show(std::string activationToken = {});
    bool visible() const;
    // Changes only when a hidden window needs a new native shell role.
    std::uint64_t toplevelGeneration() const;
    void minimize();
    void toggleMaximized();
    bool maximized() const;
    bool fullscreen() const;
    void setFullscreen(bool enabled);
    bool active() const;
    bool shiftPressed() const;
    bool preciseScroll() const;
    float titlebarHeight() const;
    void setDesktopDecorations(bool enabled);
    bool desktopDecorations() const;
    bool desktopDecorationsAvailable() const;
    void setWindowControls(const WindowControlPreferences& preferences);
    const WindowCapabilities& capabilities() const;
    bool titlebarDragContains(float x, float y) const;
    void setCursorShape(std::uint32_t shape);
    std::uint32_t cursorShape() const;
    void invalidate();
    void setUiScale(float scale);
    float uiScale() const;
    void paste(bool images = false);
    void copy(std::string bytes, std::string mime = "text/plain;charset=utf-8");
    int width() const;
    int height() const;
    RenderContext& context();
    RenderTarget& target();
    GlSharedContext& graphics();

    std::function<void()> draw;
    // Run before a presented frame; true requests another compositor-paced frame.
    std::function<bool()> animate;
    std::function<void(float, float, std::uint32_t)> click;
    std::function<void(float, float)> contextClick;
    std::function<void(float, float)> motion;
    std::function<void()> released;
    std::function<void()> pointerLeft;
    std::function<void(float)> scroll;
    std::function<void(float)> zoom;
    std::function<void(const Key&)> key;
    std::function<void(std::uint32_t)> keyReleased;
    std::function<void(bool)> focusChanged;
    std::function<void()> closeRequested;
    std::function<void(std::string, std::string)> pasted;

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };
} // namespace app
