#include "render/text/cairo_text_renderer.h"
#include "render/text/text_position.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <pango/pangocairo.h>
#include <print>
#include <string_view>

namespace {

  bool sameMetrics(const CairoTextRenderer::TextMetrics& lhs, const CairoTextRenderer::TextMetrics& rhs) {
    constexpr float epsilon = 0.0001F;
    return std::abs(lhs.width - rhs.width) < epsilon
        && std::abs(lhs.top - rhs.top) < epsilon
        && std::abs(lhs.bottom - rhs.bottom) < epsilon
        && lhs.lineCount == rhs.lineCount;
  }

  // Wrapping-boundary cache correctness: a cached measurement at one width must
  // not reuse metrics computed for a different width.
  int wrappingBoundaryCase(CairoTextRenderer& renderer) {
    constexpr std::string_view text =
        "Error: Error while parsing table header: expected a comment or whitespace, saw 't'";
    constexpr float fontSize = 14.0F;
    const float naturalWidth = renderer.measure(1.0F, text, fontSize).width;
    constexpr std::array deltas{0.0005F, 0.001F, 0.002F, 0.003F, 0.004F, 0.006F};

    for (const float delta : deltas) {
      renderer.notifyFontConfigChanged();
      const auto above = renderer.measure(1.0F, text, fontSize, FontWeight::Normal, naturalWidth + delta, 3);
      renderer.notifyFontConfigChanged();
      const auto expectedBelow = renderer.measure(1.0F, text, fontSize, FontWeight::Normal, naturalWidth - delta, 3);
      if (above.lineCount == expectedBelow.lineCount) {
        continue;
      }

      renderer.notifyFontConfigChanged();
      [[maybe_unused]] const auto cachedAbove =
          renderer.measure(1.0F, text, fontSize, FontWeight::Normal, naturalWidth + delta, 3);
      const auto cachedBelow = renderer.measure(1.0F, text, fontSize, FontWeight::Normal, naturalWidth - delta, 3);
      if (cachedBelow.lineCount != expectedBelow.lineCount) {
        std::println(
            stderr,
            "cairo_text_renderer_test: FAIL: cached measurement at width {} reused metrics from width {} "
            "(expected {} lines, got {})",
            naturalWidth - delta, naturalWidth + delta, expectedBelow.lineCount, cachedBelow.lineCount
        );
        return 1;
      }
      return 0;
    }

    std::println(stderr, "cairo_text_renderer_test: FAIL: could not establish a wrapping boundary");
    return 1;
  }

  // The metrics cache is keyed by content scale: interleaving 1.0x and 1.25x
  // measurements of the same text must return per-scale-repeatable results, and
  // an intervening call at the other scale must not corrupt the cached result.
  int scaleKeyedCacheCase(CairoTextRenderer& renderer) {
    constexpr std::string_view text = "The quick brown fox";
    constexpr float fontSize = 16.0F;

    const auto oneA = renderer.measure(1.0F, text, fontSize);
    const auto hiA = renderer.measure(1.25F, text, fontSize);
    const auto oneB = renderer.measure(1.0F, text, fontSize); // cache hit at 1.0x
    const auto hiB = renderer.measure(1.25F, text, fontSize); // cache hit at 1.25x

    if (!sameMetrics(oneA, oneB)) {
      std::println(
          stderr,
          "cairo_text_renderer_test: FAIL: 1.0x measurement not repeatable across a 1.25x call "
          "(width {} vs {}, lines {} vs {})",
          oneA.width, oneB.width, oneA.lineCount, oneB.lineCount
      );
      return 1;
    }
    if (!sameMetrics(hiA, hiB)) {
      std::println(
          stderr,
          "cairo_text_renderer_test: FAIL: 1.25x measurement not repeatable across a 1.0x call "
          "(width {} vs {}, lines {} vs {})",
          hiA.width, hiB.width, hiA.lineCount, hiB.lineCount
      );
      return 1;
    }
    return 0;
  }

