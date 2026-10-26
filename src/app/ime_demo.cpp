#include "app/client_application.h"
#include "render/scene/input_area.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    constexpr std::string_view imeDemoDraft = "Compose here: ";
    constexpr std::string_view imeDemoChannel = "101";
    constexpr std::string_view imeDemoPreedit = "as";
    constexpr std::string_view imeDemoCommit = "á";
    constexpr std::string_view imeDemoFinalPreedit = " tiếng Việt";
    constexpr float imeDemoScaleStep = 1.1F;
    constexpr std::size_t imeDemoMessages = 2;

    TextInputEdit composing(std::string_view text) {
      return {
          .preeditText = std::string(text),
          .preeditCursorBegin = static_cast<std::int32_t>(text.size()),
          .preeditCursorEnd = static_cast<std::int32_t>(text.size()),
          .hasPreedit = true
      };
    }
    TextInputEdit committed(std::string_view text) { return {.commitText = std::string(text), .hasCommitText = true}; }
  } // namespace

  void ClientApplication::prepareImeDemo(const std::string& view) {
    m_demoOverlay = view;
    m_demoInteractionStep = 0;
    m_dmMode = false;
    m_dmPage.clear();
    m_showMembers = false;
    m_settingsOpen = false;
    m_preferences.reducedMotion = true;
    if (view.ends_with("light"))
      m_preferences.theme = "light";
    if (view.ends_with("narrow"))
      m_window.setUiScale(1.6F);
    selectChannel(std::string(imeDemoChannel));
    m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
      {"id":"9601","channel_id":"101","author":{"id":"11","username":"Maya"},
       "timestamp":"2026-10-26T10:00:00Z","content":"Compose text with your configured input method."},
      {"id":"9602","channel_id":"101","author":{"id":"12","username":"Noah"},
       "timestamp":"2026-10-26T10:01:00Z","content":"Temporary composition stays separate from the saved draft."}
    ])JSON"));
    m_drafts[m_channelId] = imeDemoDraft;
    m_channelSearchText.clear();
    m_searchHistory.clear();
    closeSearchMenu();
    m_settingsFields["search"].clear();
    m_messageLayouts.clear();
    m_unreadTimeline.snapshot({});
    m_unreadTimeline.positioned();
    m_focusedInput = "composer";
    m_fieldFocused = true;
  }

  void ClientApplication::replayImeStep() {
    const auto require = [](bool condition, const char* reason) {
      if (!condition)
        throw std::runtime_error(reason);
    };
    const auto field = [&](const std::string& id) -> Input& {
      const auto found = m_inputs.find(id);
      require(
          found != m_inputs.end() && found->second && found->second->parent() && found->second->enabled(),
          "The IME replay requires a mounted, enabled native input"
      );
      return *found->second;
    };
    const auto clickField = [&](const std::string& id) -> Input& {
      auto& input = field(id);
      float x = 0, y = 0;
      Node::mapToScene(&input, input.width() / 2, input.height() / 2, x, y);
      y += m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      m_window.released();
      require(m_controlInput.focusedArea() == input.inputArea(), "Native pointer input focuses the intended editor");
      return input;
    };
    const auto clickAction = [&](Action action) {
      const auto found = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == action && hit.width > 0 && hit.height > 0;
      });
      require(found != m_hits.end(), "The IME replay requires a visible application action");
      const auto hit = *found;
      const float x = hit.x + hit.width / 2;
      const float y = hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      m_window.released();
    };
    const auto caretState = [&](Input& input) {
      const auto state = input.textInputState();
      require(
          state.sendSurroundingText
              && !state.hiddenText
              && !state.sensitiveData
              && state.cursor >= 0
              && state.anchor >= 0
              && static_cast<std::size_t>(state.cursor) <= state.surroundingText.size()
              && static_cast<std::size_t>(state.anchor) <= state.surroundingText.size(),
          "Ordinary native inputs expose bounded committed surrounding-text indices"
      );
      require(
          state.cursorRectWidth > 0
              && state.cursorRectHeight > 0
              && state.cursorRectX >= 0
              && state.cursorRectY >= 0
              && state.cursorRectX < m_window.width()
              && state.cursorRectY < m_window.height() + layout::Window::titlebarHeight,
          "The native candidate anchor stays inside the logical surface after layout and scale changes"
      );
    };
    const auto compositionKeys = [&](Input& input) {
      const auto* focused = m_controlInput.focusedArea();
      const auto display = input.displayValue();
      for (const auto symbol : std::array<std::uint32_t, 3>{XKB_KEY_Tab, XKB_KEY_Return, XKB_KEY_Escape})
        m_window.key({.symbol = symbol});
      require(
          input.hasPreedit() && input.displayValue() == display && m_controlInput.focusedArea() == focused,
          "Tab, Enter and Escape during composition preserve the native editor and its preedit"
      );
    };

    const auto step = m_demoInteractionStep++;
    std::clog << "Offline IME frame " << step << '\n';
    require(
        m_demo && m_channelId == imeDemoChannel && m_messages.size() == imeDemoMessages && m_pendingSends.empty(),
        "IME replay remains in its synthetic conversation without outgoing account work"
    );
    switch (step) {
    case 0: {
      auto& input = clickField("composer");
      m_window.key({.symbol = XKB_KEY_End});
      require(input.value() == imeDemoDraft && !input.hasPreedit(), "The composer starts with its committed draft");
      input.textInputApplyEdit(composing(imeDemoPreedit));
      require(
          input.hasPreedit() && m_drafts.at(m_channelId) == imeDemoDraft && input.value() == imeDemoDraft,
          "Starting composition changes the native display without persisting preedit in the draft"
      );
      break;
    }
    case 1: {
      auto& input = field("composer");
      require(
          input.hasPreedit()
              && input.displayValue() == std::string(imeDemoDraft) + std::string(imeDemoPreedit)
              && m_drafts.at(m_channelId) == imeDemoDraft,
          "Native composer binding preserves uncommitted text through a complete scene rebuild"
      );
      caretState(input);
      unsigned submissions = 0;
      input.setOnSubmit([this, &submissions](const std::string&) {
        ++submissions;
        send();
      });
      compositionKeys(input);
      input.setOnSubmit([this](const std::string&) { send(); });
      require(
          submissions == 0 && m_channelPanel.empty() && !settingsVisible() && !m_completionQuery,
          "Composition navigation cannot submit a message or open application navigation"
      );
      input.textInputApplyEdit(composing(imeDemoCommit));
      break;
    }
    case 2: {
      auto& input = field("composer");
      require(
          input.hasPreedit()
              && input.displayValue() == std::string(imeDemoDraft) + std::string(imeDemoCommit)
              && m_drafts.at(m_channelId) == imeDemoDraft,
          "Telex-style composition replacement survives redraw without changing the committed draft"
      );
      input.textInputApplyEdit(committed(imeDemoCommit));
      input.textInputApplyEdit({});
      require(
          !input.hasPreedit() && m_drafts.at(m_channelId) == std::string(imeDemoDraft) + std::string(imeDemoCommit),
          "Committing composition updates the real draft exactly once"
      );
      break;
    }
    case 3: {
      auto& input = field("composer");
      require(input.value() == m_drafts.at(m_channelId), "Committed composition remains synchronized after redraw");
      m_window.key({.symbol = XKB_KEY_z, .control = true});
      require(m_drafts.at(m_channelId) == imeDemoDraft, "Native undo returns to the original committed draft");
      m_window.key({.symbol = XKB_KEY_a, .control = true});
      input.textInputApplyEdit(composing("temporary"));
      require(input.displayValue() == "temporary", "Preedit can temporarily replace the selected draft");
      clickAction(Action::Settings);
      require(
          m_settingsOpen
              && !input.hasPreedit()
              && input.value() == imeDemoDraft
              && m_drafts.at(m_channelId) == imeDemoDraft,
          "Leaving the editor cancels replacement preedit and restores its selected committed text"
      );
      break;
    }
    case 4: {
      auto& input = clickField("settings:search");
      input.textInputApplyEdit(composing(imeDemoPreedit));
      require(m_settingsFields.at("search").empty(), "Settings search does not persist temporary composition");
      break;
    }
    case 5: {
      auto& input = field("settings:search");
      require(
          input.hasPreedit() && input.displayValue() == imeDemoPreedit && m_settingsFields.at("search").empty(),
          "Settings inputs retain their preedit through the ordinary binding path"
      );
      compositionKeys(input);
      require(m_settingsOpen && !m_profileDirty, "Composition keys neither close settings nor save a profile");
      input.textInputApplyEdit(committed(imeDemoCommit));
      require(m_settingsFields.at("search") == imeDemoCommit, "Settings search receives the committed text only");
      break;
    }
    case 6:
      require(field("settings:search").value() == imeDemoCommit, "Committed settings text survives redraw");
      m_window.key({.symbol = XKB_KEY_z, .control = true});
      require(m_settingsFields.at("search").empty(), "Settings search shares the native composition undo path");
      clickAction(Action::CloseSettings);
      break;
    case 7:
      require(!settingsVisible() && m_drafts.at(m_channelId) == imeDemoDraft, "Settings preserve the original draft");
      clickField("composer");
      m_window.key({.symbol = XKB_KEY_f, .control = true});
      require(m_channelPanel == "search", "The ordinary shortcut opens the real conversation search input");
      break;
    case 8: {
      auto& input = clickField("channel-search-panel");
      input.textInputApplyEdit(composing(imeDemoPreedit));
      require(m_channelSearchText.empty(), "Conversation search does not submit or store preedit text");
      break;
    }
    case 9: {
      auto& input = field("channel-search-panel");
      require(input.hasPreedit() && input.displayValue() == imeDemoPreedit, "Search composition survives redraw");
      compositionKeys(input);
      require(
          m_channelPanel == "search" && m_searchHistory.empty() && !m_channelPageRequest,
          "Search composition keys preserve the panel without starting a search"
      );
      input.textInputApplyEdit(committed(imeDemoCommit));
      require(m_channelSearchText == imeDemoCommit, "Conversation search receives one committed value");
      break;
    }
    case 10:
      require(field("channel-search-panel").value() == imeDemoCommit, "Committed search text survives redraw");
      m_window.key({.symbol = XKB_KEY_z, .control = true});
      require(m_channelSearchText.empty(), "Conversation search can undo the composition commit");
      closeSearchMenu();
      clickAction(Action::CloseChannelPanel);
      break;
    case 11: {
      auto& input = clickField("composer");
      m_window.key({.symbol = XKB_KEY_End});
      input.textInputApplyEdit(composing(imeDemoPreedit));
      m_window.setUiScale(m_window.uiScale() * imeDemoScaleStep);
      break;
    }
    case 12: {
      auto& input = field("composer");
      require(
          input.hasPreedit() && input.value() == imeDemoDraft && input.displayValue().ends_with(imeDemoPreedit),
          "UI scaling preserves native composition and its committed draft"
      );
      caretState(input);
      input.textInputApplyEdit(committed(imeDemoCommit));
      input.textInputApplyEdit(composing(imeDemoFinalPreedit));
      break;
    }
    case 13: {
      auto& input = field("composer");
      const auto value = std::string(imeDemoDraft) + std::string(imeDemoCommit);
      require(
          input.hasPreedit()
              && input.value() == value
              && m_drafts.at(m_channelId) == value
              && input.displayValue() == value + std::string(imeDemoFinalPreedit)
              && !settingsVisible()
              && m_channelPanel.empty()
              && m_searchHistory.empty(),
          "Final native composition stays visible while only committed text enters the retained draft"
      );
      caretState(input);
      std::clog << "Offline native IME replay passed\n";
      break;
    }
    default:
      throw std::runtime_error("The native IME replay exceeded its declared frame count");
    }
    m_window.invalidate();
  }
} // namespace app
