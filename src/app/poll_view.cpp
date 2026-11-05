#include "app/client_application.h"
#include "app/interaction_key.h"
#include "app/message_time.h"
#include "app/theme.h"
#include "render/scene/text_node.h"
#include "ui/controls/button.h"
#include "ui/controls/scrollbar.h"
#include "ui/controls/select.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <linux/input-event-codes.h>
#include <numeric>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    constexpr std::array pollDurations{1, 4, 8, 24, 72, 168, api::pollMaximumDuration};
    constexpr std::size_t maximumPollUiStates = 512;
    bool ended(const api::Poll& poll) {
      const auto expiry = messageTime(poll.expiry);
      return poll.finalized || (expiry && *expiry <= std::chrono::system_clock::now());
    }
    bool voted(const api::Poll& poll) { return std::ranges::any_of(poll.answers, &api::PollAnswer::me); }
    std::string timeLeft(const api::Poll& poll) {
      if (ended(poll))
        return poll.finalized ? "Poll ended" : "Poll ended · Finalizing results";
      const auto expiry = messageTime(poll.expiry);
      if (!expiry)
        return "";
      const auto remaining = *expiry - std::chrono::system_clock::now();
      const auto hours = std::chrono::ceil<std::chrono::hours>(remaining).count();
      if (hours > 1)
        return std::to_string(hours) + "h left";
      return std::to_string(std::max<std::int64_t>(1, std::chrono::ceil<std::chrono::minutes>(remaining).count()))
          + "m left";
    }
  } // namespace

  void ClientApplication::pollButton(
      Node& parent, const std::string& id, layout::Frame frame, const std::string& caption,
      std::function<void()> clicked, bool enabled, bool primary
  ) {
    std::unique_ptr<Node> node;
    if (const auto old = m_detachedInputs.find(id); old != m_detachedInputs.end()) {
      node = std::move(old->second);
      m_detachedInputs.erase(old);
    } else
      node = std::make_unique<Button>();
    auto* control = static_cast<Button*>(node.get());
    m_widgets[id] = control;
    control->setText(caption);
    control->setVariant(primary ? ButtonVariant::Primary : ButtonVariant::Ghost);
    control->setPosition(frame.x, frame.y);
    control->setFrameSize(frame.width, frame.height);
    control->setControlHeight(frame.height);
    control->setEnabled(enabled);
    control->setOnClick([this, session = m_session, clicked = std::move(clicked)] {
      if (session != m_session)
        return;
      clicked();
      m_window.invalidate();
    });
    parent.addChild(std::move(node));
  }

  bool ClientApplication::canCreatePoll() const {
    if (!m_connected || m_channelId.empty() || selectedChannelWriteAccess() != api::MessageWriteAccess::Allowed)
      return false;
    if (m_dmMode)
      return true;
    const auto permissions = selectedChannelPermissions();
    return permissions && (*permissions & api::permission::sendPolls);
  }

  void ClientApplication::openComposerTools() {
    if ((!m_connected && !m_demo) || m_channelId.empty() || !m_dialogKind.empty())
      return;
    closeContext();
    closeCompletion();
    closeGifPicker();
    closeEmojiPicker();
    m_contextMessage.clear();
    m_contextUser.clear();
    m_contextGuild.clear();
    m_contextChannel = m_channelId;
    m_contextQuickIndex = -1;
    m_contextEntries = {
        {.id = static_cast<int>(Action::PasteClipboard),
         .label = "Paste from Clipboard",
         .enabled = !m_demo,
         .icon = "clipboard-plus"},
        {.id = static_cast<int>(Action::CreatePoll), .label = "Create Poll", .enabled = canCreatePoll(), .icon = "list"}
    };
    m_contextX = m_pointerX;
    m_contextY = m_composerTop - (Style::controlHeight + Style::spaceSm) * m_contextEntries.size();
    m_contextScroll = 0;
    m_contextOpen = m_contextDirty = true;
    m_window.invalidate();
  }

  void ClientApplication::openPoll() {
    if (!canCreatePoll() || (!m_dialogKind.empty() && m_dialogKind != "poll-create"))
      return;
    closeContext();
    closeDialog();
    closeCompletion();
    closeGifPicker();
    closeEmojiPicker();
    cancelMessageEdit(false);
    m_scrolling.stopAll();
    m_pollEditor = PollEditor{.channel = m_channelId};
    m_dialogKind = "poll-create";
    m_dialogError.clear();
    for (auto& [id, input] : m_inputs)
      if (id.starts_with("poll-create:"))
        input->setValue("");
    m_focusedInput = "poll-create:question";
    m_window.invalidate();
  }

  void ClientApplication::submitPoll() {
    if (!m_pollEditor || m_dialogKind != "poll-create")
      return;
    if (m_pollEditor->channel != m_channelId || !canCreatePoll()) {
      m_dialogError = "You can no longer create a poll in this channel. Your answers are retained.";
      return;
    }
    m_dialogError = api::pollDraftError(m_pollEditor->draft);
    if (!m_dialogError.empty())
      return;
    if (queueOutgoing("", {}, {}, {}, false, {}, m_pollEditor->draft))
      closeDialog();
    else
      m_dialogError = m_status.empty() ? "Could not queue this poll. Your answers are retained." : m_status;
  }

  void ClientApplication::drawPollDialog(Node& root, float width, float height) {
    using namespace theme;
    using D = layout::PollDialog;
    if (!m_pollEditor)
      return;
    auto& editor = *m_pollEditor;
    const float w = std::min(D::maximumWidth, width - D::padding * 2);
    const float content = std::max(Style::spaceSection, w - D::padding * 2);
    const float formHeight = D::formHeight(editor.draft.answers.size());
    const float h = std::min(
        {D::maximumHeight, height - layout::Window::titlebarHeight - D::padding * 2,
         D::padding * 2 + D::heading + formHeight + D::footer}
    );
    const float x = (width - w) / 2;
    const float y = layout::Window::titlebarHeight + (height - layout::Window::titlebarHeight - h) / 2;
    rectangle(
        root, 0, layout::Window::titlebarHeight, width, height - layout::Window::titlebarHeight,
        withAlpha(background, Style::modalBackdropOpacity)
    );
    m_hits.push_back(
        {0, layout::Window::titlebarHeight, width, height - layout::Window::titlebarHeight, Action::None, {}}
    );
    rectangle(root, x, y, w, h, sidebar, Style::radiusXl);
    m_dialogBounds = {x, y, w, h, Action::None, {}};
    m_hits.push_back(m_dialogBounds);
    label(
        root, x + D::padding, y + D::padding, content - Style::controlHeightSm, "Create a Poll", Style::fontSizeHeading,
        foreground, FontWeight::SemiBold
    );
    const auto generation = m_dialogGeneration;
    const auto valid = [this, generation] {
      return m_pollEditor && m_dialogKind == "poll-create" && generation == m_dialogGeneration;
    };
    pollButton(
        root, "poll-create:close",
        {x + w - D::padding - Style::controlHeightSm, y + D::padding / 2, Style::controlHeightSm,
         Style::controlHeightSm},
        "×", [this, valid] {
          if (valid())
            closeDialog();
        }
    );
    editor.viewport = {
        x + D::padding, y + D::padding + D::heading, content,
        std::max(0.0F, h - D::padding * 2 - D::heading - D::footer)
    };
    editor.maximumScroll = std::max(0.0F, formHeight - editor.viewport.height);
    editor.scroll = std::clamp(editor.scroll, 0.0F, editor.maximumScroll);
    auto viewport = std::make_unique<Node>();
    viewport->setPosition(editor.viewport.x, editor.viewport.y);
    viewport->setSize(editor.viewport.width, editor.viewport.height);
    viewport->setClipChildren(true);
    auto form = std::make_unique<Node>();
    form->setPosition(0, -editor.scroll);
    form->setSize(content, formHeight);
    float cursor = 0;
    const auto fieldLabel = [&](const std::string& caption) {
      label(*form, 0, cursor, content, caption, Style::fontSizeBody, foreground);
      cursor += D::label;
    };
    fieldLabel("Question");
    inputControl(
        *form, 0, cursor, content, D::questionHeight, "poll-create:question", editor.draft.question,
        "What question do you want to ask?", true
    );
    m_inputs.at("poll-create:question")->setInvalid(api::utf8Length(editor.draft.question) > api::pollQuestionLimit);
    cursor += D::questionHeight;
    label(
        *form, 0, cursor, content, std::to_string(api::utf8Length(editor.draft.question)) + " / 300",
        Style::fontSizeDescription, muted
    );
    cursor += D::label + Style::spaceLg;
    fieldLabel("Answers");
    for (std::size_t i = 0; i < editor.draft.answers.size(); ++i) {
      const auto id = "poll-create:answer:" + std::to_string(i);
      inputControl(
          *form, 0, cursor, content - Style::controlHeightLg - Style::spaceXs, Style::controlHeightLg, id,
          editor.draft.answers[i], "Type your answer"
      );
      m_inputs.at(id)->setInvalid(api::utf8Length(editor.draft.answers[i]) > api::pollAnswerLimit);
      pollButton(
          *form, "poll-create:remove:" + std::to_string(i),
          {content - Style::controlHeightLg, cursor, Style::controlHeightLg, Style::controlHeightLg}, "×",
          [this, valid, i] {
            if (!valid()
                || m_pollEditor->draft.answers.size() <= api::pollAnswerMinimum
                || i >= m_pollEditor->draft.answers.size())
              return;
            m_pollEditor->draft.answers.erase(m_pollEditor->draft.answers.begin() + i);
            m_controlInput.setFocus(nullptr);
            m_focusedInput.clear();
            for (auto& [id, input] : m_inputs)
              if (id.starts_with("poll-create:answer:"))
                input->setValue("");
          },
          editor.draft.answers.size() > api::pollAnswerMinimum
      );
      cursor += D::answerStep;
    }
    pollButton(
        *form, "poll-create:add", {0, cursor, content, Style::controlHeight}, "+ Add another answer",
        [this, valid] {
          if (!valid() || m_pollEditor->draft.answers.size() >= api::pollAnswerMaximum)
            return;
          m_pollEditor->draft.answers.emplace_back();
          m_focusedInput = "poll-create:answer:" + std::to_string(m_pollEditor->draft.answers.size() - 1);
          m_pollEditor->scroll += D::answerStep;
        },
        editor.draft.answers.size() < api::pollAnswerMaximum
    );
    cursor += Style::controlHeight + Style::spaceLg;
    fieldLabel("Duration");
    const std::string selectId = "poll-create:duration";
    std::unique_ptr<Node> selectNode;
    if (const auto old = m_detachedInputs.find(selectId); old != m_detachedInputs.end()) {
      selectNode = std::move(old->second);
      m_detachedInputs.erase(old);
    } else
      selectNode = std::make_unique<Select>();
    auto* select = static_cast<Select*>(selectNode.get());
    m_widgets[selectId] = select;
    select->setOnSelectionChanged({});
    select->setOptions({"1 hour", "4 hours", "8 hours", "24 hours", "3 days", "1 week", "32 days"});
    const auto duration = std::ranges::find(pollDurations, editor.draft.duration);
    select->setSelectedIndexSilently(static_cast<std::size_t>(duration - pollDurations.begin()));
    select->setControlHeight(Style::controlHeightLg);
    select->setPosition(0, cursor);
    select->setFrameSize(content, Style::controlHeightLg);
    select->setOnSelectionChanged([this, valid](std::size_t i, std::string_view) {
      if (valid() && i < pollDurations.size())
        m_pollEditor->draft.duration = pollDurations[i];
    });
    form->addChild(std::move(selectNode));
    cursor += Style::controlHeightLg + Style::spaceLg;
    const std::string toggleId = "poll-create:multiple";
    std::unique_ptr<Node> toggleNode;
    if (const auto old = m_detachedInputs.find(toggleId); old != m_detachedInputs.end()) {
      toggleNode = std::move(old->second);
      m_detachedInputs.erase(old);
    } else
      toggleNode = std::make_unique<Toggle>();
    auto* toggle = static_cast<Toggle*>(toggleNode.get());
    m_widgets[toggleId] = toggle;
    toggle->setToggleSize(ToggleSize::Small);
    toggle->setCheckedImmediate(editor.draft.multiple);
    toggle->setPosition(0, cursor + (Style::controlHeight - D::toggleHeight) / 2);
    toggle->setFrameSize(D::toggleWidth, D::toggleHeight);
    toggle->setOnChange([this, valid](bool enabled) {
      if (valid())
        m_pollEditor->draft.multiple = enabled;
    });
    form->addChild(std::move(toggleNode));
    const float captionX = D::toggleWidth + Style::spaceSm;
    const auto captionMetrics = m_window.target().renderer().measureText(
        "Allow Multiple Answers", Style::fontSizeBody, FontWeight::Normal, content - captionX, 1
    );
    label(
        *form, captionX,
        layout::Chat::centeredTextTop(
            cursor + Style::controlHeight / 2, captionMetrics.top, captionMetrics.inkTop, captionMetrics.inkBottom
        ),
        content - captionX, "Allow Multiple Answers", Style::fontSizeBody, foreground
    );
    viewport->addChild(std::move(form));
    root.addChild(std::move(viewport));
    if (editor.maximumScroll > 0) {
      const std::string id = "poll-create:scrollbar";
      std::unique_ptr<Node> node;
      if (const auto old = m_detachedInputs.find(id); old != m_detachedInputs.end()) {
        node = std::move(old->second);
        m_detachedInputs.erase(old);
      } else
        node = std::make_unique<Scrollbar>();
      auto* bar = static_cast<Scrollbar*>(node.get());
      m_widgets[id] = bar;
      bar->setPosition(x + w - D::padding / 2, editor.viewport.y);
      bar->setSize(Style::scrollbarWidth, editor.viewport.height);
      bar->setOnScrollChanged([this, valid](float offset) {
        if (valid()) {
          m_pollEditor->scroll = offset;
          m_window.invalidate();
        }
      });
      bar->update(editor.viewport.height, formHeight, editor.scroll);
      root.addChild(std::move(node));
    }
    const float footer = y + h - D::padding - D::footer;
    const auto error =
        !canCreatePoll() ? "You cannot create polls in this channel. Your answers are retained." : m_dialogError;
    if (!error.empty())
      label(
          root, x + D::padding, footer + Style::spaceSm, content, error, Style::fontSizeDescription, danger,
          FontWeight::Normal, 2
      );
    const float buttonY = y + h - D::padding - Style::controlHeight;
    const float half = (content - Style::spaceSm) / 2;
    pollButton(
        root, "poll-create:cancel", {x + D::padding, buttonY, half, Style::controlHeight}, "Cancel", [this, valid] {
          if (valid())
            closeDialog();
        }
    );
    pollButton(
        root, "poll-create:post", {x + D::padding + half + Style::spaceSm, buttonY, half, Style::controlHeight}, "Post",
        [this, valid] {
          if (valid())
            submitPoll();
        },
        canCreatePoll(), true
    );
  }

  void ClientApplication::drawPollCard(
      Node& parent, const api::Message& message, const MessageLayout& body, float x, float y
  ) {
    using namespace theme;
    using P = layout::PollCard;
    if (!message.poll || !body.poll || blockedMessage(message))
      return;
    const auto& poll = *message.poll;
    if (!ended(poll)) {
      if (const auto expiry = messageTime(poll.expiry)) {
        const auto now = std::chrono::system_clock::now();
        const auto deadline = std::min<std::chrono::system_clock::time_point>(
            *expiry, std::chrono::floor<std::chrono::minutes>(now) + std::chrono::minutes{1}
        );
        if (!m_pollTimer.active() || deadline < m_pollTickAt) {
          m_pollTickAt = deadline;
          m_pollTimer.start(
              std::max(std::chrono::milliseconds{1}, std::chrono::ceil<std::chrono::milliseconds>(deadline - now)),
              [this] { m_window.invalidate(); }
          );
        }
      }
    }
    auto frames = *body.poll;
    for (auto* f : {&frames.card, &frames.question, &frames.subtitle, &frames.status, &frames.results, &frames.vote}) {
      f->x += x;
      f->y += y;
    }
    for (auto& f : frames.answers) {
      f.x += x;
      f.y += y;
    }
    const auto key = std::pair{message.channelId, message.id};
    const auto at = m_pollUi.find(key);
    const PollUi empty;
    const auto& state = at == m_pollUi.end() ? empty : at->second;
    const bool closed = ended(poll), hasVote = voted(poll), results = closed || hasVote || state.results;
    const bool active = m_connected
        && api::validSnowflake(message.id)
        && !message.deleted
        && !state.pending
        && m_dialogKind.empty()
        && poll.layout == api::pollDefaultLayout;
    const auto total =
        std::accumulate(poll.answers.begin(), poll.answers.end(), std::int64_t{}, [](auto sum, const auto& answer) {
          return sum + answer.votes;
        });
    rectangle(
        parent, frames.card.x, frames.card.y, frames.card.width, frames.card.height, raised, Style::surfaceRadius
    );
    rectangle(
        parent, frames.card.x + Style::borderWidth, frames.card.y + Style::borderWidth,
        frames.card.width - Style::borderWidth * 2, frames.card.height - Style::borderWidth * 2, sidebar,
        Style::surfaceRadius
    );
    label(
        parent, frames.subtitle.x, frames.subtitle.y, frames.subtitle.width,
        poll.multiple ? "Select one or more answers" : "Select one answer", Style::fontSizeDescription, muted
    );
    const auto id = "poll:" + message.channelId + ":" + message.id;
    for (std::size_t i = 0; i < poll.answers.size() && i < frames.answers.size(); ++i) {
      const auto& answer = poll.answers[i];
      const auto& row = frames.answers[i];
      const bool selected = results ? answer.me : state.selected.contains(answer.id);
      rectangle(parent, row.x, row.y, row.width, row.height, selected ? accent : panel, Style::controlRadius);
      if (selected)
        rectangle(
            parent, row.x + Style::borderWidth, row.y + Style::borderWidth, row.width - Style::borderWidth * 2,
            row.height - Style::borderWidth * 2, panel, Style::controlRadius
        );
      if (results && poll.resultsKnown && total > 0 && answer.votes > 0)
        rectangle(
            parent, row.x + Style::borderWidth, row.y + Style::borderWidth,
            (row.width - Style::borderWidth * 2) * static_cast<float>(answer.votes) / total,
            row.height - Style::borderWidth * 2, raised, Style::controlRadius
        );
      pollButton(
          parent, id + ":answer:" + std::to_string(answer.id), row, "",
          [this, channel = message.channelId, messageId = message.id, answerId = answer.id] {
            pollVote(channel, messageId, answerId);
          },
          active && !closed && !results
      );
      auto* button = static_cast<Button*>(m_widgets.at(id + ":answer:" + std::to_string(answer.id)));
      auto colors = Button::defaultPalette(ButtonVariant::Ghost);
      colors.hover = {ColorSpec{.fixed = raised}, clearColorSpec(), ColorSpec{.fixed = foreground}};
      colors.pressed = colors.hover;
      button->setCustomPalette(std::move(colors));
      const float center = row.y + (row.height - Style::glyphSizeMedium) / 2;
      if (!answer.emojiId.empty() && m_preferences.showImages)
        emojiImage(parent, row.x + P::gap, center, Style::glyphSizeMedium, api::emojiPath(answer.emojiId));
      else if (!answer.emojiName.empty())
        label(parent, row.x + P::gap, center, P::emojiWidth, answer.emojiName, Style::fontSizeTitle, foreground);
      if (results) {
        const auto percent = total > 0 ? (static_cast<std::int64_t>(answer.votes) * 100 + total / 2) / total : 0;
        const auto value = poll.resultsKnown ? std::to_string(answer.votes)
                + (answer.votes == 1 ? " vote  " : " votes  ")
                + std::to_string(percent)
                + "%"
                                             : "Unknown";
        label(
            parent, row.x + row.width - P::resultWidth,
            [&] {
              const auto metrics = m_window.target().renderer().measureText(
                  value, Style::fontSizeDescription, FontWeight::Normal, P::resultWidth - P::gap, 1
              );
              return layout::Chat::centeredTextTop(
                  row.y + row.height / 2, metrics.top, metrics.inkTop, metrics.inkBottom
              );
            }(),
            P::resultWidth - P::gap, value, Style::fontSizeDescription, foreground
        );
      } else
        glyph(
            parent, row.x + row.width - P::gap - Style::glyphSizeMedium, center, Style::glyphSizeMedium,
            selected ? "circle-check" : "circle", button->hovered() ? foreground : (selected ? accent : muted)
        );
    }
    const auto count =
        poll.resultsKnown ? std::to_string(total) + (total == 1 ? " vote" : " votes") : "Votes unavailable";
    const auto remaining = timeLeft(poll);
    label(
        parent, frames.status.x, frames.status.y, frames.status.width,
        state.error.empty() ? count + (remaining.empty() ? "" : " · " + remaining) : state.error,
        Style::fontSizeDescription, state.error.empty() ? muted : danger, FontWeight::Normal, 2
    );
    pollButton(
        parent, id + ":results", frames.results, results ? "Go back to vote" : "Show results",
        [this, channel = message.channelId, messageId = message.id] { pollVote(channel, messageId, {}); },
        active && !closed && !hasVote
    );
    pollButton(
        parent, id + ":vote", frames.vote,
        state.pending ? "Saving…"
            : hasVote ? "Remove Vote"
                      : "Vote",
        [this, channel = message.channelId, messageId = message.id] { pollVote(channel, messageId, {}, true); },
        active && !closed && (hasVote || (!results && !state.selected.empty())), !hasVote
    );
  }

  void ClientApplication::pollVote(
      const std::string& channel, const std::string& id, std::optional<int> answer, bool submit
  ) {
    if (!m_connected || channel != m_channelId || !m_dialogKind.empty())
      return;
    const auto message = std::ranges::find(m_messages, id, &api::Message::id);
    if (message == m_messages.end()
        || message->channelId != channel
        || !message->poll
        || message->deleted
        || blockedMessage(*message)
        || ended(*message->poll)
        || !api::validSnowflake(id)
        || message->poll->layout != api::pollDefaultLayout)
      return;
    if (!m_dmMode) {
      const auto permissions = selectedChannelPermissions();
      if (!permissions || !(*permissions & api::permission::viewChannel))
        return;
    }
    const auto& poll = *message->poll;
    const auto key = std::pair{channel, id};
    if (!m_pollUi.contains(key) && m_pollUi.size() >= maximumPollUiStates) {
      const auto old = std::ranges::find_if(m_pollUi, [](const auto& item) { return !item.second.pending; });
      if (old == m_pollUi.end())
        return;
      m_pollUi.erase(old);
    }
    auto& state = m_pollUi[key];
    if (state.pending)
      return;
    if (answer) {
      if (voted(poll)
          || state.results
          || std::ranges::find(poll.answers, *answer, &api::PollAnswer::id) == poll.answers.end())
        return;
      if (state.selected.erase(*answer) == 0) {
        if (!poll.multiple)
          state.selected.clear();
        state.selected.insert(*answer);
      }
    } else if (!submit) {
      state.results = !state.results;
    } else {
      api::Interaction action{
          .kind = voted(poll) ? api::InteractionKind::RemovePollVote : api::InteractionKind::VotePoll,
          .channel = channel,
          .message = id,
          .revision = ++m_dialogGeneration
      };
      if (action.kind == api::InteractionKind::VotePoll) {
        if (state.selected.empty() || (!poll.multiple && state.selected.size() != 1))
          return;
        for (int selected : state.selected) {
          if (std::ranges::find(poll.answers, selected, &api::PollAnswer::id) == poll.answers.end())
            return;
          action.pollAnswers.push_back(selected);
        }
      }
      if (m_demo && m_demoOverlay.starts_with("polls-cycle")) {
        state.pending = action;
      } else if (startInteraction(action)) {
        state.pending = std::move(action);
        state.error.clear();
      } else
        state.error = m_status.empty() ? "Could not queue this vote. Try again." : m_status;
    }
    m_window.invalidate();
  }

  bool ClientApplication::pollResult(const api::Event& event) {
    if (!event.interaction
        || (event.interaction->kind != api::InteractionKind::VotePoll
            && event.interaction->kind != api::InteractionKind::RemovePollVote))
      return false;
    const auto& action = *event.interaction;
    const auto at = m_pollUi.find({action.channel, action.message});
    if (at == m_pollUi.end() || !at->second.pending || at->second.pending->revision != action.revision)
      return true;
    auto& state = at->second;
    const bool receivedLive = state.pending->stateRevision != 0;
    state.pending.reset();
    state.error = event.error;
    if (!event.error.empty()) {
      m_window.invalidate();
      return true;
    }
    api::MessagePatch patch;
    const auto apply = [&](const std::vector<api::Message>& messages) {
      const auto message = std::ranges::find(messages, action.message, &api::Message::id);
      if (message == messages.end() || !message->poll || message->deleted)
        return;
      for (const auto& answer : message->poll->answers) {
        const bool selected = std::ranges::find(action.pollAnswers, answer.id) != action.pollAnswers.end();
        if (answer.me != selected)
          patch.pollVotes.push_back({answer.id, selected, selected});
      }
    };
    if (action.channel == m_channelId)
      apply(m_messages);
    else if (const auto cached = m_channelMessages.find(action.channel); cached != m_channelMessages.end())
      apply(cached->second);
    if (!receivedLive && !patch.pollVotes.empty())
      updateMessage(action.channel, action.message, patch);
    state.selected.clear();
    state.results = action.kind == api::InteractionKind::VotePoll;
    m_window.invalidate();
    return true;
  }

  void ClientApplication::demoPollStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto click = [&](const std::string& key) {
      const auto at = m_widgets.find(key);
      require(at != m_widgets.end() && at->second->parent(), "Poll controls are mounted native widgets");
      float left, top, right, bottom;
      Node::transformedBounds(at->second, left, top, right, bottom);
      m_window.motion((left + right) / 2, (top + bottom) / 2);
      m_window.click((left + right) / 2, (top + bottom) / 2, BTN_LEFT);
      m_window.released();
    };
    const std::string id = "poll:" + m_channelId + ":9601";
    const auto key = std::pair{m_channelId, std::string("9601")};
    const auto pollAccess = [&](bool allowed) {
      m_roleDirectory.apply(
          {.guilds = {
               {.guild = m_guildId,
                .replaceRoles = true,
                .roles =
                    {{.id = m_guildId,
                      .permissions = std::to_string(
                          api::viewChannel | api::sendMessages | (allowed ? api::permission::sendPolls : 0)
                      )}},
                .members = {{m_account.id, {}}},
                .owner = allowed ? m_account.id : "other-owner"}
           }}
      );
    };
    const auto result = [&](const std::string& error) {
      const auto at = m_pollUi.find(key);
      require(at != m_pollUi.end() && at->second.pending.has_value(), "Voting queues one scoped request");
      api::Event event{.operation = api::Operation::Interaction, .resource = m_channelId, .error = error};
      event.interaction = at->second.pending;
      pollResult(event);
    };
    const auto step = m_demoInteractionStep++;
    std::clog << "Poll replay step " << step << '\n';
    if (step == 0) {
      api::PollDraft draft{
          .question = "Which native poll should we try?", .answers = {"A single answer", "Multiple answers"}
      };
      auto payload = api::pollCreateBody(draft);
      payload["expiry"] = "2099-01-01T00:00:00Z";
      payload["answers"][0]["answer_id"] = 4;
      payload["answers"][1]["answer_id"] = 9;
      payload["results"] = {{"is_finalized", false}, {"answer_counts", nlohmann::json::array()}};
      m_messages = api::parseMessages(
          nlohmann::json::array(
              {{{"id", "9601"},
                {"channel_id", m_channelId},
                {"author", {{"id", m_account.id}, {"username", m_account.name}}},
                {"timestamp", "2026-11-01T14:00:00Z"},
                {"poll", payload}}}
          )
      );
      m_messageLayouts.clear();
      m_channelMessages[m_channelId] = m_messages;
      require(m_messages.front().content.empty(), "Poll-only messages never become placeholders");
    } else if (step == 1) {
      require(m_messageLayouts.at("9601").layout.poll.has_value(), "Poll cards reserve native timeline geometry");
      const auto& layout = m_messageLayouts.at("9601").layout;
      for (const auto& row : layout.poll->answers) {
        bool hasInk = false;
        float first = 0, last = 0;
        for (const auto& run : layout.runs) {
          if (run.y < row.y || run.y >= row.y + row.height)
            continue;
          const auto metrics = m_window.target().renderer().measureText(
              messageMarkup(run.text, run.style, false), run.style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
              TextEllipsize::End, true
          );
          const float top = run.y + metrics.inkTop - metrics.top;
          const float bottom = run.y + metrics.inkBottom - metrics.top;
          first = hasInk ? std::min(first, top) : top;
          last = hasInk ? std::max(last, bottom) : bottom;
          hasInk = true;
        }
        require(
            hasInk && std::abs((first + last) / 2 - row.y - row.height / 2) <= Style::borderWidth,
            "Poll answer ink is vertically centered in its actual row"
        );
      }
      click(id + ":answer:4");
      require(m_pollUi.at(key).selected == std::set<int>{4}, "A native answer click selects its wire ID");
      click(id + ":answer:9");
      require(m_pollUi.at(key).selected == std::set<int>{9}, "Single-answer polls replace the earlier choice");
    } else if (step == 2) {
      click(id + ":results");
      require(m_pollUi.at(key).results && !voted(*m_messages.front().poll), "Results can be viewed without voting");
    } else if (step == 3) {
      std::size_t zeroLabels = 0;
      const auto inspectResults = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text && static_cast<const TextNode&>(node).text() == "0 votes  0%")
          ++zeroLabels;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspectResults(inspectResults, *m_widgets.at(id + ":results")->parent());
      require(zeroLabels == 2, "Known empty results render zero votes for both answers instead of unknown");
      click(id + ":results");
      pollVote(m_channelId, "9601", {}, true);
      result("Synthetic vote failure. Try again.");
      require(
          !voted(*m_messages.front().poll) && !m_pollUi.at(key).error.empty(), "Failed votes preserve confirmed counts"
      );
    } else if (step == 4) {
      click(id + ":vote");
      result("");
      require(
          m_messages.front().poll->answers[1].me && m_messages.front().poll->answers[1].votes == 1,
          "Confirmed votes reveal results once"
      );
      api::MessagePatch echo;
      echo.pollVotes.push_back({9, true, true});
      updateMessage(m_channelId, "9601", echo);
      require(m_messages.front().poll->answers[1].votes == 1, "A live self echo cannot double-count confirmation");
    } else if (step == 5) {
      click(id + ":vote");
      result("");
      require(
          !voted(*m_messages.front().poll) && m_messages.front().poll->answers[1].votes == 0,
          "Remove Vote clears the confirmed answer"
      );
      m_messages.front().poll->multiple = true;
      m_messageLayouts.clear();
    } else if (step == 6) {
      click(id + ":answer:4");
      click(id + ":answer:9");
      require(m_pollUi.at(key).selected.size() == 2, "Multiple-answer polls retain both choices");
      pollVote(m_channelId, "9601", {}, true);
      api::MessagePatch live;
      live.pollVotes = {{4, true, true}, {9, true, true}};
      updateMessage(m_channelId, "9601", live);
      result("");
      require(
          m_messages.front().poll->answers[0].votes == 1 && m_messages.front().poll->answers[1].votes == 1,
          "Gateway-first confirmation preserves both counts"
      );
      m_messages.front().poll->expiry = "2020-01-01T00:00:00Z";
      m_messageLayouts.clear();
    } else if (step == 7) {
      pollVote(m_channelId, "9601", {}, true);
      require(!m_pollUi.at(key).pending, "Expired polls reject submission even through stale targets");
      pollAccess(false);
      openComposerTools();
      require(
          std::ranges::any_of(
              m_contextEntries, [](const auto& e) { return e.id == static_cast<int>(Action::CreatePoll) && !e.enabled; }
          ),
          "Unknown or missing Send Polls access disables creation"
      );
      pollAccess(true);
      openComposerTools();
      require(
          std::ranges::any_of(
              m_contextEntries, [](const auto& e) { return e.id == static_cast<int>(Action::CreatePoll) && e.enabled; }
          ),
          "Message tools exposes poll creation with known access"
      );
      contextAction(Action::CreatePoll);
      require(m_pollEditor && m_dialogKind == "poll-create", "Create Poll opens its native dialog");
    } else if (step == 8) {
      click("poll-create:post");
      require(m_pollEditor && !m_dialogError.empty(), "Empty poll submission keeps the dialog and explains validation");
      pasteControl("poll-create:question", "Where should we go?");
      pasteControl("poll-create:answer:0", "The moon");
      pasteControl("poll-create:answer:1", "The observatory");
      require(
          m_pollEditor->draft.question == "Where should we go?", "Native poll fields accept paste into their own draft"
      );
      const auto viewport = m_pollEditor->viewport;
      m_window.motion(
          viewport.x + viewport.width / 2,
          viewport.y + viewport.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      m_window.scroll(layout::PollDialog::answerStep * 3);
    } else if (step == 9) {
      click("poll-create:add");
    } else if (step == 10) {
      require(m_pollEditor->draft.answers.size() == 3, "Adding answers grows the native form");
      pasteControl("poll-create:answer:2", "Stay here");
      pollAccess(false);
      submitPoll();
      require(
          m_pollEditor && !m_dialogError.empty() && m_pendingSends.empty(),
          "Losing write access retains all poll fields"
      );
      pollAccess(true);
      m_pollEditor->draft.answers.resize(api::pollAnswerMaximum, "Another place");
      m_pollEditor->scroll = layout::PollDialog::formHeight(api::pollAnswerMaximum);
      m_focusedInput.clear();
    } else if (step == 11) {
      require(
          m_pollEditor->maximumScroll > 0 && m_pollEditor->scroll > 0,
          "Ten-answer forms remain scrollable in narrow windows"
      );
      auto* add = static_cast<Button*>(m_widgets.at("poll-create:add"));
      require(!add->enabled(), "Ten answers disable the add control");
      auto* multiple = static_cast<Toggle*>(m_widgets.at("poll-create:multiple"));
      bool alignedCaption = false;
      for (const auto& child : multiple->parent()->children()) {
        if (child->type() != NodeType::Text)
          continue;
        const auto& text = static_cast<const TextNode&>(*child);
        if (text.text() != "Allow Multiple Answers")
          continue;
        const auto metrics = m_window.target().renderer().measureText(
            text.text(), text.fontSize(), text.fontWeight(), text.maxWidth(), 1
        );
        const float inkCenter = text.y() + (metrics.inkTop + metrics.inkBottom) / 2;
        alignedCaption = std::abs(inkCenter - multiple->y() - multiple->height() / 2) <= Style::borderWidth;
      }
      require(
          alignedCaption
              && multiple->width() == layout::PollDialog::toggleWidth
              && multiple->height() == layout::PollDialog::toggleHeight,
          "The native toggle and caption ink share a center with the complete Small control geometry"
      );
      click("poll-create:multiple");
      auto* toggle = static_cast<Toggle*>(m_widgets.at("poll-create:multiple"));
      require(
          toggle->checked() && m_pollEditor->draft.multiple && toggle->toggleSize() == ToggleSize::Small,
          "The native multiple-answer toggle updates the draft"
      );
      m_controlInput.setFocus(static_cast<Button*>(m_widgets.at("poll-create:post"))->inputArea());
      onKey({.symbol = XKB_KEY_Return});
      require(
          !m_pollEditor && m_pendingSends.size() == 1 && m_drafts[m_channelId] == "Keep this text draft",
          "Posting stages a poll independently of the existing composer draft"
      );
    } else if (step == 12) {
      auto& pending = m_pendingSends.begin()->second;
      require(
          pending.preview.poll && pending.preview.poll->answers.size() == api::pollAnswerMaximum,
          "Pending poll previews keep all answers"
      );
      const auto nonce = m_pendingSends.begin()->first;
      pollAccess(false);
      revokeUnavailableOutgoing();
      require(
          pending.revoked->load() && pending.completed,
          "Losing Send Polls revokes a queued creation without consuming its draft"
      );
      pollAccess(true);
      api::Event failure{
          .operation = api::Operation::Send, .resource = m_channelId, .error = "Synthetic delivery failure"
      };
      failure.nonce = nonce;
      outgoingResult(failure);
      restoreOutgoing(nonce);
      require(
          m_pollEditor && m_pollEditor->draft.answers.size() == api::pollAnswerMaximum && m_pendingSends.empty(),
          "Explicit restore recovers a failed poll without retrying it"
      );
    } else if (step == 13) {
      click("poll-create:cancel");
      require(
          !m_pollEditor && m_dialogKind.empty() && m_drafts[m_channelId] == "Keep this text draft",
          "Cancel closes creation without changing the composer"
      );
      m_messages.front().poll->expiry = "2099-01-01T00:00:00Z";
      m_messages.front().poll->resultsKnown = false;
      m_messageLayouts.clear();
      m_pollUi.clear();
    } else if (step == 14) {
      const auto original = m_channelId;
      m_channelId = "9999";
      pollVote(original, "9601", 4, true);
      require(m_pollUi.empty(), "Navigation invalidates a captured poll destination");
      m_channelId = original;
      m_messages.front().poll->resultsKnown = true;
      m_messages.front().poll->answers[0].me = false;
      m_messages.front().poll->answers[1].me = false;
      m_messageLayouts.clear();
    } else if (step == 15) {
      click(id + ":answer:4");
      pollVote(m_channelId, "9601", {}, true);
      deleteMessages(m_channelId, {"9601"});
      require(!m_pollUi.contains(key), "Deletion retires outstanding poll controls and pending replies");
      clearOutgoing();
    } else if (step == 16) {
      require(!m_pollEditor && m_pendingSends.empty(), "Poll replay leaves no active editor or pending send");
      api::PollDraft draft{.question = "Confirmed poll", .answers = {"Here", "There"}};
      require(queueOutgoing("", {}, {}, {}, false, {}, draft), "A poll can enter the normal outgoing queue");
      const auto nonce = m_pendingSends.begin()->first;
      auto message = m_pendingSends.begin()->second.preview;
      message.id = "9602";
      api::Event delivered{.operation = api::Operation::Send, .resource = m_channelId};
      delivered.nonce = nonce;
      auto malformed = message;
      malformed.poll.reset();
      delivered.messages = {malformed};
      outgoingResult(delivered);
      require(
          m_pendingSends.contains(nonce), "A response without the submitted poll cannot discard its retained draft"
      );
      delivered.messages = {message};
      outgoingResult(delivered);
      require(
          m_pendingSends.empty() && m_messages.back().poll && m_drafts[m_channelId] == "Keep this text draft",
          "Confirmed creation replaces its pending poll and keeps unrelated text"
      );
      std::clog << "Offline poll display, native voting, expiry, creation, recovery and navigation passed\n";
    }
    m_window.invalidate();
  }
} // namespace app
