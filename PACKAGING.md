# Packaging Cressida

Notes for Linux distribution packagers and source builds. Nix is optional;
Meson and Ninja build the client directly. Contributor checks are described in
[CONTRIBUTING.md](CONTRIBUTING.md).

## Identity

| Field | Value |
|---|---|
| Name / executable | `cressida` |
| Short description | A native Discord desktop client for Linux and Wayland. |
| Homepage | https://github.com/Ly-sec/Cressida |
| Version | `project(... version: ...)` in `meson.build`; keep `nix/package.nix` in sync |
| License | MIT; retain the bundled third-party notices described below |
| Desktop entry | `dev.lysec.Cressida.desktop` |
| Desktop icon | `dev.lysec.Cressida`, installed in `icons/hicolor/scalable/apps` |

CI builds and tests native `x86_64` and `aarch64` Linux packages with glibc,
plus an `x86_64` musl build in Alpine edge. Other targets have no dedicated
coverage. The Alpine build links dynamically to its system libraries; it does
not produce a portable static executable. Cressida requires a Wayland session;
there is no X11 window backend.

## Dependencies

The authoritative dependency checks are in [meson.build](meson.build). Package
names vary between distributions: install the development headers and pkg-config
metadata as well as the runtime libraries. Cressida uses neither Qt nor GTK. The
native activation regression fixture also uses Wayland server development headers
and `wayland-server` through pkg-config; the application itself remains a Wayland
client.

### Build tools

- A C compiler and a C++23 compiler **and standard library** providing
  `std::expected`, `std::format` and `std::ranges::contains`. A compiler accepting
  `-std=c++23` with an older standard library is insufficient.
