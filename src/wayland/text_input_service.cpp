#include "wayland/text_input_service.h"

#include "core/log.h"
#include "text-input-unstable-v3-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

  constexpr Logger kLog("text-input");
  constexpr std::size_t kMaxSurroundingTextBytes = 4000;

  void textInputEnter(void* data, zwp_text_input_v3* /*textInput*/, wl_surface* surface) {
    static_cast<TextInputService*>(data)->handleEnter(surface);
  }

  void textInputLeave(void* data, zwp_text_input_v3* /*textInput*/, wl_surface* surface) {
    static_cast<TextInputService*>(data)->handleLeave(surface);
  }

  void textInputPreeditString(
      void* data, zwp_text_input_v3* /*textInput*/, const char* text, std::int32_t cursorBegin, std::int32_t cursorEnd
  ) {
    static_cast<TextInputService*>(data)->handlePreeditString(text, cursorBegin, cursorEnd);
  }

  void textInputCommitString(void* data, zwp_text_input_v3* /*textInput*/, const char* text) {
    static_cast<TextInputService*>(data)->handleCommitString(text);
  }

  void textInputDeleteSurroundingText(
      void* data, zwp_text_input_v3* /*textInput*/, std::uint32_t beforeLength, std::uint32_t afterLength
  ) {
    static_cast<TextInputService*>(data)->handleDeleteSurroundingText(beforeLength, afterLength);
  }

  void textInputDone(void* data, zwp_text_input_v3* /*textInput*/, std::uint32_t serial) {
    static_cast<TextInputService*>(data)->handleDone(serial);
  }

  void textInputAction(
      void* data, zwp_text_input_v3* /*textInput*/, std::uint32_t action, std::uint32_t /*serial*/
  ) {
    static_cast<TextInputService*>(data)->handleAction(action);
  }

  void textInputLanguage(void* /*data*/, zwp_text_input_v3* /*textInput*/, const char* /*language*/) {}

  void textInputPreeditHint(
      void* /*data*/, zwp_text_input_v3* /*textInput*/, std::uint32_t /*start*/, std::uint32_t /*end*/,
      std::uint32_t /*hint*/
  ) {}

  const zwp_text_input_v3_listener kTextInputListener = {
      .enter = &textInputEnter,
      .leave = &textInputLeave,
      .preedit_string = &textInputPreeditString,
      .commit_string = &textInputCommitString,
      .delete_surrounding_text = &textInputDeleteSurroundingText,
      .done = &textInputDone,
      .action = &textInputAction,
      .language = &textInputLanguage,
      .preedit_hint = &textInputPreeditHint,
  };

  [[nodiscard]] std::uint32_t contentHintFor(const TextInputState& state, std::uint32_t version) {
    std::uint32_t hint = ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE;
    if (state.hiddenText) {
      hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT;
    }
    if (state.sensitiveData) {
      hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA;
    }
    if (state.multiline) {
      hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE;
    }
    if (state.preeditVisible && version >= 2) {
      hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_PREEDIT_SHOWN;
    }
    return hint;
  }

  [[nodiscard]] std::uint32_t contentPurposeFor(TextInputPurpose purpose) {
    switch (purpose) {
    case TextInputPurpose::Normal:
      return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
    case TextInputPurpose::Password:
      return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
    }
    return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
  }

  bool boundary(const std::string& text, std::size_t index) {
    return index <= text.size() && (index == text.size() || (static_cast<unsigned char>(text[index]) & 0xc0) != 0x80);
  }
  void surroundingWindow(TextInputState& state) {
    if (!state.sendSurroundingText)
      return;
    const auto cursor = static_cast<std::size_t>(std::max(0, state.cursor));
    const auto anchor = static_cast<std::size_t>(std::max(0, state.anchor));
    const auto first = std::min(cursor, anchor), last = std::max(cursor, anchor);
    if (state.cursor < 0
        || state.anchor < 0
        || !boundary(state.surroundingText, first)
        || !boundary(state.surroundingText, last)
        || last - first > kMaxSurroundingTextBytes) {
      state.sendSurroundingText = false;
      state.surroundingText.clear();
      state.cursor = state.anchor = 0;
      return;
    }
    if (state.surroundingText.size() <= kMaxSurroundingTextBytes)
      return;
    const auto padding = (kMaxSurroundingTextBytes - (last - first)) / 2;
    auto begin = first > padding ? first - padding : 0;
    begin = std::min(begin, state.surroundingText.size() - kMaxSurroundingTextBytes);
    auto end = begin + kMaxSurroundingTextBytes;
    while (!boundary(state.surroundingText, begin))
      ++begin;
    while (!boundary(state.surroundingText, end))
      --end;
    state.surroundingText = state.surroundingText.substr(begin, end - begin);
    state.cursor = static_cast<std::int32_t>(cursor - begin);
    state.anchor = static_cast<std::int32_t>(anchor - begin);
  }
  bool sameState(const TextInputState& left, const TextInputState& right) {
    return left.surroundingText == right.surroundingText
        && left.cursor == right.cursor
        && left.anchor == right.anchor
        && left.cursorRectX == right.cursorRectX
        && left.cursorRectY == right.cursorRectY
        && left.cursorRectWidth == right.cursorRectWidth
        && left.cursorRectHeight == right.cursorRectHeight
        && left.purpose == right.purpose
        && left.sendSurroundingText == right.sendSurroundingText
        && left.sensitiveData == right.sensitiveData
        && left.hiddenText == right.hiddenText
        && left.preeditVisible == right.preeditVisible
        && left.multiline == right.multiline;
  }
  std::int32_t protocolCoordinate(std::int32_t value, float scale) {
    return static_cast<std::int32_t>(std::clamp(
        std::round(static_cast<double>(value) * scale), static_cast<double>(std::numeric_limits<std::int32_t>::min()),
        static_cast<double>(std::numeric_limits<std::int32_t>::max())
    ));
  }

} // namespace

