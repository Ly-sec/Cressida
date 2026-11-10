---
title: Voice messages
description:
  Record and send a microphone recording, or play received voice messages.
---

## Record a message

Click the **microphone** icon before the GIF, sticker and emoji icons on the
right side of the chat bar. Recording starts inline in the composer, with an
elapsed time, trash, stop and send controls. Click **Stop recording** when you
are finished, then **Send recording** to post it, or **Discard recording** to
delete the local take. Stopping never sends it. The recorder uses the normal
single-line chat-bar height. There is no recording dialog or separate preview.

Recordings use your saved input device and input volume from
[Voice & Video](voice.md#choose-devices-and-volume). They capture the microphone
only, not call audio or desktop sound. Call mute, voice activity detection and
push-to-talk do not control this separate, explicitly started recorder.

Recording stops if the window loses focus, is hidden, or channel access changes.
Sending still requires an explicit click. Discarding the recorder, changing
conversations, opening settings or signing out discards the local take.
Recordings are not saved to disk or restored after restarting.

Each message contains one Ogg Opus recording, up to 20 minutes and 20 MiB. Text,
images, stickers and reply selections already in your composer stay unchanged.
Voice messages cannot include those items or be edited after sending.

Server channels require permission to write messages and **Send Voice
Messages**. Threads also need their normal posting access. Unavailable or
unknown permissions disable recording and sending; permission recovery never
sends a recording automatically.

If delivery is not confirmed, check the conversation before trying again.
**Restore** returns the stopped recording to the composer without starting the
microphone or replacing your ordinary composer draft. Sending again requires
another explicit **Send** click. Unconfirmed recordings remain in memory for the
current session only.

## Listen to a message

Click the play icon on a voice message to listen. Click it again to pause, or
click its waveform during playback to seek. Hovering the waveform highlights the
dots up to your pointer and shows the destination time, without a button fill.
The speed control cycles through 1X, 1.5X and 2X. Click the speaker to open a
vertical volume control; drag it or use Up/Down to adjust, and press Escape or
click outside to close it. Very narrow cards omit the extra speed and volume
controls. Recordings never play automatically. Only one voice message plays at a
time.

Voice-message playback starts at 100% volume and uses the system's default audio
output. Video playback keeps its separate default volume. Changing
conversations, opening settings or another dialog, losing focus, or hiding the
window stops playback. A failed recording shows a playback error rather than
retrying automatically.

Offline checks exercise synthetic recording, encoding and native controls. They
do not establish live message delivery or microphone compatibility on every
desktop.
