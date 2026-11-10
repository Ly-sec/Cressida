"""Exercise website feed preparation without releases, credentials or publication."""

import base64
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from flatpak_repository import prepare, repository_base, execute, ARCHITECTURES, APPLICATION


FINGERPRINT = "A" * 40


class FlatpakRepositoryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.site = Path(self.temporary.name) / "dist"
        self.site.mkdir()
        (self.site / "index.html").write_text("Existing website")
        self.files = {f"cressida-1.2.3-{arch}.flatpak": (arch + " bundle").encode() for arch in ARCHITECTURES}
        self.files["SHA256SUMS"] = "".join(
            hashlib.sha256(content).hexdigest() + "  " + name + "\n" for name, content in self.files.items()
        ).encode()
        self.release = {"tagName": "v1.2.3", "isDraft": False, "isPrerelease": False,
                        "assets": [{"name": name, "state": "uploaded", "size": len(content),
                                    "digest": "sha256:" + hashlib.sha256(content).hexdigest()}
                                   for name, content in self.files.items()]}
        self.calls = []
        self.corrupt = False
        self.passphrase = ""
        self.unlock_failure = False

    def runner(self, arguments, *, input=None):
        self.calls.append(arguments)
        if arguments[:3] == ["gh", "release", "view"]:
            return json.dumps(self.release).encode()
        if arguments[:3] == ["gh", "release", "download"]:
            name = arguments[arguments.index("--pattern") + 1]
            data = self.files[name]
            if self.corrupt and name.endswith("flatpak"):
                data = b"x" * len(data)
            (Path(arguments[arguments.index("--dir") + 1]) / name).write_bytes(data)
        elif arguments[0] == "gpg":
            if "--list-secret-keys" in arguments:
                return ("sec:::::::::\nfpr:::::::::" + FINGERPRINT + ":\n").encode()
            if "--export" in arguments:
                return b"public key"
            if "--detach-sign" in arguments:
                self.assertIn("loopback", arguments)
                self.assertIn("--passphrase-fd", arguments)
                self.assertEqual(input, self.passphrase.encode() + b"\n")
                self.assertNotIn(self.passphrase, arguments)
                if self.unlock_failure:
                    raise ValueError("Synthetic unlock failure")
                Path(arguments[arguments.index("--output") + 1]).write_bytes(b"probe signature")
                return b""
            self.assertEqual(input, b"private test key")
        elif arguments[:2] == ["flatpak", "build-import-bundle"]:
            self.assertIn("--gpg-sign=" + FINGERPRINT, arguments)
            Path(arguments[-2]).mkdir(exist_ok=True)
        elif arguments[0] == "ostree":
            Path(arguments[1].removeprefix("--repo=")).mkdir()
        elif arguments[0] == "gpgconf":
            return b""
        elif arguments[:2] == ["flatpak", "build-update-repo"]:
            self.assertIn("--gpg-sign=" + FINGERPRINT, arguments)
            (Path(arguments[-1]) / "summary").write_bytes(b"summary")
            (Path(arguments[-1]) / "summary.sig").write_bytes(b"signature")
        elif arguments[:2] == ["flatpak", "repo"]:
            return "".join(f"app/{APPLICATION}/{arch}/stable\n" for arch in ARCHITECTURES).encode()
        else:
            self.fail("Unexpected preparation command")
        return b""

    def test_site_and_subpath_are_preserved(self):
        for path in ("https://example.test", "https://example.test/cressida-website/"):
            with self.subTest(path=path):
                if (self.site / "flatpak").exists():
                    import shutil
                    shutil.rmtree(self.site / "flatpak")
                self.assertEqual(prepare(self.site, path, "private test key", FINGERPRINT, runner=self.runner), "v1.2.3")
                self.assertEqual((self.site / "index.html").read_text(), "Existing website")
                descriptor = (self.site / "flatpak/cressida.flatpakref").read_text()
                self.assertIn("Url=" + path.rstrip("/") + "/flatpak/repo/", descriptor)
                self.assertIn("GPGKey=" + base64.b64encode(b"public key").decode(), descriptor)
                self.assertIn("Branch=stable", descriptor)
                self.assertEqual((self.site / "flatpak/cressida.gpg").read_bytes(), b"public key")
                self.assertFalse(any("keyring" in str(file) for file in self.site.rglob("*")))

    def test_failures_keep_the_existing_site(self):
        cases = ("corrupt", "draft", "prerelease", "key", "metadata", "too_large")
        for case in cases:
            with self.subTest(case=case):
                original = json.loads(json.dumps(self.release))
                if case == "corrupt": self.corrupt = True
                if case == "draft": self.release["isDraft"] = True
                if case == "prerelease": self.release["isPrerelease"] = True
                if case == "metadata": self.release["assets"][0]["digest"] = None
                with patch("flatpak_repository.MAXIMUM_PAGES_BYTES", 1 if case == "too_large" else 1_000_000_000):
                    with self.assertRaises(ValueError):
                        prepare(self.site, "https://example.test/project", "private test key",
                                "B" * 40 if case == "key" else FINGERPRINT, runner=self.runner)
                self.assertEqual((self.site / "index.html").read_text(), "Existing website")
                self.assertFalse((self.site / "flatpak").exists())
                self.release = original
                self.corrupt = False

    def test_invalid_destinations_and_duplicate_assets(self):
        for url in ("http://example.test", "https://user@example.test", "https://example.test/path?token=secret",
                    "https://example.test/../site", "https://example.test/#fragment"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                repository_base(url)
        self.release["assets"].append(self.release["assets"][0])
        with self.assertRaises(ValueError):
            prepare(self.site, "https://example.test", "private test key", FINGERPRINT, runner=self.runner)

    def test_passphrase_uses_stdin_and_stays_outside_the_artifact(self):
        self.passphrase = "fixture phrase ; $(literal)"
        prepare(self.site, "https://example.test/project", "private test key", FINGERPRINT,
                passphrase=self.passphrase, runner=self.runner)
        for file in self.site.rglob("*"):
            if file.is_file():
                self.assertNotIn(self.passphrase.encode(), file.read_bytes())
                self.assertNotIn(b"private test key", file.read_bytes())
        self.assertFalse(any(file.name.startswith("signing-check") for file in self.site.rglob("*")))

    def test_wrong_or_multiline_passphrase_does_not_create_a_feed(self):
        for phrase in ("wrong phrase", "invalid\nphrase", "invalid\rphrase", "invalid\0phrase"):
            with self.subTest(phrase=phrase), self.assertRaises(ValueError):
                self.passphrase = phrase
                self.unlock_failure = True
                prepare(self.site, "https://example.test", "private test key", FINGERPRINT,
                        passphrase=phrase, runner=self.runner)
            self.assertFalse((self.site / "flatpak").exists())
            self.assertEqual((self.site / "index.html").read_text(), "Existing website")

    def test_commands_do_not_inherit_signing_secrets(self):
        environment = {"CRESSIDA_FLATPAK_GPG_KEY": "private fixture", "CRESSIDA_FLATPAK_GPG_PASSPHRASE": "secret fixture",
                       "GH_TOKEN": "release fixture token"}
        with patch.dict(os.environ, environment), patch("flatpak_repository.subprocess.run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = b""
            execute(["gpg", "--version"])
            child = run.call_args.kwargs["env"]
            self.assertNotIn("CRESSIDA_FLATPAK_GPG_KEY", child)
            self.assertNotIn("CRESSIDA_FLATPAK_GPG_PASSPHRASE", child)
            self.assertNotIn("GH_TOKEN", child)
            execute(["gh", "--version"])
            self.assertEqual(run.call_args.kwargs["env"]["GH_TOKEN"], "release fixture token")
            self.assertNotIn("CRESSIDA_FLATPAK_GPG_PASSPHRASE", run.call_args.kwargs["env"])


if __name__ == "__main__":
    unittest.main()
