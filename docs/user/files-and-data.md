---
title: Files and data
description:
  Locations and retention for settings, themes, saved credentials and diagnostic
  logs.
---

## Locations

| Data                     | Default location                        |
| ------------------------ | --------------------------------------- |
| Session and UI state     | `~/.local/state/cressida/session.json`  |
| Custom themes            | `~/.config/cressida/themes/`            |
| Diagnostic logs          | `~/.cache/cressida/logs/`               |
| Saved credentials        | Your desktop's Secret Service keyring.  |
| Instance lock and socket | `cressida/` beneath `$XDG_RUNTIME_DIR`. |

`XDG_STATE_HOME` replaces `~/.local/state`, and `XDG_CONFIG_HOME` replaces
`~/.config`, when those variables are set.

If `XDG_RUNTIME_DIR` is unset, instance coordination uses `/run/user/UID` when
available, or a private `/tmp/cressida-runtime-UID` directory. These temporary
files coordinate running processes. They are not account backups.

## What is saved?

Local UI state includes navigation and account preferences. Color mode, palette
and supported settings are kept per account. Custom themes remain separate TOML
files; the state file stores the selected palette's ID, not its contents.

Voice device choices, input/output volume, activation mode and activity
threshold are also saved per account. Active calls, mute/deafen state,
microphone audio and voice-session encryption keys are not saved.

Saved login credentials belong in the desktop keyring. Signing out removes the
saved login. Closing or restarting the client does not sign out.

Slash-command lists, inline arguments, selected command files and frequently
used commands stay in memory for the current session. They are not saved in the
account's UI state. See [Slash commands](slash-commands.md).

## Diagnostic logs

`cressida.log` contains the current diagnostic output. When it reaches 1 MiB,
Cressida rotates it to `cressida.log.1`, replacing the previous backup. Each
generated log file is capped at 1 MiB. These files retain recent diagnostics,
not a permanent activity archive. Log lines are capped at 8 KiB and escape
control characters.

An absolute `XDG_CACHE_HOME` replaces `~/.cache` for these files. Log files use
owner-only permissions (`0600`) in a private directory (`0700`). A separate
`.lock` coordinates file access; other instances use terminal output while a
writer owns it. If file logging is unavailable, terminal logging continues.
Close all instances before removing log files or their lock.

Files include debug details regardless of terminal verbosity. They do not store
chat history or replace the optional message history settings below. See
[Collect diagnostic logs](troubleshooting.md#collect-diagnostic-logs) before
sharing a report. You do not need to back them up to preserve account settings.

## Backups

Close Cressida before copying its state file. Back up custom themes separately.
Do not share state or keyring contents as a public issue attachment. Backups of
local settings do not replace the chat service's account or message storage.

## Message history options

The optional **Keep deleted messages** and **Keep edit history** settings retain
that information in the current client session. They are not a permanent
archive. Signing out or closing Cressida clears the retained history; disabling
either setting clears the corresponding retained information.

## Website

This documentation site is static. Its color choice is stored in your browser.
Documentation search uses a locally generated index. The site does not provide
account sign-in or ask for your chat credentials.
