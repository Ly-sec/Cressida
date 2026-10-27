#include "app/layout_metrics.h"

#include <iostream>
#include <stdexcept>

int main() {
  const auto require = [](bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  };
  try {
    for (const float width : {320.0F, 480.0F, 760.0F, 1280.0F})
      for (const float height : {400.0F, 520.0F, 800.0F}) {
        const auto frame = app::layout::signInFrame(width, height, false, false, 0);
        require(
            frame.viewport.x >= 0
                && frame.viewport.x + frame.viewport.width <= width
                && frame.viewport.y >= app::layout::Window::titlebarHeight
                && frame.viewport.y + frame.viewport.height <= height,
            "The sign-in viewport stays inside the window at small sizes"
        );
        require(
            frame.qr.x >= 0
                && frame.qr.x + frame.qr.width <= frame.card.width
                && frame.details.y >= frame.qr.y + frame.qr.height,
            "The centered QR layout keeps instructions below the complete code"
        );
        require(
            frame.browser.x + frame.browser.width <= frame.card.width
                && frame.footer.y + frame.footer.height <= frame.card.height,
            "Browser and secondary controls fit within the measured surface"
        );
        require(
            frame.maximumScroll == std::max(0.0F, frame.contentHeight - frame.viewport.height),
            "Short windows retain one bounded scroll range for the full form"
        );
        const auto failure = app::layout::signInFrame(width, height, false, false, 200);
        require(
            failure.status.y + failure.status.height < failure.footer.y && failure.maximumScroll >= frame.maximumScroll,
            "Long errors move controls together without overlap or inaccessible overflow"
        );
        const auto token = app::layout::signInFrame(width, height, true, false, 0);
        require(
            token.token.width > 0 && token.footer.y >= token.token.y + token.token.height,
            "Advanced token entry has its own complete measured form"
        );
        const auto splash = app::layout::startupFrame(width, height);
        require(
            splash.logo.x >= 0
                && splash.logo.x + splash.logo.width <= width
                && splash.logo.y >= 0
                && splash.logo.y + splash.logo.height <= height
                && splash.logo.x + splash.logo.width / 2 == width / 2
                && splash.logo.y + splash.logo.height / 2 == height / 2,
            "The startup logo is centered on both axes and fits the viewport"
        );
      }
    std::cout << "Sign-in and startup layout checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
