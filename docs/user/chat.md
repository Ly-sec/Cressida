---
title: Chat, Markdown and embeds
description:
  Send messages, check message dates, reply, use completion and format your
  text.
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

Today's messages show just the time. Yesterday's messages show **Yesterday at**
before the time, and older messages show a numeric date and time, such as
**10/15/26, 14:22**. Hover any timestamp to see the weekday, full date, year and
time. Grouped follow-ups show a compact time when you hover their row; hover
that time for the full date too. System notices, forwarded messages and
message-request previews use the same date rules.

Message dates and times use your system timezone, including daylight-saving
changes for the date of each message. Pinned-message dates also use your local
calendar day. Consecutive messages start a new group at local midnight. For
AM/PM times, enable **Use 12-hour clock** in
[Appearance settings](appearance.md).

Timestamp mentions such as `<t:1793041200:t>` display a readable time in your
system timezone. The suffix selects short or long time (`t`, `T`), short or long
date (`d`, `D`), date and time (`f`, `F`), or relative time (`R`), such as "2
hours ago". Omitting the suffix shows the date and time. Hover a timestamp
mention to see its weekday, full date, year and local time, including when the
message shows a relative time. The hover time follows **Use 12-hour clock**.
Timestamp syntax inside inline code or code blocks stays literal.

Bot authors show an **APP** chip beside their name and role icon. The chip
remains visible when role icons are turned off.

### Message menu

Right-click a message for its actions. Reaction controls come first, followed by
editing, reply, forwarding and thread actions. A divider separates these from
**Copy Text**, pinning, bookmarks, reminders and **Copy Message Link**. Author
profile and message visibility actions appear in this group too.

**Delete Message** has its own section. **Copy Message ID**, when Developer Mode
is enabled, and any local history removal actions appear below another divider.
Unavailable actions are omitted.

### Hidden messages

Messages from blocked or ignored authors stay collapsed until you click their
row or eye icon. To collapse a revealed message again, hover it and choose the
crossed-eye **Hide message** button. Its right-click menu also offers **Hide
Message**. You can show and hide each message independently without leaving the
conversation. This only changes what you see, not the author's blocked or
ignored status.

### System notices

Server joins and pinned messages appear as short one-line notices instead of
ordinary messages. A pin notice reads "_name_ pinned **a message** to this
channel. See all **pinned messages**." Click **a message** to jump to the pinned
message, or **pinned messages** to open the channel's pins panel. Notices keep
their reactions, and copying one copies its visible text. They have no Reply or
Create Thread action.

### Read-only channels

In a server channel where you cannot send messages, the composer shows a
permission explanation and its message controls stay disabled. Cressida checks
channel access before sending text, images, GIFs or stickers. Threads also use
their thread-specific posting permissions; archived threads must be reopened
before you can post.

While permissions are loading, the composer shows **Checking channel
permissions…**. If that information is unavailable, sending stays disabled.
Changes to roles or channel access update the composer while you are connected.
Your existing draft, reply, attachments and stickers are kept when access
changes. Restoring access does not send or retry a message automatically.

In server channels, actions you do not have permission for are hidden. For
example, **Reply**, **Add Reaction** and **Create Thread** do not appear without
the matching channel permission, and pasted images are refused without Attach
Files. You can still join an existing reaction when you can read the message
history. Actions also stay hidden while permissions are loading. Cressida checks
the permission again when you use an action, so a role change cannot leave a
stale button working.

## Edit and format your draft

The **+** message tools menu includes **Paste from Clipboard**, **Create Poll**
and **Record Voice Message**. See [Polls](polls.md) for voting and results, or
[Voice messages](voice-messages.md) for recording, reviewing and sending audio
without replacing your text draft.

Right-click the message input or inline message editor for **Undo**, **Redo**,
**Cut**, **Copy**, **Paste**, **Paste as plain text**, **Delete** and **Select
All**. Unavailable actions are dimmed. **Paste** can include an available
clipboard image in the composer; pasting never sends the message.

Select text to show a compact formatting toolbar, or choose **Formatting** in
the input's context menu. Apply bold, italic, underline, strikethrough, quotes,
code or spoilers. These actions add or remove Markdown while keeping the content
selected. Quotes affect complete selected lines, and multiline code uses a code
block. Each formatting action is one undoable edit.

