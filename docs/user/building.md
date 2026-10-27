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
| Desktop services  | `gio-2.0`, `libsecret-1`                               |
| Network           | `libcurl >= 8.16`, with HTTPS, `ws` and `wss`          |
| QR sign-in        | `openssl >= 3.0`, `libqrencode`                        |
| Markdown          | `md4c`                                                 |
| Video             | `libavformat`, `libavcodec`, `libavutil`, `libswscale` |
| Native playback   | `mpv >= 2.3` pkg-config API version                    |
| Voice audio       | `libpulse`, `opus`                                     |
| Voice encryption  | `libsodium`, `libdave`                                 |
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

Voice builds require the libdave C API, including `dave/dave.h`, and compatible
MLS libraries. The supplied Nix build pins and builds these dependencies with
C++23. Outside Nix, packagers must provide compatible C++23 libdave and MLS
builds, along with pkg-config metadata for `libdave`. The encryption regression
fixture also needs the MLS C++ headers and `mlspp` metadata. See
[Packaging](packaging.md#voice-dependencies) for the source references.

Wuffs is bundled in the source tree. The project does not download missing
dependencies during Meson configuration.

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

```sh
meson compile -C build-debug -j 4
meson test -C build-debug --print-errorlogs
```

Tests use synthetic fixtures, temporary directories, private buses and local
network peers. They do not need account credentials or access to a live account.

See [Contributing](contributing.md) for formatting, lint and headless UI checks,
or [Packaging](packaging.md) for installed runtime requirements.
