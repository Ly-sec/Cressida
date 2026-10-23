"""Stage a real installation and exercise it without build-tree library overrides."""

import json
import os
from contextlib import contextmanager
from pathlib import Path
import subprocess
import sys
import tempfile


def run_checked(*arguments, **kwargs):
    result = subprocess.run(
        arguments, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs,
    )
    if result.returncode:
        raise RuntimeError(f"Command failed: {arguments}\n{result.stdout}")
    return result.stdout


@contextmanager
def preserve_install_manifest(build):
    # A test install must not replace the manifest of a real user installation.
    manifest = build / "meson-logs/install-log.txt"
    previous = manifest.read_bytes() if manifest.exists() else None
    try:
        yield
    finally:
        if previous is None:
            manifest.unlink(missing_ok=True)
        else:
            manifest.write_bytes(previous)


def check_installation(build, source, meson, readelf):
    options = {
        option["name"]: option["value"]
        for option in json.loads((build / "meson-info/intro-buildoptions.json").read_text())
    }
    prefix = Path(options["prefix"])
    with preserve_install_manifest(build), tempfile.TemporaryDirectory(prefix="cressida-install-check-") as temporary:
        stage = Path(temporary)

        def installed(option, *parts):
            destination = prefix / options[option]
            return stage / destination.relative_to(destination.anchor) / Path(*parts)

        run_checked(meson, "install", "-C", str(build), "--no-rebuild", "--destdir", str(stage))
        binary = installed("bindir", "cressida")
        renderer = installed("libdir", "libcressida-render.so")
        assert binary.is_file() and os.access(binary, os.X_OK), "Installed executable is missing"
        assert renderer.is_file(), "Installed renderer is missing"
        assert installed("libdir", "libcressida-log.so").is_file(), "Installed logger is missing"

        assets = installed("datadir", "cressida", "assets")
        for path in (source / "assets").rglob("*"):
            if path.is_file():
                target = assets / path.relative_to(source / "assets")
                assert target.is_file() and target.read_bytes() == path.read_bytes(), f"Missing/changed asset: {path}"
        headers = installed("includedir", "cressida")
        for path in (source / "src").rglob("*.h"):
            assert (headers / path.relative_to(source / "src")).is_file(), f"Missing header: {path}"
        assert not list(headers.rglob("*.cpp")), "Implementation sources leaked into installed headers"
        for filename in ("LICENSE", "LICENSE-APACHE", "LICENSE-MIT"):
            assert installed("datadir", "licenses", "cressida", filename).is_file(), f"Missing notice: {filename}"
        desktop = installed("datadir", "applications", "dev.lysec.Cressida.desktop")
        assert "Exec=cressida\n" in desktop.read_text()
        assert "Icon=dev.lysec.Cressida\n" in desktop.read_text()
        assert installed("datadir", "icons", "hicolor", "scalable", "apps", "dev.lysec.Cressida.svg").is_file()

        dynamic = run_checked(readelf, "-d", str(binary))
        assert "$ORIGIN/" in dynamic, "Installed executable has no relative renderer search path"
        assert str(build) not in dynamic, "Installed executable retains a build-tree search path"
        for library in (renderer, installed("libdir", "libcressida-log.so")):
            library_dynamic = run_checked(readelf, "-d", str(library))
            assert str(build) not in library_dynamic, "Installed library retains build paths"
        assert "$ORIGIN" in run_checked(readelf, "-d", str(renderer)), "Renderer cannot locate its sibling logger"
        environment = os.environ.copy()
        environment["XDG_CACHE_HOME"] = str(stage / "private-cache")
        environment["HOME"] = str(stage / "private-home")
        for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "CRESSIDA_ASSETS_DIR", "DISCORD_CPP_ASSETS_DIR"):
            environment.pop(key, None)
        for option, expected in (("--version", "cressida "), ("--help", "Usage: cressida")):
            output = run_checked(str(binary), option, cwd=stage, env=environment)
            assert expected in output, f"Installed {option} failed"
        assert not (stage / "private-cache").exists(), "Help/version unexpectedly wrote logs"
        assert not (stage / "private-home").exists(), "Help/version unexpectedly wrote home data"
        print("Installed executable, renderer lookup, complete assets, headers, launcher, icon and notices passed")


if __name__ == "__main__":
    check_installation(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), sys.argv[3], sys.argv[4])
