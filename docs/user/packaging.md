---
title: Packaging
description:
  Stage a Cressida installation, include runtime assets and integrate with a
  Linux desktop.
---

This guide is for distribution packagers. Start with the
[build dependencies](building.md); Meson and Ninja can build directly without
Nix or `just`.

## Stage an installation

```sh
meson setup build-release --prefix=/usr --buildtype=release -Dcpp_std=c++23
meson compile -C build-release -j 4
meson test -C build-release --print-errorlogs
DESTDIR="$PWD/pkg" meson install -C build-release --no-rebuild
```

Use the distribution's usual prefix, libdir and compiler flags. Avoid
`-march=native` for general-purpose packages. LTO builds need a matching
compiler-aware archiver, such as `gcc-ar` or `llvm-ar`.

## Installed files

| Component           | Typical path with prefix `/usr`                                 |
| ------------------- | --------------------------------------------------------------- |
| Executable          | `/usr/bin/cressida`                                             |
| Renderer library    | `/usr/lib/libcressida-render.so`                                |
| Development headers | `/usr/include/cressida/`                                        |
| Complete assets     | `/usr/share/cressida/assets/`                                   |
| Desktop entry       | `/usr/share/applications/dev.lysec.Cressida.desktop`            |
| Application icon    | `/usr/share/icons/hicolor/scalable/apps/dev.lysec.Cressida.svg` |
| License notices     | `/usr/share/licenses/cressida/`                                 |

Meson supports other libdirs. Ship the renderer and executable from the same
build; there is no versioned renderer ABI. The installed executable has a
relative runpath for its renderer, so it does not need `LD_LIBRARY_PATH`.

Keep the complete assets tree, including fonts, icon metadata, emoji data,
themes and demo media. Preserve the MIT license and bundled notices for Tabler
icons, emoji aliases, theme palettes and Wuffs. Development headers can be split
into a development package.

## Voice dependencies

Ship the runtime libraries for `libpulse`, Opus, libsodium, libdave and its MLS
dependencies. Development packages must also expose their headers and pkg-config
metadata. The client requires the libdave C API in `dave/dave.h`; the encryption
tests additionally use the MLS C++ API through `mlspp`.

The pinned C++23 build recipes for libdave and MLS are in
[`nix/libdave.nix`](https://github.com/Ly-sec/Cressida/blob/main/nix/libdave.nix).
Outside Nix, provide compatible C++23 builds and equivalent `libdave` and
`mlspp` pkg-config modules. Meson does not download or build these dependencies
itself.

For a source build outside Nix, install CMake, Ninja, Python 3, curl, OpenSSL
development files and the other libraries in [Building](building.md). From the
client source directory, run:

```sh
sh scripts/build-voice-dependencies.sh /usr/local
```

This downloads pinned archives, verifies their checksums and builds the native
encryption libraries. Choose a prefix you can write to. For a custom prefix,
include its `lib/pkgconfig` directory in `PKG_CONFIG_PATH` before configuring
Cressida. `CRESSIDA_VOICE_BUILD_JOBS` controls parallel compilation and defaults
to four jobs.

Voice requires a running PulseAudio service or PipeWire with its
PulseAudio-compatible service. The client does not start or install an audio
daemon. Device enumeration alone does not start microphone capture.

## Desktop runtime

Ship OpenSSL 3.0 or newer and libqrencode with the client. QR sign-in uses them
for the encrypted approval exchange and the scannable code; it does not need an
external QR-code command or image service.

Image uploads and image copying or saving need the shared-mime-info database at
runtime. If it is installed outside the usual system data directories, include
its `share` directory in `XDG_DATA_DIRS`. The Nix package and development shell
configure this automatically while preserving your existing data directories.

Provide a Wayland compositor, EGL/GLES drivers, usable Fontconfig fonts
including color emoji, CA certificates, session D-Bus and the shared-mime-info
database. The default browser and `xdg-open` handle sign-in and external links.
Make `xdg-open` available at build time because its resolved path is recorded.

Recommended integrations include a Secret Service keyring, a desktop
notification service, a tray host and XDG portal backends for Settings and
FileChooser. The client does not install its own keyring, notification service
or autostart entry.

Do not ship user state, credentials or runtime sockets. Custom themes under the
user's configuration directory must stay user-owned. Installed packages should
find their assets without a `CRESSIDA_ASSETS_DIR` override.

For deterministic emoji fallback, configure the actual runtime font path
supplied by the distribution, for example:

```sh
meson configure build-release -Demoji_font=/usr/share/fonts/noto/NotoColorEmoji.ttf
```

Never compile a temporary staging path into that option.

## CI and release scope

CI covers native glibc builds on x86_64 and aarch64, plus an x86_64 musl build
in Alpine edge. The musl build links dynamically to system libraries; it is not
a portable static package.

The client release workflow prepares a draft source release with a
`cressida-VERSION.tar.xz` archive and `SHA256SUMS` after its checks pass. A
maintainer publishes it. There is no automatic distribution-repository upload.
