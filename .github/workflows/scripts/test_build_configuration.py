"""Exercise the release recipe with real slim-LTO static archives."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


TOOLS = ("just", "meson", "ninja", "gcc", "g++", "gcc-ar")
ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(all(shutil.which(tool) for tool in TOOLS), "GCC LTO build tools are unavailable")
class ReleaseConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cressida-release-check-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build-check-release"
        self.environment = os.environ.copy()
        self.environment.update(CC=shutil.which("gcc"), CXX=shutil.which("g++"), AR="ar")
        (self.root / "meson.build").write_text(
            "project('release-archive-check', 'cpp', default_options: ['cpp_std=c++23'])\n"
            "archive = static_library('archive-probe', 'probe.cpp', cpp_args: ['-fno-fat-lto-objects'])\n"
            "executable('cressida', 'main.cpp', link_with: archive, install: true)\n"
        )
        (self.root / "probe.cpp").write_text('extern "C" int archive_answer() { return 42; }\n')
        (self.root / "main.cpp").write_text(
            'extern "C" int archive_answer();\nint main() { return archive_answer() == 42 ? 0 : 1; }\n'
        )

    def run_command(self, *arguments):
        result = subprocess.run(
            arguments, cwd=self.root, env=self.environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def build_release(self):
        return self.run_command(
            "just", "--justfile", str(ROOT / "justfile"), "--working-directory", str(self.root),
            "--set", "build-prefix", "build-check", "build", "release",
        )

    def test_generic_ar_selects_a_working_release_archiver(self):
        self.build_release()
        self.run_command(str(self.build / "cressida"))

    def test_reconfigure_repairs_archives_without_recompiling_objects(self):
        self.run_command(
            "meson", "setup", str(self.build), "--buildtype=release",
            "-Db_lto=true", "-Db_sanitize=none", "-Dcpp_std=c++23",
        )
        self.run_command("ninja", "-C", str(self.build), "libarchive-probe.a")
        compiled = self.build / "libarchive-probe.a.p/probe.cpp.o"
        timestamp = compiled.stat().st_mtime_ns
        self.build_release()
        self.run_command(str(self.build / "cressida"))
        self.assertEqual(compiled.stat().st_mtime_ns, timestamp)

    def test_custom_archiver_is_preserved(self):
        archiver = self.root / "custom archiver"
        trace = self.root / "archiver.log"
        archiver.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$@" >> "$CRESSIDA_TEST_AR_LOG"\n'
            'exec "$CRESSIDA_TEST_AR" "$@"\n'
        )
        archiver.chmod(0o700)
        self.environment.update(
            AR=str(archiver), CRESSIDA_TEST_AR=shutil.which("gcc-ar"), CRESSIDA_TEST_AR_LOG=str(trace),
        )
        self.build_release()
        self.run_command(str(self.build / "cressida"))
        self.assertIn("libarchive-probe.a", trace.read_text())

    def install(self, *arguments):
        return self.run_command(
            "just", "--justfile", str(ROOT / "justfile"), "--working-directory", str(self.root),
            "--set", "build-prefix", "build-check", "install", *arguments,
        )

    def test_install_defaults_to_release_and_stages_without_building_tests(self):
        with (self.root / "meson.build").open("a") as output:
            output.write("executable('test-only', 'broken.cpp')\n")
        (self.root / "broken.cpp").write_text("#error Installation must not build test-only targets\n")
        stage = self.root / "staging with spaces"
        self.environment["DESTDIR"] = str(stage)
        self.install()
        self.assertTrue((self.build / "cressida").is_file())
        self.assertTrue((stage / "usr/local/bin/cressida").is_file())
        self.assertFalse((self.root / "build-check-debug").exists())
        self.assertFalse((self.build / "test-only").exists())

    def test_explicit_release_test_enables_and_runs_a_disabled_suite(self):
        (self.root / "justfile").write_bytes((ROOT / "justfile").read_bytes())
        (self.root / "meson.options").write_text(
            "option('tests', type: 'feature', value: 'disabled')\n"
        )
        with (self.root / "meson.build").open("a") as output:
            output.write(
                "if get_option('tests').enabled()\n"
                "  probe = executable('explicit-check', 'main.cpp', link_with: archive)\n"
                "  test('explicit-check', probe)\n"
                "endif\n"
            )
        output = self.run_command(
            "just", "--justfile", str(ROOT / "justfile"), "--working-directory", str(self.root),
            "--set", "build-prefix", "build-check", "test", "release", "explicit-check",
        )
        self.assertTrue((self.build / "explicit-check").is_file())
        self.assertIn("1/1", output)
        self.assertIn("explicit-check", output)

    def test_install_accepts_explicit_prefix_with_spaces(self):
        prefix = self.root / "user prefix"
        self.install("release", str(prefix))
        self.run_command(str(prefix / "bin/cressida"))

    def test_install_preserves_an_existing_custom_prefix(self):
        prefix = self.root / "configured prefix"
        self.run_command(
            "just", "--justfile", str(ROOT / "justfile"), "--working-directory", str(self.root),
            "--set", "build-prefix", "build-check", "configure", "release", str(prefix),
        )
        self.install()
        self.run_command(str(prefix / "bin/cressida"))

    def test_uninstall_uses_the_existing_manifest_without_reconfiguring(self):
        prefix = self.root / "uninstall prefix"
        self.install("release", str(prefix))
        configured = self.build / "meson-private/coredata.dat"
        timestamp = configured.stat().st_mtime_ns
        with (self.root / "meson.build").open("a") as output:
            output.write("error('Uninstall must not reconfigure changed sources')\n")
        self.run_command(
            "just", "--justfile", str(ROOT / "justfile"), "--working-directory", str(self.root),
            "--set", "build-prefix", "build-check", "uninstall",
        )
        self.assertFalse((prefix / "bin/cressida").exists())
        self.assertEqual(configured.stat().st_mtime_ns, timestamp)


if __name__ == "__main__":
    unittest.main()
