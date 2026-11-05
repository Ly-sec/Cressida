#pragma once

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace api {
  inline constexpr std::size_t pollQuestionLimit = 300, pollAnswerLimit = 55, pollAnswerMinimum = 2,
                               pollAnswerMaximum = 10;
  inline constexpr int pollDefaultDuration = 24, pollMaximumDuration = 32 * 24, pollDefaultLayout = 1;
  struct PollAnswer {
    int id = 0;
    std::string text, emojiId, emojiName;
    int votes = 0;
    bool me = false;
    bool operator==(const PollAnswer&) const = default;
  };
  struct Poll {
    std::string question, expiry;
    std::vector<PollAnswer> answers;
    bool multiple = false, resultsKnown = false, finalized = false;
    int layout = pollDefaultLayout;
    bool operator==(const Poll&) const = default;
  };
  struct PollDraft {
    std::string question;
    std::vector<std::string> answers = {"", ""};
    int duration = pollDefaultDuration;
    bool multiple = false;
  };
  struct PollVote {
    int answer = 0;
    bool add = false;
    std::optional<bool> me;
  };
  std::optional<Poll> parsePoll(const nlohmann::json& value);
  std::string pollDraftError(const PollDraft& draft);
  nlohmann::json pollCreateBody(const PollDraft& draft);
  void mergePoll(std::optional<Poll>& current, const std::optional<Poll>& incoming);
  void applyPollVote(Poll& poll, const PollVote& vote);
} // namespace api
