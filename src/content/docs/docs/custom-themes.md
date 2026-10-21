---
title: Custom themes
description:
  Create a light and dark Cressida palette with a live-reloaded TOML file.
---

## Create a theme

1. Open **Cressida > Themes** in settings and copy the starter TOML.
2. Create the themes directory if it does not exist:

   ```sh
   mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/cressida/themes"
   ```

3. Save your theme there as `my-theme.toml`.
4. Return to the Themes page and select it.

Cressida watches the directory and reloads valid changes while it runs. You do
not need to restart after editing colors.

## Complete starter

Every theme needs both `[dark]` and `[light]`, with all eight required colors in
each. Color values must be quoted six-digit hex strings.

```toml
version = 1
name = "My theme"
description = "A soft violet palette."
author = "Your name"

[dark]
background = "#17181d"
sidebar = "#1e2027"
panel = "#24262f"
raised = "#2e313d"
foreground = "#eeeff5"
muted = "#989bae"
accent = "#c6b5f5"
ink = "#211a36"

[light]
background = "#f5f5f7"
sidebar = "#e8e9ee"
panel = "#dcdee6"
raised = "#cdd0dc"
foreground = "#20222c"
muted = "#616678"
accent = "#6d4db5"
ink = "#ffffff"
```

## Color reference

| Required color | Used for                        |
| -------------- | ------------------------------- |
| `background`   | Main background surfaces.       |
| `sidebar`      | Sidebar surfaces.               |
| `panel`        | Panel and control surfaces.     |
| `raised`       | Raised or hovered surfaces.     |
| `foreground`   | Main text.                      |
| `muted`        | Secondary text.                 |
| `accent`       | Accent controls and highlights. |
| `ink`          | Text drawn on an accent fill.   |

Optional colors, inside either variant:

| Keys                                                                          | Purpose                    |
| ----------------------------------------------------------------------------- | -------------------------- |
| `rail`                                                                        | Server rail background.    |
| `danger`, `destructive`, `success`                                            | Status and action colors.  |
| `keyword`, `string`, `number`, `function`, `type`, `property`, `preprocessor` | Syntax palette.            |
| `added`, `removed`                                                            | Added and removed content. |

Omitted optional colors use Cressida's fallback values. Check text against every
surface it appears on, especially `ink` against `accent`, in both variants.

## Names, limits and overrides

The filename without `.toml` is the palette ID. It must contain only lowercase
letters, digits, hyphens and underscores, with at most 64 characters. `name` is
the label shown in settings. `description` and `author` are optional.

Files must fit within 64 KiB; keep at most 256 themes in a directory. Unknown
keys, invalid colors, missing variants or missing required fields reject the
whole file. If an edit is incomplete or invalid, Cressida keeps the last valid
version and shows an error on the Themes page.

A custom file with the same ID as a bundled palette overrides it. Removing the
file restores the bundled palette or default fallback. Cressida remembers the
selected ID so the custom theme can return when you restore its file.

Themes are ordinary user-owned files. For backup locations, see
[Files and data](../files-and-data/).
