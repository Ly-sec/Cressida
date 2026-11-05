#include "api/actions.h"
#include "api/client.h"
#include "app/message_history.h"
#include "app/message_layout.h"

#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
  void require(bool value, const char* reason) {
    if (!value)
      throw std::runtime_error(reason);
  }
} // namespace

int main() {
  using namespace api;
  using nlohmann::json;
  try {
    PollDraft draft{
        .question = "Which **literal** [choice](https://example.test)? <@30> ||hidden||", .answers = {"One", "Two"}
    };
    auto payload = pollCreateBody(draft);
    require(
        payload["duration"] == pollDefaultDuration
            && payload["layout_type"] == pollDefaultLayout
            && payload["answers"][0]["poll_media"]["text"] == "One"
            && !payload["answers"][0].contains("answer_id"),
        "Creation uses hours and media objects without invented answer IDs"
    );
    payload["expiry"] = "2099-01-01T00:00:00Z";
    payload["answers"][0]["answer_id"] = 4;
    payload["answers"][1]["answer_id"] = 9;
    auto poll = parsePoll(payload);
    require(
        poll && !poll->resultsKnown && poll->answers[1].id == 9,
        "Missing results stay unknown and nonsequential answer IDs survive"
    );
    auto emptyResults = payload;
    emptyResults["results"] = {{"is_finalized", false}, {"answer_counts", json::array()}};
    const auto zero = parsePoll(emptyResults);
    require(
        zero && zero->resultsKnown && zero->answers[0].votes == 0 && zero->answers[1].votes == 0,
        "An empty result count array means known zero votes, not unknown results"
    );
    auto message = parseMessages(
                       json::array(
                           {{{"id", "20"},
                             {"channel_id", "10"},
                             {"content", ""},
                             {"author", {{"id", "30"}, {"username", "Test"}}},
                             {"poll", payload}}}
                       )
    ).front();
    require(message.poll && message.content.empty(), "A poll-only message has no generic placeholder");
    parseMessagePatch({{"content", "Other content"}}).apply(message);
    require(message.poll == poll, "Unrelated partial updates preserve polls");
    payload["results"] = {
        {"is_finalized", false}, {"answer_counts", json::array({{{"id", 9}, {"count", 3}, {"me_voted", true}}})}
    };
    parseMessagePatch({{"poll", payload}}).apply(message);
    require(
        message.poll->resultsKnown
            && message.poll->answers[0].votes == 0
            && message.poll->answers[1].votes == 3
            && message.poll->answers[1].me,
        "Known results initialize omitted answers to zero"
    );
    payload.erase("results");
    parseMessagePatch({{"poll", payload}}).apply(message);
    require(
        message.poll->answers[1].votes == 3 && message.poll->resultsKnown,
        "A later snapshot omitting results retains known counts"
    );
    applyPollVote(*message.poll, {9, true, true});
    require(message.poll->answers[1].votes == 3, "An own vote echoed after confirmation is not counted twice");
    applyPollVote(*message.poll, {9, false, false});
    applyPollVote(*message.poll, {4, true, {}});
    require(
        message.poll->answers[1].votes == 2 && !message.poll->answers[1].me && message.poll->answers[0].votes == 1,
        "Vote deltas update their actual answer without creating reactions"
    );
    auto unknown = *poll;
    applyPollVote(unknown, {4, true, true});
    require(!unknown.resultsKnown && unknown.answers[0].me, "Own state can update without fabricating unknown totals");
    const auto stable = *message.poll;
    message.poll->finalized = true;
    applyPollVote(*message.poll, {4, true, {}});
    require(message.poll->answers == stable.answers, "Finalized totals reject delayed vote events");
    message.poll = stable;

    Interaction action{.kind = InteractionKind::VotePoll, .channel = "10", .message = "20", .pollAnswers = {4, 9}};
    const auto vote = interactionRequest(action);
    require(
        vote
            && vote->method == "PUT"
            && vote->path == "/channels/10/polls/20/answers/@me"
            && vote->body["answer_ids"] == json::array({4, 9}),
        "Votes use the personal-account answer route"
    );
    action.kind = InteractionKind::RemovePollVote;
    const auto remove = interactionRequest(action);
    require(
        remove && remove->method == "PUT" && remove->body["answer_ids"].empty(),
        "Removing a vote sends the empty replacement answer set"
    );
    action.kind = InteractionKind::VotePoll;
    for (const auto& ids :
         std::vector<std::vector<int>>{{}, {0}, {-1}, {4, 4}, std::vector<int>(pollAnswerMaximum + 1, 4)}) {
      action.pollAnswers = ids;
      require(!interactionRequest(action), "Malformed votes never enter the request queue");
    }
    action.pollAnswers = {4};
    action.message = "20/answers";
    require(!interactionRequest(action), "Poll routes reject malformed identities");

    for (const auto& invalid : std::vector<json>{
             nullptr,
             0,
             json::array(),
             {{"answers", json::array()}},
             {{"question", {{"text", "Q"}}}, {"answers", {nullptr}}}
         })
      require(!parsePoll(invalid), "Malformed poll objects are bounded and rejected");
    auto malformed = payload;
    malformed["answers"][1]["answer_id"] = 4;
    require(!parsePoll(malformed), "Duplicate answer IDs cannot alias controls");
    malformed["answers"][1]["answer_id"] = std::numeric_limits<std::uint64_t>::max();
    require(!parsePoll(malformed), "Oversized wire IDs cannot overflow");
    draft.question = std::string(pollQuestionLimit, 'q');
    draft.answers[0] = std::string(pollAnswerLimit, 'a');
    require(pollDraftError(draft).empty(), "Exact creation text limits are accepted");
    draft.question += "q";
    require(!pollDraftError(draft).empty() && pollCreateBody(draft).is_null(), "Overlong questions cannot serialize");
    draft.question = "🌙";
    draft.answers = {"🌙", "☀️"};
    require(pollDraftError(draft).empty(), "Limits count Unicode characters instead of bytes");
    draft.answers = {"", "Two"};
    require(!pollDraftError(draft).empty(), "Empty answers are rejected");
    draft.answers = {"One"};
    require(!pollDraftError(draft).empty(), "A poll needs at least two answers");
    draft.answers.assign(pollAnswerMaximum, "Answer");
    draft.duration = pollMaximumDuration;
    require(pollDraftError(draft).empty(), "Ten answers and the maximum duration are accepted");
    draft.answers.push_back("Overflow");
    require(!pollDraftError(draft).empty(), "More than ten answers are rejected");
    draft.answers.pop_back();
    draft.duration = pollMaximumDuration + 1;
    require(!pollDraftError(draft).empty(), "Unbounded durations cannot serialize");

    app::MessageHistory history;
    std::vector<Message> messages;
    auto query = history.begin(HistoryDirection::Latest, messages);
    MessagePatch delta;
    delta.pollVotes.push_back({4, true, true});
    history.changed(message.id, delta);
    require(
        history.apply(*query, messages, {message}, {}) && messages.front().poll->answers[0].me,
        "Votes arriving during history loading survive the old snapshot"
    );
    query = history.begin(HistoryDirection::Latest, messages);
    history.changed(message.id, {.poll = *poll});
    history.changed(message.id, delta);
    history.apply(*query, messages, {message}, {});
    require(messages.front().poll->answers[0].me, "Poll replacement and later vote deltas retain ordering");
    parseMessagePatch({{"poll", nullptr}}).apply(messages.front());
    require(!messages.front().poll, "Explicit null removes a poll");

    const app::MessageMeasure measure = [](std::string_view text, const app::MessageStyle& style) {
      return static_cast<float>(api::utf8Length(std::string(text))) * style.size / 2;
    };
    for (float width : {180.0F, 300.0F, 600.0F}) {
      const auto layout = app::layoutRichMessage(message, width, measure);
      require(
          layout.poll
              && layout.poll->card.width <= width
              && layout.poll->answers.size() == 2
              && layout.height >= layout.poll->card.y + layout.poll->card.height,
          "Poll measurement reserves the complete card and its controls at every width"
      );
      require(
          layout.plainText.contains("**literal**") && !layout.plainText.contains("System message"),
          "Poll text stays literal and available to the selectable document"
      );
      for (const auto& run : layout.runs)
        require(
            !run.style.bold && run.style.link.empty() && !run.style.spoiler,
            "Poll text cannot inject Markdown, links or hidden spoilers"
        );
    }
    Client client;
    require(
        !client.send("10", "", {}, {}, {}, "1", {}, draft),
        "Invalid poll creation stays offline even with a valid destination"
    );
    std::cout << "Poll parsing, creation, votes, partial updates, history and layout passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
