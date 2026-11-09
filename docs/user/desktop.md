---
title: Desktop integration
description:
  Use the tray, start minimized and return to the running window from
  notifications.
---

## Tray menu

When your desktop has a compatible tray host, Cressida provides these actions:

| Action  | Behavior                                |
| ------- | --------------------------------------- |
| Open    | Show or focus the running window.       |
| Restart | Restart the client without signing out. |
| Close   | Exit the application.                   |

While you are [sharing a screen](screen-sharing.md), the menu also provides
**Stop Sharing**. Sharing continues when the main window is hidden; use that
action to stop without reopening the window.

Closing the window with a tray host available hides it and keeps the connection
alive. Without a tray host, closing the window exits normally. If the tray host
disappears while the window is hidden, Cressida restores the window.

A connected [voice call](voice.md) also continues while the window is hidden.
Voice activity can keep transmitting; mute or disconnect when you want to stop.
Hiding the window releases push to talk.

Tray support depends on your desktop. Some desktops need a compatible tray
extension or panel applet.

## Start hidden

```sh
cressida --start-minimized
```

This starts Cressida hidden in the tray. If no tray host appears within five
seconds, the window opens so you can still reach it. The flag applies to that
launch; it is not a saved preference.

To launch this way when you sign in to your desktop, add the command to your
desktop's autostart settings. Cressida does not install an autostart entry
itself.

## Return to the running client

Open Cressida from your launcher or run `cressida` again. The new launch asks
the existing client to show or focus its window, preserving the conversation,
settings and drafts.

Use `cressida --new-instance` or `cressida -n` when you deliberately want a
separate instance. This is not an account-switching feature.

## Notifications

With a desktop notification service available, notifications can bring you back
to the client. Clicking a valid notification restores a hidden window or asks
the desktop to focus the existing one. Your compositor controls whether focus
can be granted.

Custom emoji in notification text use readable `:name:` labels because system
notification text does not render the same way as a chat message.

### Sound sets

Open **Settings > Notifications > Interface sounds** to choose **Soft**,
**Crisp** or **Warm**. Soft is the default. Your choice is saved separately for
each account.

**Sound volume** adjusts all interface cues and previews from 0% to 100%. It
starts at 50%; 0% silences them. This per-account setting does not change call,
video or voice-message volume.

The **Notification sounds** switch enables or disables all interface sounds.
Every event also has its own switch and a play glyph beside its heading to
preview the sound. Previews let you compare sounds even when their switches are
off. All fourteen events start enabled:

- Messages and mentions, with a mention cue replacing the ordinary message cue.
  Due reminders use the message cue.
- Microphone mute and unmute, including voice commands.
- Output mute and unmute.
- Your voice connection joining and leaving.
- Other participants joining and leaving your current call.
- Your screen share starting and stopping.
- Serious errors when voice or screen-share connections fail.
- Serious warnings when an audio device failure interrupts a call.

Message and reminder sounds follow desktop-notification eligibility, including
mute preferences and Do Not Disturb. Participant sounds also respect Do Not
Disturb. Explicit voice controls still provide feedback in Do Not Disturb.
Deafening silences other cues and previews, but keeps output mute and unmute
feedback audible.

Join and sharing-start cues wait for a ready connection. Initial room rosters,
ordinary background-request errors, opening a screen preview and successful
voice rekeying do not play cues. Bursts are bounded; stale cues are dropped
rather than played long after their event. Changing sound settings or signing
out cancels queued playback.

## Window decorations

In **Advanced**, use **Desktop decorations** to request your compositor's window
decorations. Disable it to request Cressida's own controls. The compositor
decides which decoration mode it supports. The Titlebar controls settings let
you adjust the available client-side buttons and their placement.

## Window rules

Cressida's Wayland application ID is `cressida`. Use `^cressida$` when your
compositor matches application IDs with regular expressions. The installed
launcher is `cressida.desktop` and the icon name is `cressida`.

Update rules and pinned launcher shortcuts that still refer to
`dev.lysec.Cressida`. Fully exit and reopen Cressida after updating the client
to use the new application ID.
