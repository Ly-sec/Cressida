#!/usr/bin/env bash
set -euo pipefail

work=${1:?Set the package build directory}
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
source_sha256=10c28f76758422fc1d3e4fa38ac2bc88e27d18ad8525a917ab0a71ded3c1135c
curl --fail --location --retry 3 \
    https://codeload.github.com/probonopd/go-appimage/tar.gz/b7864b5e53d4d7a1d5f31fdc23d1e591d77b8a21 \
    --output "$work/deployer-source.tar.gz"
printf '%s  %s\n' "$source_sha256" "$work/deployer-source.tar.gz" | sha256sum --check -
mkdir "$work/deployer-source"
tar -xzf "$work/deployer-source.tar.gz" --strip-components=1 -C "$work/deployer-source"
patch -d "$work/deployer-source" -p1 < "$root/packaging/appimage/built-in-pixbuf.patch"
cd "$work/deployer-source"
CGO_ENABLED=0 GOTOOLCHAIN=local GOVCS='*:off' GOPROXY=https://proxy.golang.org \
    GOMODCACHE="$work/go-modules" GOCACHE="$work/go-cache" \
    go build -mod=readonly -trimpath -buildvcs=false \
    -ldflags='-s -w' -o "$work/tool/squashfs-root/usr/bin/appimagetool" ./src/appimagetool
