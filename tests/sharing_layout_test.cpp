#include "app/layout_metrics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  using app::layout::Frame;
  using Sharing = app::layout::Sharing;
  constexpr float tolerance = .01F;
  std::string scenario;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(scenario + ": " + message);
  }
  bool valid(Frame frame) {
    return std::isfinite(frame.x)
        && std::isfinite(frame.y)
        && std::isfinite(frame.width)
        && std::isfinite(frame.height)
        && frame.width >= 0
        && frame.height >= 0;
  }
  bool fits(Frame outer, Frame inner) {
    return valid(outer)
        && valid(inner)
        && inner.x >= outer.x - tolerance
        && inner.y >= outer.y - tolerance
        && inner.x + inner.width <= outer.x + outer.width + tolerance
        && inner.y + inner.height <= outer.y + outer.height + tolerance;
  }
  bool disjoint(Frame a, Frame b) {
    return a.width == 0
        || a.height == 0
        || b.width == 0
        || b.height == 0
        || a.x + a.width <= b.x + tolerance
        || b.x + b.width <= a.x + tolerance
        || a.y + a.height <= b.y + tolerance
        || b.y + b.height <= a.y + tolerance;
  }
  void previewChecks() {
    for (const float width : {Style::profileCardWidth, Style::embedMaximumWidth, Sharing::preferredWidth})
      for (const float bottom :
           {Style::windowMinimumHeight / 2.0F, static_cast<float>(Style::windowMinimumHeight), Sharing::pickerHeight})
        for (const float horizontal : {-1.0F, 0.0F, .5F, 1.0F, 2.0F})
          for (const float vertical : {-1.0F, 0.0F, .5F, 1.0F, 2.0F}) {
            scenario = "Floating preview bounds after moving or resizing";
            const float left = app::layout::Sidebar::railWidth + app::layout::Sidebar::preferredWidth;
            const auto area = Sharing::selfPreviewArea(left, left + width, bottom);
            const auto preview = Sharing::selfPreview(left, left + width, bottom, horizontal, vertical);
            if (!preview.card.width)
              continue;
            require(fits(area, preview.card), "A moved preview stays inside the conversation at every window size");
            require(
                fits(preview.card, preview.title)
                    && fits(preview.card, preview.close)
                    && fits(preview.card, preview.video)
                    && fits(preview.card, preview.detail),
                "Moving the preview retains all its content inside its frame"
            );
            for (const auto control : preview.controls)
              require(
                  fits(preview.card, control) && control.width >= Style::glyphSizeMedium,
                  "A moved or resized preview retains usable controls"
              );
          }
    scenario = "Single stream controls";
    const Frame area{0, 0, Style::profileCardWidth, Style::profileCardWidth};
    const auto tile = Sharing::stageTile(area, 0, 1, false);
    require(tile.controls[0].width == 0, "A single stream reserves no empty focus control");
    for (std::size_t i = 1; i < tile.controls.size(); ++i)
      require(fits(area, tile.controls[i]), "Single stream actions use the full available row");
  }
  void stageChecks() {
    const std::array widths{
        Style::profileCardWidth, Style::profileCardWidth * 2 - Style::borderWidth, Style::profileCardWidth * 2,
        Style::embedMaximumWidth * 2, Style::embedMaximumWidth * 3
    };
    for (const float left :
         {app::layout::Sidebar::railWidth + app::layout::Sidebar::preferredWidth,
          app::layout::Sidebar::railWidth + app::layout::Sidebar::maximumWidth})
      for (const float width : widths)
        for (const float height :
             {static_cast<float>(Style::windowMinimumHeight), Style::spaceGroup * 16, Style::spaceGroup * 24})
          for (const float reserve : {0.0F, Style::controlHeightLg * 3, Style::profileCardWidth, height * 2}) {
            scenario = "Stage width "
                + std::to_string(width)
                + ", height "
                + std::to_string(height)
                + ", composer reserve "
                + std::to_string(reserve);
            const Frame available{
                left, app::layout::Window::titlebarHeight + app::layout::ChannelHeader::height, width,
                height - app::layout::Window::titlebarHeight - app::layout::ChannelHeader::height
            };
            const auto stage = Sharing::stage(left, left + width, height, reserve);
            require(fits(available, stage.video) && fits(available, stage.chat), "Both panels stay inside the window");
            require(disjoint(stage.video, stage.chat), "Video and chat never cover each other");
            const auto full = available.width * available.height;
            const auto used = stage.video.width * stage.video.height + stage.chat.width * stage.chat.height;
            require(
                std::abs(full - used) <= std::max(1.0F, full) * tolerance / 100,
                "The panels partition the available body without losing usable space"
            );
            require(
                stage.chat.height + tolerance >= std::min(reserve, available.height),
                "A tall composer retains its requested chat space up to the available height"
            );
            if (width < Style::profileCardWidth * 2 && reserve >= available.height)
              require(
                  stage.video.height == 0 && std::abs(stage.chat.height - available.height) < tolerance,
                  "A composer that fills the body collapses the stacked video instead of covering it"
              );
            if (stage.video.height <= 0)
              continue;
            const auto area = Sharing::stageArea(stage.video);
            require(fits(stage.video, area), "Stream tiles stay below their header and inside the stage");
            const auto capacity = Sharing::stagePageSize(stage.video);
            require(
                capacity > 0 && capacity <= Sharing::visibleStreams,
                "Every visible stage has a finite usable page capacity"
            );
            constexpr std::size_t maximumFixtureStreams = 50;
            for (std::size_t total = 1; total <= maximumFixtureStreams; ++total) {
              std::vector<Frame> painted;
              for (std::size_t first = 0; first < total; first += capacity) {
                const auto count = std::min(capacity, total - first);
                painted.clear();
                for (std::size_t i = 0; i < count; ++i) {
                  const auto tile = Sharing::stageTile(area, i, count);
                  require(
                      fits(area, tile.title) && fits(area, tile.video),
                      "Each paged title and preview stays inside the stage"
                  );
                  require(tile.video.height > 0, "Responsive pagination keeps room for actual video");
                  require(disjoint(tile.title, tile.video), "A title never covers its video");
                  require(fits(area, tile.error), "A failed stream's explanation stays inside the stage");
                  require(
                      fits(tile.video, tile.error),
                      "The clipped failure viewport stays inside the preview that it replaces"
                  );
                  for (std::size_t controlIndex = 0; controlIndex < tile.controls.size(); ++controlIndex) {
                    const auto control = tile.controls[controlIndex];
                    require(
                        fits(area, control) && control.width >= Style::glyphSizeMedium,
                        "Every control remains visible and wide enough for its glyph"
                    );
                    require(
                        disjoint(control, tile.title) && disjoint(control, tile.video),
                        "Controls never overlap a title or video"
                    );
                    require(disjoint(control, tile.error), "A failed stream's explanation cannot cover controls");
                    for (std::size_t previous = 0; previous < controlIndex; ++previous)
                      require(disjoint(control, tile.controls[previous]), "Independent stream controls never overlap");
                    for (const auto previous : painted)
                      require(disjoint(control, previous), "Controls from separate tiles never overlap");
                  }
                  for (const auto previous : painted)
                    require(
                        disjoint(tile.title, previous) && disjoint(tile.video, previous),
                        "Adjacent stream content never covers another tile"
                    );
                  painted.insert(painted.end(), tile.controls.begin(), tile.controls.end());
                  painted.push_back(tile.title);
                  painted.push_back(tile.video);
                }
              }
            }
          }
  }
  void menuChecks() {
    for (const float width : {Style::profileCardWidth + Style::spaceSection, Sharing::preferredWidth})
      for (const float height : {static_cast<float>(Style::windowMinimumHeight), Sharing::pickerHeight * 2})
        for (const auto count : {std::size_t{2}, std::size_t{4}, std::size_t{34}})
          for (const bool audio : {false, true})
            for (const bool below : {false, true}) {
              scenario = "Floating sharing menu bounds and scroll reachability";
              const Frame window{
                  0, app::layout::Window::titlebarHeight, width, height - app::layout::Window::titlebarHeight
              };
              const Frame anchor{
                  Style::spaceLg, below ? window.y + Style::spaceLg : height - Style::spaceLg - Style::controlHeight,
                  width - Style::spaceLg * 2, Style::controlHeight
              };
              const auto menu = Sharing::menu(anchor, count, width, height, audio);
              require(
                  fits(window, menu.panel) && fits(menu.panel, menu.viewport),
                  "Menus remain inside the window independently of the picker scroll viewport"
              );
              require(disjoint(anchor, menu.panel), "A menu opens above or below its trigger without covering it");
              const float scroll = std::max(0.0F, menu.contentHeight - menu.viewport.height);
              const auto last = Sharing::menuRow(menu, count - 1, scroll);
              require(
                  last.y + last.height <= menu.viewport.height + tolerance,
                  "The final application remains reachable in a bounded menu"
              );
            }
  }
  void pickerChecks() {
    for (const float width :
         {Style::profileCardWidth + Style::spaceSection, Style::embedMaximumWidth, Sharing::preferredWidth,
          Style::embedMaximumWidth * 3})
      for (const float height :
           {static_cast<float>(Style::windowMinimumHeight), Sharing::pickerHeight + Style::spaceGroup,
            Sharing::pickerHeight * 2})
        for (const bool status : {false, true}) {
          scenario = "Picker width "
              + std::to_string(width)
              + ", height "
              + std::to_string(height)
              + ", status "
              + std::to_string(status);
          const auto modal = Sharing::dialog(width, height), body = Sharing::body(modal);
          const Frame window{
              0, app::layout::Window::titlebarHeight, width, height - app::layout::Window::titlebarHeight
          };
          require(fits(window, modal) && fits(modal, body), "The picker and its scroll viewport fit the window");
          const auto picker = Sharing::picker(body.width, status, body.height);
          const auto base = Sharing::picker(body.width, false);
          if (!status && base.bottom - base.preview.height + Style::spaceGroup * 2 <= body.height)
            require(
                picker.bottom <= body.height + tolerance,
                "The standard settings fit without scrolling when the preview can shrink"
            );
          const Frame content{0, 0, body.width, picker.bottom};
          const std::array fixed{picker.sourceTitle,  picker.change,          picker.preview,   picker.settingsTitle,
                                 picker.settings,     picker.resolutionLabel, picker.rateLabel, picker.contentLabel,
                                 picker.hint,         picker.audioLabel,      picker.audio,     picker.refresh,
                                 picker.audioSettings};
          for (const auto frame : fixed)
            require(fits(content, frame), "All picker content fits its measured scroll extent");
          std::vector<Frame> controls{picker.change, picker.audio, picker.refresh, picker.audioSettings};
          controls.insert(controls.end(), picker.resolutions.begin(), picker.resolutions.end());
          controls.insert(controls.end(), picker.rates.begin(), picker.rates.end());
          controls.insert(controls.end(), picker.content.begin(), picker.content.end());
          for (std::size_t i = 0; i < controls.size(); ++i) {
            require(
                fits(content, controls[i]) && controls[i].width > 0 && controls[i].height > 0,
                "Every picker action has a positive region inside the scroll content"
            );
            for (std::size_t j = 0; j < i; ++j)
              require(disjoint(controls[i], controls[j]), "Independent picker actions never cover each other");
          }
          if (status)
            require(
                fits(content, picker.status) && disjoint(picker.status, picker.settings),
                "Errors extend the content instead of covering settings"
            );
          const Frame footer{body.x, modal.y + modal.height - Sharing::footer, body.width, Sharing::footer};
          require(
              fits(modal, footer) && disjoint(body, footer),
              "The sticky footer stays inside the dialog and outside scrolling content"
          );
          const auto available = footer.height - Style::spaceLg * 2;
          require(available >= Style::controlHeight, "The footer keeps complete Cancel and Go Live controls visible");
          const auto maximumScroll = std::max(0.0F, picker.bottom - body.height);
          require(
              picker.bottom - maximumScroll <= body.height + tolerance,
              "The final content row remains reachable when options exceed the viewport"
          );
        }
  }
} // namespace
int main() {
  try {
    pickerChecks();
    menuChecks();
    previewChecks();
    stageChecks();
    std::cout << "Sharing stage, pagination, composer reserve and picker geometry passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
