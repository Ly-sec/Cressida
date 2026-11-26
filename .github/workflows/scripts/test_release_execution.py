"""Exercise release shell helpers with private SDK and compositor command peers."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPTS = Path(__file__).resolve().parent


class ReleaseExecutionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cressida-release-execution-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.commands = self.root / "commands"
        self.commands.mkdir()
        self.record = self.root / "record"
        self.environment = os.environ.copy()
        self.environment.update(
            PATH=str(self.commands) + os.pathsep + self.environment["PATH"],
            CRESSIDA_TEST_RECORD=str(self.record),
        )

    def executable(self, path, source, *, isolate_libraries=False):
        path.parent.mkdir(parents=True, exist_ok=True)
        if isolate_libraries:
            peer = path.with_name(path.name + ".py")
            peer.write_text(source)
            # Observe the SDK search path without loading its libraries into host Python.
            path.write_text(
                '#!/bin/sh\n'
                'export CRESSIDA_TEST_TOOL_LIBRARY_PATH="${LD_LIBRARY_PATH-}"\n'
                'unset LD_LIBRARY_PATH\n'
                f'exec {shlex.quote(sys.executable)} {shlex.quote(str(peer))} "$@"\n'
            )
        else:
            path.write_text(f"#!{sys.executable}\n" + source)
        path.chmod(0o700)

    def smoke(self, fail=False, compositor_delay=0, compositor_failure=False):
        self.executable(self.commands / "weston", """
import os, signal, socket, sys, time
from pathlib import Path
if os.environ.get('CRESSIDA_TEST_FAIL_WESTON'):
    print('synthetic compositor startup failure', file=sys.stderr)
    exit(19)
time.sleep(float(os.environ.get('CRESSIDA_TEST_WESTON_DELAY', '0')))
with socket.socket(socket.AF_UNIX) as listener:
    listener.bind(str(Path(os.environ['XDG_RUNTIME_DIR']) / os.environ['WAYLAND_DISPLAY']))
    listener.listen()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    while True:
        connection, _ = listener.accept()
        connection.close()
""")
        self.executable(self.commands / "flatpak", """
import os, socket, sys
from pathlib import Path
if sys.argv[1] == 'install':
    exit(0)
assert sys.argv[1] == 'run'
assert not any(arg.startswith('--env=XDG_RUNTIME_DIR=') for arg in sys.argv)
with socket.socket(socket.AF_UNIX) as peer:
    peer.connect(str(Path(os.environ['XDG_RUNTIME_DIR']) / os.environ['WAYLAND_DISPLAY']))
if os.environ.get('CRESSIDA_TEST_FAIL_FLATPAK'):
    print('synthetic package launch failure', file=sys.stderr)
    exit(17)
with open(os.environ['CRESSIDA_TEST_RECORD'], 'a') as record:
    view = next(arg.split('=', 2)[2] for arg in sys.argv if arg.startswith('--env=CRESSIDA_DEMO_VIEW='))
    record.write('flatpak:' + view + '\\n')
""")
        app = """
import os
from pathlib import Path
assert (Path(os.environ['XDG_RUNTIME_DIR']) / os.environ['WAYLAND_DISPLAY']).is_socket()
with open(os.environ['CRESSIDA_TEST_RECORD'], 'a') as record:
    record.write('appimage:' + os.environ['CRESSIDA_DEMO_VIEW'] + '\\n')
"""
        image = self.root / "image"
        self.executable(image, f"""
from pathlib import Path
app = Path('squashfs-root/AppRun')
app.parent.mkdir()
app.write_text({(f'#!{sys.executable}' + chr(10) + app)!r})
app.chmod(0o700)
""")
        if fail:
            self.environment["CRESSIDA_TEST_FAIL_FLATPAK"] = "1"
        self.environment["CRESSIDA_TEST_WESTON_DELAY"] = str(compositor_delay)
        if compositor_failure:
            self.environment["CRESSIDA_TEST_FAIL_WESTON"] = "1"
        return subprocess.run(
            ["bash", str(SCRIPTS / "smoke-release-packages.sh"), str(image),
             str(self.root / "bundle"), str(self.root / "logs")],
            env=self.environment, text=True, capture_output=True, timeout=20,
        )

    def test_flatpak_launcher_receives_the_private_compositor_socket(self):
        result = self.smoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        views = (
            "login-themed", "login-themed-light", "autocomplete-cycle", "sharing-cycle",
            "rpc-cycle", "media-gallery-cycle", "video-cycle",
        )
        self.assertEqual(
            self.record.read_text().splitlines(),
            [f"{package}:{view}" for view in views for package in ("appimage", "flatpak")],
        )

    def test_compositor_can_take_longer_than_five_seconds_to_start(self):
        result = self.smoke(compositor_delay=6)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(self.record.read_text().splitlines()), 14)

    def test_compositor_startup_failure_prints_its_log(self):
        result = self.smoke(compositor_failure=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("synthetic compositor startup failure", result.stderr)
        self.assertFalse(self.record.exists())

    def test_bootstrap_uses_retained_verified_assets_for_both_architectures(self):
        self.executable(self.commands / "curl", """
