#include "app/client_application.h"
#include "app/theme.h"
#include "core/files/resource_paths.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "render/scene/text_node.h"
#include "ui/controls/button.h"
#include "ui/controls/select.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;

  void ClientApplication::loginButton(
      Node& parent, Hit hit, const std::string& text, const std::string& id, const std::string& icon, bool primary,
      bool quiet
  ) {
    std::unique_ptr<Node> saved;
    if (auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
      saved = std::move(at->second);
      m_detachedInputs.erase(at);
    }
    auto control = saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
    m_widgets[id] = control.get();
    control->setText(text);
    control->setGlyph(icon);
    control->setFontSize(Style::fontSizeBody);
    control->setGlyphSize(Style::baseGlyphSize);
    control->setVariant(primary ? ButtonVariant::Primary : quiet ? ButtonVariant::Ghost : ButtonVariant::Outline);
    control->setPosition(hit.x, hit.y);
    control->setControlHeight(hit.height);
    control->setPadding(0, Style::spaceMd);
    control->setFrameSize(hit.width, hit.height);
    control->setRadius(Style::controlRadius);
    control->setEnabled(hit.action != Action::None);
    control->setOnClick([this, hit] { activate(hit); });
    parent.addChild(std::move(control));
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      hit.x += node->x();
      hit.y += node->y();
    }
    m_hits.push_back(hit);
  }

  void ClientApplication::clearQrLogin() {
    m_qrLogin.cancel();
    m_qrLoginState.reset();
    m_loginQr.reset();
  }

  void ClientApplication::beginQrLogin() {
    if (m_connected || m_connecting)
      return;
    m_qrAutoStartPending = false;
    if (m_demo) {
      m_status = "The offline preview does not start sign-in requests.";
      m_window.invalidate();
      return;
    }
    clearQrLogin();
    m_login.cancel();
    m_browserLoginWaiting = false;
    m_showTokenEntry = false;
    m_fieldFocused = false;
    m_controlInput.setFocus(nullptr);
    m_status.clear();
    m_loginAnimationStart = std::chrono::steady_clock::now();
    m_qrLoginState = auth::QrUpdate{};
    if (!m_qrLogin.start())
      applyQrLoginUpdate(
          {.stage = auth::QrStage::Failed, .error = "Could not start QR sign-in. Try your browser instead."}
      );
    m_window.invalidate();
  }

  void ClientApplication::revealLoginFocus() {
    if (m_connected || !m_scene)
      return;
    const auto* area = m_controlInput.focusedArea();
    if (!area)
      return;
    float left = 0, top = 0, right = 0, bottom = 0;
    Node::transformedBounds(area, left, top, right, bottom);
    top -= m_scene->y();
    bottom -= m_scene->y();
    const float viewportTop = layout::Window::titlebarHeight + Style::spaceLg;
    const float viewportBottom = m_scene->height() - Style::spaceLg;
    if (top < viewportTop)
      m_loginScroll += top - viewportTop;
    else if (bottom > viewportBottom)
      m_loginScroll += bottom - viewportBottom;
    m_loginScroll = std::clamp(m_loginScroll, 0.0F, m_loginMaxScroll);
    m_scrolling.stop(m_loginScroll);
  }

  void ClientApplication::applyQrLoginUpdate(auth::QrUpdate update) {
    m_loginQr.reset();
    if (update.stage == auth::QrStage::Ready) {
      m_loginQr = encodeQrCode(update.qrUrl);
      if (!m_loginQr) {
        m_qrLogin.cancel();
        update = {.stage = auth::QrStage::Failed, .error = "Could not display the QR code. Try your browser instead."};
      }
    }
    // Keep transient connection material out of the retained view model.
    update.qrUrl.clear();
    m_status = update.error;
    m_qrLoginState = std::move(update);
    m_window.invalidate();
  }

  void ClientApplication::drawLoginLogo(Node& parent, const layout::Frame& frame, bool rotating, bool pulsing) {
    const float elapsed =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_loginAnimationStart).count();
    const float phase = elapsed / (Style::animSlow * 6) * std::numbers::pi_v<float> * 2;
    const float scale = pulsing && !m_preferences.reducedMotion ? 1 + std::sin(phase) * Style::signInLoadingPulse : 1;
    const float side = std::min(frame.width, frame.height);
    const int pixels = std::max(1, static_cast<int>(std::ceil(side * m_window.target().contentScale())));
    const std::string key = "builtin:signin-logo:" + std::to_string(pixels);
    auto& cached = m_images[key];
    cached.uses |= imageUseMask(ImageUse::Content);
    cached.touched = ++m_imageSerial;
    cached.visibleDraw = m_animationDraw;
    if (!cached.texture.valid())
      cached.texture =
          m_window.context().textureManager().loadFromFile(paths::assetPath("cressida-logo.svg").string(), pixels);
    auto mark = std::make_unique<Node>();
    mark->setPosition(frame.x + frame.width / 2, frame.y + frame.height / 2);
    mark->setSize(side, side);
    mark->setTransformOrigin(0, 0);
    mark->setRotation(rotating && !m_preferences.reducedMotion ? phase : 0);
    mark->setScale(scale);
    if (!picture(*mark, -side / 2, -side / 2, side, side, key, 0, false, false, ImageFitMode::Contain))
      glyph(*mark, -side / 2, -side / 2, side, "moon-stars", accent);
    parent.addChild(std::move(mark));
  }

  void ClientApplication::drawStartupLoading(Node& root, float width, float height) {
    const float compensation = m_startupReferenceScale / m_window.uiScale();
    const auto frame = layout::startupFrame(
        m_window.target().renderWidth() / compensation, m_window.target().renderHeight() / compensation
    );
    auto presentation = std::make_unique<Node>();
    presentation->setPosition(-m_scene->x(), -m_scene->y());
    presentation->setScale(compensation);
    drawLoginLogo(*presentation, frame.logo, true);
    root.addChild(std::move(presentation));
    m_hits.push_back({0, layout::Window::titlebarHeight, width, height - layout::Window::titlebarHeight, Action::None});
  }

  void ClientApplication::drawLogin(Node& root, float width, float height) {
    using Stage = auth::QrStage;
    const bool loading = m_restoringLogin || m_connecting;
    if (loading) {
      drawStartupLoading(root, width, height);
      return;
    }
    const bool browserWaiting = m_browserLoginWaiting;
    const auto stage = m_qrLoginState ? m_qrLoginState->stage : Stage::Cancelled;
    const bool qrWorking = (!m_qrLoginState && m_qrAutoStartPending)
        || (m_qrLoginState && (stage == Stage::Starting || stage == Stage::Completing));
    const bool qrReady = m_loginQr && stage == Stage::Ready;
    const bool qrScanned = m_qrLoginState && stage == Stage::AwaitingApproval;
    const bool expired = m_qrLoginState && stage == Stage::Expired;
    const bool error = !m_status.empty() && !loading && !browserWaiting && !qrWorking && !qrReady && !qrScanned;
    auto& renderer = m_window.target().renderer();
    const float cardWidth = std::min(layout::SignIn::maximumWidth, width - Style::spaceLg * 2);
    const float inner = std::max(Style::space2Xl, cardWidth - layout::SignIn::padding * 2);
    const float detailWidth = inner;
    const bool showRetry = !browserWaiting && !qrWorking && !qrReady && !qrScanned;
    const std::string qrHeading = browserWaiting ? "Continue in your browser"
        : qrScanned                              ? "Approve on your phone"
        : qrReady                                ? "Scan to sign in"
        : stage == Stage::Completing             ? "Finishing sign-in"
        : qrWorking                              ? "Preparing your sign-in"
        : expired                                ? "This code has expired"
        : stage == Stage::Failed                 ? "Could not load the code"
                                                 : "QR sign-in is paused";
    const std::string qrDescription = browserWaiting ? "Finish signing in on the page that opened, then return here."
        : qrScanned && m_qrLoginState->account
        ? "Signing in as " + m_qrLoginState->account->name + ". Confirm this sign-in in your mobile app."
        : qrReady                    ? "Scan with your mobile app, then approve sign-in on your phone."
        : stage == Stage::Completing ? "Approval received. Completing your connection."
        : qrWorking                  ? "Getting a fresh, short-lived code for this device."
        : expired                    ? "Generate a new code when you are ready to scan."
                                     : "Try a fresh code, or continue in your browser below.";
    const auto headingMetrics =
        renderer.measureText(qrHeading, Style::fontSizeHeader, FontWeight::SemiBold, detailWidth, 0);
    const float headingHeight = std::ceil(headingMetrics.bottom - headingMetrics.top);
    const auto detailMetrics =
        renderer.measureText(qrDescription, Style::fontSizeBody, FontWeight::Normal, detailWidth, 0);
    const float detailHeight = headingHeight
        + std::ceil(detailMetrics.bottom - detailMetrics.top)
        + layout::SignIn::detailGap
        + (showRetry ? layout::SignIn::detailGap + layout::SignIn::actionHeight : 0);
    const std::string status = loading ? std::string{} : m_status;
    const auto statusMetrics = renderer.measureText(status, Style::fontSizeDescription, FontWeight::Normal, inner, 0);
    const float statusHeight = (status.empty() ? 0 : std::ceil(statusMetrics.bottom - statusMetrics.top))
        + (m_retrySavedLogin && !loading ? layout::SignIn::footerHeight + layout::SignIn::detailGap : 0);
    const auto frames = layout::signInFrame(width, height, m_showTokenEntry, loading, statusHeight, detailHeight);
    m_loginMaxScroll = frames.maximumScroll;
    m_loginScroll = std::clamp(m_loginScroll, 0.0F, m_loginMaxScroll);
    m_scrolling.limit(m_loginScroll, m_loginMaxScroll);
    const auto firstHit = m_hits.size();
    auto viewport = std::make_unique<Node>();
    viewport->setPosition(frames.viewport.x, frames.viewport.y);
    viewport->setSize(frames.viewport.width, frames.viewport.height);
    viewport->setClipChildren(true);
    auto ownedCard = std::make_unique<Node>();
    auto* card = ownedCard.get();
    card->setPosition(frames.card.x, frames.card.y - m_loginScroll);
    card->setSize(frames.card.width, frames.card.height);
    viewport->addChild(std::move(ownedCard));
    root.addChild(std::move(viewport));
    rectangle(*card, 0, 0, frames.card.width, frames.card.height, sidebar, Style::radiusXl);

    const float headingX = frames.brand.x;
    label(
        *card, headingX, frames.brand.y, frames.brand.width, "Cressida", Style::fontSizeWelcome, foreground,
        FontWeight::SemiBold
    );
    static_cast<TextNode*>(card->children().back().get())->setTextAlign(TextAlign::Center);

    if (m_showTokenEntry) {
      const auto& area = frames.token;
      label(*card, area.x, area.y, area.width, "Account token", Style::fontSizeBody, foreground, FontWeight::Medium);
      label(
          *card, area.x, area.y + Style::spaceXl, area.width, "Treat this like a password. Never share it.",
          Style::fontSizeDescription, muted, FontWeight::Normal, 2
      );
      inputControl(
          *card, area.x, area.y + Style::spaceGroup, area.width, layout::SignIn::tokenHeight, "token", m_token,
          "Paste your account token", false, true
      );
      const float backWidth =
          std::min(area.width / 2, renderer.measureText("Back", Style::fontSizeBody).width + Style::spaceGroup);
      loginButton(
          *card,
          {frames.footer.x, frames.footer.y, frames.footer.width - backWidth - Style::spaceMd, frames.footer.height,
           Action::Connect},
          "Connect account", "login:connect", "login", true
      );
      loginButton(
          *card,
          {frames.footer.x + frames.footer.width - backWidth, frames.footer.y, backWidth, frames.footer.height,
           Action::ToggleToken},
          "Back", "login:back", "arrow-left", false, true
      );
    } else {
      const auto& tile = frames.qr;
      if (!qrReady)
        rectangle(*card, tile.x, tile.y, tile.width, tile.height, panel, Style::surfaceRadius);
      if (qrReady) {
        const auto& code = *m_loginQr;
        const int total = code.side + QrCode::quietZoneModules * 2;
        const float deviceScale = m_window.target().contentScale();
        const float module = std::max(1.0F, std::floor(tile.width * deviceScale / total)) / deviceScale;
        const float size = total * module;
        const float originX = frames.viewport.x + frames.card.x;
        const float originY = m_scene->y() + frames.viewport.y + frames.card.y - m_loginScroll;
        const float x = std::round((originX + tile.x + (tile.width - size) / 2) * deviceScale) / deviceScale - originX;
        const float y = std::round((originY + tile.y + (tile.height - size) / 2) * deviceScale) / deviceScale - originY;
        rectangle(*card, x, y, size, size, rgbHex(Style::qrCodeLightRgb));
        for (int row = 0; row < code.side; ++row) {
          int column = 0;
          while (column < code.side) {
            if (!code.at(column, row)) {
              ++column;
              continue;
            }
            const int first = column;
            while (column < code.side && code.at(column, row))
              ++column;
            rectangle(
                *card, x + (first + QrCode::quietZoneModules) * module, y + (row + QrCode::quietZoneModules) * module,
                (column - first) * module, module, rgbHex(Style::qrCodeDarkRgb)
            );
          }
        }
      } else {
        const float size = tile.width / 2;
        drawLoginLogo(
            *card, {tile.x + (tile.width - size) / 2, tile.y + (tile.height - size) / 2, size, size}, false,
            qrWorking || browserWaiting
        );
      }
      const auto& text = frames.details;
      label(*card, text.x, text.y, text.width, qrHeading, Style::fontSizeHeader, foreground, FontWeight::SemiBold, 0);
      static_cast<TextNode*>(card->children().back().get())->setTextAlign(TextAlign::Center);
      label(
          *card, text.x, text.y + headingHeight + layout::SignIn::detailGap, text.width, qrDescription,
          Style::fontSizeBody, muted, FontWeight::Normal, 0
      );
      static_cast<TextNode*>(card->children().back().get())->setTextAlign(TextAlign::Center);
      if (showRetry) {
        const std::string caption = "Generate new code";
        const float actionWidth =
            std::min(text.width, renderer.measureText(caption, Style::fontSizeBody).width + Style::spaceGroup);
        loginButton(
            *card,
            {text.x + (text.width - actionWidth) / 2, text.y + text.height - layout::SignIn::actionHeight, actionWidth,
             layout::SignIn::actionHeight, Action::QrLogin},
            caption, "login:qr", "refresh", true
        );
      }
      const auto& separator = frames.separator;
      const float captionWidth = renderer.measureText("or", Style::fontSizeDescription).width + Style::spaceLg * 2;
      const float line = std::max(0.0F, (separator.width - captionWidth) / 2);
      rectangle(*card, separator.x, separator.y + separator.height / 2, line, Style::borderWidth, raised);
      rectangle(
          *card, separator.x + separator.width - line, separator.y + separator.height / 2, line, Style::borderWidth,
          raised
      );
      label(
          *card, separator.x + line, separator.y + Style::spaceSm, captionWidth, "or", Style::fontSizeDescription, muted
      );
      static_cast<TextNode*>(card->children().back().get())->setTextAlign(TextAlign::Center);
      loginButton(
          *card,
          {frames.browser.x, frames.browser.y, frames.browser.width, frames.browser.height,
           browserWaiting ? Action::None : Action::DiscordLogin},
          browserWaiting ? "Waiting for your browser…" : "Continue in browser", "login:browser", "external-link"
      );

      constexpr std::string_view optionsId = "login:options";
      std::unique_ptr<Node> saved;
      if (auto at = m_detachedInputs.find(std::string(optionsId)); at != m_detachedInputs.end()) {
        saved = std::move(at->second);
        m_detachedInputs.erase(at);
      }
      auto options =
          saved ? std::unique_ptr<Select>(static_cast<Select*>(saved.release())) : std::make_unique<Select>();
      m_widgets[std::string(optionsId)] = options.get();
      options->setOptions({"Use a token", "Explore demo"});
      options->clearSelection();
      options->setPlaceholder("Other options");
      options->setFontSize(Style::fontSizeDescription);
      options->setControlHeight(layout::SignIn::footerHeight);
      const float optionsWidth =
          renderer.measureText("Other options", Style::fontSizeDescription).width + Style::spaceGroup;
      options->setPosition(frames.footer.x + (frames.footer.width - optionsWidth) / 2, frames.footer.y);
      options->setFrameSize(optionsWidth, frames.footer.height);
      options->setOnSelectionChanged([this](std::size_t index, std::string_view) {
        if (!m_connected)
          activate({.action = index == 0 ? Action::ToggleToken : Action::Demo});
      });
      if (auto* trigger = m_controlInput.firstTabFocusUnder(options.get()))
        trigger->setRetainsFocusOnPointerRelease(true);
      card->addChild(std::move(options));
    }
    if (!status.empty())
      label(
          *card, frames.status.x, frames.status.y, frames.status.width, status, Style::fontSizeDescription,
          error ? danger : muted, FontWeight::Normal, 0
      );
    if (m_retrySavedLogin && !loading) {
      const float y = frames.status.y + frames.status.height - layout::SignIn::footerHeight;
      const float retryWidth = std::min(
          frames.status.width,
          renderer.measureText("Retry saved sign-in", Style::fontSizeBody).width + Style::spaceGroup
      );
      loginButton(
          *card, {frames.status.x, y, retryWidth, layout::SignIn::footerHeight, Action::RetryLogin},
          "Retry saved sign-in", "login:retry", "refresh", false, true
      );
    }
    for (auto index = firstHit; index < m_hits.size(); ++index) {
      auto& hit = m_hits[index];
      const float top = std::max(hit.y, frames.viewport.y),
                  bottom = std::min(hit.y + hit.height, frames.viewport.y + frames.viewport.height);
      hit.y = top;
      hit.height = std::max(0.0F, bottom - top);
    }
    if (m_loginMaxScroll > 0)
      settingsScrollbar(
          root, "login:scrollbar",
          {width - Style::spaceLg, frames.viewport.y, Style::scrollbarWidth, frames.viewport.height},
          frames.contentHeight, m_loginScroll
      );
  }
  void ClientApplication::demoLoginStep() {
    using Stage = auth::QrStage;
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto step = m_demoInteractionStep++;
    const auto containsText = [&](std::string_view expected) {
      bool found = false;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text)
          found |= static_cast<const TextNode&>(node).text().contains(expected);
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      return found;
    };
    const auto images = [&] {
      int count = 0;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        count += node.type() == NodeType::Image;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      return count;
    };
    const auto clickWidget = [&](const std::string& id) {
      const auto found = m_widgets.find(id);
      require(found != m_widgets.end() && found->second->parent(), "The sign-in control is mounted");
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(found->second, left, top, right, bottom);
      const float x = (left + right) / 2, y = (top + bottom) / 2;
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.released();
    };
    const auto ready = [&] {
      applyQrLoginUpdate({.stage = Stage::Ready, .qrUrl = "https://example.com/cressida/offline-login"});
    };
    require(!m_qrLogin.active() && !m_login.active(), "Sign-in replays never start authentication workers");
    if (step == 0) {
      require(
          images() == 1 && containsText("Cressida") && !containsText("Show QR code"),
          "The loading screen has one logo, a wordmark and no extra start button"
      );
      require(m_window.animate(), "The loading logo requests compositor-paced animation");
      m_preferences.reducedMotion = true;
    } else if (step == 1) {
      require(!m_window.animate(), "Reduced motion keeps the loading logo still");
      ready();
      m_preferences.reducedMotion = false;
      if (m_loginMaxScroll > 0)
        onKey({XKB_KEY_Page_Down});
    } else if (step == 2) {
      require(
          m_loginQr.has_value() && images() == 0 && containsText("Scan to sign in"),
          "A real matrix replaces the loading logo without a logo overlay"
      );
      layout::Frame canvas{};
      std::vector<layout::Frame> modules;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Rect) {
          const auto& rect = static_cast<const RectNode&>(node);
          float left = 0, top = 0, right = 0, bottom = 0;
          Node::transformedBounds(&node, left, top, right, bottom);
          const layout::Frame bounds{left, top, right - left, bottom - top};
          if (rect.style().fill == rgbHex(Style::qrCodeLightRgb)
              && std::abs(bounds.width - bounds.height) < Style::borderWidth / 2
              && bounds.width > canvas.width
              && bounds.width <= layout::SignIn::qrSize)
            canvas = bounds;
          if (rect.style().fill == rgbHex(Style::qrCodeDarkRgb))
            modules.push_back(bounds);
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(canvas.width > 0 && !modules.empty(), "The native QR has a white canvas and real dark modules");
      float moduleSize = canvas.width;
      for (const auto& bounds : modules)
        if (bounds.x >= canvas.x
            && bounds.y >= canvas.y
            && bounds.x + bounds.width <= canvas.x + canvas.width + Style::borderWidth
            && bounds.y + bounds.height <= canvas.y + canvas.height + Style::borderWidth)
          moduleSize = std::min(moduleSize, bounds.height);
      const float requiredSide = (m_loginQr->side + QrCode::quietZoneModules * 2) * moduleSize;
      require(
          std::abs(canvas.width - requiredSide) < Style::borderWidth / 2,
          "The white canvas fits the code plus its required quiet zone without extra padding"
      );
      require(
          !containsText("Use a token") && !containsText("Explore demo"),
          "Secondary sign-in methods remain behind Other options"
      );
      clickWidget("login:options");
    } else if (step == 3) {
      require(
          m_selectRequest && m_selectOwner == "login:options" && m_selectControl->highlightedIndex() == 0,
          "Other options opens its native dropdown on the first action"
      );
      onKey({XKB_KEY_Escape});
      require(!m_selectRequest && m_loginQr.has_value(), "Escape dismisses the dropdown before QR sign-in");
      onKey({XKB_KEY_Return});
    } else if (step == 4) {
      require(m_selectRequest.has_value(), "The options dropdown can reopen from its keyboard focus");
      onKey({XKB_KEY_Return});
      require(m_showTokenEntry && !m_loginQr && !m_qrLoginState, "Choosing token sign-in clears the QR attempt");
    } else if (step == 5) {
      require(
          m_inputs.contains("token") && m_controlInput.focusedArea() == m_inputs.at("token")->inputArea(),
          "Token selection focuses the retained masked editor"
      );
      pasteControl("token", "offline-credential-fixture");
      require(m_token == "offline-credential-fixture", "Token paste reaches the focused native editor");
    } else if (step == 6) {
      require(!containsText("offline-credential-fixture"), "The token never appears as unmasked scene text");
      clickWidget("login:connect");
      require(m_status.contains("offline preview"), "Offline token previews never submit credentials");
      onKey({XKB_KEY_Escape});
      require(
          !m_showTokenEntry && m_token.empty() && m_inputs.at("token")->value().empty(),
          "Leaving advanced sign-in clears the token and native editor"
      );
    } else if (step == 7) {
      ready();
      applyQrLoginUpdate({.stage = Stage::AwaitingApproval, .account = auth::QrAccount{"11", "Maya", ""}});
    } else if (step == 8) {
      require(
          !m_loginQr && containsText("Maya") && containsText("Approve on your phone"),
          "A scanned code is hidden while the selected account approves sign-in"
      );
      onKey({XKB_KEY_Escape});
      require(!m_qrLoginState && !m_qrAutoStartPending, "Explicit cancellation cannot restart QR sign-in");
      applyQrLoginUpdate({.stage = Stage::Expired});
    } else if (step == 9) {
      require(
          m_qrLoginState && m_qrLoginState->stage == Stage::Expired && !m_loginQr && containsText("Generate new code"),
          "Expired codes remain hidden until an explicit retry"
      );
      activate({.action = Action::QrLogin});
      require(m_status.contains("offline preview"), "Explicit QR retry remains offline in replay mode");
      applyQrLoginUpdate({.stage = Stage::Failed, .error = "Offline QR connection failure"});
    } else if (step == 10) {
      require(
          containsText("Offline QR connection failure") && containsText("Continue in browser"),
          "QR failures show an error and retain browser sign-in"
      );
      beginLogin();
      require(m_status.contains("offline preview"), "The browser action stays offline in replay mode");
      clearQrLogin();
      m_browserLoginWaiting = true;
      m_status = "Finish sign-in in your browser.";
    } else if (step == 11) {
      require(
          containsText("Waiting for your browser") && m_window.animate(),
          "Browser handoff has an explicit waiting state and cancellation"
      );
      onKey({XKB_KEY_Escape});
      require(!m_browserLoginWaiting, "Escape cancels browser handoff");
    } else if (step == 12) {
      m_connecting = m_restoringLogin = true;
      m_status = "Your desktop keyring may ask you to unlock it.";
    } else if (step == 13) {
      require(
          images() == 1
              && !containsText("Restoring your sign-in")
              && !containsText("Your desktop keyring")
              && !m_widgets.at("login:options")->parent(),
          "Saved sign-in shows only the loading logo while alternate methods remain hidden"
      );
      m_preferences.reducedMotion = true;
      require(!m_window.animate(), "Reduced motion also applies during saved sign-in restoration");
      onKey({XKB_KEY_Escape});
    } else if (step == 14) {
      m_retrySavedLogin = true;
      m_status = "Offline temporary connection failure. Your saved sign-in is still available.";
      m_qrAutoStartPending = false;
    } else if (step == 15) {
      require(containsText("Retry saved sign-in"), "Temporary failures retain an explicit saved-login retry");
      const auto* options = m_widgets.at("login:options");
      bool focused = false;
      for (std::size_t index = 0; index < m_widgets.size() && !focused; ++index) {
        onKey({XKB_KEY_Tab});
        for (const auto* node = static_cast<const Node*>(m_controlInput.focusedArea()); node; node = node->parent())
          focused |= node == options;
      }
      require(focused, "Other options is reachable by keyboard");
    } else if (step == 16) {
      float left = 0, top = 0, right = 0, bottom = 0;
      Node::transformedBounds(m_controlInput.focusedArea(), left, top, right, bottom);
      require(
          top >= m_window.titlebarHeight() + Style::spaceLg - Style::borderWidth
              && bottom <= m_window.height() - Style::spaceLg + Style::borderWidth,
          "Keyboard focus is revealed inside the scrollable sign-in viewport"
      );
      onKey({XKB_KEY_Return});
    } else if (step == 17) {
      require(m_selectRequest.has_value(), "The revealed secondary menu opens from keyboard focus");
      onKey({XKB_KEY_Down});
      const auto view = m_demoOverlay;
      onKey({XKB_KEY_Return});
      require(m_connected && m_demo && !m_qrLogin.active(), "The hidden demo action opens the offline client");
      m_demoOverlay = view;
      m_demoPrepared = m_demoOverlayApplied = true;
      std::clog << "Offline sign-in controls, QR states, logo motion and keyboard replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
