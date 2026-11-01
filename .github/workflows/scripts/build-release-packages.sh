#!/usr/bin/env bash
set -euo pipefail

: "${RELEASE_REF:?Select a release tag}"
: "${RELEASE_ARCH:?Set the native package architecture}"
if [[ "$RELEASE_ARCH" != "$(uname -m)" ]]; then
    echo 'Release packages must be built on their native architecture.' >&2
    exit 1
fi
case "$RELEASE_ARCH" in
    x86_64) tool_sha256=7c974f525d5bcde2712dd080e0b079a6c3802113a337f0ab142522dcefbf452d ;;
    aarch64) tool_sha256=5a5e2f00a866f9fcedc9e74ee7d35c0744bf83e50be8080425df2d81b2dde71e ;;
    *) echo 'Unsupported package architecture.' >&2; exit 1 ;;
esac

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/cressida-release.XXXXXX")
mkdir -p "$root/dist" "$root/ci-logs/packages"
exec > >(tee "$root/ci-logs/packages/build.log") 2>&1
trap 'find "$work/state/build" -path "*/meson-logs/meson-log.txt" -exec cp --parents {} "$root/ci-logs/packages/" \; 2>/dev/null || true' EXIT

version=$(python3 "$root/.github/workflows/scripts/prepare_release_packages.py" "$RELEASE_REF" "$work")
manifest="$work/dev.lysec.Cressida.json"
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user --noninteractive flathub org.freedesktop.Sdk//26.08 org.freedesktop.Platform//26.08
flatpak-builder --user --force-clean --disable-rofiles-fuse --jobs=2 \
    --state-dir="$work/state" --repo="$work/repo" --default-branch=stable \
    "$work/build" "$manifest"
flatpak build-bundle --arch="$RELEASE_ARCH" \
    --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
    "$work/repo" "$root/dist/cressida-$version-$RELEASE_ARCH.flatpak" dev.lysec.Cressida stable

curl --fail --location --retry 3 \
    "https://github.com/probonopd/go-appimage/releases/download/continuous/appimagetool-951-$RELEASE_ARCH.AppImage" \
    --output "$work/appimagetool.AppImage"
printf '%s  %s\n' "$tool_sha256" "$work/appimagetool.AppImage" | sha256sum --check -
chmod +x "$work/appimagetool.AppImage"
mkdir "$work/tool"
(cd "$work/tool" && "$work/appimagetool.AppImage" --appimage-extract)
bash "$root/.github/workflows/scripts/build-release-deployer.sh" "$work"
flatpak build --filesystem="$work" --filesystem="$root:ro" \
    "$work/build" bash "$root/.github/workflows/scripts/build-release-appimage.sh" "$work" "$version" "$RELEASE_ARCH"
install -m755 "$work/cressida-$version-$RELEASE_ARCH.AppImage" "$root/dist/"
bash "$root/.github/workflows/scripts/smoke-release-packages.sh" \
    "$root/dist/cressida-$version-$RELEASE_ARCH.AppImage" \
    "$root/dist/cressida-$version-$RELEASE_ARCH.flatpak" "$root/ci-logs/packages/smoke"
