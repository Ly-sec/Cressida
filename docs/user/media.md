---
title: Images and video
description:
  Upload pasted images, browse galleries and use the native image and video
  viewers.
---

## Send an image

Copy an image to the clipboard, focus the chat composer and paste it with
**Ctrl+V**. Review the attachment before sending. Image uploads are supported;
arbitrary file selection, file drag-and-drop and video-file uploads are not
currently available in the composer.

Apps can request a local file through an attachment argument in a
[slash command](slash-commands.md). These files use the inline command arguments
and desktop file chooser.

## Randomize outgoing filenames

Open settings, then **Cressida > General > Uploads** and enable **Randomize
media filenames**. It is off by default and saved per account.

When enabled, supported outgoing media get a random filename. The extension and
media contents stay the same, and a spoiler prefix is preserved. This changes
the upload name, not metadata inside the image or the original file on disk.

## View an image

Click an image to open the viewer. Multiple images in a message use a shared
gallery layout. In the viewer:

- **Left / Right** moves between images.
- **+ / -** zooms in or out; **0** restores 100% zoom.
- **Ctrl+C** copies the current image.
- **Escape** closes the viewer.

Right-click an enlarged image for **Copy Image**, **Save Image**, **Copy Media
Link** and **Open Media Link**. Saving uses your desktop's file selection
service where available. Opening a media link uses your external browser.

## Memory use

Cressida stores larger animation frames with lossless compression, preserving
their resolution, colors and playback timing. Older media is released as the
image caches fill. Revisiting it may download it again. Visible images and local
upload previews stay available.

If many images load at once, some may appear a little later to keep memory use
bounded. Their animation data stays intact while they wait.

## Play video

Received video attachments can play in the native player. Use its controls to
play, pause, seek, adjust volume, mute or enter its expanded view. Ordinary
videos do not start automatically.

While the player is active, **Space** toggles playback, **M** toggles mute,
**F** toggles its expanded view and **Left / Right** seeks. **Escape** exits the
expanded view or closes the player. Background chat controls stay covered while
a media viewer is open.

Video attachment playback is separate from calls. [Voice chat](voice.md) is
available; camera calls and screen sharing are not supported yet.
