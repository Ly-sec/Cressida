#!/usr/bin/env bash
set -euo pipefail

# Run inside the build SDK so deployment resolves its libraries, not host paths.
work=${1:?Set the package build directory}
version=${2:?Set the release version}
arch=${3:?Set the native architecture}
appdir="$work/AppDir"
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
mkdir -p "$appdir/usr"
cp -a /app/. "$appdir/usr/"
rm -rf -- "$appdir/usr/lib/debug"
mkdir -p "$appdir/usr/share/fonts/noto-emoji"
cp /usr/share/fonts/noto-emoji/NotoColorEmoji.ttf "$appdir/usr/share/fonts/noto-emoji/"
cp -a /usr/share/mime "$appdir/usr/share/"
mkdir -p "$appdir/usr/share/X11"
cp -a /usr/share/X11/locale "$appdir/usr/share/X11/"
cp -a /usr/share/licenses "$appdir/usr/share/sdk-licenses"
cp /usr/manifest.json "$appdir/usr/share/freedesktop-sdk-manifest.json"
install -Dm644 "$source_dir/packaging/appimage/go-appimage.LICENSE" \
    "$appdir/usr/share/licenses/go-appimage/LICENSE"
case "$arch" in
    x86_64) triplet=x86_64-linux-gnu ;;
    aarch64) triplet=aarch64-linux-gnu ;;
    *) echo 'Unsupported AppImage architecture.' >&2; exit 1 ;;
esac
mkdir -p "$appdir/usr/lib/$triplet/GL"
cp -a "/usr/lib/$triplet/GL/default" "$appdir/usr/lib/$triplet/GL/"

# The packaging tool must never publish assets itself; the release job owns that.
unset GITHUB_TOKEN GH_TOKEN GITHUB_REPOSITORY GITHUB_REF GITHUB_RUN_NUMBER TRAVIS_REPO_SLUG
export PATH="$work/tool/squashfs-root/usr/bin:$PATH"
export VERSION="$version" ARCH="$arch"
tool="$work/tool/squashfs-root/usr/bin/appimagetool"
interpreter=$(patchelf --print-interpreter "$appdir/usr/bin/cressida")
"$tool" -s deploy "$appdir/usr/share/applications/dev.lysec.Cressida.desktop"

# Resolve the loader and bundled resources relative to the extracted image.
mv "$appdir/AppRun" "$appdir/AppRun.loader"
sed -i "s|^LD_LINUX=.*|LD_LINUX=\"\$HERE$interpreter\"|" "$appdir/AppRun.loader"
test -x "$appdir$interpreter"
cat > "$appdir/AppRun" <<'APPRUN'
#!/bin/sh
appdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export CRESSIDA_EMOJI_FONT="$appdir/usr/share/fonts/noto-emoji/NotoColorEmoji.ttf"
export CRESSIDA_ASSETS_DIR="$appdir/usr/share/cressida/assets"
export XLOCALEDIR="$appdir/usr/share/X11/locale"
case "$(uname -m)" in
    x86_64) triplet=x86_64-linux-gnu ;;
    aarch64) triplet=aarch64-linux-gnu ;;
    *) exit 1 ;;
esac
graphics="$appdir/usr/lib/$triplet/GL/default"
export LIBGL_DRIVERS_PATH="$graphics/lib/dri"
export __EGL_VENDOR_LIBRARY_DIRS="$graphics/share/glvnd/egl_vendor.d:/usr/share/glvnd/egl_vendor.d"
exec "$appdir/AppRun.loader" "$@"
APPRUN
chmod +x "$appdir/AppRun"
cd "$work"
"$tool" "$appdir"
mv "$work/Cressida-$version-$arch.AppImage" "$work/cressida-$version-$arch.AppImage"
