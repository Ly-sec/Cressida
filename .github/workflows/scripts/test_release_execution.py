"""Exercise release shell helpers with private SDK and compositor command peers."""

import os
from pathlib import Path
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

    def executable(self, path, source):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"#!{sys.executable}\n" + source)
        path.chmod(0o700)

    def smoke(self, fail=False):
        self.executable(self.commands / "weston", """
import os, signal, socket, sys
from pathlib import Path
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
    record.write('flatpak\\n')
""")
        app = """
import os
from pathlib import Path
assert (Path(os.environ['XDG_RUNTIME_DIR']) / os.environ['WAYLAND_DISPLAY']).is_socket()
with open(os.environ['CRESSIDA_TEST_RECORD'], 'a') as record:
    record.write('appimage\\n')
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
        return subprocess.run(
            ["bash", str(SCRIPTS / "smoke-release-packages.sh"), str(image),
             str(self.root / "bundle"), str(self.root / "logs")],
            env=self.environment, text=True, capture_output=True, timeout=20,
        )

    def test_flatpak_launcher_receives_the_private_compositor_socket(self):
        result = self.smoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.record.read_text().splitlines(), ["appimage", "flatpak"] * 6)

    def test_failed_package_checks_print_the_underlying_log(self):
        result = self.smoke(fail=True)
        self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
        self.assertIn("synthetic package launch failure", result.stderr)
        self.assertIn("flatpak-login-themed.log", result.stderr)

    def test_deployment_searches_the_selected_sdk_triplet_without_leaking_it(self):
        self.executable(self.commands / "cp", """
import sys
from pathlib import Path
if '/app/.' in sys.argv:
    binary = Path(sys.argv[-1]) / 'bin/cressida'
    binary.parent.mkdir(parents=True)
    binary.touch()
""")
        self.executable(self.commands / "install", "")
        self.executable(self.commands / "patchelf", "print('/lib/ld-release-test.so')\n")
        self.environment["LD_LIBRARY_PATH"] = "/cressida-test-host-libs"
        for arch, triplet in (("x86_64", "x86_64-linux-gnu"), ("aarch64", "aarch64-linux-gnu")):
            with self.subTest(arch=arch):
                work = self.root / arch
                self.environment["CRESSIDA_TEST_TRIPLET"] = triplet
                tool = work / "tool/squashfs-root/usr/bin/appimagetool"
                self.executable(tool, """
import os, sys
from pathlib import Path
if sys.argv[1] == '-s':
    triplet = os.environ['CRESSIDA_TEST_TRIPLET']
    assert os.environ['LD_LIBRARY_PATH'].split(':') == ['/app/lib', '/usr/lib/' + triplet, '/lib/' + triplet]
    appdir = Path(sys.argv[-1]).parents[3]
    loader = appdir / 'lib/ld-release-test.so'
    loader.parent.mkdir()
    loader.touch(mode=0o700)
    (appdir / 'AppRun').write_text('#!/bin/sh\\nLD_LINUX=/lib/ld-release-test.so\\n')
else:
    assert os.environ['LD_LIBRARY_PATH'] == '/cressida-test-host-libs'
    Path('Cressida-' + os.environ['VERSION'] + '-' + os.environ['ARCH'] + '.AppImage').write_bytes(b'fixture')
""")
                result = subprocess.run(
                    ["bash", str(SCRIPTS / "build-release-appimage.sh"), str(work), "0.1.0", arch],
                    env=self.environment, text=True, capture_output=True, timeout=20,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue((work / f"cressida-0.1.0-{arch}.AppImage").is_file())


if __name__ == "__main__":
    unittest.main()
