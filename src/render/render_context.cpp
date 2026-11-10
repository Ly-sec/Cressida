#include "render/render_context.h"

#include "core/files/resource_paths.h"
#include "core/log.h"
#include "core/scoped_timer.h"
#include "core/ui_phase.h"
#include "render/backend/render_backend.h"
#include "render/core/texture_handle.h"
#include "render/core/texture_manager.h"
#include "render/render_target.h"
#include "render/scene/glyph_node.h"
#include "render/scene/image_node.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "render/scene/spinner_node.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("render");
  constexpr float kSlowRenderOperationDebugMs = 50.0F;
  constexpr float kSlowRenderOperationWarnMs = 1000.0F;
  constexpr float kPaintCullSlack = 16.0F;

} // namespace

namespace {

  float elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  template <typename... Args> void logSlowRenderOperation(float ms, std::format_string<Args...> fmt, Args&&... args) {
    if (ms >= kSlowRenderOperationWarnMs) {
      kLog.warn(fmt, std::forward<Args>(args)...);
    } else if (ms >= kSlowRenderOperationDebugMs) {
      kLog.debug(fmt, std::forward<Args>(args)...);
    }
  }

  std::string_view graphicsResetStatusName(RenderGraphicsResetStatus status) {
    switch (status) {
    case RenderGraphicsResetStatus::NoError:
      return "no-error";
    case RenderGraphicsResetStatus::Guilty:
      return "guilty-context-reset";
    case RenderGraphicsResetStatus::Innocent:
      return "innocent-context-reset";
    case RenderGraphicsResetStatus::Unknown:
      return "unknown-context-reset";
    case RenderGraphicsResetStatus::Purged:
      return "purged-context-reset";
    case RenderGraphicsResetStatus::Other:
      return "other-context-reset";
    }
    return "other-context-reset";
  }

  RenderScissor
  scissorForClip(float sw, float sh, float bw, float bh, float left, float top, float right, float bottom) {
    const float scaleX = sw > 0.0F ? bw / sw : 1.0F;
    const float scaleY = sh > 0.0F ? bh / sh : 1.0F;
    const auto clampX = [bw](std::int32_t value) {
      return std::clamp(value, std::int32_t{0}, static_cast<std::int32_t>(std::ceil(bw)));
    };
    const auto clampY = [bh](std::int32_t value) {
      return std::clamp(value, std::int32_t{0}, static_cast<std::int32_t>(std::ceil(bh)));
    };

    // Round clip edges independently in buffer space. Rounding the size after
    // flooring the origin can drop the final column/row for fractional origins.
    const std::int32_t x0 = clampX(static_cast<std::int32_t>(std::floor(left * scaleX)));
    const std::int32_t x1 = clampX(static_cast<std::int32_t>(std::ceil(right * scaleX)));
    const std::int32_t y0 = clampY(static_cast<std::int32_t>(std::floor((sh - bottom) * scaleY)));
    const std::int32_t y1 = clampY(static_cast<std::int32_t>(std::ceil((sh - top) * scaleY)));
    return RenderScissor{
        .x = x0,
        .y = y0,
        .width = std::max(std::int32_t{0}, x1 - x0),
        .height = std::max(std::int32_t{0}, y1 - y0),
    };
  }

  Mat3 nodeLocalTransform(const Node* node) {
    const float cx = node->transformOriginX();
    const float cy = node->transformOriginY();
    return Mat3::translation(node->x(), node->y())
        * Mat3::translation(cx, cy)
        * Mat3::rotation(node->rotation())
        * Mat3::scale(node->scaleX(), node->scaleY())
        * Mat3::translation(-cx, -cy);
  }

} // namespace

RenderContext::RenderContext() = default;

RenderContext::~RenderContext() { cleanup(); }

