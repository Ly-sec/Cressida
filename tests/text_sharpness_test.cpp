#include "app/surface_scale.h"
#include "render/backend/gles_texture_manager.h"
#include "render/backend/render_backend.h"
#include "render/core/color.h"
#include "render/core/texture_handle.h"
#include "render/programs/glyph_program.h"
#include "render/render_target.h"
#include "render/text/cairo_text_renderer.h"
#include "ui/style.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <pango/pangocairo.h>
#include <stdexcept>
#include <vector>

namespace {

  struct Graphics {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint output = 0, mask = 0, fbo = 0;

    bool initialize() {
      display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
      if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr))
        return false;
      eglBindAPI(EGL_OPENGL_ES_API);
      const EGLint attributes[] = {
          EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE
      };
      EGLConfig config{};
      EGLint count = 0;
      if (!eglChooseConfig(display, attributes, &config, 1, &count) || !count)
        return false;
      const EGLint options[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
      context = eglCreateContext(display, config, EGL_NO_CONTEXT, options);
      if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        return false;
      glGenTextures(1, &output);
      glGenTextures(1, &mask);
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glDisable(GL_BLEND);
      return true;
    }

    ~Graphics() {
      if (context != EGL_NO_CONTEXT) {
        glDeleteTextures(1, &output);
        glDeleteTextures(1, &mask);
        glDeleteFramebuffers(1, &fbo);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
      }
      if (display != EGL_NO_DISPLAY)
        eglTerminate(display);
    }
  };

  // The test owns a surfaceless context and framebuffer. Keep Cairo's actual
  // glyph upload/draw path, while supplying only the surface-independent
  // backend operations that path uses.
  class GlyphBackend final : public RenderBackend {
  public:
    explicit GlyphBackend(GlyphProgram& program) : m_program(program) {}
    void initialize(GlSharedContext&) override {}
    void cleanup() override {}
    bool makeCurrent(RenderTarget&) override { return true; }
    bool makeCurrentNoSurface() override { return true; }
    bool beginFrame(RenderTarget&) override { return true; }
    void endFrame(RenderTarget&) override {}
    RenderGraphicsResetStatus graphicsResetStatus() override { return RenderGraphicsResetStatus::NoError; }
    void invalidateGpuResources() override {}
    void abandonAfterGraphicsReset() noexcept override {}
    std::unique_ptr<RenderSurfaceTarget> createSurfaceTarget(wl_surface*) override { return {}; }
    std::unique_ptr<RenderFramebuffer> createFramebuffer(std::uint32_t, std::uint32_t) override { return {}; }
    void bindFramebuffer(const RenderFramebuffer&) override {}
    void bindDefaultFramebuffer() override {}
    void setViewport(std::uint32_t, std::uint32_t) override {}
    void clear(Color) override {}
    void setBlendMode(RenderBlendMode) override {}
    int maxTextureSize() override { return 4096; }
    void setScissor(RenderScissor) override {}
    void disableScissor() override {}
    void drawRect(float, float, float, float, const RoundedRectStyle&, const Mat3&) override {}
    void drawImage(const RenderImageDraw&) override {}
    void drawGlyph(const RenderGlyphDraw& draw) override {
      if (!draw.tinted) {
        m_program.draw(
            draw.texture, draw.surfaceWidth, draw.surfaceHeight, draw.width, draw.height, draw.u0, draw.v0, draw.u1,
            draw.v1, draw.opacity, draw.transform
        );
        return;
      }
      m_program.drawTinted(
          draw.texture, draw.surfaceWidth, draw.surfaceHeight, draw.width, draw.height, draw.u0, draw.v0, draw.u1,
          draw.v1, draw.opacity, draw.tint, draw.transform
      );
    }
    void drawSpinner(float, float, float, float, const SpinnerStyle&, const Mat3&) override {}
    void drawFullscreenTexture(TextureId, bool) override {}
    void drawFullscreenTint(Color) override {}
    void drawFramebufferBlur(TextureId, std::uint32_t, std::uint32_t, float, float, float) override {}
    TextureManager& textureManager() override { return m_textures; }

  private:
    GlyphProgram& m_program;
    GlesTextureManager m_textures;
  };

  // Use the real Cairo/Pango coverage of small text, rather than a perfectly
  // opaque rectangle, so an extra bilinear sampling pass is observable in the
  // readback. Shape and rasterize at the physical font size just like the app.
  std::vector<unsigned char> rasterText(float scale, int width, int height) {
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    auto* cairo = cairo_create(surface);
    auto* layout = pango_cairo_create_layout(cairo);
    auto* description = pango_font_description_new();
    pango_font_description_set_family(description, "sans-serif");
    pango_font_description_set_absolute_size(description, 14.0 * scale * PANGO_SCALE);
    pango_layout_set_font_description(layout, description);
    pango_layout_set_text(layout, "Sharp text: Il1, mW / gj", -1);
    cairo_set_source_rgba(cairo, 1, 1, 1, 1);
    cairo_move_to(cairo, 2, 2);
    pango_cairo_show_layout(cairo, layout);
    cairo_surface_flush(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const auto* source = cairo_image_surface_get_data(surface);
    std::vector<unsigned char> result(static_cast<std::size_t>(width * height));
    for (int y = 0; y < height; ++y)
      std::copy_n(source + y * stride, width, result.data() + y * width);
    pango_font_description_free(description);
    g_object_unref(layout);
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);
    return result;
  }

  void verify(Graphics& graphics, GlyphProgram& program, unsigned numerator, float zoom, bool roundUp) {
    constexpr int surfaceWidth = 1001, surfaceHeight = 601;
    app::SurfaceScale policy;
    policy.setFractionalAvailable(true);
    policy.preferredFractional(numerator);
    const auto geometry = policy.geometry(surfaceWidth, surfaceHeight, zoom);
    const float outputScale = policy.factor(), rasterScale = geometry.contentScale;
    // Exercise the app's protocol-rounded native dimensions, plus a buffer
    // rounded upward to catch renderer-only callers with a different policy.
    const int bufferWidth = roundUp ? static_cast<int>(std::ceil(surfaceWidth * outputScale)) : geometry.bufferWidth;
    const int bufferHeight = roundUp ? static_cast<int>(std::ceil(surfaceHeight * outputScale)) : geometry.bufferHeight;
    RenderTarget target;
    target.setLogicalSize(static_cast<unsigned>(surfaceWidth / zoom), static_cast<unsigned>(surfaceHeight / zoom));
    target.setContentScale(rasterScale);
    target.resize(bufferWidth, bufferHeight);

    glBindTexture(GL_TEXTURE_2D, graphics.output);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bufferWidth, bufferHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, graphics.output, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      throw std::runtime_error("Text sharpness framebuffer is incomplete");
    glViewport(0, 0, bufferWidth, bufferHeight);

    const int maskWidth = static_cast<int>(std::ceil(240 * rasterScale));
    const int maskHeight = static_cast<int>(std::ceil(36 * rasterScale));
    const auto mask = rasterText(rasterScale, maskWidth, maskHeight);
    if (std::ranges::none_of(mask, [](unsigned char value) { return value > 0 && value < 255; }))
      throw std::runtime_error("Text sharpness requires an antialiased glyph mask");
    glBindTexture(GL_TEXTURE_2D, graphics.mask);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, maskWidth, maskHeight, 0, GL_ALPHA, GL_UNSIGNED_BYTE, mask.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Catch drift from using rounded layout dimensions as projection bounds:
    // it grows toward the far edge, and can differ independently on each axis.
    const std::array positions{
        std::array{7, 9}, std::array{(bufferWidth - maskWidth) / 2, (bufferHeight - maskHeight) / 2},
        std::array{bufferWidth - maskWidth - 7, bufferHeight - maskHeight - 9}
    };
    for (const auto& position : positions) {
      glClearColor(0, 0, 0, 0);
      glClear(GL_COLOR_BUFFER_BIT);
      program.drawTinted(
          TextureId(graphics.mask), target.renderWidth(), target.renderHeight(), maskWidth / rasterScale,
          maskHeight / rasterScale, 0, 0, 1, 1, 1, rgba(1, 1, 1, 1),
          Mat3::translation(position[0] / rasterScale, position[1] / rasterScale)
      );
      std::vector<unsigned char> pixels(static_cast<std::size_t>(maskWidth * maskHeight * 4));
      glReadPixels(
          position[0], bufferHeight - position[1] - maskHeight, maskWidth, maskHeight, GL_RGBA, GL_UNSIGNED_BYTE,
          pixels.data()
      );
      int maximumDifference = 0;
      for (int y = 0; y < maskHeight; ++y) {
        for (int x = 0; x < maskWidth; ++x) {
          const int expected = mask[y * maskWidth + x];
          const int actual = pixels[((maskHeight - 1 - y) * maskWidth + x) * 4 + 3];
          maximumDifference = std::max(maximumDifference, std::abs(expected - actual));
        }
      }
      if (maximumDifference > 1) {
        std::cerr
            << "Glyph texels changed by "
            << maximumDifference
            << " at output scale "
            << outputScale
            << ", zoom "
            << zoom
            << ", pixel "
            << position[0]
            << ','
            << position[1]
            << '\n';
        throw std::runtime_error("Pixel-aligned text must retain its rasterized coverage without resampling");
      }
    }
    if (glGetError() != GL_NO_ERROR)
      throw std::runtime_error("Text sharpness rendering leaves a GL error");
  }

  void verifyStaticPhase(Graphics& graphics, GlyphProgram& program) {
    constexpr int width = 768, height = 256;
    glBindTexture(GL_TEXTURE_2D, graphics.output);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, graphics.output, 0);
    glViewport(0, 0, width, height);
    GlyphBackend backend(program);
    CairoTextRenderer renderer;
    renderer.initialize(&backend, &backend.textureManager());
    for (const float scale : {0.67F, 1.0F, 1.25F, 1.33F, 2.0F}) {
      for (const float size : {11.0F, 14.0F}) {
        for (const auto weight : {FontWeight::Normal, FontWeight::Bold}) {
          const auto draw = [&](bool snap, std::optional<float> scroll, float offset = 0) {
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.draw(
                scale, width / scale, height / scale, 0, 0, "Small text: Il1, mW / gj", size, rgba(1, 1, 1, 1),
                Mat3::translation(12.25F, 81.37F + offset), weight, 0, 0, TextAlign::Start, {}, TextEllipsize::End,
                false, snap, nullptr, scroll
            );
            std::vector<unsigned char> pixels(width * height * 4);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            return pixels;
          };
          for (const float physicalOffset : {-1.0F, -0.75F, -0.5F, -0.25F, 0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
            const float offset = physicalOffset / scale;
            const auto reference = draw(true, {}, offset);
            const auto scrollingPane = draw(false, offset, offset);
            if (reference != scrollingPane) {
              int maximumDifference = 0;
              for (std::size_t i = 0; i < reference.size(); ++i)
                maximumDifference = std::max(maximumDifference, std::abs(int(reference[i]) - int(scrollingPane[i])));
              std::cerr
                  << "Glyph coverage changed by "
                  << maximumDifference
                  << " at scale "
                  << scale
                  << ", font size "
                  << size
                  << ", weight "
                  << static_cast<int>(weight)
                  << ", physical scroll "
                  << physicalOffset
                  << '\n';
              throw std::runtime_error("Scrolling text must retain the coverage of the final pixel-aligned reference");
            }
          }
          if (draw(true, {}) == draw(false, {}))
            throw std::runtime_error("Fractional-baseline fixture did not expose legacy unaligned glyph filtering");
        }
      }
    }
    if (glGetError() != GL_NO_ERROR)
      throw std::runtime_error("Scrolled text coverage rendering leaves a GL error");
  }

  void verifyMarkup(Graphics& graphics, GlyphProgram& program) {
    constexpr int width = 768, height = 256;
    glBindTexture(GL_TEXTURE_2D, graphics.output);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, graphics.output, 0);
    glViewport(0, 0, width, height);
    GlyphBackend backend(program);
    CairoTextRenderer renderer;
    renderer.initialize(&backend, &backend.textureManager());
    for (const float scale : {1.0F, 1.25F, 2.0F}) {
      const auto draw = [&](std::string_view markup) {
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.draw(
            scale, width / scale, height / scale, 0, 0, markup, Style::fontSizeBody, rgba(1, 1, 1, 1),
            Mat3::translation(Style::spaceXl, Style::controlHeightLg), FontWeight::Normal, Style::controlHeightLg * 4,
            0, TextAlign::Start, {}, TextEllipsize::End, true
        );
        std::vector<unsigned char> pixels(width * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return pixels;
      };
      const auto plain = draw("Formatting sample");
      for (const auto* attribute :
           {"weight=\"bold\"", "style=\"italic\"", "underline=\"single\"", "strikethrough=\"true\"",
            "font_family=\"monospace\""})
        if (draw(std::string("<span ") + attribute + ">Formatting sample</span>") == plain)
          throw std::runtime_error("Constrained markup must change the actual rendered glyphs for each text style");
      const auto colored = draw("<span foreground=\"#00FF00\" background=\"#FF0000\">Colored code</span>");
      bool foreground = false, background = false;
      for (std::size_t at = 0; at < colored.size(); at += 4) {
        foreground |= colored[at] == 0 && colored[at + 1] > 0 && colored[at + 2] == 0;
        background |= colored[at] > 0 && colored[at + 1] == 0 && colored[at + 2] == 0;
      }
      if (!foreground || !background)
        throw std::runtime_error("Markup foreground and background colors must survive rasterization without emoji");
    }
  }
} // namespace

int main() {
  Graphics graphics;
  if (!graphics.initialize()) {
    std::cerr << "No headless EGL context for text sharpness readback\n";
    return 77;
  }
  try {
    GlyphProgram program;
    program.ensureInitialized();
    for (unsigned numerator : {120U, 150U, 160U, 240U}) {
      for (float zoom : {0.9F, 1.0F, 1.25F, 1.33F, 1.5F}) {
        verify(graphics, program, numerator, zoom, false);
        verify(graphics, program, numerator, zoom, true);
      }
    }
    verifyStaticPhase(graphics, program);
    verifyMarkup(graphics, program);
    program.destroy();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