- Meson 1.1 or newer, Ninja and pkg-config/pkgconf.
- For LTO builds, a matching compiler-aware archiver: `gcc-ar` with GCC or
  `llvm-ar` with Clang. `just build release` lets Meson choose it when `AR` is
  unset or simply `ar`; custom `AR` commands remain explicit overrides. Plain
  archivers without the compiler's LTO plugin can produce archives whose symbols
  the linker cannot find. See [GCC's LTO documentation](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html#index-flto).
- `wayland-scanner` and `wayland-protocols`, including xdg-shell, xdg-decoration,
  viewporter, fractional-scale-v1 and xdg-activation-v1 XML files.
- Python 3, `readelf` (binutils) and the `ffmpeg` executable for the test fixtures.
- `dbus-daemon` for private test buses.

`just`, clang-format 22, nixfmt, actionlint, ShellCheck and Weston are
developer/CI tools, not requirements for a plain Meson package build. Do not add
them to the installed client's runtime dependencies.

### Libraries and headers

Voice uses Cressida's inbuilt DAVE layer, a private bundled MLS++ snapshot,
system OpenSSL 3.0 or newer, libsodium, Opus and libpulse. The bundled dependency
is compiled with C++23 and does not require system libdave or MLS++ packages,
custom prefixes, CMake or a separate dependency-install script. Meson downloads
no sources. See [voice development packages](docs/user/building.md#voice-development-packages)
for distribution package names and [the bundled provenance](third_party/mlspp/PROVENANCE.md)
for the source pin, checksums and local patches.

Install all notices under `share/licenses/cressida/mlspp/`, including the
BSD-2-Clause license, provenance and source manifests. Include the complete
bundled source closure in source releases. Private MLS/DAVE headers and archives
are excluded from the public installation.

`-Dvoice_crypto_only=true` builds the offline encryption fixture with only
system crypto and JSON prerequisites. `-Ddave_reference=enabled` adds a
separately installed libdave oracle for optional comparison tests; normal builds
leave it disabled. These options do not change ordinary voice behavior.

| Dependency | pkg-config module / requirement | Purpose |
|---|---|---|
| Wayland | `wayland-client`, `wayland-egl`, `wayland-protocols` | Native windows and generated protocol bindings |
| EGL and OpenGL ES | `egl`, `glesv2` | Hardware or Mesa software rendering |
| Cairo | `cairo`, `cairo-ft` | Text and drawing support |
| Pango | `pango`, `pangocairo`, `pangoft2` | Text shaping and layout |
| FreeType | `freetype2` | Font rasterization |
| Fontconfig | `fontconfig` | Font discovery and fallback |
| HarfBuzz | `harfbuzz` | Text shaping |
| librsvg | `librsvg-2.0` | SVG images |
| libwebp | `libwebp`, `libwebpdemux` | Static and animated WebP |
| Zstandard | `libzstd` | Lossless animation frame caching |
| libjxl | `libjxl`, `libjxl_threads` | JPEG XL images |
| libxkbcommon | `xkbcommon` | Keyboard input |
| nlohmann JSON | `nlohmann_json` | Protocol and state data |
| toml++ | `tomlplusplus` | Theme configuration |
| GLib / GIO | `gio-2.0` | D-Bus, portals and theme file monitoring |
| libcurl | `libcurl >= 8.16`, with HTTPS, `ws` and `wss` enabled | REST and Gateway connections |
| libsecret | `libsecret-1` | Secret Service credentials |
| MD4C | `md4c` | Markdown parsing |
| FFmpeg libraries | `libavformat`, `libavcodec`, `libavutil`, `libswscale` | Media decoding |
| libmpv | `mpv >= 2.3` pkg-config API version | Embedded native video and audio playback |
| rlottie | `rlottie` | Animated stickers |
| stb headers | `stb_image_resize2.h` and `stb_image_write.h` | Image resizing and encoding |
| POSIX threads | Compiler / libc thread support | Background workers |

**libcurl:** the library linked into Cressida must support secure WebSockets;
installing a curl command-line tool with different features does not fix the
library. Native Meson configuration compiles and runs a `curl_version_info`
probe that requires `wss`. If rebuilding curl, enable WebSockets and a TLS backend
in that package. Consult curl's [WebSocket documentation](https://curl.se/docs/websocket.html).
The Nix package enables `websocketSupport` explicitly. Cross builds cannot run
this probe, so packagers must verify the target library's enabled protocols.

**stb:** old packages with only `stb_image_resize.h` are insufficient. Headers
may be installed below `stb/` or exposed through `stb.pc`. The Nix recipe pins
revision `f1c79c02822848a9bed4315b12c8c8f3761e1296`; use that revision as a
reference if your distribution's headers are too old. The source tree does not
download dependencies during configuration.

**libmpv:** `>= 2.3` refers to the pkg-config/libmpv API version, not the mpv
player application's release number. The client links the library directly;
it does not launch an external player window.

### Bundled components and notices

Wuffs is shipped under `third_party/wuffs` with its MIT and Apache-2.0 notices;
no separate Wuffs system package is needed. The renderer and native controls in
`src/render` and `src/ui` are project sources, not external build dependencies.
Ship the complete assets tree, preserving these notices:

- `assets/fonts/tabler-icons-license.txt`
- `assets/emoji-data-license.txt`
- `assets/themes/LICENSES.txt`
- `third_party/wuffs/LICENSE-MIT` and `LICENSE-APACHE`

## Build and install

### Using just

With the build dependencies and `just` installed, run from the checkout:

```sh
just install
```

This builds the release client and its runtime libraries, then installs the
executable, renderer, complete assets, headers, desktop launcher, application
icon and license notices. It does not build test-only targets. A fresh build
uses `/usr/local`; an already configured release build keeps its prefix.
Build as your normal user. Meson can request administrator privileges for the
installation step when needed; see [Meson's installation guide](https://mesonbuild.com/Installing.html#installing-as-the-superuser).

For a user-local install without administrator privileges:

```sh
just install release "$HOME/.local"
```

Ensure `$HOME/.local/bin` is on your session's `PATH`. Your desktop must also
search the prefix's `share` directory for application entries and icons; the
usual user data directory is `$HOME/.local/share`.

The optional first argument selects `debug`, `release` or `asan`; the optional
second argument explicitly sets the prefix. Omitting that second argument
preserves the build directory's configured prefix. For example,
`just configure release /opt/cressida` followed by `just install` keeps
`/opt/cressida`. `just uninstall` removes files recorded by that release build's
install manifest; `just uninstall debug` selects a debug installation. It does
not reconfigure or rebuild. Keep the build directory if you want that uninstall
manifest available. Removing shared directories is left to the user/package manager.

### Using Meson directly

From an extracted release archive or source checkout with dependencies installed:

```sh
meson setup build-release --prefix=/usr --buildtype=release -Dcpp_std=c++23
meson compile -C build-release -j 4
meson test -C build-release --print-errorlogs
DESTDIR="$PWD/pkg" meson install -C build-release --no-rebuild
```

For a direct installation instead of package staging, omit `DESTDIR` and run
`meson install -C build-release`. To build only runtime targets first, use
`meson compile -C build-release cressida`, then
`meson install -C build-release --no-rebuild`.

`DESTDIR` stages files beneath `pkg` without changing their final `/usr` paths;
see [Meson's installation reference](https://mesonbuild.com/Installing.html#destdir-support).
Use your distribution's normal prefix, libdir, compiler and linker flags. Avoid
CPU-specific flags such as `-march=native` in general-purpose binary packages.
No particular linker is required. `just` is a convenience wrapper, not a package
build dependency. There is currently no option to omit compiling test targets
from the default all-target build.

For an unpackaged development build:

```sh
meson setup build-debug --buildtype=debug -Dcpp_std=c++23
meson compile -C build-debug -j 4 cressida
./build-debug/cressida --demo
```

The native offline test suite uses synthetic fixtures, temporary directories,
private D-Bus instances and loopback network peers. It needs no Discord account,
credentials or external network access. Sandboxes must permit local sockets;
a few graphics checks may explicitly skip if an EGL context is unavailable.
`--smoke-test` renders the offline UI on an available Wayland compositor and exits.

For deterministic emoji fallback, install a color emoji font such as Noto Color
Emoji and configure its final runtime path if needed:

```sh
meson configure build-release -Demoji_font=/usr/share/fonts/noto/NotoColorEmoji.ttf
```

Use the actual path provided by your distribution. Do not embed a temporary
staging or build-directory path in this option.

### Installed layout

With prefix `/usr` and the usual `lib`/`share` directories:

```text
/usr/bin/cressida
/usr/lib/libcressida-render.so
/usr/lib/libcressida-log.so
/usr/include/cressida/...
/usr/share/cressida/assets/...
/usr/share/applications/dev.lysec.Cressida.desktop
/usr/share/icons/hicolor/scalable/apps/dev.lysec.Cressida.svg
/usr/share/licenses/cressida/...
```

Meson honors a distribution's alternative libdir. Ship the renderer and logging
libraries with the executable from the same build; they currently have no versioned ABI.
Development headers can go in a distribution's development subpackage. Keep
the complete assets bundle with the runtime package: fonts, icon metadata,
emoji catalogs, themes and demo media are used by the application and previews.

The compiled prefix/datadir locate the installed bundle. Development builds also
look beside the executable and its parent directory. `CRESSIDA_ASSETS_DIR` can
select an alternative complete assets root; an installed package should work
without this override. Cressida does not currently ship AppStream metadata,
man pages or systemd units. The installed application icon
uses the original Cressida logo; the tray uses its separate small-size artwork.

The executable's installed runpath locates the renderer and logger relative to its
own directory, including custom libdirs. The renderer also finds its sibling
logging library through `$ORIGIN`, without `LD_LIBRARY_PATH` or an `ldconfig`
step. External runtime dependencies must still be provided by the system/package
manager. Meson refreshes the desktop-entry and icon caches when the corresponding
optional tools are available. It skips those refreshes for `DESTDIR` staging;
distribution packages should use their normal cache triggers.

The native `installation` regression stages an actual install in a temporary
directory, checks all assets/headers and integration files, and runs the installed
executable away from the checkout with library-path overrides removed. It never
writes into the system prefix or changes desktop caches, and preserves any
existing uninstall manifest.

## Runtime integration

Voice needs a running PulseAudio service or PipeWire's PulseAudio-compatible
service, plus the voice libraries listed above. Cressida does not install or
start an audio daemon.

| Dependency | Requirement |
|---|---|
| Wayland compositor and EGL/GLES driver | Required to display the client |
| Fontconfig fonts, including a color emoji font | Required for usable text and emoji fallback |
| CA certificate store | Required for TLS certificate validation |
| Default web browser and `xdg-open` | Browser sign-in and external links; make `xdg-open` available at build time as its resolved path is recorded |
| Session D-Bus | Desktop service integration |
| shared-mime-info database | File-type detection for uploads and image copy/save; expose its share directory through XDG_DATA_DIRS at runtime and in clean tests |
| Secret Service provider | Recommended for remembering sign-in; libsecret alone does not provide a keyring |
| XDG desktop portal with Settings / FileChooser backends | System color mode and native file selection where supported |
| Desktop notification service | Native notification delivery |

GNOME Keyring or KeePassXC with Secret Service enabled can store credentials.
The client uses the existing desktop services; it does not install its own
notification daemon, browser engine or keyring. No compositor autostart entry
or privileged service is required. Start `cressida` from the application menu
or a terminal.
For a desktop-managed autostart command, use `cressida --start-minimized` to start
hidden in the tray. It opens the window if no tray host appears within five
seconds. Cressida does not install or enable an autostart entry itself. Demo and
smoke-test modes ignore this flag.

Ordinary terminal and desktop-launcher starts reuse the running client, including
a window hidden in the tray. Handoff happens before credentials, connections or
graphics are initialized and preserves the existing conversation, settings and
drafts. `--start-minimized` affects only the first launch; another launch requests
Open. `cressida --new-instance` (short form `-n`) explicitly starts a separate
client. Demo and smoke-test modes always run independently.

Single-instance coordination uses a user-private `cressida` directory below
`$XDG_RUNTIME_DIR`, with a lock and local activation socket. When the variable is
unset, it uses `/run/user/UID`, or a private `/tmp/cressida-runtime-UID` directory
when that runtime is absent. No D-Bus service or extra runtime dependency is
required for handoff. Socket requests wake the native event loop even when the
window is hidden. The lock is released on exit; stale sockets are recovered after
abrupt termination. Runtime files should not be included in packages or user backups.

## User files

| Kind | Default location |
|---|---|
| Session/UI state | `$XDG_STATE_HOME/cressida/session.json` (`~/.local/state/cressida/session.json`) |
| Custom themes | `$XDG_CONFIG_HOME/cressida/themes/` (`~/.config/cressida/themes/`) |
| Saved credentials | Desktop Secret Service provider |

Do not package user state, tokens, keyring contents or generated build caches.
Custom themes remain user-owned TOML files; bundled palettes live in the assets
directory and must not be installed over those custom files.

## Releases

Prefer tagged release archives for stable packages. The release workflow checks
that the tag matches both package version declarations and creates a draft
release containing `cressida-VERSION.tar.xz` and `SHA256SUMS` after CI passes.
The archive includes the Nix files and lock as well as the plain Meson build,
assets and license notices. No AUR upload or distribution-repository publishing
is configured by this project.
