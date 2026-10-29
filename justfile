set positional-arguments

mode := "debug"
prefix := "/usr/local"
cpp-std := "c++23"
build-prefix := "build"

default:
    @just --list

# Configure debug, release, or asan builds.
_configure m=mode install_prefix="":
    #!/usr/bin/env bash
    set -euo pipefail
    case "$1" in
        debug)   args=(--buildtype=debug -Db_sanitize=none -Db_lto=false) ;;
        release) args=(--buildtype=release -Db_sanitize=none -Db_lto=true) ;;
        asan)    args=(--buildtype=debugoptimized -Doptimization=1 -Db_sanitize=address,undefined -Db_lto=false -Db_ndebug=false -Dstrip=false "-Dc_args=['-fno-omit-frame-pointer','-fno-optimize-sibling-calls','-g3']" "-Dcpp_args=['-fno-omit-frame-pointer','-fno-optimize-sibling-calls','-g3']") ;;
        *) echo "Unknown mode: $1 (expected debug, release, or asan)" >&2; exit 1 ;;
    esac
    # A generic AR override (as set by Nix) bypasses Meson's compiler-aware
    # gcc-ar/llvm-ar selection. Plain ar may omit every slim-LTO symbol.
    if [[ "$1" == release && "${AR:-ar}" == ar ]]; then
        unset AR
    fi
    build_path="{{build-prefix}}-$1"
    if [[ -f "$build_path/meson-private/coredata.dat" ]]; then
        args+=(--reconfigure)
    fi
    if [[ -n "$2" ]]; then
        args+=(--prefix "$2")
    fi
    meson setup "$build_path" "${args[@]}" -Dcpp_std={{cpp-std}} "-Dpkg_config_path=${PKG_CONFIG_PATH:-}"
    if [[ "{{build-prefix}}" == build ]]; then
        cp "$build_path/compile_commands.json" compile_commands.json
    fi

configure m=mode install_prefix=prefix: (_configure m install_prefix)

_ensure-configured m=mode: (_configure m)

# Build the desktop client and its libraries.
build m=mode: (_ensure-configured m)
    meson compile -C {{build-prefix}}-{{m}} cressida
    @if [ '{{build-prefix}}' = build ]; then cp {{build-prefix}}-{{m}}/compile_commands.json compile_commands.json; fi

_with-sanitizers m *command:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ "$1" == asan ]]; then
        export ASAN_SYMBOLIZER_PATH="${ASAN_SYMBOLIZER_PATH:-$(command -v llvm-symbolizer || true)}"
        if [[ -z "$ASAN_SYMBOLIZER_PATH" ]]; then
            echo 'llvm-symbolizer is missing; re-enter nix develop.' >&2
            exit 1
        fi
        export ASAN_OPTIONS="symbolize=1:abort_on_error=1:detect_stack_use_after_return=1:fast_unwind_on_malloc=0:malloc_context_size=40${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
        export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:report_error_type=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"
    fi
    shift
    exec "$@"

run m=mode *args: (build m)
    #!/usr/bin/env bash
    set -euo pipefail
    # Driver/font global allocations can obscure the crash report on GUI exit.
    if [[ "$1" == asan ]]; then
        export ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
    fi
    exec just _with-sanitizers "$1" "./{{build-prefix}}-$1/cressida" "${@:2}"

# Run the normal build in GDB and print stack traces if it crashes.
debug *args: (build "debug")
    #!/usr/bin/env bash
    set -euo pipefail
    exec gdb -q --batch \
        -ex 'set pagination off' -ex 'set debuginfod enabled off' \
        -ex 'set print frame-arguments none' -ex 'set print entry-values no' \
        -ex 'handle SIGPIPE nostop noprint pass' \
        -ex run -ex 'thread apply all bt 32' \
        --args ./{{build-prefix}}-debug/cressida "$@"

demo m=mode:
    @just --set build-prefix '{{build-prefix}}' run {{m}} --demo

# Render one demo frame on the current Wayland desktop and exit, without network access.
smoke m=mode:
    @just --set build-prefix '{{build-prefix}}' run {{m}} --smoke-test

# Build and run text and image regression tests; accepts Meson test arguments.
test m=mode *args: (_ensure-configured m)
    #!/usr/bin/env bash
    set -euo pipefail
    exec just _with-sanitizers "$1" meson test -C "{{build-prefix}}-$1" --print-errorlogs "${@:2}"

# Install a release build; an optional prefix overrides the configured destination.
install m="release" install_prefix="": (_configure m install_prefix)
    meson compile -C {{build-prefix}}-{{m}} cressida
    meson install --no-rebuild -C {{build-prefix}}-{{m}}

# Use the existing install manifest without reconfiguring or rebuilding.
uninstall m="release":
    #!/usr/bin/env bash
    set -euo pipefail
    case "$1" in debug|release|asan) ;; *) echo "Unknown mode: $1 (expected debug, release, or asan)" >&2; exit 1 ;; esac
    # Run the same manifest handler as Meson's Ninja target without regenerating it.
    cd "{{build-prefix}}-$1"
    exec meson --internal uninstall

# Format project C++ and Nix files; external decoder code is excluded.
format:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
    nixfmt flake.nix nix/*.nix

# Check formatting without changing files.
format-check:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format --dry-run -Werror
    nixfmt --check flake.nix nix/*.nix

# Validate workflow syntax, shell helpers and repository automation.
lint: format-check
    actionlint
    shellcheck .github/workflows/scripts/*.sh
    python3 -m unittest discover -s .github/workflows/scripts -p 'test_*.py'

# Exercise offline UI replays in a private software-rendered Wayland session.
smoke-headless binary="build-debug/cressida":
    bash .github/workflows/scripts/smoke-headless.sh '{{binary}}'

# Build and test the locked package in the Nix sandbox.
check:
    nix flake check path:.

clean m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    case "$1" in debug|release|asan) ;; *) echo "Unknown mode: $1" >&2; exit 1 ;; esac
    rm -rf -- "{{build-prefix}}-$1"

rebuild m=mode: (clean m) (build m)
