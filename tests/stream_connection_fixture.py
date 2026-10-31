#!/usr/bin/env python3
"""Exercise encrypted stream publication and watching through private relay sockets."""

import base64
import hashlib
import json
import select
import socket
import struct
import subprocess
import sys
import time

from voice_connection_fixture import exact, expected, receive, send


class Relay:
    def __init__(self, process, peer, udp):
        self.process, self.peer, self.udp = process, peer, udp
        self.signals, self.packets = [], []
        self.closed = False
        self.last_sequence = -1

    def pump(self, timeout=0.01):
        readable, _, _ = select.select([self.udp, self.process.stdout] + ([] if self.closed else [self.peer]), [], [], timeout)
        if self.peer in readable:
            try:
                value = receive(self.peer)
                if isinstance(value, dict) and value["op"] == 3:
                    self.last_sequence = value["d"].get("seq_ack", -1)
                    send(self.peer, {"op": 6, "d": {"t": value["d"]["t"]}})
                else:
                    self.signals.append(value)
            except EOFError:
                self.closed = True
        if self.udp in readable:
            data, _ = self.udp.recvfrom(8192)
            self.packets.append(data)
        return self.process.stdout in readable

    def record(self):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if self.pump():
                line = self.process.stdout.readline()
                assert line, self.process.stderr.read()
                return json.loads(line)
        raise AssertionError("The native stream peer did not complete its bounded stage")

    def reply(self, **value):
        self.process.stdin.write(json.dumps(value) + "\n")
        self.process.stdin.flush()

    def wait_signal(self, predicate):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            for index, signal in enumerate(self.signals):
                if predicate(signal):
                    return self.signals.pop(index)
            self.pump()
        raise AssertionError("The relay did not receive the expected stream control")

    def drain_for(self, duration):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            self.pump(min(0.01, deadline - time.monotonic()))

    def acknowledged(self, sequence):
        send(self.peer, {"op": 11, "seq": sequence, "d": {"user_ids": ["4"]}})
        deadline = time.monotonic() + 4
        while self.last_sequence != sequence and time.monotonic() < deadline:
            self.pump()
        assert self.last_sequence == sequence, "The peer acknowledges the preceding control changes"

    def media(self, payload_type, audio):
        def complete():
            return (any(len(p) > 12 and p[1] & 0x80 and p[1] & 0x7f == payload_type for p in self.packets)
                    and (not audio or any(len(p) > 12 and p[1] & 0x7f == 120 for p in self.packets)))
        deadline = time.monotonic() + 4
        while not complete() and time.monotonic() < deadline:
            self.pump()
        assert complete(), "The publisher must deliver an entire frame and selected audio"
        packets, self.packets = self.packets, []
        return [packet.hex() for packet in packets]