Choose **Insert Link** from the toolbar or input menu, or press **Ctrl+K**. Its
window has separate **Text** and **Link** fields, with selected text already
filled in. Without a selection, it inserts at the caret. **Cancel** keeps the
draft unchanged. Links use complete `http://` or `https://` addresses.

Pasting a web address onto selected draft or editor text also creates a Markdown
link. Use **Paste as plain text** or **Ctrl+Shift+V** to insert the address
literally.

With draft or editor text selected, **Ctrl+B**, **Ctrl+I** and **Ctrl+U** toggle
bold, italic and underline. **Shift+F10** or the keyboard's menu key opens the
editing menu. Menus close on outside click, **Escape** or window focus loss.
Formatting controls stay hidden during input-method composition.

## Copy text

Drag across a message's author name, timestamp or text, then press **Ctrl+C** to
copy the selection. You can include the header and body in the same drag. Inline
channel mentions are selectable too, with their readable channel names included
in the copied text. Selection highlights include the spaces between words. Keep
dragging across message boundaries to include more messages. Drag near the top
or bottom of the conversation to scroll while selecting. After selecting chat
text, **Ctrl+A** selects the whole loaded conversation. Messages outside the
loaded history are not included. New arrivals preserve your existing selection.

Clicking an author name still opens their profile, and clicking a channel
mention still opens that channel. Dragging selects text instead of following
those links. The message menu's **Copy Text** action continues to copy just the
body.

## See who reacted

Hover a reaction badge to see a few names and the reaction count. Move onto the
hover card and click its colored **View all** count to open the people list. The
count underlines on hover and sits below the names or loading message. The card
closes immediately when the pointer leaves the badge and card. Moving directly
from the badge onto the card keeps its count accessible. You can also
right-click a reaction badge, or right-click its message and choose **View
Reactions**.

Choose an emoji in the left column to see the people who used it. The selected
emoji stays highlighted when you move the pointer away. The list shows their
display names and usernames; click a person to open their profile. When an emoji
has super reactions, **Normal** and **Super** show the two lists separately. The
two columns scroll independently. More people load automatically as you scroll
down. Earlier entries stay accessible when you scroll back. Failed reads retry
automatically with increasing delays while the window is open, keeping entries
already loaded. Closing the window cancels its retries. Live reaction changes
update the counts and refresh the open list after a short pause.

Use **Tab** and **Shift+Tab** to move through visible controls, **Page Up** and
**Page Down** to scroll the people list, and **Escape** to close the window.
Clicking a reaction badge still adds or removes your own reaction.

## Forward a message

Right-click a message and choose **Forward**. Search by conversation, channel or
server name, then select up to five destinations. The list includes existing
direct and group conversations and cached server channels and threads where you
can send messages. It does not start new conversations. Read-only source
channels can still be forwarded from when you can read their message history.

Destinations use the account's saved channel usage rankings when available, with
cached navigation and message activity filling in the remaining entries. Channel
rows show their server icon, the channel name and the server name underneath.
The whole row, including its checkbox, is selectable. The message preview
renders Markdown and emoji, with a thumbnail for supported images, following
your display preferences.

Add an optional comment beside the message preview. The emoji button opens the
usual searchable emoji picker and inserts your choice into this comment. The
main composer draft, attachments and reply stay separate. **Escape** closes the
emoji picker first; another **Escape**, the close button or an outside click
cancels the forwarding dialog.

Choose **Send** to forward the message to the selected destinations. Each
forward contains a snapshot created by the service, including supported
attachments. Cressida shows it with a **Forwarded** label. The optional comment
is sent as a separate message only after that destination's forward is
confirmed. Duplicate confirmations do not send the comment again. The comment
shares the forward's author header like other consecutive messages.

If delivery cannot be confirmed, the pending forward keeps its comment. Open the
destination conversation and choose **Restore** on the pending message to reopen
the forwarding dialog. Check the conversation before sending again. Cressida
does not retry uncertain forwards automatically. If a forward is confirmed but
its comment cannot be queued, the comment remains as a separate pending message
for explicit restoration.

