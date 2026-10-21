---
title: Installation
description:
  Install Cressida on a Wayland Linux desktop with Nix, Guix or Meson.
---

## Before you begin

You need a Linux desktop running **Wayland** and working EGL/OpenGL ES drivers.
There is no X11 window backend. A desktop keyring is recommended so Cressida can
remember your sign-in.

Choose an installation route below. For a plain source build, install the
[build dependencies](../building/) first. Package names vary by distribution.

## Nix

From the extracted Cressida source directory, run:

```sh
nix run path:.
```

This uses the project's Nix configuration. Your Nix installation must support
flakes and the `nix-command` feature. The application still needs your Wayland
desktop and its services.

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
[Building from source](../building/), plus `just`.

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
[manual installation guide](../manual-installation/).

`just` is optional. With the dependencies installed:

```sh
meson setup build-release --prefix=/usr/local --buildtype=release -Dcpp_std=c++23
meson compile -C build-release -j 4 cressida
meson install -C build-release --no-rebuild
```

This installs the executable, renderer library, assets and desktop integration.
See [Packaging](../packaging/) for distribution prefixes and staged
installations.

## First launch

Open **Cressida** from your application launcher or run:

```sh
cressida
```

Then follow [Sign in](../sign-in/). To preview the interface without an account:

```sh
cressida --demo
```

Running the ordinary command again focuses the existing client. Use
`cressida --new-instance` if you intentionally want a separate instance.
