#include "app/client_application.h"
#include "render/scene/rect_node.h"

#include <iostream>
#include <linux/input-event-codes.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::demoFolderAnimationStep() {
    using R = layout::ServerRail;
    const auto require = [](bool ok, const char* reason) {
      if (!ok)
        throw std::runtime_error(reason);
    };
    const auto hit = [&](Action action, const std::string& id) -> Hit {
      const auto at =
          std::ranges::find_if(m_hits, [&](const auto& item) { return item.action == action && item.id == id; });
      require(at != m_hits.end(), "Animated rail retains the visible folder/server hit");
      return *at;
    };
    const auto click = [&] {
      const auto header = hit(Action::Folder, "10");
      const auto x = header.x + header.width / 2;
      const auto y = header.y + header.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      m_window.released();
    };
    const auto expanded = [&] {
      const auto header = hit(Action::Folder, "10");
      require(header.height == R::folderHeaderHeight, "Open folders have compact glyph headers");
      const auto first = hit(Action::Guild, "1"), second = hit(Action::Guild, "2");
      require(
          first.y == header.y + header.height && second.y - first.y == R::folderChildStep,
          "Expanded children share compact measured positions"
      );
      const auto frame = R::folderFrame(2, 1);
      int backgrounds = 0;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Rect && node.width() == R::folderSize && node.height() == frame.height)
          ++backgrounds;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(backgrounds == 1, "One continuous surface encloses the open folder and its children");
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      expanded();
      click();
      require(
          !m_openFolders.contains("10") && m_folderMotion.active(),
          "Native clicks begin closing without losing persisted state"
      );
    } else if (step == 1) {
      require(m_folderMotion.active(), "The compositor-paced close keeps its children while sliding");
    } else if (step == 2) {
      const auto header = hit(Action::Folder, "10"), child = hit(Action::Guild, "2"),
                 following = hit(Action::Guild, "3");
      require(
          header.height > R::folderHeaderHeight
              && header.height < R::folderSize
              && child.height > 0
              && child.height < layout::Avatar::rail
              && following.y < R::top + R::folderFrame(2, 1).height + R::folderGap,
          "Intermediate frames slide following servers and clip partially hidden child hits"
      );
      click();
    } else if (step == 3) {
      require(
          m_openFolders.contains("10") && m_folderMotion.active(),
          "A click during closing reverses the visible transition"
      );
    } else if (step == 4) {
      m_preferences.reducedMotion = true;
      click();
    } else if (step == 5) {
      require(
          !m_folderMotion.active() && hit(Action::Folder, "10").height == R::folderSize,
          "Reduced motion collapses directly to the miniature header"
      );
      require(
          std::ranges::none_of(
              m_hits,
              [](const auto& item) { return item.action == Action::Guild && (item.id == "1" || item.id == "2"); }
          ),
          "Collapsed children have no selectable hidden targets"
      );
      click();
    } else if (step == 6) {
      expanded();
      m_preferences.reducedMotion = false;
      m_openFolders.clear();
      saveExpandedFolders();
      require(m_folderMotion.active(), "Close All Folders uses the same animated targets");
    } else if (step == 7) {
      std::erase_if(m_folders, [](const auto& folder) { return folder.id == "10"; });
    } else if (step == 8) {
      require(
          m_folderMotion.size() == 0 && !m_folderMotion.active(), "Removed folders release in-flight animation state"
      );
      require(
          std::ranges::none_of(m_hits, [](const auto& item) { return item.action == Action::Folder; }),
          "Removed folder headers cannot retain raced click targets"
      );
      std::clog << "Offline folder surfaces, compact headers, sliding positions, clipped hits, reversal, reduced "
                   "motion and removal passed\n";
    }
    m_window.invalidate();
  }
  void ClientApplication::demoRailDragStep() {
    using R = layout::ServerRail;
    const auto require = [](bool ok, const char* reason) {
      if (!ok)
        throw std::runtime_error(reason);
    };
    const auto hit = [&](Action action, const std::string& id) -> Hit {
      const auto at = std::ranges::find_if(m_hits, [&](const auto& h) { return h.action == action && h.id == id; });
      require(at != m_hits.end(), "Drag replay has a rendered source and destination");
      return *at;
    };
    const auto motion = [&](float x, float y) {
      m_window.motion(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight);
    };
    const auto drag = [&](Action action, const std::string& source, Action destination, const std::string& target,
                          float fraction) {
      m_demoRailMove.reset();
      const auto from = hit(action, source), to = hit(destination, target);
      motion(from.x + from.width / 2, from.y + from.height / 2);
      m_window.click(m_pointerX, m_pointerY + m_window.titlebarHeight() - layout::Window::titlebarHeight, BTN_LEFT);
      require(m_railDrag && !m_railDrag->active, "Press arms a drag without navigating or toggling");
      motion(to.x + to.width / 2, to.y + to.height * fraction);
      require(
          m_railDrag && m_railDrag->active && m_railDrag->drop, "Movement past Style's threshold picks a drop target"
      );
    };
    const auto reset = [&] {
      finishRailDrag(true);
      m_folders = {{"10", "Hangouts", 0x8F80D4, {"1", "2"}}, {{}, {}, 0, {"3"}}};
      m_guildLayout.folders = m_folders;
      m_openFolders = {"10"};
      m_folderMotion.clear();
    };
    const auto acknowledge = [&](bool anonymous = false, bool newlyJoined = false) {
      require(m_demoRailMove.has_value(), "Release records one typed organization operation");
      const nlohmann::json snapshot{
          {"settings",
           newlyJoined
               ? (anonymous ? "CgIYKnIYCgoKCAEAAAAAAAAACgoKCAIAAAAAAAAA" : "CgIYKnIYChYKEAEAAAAAAAAAAgAAAAAAAAASAggK")
               : (anonymous ? "CgIYKnIkCgoKCAEAAAAAAAAACgoKCAIAAAAAAAAACgoKCAMAAAAAAAAA"
                            : "CgIYKnIkChYKEAEAAAAAAAAAAgAAAAAAAAASAggKCgoKCAMAAAAAAAAA")}
      };
      const std::vector<std::string> joined = newlyJoined ? std::vector<std::string>{"3"} : std::vector<std::string>{};
      const auto patch = api::guildRailPatch(snapshot, *m_demoRailMove, joined);
      api::Event event;
      event.interaction =
          api::Interaction{.kind = api::InteractionKind::ReorderGuilds, .guild = "1", .railMove = m_demoRailMove};
      event.interaction->stateRevision = m_guildLayoutRevision;
      event.layout = api::parseGuildLayout(patch);
      serverMenuInteractionResult(event);
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      drag(Action::Guild, "3", Action::Folder, "10", 0.5F);
      require(m_folders.front().guildIds.size() == 2, "A hover cannot mutate the account organization");
      m_openFolders.erase("10");
      saveExpandedFolders();
      m_folderMotion.finish();
      tickRailDrag();
      m_railDrag->hover = FolderMotion::Clock::now() - std::chrono::milliseconds(Style::animSlow);
      tickRailDrag();
      require(m_openFolders.contains("10"), "Hovering a closed destination opens it through the owning-loop clock");
    } else if (step == 1) {
      require(m_railDrag && m_railDrag->active, "Drag capture survives a scene rebuild");
      m_window.released();
      require(m_demoRailMove->placement == api::RailPlacement::Into, "Middle drops group servers");
      acknowledge(false, true);
      require(
          m_folders.front().guildIds == std::vector<std::string>{"1", "2", "3"}, "Acknowledged drops update membership"
      );
      reset();
    } else if (step == 2) {
      drag(Action::Folder, "10", Action::Guild, "3", 1 - R::dropEdgeFraction / 2);
      m_window.released();
      acknowledge(false, true);
    } else if (step == 3) {
      require(
          m_folders.back().id == "10" && hit(Action::Guild, "3").y < hit(Action::Folder, "10").y,
          "A folder drag moves the complete block and its rendered hits"
      );
      drag(Action::Folder, "10", Action::Guild, "3", R::dropEdgeFraction / 2);
      onKey({.symbol = XKB_KEY_Escape});
      m_window.released();
      require(!m_railDrag && !m_demoRailMove, "Escape cancels without submitting a drop");
      reset();
    } else if (step == 4) {
      drag(Action::Guild, "2", Action::Guild, "1", R::dropEdgeFraction / 2);
    } else if (step == 5) {
      m_window.released();
      acknowledge();
      require(m_folders.front().guildIds == std::vector<std::string>{"2", "1"}, "Edge drops reorder within a folder");
      reset();
    } else if (step == 6) {
      drag(Action::Guild, "1", Action::Guild, "3", 1 - R::dropEdgeFraction / 2);
      m_window.released();
      acknowledge(false, true);
      require(
          m_folders.back().id.empty() && m_folders.back().guildIds == std::vector<std::string>{"1"},
          "Dragging out creates a correctly positioned standalone server"
      );
    } else if (step == 7) {
      m_folders = {{{}, {}, 0, {"1"}}, {{}, {}, 0, {"2"}}, {{}, {}, 0, {"3"}}};
      m_guildLayout.folders = m_folders;
      m_folderMotion.clear();
    } else if (step == 8) {
      drag(Action::Guild, "3", Action::Guild, "1", 0.5F);
      m_window.released();
      acknowledge(true, true);
      require(
          !m_folders.front().id.empty() && m_folders.front().guildIds == std::vector<std::string>{"1", "3"},
          "A central server drop creates a folder through the same versioned writer"
      );
      m_openFolders.insert(m_folders.front().id);
      m_preferences.reducedMotion = true;
    } else if (step == 9) {
      drag(Action::Guild, "2", Action::Folder, m_folders.front().id, 0.5F);
      windowFocusChanged(false);
      m_window.released();
      require(!m_demoRailMove && !m_railDrag, "Focus loss cancels the drag without a request");
      drag(Action::Guild, "2", Action::Folder, m_folders.front().id, 0.5F);
      ++m_guildLayoutRevision;
      m_window.released();
      require(!m_demoRailMove, "Organization changes invalidate an in-flight drag");
      std::clog
          << "Offline native server/folder dragging, grouping, extraction, order, capture and cancellation passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
