---
title: Capabilities and FAQ
description:
  Current support, known limitations and answers to common Cressida questions.
---

## What works today?

| Area          | Current support                                                |
| ------------- | -------------------------------------------------------------- |
| Platforms     | Linux with Wayland. No X11 window backend.                     |
| Messaging     | Text chat, replies, DMs, threads and forum conversations.      |
| Rich content  | Markdown, embeds, custom emoji, GIFs and stickers.             |
| Uploads       | Pasted images.                                                 |
| Media viewing | Image galleries and native received-video playback.            |
| Profiles      | Rich profiles and a profile panel in one-to-one DMs.           |
| Appearance    | Dark, Light and System modes; bundled and custom palettes.     |
| Desktop       | Tray, notification activation and reuse of the running window. |

## What is not supported yet?

Voice and video calls, arbitrary file uploads, file drag-and-drop and multiple
remembered accounts are not available yet. Some account, moderation and other
settings or actions remain incomplete. A visible unavailable control should not
be taken as a working feature.

## Is video supported?

Received video attachments can play in the native viewer. Video calls and
video-file uploads from the composer are separate features and are not currently
supported.

## Is it a browser wrapper?

No. The desktop client is written in C++23 with a native Wayland window and its
own rendering stack. External links and browser sign-in open in your default
browser. It does not bundle an embedded browser engine.

## Is it an official client?

No. Cressida is an independent, unofficial third-party project with no platform
affiliation or endorsement.

## Can I use it on X11, Windows or macOS?

The current window backend targets Linux and Wayland. Those other environments
are not supported.

## Are there portable downloads?

Follow [Installation](../installation/) for the supported routes and check
[published releases](https://github.com/Ly-sec/Cressida/releases) for available
artifacts. The Alpine musl CI build is a dynamically linked test build, not a
portable static download. These docs do not promise AppImage or Flatpak
packages.

## Does Cressida need zstd?

Yes. `libzstd` is a native dependency used for lossless animation frame caching.
See [Building from source](../building/) for the complete dependency list.

## Does closing the window quit?

With a tray host, it hides the window and stays connected. Use tray **Close** to
quit. Without a tray host, closing the window exits. See
[Desktop integration](../desktop/).

## Can I contribute?

Yes. Bug reports, documentation improvements and focused code contributions are
welcome. Start with [Contributing](../contributing/).
