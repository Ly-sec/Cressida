---
title: Manual installation
description:
  Build and install Cressida yourself with Meson and Ninja, without Nix or Guix.
---

This route builds Cressida from source using your distribution's development
packages. It does not require Nix, Guix or `just`.

## 1. Check your desktop

Use a Linux Wayland session with working EGL/OpenGL ES drivers. Install a
default browser and `xdg-open`. A desktop Secret Service provider is recommended
for remembering your sign-in. See
[runtime integration](packaging.md#desktop-runtime) for the remaining desktop
services.

## 2. Get the source

Download a source archive from the
[Cressida repository](https://github.com/Ly-sec/Cressida) or a
[published release](https://github.com/Ly-sec/Cressida/releases). Extract it
with your archive manager and open a terminal in the extracted source directory.
The directory should contain `meson.build`, `src/` and `assets/`.

For a release archive, compare its checksum with the published `SHA256SUMS`
before extracting it. Use a matching source revision for the executable,
renderer and assets.

## 3. Install build dependencies

Install Meson 1.2 or newer, Ninja, pkg-config/pkgconf, a C compiler and a C++23
compiler with a compatible standard library. Install Wayland's scanner and
protocol files, then the development libraries in
[Building from source](building.md#libraries).

Package names differ by distribution. Pay particular attention to:

- **libcurl 8.16 or newer**, built with HTTPS and secure WebSockets (`wss`).
- **stb** headers including `stb_image_resize2.h` and `stb_image_write.h`.
- **libzstd**, libmpv, FFmpeg, rlottie, librsvg, WebP and JPEG XL development
  files.
- **toml++**, MD4C, nlohmann JSON and the font/text libraries.
- **OpenSSL 3.0 or newer** and **libqrencode** development headers and
  pkg-config metadata for QR sign-in.

Meson reports missing requirements during configuration. It does not download
them for you.

## 4. Configure a user-local installation

This installs under your own account without writing to `/usr`:

```sh
meson setup build-release --prefix="$HOME/.local" --buildtype=release -Dcpp_std=c++23
```

For an existing build directory, change its prefix with:

```sh
meson configure build-release --prefix="$HOME/.local"
```

## 5. Build and install

```sh
meson compile -C build-release -j 4 cressida
meson install -C build-release --no-rebuild
```

The installation includes the executable, renderer library, complete assets,
desktop launcher, icon and notices. Copying just the executable is insufficient.

## 6. Launch Cressida

```sh
"$HOME/.local/bin/cressida" --version
"$HOME/.local/bin/cressida" --demo
```

Close the offline preview, then run without `--demo` to [sign in](sign-in.md).
Ensure `$HOME/.local/bin` is on your desktop session's `PATH` if you want to run
`cressida` directly. Most desktops also search `$HOME/.local/share` for the
installed launcher and icon; you may need to refresh your application menu or
sign in to your desktop again.

## System-wide installation

For a new system-wide build, use `--prefix=/usr/local` during Meson setup
instead. Build as your normal user. Installing into that prefix may require
administrator privileges, which Meson's install step can request. Distribution
packagers should use their normal prefix and a [staged install](packaging.md).

## Update or uninstall

To update, obtain newer source and repeat the build/install steps with the same
prefix. Keep the renderer and assets from that build together.

Keep the original build directory for its installation manifest. To remove that
installation:

```sh
ninja -C build-release uninstall
```

Uninstalling the application does not remove your user state or saved keyring
entry. Sign out first if you want to remove the remembered login. See
[Files and data](files-and-data.md) for user-owned files.
