#include "app/layout_metrics.h"

#include <stdexcept>

int main() {
  using Metrics = app::layout::SlashCommands;
  using Frame = app::layout::Frame;
  const auto require = [](bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  };
  const std::vector<Frame> sizes{
      {0, 0, Style::spaceGroup * 2, Style::controlHeight},
      {0, 0, Metrics::inputMaximumWidth, Metrics::inputMaximumHeight},
      {0, 0, Metrics::chipMaximumWidth, Style::controlHeight},
      {0, 0, Style::spaceGroup * 2, Style::controlHeight}
  };
  for (const float width : {Style::spaceGroup * 5, Style::spaceGroup * 11, Style::spaceGroup * 17}) {
    const auto layout = Metrics::compose(width, Style::spaceGroup * 14, sizes, false);
    require(layout.items.size() == sizes.size(), "Every argument retains one measured frame");
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
      const auto& item = layout.items[i];
      require(
          item.x >= 0 && item.width > 0 && item.x + item.width <= width - Metrics::composerPadding * 2,
          "Wrapped command fields stay inside the composer width"
      );
      for (std::size_t previous = 0; previous < i; ++previous) {
        const auto& before = layout.items[previous];
        require(
            item.x >= before.x + before.width
                || item.y >= before.y + before.height
                || before.x >= item.x + item.width
                || before.y >= item.y + item.height,
            "Argument labels, fields and chips never overlap"
        );
      }
    }
    const auto failure = Metrics::compose(width, Style::spaceGroup * 14, sizes, true);
    require(
        failure.height == layout.height + Metrics::feedbackHeight, "Inline feedback reserves matching measured space"
    );
  }
  std::vector<Frame> many(25, {0, 0, Metrics::chipMaximumWidth, Style::controlHeight});
  const auto bounded = Metrics::compose(Style::spaceGroup * 8, Style::spaceGroup * 14, many, false);
  require(
      bounded.contentHeight > bounded.viewportHeight && bounded.viewportHeight <= Metrics::maximumComposerBody,
      "Large commands retain all argument frames within a bounded scroll viewport"
  );
}
