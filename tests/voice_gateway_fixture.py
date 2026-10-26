#!/usr/bin/env python3
"""Validate main Gateway voice frames over private loopback WebSockets."""

import base64
import hashlib
import json
import select
import socket
import struct
import subprocess
import sys
import time


def exact(peer, size):
    result = bytearray()
    while len(result) < size:
        part = peer.recv(size - len(result))
        if not part:
            raise EOFError("The synthetic Gateway peer closed")
        result.extend(part)
    return bytes(result)


def send(peer, opcode, data, kind=None, sequence=None):
    payload = json.dumps({"op": opcode, "d": data, "t": kind, "s": sequence}).encode()
    header = b"\x81"
    if len(payload) < 126:
        header += bytes([len(payload)])
    else:
        header += b"\x7e" + struct.pack("!H", len(payload))
    peer.sendall(header + payload)


def receive(peer):
    first, second = exact(peer, 2)
    size = second & 127
    if size == 126:
        size = struct.unpack("!H", exact(peer, 2))[0]
    elif size == 127:
        size = struct.unpack("!Q", exact(peer, 8))[0]
    assert second & 128 and size <= 65536, "Client frames are masked and bounded"
    mask = exact(peer, 4)
    payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(exact(peer, size)))
    assert first == 129, "Gateway commands use complete text frames"
    value = json.loads(payload)
    assert type(value.get("op")) is int
    if value["op"] == 1:
        send(peer, 11, None)
    else:
        assert isinstance(value.get("d"), dict)
    return value


def upgrade(listener, expected_token):
    peer, _ = listener.accept()
    peer.settimeout(5)
    request = bytearray()
    while not request.endswith(b"\r\n\r\n"):
        request.extend(exact(peer, 1))
        assert len(request) <= 8192
    lines = request.decode().split("\r\n")
    assert lines[0] == "GET /?v=9&encoding=json HTTP/1.1"
    headers = dict(line.split(": ", 1) for line in lines[1:] if ": " in line)
    key = next(value for name, value in headers.items() if name.lower() == "sec-websocket-key")
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    peer.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
    send(peer, 10, {"heartbeat_interval": 1000})
    while True:
        value = receive(peer)
        if value["op"] == 1:
            continue
        assert value["op"] == 2, "A new account generation identifies instead of resuming the old session"
        assert value["d"]["token"] == expected_token
        break
    return peer


def next_voice(peer):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        value = receive(peer)
        if value["op"] in (1, 3):
            continue
        assert value["op"] == 4, "Only the explicit voice action is expected"
        assert set(value["d"]) == {"guild_id", "channel_id", "self_mute", "self_deaf", "self_video"}
        assert all(type(value["d"][field]) is bool for field in ("self_mute", "self_deaf", "self_video"))
        assert value["d"]["self_video"] is False
        return value["d"], time.monotonic()
    raise AssertionError("The expected voice frame did not arrive")


def expected_voice(peer, guild, channel, muted, deafened):
    value, received_at = next_voice(peer)
    assert value == {"guild_id": guild, "channel_id": channel, "self_mute": muted,
                     "self_deaf": deafened, "self_video": False}, value
    return received_at


def quiet(peer, duration):
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        ready, _, _ = select.select([peer], [], [], max(0, deadline - time.monotonic()))
        if not ready:
            break
        value = receive(peer)
        assert value["op"] in (1, 3), "Reset must discard the previous generation's queued voice target"


def closed(peer):
    while True:
        try:
            value = receive(peer)
        except (EOFError, ConnectionResetError):
            return
        assert value["op"] in (1, 3), "No queued voice target may escape after session cancellation"


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Expected the synthetic Gateway test binary")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(2)
        listener.settimeout(8)
        endpoint = f"ws://127.0.0.1:{listener.getsockname()[1]}/?v=9&encoding=json"
        process = subprocess.Popen([sys.argv[1], endpoint], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            with upgrade(listener, "synthetic-voice-gateway-token") as peer:
                send(peer, 0, {"session_id": "synthetic-voice-first-session"}, "READY", 1)
                joined = expected_voice(peer, "1", "10", True, False)
                send(peer, 0, {}, "VOICE_BURST", 2)
                switched = expected_voice(peer, "2", "20", False, True)
                assert switched - joined >= 0.20, "Rapid voice changes must honor the 250ms send cadence"
                send(peer, 0, {}, "VOICE_LEAVE", 3)
                left = expected_voice(peer, "2", None, False, False)
                assert left - switched >= 0.20, "Leaving voice shares the bounded command cadence"
                send(peer, 0, {}, "VOICE_DM", 4)
                direct = expected_voice(peer, None, "30", True, True)
                assert direct - left >= 0.20, "DM transitions share the voice send cadence"
                send(peer, 0, {}, "VOICE_RESET", 5)
                closed(peer)
            with upgrade(listener, "synthetic-voice-gateway-next-token") as peer:
                send(peer, 0, {"session_id": "synthetic-voice-next-session"}, "READY", 1)
                quiet(peer, 0.40)
                send(peer, 0, {}, "VOICE_SECOND_JOIN", 2)
                expected_voice(peer, "3", "40", False, False)
                send(peer, 0, {}, "VOICE_FINAL_LEAVE", 3)
                expected_voice(peer, "3", None, False, False)
                send(peer, 0, {}, "VOICE_COMPLETE", 4)
                closed(peer)
            output, error = process.communicate(timeout=3)
            assert process.returncode == 0, output + error
            assert "passed" in output
            for private in ("synthetic-voice-gateway-token", "synthetic-voice-gateway-next-token",
                            "synthetic-voice-first-session", "synthetic-voice-next-session", "127.0.0.1"):
                assert private not in error, "Gateway diagnostics keep credentials and endpoints private"
            print(output.strip())
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    main()