TextInputService::TextInputService() = default;

TextInputService::~TextInputService() {
  m_changed = {};
  cleanup();
}

bool TextInputService::bind(zwp_text_input_manager_v3* manager, wl_seat* seat) {
  if (manager == nullptr || seat == nullptr) {
    return false;
  }
  if (m_manager == manager && m_seat == seat && m_textInput != nullptr) {
    return true;
  }

  cleanup();
  m_manager = manager;
  m_seat = seat;
  m_textInput = zwp_text_input_manager_v3_get_text_input(m_manager, m_seat);
  if (m_textInput == nullptr) {
    kLog.warn("failed to create text-input-v3 object");
    cleanup();
    return false;
  }

  m_textInputVersion = static_cast<std::uint32_t>(zwp_text_input_v3_get_version(m_textInput));
  zwp_text_input_v3_add_listener(m_textInput, &kTextInputListener, this);
  kLog.info("text-input-v3 bound");
  return true;
}

void TextInputService::cleanup() {
  ++m_focusGeneration;
  if (m_activeClient != nullptr) {
    deactivateClient(m_activeClient);
  }
  if (m_textInput != nullptr) {
    zwp_text_input_v3_destroy(m_textInput);
    m_textInput = nullptr;
  }
  m_manager = nullptr;
  m_seat = nullptr;
  m_enteredSurface = nullptr;
  m_keyboardFocusSurface = nullptr;
  m_keyboardFocusParentSurface = nullptr;
  m_activeSurface = nullptr;
  m_activeClient = nullptr;
  m_activeAcceptsKeyboardFocusActivation = false;
  m_pendingEdit = {};
  m_sentState.reset();
  m_commitSerial = 0;
  m_enabledSerial = 0;
  m_textInputVersion = 0;
  m_enabled = false;
  m_composing = m_applyingEdit = m_deferState = false;
  stateChanged();
}

bool TextInputService::isAvailable() const noexcept { return m_textInput != nullptr; }
bool TextInputService::isEnabled() const noexcept { return m_enabled && activeSurfaceAcceptsTextInput(); }
bool TextInputService::hasComposition() const noexcept { return m_composing; }
bool TextInputService::hasPendingEdit() const noexcept {
  return m_applyingEdit
      || m_pendingEdit.hasPreedit
      || m_pendingEdit.hasCommitText
      || m_pendingEdit.hasDelete
      || m_pendingEdit.submit;
}
void TextInputService::setStateChanged(std::function<void()> changed) { m_changed = std::move(changed); }
void TextInputService::setCoordinateScale(float scale) {
  scale = std::isfinite(scale) && scale > 0 ? scale : 1;
  if (scale != m_coordinateScale) {
    m_coordinateScale = scale;
    commitActiveState(TextInputChangeCause::Other);
  }
}

