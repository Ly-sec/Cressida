#!/usr/bin/env python3
"""Run capture against an isolated bus and synthetic PipeWire video source."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


with tempfile.TemporaryDirectory(prefix="cressida-screen-test-") as directory:
    root = Path(directory)
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    config = root / "pipewire.conf"
    config.write_text("""
context.properties = { core.daemon = true core.name = cressida-screen-test }
context.spa-libs = {
    support.* = support/libspa-support
    videotestsrc = videotestsrc/libspa-videotestsrc
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-access args = { access.force = unrestricted } }
]
context.objects = [
    { factory = spa-node-factory args = {
        factory.name = videotestsrc node.name = synthetic-screen
        node.description = SyntheticScreen node.driver = true
        node.param.Props = { patternType = 1 }
    } }
]
""")
    environment = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), PIPEWIRE_RUNTIME_DIR=str(runtime),
                       PIPEWIRE_REMOTE="cressida-screen-test", XDG_CONFIG_HOME=str(root / "config"),
                       XDG_STATE_HOME=str(root / "state"), XDG_DATA_HOME=str(root / "data"))
    with (root / "pipewire.log").open("w+") as log:
        daemon = subprocess.Popen([sys.argv[2], "-c", str(config)], env=environment, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 5
            while not (runtime / "cressida-screen-test").exists():
                if daemon.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Private video server did not start")
                time.sleep(0.01)
            result = subprocess.run([sys.argv[1]], env=environment, capture_output=True, text=True, timeout=30)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode:
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise SystemExit(result.returncode)
        finally:
            daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
