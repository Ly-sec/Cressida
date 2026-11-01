"""Stage a version-checked source archive and a native Flatpak build manifest."""

import argparse
import json
from pathlib import Path

from release_archive import archive_source, release_version


def prepare(root, destination, ref):
    version = release_version(ref, root)
    destination.mkdir(parents=True, exist_ok=True)
    source = archive_source(root, destination, version)
    manifest = json.loads((root / "packaging/flatpak/dev.lysec.Cressida.json").read_text())
    manifest["modules"][-1]["sources"] = [{"type": "archive", "path": source.name}]
    path = destination / "dev.lysec.Cressida.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n")
    return version, path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ref")
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    try:
        version, _ = prepare(root, args.destination, args.ref)
    except ValueError as error:
        parser.error(str(error))
    print(version)


if __name__ == "__main__":
    main()
