#include "api/polls.h"

#include "api/models.h"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>

namespace api {
  namespace {
    constexpr int maximumPollVotes = 100000000;
    std::string text(const nlohmann::json& value, const char* key, std::size_t limit) {
      const auto at = value.find(key);
      return at != value.end() && at->is_string() && at->get_ref<const std::string&>().size() <= limit * 4
          ? at->get<std::string>()
          : std::string{};
    }
    int integer(const nlohmann::json& value, const char* key) {
      const auto at = value.find(key);
      if (at == value.end() || !at->is_number_integer() || *at < 0 || *at > std::numeric_limits<int>::max())
        return 0;
      return at->get<int>();
    }
    bool nonblank(const std::string& value) { return value.find_first_not_of(" \t\r\n") != std::string::npos; }
  } // namespace
  std::optional<Poll> parsePoll(const nlohmann::json& value) {
    if (!value.is_object()
        || !value.contains("question")
        || !value["question"].is_object()
        || !value.contains("answers")
        || !value["answers"].is_array()
        || value["answers"].empty()
        || value["answers"].size() > pollAnswerMaximum)
      return {};
    Poll result;
    result.question = text(value["question"], "text", pollQuestionLimit);
    result.expiry = text(value, "expiry", 64);
    result.multiple = value.contains("allow_multiselect") && value["allow_multiselect"] == true;
    result.layout = integer(value, "layout_type");
    std::set<int> ids;
    for (const auto& item : value["answers"]) {
      const int id = integer(item, "answer_id");
      if (!id || !ids.insert(id).second || !item.contains("poll_media") || !item["poll_media"].is_object())
        return {};
      const auto& media = item["poll_media"];
      PollAnswer answer{.id = id, .text = text(media, "text", pollAnswerLimit)};
      if (media.contains("emoji") && media["emoji"].is_object()) {
        answer.emojiId = text(media["emoji"], "id", 20);
        if (!validSnowflake(answer.emojiId))
          answer.emojiId.clear();
        answer.emojiName = text(media["emoji"], "name", pollAnswerLimit);
      }
      result.answers.push_back(std::move(answer));
    }
    if (const auto counts = value.find("results"); counts != value.end()
        && counts->is_object()
        && counts->contains("answer_counts")
        && (*counts)["answer_counts"].is_array()
        && (*counts)["answer_counts"].size() <= pollAnswerMaximum) {
      result.resultsKnown = true;
      result.finalized = counts->contains("is_finalized") && (*counts)["is_finalized"] == true;
      std::set<int> counted;
      for (const auto& count : (*counts)["answer_counts"]) {
        const auto id = integer(count, "id");
        const auto at = std::ranges::find(result.answers, id, &PollAnswer::id);
        if (at == result.answers.end() || !counted.insert(id).second)
          continue;
        at->votes = std::min(integer(count, "count"), maximumPollVotes);
        at->me = count.contains("me_voted") && count["me_voted"] == true;
      }
    }
    return result;
  }
  std::string pollDraftError(const PollDraft& draft) {
    if (!nonblank(draft.question)
        || draft.question.size() > pollQuestionLimit * 4
        || utf8Length(draft.question) > pollQuestionLimit)
      return "Enter a question of up to 300 characters.";
    if (draft.answers.size() < pollAnswerMinimum || draft.answers.size() > pollAnswerMaximum)
      return "Add between 2 and 10 answers.";
    for (const auto& answer : draft.answers)
      if (!nonblank(answer) || answer.size() > pollAnswerLimit * 4 || utf8Length(answer) > pollAnswerLimit)
        return "Enter text for every answer, up to 55 characters each.";
    if (draft.duration < 1 || draft.duration > pollMaximumDuration)
      return "Choose a duration between 1 hour and 32 days.";
    return {};
  }
  nlohmann::json pollCreateBody(const PollDraft& draft) {
    if (!pollDraftError(draft).empty())
      return nullptr;
    auto answers = nlohmann::json::array();
    for (const auto& answer : draft.answers)
      answers.push_back({{"poll_media", {{"text", answer}}}});
    return {
        {"question", {{"text", draft.question}}},
        {"answers", std::move(answers)},
        {"duration", draft.duration},
        {"allow_multiselect", draft.multiple},
        {"layout_type", pollDefaultLayout}
    };
  }
  void mergePoll(std::optional<Poll>& current, const std::optional<Poll>& incoming) {
    auto next = incoming;
    if (current
        && next
        && !next->resultsKnown
        && current->question == next->question
        && current->answers.size() == next->answers.size()
        && std::ranges::equal(current->answers, next->answers, [](const auto& a, const auto& b) {
             return a.id == b.id && a.text == b.text && a.emojiId == b.emojiId && a.emojiName == b.emojiName;
           })) {
      for (auto& answer : next->answers)
        if (const auto old = std::ranges::find(current->answers, answer.id, &PollAnswer::id);
            old != current->answers.end() && old->text == answer.text) {
          answer.votes = old->votes;
          answer.me = old->me;
        }
      next->resultsKnown = current->resultsKnown;
      next->finalized = current->finalized;
    }
    current = std::move(next);
  }
  void applyPollVote(Poll& poll, const PollVote& vote) {
    const auto at = std::ranges::find(poll.answers, vote.answer, &PollAnswer::id);
    if (at == poll.answers.end() || poll.finalized)
      return;
    // Unknown totals stay unknown. Own duplicate notifications are idempotent.
    if (poll.resultsKnown && (!vote.me || at->me != *vote.me))
      at->votes = std::clamp(at->votes + (vote.add ? 1 : -1), 0, maximumPollVotes);
    if (vote.me)
      at->me = *vote.me;
  }
} // namespace api
