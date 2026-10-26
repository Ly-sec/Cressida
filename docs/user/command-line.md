---
title: Command-line reference
description:
  Launch options and voice controls for Cressida, including tray startup,
  separate instances, offline demos and diagnostic logging.
---

```text
cressida [--new-instance | -n] [--start-minimized] [--demo] [--smoke-test]
         [--token-file PATH] [--log-level LEVEL]
cressida voice input-mute toggle|on|off
```

| Option                 | Behavior                                                                              |
| ---------------------- | ------------------------------------------------------------------------------------- |
| No options             | Start normally, or show/focus the running client.                                     |
| `--start-minimized`    | Start hidden in the tray; show the window after five seconds if no tray host appears. |
| `--new-instance`, `-n` | Start a separate client instead of activating the running one.                        |
| `--demo`               | Open offline sample conversations without account access.                             |
| `--smoke-test`         | Render an offline demo frame on an available Wayland compositor, then exit.           |
| `--token-file PATH`    | Read an account token from a file for advanced sign-in.                               |
| `--log-level LEVEL`    | Set terminal verbosity to `debug`, `info`, `warn` or `error`.                         |
| `--help`               | Print usage and exit.                                                                 |
| `--version`            | Print the client version and exit.                                                    |

`--start-minimized` affects the first ordinary launch. If a client is already
running, another ordinary launch requests Open instead. Demo and smoke-test
modes run independently and ignore hidden startup.

Terminal logging defaults to `info`. `--log-level` overrides
`CRESSIDA_LOG_LEVEL` for the launched process. Log files retain debug details
independently of terminal verbosity. Quit the existing client before relaunching
with a different log level; an ordinary repeat launch only activates it.

## Examples

```sh
cressida
cressida --start-minimized
cressida --new-instance
cressida --demo
cressida --log-level debug
```

Use your desktop's autostart settings to run the minimized command at login.
Cressida does not enable autostart itself.

## Voice controls

These commands control the ordinary running client without opening or focusing
its window. They do not start a client or read account tokens. Instances opened
with **--new-instance** are separate and are not targeted. Use voice commands on
their own, without launch options; `cressida voice --help` prints their usage.

| Command                            | Action                       |
| ---------------------------------- | ---------------------------- |
| `cressida voice input-mute toggle` | Toggle your microphone mute. |
| `cressida voice input-mute on`     | Mute your microphone.        |
| `cressida voice input-mute off`    | Unmute your microphone.      |

Input-mute works while an account is signed in, including between calls. Its
runtime setting is not saved across restarts or sign-ins. Server mute, deafen
and speaking restrictions still apply.

Commands wait at most two seconds for a reply from the running client:

| Exit status | Meaning                                                                                   |
| ----------- | ----------------------------------------------------------------------------------------- |
| `0`         | The running client applied the command.                                                   |
| `1`         | The client is unavailable, the command was rejected or communication failed or timed out. |
| `2`         | The command syntax is invalid.                                                            |

A timeout does not prove that an action was ignored. Commands are not retried
automatically. Check your microphone's mute state in the client before repeating
a toggle.

## Environment variables

| Variable              | Purpose                                                                              |
| --------------------- | ------------------------------------------------------------------------------------ |
| `CRESSIDA_TOKEN`      | Advanced account token input when no token file is supplied.                         |
| `CRESSIDA_ASSETS_DIR` | Override the complete asset directory, mainly for development or packaging.          |
| `CRESSIDA_DEMO_VIEW`  | Select an offline preview, such as `themes`, when using demo mode.                   |
| `CRESSIDA_LOG_LEVEL`  | Terminal verbosity: `debug`, `info`, `warn` or `error`; overridden by `--log-level`. |
| `XDG_CACHE_HOME`      | Absolute parent location for diagnostic logs; otherwise use `~/.cache`.              |
| `XDG_CONFIG_HOME`     | Parent location for custom theme files.                                              |
| `XDG_STATE_HOME`      | Parent location for session and UI state.                                            |
| `XDG_RUNTIME_DIR`     | Parent location for private instance coordination files.                             |

Do not put account tokens in shared scripts, logs or support reports. Use the
browser sign-in flow unless you need advanced token input.

See [Collect diagnostic logs](troubleshooting.md#collect-diagnostic-logs) for a
reproduction workflow and guidance on sharing logs.
