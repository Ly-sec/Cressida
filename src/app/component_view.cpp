#include "app/client_application.h"
#include "app/theme.h"
#include "ui/controls/scrollbar.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace app {
  using namespace theme;
  float ClientApplication::messageButtons(Node* root, const api::Message& message, float x, float y, float width) {
    const bool ephemeral = (message.flags & 64) != 0;
    if (message.deleted || (message.buttons.empty() && !ephemeral) || blockedMessage(message))
      return 0;
    float rowY = 0;
    std::string feedback;
    for (std::size_t r = 0; r < message.buttons.size(); ++r) {
      float bx = 0;
      for (std::size_t c = 0; c < message.buttons[r].size(); ++c) {
        const auto& item = message.buttons[r][c];
        const std::string key = message.id + ":" + item.customId;
        const auto state = m_componentStates.find(key);
        const bool pending = state != m_componentStates.end() && state->second.pending;
        if (state != m_componentStates.end() && !state->second.status.empty())
          feedback = state->second.status;
        const bool emoji = !item.emojiId.empty() || !item.emojiName.empty();
        const float bw = std::min(
            width,
            std::max(
                36.0F,
                std::ceil(m_window.target()
                              .renderer()
                              .measureText(item.label, Style::fontSizeCaption, FontWeight::Medium)
                              .width)
                    + 26
                    + (emoji ? 24 : 0)
                    + (item.style == 5 ? 18 : 0)
            )
        );
        if (bx > 0 && bx + bw > width) {
          bx = 0;
          rowY += (Style::spaceSection - Style::spaceXxs);
        }
        if (root) {
          const bool disabled = item.disabled || pending || (item.style != 5 && message.applicationId.empty());
          const float px = conversationLeft() + Style::spaceXl + x + bx, py = m_messageViewportTop + y + rowY;
          const bool over = !disabled && hovered(px, py, bw, 32);
          auto color = item.style == 1 ? accent
              : item.style == 3        ? rgbHex(0x248046)
              : item.style == 4        ? rgbHex(0xDA373C)
                                       : panel;
          if (over)
            color = lerpColor(color, foreground, 0.12F);
          if (disabled)
            color = lerpColor(background, color, 0.45F);
          rectangle(
              *root, x + bx, y + rowY, bw, 32, item.style == 2 || item.style == 5 ? raised : color, Style::hoverRadius
          );
          rectangle(
              *root, x + bx + Style::borderWidth, y + rowY + Style::borderWidth, bw - Style::spaceXxs, 30, color,
              Style::radiusMd
          );
          const auto textColor = disabled          ? muted
              : item.style == 1                    ? ink
              : item.style == 3 || item.style == 4 ? rgbHex(0xFFFFFF)
                                                   : foreground;
          float textX = x + bx + Style::spaceMd;
          if (!item.emojiId.empty()) {
            emojiImage(*root, textX, y + rowY + Style::spaceCompact, 20, api::emojiPath(item.emojiId, item.animated));
            textX += (Style::spaceXl + Style::borderWidth);
          } else if (!item.emojiName.empty()) {
            label(*root, textX, y + rowY + Style::spaceXs, 26, item.emojiName, Style::fontSizeHeader, textColor);
            textX += (Style::spaceXl + Style::borderWidth);
          }
          const auto metrics =
              m_window.target().renderer().measureText(item.label, Style::fontSizeCaption, FontWeight::Medium);
          label(
              *root, textX, y + rowY + (32 - (metrics.bottom - metrics.top)) / 2,
              std::max(1.0F, bw - (textX - x - bx) - Style::spaceDense - (item.style == 5 ? 16 : 0)), item.label,
              Style::fontSizeCaption, textColor, FontWeight::Medium
          );
          if (item.style == 5)
            glyph(*root, x + bx + bw - 23, y + rowY + Style::spaceDense, 13, "external-link", textColor);
          if (!disabled)
            m_hits.push_back(
                {px, py, bw, 32, Action::MessageButton, message.id + "\n" + std::to_string(r) + ":" + std::to_string(c)}
            );
        }
        bx += bw + 7;
      }
      rowY += (Style::spaceSection - Style::spaceXxs);
    }
    if (!feedback.empty()) {
      if (root)
        label(
            *root, x, y + rowY + Style::borderWidth, width, feedback, Style::fontSizeMini, muted, FontWeight::Normal, 2
        );
      rowY += Style::spaceWide;
    }
    if (ephemeral) {
      if (root) {
        glyph(*root, x, y + rowY + (Style::spaceXs + Style::borderWidth), Style::glyphSizeSmall, "eye", muted);
        constexpr std::string_view caption = "Only you can see this · ";
        const float captionWidth =
            m_window.target().renderer().measureText(std::string(caption), Style::fontSizeMini).width;
        label(
            *root, x + Style::spaceRelaxed, y + rowY + (Style::spaceXs + Style::borderWidth),
            captionWidth + Style::spaceXxs, std::string(caption), Style::fontSizeMini, muted
        );
        const float dismissWidth =
            m_window.target().renderer().measureText("Dismiss message", Style::fontSizeMini).width;
        label(
            *root, x + Style::spaceRelaxed + captionWidth, y + rowY + (Style::spaceXs + Style::borderWidth),
            dismissWidth + Style::spaceXxs, "Dismiss message", Style::fontSizeMini, accent
        );
        m_hits.push_back(
            {conversationLeft() + Style::spaceXl + x + Style::spaceRelaxed + captionWidth,
             m_messageViewportTop + y + rowY, dismissWidth + Style::spaceXxs, 25, Action::DismissEphemeral, message.id}
        );
      }
      rowY += Style::spaceWide;
    }
    return rowY;
  }
  void ClientApplication::componentResult(const api::Event& event) {
    if (event.operation == api::Operation::Interaction && event.interaction) {
      const auto& action = *event.interaction;
      if (action.kind == api::InteractionKind::SubmitModal) {
        if (m_dialogKind == "application-modal" && m_modalNonce == action.nonce && !event.error.empty()) {
          m_dialogPending = false;
          m_dialogError = event.error;
        }
        m_window.invalidate();
        return;
      }
      const auto state = m_componentStates.find(action.message + ":" + action.customId);
      if (state == m_componentStates.end() || state->second.nonce != action.nonce)
        return;
      auto& result = state->second;
      if (!event.error.empty())
        result.status = event.error;
      else if (result.pending)
        result.status.clear();
      result.pending = false;
    } else if (!event.nonce.empty()) {
      if (m_dialogKind == "application-modal" && m_modalNonce == event.nonce) {
        if (event.error.empty())
          closeDialog();
        else {
          m_dialogPending = false;
          m_dialogError = event.error;
        }
      }
      for (auto& [key, state] : m_componentStates)
        if (state.nonce == event.nonce) {
          state.pending = false;
          if (!event.error.empty())
            state.status = event.error;
          else if (state.status == "Sending…")
            state.status.clear();
          if (event.modal && event.modal->channel == m_channelId && !settingsVisible()) {
            const auto source = std::ranges::find(m_messages, key.substr(0, key.find(':')), &api::Message::id);
            if (source != m_messages.end() && source->applicationId == event.modal->application) {
              closeDialog();
              closeContext();
              closeGifPicker();
              m_emojiMenu = m_presenceMenu = false;
              m_scrolling.stopAll();
              m_channelDialogScroll = 0;
              m_applicationModal = event.modal;
              m_applicationValues.clear();
              for (const auto& field : event.modal->fields)
                m_applicationValues.push_back(field.value);
              m_dialogKind = "application-modal";
              m_focusedInput = "application-field:0";
            }
          }
          break;
        }
    }
    m_window.invalidate();
  }
  void ClientApplication::dismissEphemeral(const std::string& id) {
    const auto message = std::ranges::find(m_messages, id, &api::Message::id);
    if (message == m_messages.end() || !(message->flags & 64))
      return;
    m_dismissedEphemeral.insert(id);
    while (m_dismissedEphemeral.size() > 4096)
      m_dismissedEphemeral.erase(m_dismissedEphemeral.begin());
    m_liveChanges[id] = std::nullopt;
    m_messageLayouts.erase(id);
    m_messages.erase(message);
    if (auto cache = m_channelMessages.find(m_channelId); cache != m_channelMessages.end())
      std::erase_if(cache->second, [&](const auto& row) { return row.id == id; });
    std::erase_if(m_componentStates, [&](const auto& item) { return item.first.starts_with(id + ":"); });
    m_window.invalidate();
  }
  void ClientApplication::submitApplicationModal() {
    if (!m_applicationModal || m_dialogPending || m_applicationModal->channel != m_channelId)
      return;
    try {
      (void)api::modalSubmission(*m_applicationModal, m_applicationValues);
    } catch (const std::exception& error) {
      m_dialogError = error.what();
      m_window.invalidate();
      return;
    }
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const auto nonce = std::to_string(
        (static_cast<std::uint64_t>(milliseconds - 1420070400000LL) << 22) | (++m_channelFeatureSerial & 0x3FFFFF)
    );
    api::Interaction action{
        .kind = api::InteractionKind::SubmitModal,
        .channel = m_channelId,
        .guild = m_dmMode ? "" : m_guildId,
        .application = m_applicationModal->application,
        .nonce = nonce,
        .modal = m_applicationModal,
        .values = m_applicationValues
    };
    if (startInteraction(action)) {
      m_modalNonce = nonce;
      m_dialogPending = true;
      m_dialogError.clear();
    } else
      m_dialogError = m_status;
    m_window.invalidate();
  }
  void ClientApplication::drawApplicationModal(Node& root, float w, float h) {
    if (!m_applicationModal)
      return;
    float full = 0;
    for (const auto& field : m_applicationModal->fields)
      full += (field.multiline ? 146 : 82) + (field.description.empty() ? 0 : 38);
    const float width = std::min(540.0F, w - Style::space2Xl), height = std::min(full + 180, h - 72);
    const float x = (w - width) / 2, y = std::max(44.0F, (h - height) / 2);
    rectangle(root, 0, 36, w, h - layout::Window::titlebarHeight, Color{0, 0, 0, .65F});
    m_hits.push_back({0, 36, w, h - layout::Window::titlebarHeight, Action::None, {}});
    rectangle(root, x, y, width, height, sidebar, 14);
    m_dialogBounds = {x, y, width, height, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    label(
        root, x + Style::spaceXl, y + Style::spaceXl, width - 85, m_applicationModal->title, Style::fontSizeProfile,
        foreground, FontWeight::SemiBold
    );
    iconButton(root, {x + width - 44, y + Style::spaceLg, 30, 30, Action::CloseDialog, {}}, "x", "Close");
    const float top = y + 73, viewport = height - 180;
    m_scrolling.limit(m_channelDialogScroll, std::max(0.0F, full - viewport));
    auto content = std::make_unique<Node>();
    content->setPosition(x + Style::spaceXl, top);
    content->setSize(width - Style::spaceGroup, viewport);
    content->setClipChildren(true);
    content->setTextScrollOffset(-m_channelDialogScroll);
    float fieldY = -m_channelDialogScroll;
    for (std::size_t i = 0; i < m_applicationModal->fields.size(); ++i) {
      const auto& field = m_applicationModal->fields[i];
      label(
          *content, 0, fieldY, width - 70, field.label + (field.required ? " *" : " (optional)"),
          Style::fontSizeDescription, foreground, FontWeight::SemiBold
      );
      float inputY = fieldY + (Style::spaceXl + Style::borderWidth);
      if (!field.description.empty()) {
        label(*content, 0, inputY, width - 70, field.description, Style::fontSizeMini, muted, FontWeight::Normal, 2);
        inputY += (Style::spaceSection - Style::spaceXxs);
      }
      inputControl(
          *content, 0, inputY, width - 64, field.multiline ? 104 : 40, "application-field:" + std::to_string(i),
          m_applicationValues[i], field.placeholder, field.multiline
      )
          .setEnabled(!m_dialogPending);
      fieldY += (field.multiline ? 146 : 82) + (field.description.empty() ? 0 : 38);
    }
    root.addChild(std::move(content));
    if (full > viewport) {
      const std::string id = "application-modal-scrollbar";
      std::unique_ptr<Node> owned;
      if (auto old = m_detachedInputs.find(id); old != m_detachedInputs.end()) {
        owned = std::move(old->second);
        m_detachedInputs.erase(old);
      } else {
        auto bar = std::make_unique<Scrollbar>();
        bar->setOnScrollChanged([this](float v) {
          m_scrolling.stop(m_channelDialogScroll);
          m_channelDialogScroll = v;
          m_window.invalidate();
        });
        owned = std::move(bar);
      }
      auto* bar = static_cast<Scrollbar*>(owned.get());
      m_widgets[id] = bar;
      bar->setPosition(x + width - 22, top);
      bar->setSize(bar->reservedThickness(), viewport);
      bar->update(viewport, full, m_channelDialogScroll);
      root.addChild(std::move(owned));
    }
    label(
        root, x + Style::spaceXl, y + height - 96, width - Style::spaceGroup, m_dialogError, Style::fontSizeDescription,
        rgbHex(0xED4245), FontWeight::Normal, 2
    );
    button(root, {x + Style::spaceXl, y + height - 50, 85, 32, Action::CloseDialog, {}}, "Cancel");
    button(
        root,
        {x + width - 142,
         y + height - 50,
         118,
         32,
         m_dialogPending ? Action::None : Action::SubmitApplicationModal,
         {}},
        m_dialogPending ? "Submitting…" : "Submit", true
    );
  }
} // namespace app
