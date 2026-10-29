"""Verify the licensed MLS source snapshot and its recorded local patches offline."""

import hashlib
import json
from pathlib import Path


def check_snapshot(root):
    root = root.resolve()
    upstream = json.loads((root / "upstream-files.json").read_text())
    patched = json.loads((root / "patched-files.json").read_text())
    if "LICENSE" not in upstream or not patched.keys() <= upstream.keys():
        raise ValueError("Missing upstream license or unknown local patch")
    for name, digest in upstream.items():
        path = root / name
        if path.is_symlink() or not path.resolve().is_relative_to(root) or not path.is_file():
            raise ValueError(f"Missing or unsafe snapshot file: {name}")
        expected = patched.get(name, digest)
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f"Unrecorded snapshot change: {name}")
    own_files = {"upstream-files.json", "patched-files.json", "PROVENANCE.md", "meson.build"}
    present = {str(path.relative_to(root)) for path in root.rglob("*") if path.is_file()}
    if present != upstream.keys() | own_files:
        raise ValueError("The snapshot has missing or unrecorded files")


if __name__ == "__main__":
    check_snapshot(Path(__file__).resolve().parents[3] / "third_party/mlspp")