void TextInputService::setFocusedClient(
    wl_surface* surface, TextInputClient* client, bool acceptKeyboardFocusActivation,
    wl_surface* keyboardFocusParentSurface
) {
  if (surface == nullptr || client == nullptr) {
    clearFocusedClient(m_activeClient);
    return;
  }
  if (m_textInput == nullptr) {
    return;
  }

  m_activeAcceptsKeyboardFocusActivation = acceptKeyboardFocusActivation;
  m_keyboardFocusParentSurface = keyboardFocusParentSurface;

  if (m_activeClient == client && m_activeSurface == surface) {
    if (!m_enabled) {
      enableActive(TextInputChangeCause::Other);
    } else {
      commitActiveState(TextInputChangeCause::Other);
    }
    return;
  }

  ++m_focusGeneration;
  disableActive();
  if (m_activeClient != nullptr) {
    deactivateClient(m_activeClient);
  }

  m_activeSurface = surface;
  m_activeClient = client;
  resetEdits();
  m_activeClient->textInputActivated(*this);
  enableActive(TextInputChangeCause::Other);
}

void TextInputService::clearFocusedClient(TextInputClient* client) {
  if (client == nullptr || client != m_activeClient) {
    return;
  }
  ++m_focusGeneration;
  disableActive();
  deactivateClient(client);
  m_activeClient = nullptr;
  m_activeSurface = nullptr;
  m_activeAcceptsKeyboardFocusActivation = false;
  m_keyboardFocusParentSurface = nullptr;
  resetEdits();
}

void TextInputService::notifyClientStateChanged(TextInputClient* client, TextInputChangeCause cause) {
  if (client == nullptr || client != m_activeClient) {
    return;
  }
  commitActiveState(cause);
}

void TextInputService::onKeyboardFocusSurface(wl_surface* surface, bool entered) {
  if (entered) {
    m_keyboardFocusSurface = surface;
  } else if (m_keyboardFocusSurface == surface) {
    ++m_focusGeneration;
    m_keyboardFocusSurface = nullptr;
    disableActive();
    if (m_enteredSurface == surface)
      m_enteredSurface = nullptr;
    resetEdits();
  }

  if (m_activeClient != nullptr && entered && activeSurfaceAcceptsTextInput()) {
    enableActive(TextInputChangeCause::Other);
  }
}

bool TextInputService::activeSurfaceAcceptsTextInput() const noexcept {
  if (m_activeSurface == nullptr) {
    return false;
  }
  if (m_enteredSurface == m_activeSurface) {
    return true;
  }
  if (!m_activeAcceptsKeyboardFocusActivation || m_keyboardFocusSurface == nullptr) {
    return false;
  }
  if (m_keyboardFocusSurface == m_activeSurface) {
    return true;
  }
  return m_keyboardFocusParentSurface != nullptr && m_keyboardFocusSurface == m_keyboardFocusParentSurface;
}

void TextInputService::handleEnter(wl_surface* surface) {
  ++m_focusGeneration;
  m_enteredSurface = surface;
  m_enabled = false;
  resetEdits();
  if (m_activeClient != nullptr && m_activeSurface == surface) {
    enableActive(TextInputChangeCause::Other);
  }
}

void TextInputService::handleLeave(wl_surface* surface) {
  if (m_enteredSurface != surface) {
    return;
  }
  ++m_focusGeneration;
  m_enteredSurface = nullptr;
  m_enabled = false;
  resetEdits();
}

void TextInputService::handlePreeditString(const char* text, std::int32_t cursorBegin, std::int32_t cursorEnd) {
  if (!isEnabled())
    return;
  m_pendingEdit.hasPreedit = true;
  m_pendingEdit.preeditText = text != nullptr ? text : "";
  m_pendingEdit.preeditCursorBegin = cursorBegin;
  m_pendingEdit.preeditCursorEnd = cursorEnd;
  stateChanged();
}

void TextInputService::handleCommitString(const char* text) {
  if (!isEnabled())
    return;
  m_pendingEdit.hasCommitText = true;
  m_pendingEdit.commitText = text != nullptr ? text : "";
  stateChanged();
}

void TextInputService::handleDeleteSurroundingText(std::uint32_t beforeLength, std::uint32_t afterLength) {
  if (!isEnabled())
    return;
  m_pendingEdit.hasDelete = true;
  m_pendingEdit.deleteBeforeLength = beforeLength;
  m_pendingEdit.deleteAfterLength = afterLength;
  stateChanged();
}

