---
title: Updates
description:
  Check releases, update an AppImage and use your Flatpak update source.
---

## Check for an update

Open **Settings > Updates**. This page shows your current version, installation
type and update status. Choose **Check for updates** to check manually. **View
releases** opens the release page in your browser.

Stable releases are the default. **Include prereleases** also offers newer test
releases. A version older than your running version is never offered.

**Check automatically** is off initially. When enabled, Cressida checks shortly
after sign-in and once a day while it stays open. This preference is saved per
account. Release checks use a separate public connection without your chat
account credentials. A failed check needs an explicit retry or the next normal
daily check.

## AppImage

Choose **Download update** to download the image for your computer's
architecture. Cressida checks its size, release checksum and image format before
offering **Restart to update**. Downloading keeps the running image in place.
**Cancel update** stops a download or cancels a prepared update.

Keep your AppImage in a writable folder owned by your user. Links, shared
writable folders and read-only locations cannot be replaced through this page.
Extracted AppImages use the manual installation instructions instead.

**Restart to update** replaces the AppImage atomically, keeps the previous image
as a hidden backup beside it, and launches the updated image. Restart waits for
you to finish an active call, recording, screen share or pending send. Account
data stays in its existing location. Save any unsent drafts you want to keep
before restarting; those drafts belong to the current session.

**Download automatically** is also off initially. It downloads a detected update
after an automatic check. Restart always stays explicit. Failed downloads are
not installed or retried automatically.

Older AppImages without the Updates page need one manual download of a release
that includes it. See [Installation](installation.md#appimage).

## Flatpak

Flatpak checks and installation use the host's Flatpak update service and the
repository configured for your installed app. Choose **Install update**, then
**Restart to update** when you are ready. You can also use your desktop's
software manager. A release requiring additional permissions needs the software
manager.

The current standalone release bundles do not configure a continuing application
repository. Without one, use a newer bundle from Releases. The website
deployment can publish a signed repository at its `flatpak/` subdirectory once
the maintainer enables and configures it. That feed is not available merely
because this client code has been built.

Once the feed is published, its `flatpak/cressida.flatpakref` file installs an
app with that update source. To migrate an existing standalone bundle, close
Cressida, uninstall the app without deleting its data, then install that
published reference for the same user. The normal uninstall command keeps the
account data:

```sh
flatpak uninstall --user dev.lysec.Cressida
```

Do not add `--delete-data` when you want to retain your settings. See
[Flatpak installation](installation.md#flatpak).

## Nix, Guix and other installations

The Updates page can show newer releases, but your package manager owns the
installed files. Use your Nix configuration, Guix channel or distribution
package manager to upgrade. For a source installation, obtain the newer source
and repeat the
[build and install steps](manual-installation.md#update-or-uninstall).
