"""Check portability selection without repository commands or API requests."""

import unittest

from dave_changes import COMPARISON_FILE_LIMIT, comparison_range, relevant, select


BASE = "1" * 40
HEAD = "2" * 40
PUSH = {"ref": "refs/heads/main", "before": BASE, "after": HEAD}


class DaveChangesTests(unittest.TestCase):
    def decision(self, paths, event_name="push", event=None):
        def fetch(base, head):
            self.assertEqual((base, head), (BASE, HEAD))
            return {"status": "ahead", "files": [{"filename": path} for path in paths]}
        return select(event_name, PUSH if event is None else event, fetch)[0]

    def test_ui_docs_and_unrelated_workflows_skip(self):
        self.assertFalse(self.decision([
            "src/app/autocomplete.cpp", "src/ui/style.h", "docs/user/chat.md",
            "docs/sidebar.json", ".github/workflows/docs.yml", "tests/emoji_catalog_test.cpp",
        ]))

    def test_crypto_and_build_inputs_run(self):
        for path in (
            "src/voice/dave/frame.cpp", "src/voice/dave/session.h", "src/voice/encryption.cpp",
            "src/voice/encryption.h", "src/voice/media.h", "third_party/mlspp/lib/hpke/src/aead_cipher.cpp",
            "tests/voice_dave_test.cpp", "tests/voice_encryption_test.cpp",
            "tests/voice_video_crypto_test.cpp", "tests/voice_dave/meson.build",
            "meson.build", "meson.options", "meson_options.txt", "nix/package.nix",
            "flake.nix", "flake.lock", "justfile", ".github/workflows/ci.yml",
            ".github/workflows/ci-dave.yml", ".github/workflows/release.yml",
            ".github/workflows/scripts/dave_changes.py", ".github/workflows/scripts/test_dave_changes.py",
            ".github/workflows/scripts/check_mls_snapshot.py", ".github/workflows/scripts/test_mls_snapshot.py",
        ):
            with self.subTest(path=path):
                self.assertTrue(self.decision([path]))

    def test_directory_matches_require_path_boundaries(self):
        self.assertFalse(relevant("src/voice/daveish.cpp"))
        self.assertFalse(relevant("docs/nix/notes.md"))

    def test_pr_and_merge_queue_compare_full_ranges(self):
        for name, event in (
            ("pull_request", {"pull_request": {"base": {"sha": BASE}, "head": {"sha": HEAD}}}),
            ("merge_group", {"merge_group": {"base_sha": BASE, "head_sha": HEAD}}),
        ):
            with self.subTest(event=name):
                self.assertFalse(self.decision(["docs/user/chat.md"], name, event))
                self.assertTrue(self.decision(["src/voice/dave/frame.cpp"], name, event))

    def test_manual_release_and_unknown_ranges_always_run(self):
        def unexpected(*_):
            self.fail("A forced full run must not request a comparison")
        for name, event in (
            ("workflow_dispatch", {}), ("workflow_call", {}), ("unknown", {}),
            ("push", {**PUSH, "ref": "refs/tags/v1.0.0"}),
            ("push", {**PUSH, "before": "0" * 40}),
            ("push", {**PUSH, "after": "invalid/ref"}),
        ):
            with self.subTest(event=name, payload=event):
                self.assertTrue(select(name, event, unexpected)[0])

    def test_pr_with_new_base_commits_uses_its_review_diff(self):
        event = {"pull_request": {"base": {"sha": BASE}, "head": {"sha": HEAD}}}
        self.assertFalse(select("pull_request", event, lambda *_: {
            "status": "diverged", "files": [{"filename": "docs/user/chat.md"}],
        })[0])

    def test_renames_check_old_and_new_paths_and_deletions(self):
        for item in (
            {"filename": "archive/retired.cpp", "previous_filename": "src/voice/dave/frame.cpp", "status": "renamed"},
            {"filename": "src/voice/dave/new.cpp", "previous_filename": "src/app/old.cpp", "status": "renamed"},
            {"filename": "src/voice/dave/frame.cpp", "status": "removed"},
        ):
            self.assertTrue(select("push", PUSH, lambda *_: {"status": "ahead", "files": [item]})[0])

    def test_incomplete_large_and_nonlinear_comparisons_run(self):
        for comparison in (
            {}, {"status": "ahead"}, {"status": "diverged", "files": []},
            {"status": "behind", "files": []}, {"status": "ahead", "files": [{}]},
            {"status": "ahead", "files": [{"filename": "readme.md", "status": "renamed"}]},
            {"status": "ahead", "files": [{"filename": "docs/page.md"}] * COMPARISON_FILE_LIMIT},
        ):
            with self.subTest(comparison=comparison):
                self.assertTrue(select("push", PUSH, lambda *_: comparison)[0])

    def test_lookup_failures_run_and_empty_valid_comparison_skips(self):
        def failed(*_):
            raise OSError("Synthetic API failure")
        self.assertTrue(select("push", PUSH, failed)[0])
        self.assertTrue(select("pull_request", {}, failed)[0])
        self.assertFalse(select("push", PUSH, lambda *_: {"status": "identical", "files": []})[0])
        self.assertEqual(comparison_range("push", PUSH), (BASE, HEAD))
