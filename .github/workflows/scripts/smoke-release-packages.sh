#!/usr/bin/env bash
set -euo pipefail

image=${1:?Set the AppImage path}
bundle=${2:?Set the Flatpak bundle path}
logs=${3:?Set the package diagnostic directory}
test_root=$(mktemp -d)
weston_pid=
cleanup() {
    if [[ -n "$weston_pid" ]]; then
        kill "$weston_pid" 2>/dev/null || true
        wait "$weston_pid" 2>/dev/null || true
    fi
    rm -rf -- "$test_root"
}
trap cleanup EXIT
mkdir -p "$logs" "$test_root"/{runtime,config,state,cache,data,extracted}
chmod 700 "$test_root/runtime"
check_logged() {
    local output=$1
    shift
    if "$@" > "$output" 2>&1; then
        return 0
    else
        local status=$?
        echo "Package check failed: $output (exit $status)" >&2
        cat "$output" >&2
        return "$status"
    fi
}
check_logged "$logs/flatpak-install.log" flatpak install --user --noninteractive --reinstall "$bundle"
(cd "$test_root/extracted" && "$image" --appimage-extract > "$logs/extract.log")
export WAYLAND_DISPLAY=cressida-package-check LIBGL_ALWAYS_SOFTWARE=1
weston_env=("XDG_RUNTIME_DIR=$test_root/runtime")
if [[ -n "${CRESSIDA_CI_EGL_VENDOR:-}" ]]; then
    weston_env+=("__EGL_VENDOR_LIBRARY_FILENAMES=$CRESSIDA_CI_EGL_VENDOR"
        "LIBGL_DRIVERS_PATH=$CRESSIDA_CI_MESA_DRIVERS")
fi
env "${weston_env[@]}" weston --backend=headless --renderer=gl --no-config \
    --width=1600 --height=1000 --idle-time=0 --socket="$WAYLAND_DISPLAY" \
    --log="$logs/weston.log" > "$logs/weston-output.log" 2>&1 &
weston_pid=$!
for ((attempt = 0; attempt < 100; ++attempt)); do
    [[ -S "$test_root/runtime/$WAYLAND_DISPLAY" ]] && break
    if ! kill -0 "$weston_pid" 2>/dev/null; then
        cat "$logs/weston-output.log" "$logs/weston.log" >&2
        exit 1
    fi
    sleep 0.05
done
test -S "$test_root/runtime/$WAYLAND_DISPLAY"

for view in login-themed login-themed-light autocomplete-cycle sharing-cycle rpc-cycle media-gallery-cycle; do
    echo "Package smoke check: AppImage $view"
    check_logged "$logs/appimage-$view.log" env -u LD_PRELOAD -u CRESSIDA_ASSETS_DIR -u DISCORD_CPP_ASSETS_DIR \
        -u __EGL_VENDOR_LIBRARY_FILENAMES -u __EGL_VENDOR_LIBRARY_DIRS -u GIO_EXTRA_MODULES \
        XDG_RUNTIME_DIR="$test_root/runtime" XDG_CONFIG_HOME="$test_root/config" \
        XDG_STATE_HOME="$test_root/state" XDG_CACHE_HOME="$test_root/cache" \
        XDG_DATA_HOME="$test_root/data" DBUS_SESSION_BUS_ADDRESS=unix:path=/cressida-package-no-bus \
        CRESSIDA_DEMO_VIEW="$view" \
        timeout 60 "$test_root/extracted/squashfs-root/AppRun" --smoke-test
    echo "Package smoke check: Flatpak $view"
    check_logged "$logs/flatpak-$view.log" env XDG_RUNTIME_DIR="$test_root/runtime" \
        timeout 60 flatpak run --user --filesystem="$test_root" \
        --env=XDG_CONFIG_HOME="$test_root/config" \
        --env=XDG_STATE_HOME="$test_root/state" --env=XDG_CACHE_HOME="$test_root/cache" \
        --env=XDG_DATA_HOME="$test_root/data" --env=WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
        --env=LIBGL_ALWAYS_SOFTWARE=1 --env=DBUS_SESSION_BUS_ADDRESS=unix:path=/cressida-package-no-bus \
        --unset-env=CRESSIDA_ASSETS_DIR --unset-env=DISCORD_CPP_ASSETS_DIR --unset-env=LD_PRELOAD \
        --unset-env=__EGL_VENDOR_LIBRARY_FILENAMES --unset-env=LIBGL_DRIVERS_PATH --unset-env=GIO_EXTRA_MODULES \
        --env=CRESSIDA_DEMO_VIEW="$view" dev.lysec.Cressida --smoke-test
done
if [[ -n "$(find "$test_root/config" "$test_root/state" -type f -print -quit)" ]]; then
    echo 'Package replays unexpectedly wrote account or configuration state.' >&2
    exit 1
fi
