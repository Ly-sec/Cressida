---
title: Troubleshooting
description: Resolve common startup, tray, sign-in, theme and media problems.
---

## The window does not open

- Confirm you are in a Wayland session with working graphics drivers. There is
  no X11 window backend.
- If Cressida is in the tray, choose **Open** or launch `cressida` again.
- With `--start-minimized`, allow five seconds for the missing-tray fallback.
- Run `cressida --version` to confirm that the executable can be found.

For an isolated interface check, try `cressida --demo`. It needs a Wayland
compositor but does not need account access.

## Closing the window exits instead of hiding

Cressida needs a compatible tray host to stay reachable after hiding. Enable
your desktop's StatusNotifier-compatible tray support or extension. Without a
host, window close intentionally exits. Tray **Close** always exits.

## Launching again does not focus the client

A repeat launch asks the running client to open and requests desktop activation.
The compositor decides whether to grant focus. Check that both launches use the
same user session and runtime directory, and that you are not passing
`--new-instance` or `-n`.

Do not delete runtime coordination files while an instance is running. Quit it
normally before investigating leftover files.

## Sign-in is not remembered

Check that a Secret Service provider is running and unlocked. GNOME Keyring or
KeePassXC with Secret Service enabled can provide it. Installing `libsecret`
alone is insufficient. See [Sign in](sign-in.md).

If the browser does not open for sign-in or links, check your default browser
and `xdg-open` installation.

## System colors do not change

Select **System** under Appearance. Fixed Light or Dark choices override the
desktop preference. Ensure an XDG desktop portal with a working Settings backend
is available. An absent or unknown desktop preference falls back to dark.

## A custom theme does not load

Open **Cressida > Themes** and read the reported error. Check the filename,
`version = 1`, both light and dark variants, and every required color. Unknown
keys are rejected. The last valid version remains active during invalid edits.
Use the [complete starter](custom-themes.md#complete-starter) to compare.

## Account restrictions or security checks

If the service asks for verification, restricts messaging or challenges an
action, Cressida pauses the affected writes and explains the next step. Complete
the requested check or review **Account Standing** in the official app, then
reconnect Cressida before trying again.

Queued actions that are stopped show an error. Cressida does not automatically
repeat them after the restriction clears. Check whether a message arrived before
using **Restore** to put a failed message back in an empty composer.

A rate-limit response pauses requests for the service's indicated duration. Long
waits return an error so other work can continue. Wait before trying again.

These controls do not remove account violations or establish why an account was
flagged. Include the exact warning and the action immediately before it when
reporting a problem. Never share account tokens, challenge data or private
message contents.

## Media or emoji do not display correctly

- Install a color emoji font and check your Fontconfig setup.
- Keep the complete Cressida assets directory with the executable.
- For video playback, check the package's libmpv and FFmpeg dependencies.
- For saving images, check that your desktop portal's FileChooser backend works.

## A source build fails

Use the [build dependency reference](building.md). In particular:

- The compiler **and standard library** must support the required C++23
  features.
- The linked libcurl must provide HTTPS and secure WebSockets (`wss`).
- stb must include `stb_image_resize2.h`; the older resize header is
  insufficient.
- `libzstd` is required for animation frame caching.

## Collect diagnostic logs

Cressida writes diagnostic logs automatically during startup and use. The
current file is `~/.cache/cressida/logs/cressida.log`; `cressida.log.1` contains
the previous rotation. An absolute `XDG_CACHE_HOME` changes the parent cache
location. See [Files and data](files-and-data.md#diagnostic-logs) for retention
and access.

Log files include debug details even when the terminal only shows informational
messages, warnings and errors. To also see debug details in the terminal:

1. Quit the running client using **Close** in its tray menu or your Quit
   shortcut.
2. Start it from a terminal:

   ```sh
   cressida --log-level debug
   ```

3. Reproduce the problem, note the approximate time, then quit normally to flush
   the log before copying it.

Launching again normally focuses the existing client. It does not change that
process's log level. If a separate instance already owns the log file, another
instance reports that it is using terminal logging instead.

Logs cover startup and shutdown, connections, window and desktop services,
credential operations, local state, media failures and cache usage. Network
entries include a local request reference, an action, an anonymized route,
status codes, timing and retry information. They omit message contents,
credentials, request bodies and raw headers.

Review logs and terminal captures before sharing them. Terminal startup errors
can contain additional error details. Include only the relevant period and
remove any personal information. Logging does not establish the cause of an
account restriction.

## Report a problem

[Open an issue](https://github.com/Ly-sec/Cressida/issues) with your
distribution, desktop/compositor, installation method, Cressida version, steps
to reproduce and what you expected. Add a screenshot when useful. Remove
personal conversations, account tokens and other credentials from anything you
share.