void RenderContext::initialize(GlSharedContext& shared) {
  cleanup();
  m_backend = createDefaultRenderBackend();
  m_backend->initialize(shared);

  // Pango handles font fallback via Fontconfig automatically — no explicit chain.
  m_backend->textureManager().probeExtensions();
  m_textRenderer.initialize(m_backend.get(), &m_backend->textureManager());
  m_glyphRenderer.initialize(
      paths::assetPath("fonts/tabler-icons.ttf").string(), m_backend.get(), &m_backend->textureManager()
  );
  m_textRenderer.setFontFamily(m_textFontFamily);
  m_textRenderer.setBaseDirection(m_textBaseDirRtl);
  ++m_textMetricsGeneration;
  m_graphicsResetPending = false;
}

void RenderContext::restoreAfterGraphicsReset(GlSharedContext& shared) {
  if (m_backend == nullptr) {
    throw std::runtime_error("cannot restore an uninitialized render context");
  }
  m_backend->initialize(shared);
  m_backend->textureManager().probeExtensions();
  invalidateGpuResourcesNextFrame();
}

void RenderContext::prepareForGraphicsReset() {
  if (m_backend == nullptr) {
    return;
  }

  m_textRenderer.abandonGlyphTextures();
  m_glyphRenderer.abandonGlyphTextures();
  m_backend->abandonAfterGraphicsReset();
}

bool RenderContext::makeCurrentNoSurface() {
  if (m_backend == nullptr || m_graphicsResetPending) {
    return false;
  }
  return m_backend->makeCurrentNoSurface();
}

bool RenderContext::makeCurrent(RenderTarget& target) {
  if (m_backend == nullptr || m_graphicsResetPending || !m_backend->makeCurrent(target)) {
    return false;
  }
  return true;
}

void RenderContext::setTextFontFamily(std::string family) {
  if (family.empty()) {
    family = "sans-serif";
  }
  if (m_textFontFamily == family) {
    return;
  }
  makeCurrentNoSurface();
  m_textFontFamily = std::move(family);
  m_textRenderer.setFontFamily(m_textFontFamily);
  ++m_textMetricsGeneration;
}

void RenderContext::setTextBaseDirection(bool rtl) {
  if (m_textBaseDirRtl == rtl) {
    return;
  }
  makeCurrentNoSurface();
  m_textBaseDirRtl = rtl;
  m_textRenderer.setBaseDirection(rtl);
  ++m_textMetricsGeneration;
}

void RenderContext::notifyFontConfigChanged() {
  m_textRenderer.notifyFontConfigChanged();
  ++m_textMetricsGeneration;
}

void RenderContext::renderScene(RenderTarget& target, Node* sceneRoot) {
  if (m_backend == nullptr || m_graphicsResetPending) {
    return;
  }
  const auto totalStart = std::chrono::steady_clock::now();
  if (!m_backend->beginFrame(target)) {
    return;
  }
  const float renderScale = target.contentScale();

  if (sceneRoot != nullptr
      && m_gpuResourceGeneration != 0
      && sceneRoot->gpuResourceGeneration() != m_gpuResourceGeneration) {
    sceneRoot->invalidateGpuResources(target.renderer(), m_gpuResourceGeneration);
  }

  if (m_glyphTexturesDirty) {
    m_textRenderer.invalidateGlyphTextures();
    m_glyphRenderer.invalidateGlyphTextures();
    m_glyphTexturesDirty = false;
  }

  const auto drawStart = std::chrono::steady_clock::now();
  {
    UiPhaseScope renderPhase(UiPhase::Render);
    if (sceneRoot != nullptr) {
      const float sw = target.renderWidth();
      const float sh = target.renderHeight();
      const auto bw = static_cast<float>(target.bufferWidth());
      const auto bh = static_cast<float>(target.bufferHeight());
      renderNode(
          renderScale, sceneRoot, Mat3::identity(), 1.0F, sw, sh, bw, bh, 0.0F, 0.0F, sw, sh, false, false, false
      );
    }
  }
  float ms = elapsedSince(drawStart);
  logSlowRenderOperation(
      ms, "scene draw took {:.1F}ms ({}x{} logical, {}x{} buffer)", ms, target.logicalWidth(), target.logicalHeight(),
      target.bufferWidth(), target.bufferHeight()
  );

  m_backend->endFrame(target);
  const RenderGraphicsResetStatus resetStatus = m_backend->graphicsResetStatus();
  ms = elapsedSince(totalStart);
  logSlowRenderOperation(ms, "renderScene took {:.1F}ms total", ms);
  if (resetStatus != RenderGraphicsResetStatus::NoError) {
    handleGraphicsReset(resetStatus);
  }
}

