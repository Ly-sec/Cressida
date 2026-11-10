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
meson setup build-release --prefix=/usr --buildtype=release -Dcpp_std=c++23 -Dtests=enabled
meson compile -C build-release -j 4
meson test -C build-release --print-errorlogs
DESTDIR="$PWD/pkg" meson install -C build-release --no-rebuild
```

Use the distribution's usual prefix, libdir and compiler flags. Avoid
`-march=native` for general-purpose packages. LTO builds need a matching
compiler-aware archiver, such as `gcc-ar` or `llvm-ar`.

Release and minsize builds omit tests by default. The example explicitly enables
them for package validation. Use `-Dtests=disabled` and omit the test command
for a package build without test targets. Ordinary Nix installations use this
mode; the flake's CI check package enables and runs tests separately.

## Installed files

| Component           | Typical path with prefix `/usr`                       |
| ------------------- | ----------------------------------------------------- |
| Executable          | `/usr/bin/cressida`                                   |
| Renderer library    | `/usr/lib/libcressida-render.so`                      |
| Development headers | `/usr/include/cressida/`                              |
| Complete assets     | `/usr/share/cressida/assets/`                         |
| Desktop entry       | `/usr/share/applications/cressida.desktop`            |
| Application icon    | `/usr/share/icons/hicolor/scalable/apps/cressida.svg` |
| License notices     | `/usr/share/licenses/cressida/`                       |

When upgrading a manual installation, remove the previous
`dev.lysec.Cressida.desktop` launcher and `dev.lysec.Cressida.svg` icon from
that installation prefix to avoid duplicate launcher entries. Package upgrades
should replace those old files with the paths above.

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
find their assets without users setting `CRESSIDA_ASSETS_DIR`. The AppImage
launcher resolves its bundled paths when it starts.

For deterministic emoji fallback, configure the actual runtime font path
supplied by the distribution, for example:

```sh
meson configure build-release -Demoji_font=/usr/share/fonts/noto/NotoColorEmoji.ttf
```

Never compile a temporary staging path into that option.

## CI and release scope

### Updating installed copies

The native **Settings > Updates** page detects AppImage, Flatpak, Nix, Guix and
other installations. AppImages use the published release assets and SHA-256
checksums. Flatpaks use their configured origin through the host's update
portal. Package-managed and source installations retain their own update
procedure. See [Updates](updates.md).

The existing website branch can include a signed Flatpak repository in the same
Pages artifact as the website and documentation. The source helper lives on main
at `.github/workflows/scripts/flatpak_repository.py`; the website workflow owns
deployment. Binary bundles and repository objects are generated during that job,
not stored on the website branch.

To enable the feed after both branches contain these changes:

1. Keep a dedicated OpenPGP signing key for the repository. Add its armored
   private key as the repository Actions secret `CRESSIDA_FLATPAK_GPG_KEY`. If
   the key has a passphrase, add its exact passphrase as another repository
   Actions secret named `CRESSIDA_FLATPAK_GPG_PASSPHRASE`. Keep the private key
   protected; the signing job unlocks it without an interactive prompt.
2. Set the Actions variable `CRESSIDA_FLATPAK_GPG_FINGERPRINT` to its full
   primary fingerprint without spaces. Keep the same key for future feed
   updates.
3. Set the Actions variable `CRESSIDA_FLATPAK_UPDATES` to `true` and run
   **Documentation website** manually after a stable release has been published.

Configure these under the repository's **Settings > Secrets and variables >
Actions**, rather than the `github-pages` environment. Signing happens in the
build job before deployment. Passphrases must fit on one line, without a
trailing newline.

The website workflow imports both verified release bundles, signs their commits
and repository summary, and places the feed under `flatpak/`. Its `.flatpakref`
and `.flatpakrepo` files contain the public signing key and the website's actual
base URL, including a repository subpath or custom domain. The private key and
passphrase stay outside the public artifact. The passphrase reaches the isolated
signing agent through standard input, and the agent stops when preparation ends.
Preparation verifies the release metadata and checksums and rejects a combined
site larger than the Pages size limit.

Feed preparation failures stop deployment and leave the previously published
site in place. Leaving the feature disabled keeps the ordinary website build.
After each new stable release, use the existing manual deployment to refresh the
feed. The existing deployment triggers stay unchanged.

Land the website workflow's optional signing-secret interface before the main
workflow starts passing those secrets. Main-only changes cannot publish this
feed.

### Published packages

CI covers native glibc builds on x86_64 and aarch64, plus an x86_64 musl build
in Alpine edge. The musl build links dynamically to system libraries; it is not
a portable static package.

The root `VERSION` file is the single source of the client version. Meson, Nix
packaging, `cressida --version`, startup diagnostics and network identifiers all
use that value. To prepare a release, update its single line, for example
`v0.2.0`, and use the identical `v0.2.0` tag. Prerelease values such as
`v0.2.0-rc.1` use the identical `v0.2.0-rc.1` tag. Keep the leading `v` in both
places. A missing or mismatched version stops release packaging. Source archives
include `VERSION` so builds from an archive report the same version. Release
filenames omit that prefix, for example `cressida-0.2.0.tar.xz` and
`cressida-0.2.0-x86_64.AppImage`.

Pushing a matching version tag runs CI, then calls `release-packages.yml`.
Native x86_64 and aarch64 runners build the client with pinned dependencies in
the Freedesktop 26.08 SDK. The workflow exports a Flatpak bundle and deploys the
same native payload into an AppImage with its runtime libraries. Neither package
requires Nix. Package builds omit test targets; private headless replays check
both packaged formats after the main test suite passes. All manifest modules
install libraries into `/app/lib`, keeping dependency lookup and the packaged
runtime consistent across architectures and builder versions. AppImage
deployment searches the selected SDK architecture's library directories. Package
checks forward a private compositor socket through Flatpak's normal Wayland
permission and print the failing check's log directly in the job output.

The release job attaches both `.AppImage` and `.flatpak` files for each
architecture, a `cressida-VERSION.tar.xz` source archive and `SHA256SUMS`
covering all five files, then publishes the release automatically after every
required check succeeds. Uploads stay in a private draft until the complete
asset set, file sizes and SHA-256 digests have been verified. Failed uploads or
verification leave that draft unpublished. Rerunning a tag can repair an
unfinished draft; an already published release and its assets remain unchanged.

The workflow does not upload an application to Flathub or another distribution
repository. Manual runs of **Release packages** on a version tag retain package
artifacts for seven days without creating or publishing a release.
