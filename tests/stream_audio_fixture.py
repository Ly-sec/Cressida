#!/usr/bin/env python3
"""Feed synthetic stereo applications to an isolated native audio service."""

import math
import os
import select
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time


def main():
    binary, server = map(lambda value: str(Path(value).resolve()), sys.argv[1:])
    player = str(Path(server).with_name("pacat"))
    with tempfile.TemporaryDirectory(prefix="cressida-stream-audio-") as temporary:
        root = Path(temporary)
        endpoint = root / "native"
        config = root / "server.pa"
        config.write_text(
            f"load-module module-native-protocol-unix socket={endpoint} auth-anonymous=1\n"
            "load-module module-null-sink sink_name=cressida_stream_test rate=48000 channels=2\n"
            "set-default-sink cressida_stream_test\n", encoding="utf-8")
        environment = os.environ | {
            "PULSE_SERVER": f"unix:{endpoint}", "PULSE_COOKIE": str(root / "cookie"),
            "PULSE_RUNTIME_PATH": str(root), "PULSE_STATE_PATH": str(root / "state"),
            "XDG_RUNTIME_DIR": str(root), "XDG_CONFIG_HOME": str(root / "config"),
            "CRESSIDA_VOICE_TEST_PRIVATE": "1", "DBUS_SESSION_BUS_ADDRESS": "unix:path=/cressida-stream-test-no-bus"}
        stopped = threading.Event()
        applications = []
        producers = []
        probe = None
        with (root / "server.log").open("w+") as log:
            process = subprocess.Popen([server, "--daemonize=no", "--use-pid-file=no", "--disallow-exit=yes",
                                        "--exit-idle-time=-1", "--high-priority=no", "--realtime=no", "--log-target=stderr",
                                        "--file=" + str(config), "-n"], env=environment, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while not endpoint.exists():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        log.seek(0)
                        raise RuntimeError("The private audio server failed: " + log.read())
                    time.sleep(.01)
                for identity, name, left, right in (("fixture.browser", "Browser", 400, 800),
                                                   ("fixture.music", "Music", 200, 600),
                                                   ("dev.lysec.Cressida", "Excluded self playback", 1000, 1000)):
                    app = subprocess.Popen([player, "--playback", "--raw", "--format=float32le", "--rate=48000", "--channels=2",
                                            "--latency-msec=40", "--property=application.id=" + identity,
                                            "--property=application.name=" + name], env=environment,
                                           stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=log)
                    applications.append(app)
                    frame = b"".join(struct.pack("<ff", .1 * math.sin(2 * math.pi * left * i / 48000),
                                                .1 * math.sin(2 * math.pi * right * i / 48000)) for i in range(960))

                    def produce(app=app, frame=frame):
                        try:
                            while not stopped.is_set():
                                app.stdin.write(frame)
                                app.stdin.flush()
                        except (BrokenPipeError, OSError):
                            pass
                    producer = threading.Thread(target=produce, daemon=True)
                    producer.start()
                    producers.append(producer)
                probe = subprocess.Popen([player, "--record", "--raw", "--format=float32le", "--rate=48000", "--channels=2", "--latency-msec=40", "--device=cressida_stream_test.monitor"], env=environment, stdout=subprocess.PIPE, stderr=log)
                probe_bytes = bytearray()
                deadline = time.monotonic() + 5
                while len(probe_bytes) < 7680 * 5:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not select.select([probe.stdout], [], [], remaining)[0]:
                        raise RuntimeError("The private synthetic playback did not become ready")
                    data = os.read(probe.stdout.fileno(), 7680 * 5 - len(probe_bytes))
                    if not data:
                        raise RuntimeError("The private synthetic playback stopped")
                    probe_bytes.extend(data)
                probe.terminate()
                probe.wait(timeout=3)
                if max(map(abs, struct.unpack("<" + "f" * (len(probe_bytes) // 4), probe_bytes))) < .01:
                    raise RuntimeError("The private synthetic applications produced only silence")
                result = subprocess.run([binary], env=environment, timeout=25, check=False).returncode
                if result:
                    log.flush()
                    log.seek(0)
                    print(log.read(), file=sys.stderr)
                return result
            finally:
                if probe is not None and probe.poll() is None:
                    probe.terminate()
                    probe.wait(timeout=3)
                stopped.set()
                for app in applications:
                    app.terminate()
                for app in applications:
                    app.wait(timeout=3)
                for producer in producers:
                    producer.join(timeout=2)
                for app in applications:
                    try:
                        app.stdin.close()
                    except BrokenPipeError:
                        pass
                process.terminate()
                process.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
