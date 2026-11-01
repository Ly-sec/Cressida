import hashlib
from pathlib import Path
import tarfile
import tempfile
import unittest

from release_archive import archive_source, release_version


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "nix").mkdir()
        (self.root / "VERSION").write_text("0.1.0\n")
        (self.root / "meson.build").write_text("project('cressida', version: files('VERSION'))")
        (self.root / "nix/package.nix").write_text("version = lib.fileContents ../VERSION;")

    def test_version_must_match_the_tag(self):
        self.assertEqual(release_version("refs/tags/v0.1.0", self.root), "0.1.0")
        for ref in ("refs/heads/main", "refs/tags/v1", "refs/tags/v../bad", "refs/tags/v0.2.0"):
            with self.assertRaises(ValueError):
                release_version(ref, self.root)
        (self.root / "VERSION").write_text("0.2.0\n")
        with self.assertRaises(ValueError):
            release_version("refs/tags/v0.1.0", self.root)

    def test_version_changes_and_prereleases_use_the_same_file(self):
        for version in ("1.2.3", "2.0.0-rc.1"):
            for ending in ("", "\n"):
                with self.subTest(version=version, ending=ending):
                    (self.root / "VERSION").write_text(version + ending)
                    self.assertEqual(release_version(f"refs/tags/v{version}", self.root), version)

    def test_missing_or_malformed_version_cannot_release(self):
        for content in ("", "\n", "0.1.0\n0.1.0\n", "0.1.0\n\n", " 0.1.0\n", "v0.1.0\n", "0.1\n"):
            with self.subTest(content=content), self.assertRaisesRegex(ValueError, "VERSION"):
                (self.root / "VERSION").write_text(content)
                release_version("refs/tags/v0.1.0", self.root)
        (self.root / "VERSION").unlink()
        with self.assertRaisesRegex(ValueError, "VERSION"):
            release_version("refs/tags/v0.1.0", self.root)

    def test_archive_contains_build_inputs_and_not_local_state(self):
        for name in ("LICENSE", "flake.nix", "flake.lock"):
            (self.root / name).write_text(name)
        for name in (".git", ".codex", "build-debug", "ci-logs"):
            (self.root / name).mkdir()
            (self.root / name / "private").write_text("excluded")
        (self.root / "assets").mkdir()
        (self.root / "assets/notice.txt").write_text("third-party notice")
        bundled = self.root / "third_party/mlspp"
        (bundled / "include").mkdir(parents=True)
        for name in ("LICENSE", "PROVENANCE.md", "upstream-files.json", "patched-files.json", "meson.build", "include/namespace.h"):
            (bundled / name).write_text("Synthetic bundled input")
        destination = self.root / "dist"
        path = archive_source(self.root, destination, "0.1.0")
        with tarfile.open(path) as archive:
            names = archive.getnames()
            for name in ("VERSION", "LICENSE", "flake.lock", "nix/package.nix", "assets/notice.txt"):
                self.assertIn(f"cressida-0.1.0/{name}", names)
            for item in ("LICENSE", "PROVENANCE.md", "upstream-files.json", "patched-files.json", "meson.build", "include/namespace.h"):
                self.assertIn(f"cressida-0.1.0/third_party/mlspp/{item}", names)
            self.assertFalse(any("private" in name or "/dist" in name for name in names))
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        self.assertEqual((destination / "SHA256SUMS").read_text(), f"{digest}  {path.name}\n")
        # Fixed ownership/timestamps make reruns byte-for-byte reproducible.
        archive_source(self.root, destination, "0.1.0")
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)


if __name__ == "__main__":
    unittest.main()
