#!/usr/bin/env python3
"""Check actual activity presence packets using a private loopback Gateway."""

import socket
import subprocess
import sys
import time

from voice_gateway_fixture import receive, send, upgrade


def presence(peer, predicate):
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        value = receive(peer)
        if value["op"] == 1:
            continue
        assert value["op"] == 3, "Only activity presence updates are expected"
        if predicate(value["d"]):
            return value["d"]
    raise AssertionError("Expected activity presence did not arrive")


def main():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(2)
        listener.settimeout(8)
        endpoint = f"ws://127.0.0.1:{listener.getsockname()[1]}/?v=9&encoding=json"
        process = subprocess.Popen([sys.argv[1], endpoint], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            with upgrade(listener, "synthetic-rpc-token") as peer:
                send(peer, 0, {"session_id": "private-rpc-session"}, "READY", 1)
                value = presence(peer, lambda data: len(data["activities"]) == 2)
                assert value["status"] == "dnd"
                custom, activity = value["activities"]
                assert custom["type"] == 4 and custom["state"] == "Keep this status"
                assert activity["application_id"] == "44" and activity["type"] == 2
                assert activity["timestamps"] == {"start": 1793534400000, "end": 1793534640000}
                assert activity["buttons"] == ["Website", "Source"]
                assert all(isinstance(label, str) for label in activity["buttons"])
                assert activity["metadata"] == {"button_urls": ["https://example.com/music", "https://example.com/source"]}
                assert activity["assets"] == {"large_image": "123"}
                stale = dict(activity)
                stale["details"] = "An older track"
                send(peer, 0, {"activities": [stale]}, "STALE_PRESENCE", 2)
                corrected = presence(peer, lambda data: len(data["activities"]) == 2)
                assert corrected["activities"][1]["details"] == "An offline track"
                send(
                    peer,
                    0,
                    {"activities": corrected["activities"]},
                    "CURRENT_PRESENCE",
                    3,
                )
                send(peer, 0, {}, "RPC_CLEAR", 4)
                value = presence(peer, lambda data: len(data["activities"]) == 1)
                assert value["activities"] == [custom] and value["status"] == "dnd"
                send(peer, 0, {}, "RPC_RESET", 5)
            with upgrade(listener, "synthetic-rpc-next-token") as peer:
                send(peer, 0, {"session_id": "private-rpc-next-session"}, "READY", 1)
                value = presence(peer, lambda data: data["status"] == "idle")
                assert len(value["activities"]) == 1 and value["activities"][0]["type"] == 4
                assert value["activities"][0]["state"] == "A second status"
                send(peer, 0, {}, "RPC_COMPLETE", 2)
                output, error = process.communicate(timeout=4)
                assert process.returncode == 0, output + error
                for private in ("synthetic-rpc-token", "synthetic-rpc-next-token", "private-rpc-session", "127.0.0.1"):
                    assert private not in error, "Diagnostics exclude credentials and endpoints"
                print(output.strip())
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    main()
