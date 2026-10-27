---
title: Sign in with QR or browser
description:
  Connect your account with a QR code or your browser, and remember your sign-in
  with a desktop keyring.
---

## Scan a QR code

1. Open Cressida. If your saved sign-in is not restored, wait for the QR code.
2. Use the QR scanner in the mobile app where you are already signed in.
3. Approve the sign-in request on your phone.
4. Return to Cressida and wait for your conversations to load.

Cressida first checks for a saved sign-in. When the sign-in screen appears, it
starts one QR sign-in attempt automatically, then waits for approval on your
phone before connecting.

Codes expire. Choose **Generate new code** to start a fresh attempt after a code
expires, the connection fails or you cancel. Cressida does not retry
automatically. Cancelling or choosing another sign-in method stops the current
attempt.

Press **Escape** to cancel the current QR or browser sign-in attempt.

## Use your browser

1. Open Cressida and choose the browser sign-in option.
2. Complete the sign-in flow in your default web browser.
3. Return to Cressida and wait for your conversations to load.

External sign-in pages open in your browser. Cressida does not embed a browser
engine. If the browser does not open, check that a default browser is configured
and `xdg-open` is available.

If you are asked to complete a verification or security check, use the official
app or browser. Those checks are not available inside Cressida.

## Opening your conversations

Cressida shows a full-window loading screen while restoring a saved sign-in and
preparing your last server or conversation after authentication. It opens the
app once that destination is ready, including an empty conversation. When the
server member list is visible, startup also waits for the live connection and
its first member list result, including an empty list. A centered Cressida logo
rotates while loading, then fades into the app. Its size stays consistent when
your saved interface zoom is restored. Reduced motion disables the rotation and
transition.

If loading fails, Cressida opens the app so you can see the error and retry
there. If preparation takes longer, the loading screen closes after 12 seconds
while the app continues loading. Press **Escape** during conversation
preparation to open the app right away without signing out.

## Remember your sign-in

A desktop **Secret Service** provider can save your login. Common choices are
GNOME Keyring and KeePassXC with Secret Service integration enabled. The
`libsecret` library alone does not provide a keyring.

Make sure your keyring is running and unlocked. Without an available provider,
you may need to sign in again the next time you open Cressida.

## Sign out

Use the sign-out action in settings to remove the saved login. Closing the
window or choosing **Close** from the tray exits or hides the client without
signing out.

Appearance and navigation state are separate from saved credentials. For file
locations, see [Files and data](files-and-data.md).

## Other options

Open **Other options** on the sign-in screen to enter an account token or
explore the offline demo. The demo shows sample conversations without signing
in.

Token input, `CRESSIDA_TOKEN` and `--token-file` are available for advanced use.
Account tokens are credentials; never include them in screenshots, support
reports or shared configuration. The [command-line reference](command-line.md)
describes these options.

Having trouble? Start with
[Sign-in troubleshooting](troubleshooting.md#sign-in-is-not-remembered).
