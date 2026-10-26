#!/usr/bin/env python3
"""Run native voice audio against private null devices, without desktop audio."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Expected the native test binary and Pulse server executable")
    binary = str(Path(sys.argv[1]).resolve())
    server = str(Path(sys.argv[2]).resolve())
    with tempfile.TemporaryDirectory(prefix="cressida-voice-audio-") as temporary:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        endpoint = runtime / "native"
        config = root / "server.pa"
        config.write_text(
            f"load-module module-native-protocol-unix socket={endpoint} auth-anonymous=1\n"
            "load-module module-null-source source_name=cressida_test_input rate=48000 channels=1\n"
            "load-module module-null-sink sink_name=cressida_test_output rate=48000 channels=1\n"
            "load-module module-null-source source_name=cressida_fallback_input rate=48000 channels=1\n"
            "load-module module-null-sink sink_name=cressida_fallback_output rate=48000 channels=1\n"
            "load-module module-rescue-streams\n"
            "set-default-source cressida_fallback_input\n"
            "set-default-sink cressida_fallback_output\n",
            encoding="utf-8",
        )
        environment = os.environ.copy()
        environment.update(
            {
                "XDG_RUNTIME_DIR": str(runtime),
                "XDG_CONFIG_HOME": str(root / "config"),
                "PULSE_COOKIE": str(root / "cookie"),
                "PULSE_RUNTIME_PATH": str(runtime),
                "PULSE_STATE_PATH": str(root / "state"),
                "PULSE_SERVER": f"unix:{endpoint}",
                "CRESSIDA_VOICE_TEST_PRIVATE": "1",
                "DBUS_SESSION_BUS_ADDRESS": "unix:path=/cressida-voice-test-no-session-bus",
            }
        )
        with (root / "server.log").open("w+") as log:
            process = subprocess.Popen(
                [
                    server,
                    "--daemonize=no",
                    "--use-pid-file=no",
                    "--disallow-exit=yes",
                    "--exit-idle-time=-1",
                    "--high-priority=no",
                    "--realtime=no",
                    "--log-target=stderr",
                    "--file=" + str(config),
                    "-n",
                ],
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                deadline = time.monotonic() + 5
                while not endpoint.exists():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        log.seek(0)
                        raise RuntimeError("Private audio server failed: " + log.read())
                    time.sleep(0.01)
                return subprocess.run([binary], env=environment, timeout=30, check=False).returncode
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
