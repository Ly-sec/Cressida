---
title: Polls
description: Create polls, vote for answers and view results in a conversation.
---

## Read and vote

Polls appear as cards inside the conversation, with their question, answer
choices, vote counts and remaining time. Answer emoji are shown when supplied
with the poll. Poll questions and answers are literal text.

Select an answer, then choose **Vote**. A poll marked **Select one or more
answers** lets you select several choices before submitting. Results appear
after your vote is confirmed. Choose **Remove Vote** to clear your vote while
the poll is still open.

You can also choose **Show results** without voting, then **Go back to vote** to
return to the choices. Ended polls show their results and no longer accept
votes. Vote counts update as live events arrive. If counts were not supplied,
Cressida shows that results are unavailable instead of treating them as zero.
Multiple-answer polls count answer selections, so one person can contribute more
than one vote to the total.

A failed vote leaves the confirmed results unchanged and shows an error on the
card. Try the action again explicitly. Cressida does not retry failed votes
automatically.

## Create a poll

1. Open the **+** message tools menu beside the composer and choose **Create
   Poll**.
2. Enter a question of up to 300 characters and between 2 and 10 text answers.
   Each answer can contain up to 55 characters, including emoji typed or pasted
   into its text.
3. Choose a duration, from 1 hour to 32 days. The default is 24 hours.
4. Enable **Allow Multiple Answers** if people can choose more than one answer.
5. Choose **Post**.

The form scrolls when its answers do not fit the window. Use **Add another
answer** to add a choice and the **×** beside an answer to remove it. At least
two answers must remain. **Cancel**, the close button or **Escape** closes the
form.

Creating a poll requires message-writing access. Server channels also require
**Send Polls**. Access is checked again when you post. If access changes while
the form is open, your entered fields stay available.

Posting a poll keeps your existing text draft, attachments, stickers and reply.
The poll appears dimmed until delivery is confirmed. If delivery is uncertain,
check the conversation before posting again. **Restore** reopens the retained
poll form without replacing your text draft or sending automatically. Pending
polls are kept for the current session only.

Polls cannot be edited after posting. Creating separate answer-emoji badges,
viewing voter lists, ending polls early and scheduling polls are not available.
Native replays and protocol tests use synthetic data; they do not establish live
interoperability.

See [Chat](chat.md) for composer controls and message delivery, and
[Capabilities and FAQ](faq.md) for account-risk information.
