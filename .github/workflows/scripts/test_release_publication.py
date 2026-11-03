"""Check staged release publication using a private command peer, without accounts."""

import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from publish_release import publish


PEER = r'''
import hashlib
import json
import os
from pathlib import Path
import sys

state_path = Path(os.environ['CRESSIDA_RELEASE_STATE'])
record_path = Path(os.environ['CRESSIDA_RELEASE_RECORD'])
arguments = sys.argv[1:]
with record_path.open('a') as output:
    output.write(json.dumps(arguments) + '\n')
assert arguments[0] == 'release'
assert arguments[-2:] == ['--repo', 'example/cressida']
arguments = arguments[1:-2]
command, tag = arguments[:2]
state = json.loads(state_path.read_text())
release = state.get('release')
if command == 'view':
    if state.get('lookup_error'):
        print('API unavailable', file=sys.stderr)
        exit(1)
    if release is None:
        print('release not found', file=sys.stderr)
        exit(1)
    print(json.dumps(release))
elif command == 'create':
    assert release is None
    assert '--draft' in arguments and '--verify-tag' in arguments and '--generate-notes' in arguments
    release = state['release'] = {
        'databaseId': 31, 'tagName': tag, 'isDraft': True,
        'isPrerelease': '--prerelease' in arguments,
        'url': 'https://example.test/releases/tag/' + tag, 'assets': [],
    }
elif command == 'upload':
    assert release['isDraft']
    if state.get('upload_error'):
        print('Upload failed', file=sys.stderr)
        exit(1)
    assert arguments[-1] == '--clobber'
    release['assets'] = []
    for value in arguments[2:-1]:
        path = Path(value)
        release['assets'].append({
            'name': path.name, 'state': 'uploaded', 'size': path.stat().st_size,
            'digest': 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest(),
        })
    if state.get('bad_digest'):
        release['assets'][0]['digest'] = 'sha256:' + '0' * 64
    if state.get('pending_asset'):
        release['assets'][0]['state'] = 'new'
    if state.get('bad_size'):
        release['assets'][0]['size'] += 1
    if state.get('missing_digest'):
        release['assets'][0]['digest'] = None
    if state.get('extra_asset'):
        release['assets'].append({'name': 'unexpected.log'})
    if state.get('changed_identity'):
        release['databaseId'] += 1
elif command == 'edit':
    assert release['isDraft'] and arguments[2:] == ['--draft=false']
    assert len(release['assets']) == 6
    if not state.get('publish_noop'):
        release['isDraft'] = False
else:
    raise AssertionError(command)
state_path.write_text(json.dumps(state))
'''


class ReleasePublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cressida release publication ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "VERSION").write_text("v0.1.0\n")
        self.destination = self.root / "dist"
        self.destination.mkdir()
        self.state = self.root / "state.json"
        self.record = self.root / "commands.jsonl"
        commands = self.root / "commands"
        commands.mkdir()
        peer = commands / "gh"
        peer.write_text(f"#!{sys.executable}\n" + PEER)
        peer.chmod(0o700)
        self.environment = patch.dict(os.environ, {
            "PATH": str(commands) + os.pathsep + os.environ["PATH"],
            "CRESSIDA_RELEASE_STATE": str(self.state),
            "CRESSIDA_RELEASE_RECORD": str(self.record),
        })
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.write_state()
        self.names = self.assets()

    def write_state(self, release=None, **options):
        self.state.write_text(json.dumps({"release": release, **options}))

    def assets(self, version="0.1.0"):
        names = [f"cressida-{version}.tar.xz"]
        names += [f"cressida-{version}-{arch}.{kind}"
                  for arch in ("x86_64", "aarch64") for kind in ("AppImage", "flatpak")]
        for name in names:
            (self.destination / name).write_bytes(("fixture: " + name).encode())
        return names

    def release(self, draft=True):
        return {"databaseId": 31, "tagName": "v0.1.0", "isDraft": draft, "isPrerelease": False,
                "url": "https://example.test/releases/tag/v0.1.0", "assets": []}

    def commands(self):
        return [json.loads(line)[1] for line in self.record.read_text().splitlines()]

    def execute(self, tag="v0.1.0"):
        return publish(self.root, "refs/tags/" + tag, "example/cressida")

    def test_new_release_publishes_only_after_exact_assets_are_verified(self):
        result = self.execute()
        self.assertIn("Published complete release:", result)
        self.assertEqual(self.commands(), ["view", "create", "view", "upload", "view", "edit", "view"])
        release = json.loads(self.state.read_text())["release"]
        self.assertFalse(release["isDraft"])
        self.assertEqual({asset["name"] for asset in release["assets"]}, set(self.names) | {"SHA256SUMS"})
        for asset in release["assets"]:
            content = (self.destination / asset["name"]).read_bytes()
            self.assertEqual(asset["digest"], "sha256:" + hashlib.sha256(content).hexdigest())

    def test_existing_draft_can_finish_without_replacing_release_identity(self):
        self.write_state(self.release())
        self.execute()
        self.assertEqual(self.commands(), ["view", "upload", "view", "edit", "view"])
        self.assertEqual(json.loads(self.state.read_text())["release"]["databaseId"], 31)

    def test_published_rerun_does_not_upload_or_republish(self):
        release = self.release(draft=False)
        self.write_state(release)
        self.assertIn("left unchanged", self.execute())
        self.assertEqual(self.commands(), ["view"])
        self.assertEqual(json.loads(self.state.read_text())["release"], release)

    def test_upload_failure_cannot_publish(self):
        self.write_state(self.release(), upload_error=True)
        with self.assertRaisesRegex(ValueError, "Upload failed"):
            self.execute()
        self.assertEqual(self.commands(), ["view", "upload"])
        self.assertTrue(json.loads(self.state.read_text())["release"]["isDraft"])

    def test_incomplete_or_changed_remote_assets_remain_private(self):
        for condition in ("bad_digest", "pending_asset", "bad_size", "missing_digest", "extra_asset", "changed_identity"):
            with self.subTest(condition=condition):
                self.record.unlink(missing_ok=True)
                self.write_state(self.release(), **{condition: True})
                with self.assertRaises(ValueError):
                    self.execute()
                self.assertEqual(self.commands(), ["view", "upload", "view"])
                self.assertTrue(json.loads(self.state.read_text())["release"]["isDraft"])

    def test_missing_empty_or_symlink_assets_cannot_create_a_draft(self):
        asset = self.destination / self.names[0]
        for condition in ("missing", "empty", "symlink"):
            with self.subTest(condition=condition):
                self.record.unlink(missing_ok=True)
                self.write_state()
                asset.unlink(missing_ok=True)
                if condition == "empty":
                    asset.touch()
                elif condition == "symlink":
                    asset.symlink_to(self.destination / self.names[1])
                with self.assertRaises(ValueError):
                    self.execute()
                self.assertEqual(self.commands(), ["view"])

    def test_symlink_checksum_cannot_write_outside_asset_directory(self):
        other = self.root / "other"
        other.write_text("untouched")
        (self.destination / "SHA256SUMS").symlink_to(other)
        with self.assertRaisesRegex(ValueError, "regular file"):
            self.execute()
        self.assertEqual(other.read_text(), "untouched")
        self.assertEqual(self.commands(), ["view"])

    def test_lookup_failure_does_not_attempt_release_creation(self):
        self.write_state(lookup_error=True)
        with self.assertRaisesRegex(ValueError, "API unavailable"):
            self.execute()
        self.assertEqual(self.commands(), ["view"])

    def test_prerelease_tag_is_marked_during_private_staging(self):
        (self.root / "VERSION").write_text("v0.2.0-rc.1\n")
        self.assets("0.2.0-rc.1")
        self.execute("v0.2.0-rc.1")
        self.assertTrue(json.loads(self.state.read_text())["release"]["isPrerelease"])

    def test_success_requires_a_visible_release_after_publication(self):
        self.write_state(self.release(), publish_noop=True)
        with self.assertRaisesRegex(ValueError, "not become public"):
            self.execute()
        self.assertEqual(self.commands(), ["view", "upload", "view", "edit", "view"])


if __name__ == "__main__":
    unittest.main()
