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
icons, emoji aliases, theme palettes, Wuffs and MLS++. Development headers can
be split into a development package.

## Voice dependencies

Ship the runtime libraries for `libpulse`, Opus, libsodium and OpenSSL 3.0 or
newer. Development packages expose `libpulse`, `opus`, `libsodium`, `libcrypto`
and `nlohmann_json` to pkg-config. See the distribution package names in
[Building](building.md#voice-development-packages).

The DAVE layer is part of Cressida. The pinned MLS++ snapshot is built privately
from bundled sources with C++23 and system OpenSSL; no separate libdave, MLS++,
HPKE support library or custom source-install step is required. Meson performs
no dependency downloads. Private MLS/DAVE headers and archives are not installed
as a public library.

Preserve `share/licenses/cressida/mlspp/`, including the BSD-2-Clause LICENSE,
PROVENANCE.md and both source manifests. Keep the bundled source and these
notices in source releases as well. The application and other bundled assets
retain their existing licenses. Static inclusion of MLS++ does not remove its
notice requirements.

The optional `dave_reference` build option adds an external library solely for
comparison tests; leave it disabled for ordinary packages. The
`voice_crypto_only` option builds just the offline encryption fixture, not the
application. See
[Building](building.md#check-encryption-without-desktop-dependencies).

Voice requires a running PulseAudio service or PipeWire with its
PulseAudio-compatible service. The client does not start or install an audio
daemon. Device enumeration alone does not start microphone capture.

Screen capture additionally links the PipeWire client library (`libpipewire-0.3`
through pkg-config) and GIO's Unix support (`gio-unix-2.0`). Keep the FFmpeg
encoders required for publication available alongside its decoders. See
[Building](building.md#screen-sharing-development-packages).

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

For [screen sharing](screen-sharing.md), provide PipeWire and a portal backend
that implements ScreenCast for the active compositor. Shared application audio
also needs a PulseAudio-compatible service. A ScreenCast chooser grants access
to the selected source; the client does not request or restore screen access on
sign-in.

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
