#!/bin/sh
set -eu

# Run inside a disposable Alpine container, with the checkout owned by CI_UID.
: "${CI_UID:?Set CI_UID to the checkout owner}"
: "${CI_GID:?Set CI_GID to the checkout group}"
test -f /etc/alpine-release
apk upgrade --no-cache
apk add --no-cache \
    bash binutils build-base ca-certificates curl dbus ffmpeg meson ninja \
    pkgconf python3 su-exec cairo-dev curl-dev fontconfig-dev freetype-dev \
    glib-dev harfbuzz-dev libjxl-dev librsvg-dev libsecret-dev libwebp-dev \
    libxkbcommon-dev md4c-dev mesa-dev mpv-dev nlohmann-json pango-dev \
    rlottie-dev tomlplusplus-dev wayland-dev wayland-protocols zstd-dev \
    opus-dev libsodium-dev pulseaudio-dev openssl-dev libqrencode-dev \
    pipewire pipewire-dev \
    font-dejavu font-noto-emoji shared-mime-info xdg-utils \
    mesa-dri-gallium mesa-egl

# Alpine's stb package predates resize2; match the revision in nix/stb.nix.
stb_revision=f1c79c02822848a9bed4315b12c8c8f3761e1296
stb_sha256=a39b0f2d96d9b64171ac1fd73295b7c5866f904e653b379da886d050a6900db9
dependencies=$(mktemp -d)
trap 'rm -rf -- "$dependencies"' EXIT
curl --fail --location --retry 3 --connect-timeout 20 --max-time 120 \
    "https://codeload.github.com/nothings/stb/tar.gz/$stb_revision" \
    --output "$dependencies/stb.tar.gz"
echo "$stb_sha256  $dependencies/stb.tar.gz" | sha256sum -c -
tar -xzf "$dependencies/stb.tar.gz" -C "$dependencies"
install -d /usr/local/include/stb
install -m 644 "$dependencies/stb-$stb_revision/stb_image_resize2.h" \
    "$dependencies/stb-$stb_revision/stb_image_write.h" /usr/local/include/stb/

# Private D-Bus test buses require a passwd entry for the unprivileged UID.
if ! getent group "$CI_GID" >/dev/null; then
    addgroup -g "$CI_GID" cressida-ci
fi
if ! getent passwd "$CI_UID" >/dev/null; then
    ci_group=$(getent group "$CI_GID" | cut -d: -f1)
    adduser -D -H -u "$CI_UID" -G "$ci_group" -s /bin/sh cressida-ci
fi
ci_home=$(mktemp -d /tmp/cressida-musl-home.XXXXXX)
chown "$CI_UID:$CI_GID" "$ci_home"
export HOME="$ci_home"
su-exec "$CI_UID:$CI_GID" sh -eu <<'BUILD'
export XDG_CONFIG_HOME="$HOME/config"
export XDG_STATE_HOME="$HOME/state"
export XDG_CACHE_HOME="$HOME/cache"
export XDG_DATA_HOME="$HOME/data"
export XDG_DATA_DIRS=/usr/local/share:/usr/share
export LIBGL_ALWAYS_SOFTWARE=1
export CRESSIDA_ASSETS_DIR="$PWD/assets"
build_dir=${CRESSIDA_MUSL_BUILD_DIR:-build-musl}
meson setup "$build_dir" --prefix=/usr --buildtype=debugoptimized -Dcpp_std=c++23 \
    -Demoji_font=/usr/share/fonts/noto/NotoColorEmoji.ttf
meson compile -C "$build_dir" -j 4
meson test -C "$build_dir" --no-rebuild --print-errorlogs -j 4
BUILD
