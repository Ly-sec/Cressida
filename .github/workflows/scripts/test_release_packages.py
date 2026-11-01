"""Exercise native release staging and complete asset checks without publishing."""

import hashlib
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

from prepare_release_packages import prepare
from release_checksums import write_checksums


class ReleasePackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "nix").mkdir()
        (self.root / "meson.build").write_text("project('fixture', version: '0.1.0')")
        (self.root / "nix/package.nix").write_text('version = "0.1.0";')
        (self.root / "packaging/flatpak").mkdir(parents=True)
        self.manifest = self.root / "packaging/flatpak/dev.lysec.Cressida.json"
        self.manifest.write_text(json.dumps({
            "app-id": "dev.lysec.Cressida", "modules": [{
                "name": "cressida", "sources": [{"type": "archive", "path": "cressida-source.tar.xz"}],
            }],
        }))
        self.output = self.root / "dist"

    def test_staged_manifest_uses_the_checked_source_archive(self):
        (self.root / ".flatpak-builder").mkdir()
        (self.root / ".flatpak-builder/private-cache").write_text("not source")
        original = self.manifest.read_bytes()
        version, manifest = prepare(self.root, self.output, "refs/tags/v0.1.0")
        self.assertEqual(version, "0.1.0")
        source = json.loads(manifest.read_text())["modules"][-1]["sources"][0]
        self.assertEqual(source, {"type": "archive", "path": "cressida-0.1.0.tar.xz"})
        with tarfile.open(self.output / source["path"]) as archive:
            self.assertIn("cressida-0.1.0/packaging/flatpak/dev.lysec.Cressida.json", archive.getnames())
            self.assertFalse(any(".flatpak-builder" in name for name in archive.getnames()))
        self.assertEqual(self.manifest.read_bytes(), original)

    def test_invalid_tag_cannot_stage_packages(self):
        for ref in ("refs/heads/main", "refs/tags/v0.2.0"):
            with self.assertRaises(ValueError):
                prepare(self.root, self.output, ref)
        self.assertFalse(self.output.exists())

    def assets(self):
        self.output.mkdir()
        names = ["cressida-0.1.0.tar.xz"]
        names += [f"cressida-0.1.0-{arch}.{kind}"
                  for arch in ("x86_64", "aarch64") for kind in ("AppImage", "flatpak")]
        for name in names:
            (self.output / name).write_bytes(name.encode())
        return names

    def test_checksums_cover_both_packages_for_each_architecture(self):
        names = self.assets()
        write_checksums(self.output, "0.1.0")
        expected = "".join(f"{hashlib.sha256(name.encode()).hexdigest()}  {name}\n" for name in sorted(names))
        self.assertEqual((self.output / "SHA256SUMS").read_text(), expected)

    def test_incomplete_assets_cannot_replace_checksums(self):
        names = self.assets()
        checksum = self.output / "SHA256SUMS"
        checksum.write_text("prior checksums")
        asset = self.output / names[-1]
        asset.unlink()
        for state in ("missing", "empty", "symlink"):
            if state == "empty":
                asset.touch()
            elif state == "symlink":
                asset.unlink()
                asset.symlink_to(self.output / names[0])
            with self.subTest(state=state), self.assertRaises(ValueError):
                write_checksums(self.output, "0.1.0")
            self.assertEqual(checksum.read_text(), "prior checksums")
