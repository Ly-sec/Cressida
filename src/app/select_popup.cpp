#include "app/client_application.h"
#include "app/theme.h"
#include "ui/controls/select.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) {
    closeSelectDropdown();
    if (request.options.empty()) {
      if (callbacks.onDismiss)
        callbacks.onDismiss();
      return;
    }
    m_selectOwner.clear();
    for (const auto& [id, widget] : m_widgets)
      if (const auto* select = dynamic_cast<const Select*>(widget); select && select->open()) {
        m_selectOwner = id;
        break;
      }
    m_selectRequest = request;
    m_selectCallbacks = std::move(callbacks);
    m_selectDirty = true;
    m_selectScroll = 0;
    m_scrolling.stopAll();
    m_controlInput.cancelPointerCapture();
    m_window.invalidate();
  }
  void ClientApplication::closeSelectDropdown() {
    if (!m_selectRequest)
      return;
    auto callbacks = std::move(m_selectCallbacks);
    m_selectRequest.reset();
    m_selectCallbacks = {};
    m_scrolling.stop(m_selectScroll);
    if (callbacks.onDismiss)
      callbacks.onDismiss();
    m_window.invalidate();
  }
  void ClientApplication::selectDropdownChoose(std::size_t index) {
    if (!m_selectRequest || index >= m_selectRequest->options.size())
      return;
    auto callbacks = std::move(m_selectCallbacks);
    m_selectRequest.reset();
    m_selectCallbacks = {};
    m_scrolling.stop(m_selectScroll);
    if (callbacks.onSelect)
      callbacks.onSelect(index);
    m_window.invalidate();
  }
  void ClientApplication::selectDropdownReveal() {
    if (!m_selectRequest || !m_selectControl)
      return;
    const auto index = m_selectControl->highlightedIndex();
    const float top = m_selectControl->rowTop(index), bottom = m_selectControl->rowBottom(index);
    m_scrolling.stop(m_selectScroll);
    if (top < m_selectScroll)
      m_selectScroll = top;
    if (bottom > m_selectScroll + m_selectBounds.height)
      m_selectScroll = bottom - m_selectBounds.height;
    m_selectScroll = std::clamp(m_selectScroll, 0.0F, std::max(0.0F, m_selectContent - m_selectBounds.height));
  }
  void ClientApplication::drawSelectDropdown(Node& root, float w, float h) {
    if (!m_selectRequest)
      return;
    const auto owner = m_widgets.find(m_selectOwner);
    if (owner == m_widgets.end() || !owner->second->parent()) {
      closeSelectDropdown();
      return;
    }
    std::unique_ptr<Node> saved;
    if (const auto at = m_detachedInputs.find("select-popup"); at != m_detachedInputs.end()) {
      saved = std::move(at->second);
      m_detachedInputs.erase(at);
    } else {
      auto menu = std::make_unique<ContextMenuControl>();
      menu->setRedrawCallback([this] { m_window.invalidate(); });
      menu->setOnActivate([this](const auto& entry) { selectDropdownChoose(static_cast<std::size_t>(entry.id)); });
      m_selectControl = menu.get();
      m_widgets["select-popup"] = menu.get();
      saved = std::move(menu);
      m_selectDirty = true;
    }
    auto& request = *m_selectRequest;
    float left = 0, top = 0, right = 0, bottom = 0;
    Node::transformedBounds(owner->second, left, top, right, bottom);
    request.anchorX = static_cast<std::int32_t>(std::round(left));
    request.anchorY = static_cast<std::int32_t>(std::round(top));
    request.anchorWidth = static_cast<std::int32_t>(std::round(right - left));
    request.anchorHeight = static_cast<std::int32_t>(std::round(bottom - top));
    request.menuWidth = right - left;
    std::vector<ContextMenuControlEntry> entries;
    for (std::size_t i = 0; i < request.options.size(); ++i) {
      ContextMenuControlEntry entry{static_cast<int>(i), request.options[i]};
      entry.checkmark = true;
      entry.toggleState = i == request.selectedIndex ? 1 : 0;
      if (i < request.indicatorColors.size())
        entry.indicatorColor = request.indicatorColors[i];
      if (i < request.optionSwatchPreviews.size())
        entry.swatchPreview = request.optionSwatchPreviews[i];
      entries.push_back(std::move(entry));
    }
    const float scale = request.fontSize > 0 ? request.fontSize / Style::fontSizeCaption : 1;
    m_selectControl->setContentScale(scale);
    if (m_selectDirty) {
      m_selectControl->setMaxVisible(entries.size());
      m_selectControl->setEntries(entries);
    }
    const float minimum = std::min(std::max(120.0F, request.menuWidth), w - Style::spaceLg);
    const float width = std::clamp(
        ContextMenuControl::preferredWidth(m_window.target().renderer(), entries, scale), minimum,
        std::max(minimum, std::min(request.maxMenuWidth, w - Style::spaceLg))
    );
    if (m_selectControl->width() != width)
      m_selectControl->setMenuWidth(width);
    m_selectControl->layout(m_window.target().renderer());
    m_selectContent = m_selectControl->preferredHeight();
    const float height = std::min(
        {m_selectContent, h - 64, ContextMenuControl::preferredHeight(entries, request.maxVisibleOptions, scale)}
    );
    const float x = std::clamp(static_cast<float>(request.anchorX), 8.0F, std::max(8.0F, w - width - 8));
    const float anchorTop = request.anchorY + layout::Window::titlebarHeight - m_window.titlebarHeight();
    const float below = anchorTop + request.anchorHeight + Style::spaceXs;
    const float y = std::clamp(
        below + height < h - Style::spaceSm ? below : anchorTop - height - 4, 40.0F, std::max(40.0F, h - height - 8)
    );
    m_selectBounds = {x, y, width, height, Action::None, {}};
    if (m_selectDirty) {
      m_selectControl->setHighlightedIndex(request.selectedIndex < request.options.size() ? request.selectedIndex : 0);
      selectDropdownReveal();
      m_selectDirty = false;
    }
    m_scrolling.limit(m_selectScroll, std::max(0.0F, m_selectContent - height));
    rectangle(
        root, x - Style::borderWidth, y - Style::borderWidth, width + Style::spaceXxs, height + Style::spaceXxs,
        theme::raised, Style::hoverRadius
    );
    rectangle(root, x, y, width, height, theme::panel, Style::radiusMd);
    auto viewport = std::make_unique<Node>();
    viewport->setPosition(x, y);
    viewport->setSize(width, height);
    viewport->setClipChildren(true);
    saved->setPosition(0, -m_selectScroll);
    viewport->addChild(std::move(saved));
    root.addChild(std::move(viewport));
    if (m_selectContent > height)
      settingsScrollbar(
          root, "select-popup-scrollbar", {x + width - 7, y, 5, height, Action::None, {}}, m_selectContent,
          m_selectScroll
      );
  }
  bool ClientApplication::selectDropdownClick(float x, float y) {
    if (!m_selectRequest)
      return false;
    if (m_selectBounds.contains(x, y))
      m_controlInput.pointerButton(
          x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
      );
    else
      closeSelectDropdown();
    return true;
  }
  bool ClientApplication::selectDropdownKey(const Key& key) {
    if (!m_selectRequest)
      return false;
    if (key.control && (key.symbol == XKB_KEY_q || key.symbol == XKB_KEY_Q))
      return false;
    if (key.symbol == XKB_KEY_Escape || key.symbol == XKB_KEY_Tab)
      closeSelectDropdown();
    else if (m_selectControl && !m_selectDirty) {
      if (key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter || key.symbol == XKB_KEY_space)
        (void)m_selectControl->activateHighlighted();
      else if (key.symbol == XKB_KEY_Up || key.symbol == XKB_KEY_Down) {
        (void)m_selectControl->moveHighlight(key.symbol == XKB_KEY_Up ? -1 : 1);
        selectDropdownReveal();
      } else if (key.symbol == XKB_KEY_Home || key.symbol == XKB_KEY_End) {
        m_selectControl->setHighlightedIndex(key.symbol == XKB_KEY_Home ? 0 : m_selectRequest->options.size() - 1);
        selectDropdownReveal();
      }
    }
    m_window.invalidate();
    return true;
  }
} // namespace app
