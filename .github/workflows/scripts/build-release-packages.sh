#!/usr/bin/env bash
set -euo pipefail

: "${RELEASE_REF:?Select a release tag}"
: "${RELEASE_ARCH:?Set the native package architecture}"
if [[ "$RELEASE_ARCH" != "$(uname -m)" ]]; then
    echo 'Release packages must be built on their native architecture.' >&2
    exit 1
fi
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/cressida-release.XXXXXX")
mkdir -p "$root/dist" "$root/ci-logs/packages"
exec > >(tee "$root/ci-logs/packages/build.log") 2>&1
trap 'find "$work/state/build" -path "*/meson-logs/meson-log.txt" -exec cp --parents {} "$root/ci-logs/packages/" \; 2>/dev/null || true' EXIT

bash "$root/.github/workflows/scripts/prepare-release-appimage-tool.sh" "$work" "$RELEASE_ARCH"
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

bash "$root/.github/workflows/scripts/build-release-deployer.sh" "$work"
flatpak build --filesystem="$work" --filesystem="$root:ro" \
    "$work/build" bash "$root/.github/workflows/scripts/build-release-appimage.sh" "$work" "$version" "$RELEASE_ARCH"
install -m755 "$work/cressida-$version-$RELEASE_ARCH.AppImage" "$root/dist/"
bash "$root/.github/workflows/scripts/smoke-release-packages.sh" \
    "$root/dist/cressida-$version-$RELEASE_ARCH.AppImage" \
    "$root/dist/cressida-$version-$RELEASE_ARCH.flatpak" "$root/ci-logs/packages/smoke"