def scenario(binary, direction, codec):
    with socket.socket() as listener, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(6)
        udp.bind(("127.0.0.1", 0))
        udp.settimeout(4)
        process = subprocess.Popen([binary, f"ws://127.0.0.1:{listener.getsockname()[1]}/", direction, codec],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            peer, _ = listener.accept()
            with peer:
                peer.settimeout(5)
                request = bytearray()
                while not request.endswith(b"\r\n\r\n"):
                    request.extend(exact(peer, 1))
                    assert len(request) < 8192
                headers = dict(line.split(": ", 1) for line in request.decode().split("\r\n")[1:] if ": " in line)
                key = next(value for name, value in headers.items() if name.lower() == "sec-websocket-key")
                accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
                peer.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
                send(peer, {"op": 8, "d": {"heartbeat_interval": 200}})
                identify = expected(peer, 0)["d"]
                publishing = direction == "publish"
                assert identify["channel_id"] == "3" and identify["video"]
                assert identify["streams"] == [{"type": "screen", "rid": "100", "quality": 100}]
                relay = Relay(process, peer, udp)
                external = relay.record()
                send(peer, struct.pack("!HB", 1, 25) + bytes.fromhex(external["external"]), binary=True)
                send(peer, {"op": 11, "seq": 2, "d": {"user_ids": ["4"]}})
                send(peer, {"op": 2, "d": {"ssrc": 123, "ip": "127.0.0.1", "port": udp.getsockname()[1],
                                              "modes": ["aead_xchacha20_poly1305_rtpsize"],
                                              "streams": [{"ssrc": 456, "rtx_ssrc": 457}]}})
                discovery, address = udp.recvfrom(8192)
                assert len(discovery) == 74
                udp.sendto(struct.pack("!HHI", 2, 70, 123) + b"127.0.0.1\0".ljust(64, b"\0") + struct.pack("!H", address[1]), address)
                selected = expected(peer, 1)["d"]
                assert selected["protocol"] == "udp" and selected["experiments"] == []
                assert len(selected["rtc_connection_id"]) == 36
                negotiated = next(c for c in selected["codecs"] if c["name"] == codec)
                assert negotiated["encode"] and negotiated["decode"]
                pt, rtx = negotiated["payload_type"], negotiated["rtx_payload_type"]
                send(peer, {"op": 4, "d": {"mode": "aead_xchacha20_poly1305_rtpsize", "secret_key": list(range(32)),
                                              "dave_protocol_version": 1, "video_codec": codec}})
                package = expected(peer, 26)
                relay.drain_for(0.05)
                assert not relay.packets, "No media is transmitted before the encrypted group is ready"
                relay.reply(package=package[1:].hex(), pt=pt, rtx=rtx)
                welcome = relay.record()
                send(peer, struct.pack("!HB", 3, 30) + bytes.fromhex(welcome["welcome"]), binary=True)
                if publishing:
                    assert relay.record()["emitted"]
                    speaking = relay.wait_signal(lambda s: isinstance(s, dict) and s["op"] == 5 and s["d"]["speaking"] > 0)
                    assert speaking["d"] == {"speaking": 2, "delay": 0, "ssrc": 123}, "Shared audio uses the context-audio flag"
                    relay.reply(packets=relay.media(pt, True))
                    assert relay.record()["constrain"]
                    send(peer, {"op": 15, "d": {"456": 100, "pixelCounts": {"456": 160 * 90}}})
                    relay.wait_signal(lambda s: isinstance(s, dict) and s["op"] == 12 and s["d"]["streams"][0]["max_resolution"]["height"] == 90)
                    relay.reply(packets=relay.media(pt, False))
                    assert relay.record()["release"]
                    send(peer, {"op": 15, "d": {"456": 100, "pixelCounts": {"456": 0}}})
                    relay.wait_signal(lambda s: isinstance(s, dict) and s["op"] == 12 and s["d"]["streams"][0]["max_resolution"]["height"] == 180)
                    relay.reply(packets=relay.media(pt, False))
                    assert relay.record()["no_sink"]
                    send(peer, {"op": 15, "d": {"456": 0, "pixelCounts": {"456": 0}}})
                    relay.acknowledged(8)
                    relay.packets.clear()
                    relay.reply(suspended=True)
                    assert relay.record()["suspended_source"]
                    relay.drain_for(0.12)
                    assert not any(len(p) > 12 and p[1] & 0x7f in {120, pt, rtx} for p in relay.packets), "A zero sink suppresses media without ending the publication"
                    relay.reply(quiet=True)
                    assert relay.record()["restore_sink"]
                    send(peer, {"op": 15, "d": {"any": 100, "pixelCounts": {}}})
                    relay.acknowledged(9)
                    relay.reply(packets=relay.media(pt, False))
                    assert relay.record()["pause"]
                    relay.wait_signal(lambda s: isinstance(s, dict) and s["op"] == 12 and not s["d"]["streams"][0]["active"])
                    relay.drain_for(0.05)
                    relay.packets.clear()
                    relay.reply(paused=True)
                    assert relay.record()["paused_source"]
                    relay.drain_for(0.15)
                    assert not any(len(p) > 12 and p[1] & 0x7f in {120, pt, rtx} for p in relay.packets), "Pause prevents outgoing application audio and screen frames"
                    relay.reply(quiet=True)
                    assert relay.record()["resume"]
                    relay.wait_signal(lambda s: isinstance(s, dict) and s["op"] == 12 and s["d"]["streams"][0]["active"])
                    relay.reply(resumed=True)
                    assert relay.record()["emitted"]
                    relay.reply(packets=relay.media(pt, False))
                else:
                    send(peer, {"op": 12, "seq": 4, "d": {"user_id": "4", "audio_ssrc": 126,
                        "streams": [{"type": "screen", "rid": "100", "ssrc": 124, "rtx_ssrc": 125, "active": True}]}})
                    media = relay.record()
                    subscription = relay.wait_signal(lambda s: isinstance(s, dict) and s["op"] == 15 and s["d"].get("124", 0) > 0)
                    assert subscription["d"] == {"124": 100, "pixelCounts": {"124": 720 * 720 * 16 // 9}}, "Watching requests its own dimensions with the protocol pixel-count field"
                    send(peer, {"op": 15, "d": {"124": 0, "pixelCounts": {"124": 4}}})
                    for index, packet in enumerate(media["video"]):
                        if index != 1:
                            udp.sendto(bytes.fromhex(packet), address)
                    relay.drain_for(0.12)
                    assert not any(len(p) > 12 and p[1] & 0x7f == 120 for p in relay.packets), "Watching must never transmit injected microphone capture"
                    feedback, relay.packets = relay.packets, []
                    relay.reply(feedback=[p.hex() for p in feedback if 200 <= p[1] <= 206])
                    repair = relay.record()
                    for packet in repair["repair"]:
                        udp.sendto(bytes.fromhex(packet), address)
                    udp.sendto(bytes.fromhex(media["audio"]), address)
                    relay.reply(delivered=True)
                    deaf = relay.record()
                    for packet in deaf["deaf_video"]:
                        udp.sendto(bytes.fromhex(packet), address)
                    udp.sendto(bytes.fromhex(deaf["audio"]), address)
                    relay.reply(delivered=True)
                assert relay.record()["passed"]
                output, error = process.communicate(timeout=3)
                assert process.returncode == 0, (output, error)
                assert "synthetic-token" not in error and "synthetic-session" not in error
        finally:
            if process.poll() is None:
                process.kill()
                output, error = process.communicate()
                if error:
                    print(error, file=sys.stderr)


if __name__ == "__main__":
    for selected_codec in ("VP8", "H264", "VP9"):
        for selected_direction in ("publish", "watch"):
            scenario(sys.argv[1], selected_direction, selected_codec)
    print("Private encrypted stream publication, playback, RTX, pause and deafen passed")