  // A short symbol run may fall back to Noto Sans Math, whose font-wide
  // clipping box accommodates huge operators. It must not become our line box.
  int fallbackBoundsCase(CairoTextRenderer& renderer) {
    auto* fontMap = pango_cairo_font_map_new();
    auto* context = pango_font_map_create_context(fontMap);
    auto* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    pango_cairo_context_set_font_options(context, options);
    cairo_font_options_destroy(options);
    bool failed = false;
    constexpr std::array texts{
        "Alex",       "⪻𝚂ҟ₳𝘕ↁ✧亗⪼", "⪻𝚂ҟ₳𝘕ↁ✧亗⪼ [KURU]", "𝓢𝓮𝓻𝓮𝓲𝓷",        "𓆩Serein𓆪",
        "Ágatha é q̱", "Z̴a̶l̷g̸o̶",      "العربية עברית",     "日本語 한국어", "First line\n⪻𝚂ҟ₳𝘕ↁ✧亗⪼"
    };
    for (const float scale : {1.0F, 1.25F, 2.0F}) {
      for (auto weight : {FontWeight::Normal, FontWeight::SemiBold}) {
        auto* desc = pango_font_description_new();
        pango_font_description_set_family(desc, "sans-serif");
        pango_font_description_set_weight(desc, static_cast<PangoWeight>(weight));
        pango_font_description_set_absolute_size(desc, 13.0 * scale * PANGO_SCALE);
        for (const auto* text : texts) {
          auto* layout = pango_layout_new(context);
          pango_layout_set_font_description(layout, desc);
          pango_layout_set_text(layout, text, -1);
          PangoRectangle ink, logical;
          pango_layout_get_extents(layout, &ink, &logical);
          const int baseline = pango_layout_get_baseline(layout);
          const float unit = 1.0F / (PANGO_SCALE * scale);
          const float expectedTop = std::min(logical.y, ink.y) * unit - baseline * unit;
          const float expectedBottom =
              std::max(logical.y + logical.height, ink.y + ink.height) * unit - baseline * unit;
          const auto actual = renderer.measure(scale, text, 13, weight);
          if (std::abs(actual.top - expectedTop) > 0.01F
              || std::abs(actual.bottom - expectedBottom) > 0.01F
              || actual.top > actual.inkTop + 0.01F
              || actual.bottom < actual.inkBottom - 0.01F) {
            std::println(
                stderr, "cairo_text_renderer_test: FAIL: fallback line bounds at {}x: expected {}..{}, got {}..{}",
                scale, expectedTop, expectedBottom, actual.top, actual.bottom
            );
            failed = true;
          }
          const auto cached = renderer.measure(scale, text, 13, weight);
          if (!sameMetrics(actual, cached))
            failed = true;
          g_object_unref(layout);
        }
        pango_font_description_free(desc);
      }
    }
    g_object_unref(context);
    g_object_unref(fontMap);
    return failed ? 1 : 0;
  }

} // namespace

int main() {
  for (const float scale : {1.0F, 1.25F, 2.0F}) {
    const auto first = Mat3::translation(10.1F, 20.1F);
    const auto next = Mat3::translation(10.1F, 20.3F);
    const auto movingFirst = textQuadTransform(first, scale, false);
    const auto movingNext = textQuadTransform(next, scale, false);
    const auto resting = textQuadTransform(next, scale, true);
    if (movingFirst.m != first.m
        || movingNext.m != next.m
        || std::abs((movingNext.m[7] - movingFirst.m[7]) - 0.2F) > 0.0001F
        || std::abs(resting.m[7] * scale - std::round(resting.m[7] * scale)) > 0.0001F) {
      std::println(
          stderr, "cairo_text_renderer_test: FAIL: moving text must retain fractional travel at scale {}", scale
      );
      return 1;
    }
  }
  const auto rotated = Mat3::translation(10.2F, 20.3F) * Mat3::rotation(0.2F);
  if (textQuadTransform(rotated, 1, true).m != rotated.m) {
    std::println(stderr, "cairo_text_renderer_test: FAIL: snapping must preserve rotated text transforms");
    return 1;
  }

  CairoTextRenderer renderer;
  renderer.initialize(nullptr, nullptr);

  // A width constraint must retain markup instead of replacing its attributes
  // when wrapping disables automatic hyphens.
  for (const float scale : {1.0F, 1.25F, 2.0F}) {
    constexpr std::string_view word = "Wide bold words";
    constexpr std::string_view markup = "<b>Wide bold words</b>";
    constexpr float width = 400;
    const auto expected = renderer.measure(scale, word, 14, FontWeight::Bold, width);
    const auto actual = renderer.measure(
        scale, markup, 14, FontWeight::Normal, width, 0, TextAlign::Start, {}, TextEllipsize::End, true
    );
    if (!sameMetrics(expected, actual)) {
      std::println(stderr, "Constrained markup lost its bold font attributes");
      return 1;
    }
    std::vector<TextCursorStop> expectedStops, actualStops;
    renderer.measureCursorStopsWrapped(scale, word, 14, {0, word.size()}, width, expectedStops, FontWeight::Bold);
    renderer.measureCursorStopsWrapped(
        scale, markup, 14, {0, word.size()}, width, actualStops, FontWeight::Normal, true
    );
    if (std::abs(expectedStops.back().x - actualStops.back().x) > 0.01F) {
      std::println(stderr, "Markup cursor geometry lost its bold font attributes");
      return 1;
    }
  }

  if (const int rc = fallbackBoundsCase(renderer); rc != 0) {
    return rc;
  }
  if (const int rc = scaleKeyedCacheCase(renderer); rc != 0) {
    return rc;
  }
  return wrappingBoundaryCase(renderer);
}