Forwarding checks source access and destination write permissions again when you
send. A deleted source or lost access revokes queued work. System notices,
polls, call or activity messages, private app responses and unavailable
snapshots cannot be forwarded. Service restrictions can also reject a forward,
with an explicit delivery error.

## Edit a sent message

Right-click one of your messages and choose **Edit Message**. Edit its text
directly in the conversation, then click **Save** or press **Enter**. Use
**Shift+Enter** for a new line, or **Escape** or **Cancel** to discard the edit.
The input supports pointer selection, clipboard shortcuts and undo.

Messages with image attachments can be edited even when newer messages follow
them. Saving changes the text without removing existing attachments. Your unsent
composer draft stays separate. Failed saves keep the editor and its text for
another explicit attempt; they never retry automatically.

With an empty, focused composer at the latest messages, **Up** opens the most
recent editable message you sent. Voice messages, polls, forwarded snapshots and
retained deleted messages cannot be edited.

## Delete a message

Right-click a message and choose **Delete Message**, then confirm **Delete**.
You can delete your own messages. In a server channel, **Manage Messages** also
lets you delete other authors' messages, including bot and webhook responses.
Cressida checks your current permission again when you confirm. A failed request
keeps the message and shows an error; it does not retry automatically.

Private app responses use their local dismiss action instead. See
[Slash commands](slash-commands.md) for app responses.

## Pin a message

Right-click a message and choose **Pin Message**. A confirmation shows the
channel name and a preview of the message as it appears in chat, including
formatting, images, embeds and stickers. Spoilers stay hidden, and your image
and embed display settings apply. Choose **Oh yeah. Pin it** to pin it or
**Cancel** to close without changes. Messages that are already pinned offer
**Unpin Message** instead; confirm with **Remove it please**.

You can pin messages in direct and group conversations. In a server channel, you
need **Pin Messages** or **Manage Messages**. Cressida checks your current
permission again when you confirm. After the pin succeeds, an open pins panel
refreshes. A failed request keeps the dialog open with an error and does not
retry automatically. System notices, private app responses and deleted messages
cannot be pinned.

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

People suggestions cover everyone in the current server or DM, online or
offline. Channel suggestions use channels currently loaded in the client. You
can also enter an ID, or type an exact name. Put names containing spaces in
double quotes, such as `from:"Alex Smith"`. If a name is ambiguous, choose a
suggestion or use its ID. Search stays within the current server or DM.

Use **Up** and **Down** to highlight a suggestion and **Enter** or **Tab** to
accept it. Press **Enter** without a highlighted suggestion to search.
**Escape** dismisses the menu while keeping your query. Open a result to jump to
its message.

Results group under headers naming their channel, with the channel's category
beside it. Each hit shows the author's avatar, name, server tag, role icon and
time like a regular message, and mentions show readable names in the accent
color used across chat. The panel header gives the result total; numbered page
buttons and Previous/Next arrows move through large result sets.

Recent searches appear beneath the filters, separately for each server or DM.
Choose one to restore it, or use the trash button beside **History** to clear
that list. Search history lasts for the current session and is cleared on
sign-out.

## Reply to someone

Hover a message and choose **Reply**. The composer shows who you are replying
to. The **@ ON** / **@ OFF** control switches the reply notification on or off.
Cressida remembers your last choice for new replies on that account, including
after restarting. It starts off until you change it; explicit mentions in your
message are independent of this control.

Channel and user mentions in reply previews show readable names, including your
local aliases for people.

Press **Escape** or use the reply row's close button to cancel the reply while
keeping your draft. If a picker or dialog is open, Escape dismisses it first.

Consecutive messages from the same person can share a header. A message
following your reply can join that group; each message keeps its own actions and
timestamp.

## Input methods

