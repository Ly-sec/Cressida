---
title: Slash commands
description:
  Find an app's commands, choose arguments and run them from the native
  composer.
---

## Find a command

Type `/` at the beginning of the composer. The command picker opens above your
draft with command names, descriptions and the app that provides each command.
Keep typing to search names, descriptions or apps. Subcommands appear together,
such as `/logging moderation`.

The rail on the left lets you browse commands by app. Select an app's icon to
show its commands, **All commands** to show every available app, or **Frequently
used** to see commands you have run during this session. The app rail and
command list scroll separately.

- Use **Up** and **Down** to highlight a command.
- Use **Left** and **Right** to switch apps or categories.
- Press **Enter** or **Tab**, or click a command, to choose it.
- Press **Escape** to close the picker and keep your draft.

Commands depend on the current server or private conversation, the apps
installed there or on your account, and your permissions. A command may also be
restricted to particular channel types or age-restricted channels. A failed list
shows **Retry loading commands**. Selecting it loads the list again.

## Enter arguments and run

Choosing a command keeps you in the composer. Its name and description appear in
a compact strip, with argument fields beside the command name. Choosing a
command does not run it.

Required arguments are selected first. Optional arguments appear in the
**Options** popup above the composer. Click an option or press **Tab** to add
it. The **+ options** field lets you find and add more arguments. An optional
argument you leave unset is omitted when you send.

Completed arguments appear as chips. Click one to edit it, use **Tab** to move
forward or **Shift+Tab** to move back. **Backspace** in an empty argument moves
back through the command. Longer commands wrap within a scrollable composer; the
active argument stays visible.

Text, whole numbers, decimal numbers, true/false choices and app-defined choices
have separate validation. For people, roles and channels, search by name and
choose a suggestion, or enter an ID. People searches can also look up server
members. Channel suggestions respect the channel types the command accepts.

Some apps provide suggestions while you type an argument. Suggestions appear
above the composer. Use **Up** and **Down**, then **Enter** or **Tab**, to
choose one. Choosing a value does not run the command. **Enter** sends after you
accept the value; **Tab** accepts it and moves to another argument. Use
**Shift+Enter** for a new line in a text argument. Native editing, undo and
input methods remain available.

For an attachment argument, choose a local file with the desktop file chooser.
Use the attachment chip's remove control to remove it. Files can be up to 10 MiB
each, with up to ten files and 20 MiB total per command. The app or service may
impose additional limits.

Press **Enter** to send the command once its arguments are ready. An untouched
optional-only command can be sent immediately. **Ctrl+Enter** also submits.
Cressida checks required arguments, value limits and current permissions first.
Press **Escape** to dismiss suggestions, then again to cancel the command, or
use the close control in its header. Cancellation keeps your original slash
draft. A completed command clears that draft; newer text is preserved. Reply
selections and ordinary message attachments are retained for your next message.

## Responses and errors

App responses appear in the conversation. Private responses show **Only you can
see this** and can be dismissed locally. An app can also open a native follow-up
text form. See [Chat](chat.md) for ordinary messages and replies.

An inline error keeps the argument values and selected files in the composer.
Cressida does not automatically repeat a command. If confirmation does not
arrive, the composer explains that the command may already have run. Check the
conversation before pressing **Enter** again.

The picker supports app-defined chat commands. Message and user context commands
and local text shortcuts are not available yet. Follow-up forms currently
support text fields; other field types report an explicit error. Frequently used
commands are kept only for the current session and clear on sign-out. They do
not import another client's usage history.

To write text beginning with a literal slash, escape it with a backslash, such
as `\/example`. Unknown slash text is kept in your draft instead of being sent
as an ordinary message.
