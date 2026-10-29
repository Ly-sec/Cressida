import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from check_mls_snapshot import check_snapshot


class SnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        content = {"LICENSE": "Synthetic license\n", "source.cpp": "Original source\n"}
        self.upstream = {name: hashlib.sha256(data.encode()).hexdigest() for name, data in content.items()}
        for name, data in content.items():
            (self.root / name).write_text(data)
        (self.root / "upstream-files.json").write_text(json.dumps(self.upstream))
        (self.root / "patched-files.json").write_text("{}")
        (self.root / "PROVENANCE.md").write_text("Synthetic provenance\n")
        (self.root / "meson.build").write_text("# Synthetic build integration\n")

    def test_actual_snapshot_and_fixture_are_complete(self):
        check_snapshot(Path(__file__).resolve().parents[3] / "third_party/mlspp")
        check_snapshot(self.root)

    def test_unrecorded_changes_and_missing_notices_are_rejected(self):
        (self.root / "source.cpp").write_text("Changed\n")
        with self.assertRaises(ValueError):
            check_snapshot(self.root)
        (self.root / "source.cpp").write_text("Original source\n")
        (self.root / "LICENSE").unlink()
        with self.assertRaises(ValueError):
            check_snapshot(self.root)

    def test_only_explicitly_recorded_patches_are_allowed(self):
        data = b"Recorded local patch\n"
        (self.root / "source.cpp").write_bytes(data)
        patch = {"source.cpp": hashlib.sha256(data).hexdigest()}
        (self.root / "patched-files.json").write_text(json.dumps(patch))
        check_snapshot(self.root)
        patch["unknown.cpp"] = "0" * 64
        (self.root / "patched-files.json").write_text(json.dumps(patch))
        with self.assertRaises(ValueError):
            check_snapshot(self.root)

    def test_extra_files_and_symlinks_are_rejected(self):
        extra = self.root / "extra.cpp"
        extra.write_text("Unrecorded source")
        with self.assertRaises(ValueError):
            check_snapshot(self.root)
        extra.unlink()
        source = self.root / "source.cpp"
        source.unlink()
        source.symlink_to(self.root / "LICENSE")
        with self.assertRaises(ValueError):
            check_snapshot(self.root)


if __name__ == "__main__":
    unittest.main()
