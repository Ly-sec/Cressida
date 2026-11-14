#!/usr/bin/env bash
set -euo pipefail

work=${1:?Set the package build directory}
arch=${2:?Set the native architecture}
# Retained releases keep their assets when the continuous build is replaced.
release=832
version=823
case "$arch" in
    x86_64) checksum=46de8381cfbd142da0ac8af24f34aa8b7e1435857bac2edff3427751047e4bcb ;;
    aarch64) checksum=0e5425584b60111833cd8df5fea7424d16866ba86adcca485c6dcf2708b1ab99 ;;
    *) echo 'Unsupported AppImage tool architecture.' >&2; exit 1 ;;
esac

curl --fail --location --retry 3 \
    "https://github.com/probonopd/go-appimage/releases/download/$release/appimagetool-$version-$arch.AppImage" \
    --output "$work/appimagetool.AppImage"
printf '%s  %s\n' "$checksum" "$work/appimagetool.AppImage" | sha256sum --check -
chmod +x "$work/appimagetool.AppImage"
mkdir "$work/tool"
(cd "$work/tool" && "$work/appimagetool.AppImage" --appimage-extract)
test -x "$work/tool/squashfs-root/usr/bin/mksquashfs"
test -x "$work/tool/squashfs-root/usr/bin/runtime-$arch"
