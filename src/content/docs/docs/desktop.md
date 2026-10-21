---
title: Desktop integration
description:
  Use the tray, start minimized and return to the running window from
  notifications.
---

## Tray menu

When your desktop has a compatible tray host, Cressida provides three actions:

| Action  | Behavior                                |
| ------- | --------------------------------------- |
| Open    | Show or focus the running window.       |
| Restart | Restart the client without signing out. |
| Close   | Exit the application.                   |

Closing the window with a tray host available hides it and keeps the connection
alive. Without a tray host, closing the window exits normally. If the tray host
disappears while the window is hidden, Cressida restores the window.

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

## Window decorations

In **Advanced**, use **Desktop decorations** to request your compositor's window
decorations. Disable it to request Cressida's own controls. The compositor
decides which decoration mode it supports. The Titlebar controls settings let
you adjust the available client-side buttons and their placement.
