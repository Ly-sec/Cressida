#!/usr/bin/env python3
"""Exercise voice signaling against private loopback sockets and fake audio."""

import base64
import hashlib
import json
import os
import re
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def exact(peer, count):
    data = bytearray()
    while len(data) < count:
        part = peer.recv(count - len(data))
        if not part:
            raise EOFError("The voice test peer closed")
        data.extend(part)
    return bytes(data)


def send(peer, value, binary=False):
    payload = value if binary else json.dumps(value).encode()
    header = bytes([0x82 if binary else 0x81])
    if len(payload) < 126:
        header += bytes([len(payload)])
    elif len(payload) <= 65535:
        header += bytes([126]) + struct.pack("!H", len(payload))
    else:
        header += bytes([127]) + struct.pack("!Q", len(payload))
    peer.sendall(header + payload)


def receive(peer):
    first, second = exact(peer, 2)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", exact(peer, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", exact(peer, 8))[0]
    assert length <= 1024 * 1024
    assert second & 0x80, "Client frames must be masked"
    mask = exact(peer, 4)
    payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(exact(peer, length)))
    if first & 0xF == 2:
        return payload
    assert first & 0xF == 1
    return json.loads(payload)


def expected(peer, opcode):
    while True:
        value = receive(peer)
        if isinstance(value, dict) and value["op"] == 3:
            assert isinstance(value["d"]["t"], int)
            assert "seq_ack" in value["d"]
            send(peer, {"op": 6, "d": {"t": value["d"]["t"]}})
            continue
        if isinstance(value, bytes):
            assert value and value[0] == opcode
        else:
            assert value["op"] == opcode
        return value


def close(peer, code=None, split=False):
    payload = struct.pack("!H", code) + b"private-close-reason" if code is not None else b""
    frame = bytes([0x88, len(payload)]) + payload
    if split:
        peer.sendall(frame[:3])
        time.sleep(0.05)
        peer.sendall(frame[3:])
    else:
        peer.sendall(frame)


