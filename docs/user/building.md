---
title: Building from source
description:
  Set up a native C++23 build with Meson, Ninja and the required development
  libraries.
---

## Build tools

Install a C compiler, a C++23 compiler and a compatible standard library.
`std::expected`, `std::format` and `std::ranges::contains` must be available;
accepting the `-std=c++23` flag alone is not enough.

You also need Meson 1.2 or newer, Ninja, pkg-config/pkgconf, `wayland-scanner`
and recent `wayland-protocols`. Nix users can enter the supplied environment
with `nix develop path:.` from the client source directory.

## Libraries

Install development headers and pkg-config metadata as well as runtime
libraries.

| Library           | pkg-config modules or requirement                      |
| ----------------- | ------------------------------------------------------ |
| Wayland           | `wayland-client`, `wayland-egl`, `wayland-protocols`   |
| Graphics          | `egl`, `glesv2`                                        |
| Cairo             | `cairo`, `cairo-ft`                                    |
| Pango             | `pango`, `pangocairo`, `pangoft2`                      |
| Text and fonts    | `freetype2`, `fontconfig`, `harfbuzz`                  |
| SVG               | `librsvg-2.0`                                          |
| WebP              | `libwebp`, `libwebpdemux`                              |
| JPEG XL           | `libjxl`, `libjxl_threads`                             |
| Animation cache   | `libzstd`                                              |
| Keyboard          | `xkbcommon`                                            |
| Data              | `nlohmann_json`, `tomlplusplus`                        |
| Desktop services  | `gio-2.0`, `gio-unix-2.0`, `libsecret-1`               |
| Network           | `libcurl >= 8.16`, with HTTPS, `ws` and `wss`          |
| QR sign-in        | `openssl >= 3.0`, `libqrencode`                        |
| Markdown          | `md4c`                                                 |
| Video             | `libavformat`, `libavcodec`, `libavutil`, `libswscale` |
| Native playback   | `mpv >= 2.3` pkg-config API version                    |
| Voice audio       | `libpulse`, `opus`                                     |
| Screen capture    | `libpipewire-0.3`                                      |
| Voice encryption  | `libsodium`, `libcrypto >= 3.0`                        |
| Animated stickers | `rlottie`                                              |
| Image headers     | `stb_image_resize2.h`, `stb_image_write.h`             |

On Alpine, install `libqrencode-dev` for QR sign-in development headers and
pkg-config metadata.

The libmpv requirement refers to its pkg-config API version, not the player
application's release number. Old stb packages with only `stb_image_resize.h`
are insufficient. The Nix package pins stb revision
`f1c79c02822848a9bed4315b12c8c8f3761e1296` as a known reference.

The **linked libcurl** must support secure WebSockets. A separate curl
executable with different features does not fix the library. Native Meson
configuration checks the linked library; cross-build packagers must verify their
target library.

Cressida builds its DAVE voice encryption code and a pinned MLS++ source
snapshot with C++23. Install system OpenSSL development files, libsodium, Opus,
the Pulse client library and JSON headers. Separate libdave or MLS++ packages
and custom voice dependency prefixes are no longer needed. Meson uses the
bundled source without downloading dependencies. Wuffs is also bundled.

### Voice development packages

These packages cover voice audio, encryption and the bundled MLS library's
prerequisites. Install the other libraries listed above for a full client build.

| Distribution      | Development packages                                                                  |
| ----------------- | ------------------------------------------------------------------------------------- |
| Arch              | `openssl libsodium opus libpulse nlohmann-json`                                       |
| Fedora            | `openssl-devel libsodium-devel opus-devel pulseaudio-libs-devel json-devel`           |
| Debian and Ubuntu | `libssl-dev libsodium-dev libopus-dev libpulse-dev nlohmann-json3-dev`                |
| Alpine            | `openssl-dev libsodium-dev opus-dev pulseaudio-dev nlohmann-json`                     |
| openSUSE          | `libopenssl-3-devel libsodium-devel libopus-devel libpulse-devel nlohmann_json-devel` |

Arch includes development files in its regular packages. Package availability
alone does not establish that an older release meets the full client's compiler,
secure WebSocket, media and image-header requirements.

### Screen sharing development packages

Screen capture adds the PipeWire client headers and `libpipewire-0.3` pkg-config
metadata. Package names include
[`libpipewire` on Arch](https://archlinux.org/packages/extra/x86_64/libpipewire/files/),
[`pipewire-devel` on Fedora](https://packages.fedoraproject.org/pkgs/pipewire/pipewire-devel/),
[`libpipewire-0.3-dev` on Debian](https://packages.debian.org/source/pipewire)
and [Ubuntu](https://packages.ubuntu.com/en/noble-updates/libpipewire-0.3-dev),
and
[`pipewire-dev` on Alpine](https://pkgs.alpinelinux.org/package/edge/community/x86_64/pipewire-dev).
GIO's Unix development files supply `gio-unix-2.0`.

Publishing also needs FFmpeg with an available H.264, VP8 or VP9 encoder;
decoders alone do not provide screen sharing. The native client tries its
supported hardware encoders where available and otherwise uses software.

## Compile and run

From the client source directory:

```sh
meson setup build-debug --buildtype=debug -Dcpp_std=c++23
meson compile -C build-debug -j 4 cressida
./build-debug/cressida --demo
```

With `just` installed, `just build` and `just demo` provide convenience
commands. For a release install, follow [Installation](installation.md).

## Run tests

Test fixtures additionally need Python 3, `readelf`, the `ffmpeg` executable,
`dbus-daemon` and Wayland server headers/library. Graphics tests need a usable
EGL context. Local sockets must be permitted.

When the `pipewire` executable is available, the capture regression starts a
private server with a synthetic video source and a private portal bus. It needs
PipeWire's `videotestsrc` SPA plugin. When `pulseaudio` is available, native
audio tests use their own server and synthetic applications. These fixtures
never capture your desktop or microphone.

```sh
meson compile -C build-debug -j 4
meson test -C build-debug --print-errorlogs
```

Tests use synthetic fixtures, temporary directories, private buses and local
network peers. They do not need account credentials or access to a live account.

See [Contributing](contributing.md) for formatting, lint and headless UI checks,
or [Packaging](packaging.md) for installed runtime requirements.

## Check encryption without desktop dependencies

The offline encryption fixture needs only a C++23 compiler, Meson, Ninja,
pkg-config, system OpenSSL 3.0 development files and JSON headers:

```sh
meson setup build-voice-crypto -Dvoice_crypto_only=true -Dcpp_std=c++23
meson compile -C build-voice-crypto -j 4
meson test -C build-voice-crypto --print-errorlogs
```

This builds frame, parser and encrypted-group tests without graphics, desktop
audio or network access. It does not build the desktop application.

Developers can separately enable `-Ddave_reference=enabled` when an explicit
libdave test installation is available. That adds mixed-implementation creator,
joiner, media and group-transition tests. It is disabled by default and is not
an application or ordinary-test dependency. Offline results do not establish
live call interoperability.
