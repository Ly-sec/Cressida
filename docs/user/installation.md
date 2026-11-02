---
title: Installation
description:
  Install Cressida on a Wayland Linux desktop with an AppImage, Flatpak, Nix,
  Guix or Meson.
---

## Before you begin

> **Account-risk notice:** Take particular care when starting new DMs or joining
> new servers. Users have reported account flags and messaging restrictions
> during use, but the trigger remains unconfirmed. Read the
> [account-risk notice](faq.md#can-my-account-be-restricted).

You need a Linux desktop running **Wayland** and working EGL/OpenGL ES drivers.
There is no X11 window backend. A desktop keyring is recommended so Cressida can
remember your sign-in.

Choose an installation route below. For a plain source build, install the
[build dependencies](building.md) first. Package names vary by distribution.

### Input methods

For composed text, install and configure your preferred input-method service and
an engine for your language. Cressida connects through Wayland text-input-v3;
the compositor must support that connection. Fcitx 5 is supported through this
path without installing GTK or Qt input modules for Cressida.

Follow the official
[Fcitx 5 setup guide](https://fcitx-im.org/wiki/Setup_Fcitx_5) and
[Wayland guidance](https://fcitx-im.org/wiki/Using_Fcitx_5_on_Wayland) for your
desktop. On KDE Plasma, select **Fcitx 5** under **Virtual Keyboard** in System
Settings. Other compositors may start the service through their session or
autostart configuration.

## AppImage

Download the `.AppImage` matching your architecture from a published
[Cressida release](https://github.com/Ly-sec/Cressida/releases). Packages are
built for `x86_64` and `aarch64`. Check the download against the release's
`SHA256SUMS`, then make it executable and launch it:

```sh
chmod +x cressida-VERSION-x86_64.AppImage
./cressida-VERSION-x86_64.AppImage
```

Replace `VERSION` and the architecture with your download's filename. A host
Wayland session and working graphics drivers are still required. If FUSE is
unavailable, extract and run the same package:

```sh
./cressida-VERSION-x86_64.AppImage --appimage-extract
./squashfs-root/AppRun
```

To update, download the newer release and replace the executable. The AppImage
does not install an automatic updater or alter your account data.

## Flatpak

Install [Flatpak](https://flatpak.org/setup/) for your distribution, then
download the matching `.flatpak` from a published
[Cressida release](https://github.com/Ly-sec/Cressida/releases). Check it
against `SHA256SUMS` and install it:

```sh
flatpak install --user ./cressida-VERSION-x86_64.flatpak
flatpak run dev.lysec.Cressida
```

The installer can fetch the required Freedesktop 26.08 runtime from Flathub. The
bundle itself comes from the release, not a Flathub application listing. Install
a newer release bundle to update Cressida.

The package permits Wayland, network, graphics, desktop audio, keyring,
notification and tray access. Files are selected through desktop portals; it
does not grant access to your entire home directory. Settings and account data
are stored separately under `~/.var/app/dev.lysec.Cressida/`. Host applications'
local RPC connections may need a socket bridge into the sandbox.

To remove the package while retaining its data:

```sh
flatpak uninstall --user dev.lysec.Cressida
```

## Nix

Follow [Install with a Nix flake](nix.md) to add Cressida to your NixOS or Home
Manager configuration, install the package and manage updates.

## Guix

Add the Cressida channel to your Guix channel list:

```scheme
(channel
  (name 'cressida)
  (url "https://github.com/Ly-sec/Cressida")
  (branch "main"))
```

Refresh the channel and install the package:

```sh
guix pull
guix install cressida-git
```

The package is available in the `(cressida)` module. Refresh the channel again
when you want to update to a newer source revision.

## Build and install with just

Obtain the source from the
[Cressida repository](https://github.com/Ly-sec/Cressida) or a published source
archive. Install the development libraries listed in
[Building from source](building.md), plus `just`.

From the source directory:

```sh
just install
```

A fresh release build installs under `/usr/local`. If the release build
directory already has a prefix configured, the command keeps it. Build as your
normal user; the installation step can request administrator privileges when
needed.

For a user-local installation:

```sh
just install release "$HOME/.local"
```

Ensure `$HOME/.local/bin` is on your `PATH`. Your desktop also needs to find
application entries and icons in `$HOME/.local/share`.

### Remove a just installation

Keep the build directory used for installation, then run:

```sh
just uninstall
```

This removes files recorded in that release build's installation manifest. It
does not rebuild or remove your account's data. Distribution-managed packages
should be removed with the distribution's package manager instead.

## Build with Meson directly

For a complete step-by-step walkthrough, use the
[manual installation guide](manual-installation.md).

`just` is optional. With the dependencies installed:

```sh
meson setup build-release --prefix=/usr/local --buildtype=release -Dcpp_std=c++23
meson compile -C build-release -j 4 cressida
meson install -C build-release --no-rebuild
```

This installs the executable, renderer library, assets and desktop integration.
See [Packaging](packaging.md) for distribution prefixes and staged
installations.

## First launch

Open **Cressida** from your application launcher or run:

```sh
cressida
```

Then follow [Sign in](sign-in.md). To preview the interface without an account:

```sh
cressida --demo
```

Running the ordinary command again focuses the existing client. Use
`cressida --new-instance` if you intentionally want a separate instance.
