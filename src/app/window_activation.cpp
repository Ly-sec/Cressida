#include "app/window_activation.h"

#include "xdg-activation-v1-client-protocol.h"

#include <algorithm>

namespace app {
  namespace {
    constexpr std::size_t maximumExports = 8;
    constexpr auto exportDeadline = std::chrono::seconds(1);
  } // namespace
  WindowActivation::~WindowActivation() { unmap(); }
  void WindowActivation::bind(xdg_activation_v1* manager, wl_surface* surface) {
    unmap();
    m_manager = manager;
    m_surface = surface;
  }
  void WindowActivation::cancelRequest() {
    m_deadline.stop();
    if (m_request) {
      xdg_activation_token_v1_destroy(m_request);
      m_request = nullptr;
    }
  }
  void WindowActivation::activateMapped() {
    if (m_manager && m_presented) {
      const auto token = m_token.take();
      if (!token.empty())
        xdg_activation_v1_activate(m_manager, token.c_str(), m_surface);
    }
  }
  void WindowActivation::presented() {
    m_presented = true;
    activateMapped();
  }
  void WindowActivation::unmap() {
    m_presented = false;
    cancelRequest();
    m_token.clear();
    for (auto& request : m_exports)
      xdg_activation_token_v1_destroy(request.request);
    m_exports.clear();
  }
  void WindowActivation::finishExport(Export* request, std::string token) {
    const auto at =
        std::find_if(m_exports.begin(), m_exports.end(), [request](auto& value) { return &value == request; });
    if (at == m_exports.end())
      return;
    auto ready = std::move(at->ready);
    xdg_activation_token_v1_destroy(at->request);
    m_exports.erase(at);
    DesktopActivationToken checked;
    checked.assign(token);
    ready(checked.take());
  }
  bool WindowActivation::exportToken(wl_seat* seat, std::uint32_t serial, std::function<void(std::string)> ready) {
    if (m_exports.size() >= maximumExports)
      return false;
    if (!m_manager || !m_surface || !seat || !serial) {
      ready({});
      return true;
    }
    auto* request = xdg_activation_v1_get_activation_token(m_manager);
    auto& pending = m_exports.emplace_back(this, request, std::move(ready), Timer{});
    static const xdg_activation_token_v1_listener listener{
        .done = [](void* data, xdg_activation_token_v1*, const char* token) {
          auto* pending = static_cast<Export*>(data);
          pending->owner->finishExport(pending, token);
        }
    };
    xdg_activation_token_v1_add_listener(request, &listener, &pending);
    xdg_activation_token_v1_set_surface(request, m_surface);
    xdg_activation_token_v1_set_serial(request, serial, seat);
    // The destination is the user's selected browser, whose app ID is unknown.
    pending.deadline.start(exportDeadline, [this, pending = &pending] { finishExport(pending, {}); });
    xdg_activation_token_v1_commit(request);
    return true;
  }
  void WindowActivation::request(std::string_view token) {
    if (!m_manager || !m_surface)
      return;
    if (!token.empty() && m_token.assign(token)) {
      cancelRequest();
      activateMapped();
      return;
    }
    if (m_request || m_token.pending())
      return;
    m_request = xdg_activation_v1_get_activation_token(m_manager);
    static const xdg_activation_token_v1_listener listener{
        .done = [](void* data, xdg_activation_token_v1* request, const char* value) {
          auto& self = *static_cast<WindowActivation*>(data);
          if (request != self.m_request)
            return;
          self.m_token.assign(value);
          self.cancelRequest();
          self.activateMapped();
        }
    };
    xdg_activation_token_v1_add_listener(m_request, &listener, this);
    xdg_activation_token_v1_set_app_id(m_request, desktopApplicationId);
    xdg_activation_token_v1_set_surface(m_request, m_surface);
    // The external desktop click has no input serial on our seat. Never reuse
    // an unrelated old event as evidence of that interaction.
    m_deadline.start(std::chrono::duration_cast<std::chrono::milliseconds>(DesktopActivationToken::lifetime), [this] {
      cancelRequest();
    });
    xdg_activation_token_v1_commit(m_request);
  }
} // namespace app