def scenario(binary, mode):
    with tempfile.TemporaryDirectory(prefix="cressida-voice-logs-") as cache, socket.socket() as listener, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(8)
        udp.bind(("127.0.0.1", 0))
        udp.settimeout(4)
        origin = f"ws://127.0.0.1:{listener.getsockname()[1]}/"
        process = subprocess.Popen([binary, origin, mode], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                   env={**os.environ, "XDG_CACHE_HOME": cache})
        try:
            peer, _ = listener.accept()
            with peer:
                peer.settimeout(5)
                request = bytearray()
                while not request.endswith(b"\r\n\r\n"):
                    request.extend(exact(peer, 1))
                    assert len(request) <= 8192
                headers = request.decode().split("\r\n")
                assert headers[0] == "GET /?v=8 HTTP/1.1"
                values = dict(line.split(": ", 1) for line in headers[1:] if ": " in line)
                key = next(value for name, value in values.items() if name.lower() == "sec-websocket-key")
                accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
                peer.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
                send(peer, {"op": 8, "d": {"heartbeat_interval": 1000 if mode.startswith("media") else 300}})
                identify = expected(peer, 0)["d"]
                assert identify["server_id"] == "1" and identify["user_id"] == "2"
                assert identify["session_id"] == "synthetic-session" and identify["token"] == "synthetic-token"
                assert identify["max_dave_protocol_version"] >= 1
                media = mode.startswith("media")
                if media:
                    external = json.loads(process.stdout.readline())
                    # External senders and member identities can arrive before session description.
                    send(peer, struct.pack("!HB", 1, 25) + bytes.fromhex(external["external"]), binary=True)
                    if mode != "media-hints":
                        send(peer, {"op": 11, "seq": 2, "d": {"user_ids": ["4"]}})
                if mode.startswith("close-"):
                    code = None if mode == "close-empty" else 4009 if mode == "close-split" else int(mode.removeprefix("close-"))
                    close(peer, code, mode == "close-split")
                elif mode == "transport-eof":
                    peer.shutdown(socket.SHUT_RDWR)
                elif mode == "oversized":
                    try:
                        send(peer, bytes(1024 * 1024 + 1), binary=True)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                else:
                    send(peer, {"op": 2, "d": {"ssrc": 123, "ip": "127.0.0.1", "port": udp.getsockname()[1],
                                                  "modes": ["aead_xchacha20_poly1305_rtpsize"]}})
                    discovery, address = udp.recvfrom(8192)
                    assert len(discovery) == 74 and struct.unpack("!HHI", discovery[:8]) == (1, 70, 123)
                    response = struct.pack("!HHI", 2, 70, 123) + b"127.0.0.1\0".ljust(64, b"\0") + struct.pack("!H", address[1])
                    udp.sendto(response, address)
                    selected = expected(peer, 1)["d"]
                    assert selected == {"protocol": "udp", "data": {"address": "127.0.0.1", "port": address[1],
                                                                 "mode": "aead_xchacha20_poly1305_rtpsize"}}
                    send(peer, {"op": 4, "d": {"mode": "aead_xchacha20_poly1305_rtpsize", "secret_key": list(range(32)),
                                                  "dave_protocol_version": 0 if mode == "downgrade" else 1}})
                    if mode != "downgrade":
                        registered = expected(peer, 5)
                        assert registered["d"] == {"speaking": 0, "delay": 0, "ssrc": 123}, "The SSRC is registered before anyone speaks"
                        package = expected(peer, 26)
                        assert len(package) > 16, "A real MLS key package is sent"
                        speaking = expected(peer, 5)
                        assert speaking["d"] == {"speaking": 1, "delay": 0, "ssrc": 123}, "Actual local activity is announced while encrypted media remains gated"
                        udp.setblocking(False)
                        try:
                            udp.recvfrom(8192)
                            raise AssertionError("Pending microphone frames must not be transmitted")
                        except BlockingIOError:
                            pass
                        finally:
                            udp.settimeout(4)
                    if media:
                        process.stdin.write(json.dumps({"package": package[1:].hex()}) + "\n")
                        process.stdin.flush()
                        negotiated = json.loads(process.stdout.readline())
                        if mode == "media-creator":
                            send(peer, struct.pack("!HB", 3, 27) + bytes.fromhex(negotiated["proposal"]), binary=True)
                            commit = expected(peer, 28)
                            process.stdin.write(json.dumps({"commit": commit[1:].hex()}) + "\n")
                            process.stdin.flush()
                            negotiated = json.loads(process.stdout.readline())
                        if mode != "media-hints":
                            send(peer, {"op": 12, "seq": 3, "d": {"user_id": "4", "audio_ssrc": 124}})
                        update = "commit" if mode == "media-creator" else "welcome"
                        send(peer, struct.pack("!HB", 4, 29 if mode == "media-creator" else 30) + bytes.fromhex(negotiated[update]), binary=True)
                        if mode == "media-transition":
                            acknowledged = expected(peer, 23)
                            assert acknowledged["d"]["transition_id"] == 9
                            process.stdin.write(json.dumps({"before_execute": True}) + "\n")
                            process.stdin.flush()
                            assert json.loads(process.stdout.readline())["gated"]
                            udp.setblocking(False)
                            try:
                                udp.recvfrom(8192)
                                raise AssertionError("Prepared keys must not transmit before execute-transition")
                            except BlockingIOError:
                                pass
                            finally:
                                udp.settimeout(4)
                            send(peer, {"op": 22, "seq": 5, "d": {"transition_id": 9}})
                        if mode == "media-hints":
                            # Keys must be ready using main-session membership alone.
                            send(peer, {"op": 12, "seq": 6, "d": {"user_id": "4", "audio_ssrc": 124}})
                            while True:
                                heartbeat = receive(peer)
                                assert heartbeat["op"] == 3
                                send(peer, {"op": 6, "d": {"t": heartbeat["d"]["t"]}})
                                if heartbeat["d"]["seq_ack"] == 6:
                                    break
                        else:
                            acknowledged = 5 if mode == "media-transition" else 4
                            while True:
                                heartbeat = receive(peer)
                                assert heartbeat["op"] == 3
                                send(peer, {"op": 6, "d": {"t": heartbeat["d"]["t"]}})
                                if heartbeat["d"]["seq_ack"] == acknowledged:
                                    break
                        if mode == "media-activity":
                            tampered = bytearray.fromhex(negotiated["rtp"])
                            tampered[12] ^= 1
                            udp.sendto(tampered, address)
                            send(peer, {"op": 11, "seq": 7, "d": {"user_ids": ["4"]}})
                            while True:
                                heartbeat = receive(peer)
                                assert heartbeat["op"] == 3
                                send(peer, {"op": 6, "d": {"t": heartbeat["d"]["t"]}})
                                if heartbeat["d"]["seq_ack"] == 7:
                                    break
                            process.stdin.write(json.dumps({"untrusted": True}) + "\n")
                            process.stdin.flush()
                            assert json.loads(process.stdout.readline())["ignored"]
                        udp.sendto(bytes.fromhex(negotiated["rtp"]), address)
                        captured, captured_address = udp.recvfrom(8192)
                        assert captured_address == address and len(captured) > 32
                        process.stdin.write(json.dumps({"packet": captured.hex()}) + "\n")
                        process.stdin.flush()
                        if mode == "media-activity":
                            assert json.loads(process.stdout.readline())["voiced"]
                            udp.sendto(bytes.fromhex(negotiated["silence"]), address)
                        if mode == "media-keepalive":
                            assert json.loads(process.stdout.readline())["observe"]
                            deadline = time.monotonic() + 7
                            while True:
                                assert time.monotonic() < deadline, "Muted capture does not stop UDP keepalives"
                                readable, _, _ = select.select([peer, udp], [], [], 0.1)
                                if peer in readable:
                                    value = receive(peer)
                                    assert value["op"] in (3, 5)
                                    if value["op"] == 3:
                                        send(peer, {"op": 6, "d": {"t": value["d"]["t"]}})
                                if udp in readable:
                                    keepalive, source = udp.recvfrom(8192)
                                    assert source == address and len(keepalive) == 8
                                    udp.sendto(keepalive, address)
                                    udp.sendto(keepalive, address)
                                    udp.sendto((int.from_bytes(keepalive, "little") + 1).to_bytes(8, "little"), address)
                                    break
                            while True:
                                heartbeat = receive(peer)
                                if heartbeat["op"] == 3:
                                    send(peer, {"op": 6, "d": {"t": heartbeat["d"]["t"] + 1}})
                                    send(peer, {"op": 6, "d": {"t": heartbeat["d"]["t"]}})
                                    send(peer, {"op": 6, "d": {"t": heartbeat["d"]["t"]}})
                                    break
                                assert heartbeat["op"] == 5
                            close(peer, 4009)
                    if mode == "heartbeat":
                        heartbeat = receive(peer)
                        assert heartbeat["op"] == 3 and "seq_ack" in heartbeat["d"]
                output, error = process.communicate(timeout=8)
                assert process.returncode == 0, f"{mode}: {output}\n{error}"
                assert "passed" in output
                diagnostic = (Path(cache) / "cressida/logs/cressida.log").read_text()
                for secret in ("synthetic-token", "synthetic-session", "voice-test.discord.media", "private-close-reason"):
                    assert secret not in output and secret not in error and secret not in diagnostic, "Voice diagnostics omit private signaling and close reasons"
                failures = [line for line in diagnostic.splitlines() if "Connection failed " in line]
                if mode == "cancel" or (media and mode != "media-keepalive"):
                    assert not failures, "Explicit cancellation emits no failure summary"
                else:
                    assert len(failures) == 1, "Each failed connection emits one bounded diagnostic summary"
                    fields = dict(re.findall(r"([a-z][a-z0-9_]+)=([a-z0-9-]+)", failures[0]))
                    assert int(fields["connection"]) > 0 and int(fields["elapsed_ms"]) >= 0
                    assert fields["stream"] == fields["publishing"] == "false"
                    if mode.startswith("close-") or mode == "media-keepalive":
                        assert fields["kind"] == "peer-close" and fields["transport_code"] == "0"
                        assert fields["close_code"] == str(4009 if mode in ("close-split", "media-keepalive") else code or 0)
                    elif mode == "transport-eof":
                        assert fields["kind"] == "signaling-receive" and int(fields["transport_code"]) != 0
                        assert fields["close_code"] == "0"
                    elif mode == "heartbeat":
                        assert fields["kind"] == "heartbeat-timeout" and fields["heartbeat_pending"] == "true"
                        assert int(fields["heartbeat_sent"]) == int(fields["heartbeat_acks"]) + 1
                        assert int(fields["heartbeat_ack_age_ms"]) >= 0 if int(fields["heartbeat_acks"]) else fields["heartbeat_ack_age_ms"] == "-1"
                    if mode == "media-keepalive":
                        assert fields["muted"] == "true" and fields["udp_keepalive_sent"] == fields["udp_keepalive_acks"] == "1"
                        assert fields["udp_keepalive_send_failures"] == fields["udp_keepalive_errno"] == "0"
                        assert int(fields["udp_keepalive_ack_age_ms"]) >= 0
                        assert fields["transport_rejected"] == "0", "UDP keepalive replies are not rejected media"
                        assert fields["heartbeat_pending"] == "false"
                        assert fields["heartbeat_sent"] == fields["heartbeat_acks"], "Stale and repeated ACKs do not inflate counts"
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    for selected_mode in ("cancel", "downgrade", "heartbeat", "oversized", "media", "media-transition", "media-hints", "media-creator", "media-activity",
                          "close-4006", "close-4009", "close-4014", "close-4015", "close-empty", "close-split", "transport-eof", "media-keepalive"):
        scenario(sys.argv[1], selected_mode)
    print("Private voice WebSocket and UDP setup scenarios passed")
