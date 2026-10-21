---
title: Command-line reference
description:
  Launch options for Cressida, including tray startup, separate instances and
  offline demos.
---

```text
cressida [--new-instance | -n] [--start-minimized] [--demo] [--smoke-test]
         [--token-file PATH]
```

| Option                 | Behavior                                                                              |
| ---------------------- | ------------------------------------------------------------------------------------- |
| No options             | Start normally, or show/focus the running client.                                     |
| `--start-minimized`    | Start hidden in the tray; show the window after five seconds if no tray host appears. |
| `--new-instance`, `-n` | Start a separate client instead of activating the running one.                        |
| `--demo`               | Open offline sample conversations without account access.                             |
| `--smoke-test`         | Render an offline demo frame on an available Wayland compositor, then exit.           |
| `--token-file PATH`    | Read an account token from a file for advanced sign-in.                               |
| `--help`               | Print usage and exit.                                                                 |
| `--version`            | Print the client version and exit.                                                    |

`--start-minimized` affects the first ordinary launch. If a client is already
running, another ordinary launch requests Open instead. Demo and smoke-test
modes run independently and ignore hidden startup.

## Examples

```sh
cressida
cressida --start-minimized
cressida --new-instance
cressida --demo
```

Use your desktop's autostart settings to run the minimized command at login.
Cressida does not enable autostart itself.

## Environment variables

| Variable              | Purpose                                                                     |
| --------------------- | --------------------------------------------------------------------------- |
| `CRESSIDA_TOKEN`      | Advanced account token input when no token file is supplied.                |
| `CRESSIDA_ASSETS_DIR` | Override the complete asset directory, mainly for development or packaging. |
| `CRESSIDA_DEMO_VIEW`  | Select an offline preview, such as `themes`, when using demo mode.          |
| `XDG_CONFIG_HOME`     | Parent location for custom theme files.                                     |
| `XDG_STATE_HOME`      | Parent location for session and UI state.                                   |
| `XDG_RUNTIME_DIR`     | Parent location for private instance coordination files.                    |

Do not put account tokens in shared scripts, logs or support reports. Use the
browser sign-in flow unless you need advanced token input.
