---
title: Custom themes
description:
  Create light, dark and translucent Cressida palettes with a live-reloaded TOML
  file.
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
each. Color values must be quoted hex strings: `#RRGGBB` for an opaque color or
`#RRGGBBAA` with alpha last for transparency. You can mix both formats in a
theme.

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

| Keys                                                                          | Purpose                     |
| ----------------------------------------------------------------------------- | --------------------------- |
| `rail`                                                                        | Server rail background.     |
| `danger`, `destructive`, `success`                                            | Status and action colors.   |
| `keyword`, `string`, `number`, `function`, `type`, `property`, `preprocessor` | Syntax palette.             |
| `added`, `removed`                                                            | Added and removed content.  |
| `hover`, `on_hover`                                                           | Button hover fill and text. |

Omitted optional colors use Cressida's fallback values. `hover` defaults to the
variant's `property` color and `on_hover` to its `ink` color. Set both to
customize button hover colors independently. Check text against every surface it
appears on, especially `ink` against `accent` and `on_hover` against `hover`, in
both variants.

## Transparent surfaces

The last two digits of `#RRGGBBAA` control opacity: `00` is fully transparent,
`80` is about half opaque and `ff` is fully opaque. Six-digit colors remain
fully opaque, so existing themes keep their appearance.

For example, replace these colors inside your theme's `[dark]` section:

```toml
background = "#17181d00"
sidebar = "#1e2027cc"
rail = "#14151acc"
panel = "#24262fb3"
raised = "#2e313de6"
```

Surfaces overlap. A translucent panel over an opaque background still shows the
background underneath it. Set `background` to a transparent color if you want
those surfaces to reveal the desktop. Adjust other surface colors as needed, and
keep text colors opaque for readability.

Alpha changes reload while Cressida runs, including changes that leave the RGB
digits unchanged. Blur is a separate compositor setting; theme transparency does
not enable it or make transparent areas pass clicks to other windows.

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
[Files and data](files-and-data.md).
