---
title: Chat and Markdown
description:
  Send messages, reply to a conversation, use completion and format your text.
---

## Send a message

Choose a channel or DM, type in the composer and press **Enter** to send. Use
**Shift+Enter** for a new line. When an ordinary chat window regains focus, the
composer is focused so you can type right away. If you click away from the
composer and no control has focus, pressing a key focuses it and applies that
key to your draft. Shortcuts, Tab navigation and Escape keep their usual
actions. Open dialogs, pickers, settings and editors keep their own focus.

Messages appear in the conversation immediately in a dimmed state, including
local image previews. They use normal colors once delivery is confirmed. You can
keep typing and send another message while an earlier one is pending. Pending
follow-ups share the previous message group and do not repeat your avatar.

If delivery cannot be confirmed, the pending message shows **Not confirmed**.
Choose **Restore** to move its text, attachments and reply back to an empty
composer. Your newer draft is kept until you clear it. Cressida does not retry
automatically, since a delayed confirmation could mean the message already
arrived. Pending messages are kept for the current session only.

Message times use your system timezone, including daylight-saving changes for
the date of each message. This applies to chat headers, hover timestamps,
forwarded messages and message previews. Pinned-message dates also use your
local calendar day. Consecutive messages start a new group at local midnight.

## Search messages

Click the search field in the upper right or press **Ctrl+F**. The menu offers
filters for people, channels, mentions and content types. Choose a filter, then
pick a suggestion or type its value. Chosen people and channels display readable
names in the field.

| Filter      | Finds                                                                                     |
| ----------- | ----------------------------------------------------------------------------------------- |
| `from:`     | Messages sent by a person. Use `from:me` for your own messages.                           |
| `in:`       | Messages in a channel. Use `in:here` for the current channel.                             |
| `mentions:` | Messages that mention a person.                                                           |
| `has:`      | Images, videos, GIFs, files, links, embeds, audio, stickers, polls or forwarded messages. |
| `before:`   | Messages before a date.                                                                   |
| `after:`    | Messages from a date onward.                                                              |
| `during:`   | Messages on a specific date.                                                              |
| `author:`   | Messages from `user`, `bot` or `webhook` authors.                                         |

Combine filters with ordinary text, for example:

```text
release from:me in:announcements has:video
```

Filters also work on their own, such as `has:image`. Dates use `YYYY-MM-DD` in
UTC. **More filters** offers date and author options. Use `has:-image` to
exclude images, or another supported content type after the minus sign.

Suggestions use people and channels currently loaded in the client. You can also
enter an ID, or type an exact name. Put names containing spaces in double
quotes, such as `from:"Alex Smith"`. If a name is ambiguous, choose a suggestion
or use its ID. Search stays within the current server or DM.

Use **Up** and **Down** to highlight a suggestion and **Enter** or **Tab** to
accept it. Press **Enter** without a highlighted suggestion to search.
**Escape** dismisses the menu while keeping your query. Open a result to jump to
its message.

Recent searches appear beneath the filters, separately for each server or DM.
Choose one to restore it, or use the trash button beside **History** to clear
that list. Search history lasts for the current session and is cleared on
sign-out.

## Reply to someone

Hover a message and choose **Reply**. The composer shows who you are replying
to. The mention control switches the reply notification on or off.

Press **Escape** or use the reply row's close button to cancel the reply while
keeping your draft. If a picker or dialog is open, Escape dismisses it first.

Consecutive messages from the same person can share a header. A message
following your reply can join that group; each message keeps its own actions and
timestamp.

## Mentions and emoji

Type `@` to choose a member, role, everyone or here. Type `:` followed by at
least two characters to search emoji. Suggestions can include Unicode and server
emoji.

- Use **Up** and **Down** to move through suggestions.
- Press **Enter** or **Tab** to insert the selected suggestion.
- Press **Escape** to dismiss the list.

Choosing a suggestion inserts it without sending the message. Mentions display
readable names and emoji display their images while you compose.

## Format your text

| Write                          | Result                                     |
| ------------------------------ | ------------------------------------------ |
| `**bold**`                     | Bold text                                  |
| `*italic*`                     | Italic text                                |
| `__underlined__`               | Underlined text                            |
| `~~crossed out~~`              | Strikethrough                              |
| `                              |                                            | hidden text |     | `   | A spoiler, revealed on click |
| `` `inline code` ``            | Inline code                                |
| `[label](https://example.com)` | A labeled link                             |
| `> quoted text`                | A quote                                    |
| `# Heading`                    | A heading at the start of a line           |
| `-# Small text`                | Smaller, muted text at the start of a line |

Use three backticks on their own lines for a code block. You can put a language
name after the opening backticks. Spoilers and strikethrough cover spaces within
the formatted text. Link underlines appear when you hover the link.

## Images and embeds

Pasted images can be uploaded from the composer. Multiple images in a message
appear together in a gallery. Links may show compact previews with a title,
description or image. See [Images and video](media.md).

## Catch up and save for later

Use the [Inbox](inbox.md) for unread conversations and mentions across your
account. Message menus also let you bookmark a message or set a reminder.
