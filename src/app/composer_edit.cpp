#include "app/client_application.h"
#include "app/composer_format.h"
#include "app/controls/chat_input.h"
#include "app/theme.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"

#include <algorithm>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    enum InputMenuCommand {
      Undo,
      Redo,
      Cut,
      Copy,
      Paste,
      PasteText,
      Delete,
      SelectAll,
      Formatting,
      InsertLink,
      FormatFirst
    };
    constexpr std::array editCommands{Input::EditCommand::Undo,   Input::EditCommand::Redo,
                                      Input::EditCommand::Cut,    Input::EditCommand::Copy,
                                      Input::EditCommand::Delete, Input::EditCommand::SelectAll};
    constexpr std::array editMenuCommands{Undo, Redo, Cut, Copy, Delete, SelectAll};
  } // namespace

  std::optional<ClientApplication::InputEditTarget> ClientApplication::captureInputEdit(const std::string& id) const {
    if (id != "composer" && id != "message-edit")
      return {};
    const auto at = m_inputs.find(id);
    if (at == m_inputs.end() || at->second->hasPreedit())
      return {};
    bool mounted = false;
    for (const auto* node = static_cast<const Node*>(at->second); node; node = node->parent())
      mounted |= node == m_scene.get();
    if (!mounted)
      return {};
    const auto state = at->second->textInputState();
    if (state.hiddenText || state.sensitiveData)
      return {};
    return InputEditTarget{
        id,
        m_channelId,
        at->second->value(),
        m_session,
        id == "message-edit" && m_messageEdit ? m_messageEdit->generation : 0,
        state.anchor,
        state.cursor
    };
  }

  bool ClientApplication::inputEditValid(const InputEditTarget& target) const {
    const bool ownLinkDialog =
        m_dialogKind == "composer-link" && m_composerLink && m_composerLink->target.id == target.id;
    if (!m_connected
        || startupCovered()
        || settingsVisible()
        || (!m_dialogKind.empty() && !ownLinkDialog)
        || !m_viewImage.empty()
        || m_videoExpanded
        || m_shareOpen
        || m_inboxOpen
        || target.session != m_session
        || target.channel != m_channelId)
      return false;
    const auto current = captureInputEdit(target.id);
    return current
        && (ownLinkDialog || m_inputs.at(target.id)->enabled())
        && (target.id != "message-edit" || (m_messageEdit && !m_messageEdit->request))
        && current->editor == target.editor
        && current->value == target.value
        && current->anchor == target.anchor
        && current->cursor == target.cursor;
  }

  bool ClientApplication::openInputContext(float x, float y) {
    if (m_contextOpen && m_contextBounds.contains(x, y))
      return true;
    if (m_composerFormatBounds.contains(x, y))
      return true;
    if (!m_connected
        || startupCovered()
        || settingsVisible()
        || !m_dialogKind.empty()
        || !m_viewImage.empty()
        || m_videoExpanded
        || m_shareOpen
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || m_selectRequest
        || m_voiceMenuInput
        || m_inboxOpen)
      return false;
    for (const auto* id : {"message-edit", "composer"}) {
      const auto at = m_inputs.find(id);
      if (at == m_inputs.end() || !at->second->enabled())
        continue;
      float left, top, right, bottom;
      Node::transformedBounds(at->second, left, top, right, bottom);
      const float sceneY = y + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      if (x < left
          || x >= right
          || sceneY < top
          || sceneY >= bottom
          || m_controlInput.inputAreaAt(x, sceneY) != at->second->inputArea())
        continue;
      // Right clicking keeps the existing native selection, including its direction.
      const auto target = captureInputEdit(id);
      if (!target)
        return true;
      closeContext();
      closeCompletion(true);
      focusControl(id);
      m_inputEditMenu = target;
      m_contextMessage.clear();
      m_contextUser.clear();
      m_contextGuild.clear();
      m_contextChannel = m_channelId;
      m_contextQuickIndex = -1;
      m_contextEntries.clear();
      const auto add = [&](InputMenuCommand command, const char* label, const char* glyph, bool enabled) {
        m_contextEntries.push_back(
            {.id = inputMenuBase + command,
             .label = label,
             .enabled = enabled,
             .hasSubmenu = command == Formatting,
             .icon = glyph}
        );
      };
      const auto separator = [&] { m_contextEntries.push_back({.separator = true}); };
      add(Undo, "Undo", "arrow-back-up", at->second->canEdit(Input::EditCommand::Undo));
      add(Redo, "Redo", "arrow-forward-up", at->second->canEdit(Input::EditCommand::Redo));
      separator();
      add(Cut, "Cut", "scissors", at->second->canEdit(Input::EditCommand::Cut));
      add(Copy, "Copy", "copy", at->second->canEdit(Input::EditCommand::Copy));
      add(Paste, "Paste", "clipboard", true);
      add(PasteText, "Paste as plain text", "clipboard-text", true);
      add(Delete, "Delete", "trash", at->second->canEdit(Input::EditCommand::Delete));
      separator();
      add(SelectAll, "Select All", "select-all", at->second->canEdit(Input::EditCommand::SelectAll));
      separator();
      add(Formatting, "Formatting", "typography", target->anchor != target->cursor);
      add(InsertLink, "Insert Link", "link", true);
      m_contextX = x;
      m_contextY = y;
      m_contextScroll = 0;
      m_contextOpen = m_contextDirty = true;
      m_window.invalidate();
      return true;
    }
    return false;
  }

  void ClientApplication::openInputFormatting(float center) {
    if (!m_inputEditMenu || !inputEditValid(*m_inputEditMenu) || m_inputEditMenu->anchor == m_inputEditMenu->cursor)
      return;
    m_channelSubmenuEntries.clear();
    m_reactionChoices.clear();
    for (std::size_t i = 0; i < composerFormats.size(); ++i) {
      const auto& info = composerFormats[i];
      m_channelSubmenuEntries.push_back(
          {.id = inputMenuBase + FormatFirst + static_cast<int>(i),
           .label = std::string(info.label),
           .enabled = info.format == ComposerFormat::Link
               || formatComposer(m_inputEditMenu->value, m_inputEditMenu->anchor, m_inputEditMenu->cursor, info.format)
                      .has_value(),
           .icon = std::string(info.glyph)}
      );
      m_reactionChoices.emplace_back("", info.label);
    }
    m_channelSubmenu = m_reactionSubmenu = m_reactionSubmenuDirty = true;
    m_reactionSubmenuAnchor = center;
    m_reactionSubmenuScroll = 0;
    m_window.invalidate();
  }

  bool ClientApplication::inputMenuEntry(int id) {
    if (id < inputMenuBase || id >= inputMenuBase + FormatFirst + static_cast<int>(composerFormats.size()))
      return false;
    if (!m_inputEditMenu)
      return true;
    const auto command = id - inputMenuBase;
    const auto target = *m_inputEditMenu;
    if (!inputEditValid(target)) {
      closeContext();
      return true;
    }
    if (command == Formatting) {
      const float center = m_contextControl ? (m_contextControl->rowTop(m_contextControl->highlightedIndex())
                                               + m_contextControl->rowBottom(m_contextControl->highlightedIndex()))
              / 2
                                            : Style::spaceLg;
      openInputFormatting(center);
      return true;
    }
    closeContext();
    focusControl(target.id);
    if (command == InsertLink)
      openComposerLink(target);
    else if (command >= FormatFirst)
      applyComposerFormat(target, command - FormatFirst);
    else if (command == Paste || command == PasteText)
      beginInputPaste(target.id, command == Paste && target.id == "composer", true, command == PasteText);
    else if (const auto at = std::ranges::find(editMenuCommands, command); at != editMenuCommands.end())
      m_inputs.at(target.id)->edit(editCommands[at - editMenuCommands.begin()]);
    m_window.invalidate();
    return true;
  }

  void ClientApplication::applyComposerFormat(const InputEditTarget& target, std::size_t index) {
    if (index >= composerFormats.size() || !inputEditValid(target))
      return;
    if (composerFormats[index].format == ComposerFormat::Link) {
      openComposerLink(target);
      return;
    }
    const auto formatted = formatComposer(target.value, target.anchor, target.cursor, composerFormats[index].format);
    if (formatted) {
      closeCompletion(true);
      m_inputs.at(target.id)->applyEdit(formatted->text, formatted->anchor, formatted->cursor);
      focusControl(target.id);
    }
    m_window.invalidate();
  }

  void ClientApplication::beginInputPaste(const std::string& id, bool images, bool captureSelection, bool plainText) {
    m_pasteAsPlainText = plainText;
    m_pasteEditTarget =
        captureSelection || id == "composer" || id == "message-edit" ? captureInputEdit(id) : std::nullopt;
    m_pasteInput = id;
    m_pasteServerScope = serverSettingsScope();
    m_pasteDialogGeneration = m_dialogGeneration;
    m_pasteMessageEditGeneration = m_messageEdit ? m_messageEdit->generation : 0;
    m_pasteSlashGeneration = m_slashGeneration;
    m_pasteConnected = m_connected;
    m_pasteSession = m_session;
    m_pasteChannel = m_channelId;
    m_window.paste(images);
  }

  bool ClientApplication::composerEditKey(const Key& key) {
    if (m_contextOpen
        || settingsVisible()
        || !m_dialogKind.empty()
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || m_selectRequest
        || m_inboxOpen
        || key.alt
        || key.super)
      return false;
    const auto target = captureInputEdit(m_focusedInput);
    if (!target || !inputEditValid(*target))
      return false;
    const auto state = m_inputs.at(target->id)->textInputState();
    if (key.symbol == XKB_KEY_Menu || (key.shift && key.symbol == XKB_KEY_F10))
      return openInputContext(
          state.cursorRectX, state.cursorRectY + layout::Window::titlebarHeight - m_window.titlebarHeight()
      );
    if (key.control && !key.shift && (key.symbol == XKB_KEY_k || key.symbol == XKB_KEY_K)) {
      openComposerLink(*target);
      return true;
    }
    if (key.control && !key.shift && target->anchor != target->cursor) {
      std::optional<ComposerFormat> format;
      if (key.symbol == XKB_KEY_b || key.symbol == XKB_KEY_B)
        format = ComposerFormat::Bold;
      else if (key.symbol == XKB_KEY_i || key.symbol == XKB_KEY_I)
        format = ComposerFormat::Italic;
      else if (key.symbol == XKB_KEY_u || key.symbol == XKB_KEY_U)
        format = ComposerFormat::Underline;
      if (format) {
        const auto at = std::ranges::find(composerFormats, *format, &ComposerFormatInfo::format);
        applyComposerFormat(*target, at - composerFormats.begin());
        return true;
      }
    }
    return false;
  }

  void ClientApplication::drawComposerFormatting(Node& root, float width, float height) {
    m_composerFormatBounds = {};
    if (m_contextOpen
        || settingsVisible()
        || !m_dialogKind.empty()
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || m_selectRequest
        || m_inboxOpen
        || (!m_demo && !m_window.active())) {
      m_composerFormatTarget.reset();
      return;
    }
    if (const auto target = captureInputEdit(m_focusedInput); target && inputEditValid(*target))
      m_composerFormatTarget = target;
    else {
      bool toolbarFocused = false;
      for (const auto& [id, widget] : m_widgets)
        if (id.starts_with("composer-format:"))
          toolbarFocused |= static_cast<Button*>(widget)->inputArea() == m_controlInput.focusedArea();
      if (!toolbarFocused)
        m_composerFormatTarget.reset();
    }
    if (!m_composerFormatTarget
        || !inputEditValid(*m_composerFormatTarget)
        || m_composerFormatTarget->anchor == m_composerFormatTarget->cursor)
      return;
    const auto target = *m_composerFormatTarget;
    const auto* input = m_inputs.at(target.id);
    // The root translation differs when the desktop supplies the title bar.
    const float titleOffset = layout::Window::titlebarHeight - m_window.titlebarHeight();
    const auto state = input->textInputState();
    float left, top, right, bottom;
    Node::transformedBounds(input, left, top, right, bottom);
    const auto frame = layout::composerFormattingFrame(
        width, height, std::clamp(static_cast<float>(state.cursorRectX), left, right), top + titleOffset,
        composerFormats.size()
    );
    m_composerFormatBounds = frame;
    using F = layout::ComposerFormatting;
    rectangle(root, frame.x, frame.y, frame.width, frame.height, theme::raised, Style::surfaceRadius);
    const float cell = (frame.width - F::padding * 2 - (composerFormats.size() - 1) * F::gap) / composerFormats.size();
    for (std::size_t i = 0; i < composerFormats.size(); ++i) {
      const auto id = "composer-format:" + std::to_string(i);
      std::unique_ptr<Node> node;
      if (const auto old = m_detachedInputs.find(id); old != m_detachedInputs.end()) {
        node = std::move(old->second);
        m_detachedInputs.erase(old);
      } else
        node = std::make_unique<Button>();
      auto* button = static_cast<Button*>(node.get());
      m_widgets[id] = button;
      button->setVariant(ButtonVariant::Ghost);
      button->setGlyph(composerFormats[i].glyph);
      button->setGlyphSize(F::glyph);
      button->setControlHeight(F::cell);
      button->setPadding(0, 0);
      button->setPosition(frame.x + F::padding + i * (cell + F::gap), frame.y + F::padding);
      button->setFrameSize(cell, F::cell);
      button->setTooltip(composerFormats[i].label);
      const auto edit = formatComposer(target.value, target.anchor, target.cursor, composerFormats[i].format);
      button->setEnabled(composerFormats[i].format == ComposerFormat::Link || edit.has_value());
      button->setSelected(edit && edit->removed);
      button->setOnClick([this, target, i] { applyComposerFormat(target, i); });
      root.addChild(std::move(node));
    }
  }
  void ClientApplication::openComposerLink(const InputEditTarget& target) {
    if (!inputEditValid(target) || !m_dialogKind.empty())
      return;
    closeContext();
    closeCompletion(true);
    closeDialog();
    const auto begin = std::min(target.anchor, target.cursor);
    std::string caption = target.value.substr(begin, std::max(target.anchor, target.cursor) - begin);
    std::ranges::replace(caption, '\n', ' ');
    std::ranges::replace(caption, '\r', ' ');
    m_composerLink = ComposerLink{target, std::move(caption), {}};
    m_dialogKind = "composer-link";
    m_dialogError.clear();
    m_dialogAction.reset();
    m_focusedInput = m_composerLink->label.empty() ? "composer-link:label" : "composer-link:url";
    m_window.invalidate();
  }

  void ClientApplication::submitComposerLink() {
    if (m_dialogKind != "composer-link" || !m_composerLink)
      return;
    const auto& link = *m_composerLink;
    if (!inputEditValid(link.target))
      m_dialogError = "The draft or selection changed. Close this window and select the text again.";
    else if (link.label.empty() || link.label.find_first_not_of(" \t\r\n") == std::string::npos)
      m_dialogError = "Enter the text to display.";
    else if (!composerWebUrl(link.url))
      m_dialogError = "Enter a complete http:// or https:// link without spaces.";
    else if (
        const auto edit = linkComposer(link.target.value, link.target.anchor, link.target.cursor, link.label, link.url)
    ) {
      const auto target = link.target;
      closeDialog();
      auto* input = m_inputs.at(target.id);
      input->setEnabled(true);
      input->applyEdit(edit->text, edit->anchor, edit->cursor);
      m_focusedInput = target.id;
      m_window.invalidate();
      return;
    } else
      m_dialogError = "The link would make this message too long. Shorten its text or address.";
    m_window.invalidate();
  }

  void ClientApplication::drawComposerLink(Node& root, float width, float height) {
    if (!m_composerLink) {
      closeDialog();
      return;
    }
    const auto frames = layout::composerLinkFrames(width, height);
    using L = layout::ComposerLink;
    const auto& frame = frames.dialog;
    rectangle(
        root, 0, layout::Window::titlebarHeight, width, height - layout::Window::titlebarHeight,
        withAlpha(theme::background, Style::modalBackdropOpacity)
    );
    m_hits.push_back(
        {0, layout::Window::titlebarHeight, width, height - layout::Window::titlebarHeight, Action::None, {}}
    );
    rectangle(root, frame.x, frame.y, frame.width, frame.height, theme::panel, Style::surfaceRadius);
    m_dialogBounds = {frame.x, frame.y, frame.width, frame.height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    label(
        root, frame.x + L::padding, frame.y + L::padding, frame.width - L::padding * 2 - frames.close.width,
        "Insert Link", Style::fontSizeHeading, theme::foreground, FontWeight::SemiBold
    );
    const auto field = [&](const layout::Frame& bounds, const std::string& id, const std::string& value,
                           const std::string& caption, const std::string& placeholder) {
      label(root, bounds.x, bounds.y - L::fieldLabel, bounds.width, caption, Style::fontSizeCaption, theme::foreground);
      inputControl(root, bounds.x, bounds.y, bounds.width, bounds.height, id, value, placeholder);
    };
    field(frames.label, "composer-link:label", m_composerLink->label, "Text", "Text to display");
    field(frames.url, "composer-link:url", m_composerLink->url, "Link", "https://example.com");
    m_inputs.at("composer-link:url")->setInvalid(!m_dialogError.empty() && !composerWebUrl(m_composerLink->url));
    if (!m_dialogError.empty())
      label(
          root, frames.error.x, frames.error.y, frames.error.width, m_dialogError, Style::fontSizeDescription,
          theme::danger, FontWeight::Normal, 2
      );
    const auto control = [&](std::string id, const layout::Frame& bounds, const std::string& caption,
                             const std::string& icon, bool insert) {
      id = "composer-link:" + id;
      std::unique_ptr<Node> node;
      if (const auto old = m_detachedInputs.find(id); old != m_detachedInputs.end()) {
        node = std::move(old->second);
        m_detachedInputs.erase(old);
      } else
        node = std::make_unique<Button>();
      auto* button = static_cast<Button*>(node.get());
      m_widgets[id] = button;
      button->setText(caption);
      if (!icon.empty())
        button->setGlyph(icon);
      button->setVariant(insert ? ButtonVariant::Primary : ButtonVariant::Ghost);
      button->setControlHeight(bounds.height);
      button->setPosition(bounds.x, bounds.y);
      button->setFrameSize(bounds.width, bounds.height);
      button->setOnClick([this, insert, generation = m_dialogGeneration] {
        if (m_dialogKind != "composer-link" || generation != m_dialogGeneration)
          return;
        if (insert)
          submitComposerLink();
        else
          closeDialog();
      });
      root.addChild(std::move(node));
    };
    control("close", frames.close, "", "x", false);
    control("cancel", frames.cancel, "Cancel", "", false);
    control("insert", frames.insert, "Insert", "", true);
  }

  void ClientApplication::prepareComposerEditDemo() {
    m_preferences.reducedMotion = true;
    m_drafts[m_channelId] = "héllo <#102> 👋";
    m_messages = {{"9701", m_channelId, m_account.name, "Editable message", "2026-11-01T12:00:00Z", m_account.id}};
    m_unreadTimeline.snapshot({});
    m_unreadTimeline.positioned();
  }

  void ClientApplication::demoComposerEditStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto click = [&](float x, float y, bool release = true) {
      y += m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      if (release)
        m_window.released();
    };
    const auto toolbar = [&](std::size_t index, bool release = true) {
      const auto at = m_widgets.find("composer-format:" + std::to_string(index));
      require(at != m_widgets.end() && at->second->parent(), "The selected text has a mounted formatting control");
      float left, top, right, bottom;
      Node::transformedBounds(at->second, left, top, right, bottom);
      click(
          (left + right) / 2, (top + bottom) / 2 + layout::Window::titlebarHeight - m_window.titlebarHeight(), release
      );
    };
    const auto context = [&](const std::string& id) {
      const auto* input = m_inputs.at(id);
      float left, top, right, bottom;
      Node::transformedBounds(input, left, top, right, bottom);
      m_window.contextClick((left + right) / 2, (top + bottom) / 2);
    };
    const auto menuClick = [&](int command, bool child = false) {
      const auto* menu = child ? m_reactionControl : m_contextControl;
      const auto& entries = child ? m_channelSubmenuEntries : m_contextEntries;
      const auto at = std::ranges::find(entries, inputMenuBase + command, &ContextMenuControlEntry::id);
      require(menu && at != entries.end() && at->enabled, "The native editing menu contains the enabled action");
      const auto index = at - entries.begin();
      const auto& bounds = child ? m_reactionSubmenuBounds : m_contextBounds;
      click(
          bounds.x + bounds.width / 2,
          bounds.y + (menu->rowTop(index) + menu->rowBottom(index)) / 2 - (child ? 0 : m_contextScroll)
      );
    };
    const auto original = std::string("héllo <#102> 👋");
    const std::string preview = "__under__ **bold** _italic_ ~~strike~~ ||spoiler|| `code` <#102> 👋";
    const std::string quotedPreview =
        "> **quoted** with `code`\n> a second line that can wrap naturally in a narrow composer";
    switch (m_demoInteractionStep++) {
    case 0:
      focusControl("composer");
      m_inputs.at("composer")->selectAll();
      break;
    case 1:
      require(m_composerFormatBounds.width > 0, "Selecting a draft shows its formatting toolbar");
      require(
          m_composerFormatBounds.x >= 0 && m_composerFormatBounds.x + m_composerFormatBounds.width <= m_window.width(),
          "Formatting controls remain within the window"
      );
      toolbar(0, false);
      break;
    case 2:
      require(m_composerFormatBounds.width > 0, "A pressed toolbar button survives the intervening repaint");
      m_window.released();
      require(m_drafts.at(m_channelId) == "**" + original + "**", "The Bold control keeps protocol mentions intact");
      onKey({.symbol = XKB_KEY_z, .control = true});
      require(m_drafts.at(m_channelId) == original, "Formatting is one native undo transaction");
      context("composer");
      break;
    case 3:
      require(m_inputEditMenu && m_contextOpen, "Right clicking the composer opens its native editing menu");
      require(m_inputEditMenu->anchor != m_inputEditMenu->cursor, "Right click preserves the selected text");
      m_contextControl->setHighlightedIndex(m_contextEntries.size() - 2);
      onKey({.symbol = XKB_KEY_Right});
      break;
    case 4:
      require(
          m_channelSubmenu && m_channelSubmenuEntries.size() == composerFormats.size(),
          "The Formatting submenu exposes every supported format"
      );
      menuClick(FormatFirst + 1, true);
      break;
    case 5:
      require(m_drafts.at(m_channelId) == "*" + original + "*", "The submenu formats the original native selection");
      onKey({.symbol = XKB_KEY_z, .control = true});
      onKey({.symbol = XKB_KEY_u, .control = true});
      require(m_drafts.at(m_channelId) == "__" + original + "__", "Underline applies to the selected draft");
      break;
    case 6:
      toolbar(6);
      require(
          m_drafts.at(m_channelId) == "__||" + original + "||__", "Toolbar activation survives pointer focus transfer"
      );
      onKey({.symbol = XKB_KEY_z, .control = true});
      onKey({.symbol = XKB_KEY_z, .control = true});
      m_inputs.at("composer")->selectAll();
      context("composer");
      break;
    case 7:
      menuClick(Cut);
      require(m_drafts.at(m_channelId).empty(), "Cut uses the native selection and undo stack");
      break;
    case 8:
      onKey({.symbol = XKB_KEY_z, .control = true});
      require(m_drafts.at(m_channelId) == original, "Undo restores the cut text and its selection");
      beginInputPaste("composer", false, true);
      m_window.pasted("Replacement", "text/plain;charset=utf-8");
      break;
    case 9:
      require(m_drafts.at(m_channelId) == "Replacement", "Menu paste replaces the captured selection without sending");
      beginInputPaste("composer", false, true);
      pasteControl("composer", " newer");
      m_window.pasted("STALE", "text/plain;charset=utf-8");
      require(m_drafts.at(m_channelId) == "Replacement newer", "Late clipboard reads cannot replace a changed draft");
      require(beginMessageEdit("9701"), "The fixture includes an editable own message");
      break;
    case 10:
      focusControl("message-edit");
      m_inputs.at("message-edit")->selectAll();
      onKey({.symbol = XKB_KEY_i, .control = true});
      require(
          m_messageEdit && m_messageEdit->text == "*Editable message*" && !m_inboxOpen,
          "Formatting shortcuts apply to the selected inline editor before global shortcuts"
      );
      onKey({.symbol = XKB_KEY_z, .control = true});
      onKey({.symbol = XKB_KEY_Menu});
      break;
    case 11:
      require(m_inputEditMenu && m_inputEditMenu->id == "message-edit", "The keyboard opens the editor context menu");
      pasteControl("message-edit", "Changed externally");
      inputMenuEntry(inputMenuBase + Cut);
      require(
          m_messageEdit && m_messageEdit->text == "Changed externally" && !m_contextOpen,
          "A changed selection invalidates the menu instead of editing its new text"
      );
      cancelMessageEdit();
      break;
    case 12:
      focusControl("composer");
      m_inputs.at("composer")->textInputApplyEdit({.preeditText = "temporary", .hasPreedit = true});
      context("composer");
      require(
          !m_contextOpen && m_inputs.at("composer")->hasPreedit(), "Opening a menu never commits or cancels preedit"
      );
      break;
    case 13:
      require(m_composerFormatBounds.width == 0, "IME composition hides formatting controls");
      m_inputs.at("composer")->textInputResetPreedit();
      m_inputs.at("composer")->selectAll();
      context("composer");
      break;
    case 14:
      require(m_inputEditMenu.has_value(), "The composer menu opens again after composition ends");
      m_window.focusChanged(false);
      require(!m_contextOpen && !m_inputEditMenu, "Losing window focus closes editing menus");
      m_window.focusChanged(true);
      break;
    case 15:
      require(
          !m_messageEdit && m_drafts.at(m_channelId) == "Replacement newer" && !m_pasteEditTarget,
          "Formatting, clipboard and editor actions preserve the unsent draft"
      );
      focusControl("composer");
      m_inputs.at("composer")->selectAll();
      toolbar(composerFormats.size() - 1);
      break;
    case 16:
      require(
          m_composerLink && m_composerLink->label == "Replacement newer", "Insert Link starts with the selected text"
      );
      require(m_inputs.at("composer-link:url")->enabled(), "The link dialog uses an editable URL field");
      submitComposerLink();
      require(
          !m_dialogError.empty() && m_dialogKind == "composer-link", "An invalid URL retains the dialog and its values"
      );
      pasteControl("composer-link:url", "https://example.com/path(a)");
      onKey({.symbol = XKB_KEY_Return});
      break;
    case 17:
      require(
          m_dialogKind.empty() && m_drafts.at(m_channelId) == "[Replacement newer](https://example.com/path%28a%29)",
          "Insert Link adds escaped Markdown without sending the message"
      );
      onKey({.symbol = XKB_KEY_z, .control = true});
      require(m_drafts.at(m_channelId) == "Replacement newer", "Link insertion is one native undo operation");
      m_inputs.at("composer")->clearSelection();
      onKey({.symbol = XKB_KEY_k, .control = true});
      break;
    case 18:
      require(m_composerLink && m_composerLink->label.empty(), "Insert Link also works without selected text");
      pasteControl("composer-link:label", "Project");
      pasteControl("composer-link:url", "https://example.com");
      onKey({.symbol = XKB_KEY_Escape});
      require(
          !m_composerLink && m_drafts.at(m_channelId) == "Replacement newer", "Cancel preserves the original draft"
      );
      break;
    case 19:
      focusControl("composer");
      m_inputs.at("composer")->selectAll();
      beginInputPaste("composer", false, true);
      m_window.pasted("https://example.com", "text/plain;charset=utf-8");
      require(
          m_drafts.at(m_channelId) == "[Replacement newer](https://example.com)", "Pasting a URL links selected text"
      );
      onKey({.symbol = XKB_KEY_z, .control = true});
      beginInputPaste("composer", false, true, true);
      m_window.pasted("https://example.com", "text/plain;charset=utf-8");
      require(m_drafts.at(m_channelId) == "https://example.com", "Plain-text paste keeps the URL literal");
      break;
    case 20:
      onKey({.symbol = XKB_KEY_z, .control = true});
      onKey({.symbol = XKB_KEY_k, .control = true});
      break;
    case 21:
      require(m_composerLink.has_value(), "The link editor can reopen for the current selection");
      m_inputs.at("composer")->setValue("A changed draft");
      m_drafts[m_channelId] = "A changed draft";
      m_composerLink->url = "https://example.com";
      submitComposerLink();
      require(
          !m_dialogError.empty() && m_drafts.at(m_channelId) == "A changed draft", "Link insertion rejects stale drafts"
      );
      closeDialog();
      break;
    case 22:
      require(
          !m_composerLink && !m_contextOpen && m_drafts.at(m_channelId) == "A changed draft",
          "All editing controls retain the latest unsent content"
      );
      focusControl("composer");
      m_inputs.at("composer")->selectAll();
      pasteControl("composer", preview);
      break;
    case 23: {
      const auto* input = m_inputs.at("composer");
      require(
          input->value() == preview && input->displayValue().contains("#"),
          "Markdown preview preserves the source alongside readable mention tokens"
      );
      const TextPresentation presentation(preview, composerReplacements(preview));
      const auto markup = composerPreviewMarkup(preview, presentation);
      require(
          markup.contains("weight=\"bold\"")
              && markup.contains("font_family=\"monospace\"")
              && markup.contains("underline=\"single\"")
              && markup.contains("background="),
          "Typed formatting uses native font styles and highlighted code and spoilers"
      );
      m_inputs.at("composer")->selectAll();
      pasteControl("composer", quotedPreview);
      break;
    }
    case 24:
      require(
          m_inputs.at("composer")->displayValue().starts_with("▎ **quoted**")
              && m_drafts.at(m_channelId) == quotedPreview,
          "Quoted drafts show vertical bars while retaining the sendable source"
      );
      m_inputs.at("composer")->selectAll();
      m_inputs.at("composer")->textInputApplyEdit({.preeditText = "仮", .hasPreedit = true});
      break;
    case 25:
      require(
          m_inputs.at("composer")->hasPreedit() && m_drafts.at(m_channelId) == quotedPreview,
          "Composition over styled quotes preserves the committed draft"
      );
      m_inputs.at("composer")->textInputResetPreedit();
      m_inputs.at("composer")->applyEdit(quotedPreview, 1, 1);
      onKey({.symbol = XKB_KEY_BackSpace});
      break;
    case 26:
      require(m_drafts.at(m_channelId) == quotedPreview.substr(1), "Backspace can remove a presented quote marker");
      onKey({.symbol = XKB_KEY_z, .control = true});
      break;
    case 27:
      require(
          m_drafts.at(m_channelId) == quotedPreview && m_inputs.at("composer")->displayValue().starts_with("▎ "),
          "Undo restores quote presentation and source together"
      );
      require(beginMessageEdit("9701"), "The styled fixture opens an editable own message");
      break;
    case 28:
      focusControl("message-edit");
      m_inputs.at("message-edit")->selectAll();
      pasteControl("message-edit", preview);
      break;
    case 29:
      require(
          m_messageEdit && m_messageEdit->text == preview && dynamic_cast<ChatInput*>(m_inputs.at("message-edit")),
          "Inline message edits use the same Markdown preview without sending"
      );
      cancelMessageEdit();
      break;
    case 30:
      require(
          m_drafts.at(m_channelId) == quotedPreview && !m_messageEdit,
          "Closing the styled editor preserves the separate quoted draft"
      );
      std::clog << "Offline composer formatting, preview, quotes, undo, clipboard and composition replay passed\n";
      break;
    }
    m_window.invalidate();
  }
} // namespace app