TextMetrics RenderContext::measureTextScaled(
    float scale, std::string_view text, float fontSize, FontWeight fontWeight, float maxWidth, int maxLines,
    TextAlign align, std::string_view fontFamily, TextEllipsize ellipsize, bool useMarkup
) {
  auto m = m_textRenderer.measure(
      scale, text, fontSize, fontWeight, maxWidth, maxLines, align, fontFamily, ellipsize, useMarkup
  );
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.inkTop,
      .inkBottom = m.inkBottom,
      .inkLeft = m.inkLeft,
      .inkRight = m.inkRight,
      .lineCount = m.lineCount
  };
}

TextMetrics RenderContext::measureFontScaled(float scale, float fontSize, FontWeight fontWeight) {
  auto m = m_textRenderer.measureFont(scale, fontSize, fontWeight);
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.inkTop,
      .inkBottom = m.inkBottom,
      .inkLeft = m.inkLeft,
      .inkRight = m.inkRight,
      .capHeight = m.capHeight
  };
}

void RenderContext::measureTextCursorStopsScaled(
    float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
    std::vector<float>& outStops, FontWeight fontWeight, bool useMarkup
) {
  m_textRenderer.measureCursorStops(scale, text, fontSize, byteOffsets, outStops, fontWeight, useMarkup);
}

void RenderContext::measureTextCursorStopsWrappedScaled(
    float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
    std::vector<TextCursorStop>& outStops, FontWeight fontWeight, bool useMarkup
) {
  m_textRenderer.measureCursorStopsWrapped(
      scale, text, fontSize, byteOffsets, maxWidth, outStops, fontWeight, useMarkup
  );
}

TextMetrics RenderContext::measureGlyphScaled(float scale, char32_t codepoint, float fontSize) {
  auto m = m_glyphRenderer.measureGlyph(scale, codepoint, fontSize);
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.top,
      .inkBottom = m.bottom,
      .inkLeft = m.left,
      .inkRight = m.right
  };
}

TextureManager& RenderContext::textureManager() {
  makeCurrentNoSurface();
  return m_backend->textureManager();
}

void RenderContext::invalidateGpuResourcesNextFrame() noexcept {
  ++m_gpuResourceGeneration;
  m_glyphTexturesDirty = true;
}

void RenderContext::handleGraphicsReset(RenderGraphicsResetStatus status) {
  if (m_graphicsResetPending) {
    return;
  }
  kLog.warn("graphics reset detected: {}; scheduling context recovery", graphicsResetStatusName(status));
  m_graphicsResetPending = true;
  if (m_graphicsResetCallback) {
    m_graphicsResetCallback(status);
  }
}

