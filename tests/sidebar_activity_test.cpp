#include "app/layout_metrics.h"
#include "app/sidebar_activity.h"

#include <iostream>
#include <stdexcept>

int main() {
  const auto require = [](bool value, const char* reason) {
    if (!value)
      throw std::runtime_error(reason);
  };
  try {
    app::SidebarActivity activity{.top = 100, .bottom = 300};
    activity.add("above-unread", 40, 30, true, 0, false);
    activity.add("above-mention", 70, 30, true, 2, true);
    activity.add("visible", 100, 30, true, 9, false);
    activity.add("visible-bottom", 270, 30, true, 9, false);
    activity.add("below-unread", 280.5F, 30, true, 0, false);
    activity.add("muted", 320, 30, true, 0, true);
    activity.add("below-mention", 350, 30, true, 100, true);
    require(
        activity.above.target() == "above-mention" && activity.above.mentions == 2,
        "Only offscreen rows count, and mentions take priority over ordinary unread"
    );
    require(
        activity.below.target() == "below-mention" && activity.below.caption() == "99+ NEW MENTIONS",
        "Partially clipped rows count and muted mentions remain available"
    );
    activity.add("closer-above", 90, 30, true, 1, false);
    require(activity.above.target() == "closer-above", "Above navigation selects the nearest mentioned row");
    activity.add("overflow", 400, 30, true, 2'000'000'000, false);
    require(activity.below.mentions == app::SidebarActivityEdge::maximumMentions, "Mention sums are bounded");
    app::SidebarActivity ordinary{.top = 100, .bottom = 300};
    ordinary.add("muted", 350, 30, true, 0, true);
    require(ordinary.below.empty(), "Muted ordinary unread activity is suppressed");
    ordinary.add("first", 350, 30, true, 0, false);
    ordinary.add("last", 390, 30, true, 0, false);
    require(
        ordinary.below.target() == "first" && ordinary.below.caption() == "NEW UNREAD MESSAGES",
        "Below navigation selects the nearest ordinary unread row"
    );
    for (const auto width : {app::layout::Sidebar::minimumWidth, app::layout::Sidebar::maximumWidth}) {
      const auto viewport = app::layout::sidebarChannelViewport(100, 500, width);
      const auto above = app::layout::sidebarActivityFrame(viewport, 1000, true);
      const auto below = app::layout::sidebarActivityFrame(viewport, 1000, false);
      require(
          above.x >= viewport.x
              && above.x + above.width <= viewport.x + viewport.width
              && above.y >= viewport.y
              && below.y + below.height <= viewport.y + viewport.height,
          "Both pills stay within their viewport even when captions exceed sidebar width"
      );
      const auto down = app::layout::sidebarActivityReveal(900, 30, 100, viewport, 0, 1000);
      require(down > 0 && 100 + 900 - down + 30 <= below.y, "Reveal below leaves room for the indicator");
      const auto up = app::layout::sidebarActivityReveal(10, 30, 100, viewport, down, 1000);
      require(up == 0, "Reveal above clamps to the start of the channel list");
    }
    std::cout << "Sidebar activity projection and reveal geometry passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