Cressida uses your desktop's Wayland input method, including Fcitx 5, in the
composer, message editor, search and settings fields. Your compositor must
provide text-input-v3 support, and your input-method service and language engine
must be running and configured. See
[Input-method setup](installation.md#input-methods).

Composition remains temporary until your input method commits it. Temporary text
stays out of the retained draft and settings values. While composing, **Enter**,
**Tab** and **Escape** do not send the message, choose an app suggestion or move
focus. Your input method supplies any candidate popup; Cressida reports the
caret position to your desktop.

## Mentions and emoji

Type `@` to choose a member, role, everyone or here. Type `:` followed by at
least two characters to search emoji. Suggestions can include Unicode and server
emoji. Searches ignore letter case. The list shows up to 50 matches; type more
of the name to narrow the results. With **Emoji image links** enabled,
suggestions also include custom emoji that need an image-link fallback, just
like the emoji picker.

- Use **Up** and **Down** to move through suggestions.
- Press **Enter** or **Tab** to insert the selected suggestion.
- Press **Escape** to dismiss the list.

Choosing a suggestion inserts it without sending the message. Mentions display
readable names and emoji display their images while you compose.

### Emoji and sticker details

Hover over an emoji or sticker in a message to see its name and a larger
preview. The preview closes when the pointer leaves. Click an emoji to open its
details, including its source server when Cressida knows it. **View Server**
opens that server; emoji from an unknown source still show their name.

Unicode emoji keep their complete sequence, including skin tones and joined
characters. Dragging across an emoji selects chat text instead of opening its
details. Emoji inside hidden spoilers stay hidden until you reveal the spoiler.

Click a sticker for its existing details, source and send controls. Close a
details card with **Escape**, its close button or a click outside it.

## Slash commands

Type `/` at the start of the composer to browse app commands. Select an app from
the left rail, choose a command and enter arguments directly in the composer.
The **Options** popup offers additional arguments and values. Press **Enter** to
send the completed command. See [Slash commands](slash-commands.md) for argument
types, suggestions and responses.

## Format your text

| Write                          | Result                                     |
| ------------------------------ | ------------------------------------------ |
| `**bold**`                     | Bold text                                  |
| `*italic*`                     | Italic text                                |
| `__underlined__`               | Underlined text                            |
| `~~crossed out~~`              | Strikethrough                              |
| `\|\|hidden text\|\|`          | A spoiler, revealed on click               |
| `` `inline code` ``            | Inline code                                |
| `[label](https://example.com)` | A labeled link                             |
| `> quoted text`                | A quote                                    |
| `# Heading`                    | A heading at the start of a line           |
| `-# Small text`                | Smaller, muted text at the start of a line |

Start a line with `> ` to quote it. Quoted text is muted, and its rule covers
any wrapped lines. The next line without a quote marker returns to ordinary
text. Prefix consecutive lines with `> ` to keep them in the same quote.

Use three backticks on their own lines for a code block. You can put a language
name after the opening backticks. Outside quotes and lists, received code blocks
also display their full content when the opening or closing backticks share a
line with code. Use the block's Copy button to copy its body with spacing and
line breaks preserved. Spoilers and strikethrough cover spaces within the
formatted text. Link underlines appear when you hover the link.

## Images and embeds

Pasted images can be uploaded from the composer. Multiple images in a message
appear together in a gallery. Links may show compact previews with a title,
description or image. Previews also show their author and footer icons when
provided. Their timestamps appear below the content using your local timezone
and 12-hour or 24-hour clock choice, including **Today at**, **Yesterday at** or
an explicit date. See [Images and video](media.md).

To remove a message's embeds, hover the message and click the **×** beside a
preview, or right-click the message and choose **Remove Embeds**. You can remove
embeds from your own messages, or from other people's messages when you have
**Manage Messages** permission in that channel.

The confirmation explains that removal affects everyone. Choose **Remove All
Embeds** to remove every embed on that message, or **Cancel** to keep them. The
message text and attachments stay in place. Hold **Shift** while clicking the
close control or choosing **Remove Embeds** to skip confirmation.

If removal fails, the embeds remain visible and Cressida shows an error.
Requests are never retried automatically.

## Catch up and save for later

Unread conversations use brighter names and an edge marker. Server and folder
number badges count mentions, while unmuted direct messages also add to their
conversation badge. These indicators update as channel details arrive after
sign-in and when new messages or read acknowledgements arrive. Muted channels
suppress ordinary unread styling; mentions can still show a badge. Muting a
server hides its ordinary unread dot in the server rail while its unmuted
channels keep their unread styling.

Use the [Inbox](inbox.md) for unread conversations and mentions across your
account. Message menus also let you bookmark a message or set a reminder.