void RenderContext::renderNode(
    float renderScale, const Node* node, const Mat3& parentTransform, float parentOpacity, float sw, float sh, float bw,
    float bh, float clipLeft, float clipTop, float clipRight, float clipBottom, bool hasClip, bool ignoreNodeOpacity,
    bool parentPaintContained, bool snapText, std::optional<float> smoothScrollY
) {
  if (!node->visible()) {
    return;
  }
  snapText = snapText && node->textPixelSnap();
  if (const auto offset = node->textScrollOffset())
    smoothScrollY = smoothScrollY.value_or(0.0F) + *offset * parentTransform.m[4];

  const Mat3 worldTransform = parentTransform * nodeLocalTransform(node);
  const float effectiveOpacity = ignoreNodeOpacity ? parentOpacity : parentOpacity * node->opacity();
  float boundsLeft = 0.0F;
  float boundsTop = 0.0F;
  float boundsRight = 0.0F;
  float boundsBottom = 0.0F;
  Node::transformedBounds(node, worldTransform, boundsLeft, boundsTop, boundsRight, boundsBottom);

  const bool paintContained = parentPaintContained || node->paintContained();
  if (paintContained
      && hasClip
      && node->type() != NodeType::RenderProxy
      && node->width() > 0.0F
      && node->height() > 0.0F
      && (boundsRight + kPaintCullSlack <= clipLeft
          || boundsLeft - kPaintCullSlack >= clipRight
          || boundsBottom + kPaintCullSlack <= clipTop
          || boundsTop - kPaintCullSlack >= clipBottom)) {
    return;
  }

  if (hasClip) {
    m_backend->setScissor(scissorForClip(sw, sh, bw, bh, clipLeft, clipTop, clipRight, clipBottom));
  } else {
    m_backend->disableScissor();
  }

  switch (node->type()) {
  case NodeType::Rect: {
    const auto* rect = static_cast<const RectNode*>(node);
    auto style = rect->style();
    style.fill.a *= effectiveOpacity;
    style.border.a *= effectiveOpacity;
    for (auto& stop : style.gradientStops) {
      stop.color.a *= effectiveOpacity;
    }
    m_backend->drawRect(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::Text: {
    const auto* text = static_cast<const TextNode*>(node);
    if (!text->text().empty()) {
      const auto& font = text->fontFamily();
      if (text->hasShadow()) {
        auto shadowColor = text->shadowColor();
        shadowColor.a *= effectiveOpacity;
        const Mat3 shadowTransform = worldTransform * Mat3::translation(text->shadowOffsetX(), text->shadowOffsetY());
        m_textRenderer.draw(
            renderScale, sw, sh, 0.0F, 0.0F, text->text(), text->fontSize(), shadowColor, shadowTransform,
            text->fontWeight(), text->maxWidth(), text->maxLines(), text->textAlign(), font, text->ellipsize(),
            text->useMarkup(), snapText, nullptr, smoothScrollY
        );
      }
      auto color = text->color();
      color.a *= effectiveOpacity;
      m_textRenderer.draw(
          renderScale, sw, sh, 0.0F, 0.0F, text->text(), text->fontSize(), color, worldTransform, text->fontWeight(),
          text->maxWidth(), text->maxLines(), text->textAlign(), font, text->ellipsize(), text->useMarkup(), snapText,
          text->gradient(), smoothScrollY
      );
    }
    break;
  }
  case NodeType::Image: {
    const auto* img = static_cast<const ImageNode*>(node);
    if (img->textureId() != 0) {
      auto tint = img->tint();
      tint.a *= effectiveOpacity;
      m_backend->drawImage(
          RenderImageDraw{
              .texture = img->textureId(),
              .surfaceWidth = sw,
              .surfaceHeight = sh,
              .width = node->width(),
              .height = node->height(),
              .tint = tint,
              .monochromeTint = img->monochromeTint(),
              .alphaMaskTint = img->alphaMaskTint(),
              .opacity = effectiveOpacity,
              .radius = img->radius(),
              .borderColor = img->borderColor(),
              .borderWidth = img->borderWidth(),
              .fitMode = static_cast<RenderImageFitMode>(img->fitMode()),
              .textureWidth = static_cast<float>(img->textureWidth()),
              .textureHeight = static_cast<float>(img->textureHeight()),
              .transform = worldTransform,
              .scrim = img->scrim(),
              .roundedClip = img->roundedClip(),
          }
      );
    }
    break;
  }
  case NodeType::Glyph: {
    const auto* icon = static_cast<const GlyphNode*>(node);
    if (icon->codepoint() != 0) {
      if (icon->hasShadow()) {
        auto shadowColor = icon->shadowColor();
        shadowColor.a *= effectiveOpacity;
        const Mat3 shadowTransform = worldTransform * Mat3::translation(icon->shadowOffsetX(), icon->shadowOffsetY());
        m_glyphRenderer.drawGlyph(
            renderScale, sw, sh, 0.0F, 0.0F, icon->codepoint(), icon->fontSize(), shadowColor, shadowTransform
        );
      }
      auto color = icon->color();
      color.a *= effectiveOpacity;
      m_glyphRenderer.drawGlyph(
          renderScale, sw, sh, 0.0F, 0.0F, icon->codepoint(), icon->fontSize(), color, worldTransform
      );
    }
    break;
  }
  case NodeType::Spinner: {
    const auto* spinner = static_cast<const SpinnerNode*>(node);
    auto style = spinner->style();
    style.color.a *= effectiveOpacity;
    m_backend->drawSpinner(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::RenderProxy: {
    const auto* proxy = static_cast<const RenderProxyNode*>(node);
    const Node* source = proxy->source();
    bool sourceContainsProxy = false;
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (current == source) {
        sourceContainsProxy = true;
        break;
      }
    }
    if (source != nullptr && !sourceContainsProxy) {
      const Mat3 sourceParent = worldTransform * Mat3::translation(-source->x(), -source->y());
      renderNode(
          renderScale, source, sourceParent, effectiveOpacity, sw, sh, bw, bh, clipLeft, clipTop, clipRight, clipBottom,
          hasClip, true, false, snapText, smoothScrollY
      );
    }
    return;
  }
  case NodeType::Base:
    break;
  }

  // Fast path: children are already in zIndex order (the common case — most
  // callers never touch zIndex, or set it identically across siblings). Skip
  // allocating/sorting a side vector and iterate the child list directly.
  // Only fall back to the sorted copy when there's an actual out-of-order
  // pair, which removes a per-node heap allocation from every rendered frame.
  const auto& children = node->children();
  bool childrenSorted = true;
  for (std::size_t i = 1; i < children.size(); ++i) {
    if (children[i]->zIndex() < children[i - 1]->zIndex()) {
      childrenSorted = false;
      break;
    }
  }

  std::vector<const Node*> orderedChildren;
  if (!childrenSorted) {
    orderedChildren.reserve(children.size());
    for (const auto& child : children) {
      orderedChildren.push_back(child.get());
    }
    std::ranges::stable_sort(orderedChildren, [](const Node* a, const Node* b) { return a->zIndex() < b->zIndex(); });
  }

  float childClipLeft = clipLeft;
  float childClipTop = clipTop;
  float childClipRight = clipRight;
  float childClipBottom = clipBottom;
  bool childHasClip = hasClip;

  if (node->clipChildren()) {
    childClipLeft = hasClip ? std::max(childClipLeft, boundsLeft) : boundsLeft;
    childClipTop = hasClip ? std::max(childClipTop, boundsTop) : boundsTop;
    childClipRight = hasClip ? std::min(childClipRight, boundsRight) : boundsRight;
    childClipBottom = hasClip ? std::min(childClipBottom, boundsBottom) : boundsBottom;
    childHasClip = true;
  }

  if (childHasClip && (childClipRight <= childClipLeft || childClipBottom <= childClipTop)) {
    return;
  }

  if (childrenSorted) {
    for (const auto& child : children) {
      renderNode(
          renderScale, child.get(), worldTransform, effectiveOpacity, sw, sh, bw, bh, childClipLeft, childClipTop,
          childClipRight, childClipBottom, childHasClip, false, paintContained, snapText, smoothScrollY
      );
    }
  } else {
    for (const auto* child : orderedChildren) {
      renderNode(
          renderScale, child, worldTransform, effectiveOpacity, sw, sh, bw, bh, childClipLeft, childClipTop,
          childClipRight, childClipBottom, childHasClip, false, paintContained, snapText, smoothScrollY
      );
    }
  }
}

void RenderContext::cleanup() {
  if (m_backend != nullptr) {
    // Need a current context to destroy GL resources, but we may not have a surface.
    m_backend->makeCurrentNoSurface();
  }

  // Text renderers first — they destroy GL textures and need a current context.
  m_textRenderer.cleanup();
  m_glyphRenderer.cleanup();

  if (m_backend != nullptr) {
    m_backend->cleanup();
    m_backend.reset();
  }
}