void TextInputService::handleDone(std::uint32_t serial) {
  TextInputEdit edit = std::move(m_pendingEdit);
  m_pendingEdit = {};

  if (!m_activeClient || !isEnabled() || static_cast<std::int32_t>(serial - m_enabledSerial) < 0) {
    stateChanged();
    return;
  }
  const auto generation = m_focusGeneration;
  auto* client = m_activeClient;
  m_composing = edit.hasPreedit && !edit.preeditText.empty();
  m_applyingEdit = true;
  m_deferState = serial != m_commitSerial;
  client->textInputApplyEdit(edit);
  m_applyingEdit = false;
  if (m_activeClient == client && generation == m_focusGeneration && !m_deferState)
    commitActiveState(TextInputChangeCause::InputMethod);
  stateChanged();
}

void TextInputService::handleAction(std::uint32_t action) {
  if (isEnabled() && action == ZWP_TEXT_INPUT_V3_ACTION_SUBMIT) {
    m_pendingEdit.submit = true;
    stateChanged();
  }
}

void TextInputService::enableActive(TextInputChangeCause cause) {
  if (m_textInput == nullptr || m_activeClient == nullptr || m_activeSurface == nullptr) {
    return;
  }
  if (!activeSurfaceAcceptsTextInput()) {
    return;
  }

  if (!m_enabled) {
    zwp_text_input_v3_enable(m_textInput);
    m_enabled = true;
    m_sentState.reset();
    m_deferState = false;
    commitActiveState(cause);
    m_enabledSerial = m_commitSerial;
    stateChanged();
    return;
  }
  commitActiveState(cause);
}

void TextInputService::disableActive() {
  if (m_textInput == nullptr || !m_enabled) {
    m_enabled = false;
    return;
  }
  if (activeSurfaceAcceptsTextInput()) {
    zwp_text_input_v3_disable(m_textInput);
    commitProtocolState();
  }
  m_enabled = false;
  m_sentState.reset();
  m_deferState = false;
  stateChanged();
}

void TextInputService::commitActiveState(TextInputChangeCause cause) {
  if (m_textInput == nullptr || m_activeClient == nullptr || m_activeSurface == nullptr || !m_enabled) {
    return;
  }
  if (!activeSurfaceAcceptsTextInput()) {
    return;
  }
  if (m_applyingEdit || m_deferState)
    return;
  TextInputState state = m_activeClient->textInputState();
  surroundingWindow(state);
  state.cursorRectX = protocolCoordinate(state.cursorRectX, m_coordinateScale);
  state.cursorRectY = protocolCoordinate(state.cursorRectY, m_coordinateScale);
  state.cursorRectWidth = std::max<std::int32_t>(1, protocolCoordinate(state.cursorRectWidth, m_coordinateScale));
  state.cursorRectHeight = std::max<std::int32_t>(1, protocolCoordinate(state.cursorRectHeight, m_coordinateScale));
  if (m_sentState && sameState(*m_sentState, state))
    return;
  if (state.sendSurroundingText) {
    zwp_text_input_v3_set_surrounding_text(m_textInput, state.surroundingText.c_str(), state.cursor, state.anchor);
  }
  zwp_text_input_v3_set_content_type(
      m_textInput, contentHintFor(state, m_textInputVersion), contentPurposeFor(state.purpose)
  );
  zwp_text_input_v3_set_text_change_cause(
      m_textInput,
      cause == TextInputChangeCause::InputMethod ? ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD
                                                 : ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER
  );
  zwp_text_input_v3_set_cursor_rectangle(
      m_textInput, state.cursorRectX, state.cursorRectY, state.cursorRectWidth, state.cursorRectHeight
  );
  commitProtocolState();
  m_sentState = std::move(state);
}

void TextInputService::commitProtocolState() {
  zwp_text_input_v3_commit(m_textInput);
  ++m_commitSerial;
}

void TextInputService::deactivateClient(TextInputClient* client) {
  client->textInputResetPreedit();
  client->textInputDeactivated(*this);
}
void TextInputService::resetEdits() {
  m_pendingEdit = {};
  m_composing = false;
  m_deferState = false;
  m_sentState.reset();
  if (m_activeClient)
    m_activeClient->textInputResetPreedit();
  stateChanged();
}
void TextInputService::stateChanged() {
  if (m_changed)
    m_changed();
}
