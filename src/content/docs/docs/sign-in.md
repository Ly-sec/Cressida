---
title: Sign in
description:
  Connect your account through your browser and remember your sign-in with a
  desktop keyring.
---

## Connect your account

1. Open Cressida and choose the browser sign-in option.
2. Complete the sign-in flow in your default web browser.
3. Return to Cressida and wait for your conversations to load.

External sign-in pages open in your browser. Cressida does not embed a browser
engine. If the browser does not open, check that a default browser is configured
and `xdg-open` is available.

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
locations, see [Files and data](../files-and-data/).

## Advanced sign-in options

Token input, `CRESSIDA_TOKEN` and `--token-file` are available for advanced use.
Browser sign-in is the normal starting point. Account tokens are credentials;
never include them in screenshots, support reports or shared configuration. The
[command-line reference](../command-line/) describes these options.

Having trouble? Start with
[Sign-in troubleshooting](../troubleshooting/#sign-in-is-not-remembered).
