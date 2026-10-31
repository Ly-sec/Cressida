#include "app/layout_metrics.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
  constexpr float tolerance = 0.01F;
  bool contains(app::layout::Frame outer, app::layout::Frame inner) {
    return inner.x >= outer.x - tolerance
        && inner.y >= outer.y - tolerance
        && inner.x + inner.width <= outer.x + outer.width + tolerance
        && inner.y + inner.height <= outer.y + outer.height + tolerance;
  }
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
} // namespace
int main() {
  try {
    using namespace app::layout;
    for (const float width : {180.0F, 480.0F, 760.0F, 1120.0F, 1600.0F})
      for (const float height : {160.0F, 300.0F, 520.0F, 1000.0F})
        for (const bool right : {false, true})
          for (const bool bottom : {false, true}) {
            const Frame window{0, Window::titlebarHeight, width, height - Window::titlebarHeight};
            const Frame anchor{
                right ? width - Style::controlHeight : 0,
                bottom ? height - Style::controlHeight : Window::titlebarHeight, Style::controlHeight,
                Style::controlHeight
            };
            const auto frame = Sharing::previewTooltip(width, height, anchor);
            require(contains(window, frame.panel), "Discovery preview cards stay inside the window at every edge");
            require(
                contains(frame.panel, frame.title) && contains(frame.panel, frame.image),
                "The owner title and thumbnail stay inside the preview card"
            );
            require(
                std::abs(frame.image.height * 16 - frame.image.width * 9) < tolerance,
                "Discovery thumbnails keep their declared aspect ratio when space is limited"
            );
            if (!right && anchor.x + anchor.width + Style::spaceSm + frame.panel.width <= width - Style::spaceSm)
              require(
                  frame.panel.x >= anchor.x + anchor.width, "A preview opens beside its source when space is available"
              );
          }
    std::cout << "Discovery preview placement, bounds and image geometry passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
