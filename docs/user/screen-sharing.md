---
title: Screen sharing
description:
  Share a screen or window, choose application audio and watch a live stream.
---

Screen sharing is experimental. A local preview confirms that Cressida can
capture the selected source. It does not confirm that a stream has connected or
that another participant can receive it. Live call compatibility still needs
confirmation beyond the automated tests.

## Share a screen or window

1. Join a [voice conversation](voice.md). In a server, you also need permission
   to share video in that voice channel.
2. Choose the screen sharing button in the voice controls. Your desktop's source
   chooser opens first.
3. Select a screen or window. After its first frame arrives, **Screen Share
   Picker** opens with the preview under **What you're streaming**.
4. Under **Stream Settings**, choose the resolution, frame rate, content type
   and audio sources.
5. Choose **Go live**. The picker closes and returns you to chat with a small
   preview and connection status. **Starting screen share…** means setup is
   still pending; wait for **LIVE** before assuming publication is ready.

Your desktop grants access only after you choose a source. Cancelling the
desktop chooser returns directly to chat. Cancelling the settings before
publication stops the local capture. Cressida remembers quality and audio
preferences per account, but it does not restore a captured source or start
sharing automatically after sign-in.

## Choose quality

The picker provides 480, 720, 1080, 1440 and 2160 resolution choices, with 15,
30 or 60 frames per second. **More options** also provides **Use source
resolution**. Availability depends on the current account and server. A selected
limit does not enlarge a smaller source, and actual quality can decrease when
the connection or encoder cannot keep up.

- **Prefer Smoothness** reduces resolution under load to preserve frame rate.
- **Prefer Clarity** reduces frame rate under load to keep text and still images
  sharper.

Under **More options**, **Prefer hardware encoding** tries available hardware
encoders and falls back to software when hardware setup is unavailable. **Show
cursor** applies when you choose a source. Use **Change source** after changing
this option. If your desktop cannot include the cursor, sharing can continue
with a warning that the cursor is absent.

Open **More options** from the three-dot button beside Close in the picker
header. Its menu floats over the preview without resizing the picker.

## Stream preview privacy

Other participants can see a still thumbnail of your stream before choosing to
watch. After you choose **Go live** and the stream connection is ready, Cressida
can upload a JPEG thumbnail, up to 512 × 288 pixels. It refreshes about every
five minutes while sharing is active and unpaused. This thumbnail is separate
from your local preview.

Under **More options**, **Hide stream preview** prevents these uploads. It
starts with your account's stream preview setting. Changing the checkbox applies
only to this stream and is not saved. If the account setting has not loaded,
previews stay hidden unless you explicitly change the checkbox.

A failed or uncertain thumbnail upload is not retried automatically. A later
scheduled thumbnail uses a new capture of the source.

## Share application audio

Open **Audio Sources** and choose **None** to send no application audio,
**System audio** to include playing applications, or individual applications.
You can select more than one application. Start playback in an application and
use **Refresh Audio Sources** if it is missing from the list. **Open Audio
Settings** opens your voice device settings.

Audio choices open in a floating checklist. Scroll the list for more
applications. Click outside a menu or press Escape to dismiss it while keeping
the picker open.

Cressida excludes its own playback from shared audio. Microphone mute,
push-to-talk and microphone activity settings control your voice separately;
they do not mute the selected applications. **None**, **Pause** or **Stop
sharing** stops sending application audio.

Application audio uses PulseAudio or PipeWire's PulseAudio-compatible service.
The screen source itself uses PipeWire and your desktop's ScreenCast portal.

## Change or stop your stream

Use **Pause** to stop sending the screen and shared audio while retaining the
selected source. **Resume** continues the stream. Use **Source and quality** to
reopen the picker, or **Stop sharing** to end publication and release screen
access. A replacement source takes over after its first valid frame arrives;
cancelling its chooser keeps the previous source.

The compact preview shows setup, live, paused and failed states separately. Drag
its title or video to move it within the conversation. Its buttons remain
clickable, and hovering or scrolling over the preview does not affect chat
behind it. The position is kept only for the current session. After an error,
reopen the sharing settings and choose **Try again** for an explicit new
attempt. A preview or a zero viewer count alone does not establish whether
publication succeeded.

Sharing continues when the main window is hidden in the tray. The tray adds
**Stop Sharing** while a publication is active. Disconnecting from voice or
signing out also ends your streams. A failed connection requires an explicit new
attempt; Cressida does not automatically restart sharing. Switching away pauses
only your local preview to save rendering work; it does not pause the broadcast.

## Watch a stream

Choose a participant's **LIVE** badge or a **Watch** action to watch their
stream. Watching joins the stream's voice conversation when needed. Select your
own stream to view its live preview in the voice channel's main content area.
The text conversation moves to the right, where the member pane usually appears.
In a narrow window, chat moves below the video. The view pages streams to fit
the available space and can collapse the video area to keep a tall message
composer usable.

Hover over another participant's **LIVE** badge or **Watch** button to see a
still thumbnail without joining their stream. The card shows loading or
unavailable feedback when needed. It follows your image display preference and
does not show previews for blocked or ignored participants. It never starts
video playback automatically.

You can watch more than one stream, adjust each stream's volume, or choose **Pop
out** for a separate native window. **Stop watching** closes that stream without
leaving the voice conversation.

When more than one stream is active, **Focus** shows just the selected stream
and **Show all** returns to the stream grid. These controls stay hidden when
there is only one stream.

Deafening silences received stream audio while video can continue. Hidden video
tiles can stop requesting frames until they become visible again. Closing a view
does not itself end a publication; use **Stop sharing** to end yours.

## Desktop requirements and current limits

Screen selection needs a running session D-Bus, PipeWire and a desktop portal
backend that implements ScreenCast for your compositor. The available screen and
window choices depend on that backend. Installing only a portal's file chooser
does not provide screen capture.

The client has no camera capture. Private automated tests cover consent, real
synthetic video frames, source replacement, encrypted media transport,
retransmission and stereo application audio. They do not establish compatibility
with every desktop, hardware encoder or live call.

See
[screen sharing troubleshooting](troubleshooting.md#screen-sharing-does-not-start)
if the chooser, preview or publication fails.
