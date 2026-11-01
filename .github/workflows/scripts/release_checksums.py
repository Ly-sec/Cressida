"""Require the complete release asset set and write its SHA-256 checksums."""

import argparse
import hashlib
from pathlib import Path

from release_archive import release_version


def write_checksums(destination, version):
    names = [f"cressida-{version}.tar.xz"]
    names += [f"cressida-{version}-{arch}.{kind}"
              for arch in ("x86_64", "aarch64") for kind in ("AppImage", "flatpak")]
    lines = []
    for name in sorted(names):
        path = destination / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Missing or invalid release artifact: {name}")
        with path.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        lines.append(f"{digest}  {name}\n")
    (destination / "SHA256SUMS").write_text("".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ref")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    try:
        write_checksums(root / "dist", release_version(args.ref, root))
    except ValueError as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
