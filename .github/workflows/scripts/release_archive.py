"""Build a complete source release from a clean Actions checkout, without Git."""

import argparse
import hashlib
from pathlib import Path
import re
import tarfile


def release_version(ref, root):
    match = re.fullmatch(r"refs/tags/v(\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)", ref)
    if not match:
        raise ValueError("Run Release on a vMAJOR.MINOR.PATCH tag (optional prerelease suffix).")
    version = match[1]
    try:
        lines = (root / "VERSION").read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise ValueError("Could not read VERSION.") from error
    if len(lines) != 1 or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?", lines[0]):
        raise ValueError("VERSION must contain one MAJOR.MINOR.PATCH line (optional prerelease suffix).")
    if lines[0] != version:
        raise ValueError(f"VERSION must match the tag ({version}).")
    return version


def archive_source(root, destination, version):
    excluded = {".git", ".direnv", ".agents", ".aws", ".codex", ".flatpak-builder", "__pycache__", "dist", "ci-logs", "result"}
    prefix = f"cressida-{version}"

    def include(info):
        relative = Path(info.name).parts[1:]
        if any(part in excluded for part in relative):
            return None
        if relative and (relative[0].startswith("build") or relative[0] == "compile_commands.json"):
            return None
        info.uid = info.gid = 0
        info.uname = info.gname = "root"
        info.mtime = 0
        return info

    destination.mkdir(parents=True, exist_ok=True)
    path = destination / f"{prefix}.tar.xz"
    with tarfile.open(path, "w:xz") as archive:
        archive.add(root, arcname=prefix, filter=include)
    with path.open("rb") as source:
        checksum = hashlib.file_digest(source, "sha256").hexdigest()
    (destination / "SHA256SUMS").write_text(f"{checksum}  {path.name}\n")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ref")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    try:
        version = release_version(args.ref, root)
    except ValueError as error:
        parser.error(str(error))
    if not args.check:
        print(archive_source(root, root / "dist", version))


if __name__ == "__main__":
    main()
