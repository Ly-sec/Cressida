---
title: Files and data
description:
  Where Cressida stores settings, themes and saved credentials, and what to back
  up.
---

## Locations

| Data                     | Default location                        |
| ------------------------ | --------------------------------------- |
| Session and UI state     | `~/.local/state/cressida/session.json`  |
| Custom themes            | `~/.config/cressida/themes/`            |
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

Saved login credentials belong in the desktop keyring. Signing out removes the
saved login. Closing or restarting the client does not sign out.

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