import os, sys
from pathlib import Path
url = next(arg for arg in sys.argv if arg.startswith('https://'))
assert '/download/continuous/' not in url
assert url.endswith('-' + os.environ['CRESSIDA_TEST_ARCH'] + '.AppImage')
assert '--fail' in sys.argv and '--location' in sys.argv
output = Path(sys.argv[sys.argv.index('--output') + 1])
program = 'from pathlib import Path\\n'
program += 'root = Path("squashfs-root/usr/bin")\\nroot.mkdir(parents=True)\\n'
program += 'for name in ("mksquashfs", "runtime-' + os.environ['CRESSIDA_TEST_ARCH'] + '"):\\n'
program += ' p = root / name\\n p.touch(mode=0o700)\\n'
output.write_text('#!' + sys.executable + '\\n' + program)
""")
        self.executable(self.commands / "sha256sum", """
import os, sys
from pathlib import Path
line = sys.stdin.read().strip()
checksum, filename = line.split('  ', 1)
assert len(checksum) == 64 and all(c in '0123456789abcdef' for c in checksum)
assert Path(filename).is_file()
with open(os.environ['CRESSIDA_TEST_RECORD'], 'a') as output:
 output.write(checksum + '\\n')
""")
        for arch in ("x86_64", "aarch64"):
            with self.subTest(arch=arch):
                work = self.root / arch
                work.mkdir()
                self.environment["CRESSIDA_TEST_ARCH"] = arch
                result = subprocess.run(
                    ["bash", str(SCRIPTS / "prepare-release-appimage-tool.sh"), str(work), arch],
                    env=self.environment, text=True, capture_output=True, timeout=20,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue((work / f"tool/squashfs-root/usr/bin/runtime-{arch}").is_file())
        checksums = self.record.read_text().splitlines()
        self.assertEqual(len(set(checksums)), 2)

    def test_corrupt_bootstrap_is_rejected_before_extraction(self):
        self.executable(self.commands / "curl", """
import sys
from pathlib import Path
Path(sys.argv[sys.argv.index('--output') + 1]).write_bytes(b'corrupt download')
""")
        result = subprocess.run(
            ["bash", str(SCRIPTS / "prepare-release-appimage-tool.sh"), str(self.root), "x86_64"],
            env=self.environment, text=True, capture_output=True, timeout=20,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "tool").exists())

    def test_unavailable_bootstrap_stops_before_flatpak_builds(self):
        scripts = self.root / "checkout/.github/workflows/scripts"
        scripts.mkdir(parents=True)
        for name in ("build-release-packages.sh", "prepare-release-appimage-tool.sh"):
            shutil.copyfile(SCRIPTS / name, scripts / name)
        self.executable(self.commands / "uname", "print('x86_64')\n")
        self.executable(self.commands / "curl", "import sys\nsys.exit(22)\n")
        self.executable(self.commands / "flatpak", f"from pathlib import Path\nPath({str(self.record)!r}).touch()\n")
        self.environment.update(RELEASE_REF="refs/tags/v0.3.0", RELEASE_ARCH="x86_64", RUNNER_TEMP=str(self.root))
        result = subprocess.run(
            ["bash", str(scripts / "build-release-packages.sh")],
            env=self.environment, text=True, capture_output=True, timeout=20,
        )
        self.assertEqual(result.returncode, 22, result.stdout + result.stderr)
        self.assertFalse(self.record.exists())

    def test_failed_package_checks_print_the_underlying_log(self):
        result = self.smoke(fail=True)
        self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
        self.assertIn("synthetic package launch failure", result.stderr)
        self.assertIn("flatpak-login-themed.log", result.stderr)

    def deployment(self, arch, codec_major="62"):
        self.environment["CRESSIDA_TEST_CP"] = shutil.which("cp")
        self.executable(self.commands / "cp", """
import os, subprocess, sys
from pathlib import Path
sdk = Path(os.environ['CRESSIDA_TEST_SDK_ROOT'])
arguments = sys.argv[1:]
source = arguments[-2]
if source.startswith(('/app/', '/usr/')):
    arguments[-2] = str(sdk) + source
else:
    assert Path(source).is_relative_to(sdk), 'unexpected SDK copy source: ' + source
subprocess.run([os.environ['CRESSIDA_TEST_CP'], *arguments], check=True)
""")
        self.executable(self.commands / "install", "")
        self.executable(self.commands / "patchelf", "print('/lib/ld-release-test.so')\n")
        self.executable(self.commands / "pkg-config", """
