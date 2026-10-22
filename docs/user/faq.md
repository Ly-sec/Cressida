---
title: Capabilities and FAQ
description:
  Current support, known limitations and answers to common Cressida questions.
---

## What works today?

| Area          | Current support                                                          |
| ------------- | ------------------------------------------------------------------------ |
| Platforms     | Linux with Wayland. No X11 window backend.                               |
| Messaging     | Text chat, replies, DMs, threads and forum conversations.                |
| Inbox         | Unread conversations, mentions, synced bookmarks and reminders.          |
| Rich content  | Markdown, embeds, custom emoji, GIFs and stickers.                       |
| Uploads       | Pasted images.                                                           |
| Media viewing | Image galleries and native received-video playback.                      |
| Profiles      | Rich profiles, equipped cosmetics and a profile panel in one-to-one DMs. |
| Appearance    | Dark, Light and System modes; bundled and custom palettes.               |
| Desktop       | Tray, notification activation and reuse of the running window.           |

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

## Can my account be restricted?

Yes. Users have reported spam flags and temporary restrictions while using
Cressida. We have not established what triggered those actions. Rate-limit
handling and other request safeguards do not guarantee protection from account
restrictions or suspension.

Other native-client projects have reported restrictions or advised caution
around:

- Starting new direct conversations.
- Joining or leaving servers.
- Managing friends or changing account details.
- Frequent disconnects and reconnects.

These examples come from
[peer reports](https://github.com/uowuo/abaddon/issues/394#issuecomment-3682402026)
and [maintainer guidance](https://github.com/uowuo/abaddon#the-spam-filter).
They are possible risk factors, not confirmed Cressida triggers or a complete
list. Other features and existing conversations are not guaranteed to avoid
account flags. We do not have evidence establishing a new-account or
conversation-history threshold.

If the service requests verification or a CAPTCHA, complete it in the official
app. Avoid repeatedly retrying a challenged action. Cressida pauses affected
writes and retains failed messages for manual recovery; see
[Account restrictions or security checks](troubleshooting.md#account-restrictions-or-security-checks).

Review **Account Standing** in the official app for the specific violation and
its restrictions. If you believe a violation was incorrect, use its review link.
A temporary restriction can end while the violation remains on the account. See
the
[official explanation of account standing and reviews](https://support.discord.com/hc/en-us/articles/18210965981847-Discord-Warning-System).

## Can I use it on X11, Windows or macOS?

The current window backend targets Linux and Wayland. Those other environments
are not supported.

## Are there portable downloads?

Follow [Installation](installation.md) for the supported routes and check
[published releases](https://github.com/Ly-sec/Cressida/releases) for available
artifacts. The Alpine musl CI build is a dynamically linked test build, not a
portable static download. These docs do not promise AppImage or Flatpak
packages.

## Does Cressida need zstd?

Yes. `libzstd` is a native dependency used for lossless animation frame caching.
See [Building from source](building.md) for the complete dependency list.

## Does closing the window quit?

With a tray host, it hides the window and stays connected. Use tray **Close** to
quit. Without a tray host, closing the window exits. See
[Desktop integration](desktop.md).

## Can I contribute?

Yes. Bug reports, documentation improvements and focused code contributions are
welcome. Start with [Contributing](contributing.md).
