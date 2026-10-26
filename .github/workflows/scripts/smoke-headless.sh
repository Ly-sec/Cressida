#!/usr/bin/env bash
set -euo pipefail

binary=$(realpath "${1:-build-debug/cressida}")
test -x "$binary"
log_dir=$(realpath -m "${CRESSIDA_CI_LOG_DIR:-ci-logs/smoke}")
mkdir -p "$log_dir"
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

mkdir -p "$test_root"/{runtime,config,state,cache,data}
chmod 700 "$test_root/runtime"
export XDG_RUNTIME_DIR="$test_root/runtime"
export XDG_CONFIG_HOME="$test_root/config"
export XDG_STATE_HOME="$test_root/state"
export XDG_CACHE_HOME="$test_root/cache"
export XDG_DATA_HOME="$test_root/data"
export DBUS_SESSION_BUS_ADDRESS=unix:path=/cressida-ci-no-session-bus
export WAYLAND_DISPLAY=cressida-ci
export LIBGL_ALWAYS_SOFTWARE=1
if [[ -n "${CRESSIDA_CI_EGL_VENDOR:-}" ]]; then
    export __EGL_VENDOR_LIBRARY_FILENAMES="$CRESSIDA_CI_EGL_VENDOR"
    export LIBGL_DRIVERS_PATH="$CRESSIDA_CI_MESA_DRIVERS"
fi
unset LD_PRELOAD CRESSIDA_ASSETS_DIR DISCORD_CPP_ASSETS_DIR

weston --backend=headless --renderer=gl --no-config --width=1600 --height=1000 \
    --idle-time=0 --socket="$WAYLAND_DISPLAY" --log="$log_dir/weston.log" \
    > "$log_dir/weston-output.log" 2>&1 &
weston_pid=$!
for ((attempt = 0; attempt < 100; ++attempt)); do
    [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] && break
    if ! kill -0 "$weston_pid" 2>/dev/null; then
        cat "$log_dir/weston-output.log" "$log_dir/weston.log" >&2
        exit 1
    fi
    sleep 0.05
done
test -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"

for view in ime-cycle ime-cycle-light ime-cycle-narrow voice-cycle voice-cycle-light voice-cycle-narrow inbox-cycle inbox-cycle-light inbox-cycle-narrow search-cycle search-cycle-light search-cycle-narrow outgoing-cycle outgoing-cycle-light chat-preload-cycle connection-cycle history-cycle dm-home-cycle dm-home-cycle-light dm-profile-cycle dm-profile-cycle-light cosmetics-cycle cosmetics-cycle-light cosmetics-cycle-hover cosmetics-cycle-hover-light \
    deleted-cycle reduced-motion-cycle reduced-motion-cycle-light markdown-cycle markdown-cycle-light syntax-terminal syntax-terminal-light autocomplete-cycle settings-font-cycle themes-cycle sticker-cycle \
    server-menu-cycle folder-menu-cycle folder-animation folder-animation-light rail-drag rail-drag-light typing-cycle typing-cycle-light reply-cycle reply-cycle-light floating-cycle floating-cycle-light decorations-cycle decorations-cycle-light channel-menu-cycle reaction-spacing embed-components chat-links-cycle chat-links-cycle-light \
    chat-links-light chat-links-narrow embed-cycle embed-cycle-light categories animation-cycle gif-cycle attachment-cycle \
    autocomplete-tokens autocomplete-tokens-light reply-grouping reply-grouping-light reply-grouping-compact \
    media-gallery-cycle media-gallery-cycle-light image-menu-cycle image-menu-cycle-light tray-cycle \
    tray-startup-host tray-startup-missing tray-startup-open tray-startup-notification tray-startup-host-loss \
    window-controls-cycle window-controls-cycle-light server-settings-cycle server-settings-narrow server-settings-role-light; do
    echo "Offline UI replay: $view"
    launch_flags=(--smoke-test)
    if [[ "$view" == tray-* ]]; then
        launch_flags+=(--start-minimized)
    fi
    CRESSIDA_DEMO_VIEW="$view" timeout 45 "$binary" "${launch_flags[@]}" > "$log_dir/$view.log" 2>&1 || {
        cat "$log_dir/$view.log" >&2
        exit 1
    }
done
if [[ -n "$(find "$XDG_STATE_HOME" "$XDG_CONFIG_HOME" -type f -print -quit)" ]]; then
    echo 'Offline UI replays unexpectedly wrote account/configuration state.' >&2
    exit 1
fi
