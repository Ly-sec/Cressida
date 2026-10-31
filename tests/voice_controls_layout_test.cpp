#include "app/layout_metrics.h"

#include <iostream>
#include <stdexcept>

int main() {
  using namespace app::layout;
  const auto require = [](bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  };
  try {
    for (const float sidebar : {Sidebar::minimumWidth, Sidebar::preferredWidth, Sidebar::maximumWidth})
      for (const float height :
           {Style::profileCardWidth, static_cast<float>(Style::windowMinimumHeight), Style::profileCardWidth * 3}) {
        const float right = Sidebar::railWidth + sidebar;
        const auto account = accountFrame(height, right);
        require(
            account.nameWidth > 0 && account.presenceWidth > Avatar::account,
            "Account identity retains space beside permanent voice controls"
        );
        float previous = account.x + account.presenceWidth;
        for (const auto control : account.voice) {
          require(
              control.x >= previous
                  && control.width > 0
                  && control.height > 0
                  && control.y >= account.y
                  && control.y + control.height <= account.y + account.height
                  && control.x + control.width <= account.settingsX,
              "Account voice targets stay separate from presence, settings and each other"
          );
          previous = control.x + control.width;
        }
        for (const bool input : {false, true})
          for (const bool error : {false, true}) {
            const float width = right + Sidebar::minimumConversation;
            const auto menu = VoiceMenu::frame(account.voice[input ? 1 : 3], width, height, input, error);
            require(
                menu.panel.x >= 0
                    && menu.panel.x + menu.panel.width <= width
                    && menu.panel.y >= Window::titlebarHeight
                    && menu.panel.y + menu.panel.height <= height,
                "Audio cards stay inside short and narrow windows"
            );
            require(menu.contentHeight >= menu.viewport.height, "Clipped audio cards retain a bounded scroll range");
            const Frame content{0, 0, menu.viewport.width, menu.contentHeight};
            for (const auto control : {menu.device, menu.volume, menu.settings})
              require(
                  control.x >= 0
                      && control.x + control.width <= content.width
                      && control.y >= 0
                      && control.y + control.height <= content.height,
                  "Devices, gain and settings remain reachable inside the scrollable card"
              );
            require(
                menu.device.y + menu.device.height <= menu.volumeLabel.y
                    && menu.volume.y + menu.volume.height <= menu.settings.y,
                "Audio card controls never overlap"
            );
            if (input)
              require(
                  menu.mode.y >= menu.device.y + menu.device.height
                      && menu.mode.y + menu.mode.height <= menu.volumeLabel.y,
                  "Input activation mode fits between device and gain controls"
              );
          }
      }
    std::cout << "Account voice controls and audio card bounds passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
