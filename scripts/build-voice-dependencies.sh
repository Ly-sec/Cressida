#!/bin/sh
set -eu

# Install the same immutable native encryption sources used by nix/libdave.nix.
prefix=${1:-/usr/local}
jobs=${CRESSIDA_VOICE_BUILD_JOBS:-4}
case "$prefix" in
    /*) ;;
    *) echo 'The installation prefix must be an absolute path.' >&2; exit 1 ;;
esac
case "$jobs" in
    ''|0|*[!0-9]*) echo 'CRESSIDA_VOICE_BUILD_JOBS must be a positive integer.' >&2; exit 1 ;;
esac
if [ "$#" -gt 1 ]; then
    echo 'Usage: build-voice-dependencies.sh [absolute-prefix]' >&2
    exit 1
fi

mls_revision=1cc50a124a3bc4e143a787ec934280dc70c1034d
mls_sha256=7a9d6318627e548903bc65c3dc5a4de4a90290983efea9a49af8561ed3f999f5
dave_revision=6396c90aedbb9a6ee117ea9ce1ce9832af6a3375
dave_sha256=37784dd0c1476738426f0968a6420dc8590e4b40cb8aae42e36df3c663e18cc5
dependencies=$(mktemp -d "${TMPDIR:-/tmp}/cressida-voice-dependencies.XXXXXX")
trap 'rm -rf -- "$dependencies"' EXIT
trap 'exit 1' HUP INT TERM

download() {
    curl --fail --location --retry 3 --connect-timeout 20 --max-time 120 \
        "https://codeload.github.com/$1/tar.gz/$2" --output "$dependencies/$3.tar.gz"
    printf '%s  %s\n' "$4" "$dependencies/$3.tar.gz" | sha256sum -c -
    mkdir "$dependencies/$3"
    tar -xzf "$dependencies/$3.tar.gz" -C "$dependencies/$3" --strip-components=1
}
download cisco/mlspp "$mls_revision" mlspp "$mls_sha256"
download discord/libdave "$dave_revision" libdave "$dave_sha256"

python3 - "$dependencies" <<'PATCH'
from pathlib import Path
import sys

root = Path(sys.argv[1])
for directory in (root / "mlspp", root / "libdave/cpp"):
    configuration = directory / "CMakeLists.txt"
    original = configuration.read_text()
    expected = "set(CMAKE_CXX_STANDARD 17)"
    if original.count(expected) != 1:
        raise SystemExit("The pinned dependency's C++ standard declaration changed.")
    configuration.write_text(original.replace(expected, "set(CMAKE_CXX_STANDARD 23)"))
configuration = root / "mlspp/lib/CMakeLists.txt"
original = configuration.read_text()
for unused in ("mls_ds", "mls_vectors"):
    expected = f"add_subdirectory({unused})"
    if original.count(expected) != 1:
        raise SystemExit("The pinned MLS dependency layout changed.")
    original = original.replace(expected, "")
configuration.write_text(original)
PATCH

cmake -S "$dependencies/mlspp" -B "$dependencies/mlspp-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_RPATH="$prefix/lib" -DBUILD_SHARED_LIBS=ON -DTESTING=OFF \
    -DMLS_CXX_NAMESPACE=mlspp -DDISABLE_GREASE=ON
cmake --build "$dependencies/mlspp-build" --parallel "$jobs"
cmake --install "$dependencies/mlspp-build"
install -d "$prefix/share/licenses/cressida-mlspp" "$prefix/lib/pkgconfig"
install -m 644 "$dependencies/mlspp/LICENSE" "$prefix/share/licenses/cressida-mlspp/LICENSE"
cat > "$prefix/lib/pkgconfig/mlspp.pc" <<EOF
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include/mlspp

Name: mlspp
Description: MLS group protocol
Version: 0.1
Libs: -L"\${libdir}" -Wl,-rpath,"\${libdir}" -lmlspp -lhpke -ltls_syntax -lbytes
Cflags: -I"\${includedir}"
EOF

cmake -S "$dependencies/libdave/cpp" -B "$dependencies/libdave-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_RPATH="$prefix/lib" -DCMAKE_PREFIX_PATH="$prefix" \
    -DBUILD_SHARED_LIBS=ON -DPERSISTENT_KEYS=OFF -DTESTING=OFF
cmake --build "$dependencies/libdave-build" --parallel "$jobs"
cmake --install "$dependencies/libdave-build"
cat > "$prefix/lib/pkgconfig/libdave.pc" <<EOF
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libdave
Description: Audio frame encryption and MLS sessions
Version: 1.0
Libs: -L"\${libdir}" -Wl,-rpath,"\${libdir}" -ldave
Cflags: -I"\${includedir}"
EOF

printf 'Installed voice dependencies in %s\n' "$prefix"