import os, sys
assert sys.argv[-1] == 'libavcodec'
print(os.environ['CRESSIDA_TEST_SDK_LIBDIR'] if sys.argv[1] == '--variable=libdir' else '62.28.103')
""")
        self.environment["LD_LIBRARY_PATH"] = "/cressida-test-host-libs"
        triplet = arch + "-linux-gnu"
        work = self.root / arch
        sdk = work / "sdk"
        sdk_libdir = sdk / "usr/lib" / triplet
        for relative, content in {
            "app/bin/cressida": "synthetic client",
            "usr/share/fonts/noto-emoji/NotoColorEmoji.ttf": "synthetic font",
            "usr/share/mime/packages/cressida-test.xml": "synthetic MIME database",
            "usr/share/X11/locale/en_US.UTF-8/Compose": "synthetic Compose table",
            "usr/share/licenses/sdk-license": "synthetic SDK notice",
            "usr/manifest.json": "{}",
            f"usr/lib/{triplet}/GL/default/lib/dri/driver.so": "synthetic graphics driver",
        }.items():
            fixture = sdk / relative
            fixture.parent.mkdir(parents=True, exist_ok=True)
            fixture.write_text(content)
        codecs = sdk_libdir / "codecs-extra"
        (codecs / "lib").mkdir(parents=True)
        if codec_major:
            (codecs / "lib" / f"libavcodec.so.{codec_major}").touch()
        (codecs / "share/licenses").mkdir(parents=True)
        (codecs / "share/licenses/codec-license").write_text("synthetic codec notice")
        self.environment.update(
            CRESSIDA_TEST_TRIPLET=triplet, CRESSIDA_TEST_SDK_LIBDIR=str(sdk_libdir),
            CRESSIDA_TEST_SDK_ROOT=str(sdk),
        )
        tool = work / "tool/squashfs-root/usr/bin/appimagetool"
        self.executable(tool, """
import os, sys
from pathlib import Path
assert 'LD_LIBRARY_PATH' not in os.environ
if sys.argv[1] == '-s':
    triplet = os.environ['CRESSIDA_TEST_TRIPLET']
    codecs = os.environ['CRESSIDA_TEST_SDK_LIBDIR'] + '/codecs-extra/lib'
    assert os.environ['CRESSIDA_TEST_TOOL_LIBRARY_PATH'].split(':') == ['/app/lib', codecs, '/usr/lib/' + triplet, '/lib/' + triplet]
    appdir = Path(sys.argv[-1]).parents[3]
    loader = appdir / 'lib/ld-release-test.so'
    loader.parent.mkdir()
    loader.touch(mode=0o700)
    (appdir / 'AppRun').write_text('#!/bin/sh\\nLD_LINUX=/lib/ld-release-test.so\\n')
else:
    assert os.environ['CRESSIDA_TEST_TOOL_LIBRARY_PATH'] == '/cressida-test-host-libs'
    Path('Cressida-' + os.environ['VERSION'] + '-' + os.environ['ARCH'] + '.AppImage').write_bytes(b'fixture')
""", isolate_libraries=True)
        return subprocess.run(
            ["bash", str(SCRIPTS / "build-release-appimage.sh"), str(work), "0.1.0", arch],
            env=self.environment, text=True, capture_output=True, timeout=20,
        )

    def test_deployment_searches_the_selected_sdk_triplet_without_leaking_it(self):
        for arch in ("x86_64", "aarch64"):
            with self.subTest(arch=arch):
                result = self.deployment(arch)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                work = self.root / arch
                self.assertTrue((work / f"cressida-0.1.0-{arch}.AppImage").is_file())
                notice = work / "AppDir/usr/share/codec-licenses/codec-license"
                self.assertEqual(notice.read_text(), "synthetic codec notice")
                resources = {
                    "bin/cressida": "synthetic client",
                    "share/fonts/noto-emoji/NotoColorEmoji.ttf": "synthetic font",
                    "share/mime/packages/cressida-test.xml": "synthetic MIME database",
                    "share/X11/locale/en_US.UTF-8/Compose": "synthetic Compose table",
                    "share/sdk-licenses/sdk-license": "synthetic SDK notice",
                    "share/freedesktop-sdk-manifest.json": "{}",
                    f"lib/{arch}-linux-gnu/GL/default/lib/dri/driver.so": "synthetic graphics driver",
                }
                for relative, content in resources.items():
                    self.assertEqual((work / "AppDir/usr" / relative).read_text(), content)

    def test_missing_or_incompatible_codecs_stop_image_creation(self):
        for arch, codec_major in (("x86_64", None), ("aarch64", "61")):
            with self.subTest(arch=arch, codec_major=codec_major):
                result = self.deployment(arch, codec_major)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("SDK codec extension is missing or incompatible", result.stderr)
                self.assertFalse((self.root / arch / f"cressida-0.1.0-{arch}.AppImage").exists())


if __name__ == "__main__":
    unittest.main()
