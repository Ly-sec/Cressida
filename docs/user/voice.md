---
title: Voice chat
description:
  Join voice channels, choose audio devices and configure microphone activation.
---

## Join a conversation

Choose a server, then click a voice channel in its sidebar or use its phone
button. You need permission to view and connect to the channel. If you can
connect but cannot speak, Cressida joins with your microphone muted.

In a direct or group conversation, use the header's phone button to join an
existing call. Starting a call that rings other people and incoming-call
notifications are not implemented.

Connected people appear beneath their voice channel, with avatars and mute or
deafen indicators. In your current call, a green avatar ring, emphasized name
and active microphone icon show who is speaking. The indicator clears when
speech stops.

When you are alone, the sidebar shows **Voice connected** and **No other
participants**. Your microphone can show local activity after joining. Audio is
transmitted only when the encrypted group is ready for another participant.

Use the sidebar controls to mute your microphone, deafen yourself, open voice
settings or disconnect. Mute and deafen stay highlighted while active. The phone
button at the upper right disconnects the call. Hover over the connection status
to see the full channel and server name. Deafening stops both microphone
transmission and received call audio. Changing the selected text conversation
does not leave your call. You can also change microphone mute using the
[voice control commands](command-line.md#voice-controls).

## Choose devices and volume

Open settings and choose **Voice & Video**. Select an **Input device** for your
microphone and an **Output device** for received voices. **System default** uses
the audio service's default device. **Refresh devices** updates the choices
after plugging in a device or changing your desktop's audio setup.

Input and output volume each range from **0%** to **200%**. Values above 100%
amplify the audio and can distort loud sounds.

Devices, volumes, input mode and activity threshold are saved separately for
each account. An unavailable saved device stays visible as **Unavailable saved
device**. Select a working device and join again if audio setup fails. Calls,
mute and deafen are not restored when you restart or sign in again.

## Choose when the microphone transmits

Under **Microphone activation**, choose an **Input mode**:

- **Voice activity** sends audio when the microphone level exceeds **Activity
  threshold**. Raise the threshold to ignore quieter background sounds or lower
  it if speech is being cut off.
- **Push to talk** sends audio while you hold **Ctrl+Shift+Space**. Change this
  shortcut under **Keybinds**.

The built-in shortcut works while Cressida has keyboard focus. Releasing the
shortcut, switching to another window, opening settings or hiding Cressida
releases that hold.

With voice activity selected, a connected call continues when the window is
hidden in the tray. Use **Mute microphone** or **Disconnect voice** when you
want to stop transmitting.

## Test your microphone

Before joining a call, choose **Start test** under **Microphone test**. Speak to
hear your microphone through the selected output and watch **Microphone level**.
This is a local test; it does not join a call or send audio to other people.
Choose **Stop test** when finished. A test cannot run alongside a call.

Opening voice settings or refreshing the device list does not start microphone
capture. Capture starts after you explicitly start a test or join a call whose
voice server connection is ready. An empty channel can meter your microphone
locally while it waits for group keys; it sends no audio during that wait.

## Requirements and current limits

Voice needs a working PulseAudio service or PipeWire's PulseAudio-compatible
service. If an audio device becomes unavailable, choose a working device and
join again. A failed network connection also requires an explicit rejoin;
Cressida does not automatically resume the call.

Playback retains up to 32 speaker streams at a time. Streams idle for five
seconds are released, so very large conversations may not play every speaker.

Camera calls, screen sharing, Stage channels, echo cancellation and noise
suppression are not available. Use headphones to reduce feedback. See
[Troubleshooting](troubleshooting.md#voice-does-not-connect-or-has-no-audio) for
audio setup problems.
