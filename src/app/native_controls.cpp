#include "app/client_application.h"
#include "app/font_catalog.h"
#include "app/theme.h"
#include "core/input/key_modifiers.h"
#include "core/input/keybind_matcher.h"
#include "render/scene/input_area.h"
#include "render/scene/text_node.h"
#include "ui/controls/glyph.h"
#include "ui/controls/scrollbar.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    class WindowClipboard final : public TextClipboard {
      Window& m_window;

    public:
      explicit WindowClipboard(Window& window) : m_window(window) {}
      std::optional<std::string> clipboardText() override { return {}; }
      void setClipboardText(std::string text) override { m_window.copy(std::move(text)); }
    };
  } // namespace
  void ClientApplication::glyph(Node& parent, float x, float y, float size, const std::string& name, Color color) {
    auto icon = std::make_unique<Glyph>();
    icon->setGlyph(name);
    icon->setGlyphSize(size);
    icon->setColor(color);
    icon->setPosition(x, y);
    icon->setFrameSize(size, size);
    parent.addChild(std::move(icon));
  }
  void ClientApplication::iconButton(
      Node& parent, Hit hit, const std::string& icon, const std::string& tooltip, bool selected
  ) {
    float ox = 0, oy = 0;
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      ox += node->x();
      oy += node->y();
    }
    const bool over = hovered(hit.x + ox, hit.y + oy, hit.width, hit.height);
    if (over || selected)
      rectangle(parent, hit.x, hit.y, hit.width, hit.height, over ? theme::raised : theme::panel, Style::hoverRadius);
    const float size = std::min(20.0F, hit.height - 8);
    glyph(
        parent, hit.x + (hit.width - size) / 2, hit.y + (hit.height - size) / 2, size, icon,
        over || selected ? theme::foreground : theme::muted
    );
    hit.x += ox;
    hit.y += oy;
    m_hits.push_back(hit);
    if (over && !tooltip.empty()) {
      m_tooltip = tooltip;
      m_tooltipX = hit.x + hit.width / 2;
      m_tooltipY = hit.y;
    }
  }
  void ClientApplication::drawTooltip(Node& root, float w, float h) {
    if (m_tooltip.empty()
        || userAccountDialogOpen()
        || m_contextOpen
        || (!m_dialogKind.empty() && m_dialogKind != "profile"))
      return;
    auto& renderer = m_window.target().renderer();
    const auto natural = renderer.measureText(
        m_tooltip, Style::fontSizeDescription, FontWeight::Normal, 0, 0, TextAlign::Start, {}, TextEllipsize::End
    );
    const float width = std::min(std::min(480.0F, w - Style::spaceLg), std::ceil(natural.width) + 20);
    const auto metrics = renderer.measureText(
        m_tooltip, Style::fontSizeDescription, FontWeight::Normal, width - 20, 0, TextAlign::Start, {},
        TextEllipsize::End
    );
    const float height = std::ceil(metrics.bottom - metrics.top) + 16;
    const float x = std::clamp(m_tooltipX - width / 2, 8.0F, std::max(8.0F, w - width - 8));
    const float preferredY =
        m_tooltipY - height - 8 >= 40 ? m_tooltipY - height - 8 : m_tooltipY + (Style::spaceSection - Style::spaceXxs);
    const float y = std::clamp(preferredY, 40.0F, std::max(40.0F, h - height - 8));
    rectangle(root, x, y, width, height, theme::raised, Style::radiusMd);
    label(
        root, x + Style::spaceDense, y + Style::spaceSm, width - Style::spaceRelaxed, m_tooltip,
        Style::fontSizeDescription, theme::foreground, FontWeight::Normal, 0
    );
  }
  ClientApplication::~ClientApplication() {
    m_window.setTextInputDispatcher(nullptr);
    clearOutgoing();
    m_inboxReminderTimer.stop();
    m_readAcknowledgementTimer.stop();
    m_trayStartupTimer.stop();
    m_trayDemoTimer.stop();
    closeSelectDropdown();
    m_typingTimer.stop();
    m_filePicker.cancel();
    closeVideo();
    stopServerSound();
    m_animationTimer.stop();
    m_gifSearchTimer.stop();
    m_reactorHoverTimer.stop();
    // Destroy tracked input areas while their dispatcher and callback state live.
    m_buildingScene = true;
    m_controlInput.setSceneRoot(nullptr);
    m_scene.reset();
    m_detachedInputs.clear();
    m_inputs.clear();
    m_widgets.clear();
    Input::setTextClipboard(nullptr);
  }
  void ClientApplication::beginControls() {
    const auto generation = m_window.target().renderer().textMetricsGeneration();
    if (m_controlFontGeneration != generation) {
      m_controlFontGeneration = generation;
      for (const auto& [id, input] : m_inputs)
        input->setFontSize(id == "message-edit" ? m_preferences.messageSize : Style::fontSizeBody);
    }
    m_buildingScene = true;
    if (!m_controlClipboard) {
      m_controlClipboard = std::make_unique<WindowClipboard>(m_window);
      Input::setTextClipboard(m_controlClipboard.get());
      m_controlInput.setFocusChangeCallback([this](InputArea*, InputArea* area) {
        m_focusedInput.clear();
        m_fieldFocused = false;
        m_settingsField.clear();
        for (const auto& [id, input] : m_inputs)
          if (input->inputArea() == area) {
            m_focusedInput = id;
            m_fieldFocused = id == "composer" || id == "token";
            if (id.starts_with("settings:"))
              m_settingsField = id.substr(9);
            break;
          }
      });
      Input::setValidateKeyMatcher([](std::uint32_t key, std::uint32_t) {
        return key == XKB_KEY_Return || key == XKB_KEY_KP_Enter;
      });
      KeybindMatcher::setMatcher(KeybindAction::Validate, [](std::uint32_t key, std::uint32_t modifiers) {
        return modifiers == 0 && (key == XKB_KEY_Return || key == XKB_KEY_KP_Enter || key == XKB_KEY_space);
      });
    }
    for (const auto& [id, input] : m_inputs)
      if (input->parent() && !id.starts_with("server-settings:picker:") && !id.starts_with("server-menu:folder-color"))
        m_detachedInputs[id] = input->parent()->removeChild(input);
    for (const auto& [id, control] : m_widgets)
      if (control->parent())
        m_detachedInputs[id] = control->parent()->removeChild(control);
    using namespace theme;
    setPalette(
        {accent, ink, accent, ink, accent, ink, theme::destructive, rgbHex(0xFFFFFF), background, foreground, panel,
         muted, raised, rgbHex(0x000000), raised, foreground}
    );
  }
  void ClientApplication::focusControl(const std::string& id) {
    auto found = m_inputs.find(id);
    bool mounted = false;
    if (found != m_inputs.end())
      for (const auto* node = static_cast<Node*>(found->second); node; node = node->parent())
        mounted |= node == m_scene.get();
    if (mounted) {
      m_focusedInput = id;
      m_controlInput.setFocus(found->second->inputArea());
    }
  }
  void ClientApplication::windowFocusChanged(bool focused) {
    if (!focused) {
      releasePushToTalk();
      finishRailDrag(true);
    }
    if (focused)
      focusChatComposer();
  }
  void ClientApplication::focusChatComposer() {
    if (m_railDrag
        || !m_connected
        || m_channelId.empty()
        || settingsVisible()
        || !m_dialogKind.empty()
        || m_contextOpen
        || m_messageEdit
        || m_inboxOpen
        || m_selectRequest
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || !m_viewImage.empty()
        || m_videoExpanded
        || !m_channelPanel.empty()
        || forumView()
        || (m_dmMode && !m_dmPage.empty()))
      return;
    const auto input = m_inputs.find("composer");
    if (input == m_inputs.end() || !input->second->enabled())
      return;
    focusControl("composer");
    if (m_controlInput.focusedArea() == input->second->inputArea())
      m_fieldFocused = true;
    m_window.invalidate();
  }
  void ClientApplication::demoComposerReplyStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto click = [&](Action action) {
      const auto found = std::ranges::find(m_hits, action, &Hit::action);
      require(found != m_hits.end(), "Reply replay requires the actual visible control");
      const float x = found->x + found->width / 2,
                  y = found->y + found->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 1);
      m_window.released();
    };
    const auto reply = [&] {
      m_contextChannel = m_channelId;
      m_contextMessage = m_messages.front().id;
      contextAction(Action::Reply);
    };
    const auto focusedComposer = [&] {
      require(
          m_focusedInput == "composer" && m_controlInput.focusedArea() == m_inputs.at("composer")->inputArea(),
          "Reply cancellation and window focus select the retained native composer"
      );
    };
    const auto geometry = [&] {
      const auto toggle = std::ranges::find(m_hits, Action::ToggleReplyMention, &Hit::action);
      const auto close = std::ranges::find(m_hits, Action::CancelReply, &Hit::action);
      require(toggle != m_hits.end() && close != m_hits.end(), "Both reply controls are painted");
      require(
          toggle->height == layout::ComposerReply::controlSize
              && close->height == toggle->height
              && close->width == close->height
              && close->y == toggle->y
              && close->x - toggle->x - toggle->width == layout::ComposerReply::gap
              && close->y == m_composerTop + layout::ComposerReply::padding,
          "Reply controls have equal heights, one center line and a compact shared gap"
      );
      const auto panel = std::ranges::find_if(m_floatingFrames, [&](const auto& frame) {
        return frame.x == chatLeft() + layout::Floating::inset && frame.y == m_composerTop;
      });
      require(
          panel != m_floatingFrames.end()
              && close->x + close->width == panel->x + panel->width - layout::ComposerReply::inset
              && toggle->x >= panel->x + layout::ComposerReply::inset,
          "Reply actions keep their right padding inside the composer, including narrow layouts"
      );
      int captions = 0;
      for (const auto& node : m_scene->children()) {
        if (node->type() != NodeType::Text)
          continue;
        const auto& text = static_cast<const TextNode&>(*node);
        if (!text.text().starts_with("Replying to ") && text.text() != "@ ON" && text.text() != "@ OFF")
          continue;
        const auto metrics = m_window.target().renderer().measureText(text.text(), layout::ComposerReply::captionSize);
        require(
            text.fontSize() == layout::ComposerReply::captionSize
                && std::abs(text.y() + (metrics.top + metrics.bottom) / 2 - toggle->y - toggle->height / 2)
                    < Style::borderWidth,
            "Reply names and mention captions align by actual glyph bounds"
        );
        ++captions;
      }
      require(captions == 2, "The reply row contains its name and compact mention caption");
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      geometry();
      click(Action::ToggleReplyMention);
      require(m_replies.at(m_channelId).mention, "The aligned mention control remains functional");
    } else if (step == 1) {
      geometry();
      onKey({XKB_KEY_Escape});
      require(!m_replies.contains(m_channelId), "Escape uses the same cancel action as the close glyph");
      focusedComposer();
      reply();
    } else if (step == 2) {
      geometry();
      click(Action::CancelReply);
      require(!m_replies.contains(m_channelId), "The actual close glyph cancels the reply");
      focusedComposer();
      m_controlInput.setFocus(nullptr);
      m_window.focusChanged(false);
      require(!m_controlInput.focusedArea(), "Losing window focus never selects another input");
      m_window.focusChanged(true);
      focusedComposer();
      onKey({XKB_KEY_a, "a"});
      require(m_drafts.at(m_channelId) == "Reply drafta", "Typing works immediately after window activation");
      onKey({XKB_KEY_z, {}, true});
      require(m_drafts.at(m_channelId) == "Reply draft", "Activation preserves native composer undo");
      m_controlInput.setFocus(nullptr);
      for (const std::uint32_t symbol :
           {XKB_KEY_Control_L, XKB_KEY_Shift_L, XKB_KEY_Alt_L, XKB_KEY_Super_L, XKB_KEY_Caps_Lock,
            XKB_KEY_ISO_Level3_Shift}) {
        onKey({symbol});
        require(!m_controlInput.focusedArea(), "Modifier presses do not steal focus before their chord arrives");
      }
      onKey({XKB_KEY_x, "x", false, false, true});
      onKey({XKB_KEY_x, "x", false, false, false, true});
      require(!m_controlInput.focusedArea(), "Alt and Super chords do not focus the composer");
      onKey({XKB_KEY_c, {}, true});
      require(!m_controlInput.focusedArea(), "Copying chat text does not focus the composer");
      onKey({XKB_KEY_Escape});
      require(!m_controlInput.focusedArea(), "Escape without a reply does not focus the composer");
      onKey({XKB_KEY_exclam, "!", false, true});
      focusedComposer();
      require(m_drafts.at(m_channelId) == "Reply draft!", "Typing focuses the composer and retains the first key");
      onKey({XKB_KEY_z, {}, true});
      require(m_drafts.at(m_channelId) == "Reply draft", "Typing to focus preserves native undo");
      m_controlInput.setFocus(nullptr);
      onKey({XKB_KEY_BackSpace});
      focusedComposer();
      require(m_drafts.at(m_channelId) == "Reply draf", "Editing keys focus the composer and edit the draft");
      onKey({XKB_KEY_z, {}, true});
      m_controlInput.setFocus(nullptr);
      onKey({XKB_KEY_eacute, "é"});
      focusedComposer();
      require(m_drafts.at(m_channelId) == "Reply drafté", "Typing to focus preserves Unicode input");
      onKey({XKB_KEY_z, {}, true});
    } else if (step == 3) {
      openSettings();
    } else if (step == 4) {
      focusControl("settings:search");
      const auto* area = m_controlInput.focusedArea();
      m_window.focusChanged(true);
      require(area && m_controlInput.focusedArea() == area, "Activation preserves an active settings input");
      onKey({XKB_KEY_x, "x"});
      require(
          m_controlInput.focusedArea() == area && m_drafts.at(m_channelId) == "Reply draft",
          "Typing in settings preserves the active field and chat draft"
      );
      m_controlInput.setFocus(nullptr);
      onKey({XKB_KEY_x, "x"});
      require(
          !m_controlInput.focusedArea() && m_drafts.at(m_channelId) == "Reply draft",
          "Typing over unfocused settings never selects the hidden composer"
      );
      activate({.action = Action::CloseSettings});
    } else if (step == 5) {
      reply();
      activate({.action = Action::ToggleEmoji});
    } else if (step == 6) {
      require(m_emojiMenu && m_replies.contains(m_channelId), "The picker covers an existing reply");
      m_controlInput.setFocus(nullptr);
      onKey({XKB_KEY_x, "x"});
      require(
          !m_controlInput.focusedArea() && m_drafts.at(m_channelId) == "Reply draft",
          "Typing over an unfocused picker never selects the covered composer"
      );
      m_window.focusChanged(true);
      onKey({XKB_KEY_Escape});
      require(!m_emojiMenu && m_replies.contains(m_channelId), "Escape closes the picker before cancelling a reply");
      require(beginMessageEdit("3"), "The reply fixture includes an editable own message");
    } else if (step == 7) {
      focusControl("message-edit");
      const auto* area = m_controlInput.focusedArea();
      m_window.focusChanged(true);
      require(area && m_controlInput.focusedArea() == area, "Activation preserves an inline message editor");
      onKey({XKB_KEY_x, "x"});
      require(
          m_controlInput.focusedArea() == area && m_drafts.at(m_channelId) == "Reply draft",
          "Typing in an inline editor never selects the composer"
      );
      m_controlInput.setFocus(nullptr);
      onKey({XKB_KEY_x, "x"});
      require(
          !m_controlInput.focusedArea() && m_drafts.at(m_channelId) == "Reply draft",
          "An unfocused inline editor prevents automatic composer focus"
      );
      focusControl("message-edit");
      onKey({XKB_KEY_Escape});
      require(!m_messageEdit && m_replies.contains(m_channelId), "Escape closes the editor before the reply");
      onKey({XKB_KEY_Escape});
      require(!m_replies.contains(m_channelId), "A following Escape cancels the remaining reply");
      reply();
      m_window.setUiScale(1.7F);
    } else if (step == 8) {
      geometry();
      onKey({XKB_KEY_Escape});
      focusedComposer();
    } else if (step == 9) {
      require(
          !m_replies.contains(m_channelId) && m_drafts.at(m_channelId) == "Reply draft",
          "Reply controls and focus changes preserve the unsent draft throughout"
      );
      std::clog << "Offline reply geometry, Escape and native composer activation replay passed\n";
    }
    m_window.invalidate();
  }
  void
  ClientApplication::settingsScrollbar(Node& parent, const std::string& id, Hit bounds, float content, float& offset) {
    std::unique_ptr<Node> saved;
    if (auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
      saved = std::move(at->second);
      m_detachedInputs.erase(at);
    } else {
      auto bar = std::make_unique<Scrollbar>();
      bar->setOnScrollChanged([this, position = &offset](float value) {
        m_scrolling.stop(*position);
        *position = value;
        m_window.invalidate();
      });
      saved = std::move(bar);
    }
    auto* bar = static_cast<Scrollbar*>(saved.get());
    m_widgets[id] = bar;
    bar->setPosition(bounds.x, bounds.y);
    bar->setSize(bounds.width, bounds.height);
    bar->update(bounds.height, content, offset);
    parent.addChild(std::move(saved));
  }
  void ClientApplication::verifySettingsScrollbarGutters() const {
    if (!m_settingsOpen && !m_serverSettings.open)
      return;
    const bool server = m_serverSettings.open;
    const auto frame = layout::settingsFrame(m_window.width());
    const auto prefix = server ? "server-settings" : "settings";
    const auto tabs = server ? "server-nav:" : "nav:";
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    for (const bool navigation : {true, false}) {
      const auto id = std::string(prefix) + (navigation ? "-nav-scrollbar" : "-scrollbar");
      const auto found = m_widgets.find(id);
      if (found == m_widgets.end() || !found->second->parent())
        continue;
      const auto* bar = static_cast<const Scrollbar*>(found->second);
      const auto paneRight =
          navigation ? frame.x + frame.navigationContentWidth : frame.contentLeft + frame.contentWidth;
      require(
          bar->x() - layout::Settings::scrollbarInwardExtent - paneRight >= Style::scrollbarGap,
          "Native settings scrollbar expansion/hit slop must stay in its reserved gutter"
      );
      if (bar->visible())
        for (const auto& child : bar->children()) {
          float left = 0, top = 0, right = 0, bottom = 0;
          Node::transformedBounds(child.get(), left, top, right, bottom);
          require(
              left - child->hitTestOutset().left >= paneRight + Style::scrollbarGap,
              "Painted settings scrollbars and their native pointer regions cannot overlap content"
          );
        }
      if (navigation) {
        for (const auto& [key, node] : m_widgets) {
          if (!key.starts_with(tabs) || !node->parent())
            continue;
          float left = 0, top = 0, right = 0, bottom = 0;
          Node::transformedBounds(node, left, top, right, bottom);
          require(right <= paneRight, "Retained settings tabs must fit before the scrollbar gutter");
        }
      } else {
        for (const auto& child : m_scene->children())
          if (child->clipChildren() && child->x() == frame.contentLeft && child->y() == layout::Settings::viewportTop)
            require(child->width() == frame.contentWidth, "The settings category clip must exclude the scrollbar lane");
      }
    }
  }
  void ClientApplication::finishControls() {
    m_scene->setInvalidationCallback([this](NodeInvalidation) {
      if (!m_buildingScene)
        m_window.invalidate();
    });
    m_scene->layout(m_window.target().renderer());
    m_controlInput.setSceneRoot(m_scene.get());
    if (!m_focusedInput.empty()) {
      const auto input = m_inputs.find(m_focusedInput);
      bool mounted = false;
      if (input != m_inputs.end())
        for (const auto* node = static_cast<Node*>(input->second); node; node = node->parent())
          mounted |= node == m_scene.get();
      if (mounted)
        focusControl(m_focusedInput);
      else {
        m_controlInput.setFocus(nullptr);
        m_focusedInput.clear();
      }
    }
    m_buildingScene = false;
    m_window.updateTextInput();
  }
  void ClientApplication::pasteControl(const std::string& id, std::string text) {
    auto input = m_inputs.find(id);
    bool mounted = false;
    if (input != m_inputs.end())
      for (const auto* node = static_cast<Node*>(input->second); node; node = node->parent())
        mounted |= node == m_scene.get();
    if (!mounted || !g_utf8_validate(text.data(), text.size(), nullptr))
      return;
    const bool multiline = m_multilineInputs.contains(id)
        || id == "composer"
        || id == "forum-body"
        || id == "channel-topic"
        || id == "settings:bio"
        || id == "message-edit"
        || (id.starts_with("application-field:") && m_dialogKind == "application-modal");
    std::erase_if(text, [&](unsigned char c) {
      return (c < 32 && !(multiline && (c == '\n' || c == '\t'))) || c == 127;
    });
    TextInputEdit edit;
    edit.hasCommitText = true;
    edit.commitText = std::move(text);
    input->second->textInputApplyEdit(edit);
    m_window.invalidate();
  }
  Input& ClientApplication::inputControl(
      Node& parent, float x, float y, float width, float height, const std::string& id, const std::string& value,
      const std::string& placeholder, bool multiline, bool password
  ) {
    std::unique_ptr<Node> node;
    if (auto stored = m_detachedInputs.find(id); stored != m_detachedInputs.end()) {
      node = std::move(stored->second);
      m_detachedInputs.erase(stored);
    } else {
      auto field = std::make_unique<Input>();
      if (id == "channel-search" || id == "channel-search-panel")
        field->setTextPresenter([this](std::string_view text) { return searchReplacements(text); });
      if (id == "composer" || id == "message-edit")
        field->setTextPresenter([this](std::string_view text) { return composerReplacements(text); });
      field->setFontSize(Style::fontSizeBody);
      field->setMultiline(multiline);
      field->setMultilineCenterWhenFits(id == "composer");
      if (id == "composer")
        field->setHorizontalPadding(4);
      field->setPasswordMode(password);
      field->setSubmitOnEnter(id == "composer");
      field->setOnFocusGain([this, id] {
        m_focusedInput = id;
        if (id == "channel-search" || id == "channel-search-panel")
          m_searchDismissed.clear();
        m_fieldFocused = id == "composer" || id == "token";
        m_settingsField = id.starts_with("settings:") ? id.substr(9) : "";
      });
      field->setOnKeyEvent([this, id](std::uint32_t key, std::uint32_t modifiers) {
        if (((modifiers & KeyMod::Ctrl) && (key == XKB_KEY_v || key == XKB_KEY_V))
            || ((modifiers & KeyMod::Shift) && key == XKB_KEY_Insert)) {
          m_pasteInput = id;
          m_pasteServerScope = serverSettingsScope();
          m_pasteDialogGeneration = m_dialogGeneration;
          m_pasteMessageEditGeneration = m_messageEdit ? m_messageEdit->generation : 0;
          m_pasteConnected = m_connected;
          m_pasteSession = m_session;
          m_pasteChannel = m_channelId;
          m_window.paste(id == "composer" && !(modifiers & KeyMod::Shift));
          return true;
        }
        return false;
      });
      field->setOnChange([this, id](const std::string& text) {
        if (id == "composer") {
          if (text.size() <= 8000) {
            m_drafts[m_channelId] = text;
            m_edit.end(text);
          }
        } else if (id == "dm-create-search") {
          if (m_dialogKind == "dm-create" && text.size() <= 256) {
            m_dmCreateSearch = text;
            m_scrolling.stop(m_dmCreateScroll);
            m_dmCreateScroll = 0;
          }
        } else if (id == "dm-search" || id == "dm-friend-name") {
          if (text.size() <= 256) {
            (id == "dm-search" ? m_dmSearch : m_dmFriendName) = text;
            m_scrolling.stop(m_dmHomeScroll);
            m_dmHomeScroll = 0;
            m_scrolling.stop(m_channelScroll);
            m_channelScroll = 0;
          }
        } else if (id == "message-edit") {
          if (m_messageEdit && !m_messageEdit->request && text.size() <= 8000) {
            m_messageEdit->text = text;
            m_messageEdit->error.clear();
          }
        } else if (id == "emoji-search") {
          if (text.size() <= 256) {
            m_emojiQuery = text;
            m_emojiJump.clear();
            m_scrolling.stop(m_emojiScroll);
            m_emojiScroll = 0;
          }
        } else if (id == "channel-search" || id == "channel-search-panel") {
          if (text.size() <= 1024)
            m_channelSearchText = text;
        } else if (id == "channel-name")
          m_channelFormName = text;
        else if (id == "channel-create-access") {
          m_channelCreateFilter = text;
          m_scrolling.stop(m_channelCreateScroll);
          m_channelCreateScroll = 0;
        } else if (id == "channel-topic")
          m_channelFormTopic = text;
        else if (id == "channel-access-filter") {
          m_channelAccessFilter = text;
          m_scrolling.stop(m_channelDialogScroll);
          m_channelDialogScroll = 0;
        } else if (id.starts_with("application-field:")) {
          if (m_applicationModal && m_dialogKind == "application-modal" && !m_dialogPending)
            for (std::size_t i = 0; i < m_applicationValues.size(); ++i)
              if (id == "application-field:" + std::to_string(i) && text.size() <= 16000)
                m_applicationValues[i] = text;
        } else if (id == "forum-search") {
          if (text.size() <= 400 && api::utf8Length(text) <= 100) {
            m_forumSearch = text;
            updateForumSearch();
          }
        } else if (id == "forum-body") {
          if (text.size() <= 8000)
            m_drafts[m_channelId] = text;
        } else if (id == "gif-search") {
          if (text.size() <= 256) {
            m_gifQuery = text;
            m_gifScroll = 0;
            m_gifSearchTimer.stop();
            if (!m_gifFavoritesTab && !m_demo)
              m_gifSearchTimer.start(std::chrono::milliseconds(300), [this] { searchGifs(); });
          }
        } else if (id == "dialog") {
          m_dialogText = text;
        } else if (id == "token") {
          if (text.size() <= 4096)
            m_token = text;
        } else if (id.starts_with("settings:")) {
          const auto key = id.substr(9);
          const auto limit = key == "display_name" ? 32U : key == "pronouns" ? 40U : key == "bio" ? 190U : 128U;
          if (api::utf8Length(text) <= limit) {
            m_settingsFields[key] = text;
            if (key != "custom_status" && key != "search" && key != "font_search")
              m_profileDirty = true;
            if (key == "search")
              m_settingsNavScroll = 0;
            if (key == "font_search") {
              m_scrolling.stop(m_fontPickerScroll);
              m_fontPickerScroll = 0;
            }
          }
        }
        m_window.invalidate();
      });
      field->setOnSubmit([this, id](const std::string&) {
        if (id == "composer")
          send();
        else if (id == "message-edit")
          submitMessageEdit();
        else if (id == "dm-friend-name")
          dmAction("add-friend");
        else if (id == "dm-create-search")
          dmCreateAction("create-submit");
        else if (id == "dm-search") {
          // Searching conversations never submits profile settings or a draft.
        } else if (id == "emoji-search") {
          // Selection is explicit; Enter must not submit account settings.
        } else if (id == "channel-search" || id == "channel-search-panel")
          searchChannel();
        else if (
            id == "channel-access-filter"
            || id == "channel-create-access"
            || id == "channel-name"
            || id == "channel-topic"
            || id.starts_with("application-field:")
            || id.starts_with("user-account:")
        ) {
          // Forms submit through their explicit Save/Submit action.
        } else if (id == "gif-search")
          searchGifs();
        else if (id == "forum-search") {
          m_forumSearchTimer.stop();
          loadThreads(true);
        } else if (id == "forum-body" || id == "dialog")
          submitDialog();
        else if (id == "token")
          connect();
        else if (id == "settings:custom_status")
          setPresence(api::validStatus(m_presence.status) ? m_presence.status : "online", true);
        else if (id == "settings:font_search") {
          const auto match = std::ranges::find_if(m_installedFonts, [&](const auto& family) {
            return fontFamilyMatches(family, m_settingsFields["font_search"]);
          });
          if (m_fontPickerOpen && match != m_installedFonts.end())
            fontPreference("font-family:" + *match);
        } else if (id != "settings:search" && id != "settings:font_search")
          activate({0, 0, 0, 0, Action::SaveProfile, {}});
      });
      m_inputs[id] = field.get();
      node = std::move(field);
    }
    auto& input = *static_cast<Input*>(node.get());
    if (multiline)
      m_multilineInputs.insert(id);
    else
      m_multilineInputs.erase(id);
    input.setMultiline(multiline);
    input.setSubmitOnEnter(id == "composer" || id == "dialog" || id == "message-edit");
    if (input.value() != value)
      input.setValue(value);
    if (id == "composer" || id == "message-edit" || id == "channel-search" || id == "channel-search-panel")
      input.refreshTextPresentation();
    input.setPlaceholder(placeholder);
    input.setFrameVisible(id != "composer");
    input.setEnabled(
        (m_dialogKind.empty() || id == "dialog" || id.starts_with("server-menu:") || id == "dm-create-search")
        && (!m_serverSettings.open || id.starts_with("server-settings:"))
        && (id == "dm-create-search"   ? !m_dialogPending
                : id == "dialog"       ? !m_dialogPending
                : id == "message-edit" ? m_messageEdit && !m_messageEdit->request
                : id == "token"        ? !m_connecting
                : id == "composer"     ? !m_demo
                    || m_demoOverlay == "sidebar-cycle"
                    || m_demoOverlay == "inline-edit-cycle"
                    || m_demoOverlay.starts_with("outgoing-cycle")
                    || m_demoOverlay.starts_with("ime-cycle")
                    || m_demoOverlay.starts_with("reply-cycle")
                    || m_demoOverlay.starts_with("chat-links-cycle")
                    || m_demoOverlay.starts_with("autocomplete")
                                   : !m_savingProfile)
    );
    input.setControlHeight(height);
    input.setPosition(x, y);
    input.setFrameSize(width, height);
    parent.addChild(std::move(node));
    return input;
  }
} // namespace app
