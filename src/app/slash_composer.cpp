#include "app/client_application.h"
#include "app/theme.h"
#include "ui/controls/scrollbar.h"

#include <algorithm>
#include <cmath>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    using Metrics = layout::SlashCommands;
    using Type = api::CommandOptionType;
    std::string folded(std::string_view text) {
      auto* value = g_utf8_casefold(text.data(), text.size());
      std::string result = value ? value : "";
      g_free(value);
      return result;
    }
  } // namespace
  bool ClientApplication::slashComposerActive() const {
    return m_slashCommand
        && m_connected
        && !startupCovered()
        && m_slashChannel == m_channelId
        && !forumView()
        && (!m_dmMode || m_dmPage.empty());
  }
  void ClientApplication::cancelSlashCommand(bool restoreFocus) {
    if (!m_slashCommand)
      return;
    closeCompletion();
    m_slashAutocompleteTimer.stop();
    m_slashAutocompleteTimeout.stop();
    m_slashSubmissionTimer.stop();
    m_slashAutocompleteNonce.clear();
    m_slashNonce.clear();
    ++m_slashGeneration;
    m_slashCommand.reset();
    m_slashValues.clear();
    m_slashChoices.clear();
    m_slashMembers.clear();
    m_slashValueLabels.clear();
    m_slashUploads.clear();
    m_slashSelectedOptions.clear();
    m_slashActiveOption.reset();
    m_slashInlineItems.clear();
    m_slashOptionQuery.clear();
    m_slashError.clear();
    m_slashPending = m_slashOptionsOpen = m_slashOptionHighlighted = false;
    m_slashChannel.clear();
    m_slashDraft.clear();
    m_slashComposerFrame = {};
    m_slashComposerLayout = {};
    m_scrolling.stop(m_slashComposerScroll);
    m_slashComposerScroll = 0;
    if (m_slashFileRequest)
      m_filePicker.cancel();
    m_slashFileRequest = 0;
    for (auto& [id, input] : m_inputs)
      if (id.starts_with("slash-field:") || id == "slash-options")
        input->setValue("");
    if (restoreFocus) {
      m_controlInput.setFocus(nullptr);
      m_focusedInput = "composer";
      focusControl("composer");
    }
    m_window.invalidate();
  }
  void ClientApplication::selectSlashOption(std::optional<std::size_t> index) {
    if (!slashComposerActive() || m_slashPending || (index && *index >= m_slashValues.size()))
      return;
    m_controlInput.setFocus(nullptr);
    m_slashAutocompleteTimer.stop();
    m_slashAutocompleteTimeout.stop();
    m_slashAutocompleteNonce.clear();
    m_slashChoices.clear();
    m_slashActiveOption = index;
    m_slashNonce.clear();
    m_slashOptionsOpen = true;
    m_slashRevealActive = true;
    m_slashOptionHighlighted = false;
    m_slashOptionQuery.clear();
    if (m_inputs.contains("slash-options"))
      m_inputs.at("slash-options")->setValue("");
    closeCompletion();
    if (index) {
      m_slashSelectedOptions.insert(*index);
      if (m_slashCommand->options[*index].type == Type::Attachment) {
        m_slashActiveOption.reset();
        m_focusedInput = "slash-options";
        focusControl(m_focusedInput);
        if (!m_slashUploads.contains(*index))
          pickSlashAttachment(*index);
      } else {
        m_focusedInput = "slash-field:" + std::to_string(*index);
        slashValueChanged(*index);
        focusControl(m_focusedInput);
      }
    } else {
      m_focusedInput = "slash-options";
      focusControl(m_focusedInput);
    }
    m_completionDirty = true;
    m_window.invalidate();
  }
  void ClientApplication::advanceSlashOption(bool reverse) {
    if (!m_slashCommand)
      return;
    if (m_slashActiveOption) {
      const auto index = *m_slashActiveOption;
      if (reverse) {
        const auto before = m_slashSelectedOptions.lower_bound(index);
        if (before != m_slashSelectedOptions.begin())
          selectSlashOption(*std::prev(before));
        else
          selectSlashOption({});
        return;
      }
      for (std::size_t i = index + 1; i < m_slashValues.size(); ++i)
        if (m_slashSelectedOptions.contains(i) || m_slashCommand->options[i].required) {
          selectSlashOption(i);
          return;
        }
      selectSlashOption({});
    } else if (reverse && !m_slashSelectedOptions.empty())
      selectSlashOption(*m_slashSelectedOptions.rbegin());
    else {
      updateSlashArgumentCompletion();
      if (!m_completionItems.empty() && m_completionItems.front().kind == CompletionItem::SlashOption)
        selectSlashOption(std::stoul(m_completionItems.front().id));
    }
  }
  void ClientApplication::updateSlashArgumentCompletion() {
    const auto fieldId = m_slashActiveOption ? "slash-field:" + std::to_string(*m_slashActiveOption) : "slash-options";
    const auto field = m_inputs.find(fieldId);
    if (!slashComposerActive()
        || !m_slashOptionsOpen
        || m_slashPending
        || !m_dialogKind.empty()
        || settingsVisible()
        || m_contextOpen
        || m_inboxOpen
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || !m_viewImage.empty()
        || m_videoExpanded
        || m_focusedInput != fieldId
        || (field != m_inputs.end() && field->second->hasPreedit())) {
      closeCompletion();
      return;
    }
    const auto text = m_slashActiveOption ? m_slashValues[*m_slashActiveOption] : m_slashOptionQuery;
    const bool changed = !m_completionQuery || m_completionInput != fieldId || m_completionSnapshot != text;
    if (!changed && !m_completionDirty)
      return;
    if (changed) {
      m_completionSelected = 0;
      m_scrolling.stop(m_completionScroll);
      m_completionScroll = 0;
    }
    m_completionInput = fieldId;
    m_completionSnapshot = text;
    m_completionQuery = CompletionQuery{'/', 0, text.size(), text};
    m_completionItems.clear();
    if (m_slashActiveOption) {
      for (std::size_t i = 0; i < m_slashChoices.size(); ++i)
        m_completionItems.push_back(
            {CompletionItem::SlashValue, std::to_string(i), m_slashChoices[i].name, {}, {}, {}}
        );
    } else {
      const auto query = folded(m_slashOptionQuery);
      for (std::size_t i = 0; i < m_slashValues.size(); ++i) {
        const auto& option = m_slashCommand->options[i];
        if (m_slashSelectedOptions.contains(i)
            || (!query.empty()
                && !folded(option.label).contains(query)
                && !folded(option.name).contains(query)
                && !folded(option.description).contains(query)))
          continue;
        m_completionItems.push_back(
            {CompletionItem::SlashOption,
             std::to_string(i),
             option.label + (option.required ? " *" : ""),
             option.description,
             {},
             {}}
        );
      }
      if (m_completionItems.empty() && m_slashOptionQuery.empty()) {
        closeCompletion();
        return;
      }
    }
    if (m_completionSelected >= m_completionItems.size())
      m_completionSelected = 0;
    m_completionDirty = false;
  }
  bool ClientApplication::slashCommandKey(const Key& key) {
    if (!slashComposerActive()
        || settingsVisible()
        || !m_dialogKind.empty()
        || m_contextOpen
        || m_inboxOpen
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || !m_viewImage.empty()
        || m_videoExpanded)
      return false;
    if (m_controlInput.focusedArea()
        && m_focusedInput != "slash-options"
        && !m_focusedInput.starts_with("slash-field:"))
      return false;
    if (m_slashPending)
      return !key.alt && !key.super && (!key.control || key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter);
    if (key.alt || key.super)
      return false;
    if (key.control && (key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter)) {
      submitSlashCommand();
      return true;
    }
    if (key.control)
      return false;
    updateSlashArgumentCompletion();
    if (key.symbol == XKB_KEY_Escape) {
      if (m_completionQuery) {
        m_slashOptionsOpen = false;
        m_slashAutocompleteTimer.stop();
        m_slashAutocompleteTimeout.stop();
        m_slashAutocompleteNonce.clear();
        closeCompletion();
      } else
        cancelSlashCommand();
      m_window.invalidate();
      return true;
    }
    if (key.symbol == XKB_KEY_Up || key.symbol == XKB_KEY_Down) {
      if (!m_completionQuery) {
        m_slashOptionsOpen = true;
        if (m_slashActiveOption)
          slashValueChanged(*m_slashActiveOption);
        updateSlashArgumentCompletion();
      }
      if (m_completionItems.empty())
        return false;
      const bool highlighted = m_slashOptionHighlighted || m_slashActiveOption.has_value();
      m_slashOptionHighlighted = true;
      m_completionSelected = highlighted
          ? (m_completionSelected + (key.symbol == XKB_KEY_Up ? m_completionItems.size() - 1 : 1))
              % m_completionItems.size()
          : key.symbol == XKB_KEY_Up ? m_completionItems.size() - 1
                                     : 0;
      const float top = m_completionSelected * Metrics::rowHeight;
      m_scrolling.stop(m_completionScroll);
      m_completionScroll =
          std::clamp(m_completionScroll, std::max(0.0F, top + Metrics::rowHeight - m_completionViewport), top);
      m_window.invalidate();
      return true;
    }
    if (key.symbol == XKB_KEY_Tab || key.symbol == XKB_KEY_ISO_Left_Tab) {
      if (!key.shift && m_completionQuery && !m_completionItems.empty()) {
        const auto item = m_completionItems[m_completionSelected];
        chooseCompletion(m_completionSelected);
        if (item.kind == CompletionItem::SlashValue)
          advanceSlashOption(false);
      } else
        advanceSlashOption(key.shift || key.symbol == XKB_KEY_ISO_Left_Tab);
      return true;
    }
    if ((key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter) && !key.shift) {
      if (m_completionQuery
          && !m_completionItems.empty()
          && (m_slashActiveOption || m_slashOptionHighlighted || !m_slashOptionQuery.empty()))
        chooseCompletion(m_completionSelected);
      else if (!m_slashOptionQuery.empty()) {
        m_slashError = "Choose an option or clear the option search before sending.";
        m_window.invalidate();
      } else
        submitSlashCommand();
      return true;
    }
    if (key.symbol == XKB_KEY_BackSpace && !key.shift) {
      const auto id = m_slashActiveOption ? "slash-field:" + std::to_string(*m_slashActiveOption) : "slash-options";
      const auto field = m_inputs.find(id);
      if (field != m_inputs.end() && field->second->value().empty()) {
        if (m_slashActiveOption) {
          const auto previous = m_slashSelectedOptions.lower_bound(*m_slashActiveOption);
          const auto before = previous == m_slashSelectedOptions.begin() ? std::optional<std::size_t>{}
                                                                         : std::optional{*std::prev(previous)};
          m_slashSelectedOptions.erase(*m_slashActiveOption);
          if (before)
            selectSlashOption(before);
          else
            cancelSlashCommand();
        } else if (!m_slashSelectedOptions.empty())
          selectSlashOption(*m_slashSelectedOptions.rbegin());
        else
          cancelSlashCommand();
        return true;
      }
    }
    return false;
  }
  float ClientApplication::measureSlashComposer(float width, float height) {
    auto& renderer = m_window.target().renderer();
    m_slashInlineItems.clear();
    std::vector<layout::Frame> sizes;
    const float available = std::max(Style::controlHeight, width - Metrics::composerPadding * 2);
    const auto measure = [&](const std::string& text, float size, FontWeight weight = FontWeight::Normal) {
      return std::ceil(renderer.measureText(text, size, weight).width);
    };
    const auto commandWidth = std::min(
        available,
        Metrics::appSize
            + Style::spaceSm
            + measure(m_slashCommand->displayName(), Style::fontSizeBody, FontWeight::SemiBold)
    );
    m_slashInlineItems.push_back({SlashInlineItem::Command});
    sizes.push_back({0, 0, commandWidth, Style::controlHeight});
    for (const auto option : m_slashSelectedOptions) {
      if (option >= m_slashValues.size())
        continue;
      const auto& definition = m_slashCommand->options[option];
      std::string display = m_slashValues[option];
      if (const auto cached = m_slashValueLabels.find(option); cached != m_slashValueLabels.end())
        if (const auto value = cached->second.find(display); value != cached->second.end())
          display = value->second;
      if (const auto upload = m_slashUploads.find(option); upload != m_slashUploads.end())
        display = upload->second.filename;
      const float labelWidth = std::min(
          available / 3,
          measure(definition.label, Style::fontSizeDescription, FontWeight::SemiBold) + Style::spaceMd * 2
      );
      const float desired =
          labelWidth + measure(display.empty() ? "Enter a value" : display, Style::fontSizeBody) + Style::spaceMd * 2;
      const bool active = m_slashActiveOption == option && definition.type != Type::Attachment;
      const float itemWidth = std::min(
          available,
          active ? std::clamp(desired, Metrics::inputMinimumWidth, Metrics::inputMaximumWidth)
                 : std::min(desired, Metrics::chipMaximumWidth)
      );
      float itemHeight = Style::controlHeight;
      if (active && definition.type == Type::String && !display.empty()) {
        const auto text = renderer.measureText(
            display, Style::fontSizeBody, FontWeight::Normal,
            std::max(Style::controlHeight, itemWidth - labelWidth - Style::spaceSm), 0
        );
        itemHeight = std::clamp(
            std::ceil(text.bottom - text.top) + Style::spaceMd * 2, Style::controlHeight, Metrics::inputMaximumHeight
        );
      }
      m_slashInlineItems.push_back({SlashInlineItem::Option, option, labelWidth});
      sizes.push_back({0, 0, itemWidth, itemHeight});
    }
    const auto remaining = m_slashValues.size() - m_slashSelectedOptions.size();
    const auto tail = m_slashOptionQuery.empty()
        ? remaining ? "+" + std::to_string(remaining) + (remaining == 1 ? " option" : " options") : "Enter to send"
        : m_slashOptionQuery;
    m_slashInlineItems.push_back({SlashInlineItem::Tail});
    sizes.push_back(
        {0, 0,
         std::min(available, std::max(Style::spaceGroup * 2, measure(tail, Style::fontSizeBody) + Style::spaceMd * 2)),
         Style::controlHeight}
    );
    auto layout = Metrics::compose(width, height, sizes, !m_slashError.empty());
    if (m_slashComposerFrame.width != width || layout.viewportHeight != m_slashComposerLayout.viewportHeight)
      m_slashRevealActive = true;
    m_slashComposerLayout = std::move(layout);
    m_scrolling.limit(
        m_slashComposerScroll,
        std::max(0.0F, m_slashComposerLayout.contentHeight - m_slashComposerLayout.viewportHeight)
    );
    if (m_slashRevealActive) {
      for (std::size_t i = 0; i < m_slashInlineItems.size(); ++i) {
        const auto& item = m_slashInlineItems[i];
        if ((m_slashActiveOption && item.kind == SlashInlineItem::Option && item.option == *m_slashActiveOption)
            || (!m_slashActiveOption && item.kind == SlashInlineItem::Tail)) {
          const auto& frame = m_slashComposerLayout.items[i];
          m_scrolling.stop(m_slashComposerScroll);
          m_slashComposerScroll = std::clamp(
              m_slashComposerScroll, std::max(0.0F, frame.y + frame.height - m_slashComposerLayout.viewportHeight),
              frame.y
          );
        }
      }
      m_slashRevealActive = false;
    }
    return m_slashComposerLayout.height;
  }
  void ClientApplication::drawSlashComposer(Node& root, const layout::Frame& frame) {
    using namespace theme;
    const auto textTop = [&](std::string_view text, float size, FontWeight weight, float height) {
      const auto bounds = m_window.target().renderer().measureText(std::string(text), size, weight, 0, 1);
      const auto font = m_window.target().renderer().measureFont(size, weight);
      return font.capHeight > 0 ? height / 2 + font.capHeight / 2 + bounds.top
                                : (height - (bounds.bottom - bounds.top)) / 2;
    };
    m_slashComposerFrame = frame;
    m_composerTop = frame.y;
    const float closeX = frame.x + frame.width - Style::controlHeight;
    const float commandWidth = std::min(
        frame.width / 3,
        std::ceil(m_window.target()
                      .renderer()
                      .measureText(m_slashCommand->displayName(), Style::fontSizeDescription, FontWeight::SemiBold)
                      .width)
    );
    label(
        root, frame.x + Style::spaceMd, frame.y + Style::spaceSm, commandWidth, m_slashCommand->displayName(),
        Style::fontSizeDescription, foreground, FontWeight::SemiBold
    );
    label(
        root, frame.x + Style::spaceMd + commandWidth + Style::spaceSm, frame.y + Style::spaceSm,
        std::max(0.0F, closeX - frame.x - commandWidth - Style::spaceMd * 2 - Style::spaceSm),
        m_slashPending ? "Sending command…" : m_slashCommand->description, Style::fontSizeDescription, muted
    );
    iconButton(
        root,
        {closeX,
         frame.y + Style::spaceXs,
         Style::spaceXl,
         Style::spaceXl,
         m_slashPending ? Action::None : Action::CancelSlashCommand,
         {}},
        "circle-x", "Cancel command"
    );
    rectangle(
        root, frame.x + Style::borderWidth, frame.y + Metrics::composerHeader - Style::borderWidth,
        frame.width - Style::borderWidth * 2, Style::borderWidth, raised
    );
    auto body = std::make_unique<Node>();
    body->setPosition(frame.x + Metrics::composerPadding, frame.y + Metrics::composerHeader + Metrics::composerPadding);
    body->setSize(frame.width - Metrics::composerPadding * 2, m_slashComposerLayout.viewportHeight);
    body->setClipChildren(true);
    body->setTextScrollOffset(-m_slashComposerScroll);
    for (std::size_t i = 0; i < m_slashInlineItems.size(); ++i) {
      const auto& item = m_slashInlineItems[i];
      auto itemFrame = m_slashComposerLayout.items[i];
      itemFrame.y -= m_slashComposerScroll;
      if (item.kind == SlashInlineItem::Command) {
        const auto& app = m_slashCommand->app;
        const auto icon = app.icon.empty() ? std::string{} : "/app-icons/" + app.id + "/" + app.icon + ".png?size=64";
        if (!image(
                *body, itemFrame.x, itemFrame.y + (itemFrame.height - Metrics::appSize) / 2, Metrics::appSize, icon,
                Style::controlRadius
            ))
          glyph(
              *body, itemFrame.x + Style::spaceXs, itemFrame.y + Style::spaceSm, Style::glyphSizeLarge, "command", muted
          );
        label(
            *body, itemFrame.x + Metrics::appSize + Style::spaceSm,
            itemFrame.y
                + textTop(m_slashCommand->displayName(), Style::fontSizeBody, FontWeight::SemiBold, itemFrame.height),
            itemFrame.width - Metrics::appSize - Style::spaceSm, m_slashCommand->displayName(), Style::fontSizeBody,
            foreground, FontWeight::SemiBold
        );
        continue;
      }
      const auto clippedHit = [&](Action action, const std::string& id, float x, float width) {
        const float top = std::max(0.0F, itemFrame.y),
                    bottom = std::min(body->height(), itemFrame.y + itemFrame.height);
        if (bottom > top)
          m_hits.push_back(
              {body->x() + x, body->y() + top, width, bottom - top, m_slashPending ? Action::None : action, id}
          );
      };
      if (item.kind == SlashInlineItem::Tail) {
        const auto remaining = m_slashValues.size() - m_slashSelectedOptions.size();
        const auto placeholder =
            remaining ? "+" + std::to_string(remaining) + (remaining == 1 ? " option" : " options") : "Enter to send";
        auto& input = inputControl(
            *body, itemFrame.x, itemFrame.y, itemFrame.width, itemFrame.height, "slash-options", m_slashOptionQuery,
            placeholder
        );
        input.setFrameVisible(false);
        input.setEnabled(!m_slashPending && m_dialogKind.empty());
        input.setHorizontalPadding(Style::spaceXs);
        continue;
      }
      const auto option = item.option;
      const auto& definition = m_slashCommand->options[option];
      const bool active = m_slashActiveOption == option && definition.type != Type::Attachment;
      rectangle(
          *body, itemFrame.x, itemFrame.y, itemFrame.width, itemFrame.height, active ? accent : raised,
          Style::controlRadius
      );
      if (active)
        rectangle(
            *body, itemFrame.x + Style::borderWidth, itemFrame.y + Style::borderWidth,
            itemFrame.width - Style::borderWidth * 2, itemFrame.height - Style::borderWidth * 2, panel,
            Style::controlRadius
        );
      label(
          *body, itemFrame.x + Style::spaceSm,
          itemFrame.y + textTop(definition.label, Style::fontSizeDescription, FontWeight::SemiBold, itemFrame.height),
          item.labelWidth - Style::spaceSm, definition.label, Style::fontSizeDescription, foreground,
          FontWeight::SemiBold
      );
      if (active) {
        clippedHit(Action::SlashOption, std::to_string(option), itemFrame.x, item.labelWidth);
        auto& input = inputControl(
            *body, itemFrame.x + item.labelWidth, itemFrame.y, itemFrame.width - item.labelWidth, itemFrame.height,
            "slash-field:" + std::to_string(option), m_slashValues[option], "Enter a value",
            definition.type == Type::String
        );
        input.setFrameVisible(false);
        input.setMultilineCenterWhenFits(true);
        input.setHorizontalPadding(Style::spaceXs);
        input.setEnabled(!m_slashPending && m_dialogKind.empty());
        input.refreshTextPresentation();
      } else {
        std::string value = m_slashValues[option];
        if (const auto labels = m_slashValueLabels.find(option); labels != m_slashValueLabels.end())
          if (const auto found = labels->second.find(value); found != labels->second.end())
            value = found->second;
        const auto upload = m_slashUploads.find(option);
        if (upload != m_slashUploads.end())
          value = upload->second.filename;
        const float removeWidth = definition.type == Type::Attachment ? Style::spaceXl : 0;
        label(
            *body, itemFrame.x + item.labelWidth,
            itemFrame.y
                + textTop(
                    value.empty() ? "Choose a value" : value, Style::fontSizeBody, FontWeight::Normal, itemFrame.height
                ),
            std::max(0.0F, itemFrame.width - item.labelWidth - Style::spaceSm - removeWidth),
            value.empty() ? "Choose a value" : value, Style::fontSizeBody, value.empty() ? muted : foreground
        );
        clippedHit(Action::SlashOption, std::to_string(option), itemFrame.x, itemFrame.width - removeWidth);
        if (definition.type == Type::Attachment) {
          glyph(
              *body, itemFrame.x + itemFrame.width - removeWidth, itemFrame.y + Style::spaceSm, Style::glyphSizeSmall,
              upload == m_slashUploads.end() ? "plus" : "x", muted
          );
          clippedHit(
              Action::SlashAttachment, std::to_string(option), itemFrame.x + itemFrame.width - removeWidth, removeWidth
          );
        }
      }
    }
    root.addChild(std::move(body));
    if (!m_slashError.empty())
      label(
          root, frame.x + Metrics::composerPadding, frame.y + frame.height - Metrics::feedbackHeight,
          frame.width - Metrics::composerPadding * 2, m_slashError, Style::fontSizeDescription, danger,
          FontWeight::Normal, 2
      );
  }
  void ClientApplication::drawSlashOptions(Node& root, float w, float h) {
    using namespace theme;
    updateSlashArgumentCompletion();
    if (!m_completionQuery)
      return;
    const auto frame = Metrics::picker(
        m_slashComposerFrame.x, m_slashComposerFrame.y,
        std::min(m_slashComposerFrame.width, w - m_slashComposerFrame.x - Style::spaceSm), m_completionItems.size()
    );
    if (frame.height < Metrics::headerHeight + Metrics::footerHeight + Style::controlHeight) {
      m_completionBounds = {};
      return;
    }
    m_completionBounds = {frame.x, frame.y, frame.width, frame.height, Action::None, {}};
    m_hits.push_back(m_completionBounds);
    rectangle(root, frame.x, frame.y, frame.width, frame.height, sidebar, Style::surfaceRadius);
    std::string title = "Options";
    if (m_slashActiveOption) {
      const auto& option = m_slashCommand->options[*m_slashActiveOption];
      title = option.label + (option.required ? " · Required" : " · Optional");
    }
    label(
        root, frame.x + Style::spaceMd, frame.y + Style::spaceMd, frame.width - Style::spaceXl, title,
        Style::fontSizeDescription, muted, FontWeight::SemiBold
    );
    m_completionViewport = frame.height - Metrics::headerHeight - Metrics::footerHeight;
    const float full = std::max<std::size_t>(1, m_completionItems.size()) * Metrics::rowHeight;
    m_scrolling.limit(m_completionScroll, std::max(0.0F, full - m_completionViewport));
    auto list = std::make_unique<Node>();
    list->setPosition(frame.x + Style::spaceSm, frame.y + Metrics::headerHeight);
    list->setSize(frame.width - Style::spaceSm - Metrics::scrollbarGutter, m_completionViewport);
    list->setClipChildren(true);
    list->setTextScrollOffset(-m_completionScroll);
    for (std::size_t i = 0; i < m_completionItems.size(); ++i) {
      const auto& item = m_completionItems[i];
      const float top = i * Metrics::rowHeight - m_completionScroll;
      if (top + Metrics::rowHeight <= 0 || top >= m_completionViewport)
        continue;
      const bool over = m_completionBounds.contains(m_pointerX, m_pointerY)
          && hovered(list->x(), list->y() + top, list->width(), Metrics::rowHeight);
      if (over || (i == m_completionSelected && (m_slashActiveOption || m_slashOptionHighlighted)))
        rectangle(*list, 0, top, list->width(), Metrics::rowHeight - Style::spaceXs, raised, Style::controlRadius);
      if (item.kind == CompletionItem::SlashOption) {
        const float nameWidth = std::min(list->width() / 3, Style::spaceGroup * 3);
        label(
            *list, Style::spaceSm, top + Style::spaceLg, nameWidth - Style::spaceSm, item.name, Style::fontSizeBody,
            foreground
        );
        label(
            *list, nameWidth + Style::spaceSm, top + Style::spaceLg, list->width() - nameWidth - Style::spaceMd,
            item.detail, Style::fontSizeDescription, muted, FontWeight::Normal, 2
        );
      } else
        label(
            *list, Style::spaceSm, top + Style::spaceLg, list->width() - Style::spaceLg, item.name, Style::fontSizeBody,
            foreground
        );
      const float hitTop = std::max(0.0F, top), hitBottom = std::min(m_completionViewport, top + Metrics::rowHeight);
      m_hits.push_back(
          {list->x(), list->y() + hitTop, list->width(), hitBottom - hitTop, Action::CompleteText, std::to_string(i)}
      );
    }
    if (m_completionItems.empty()) {
      const auto hint =
          m_slashActiveOption ? m_slashCommand->options[*m_slashActiveOption].description : "No matching options";
      label(
          *list, Style::spaceSm, Style::spaceSm, list->width() - Style::spaceLg, hint, Style::fontSizeDescription,
          muted, FontWeight::Normal, 2
      );
    }
    root.addChild(std::move(list));
    if (full > m_completionViewport) {
      const std::string id = "completion-scrollbar";
      std::unique_ptr<Node> owned;
      if (auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
        owned = std::move(at->second);
        m_detachedInputs.erase(at);
      } else
        owned = std::make_unique<Scrollbar>();
      auto* bar = static_cast<Scrollbar*>(owned.get());
      m_widgets[id] = bar;
      bar->setOnScrollChanged([this](float offset) {
        m_scrolling.stop(m_completionScroll);
        m_completionScroll = offset;
        m_window.invalidate();
      });
      bar->setPosition(frame.x + frame.width - Style::spaceMd, frame.y + Metrics::headerHeight);
      bar->setSize(Style::spaceSm, m_completionViewport);
      bar->update(m_completionViewport, full, m_completionScroll);
      root.addChild(std::move(owned));
    }
    label(
        root, frame.x + Style::spaceMd, frame.y + frame.height - Metrics::footerHeight + Style::spaceXs,
        frame.width - Style::spaceXl,
        m_slashActiveOption && !m_completionItems.empty() ? "↑ ↓ Choose · Enter Select · Tab Next option · Esc Dismiss"
                                                          : "Tab Choose option · Enter Send · Esc Dismiss",
        Style::fontSizeMini, muted
    );
  }
} // namespace app
