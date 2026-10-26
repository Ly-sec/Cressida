"""Exercise the portable dependency driver with offline build-tool fixtures."""

import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
DRIVER = ROOT / "scripts/build-voice-dependencies.sh"


class VoiceDependencyBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cressida-voice-build-check-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.scratch = self.root / "scratch"
        self.scratch.mkdir()
        self.prefix = self.root / "prefix with spaces"
        self.trace = self.root / "commands.jsonl"
        self.environment = os.environ.copy()
        self.environment.update(
            PATH=str(self.tools) + os.pathsep + os.environ["PATH"],
            TMPDIR=str(self.scratch),
            CRESSIDA_DEPENDENCY_FIXTURES=str(self.root),
            CRESSIDA_DEPENDENCY_TRACE=str(self.trace),
            CRESSIDA_VOICE_BUILD_JOBS="2",
        )
        contents = DRIVER.read_text()
        self.pins = {
            name: {
                field: re.search(rf"^{name}_{field}=([a-f0-9]+)$", contents, re.MULTILINE).group(1)
                for field in ("revision", "sha256")
            }
            for name in ("mls", "dave")
        }
        for name in ("mlspp", "libdave"):
            files = (
                {"CMakeLists.txt": "set(CMAKE_CXX_STANDARD 17)\n",
                 "lib/CMakeLists.txt": "add_subdirectory(bytes)\nadd_subdirectory(mls_ds)\nadd_subdirectory(mls_vectors)\n",
                 "LICENSE": "Synthetic fixture license\n"}
                if name == "mlspp" else
                {"cpp/CMakeLists.txt": "set(CMAKE_CXX_STANDARD 17)\n"}
            )
            with tarfile.open(self.root / f"{name}.tar.gz", "w:gz") as archive:
                for path, text in files.items():
                    info = tarfile.TarInfo(f"fixture/{path}")
                    encoded = text.encode()
                    info.size = len(encoded)
                    archive.addfile(info, io.BytesIO(encoded))
        (self.root / "pins.json").write_text(json.dumps(self.pins))
        fixture = self.tools / "tool-fixture"
        fixture.write_text(
            f"#!{sys.executable}\n" + r'''
import json
import os
from pathlib import Path
import shutil
import sys

name = Path(sys.argv[0]).name
arguments = sys.argv[1:]
root = Path(os.environ["CRESSIDA_DEPENDENCY_FIXTURES"])
record = {"tool": name, "args": arguments}
if name == "sha256sum":
    record["input"] = sys.stdin.read()
with Path(os.environ["CRESSIDA_DEPENDENCY_TRACE"]).open("a") as output:
    output.write(json.dumps(record) + "\n")
if name == "curl":
    url = next(argument for argument in arguments if argument.startswith("https://"))
    library = "mlspp" if "/cisco/mlspp/" in url else "libdave"
    shutil.copyfile(root / (library + ".tar.gz"), arguments[arguments.index("--output") + 1])
elif name == "sha256sum":
    checksum, filename = record["input"].strip().split("  ", 1)
    library = Path(filename).name.removesuffix(".tar.gz")
    pin = json.loads((root / "pins.json").read_text())["mls" if library == "mlspp" else "dave"]
    if checksum != pin["sha256"] or os.environ.get("CRESSIDA_FAIL_CHECKSUM") == library:
        raise SystemExit("Synthetic checksum verification failure")
elif name == "cmake":
    if "-S" in arguments:
        source = Path(arguments[arguments.index("-S") + 1])
        build = Path(arguments[arguments.index("-B") + 1])
        if "set(CMAKE_CXX_STANDARD 23)" not in (source / "CMakeLists.txt").read_text():
            raise SystemExit("The dependency must build with C++23")
        if source.name == "mlspp":
            text = (source / "lib/CMakeLists.txt").read_text()
            if "mls_ds" in text or "mls_vectors" in text:
                raise SystemExit("Unused MLS targets must be excluded")
        build.mkdir()
        prefix = next(value.partition("=")[2] for value in arguments if value.startswith("-DCMAKE_INSTALL_PREFIX="))
        (build / "fixture-prefix").write_text(prefix)
    if os.environ.get("CRESSIDA_FAIL_BUILD") and "--build" in arguments:
        raise SystemExit("Synthetic compiler failure")
    if "--install" in arguments:
        build = Path(arguments[arguments.index("--install") + 1])
        prefix = Path((build / "fixture-prefix").read_text())
        (prefix / "lib").mkdir(parents=True, exist_ok=True)
'''
        )
        fixture.chmod(0o700)
        for tool in ("curl", "sha256sum", "cmake"):
            (self.tools / tool).symlink_to(fixture.name)

    def run_driver(self, *arguments, success=True):
        result = subprocess.run(
            ["sh", str(DRIVER), *map(str, arguments)], cwd=ROOT, env=self.environment,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
        )
        self.assertEqual(result.returncode == 0, success, result.stdout)
        self.assertFalse(any(self.scratch.iterdir()), "Temporary sources must be cleaned after success or failure")
        return result

    def commands(self, tool=None):
        rows = [json.loads(line) for line in self.trace.read_text().splitlines()] if self.trace.exists() else []
        return [row for row in rows if tool is None or row["tool"] == tool]

    def test_both_dependencies_build_with_native_flags_and_a_custom_prefix(self):
        self.run_driver(self.prefix)
        cmake = self.commands("cmake")
        self.assertEqual(len(cmake), 6)
        first, second = cmake[0]["args"], cmake[3]["args"]
        for arguments in (first, second):
            self.assertIn(f"-DCMAKE_INSTALL_PREFIX={self.prefix}", arguments)
            self.assertIn("-DCMAKE_INSTALL_LIBDIR=lib", arguments)
            self.assertIn("-DBUILD_SHARED_LIBS=ON", arguments)
            self.assertIn("-DTESTING=OFF", arguments)
        self.assertIn("-DMLS_CXX_NAMESPACE=mlspp", first)
        self.assertIn("-DDISABLE_GREASE=ON", first)
        self.assertIn("-DPERSISTENT_KEYS=OFF", second)
        self.assertIn(f"-DCMAKE_PREFIX_PATH={self.prefix}", second)
        for row in (cmake[1], cmake[4]):
            self.assertEqual(row["args"][-2:], ["--parallel", "2"])
        for library in ("mlspp", "libdave"):
            metadata = (self.prefix / "lib/pkgconfig" / f"{library}.pc").read_text()
            self.assertIn(f"prefix={self.prefix}\n", metadata)
            self.assertIn("${prefix}/lib", metadata)
            self.assertIn("-Wl,-rpath,\"${libdir}\"", metadata)
        self.assertEqual((self.prefix / "share/licenses/cressida-mlspp/LICENSE").read_text(), "Synthetic fixture license\n")

    def test_checksum_failures_stop_before_any_dependency_build(self):
        self.environment["CRESSIDA_FAIL_CHECKSUM"] = "libdave"
        self.run_driver(self.prefix, success=False)
        self.assertEqual(len(self.commands("sha256sum")), 2)
        self.assertFalse(self.commands("cmake"))
        self.assertFalse(self.prefix.exists())

    def test_compiler_failure_stops_install_and_the_following_library(self):
        self.environment["CRESSIDA_FAIL_BUILD"] = "1"
        self.run_driver(self.prefix, success=False)
        self.assertEqual(len(self.commands("cmake")), 2)
        self.assertFalse(self.prefix.exists())

    def test_invalid_parameters_fail_before_network_or_installation(self):
        self.run_driver("relative-prefix", success=False)
        self.run_driver(self.prefix, "extra", success=False)
        for value in ("0", "-1", "two"):
            self.environment["CRESSIDA_VOICE_BUILD_JOBS"] = value
            self.run_driver(self.prefix, success=False)
        self.assertFalse(self.commands())

    def test_source_revisions_match_nix_and_musl_installs_dependencies_first(self):
        nix = (ROOT / "nix/libdave.nix").read_text()
        for pin in self.pins.values():
            self.assertIn("/tar.gz/" + pin["revision"] + '"', nix)
            self.assertEqual(len(pin["sha256"]), 64)
        musl = (ROOT / ".github/workflows/scripts/build-musl.sh").read_text()
        for package in ("cmake", "openssl-dev", "opus-dev", "libsodium-dev", "pulseaudio-dev"):
            self.assertIn(package, musl)
        self.assertLess(musl.index("sh scripts/build-voice-dependencies.sh"), musl.index("meson setup"))


if __name__ == "__main__":
    unittest.main()
