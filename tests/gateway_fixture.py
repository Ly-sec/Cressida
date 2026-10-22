"""A synthetic loopback WebSocket peer. No account, browser, or external network."""
import base64
import hashlib
import json
import re
import socket
import struct
import subprocess
import sys
import threading
import time


def exact(connection, size):
    data = b""
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise EOFError("client closed WebSocket")
        data += chunk
    return data


def validate_send(packet):
    assert type(packet.get("op")) is int, "Gateway opcode must be an integer"
    opcode, data = packet["op"], packet.get("d")
    if opcode == 1:
        assert data is None or type(data) is int, "heartbeat carries a sequence number or null"
        return
    assert isinstance(data, dict), "outgoing Gateway event data must be an object"
    if opcode == 2:
        implemented = sum(1 << bit for bit in (2, 3, 4, 5, 9, 10))
        assert data["capabilities"] == implemented, "Identify must not opt into unimplemented event variants"
        assert data["client_state"] == {"guild_versions": {}}, "client-state capability needs its cache-state envelope"
        assert isinstance(data["token"], str) and data["compress"] is False
        assert all(isinstance(data["properties"][key], str) for key in ("os", "browser", "device"))
    if opcode in (2, 3):
        presence = data["presence"] if opcode == 2 else data
        assert isinstance(presence["activities"], list) and isinstance(presence["status"], str)
        assert type(presence["since"]) is int and type(presence["afk"]) is bool
    if opcode == 37:
        subscriptions = data["subscriptions"]
        assert isinstance(subscriptions, dict) and subscriptions
        for guild, subscription in subscriptions.items():
            assert isinstance(guild, str) and guild.isdecimal()
            for flag in ("typing", "threads", "activities", "member_updates"):
                if flag in subscription:
                    assert type(subscription[flag]) is bool
            for collection in ("members", "thread_member_lists"):
                if collection in subscription:
                    assert isinstance(subscription[collection], list)
            channels = subscription.get("channels", {})
            assert isinstance(channels, dict)
            for channel, ranges in channels.items():
                assert isinstance(channel, str) and channel.isdecimal()
                assert isinstance(ranges, list) and 1 <= len(ranges) <= 2, "replace removed channels; do not send empty ranges"
                previous = -1
                for bounds in ranges:
                    assert len(bounds) == 2 and all(type(value) is int for value in bounds)
                    first, last = bounds
                    assert first % 100 == 0 and 0 <= first <= last < first + 100 and previous < first
                    previous = last


def receive(connection):
    while True:
        first, second = exact(connection, 2)
        size = second & 127
        if size == 126:
            size = struct.unpack("!H", exact(connection, 2))[0]
        elif size == 127:
            size = struct.unpack("!Q", exact(connection, 8))[0]
        assert second & 128 and size < 65536, "client frames must be masked and bounded"
        mask = exact(connection, 4)
        data = exact(connection, size)
        data = bytes(byte ^ mask[index % 4] for index, byte in enumerate(data))
        if first & 15 == 10:  # libcurl answers the interleaved ping automatically.
            continue
        assert first == 129, "Gateway JSON must arrive as a complete text frame"
        packet = json.loads(data)
        validate_send(packet)
        return packet


def frame(connection, data, opcode=1, final=True):
    if isinstance(data, dict):
        data = json.dumps(data, ensure_ascii=False).encode()
    header = bytes([(128 if final else 0) | opcode])
    header += bytes([len(data)]) if len(data) < 126 else b"\x7e" + struct.pack("!H", len(data))
    connection.sendall(header + data)


def packet(connection, op, data, kind=None, sequence=None):
    frame(connection, {"op": op, "d": data, "t": kind, "s": sequence})


def upgrade(listener, path="/?v=9&encoding=json"):
    connection, _ = listener.accept()
    connection.settimeout(4)
    request = b""
    while b"\r\n\r\n" not in request:
        request += exact(connection, 1)
    assert request.split(b"\r\n", 1)[0] == f"GET {path} HTTP/1.1".encode(), "wrong Gateway route"
    headers = dict(line.split(b": ", 1) for line in request.split(b"\r\n")[1:] if b": " in line)
    key = next(value for name, value in headers.items() if name.lower() == b"sec-websocket-key")
    accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
    connection.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
    packet(connection, 10, {"heartbeat_interval": 1000})
    return connection


def wait_for(connection, wanted):
    for _ in range(12):
        value = receive(connection)
        if value["op"] == 1:
            packet(connection, 11, None)
        if value["op"] == wanted:
            return value
    raise AssertionError("expected Gateway opcode was not received")


def presence_restore(listener, phase):
    with upgrade(listener) as connection:
        wait_for(connection, 2)
        session = f"presence-{phase}"
        status = ["dnd", "idle", "online"][phase]
        packet(connection, 0, {"session_id": session, "sessions": [{"session_id": session, "status": status}]}, "READY", 1)
        presence = wait_for(connection, 3)
        assert presence["d"]["status"] == status, "READY status must win over the settings read"
        if phase == 0:
            assert presence["d"]["activities"] == [{"name": "Custom Status", "type": 4, "state": "early custom status", "emoji": {"id": "77", "name": "wave"}}]
        else:
            assert presence["d"]["activities"] == []
        initial = wait_for(connection, 37)
        assert initial["d"]["subscriptions"]["1"]["channels"] == {}
        subscription = wait_for(connection, 37)
        assert "2" in subscription["d"]["subscriptions"]["1"]["channels"]
        if phase:
            packet(connection, 0, {}, "RESTORE_PRESENCE", 2)
            if phase == 1:
                # A late restore changes only presence, without repeating the
                # subscription or reconnecting an already established socket.
                while True:
                    presence = receive(connection)
                    if presence["op"] == 1:
                        packet(connection, 11, None)
                        continue
                    assert presence["op"] == 3, "presence restoration changed the subscription"
                    break
                assert presence["d"]["status"] == "idle"
                assert presence["d"]["activities"] == [{"name": "Custom Status", "type": 4, "state": "late custom status", "emoji": {"name": "🌙"}}]
        packet(connection, 0, {}, "RESTORE_COMPLETE", 3)


def serve(listener, errors):
    try:
        for phase in range(3):
            presence_restore(listener, phase)
        with upgrade(listener) as connection:
            wait_for(connection, 2)
            packet(connection, 0, {"session_id": "thread-session"}, "READY", 1)
            wait_for(connection, 3)
            initial = wait_for(connection, 37)["d"]["subscriptions"]["1"]
            assert initial["channels"] == {} and initial["thread_member_lists"] == []
            thread = wait_for(connection, 37)["d"]["subscriptions"]["1"]
            assert thread["channels"] == {} and thread["thread_member_lists"] == ["40"], "threads need a participant subscription, not guild row ranges"
            packet(connection, 0, {"guild_id": "1", "thread_id": "40", "members": [
                {"user_id": "7", "member": {"user": {"id": "7", "username": "Maya"}, "nick": "Thread nickname", "roles": []}, "presence": {"status": "online"}},
                {"user_id": "8", "member": {"user": {"id": "8", "username": "Noah"}, "roles": []}},
            ]}, "THREAD_MEMBER_LIST_UPDATE", 2)
            regular = wait_for(connection, 37)["d"]["subscriptions"]["1"]
            assert regular["thread_member_lists"] == [] and regular["channels"] == {"2": [[0, 99], [100, 199]]}, "leaving a thread clears its participant subscription"
            switched_at = time.monotonic()
            packet(connection, 0, {}, "COALESCE_START", 3)
            packet(connection, 0, {}, "COALESCE_FINISH", 4)
            latest_presence = wait_for(connection, 3)
            assert latest_presence["d"]["status"] == "idle", "presence must coalesce rapid choices"
            latest_channel = wait_for(connection, 37)["d"]["subscriptions"]["1"]
            assert latest_channel["channels"] == {"4": [[0, 99], [100, 199]]}, "queued navigation must retain only the latest view"
            assert time.monotonic() - switched_at >= 0.4, "channel changes must honor subscription pacing"
            packet(connection, 0, {"guild_id": "1", "channel_id": "40", "user_id": "7", "timestamp": 1000,
                "member": {"user": {"id": "7", "username": "Maya"}, "nick": "Thread nickname"}}, "TYPING_START", 5)
            packet(connection, 0, {}, "THREAD_TEST_COMPLETE", 6)
        with upgrade(listener) as connection:
            wait_for(connection, 2)
            packet(connection, 0, {"session_id": "cooldown-session"}, "READY", 1)
            wait_for(connection, 3)
            frame(connection, struct.pack("!H", 4008), opcode=8)
        with upgrade(listener) as connection:
            identify = wait_for(connection, 2)
            assert identify["d"]["token"] == "synthetic-token"
            assert identify["d"]["compress"] is False
            resume_url = f"ws://127.0.0.1:{listener.getsockname()[1]}/resume"
            packet(connection, 0, {"session_id": "synthetic-session", "resume_gateway_url": resume_url, "relationships": [{"id": "7", "nickname": "Initial"}]}, "READY", 1)
            presence = wait_for(connection, 3)
            assert presence["d"]["status"] == "dnd"
            assert presence["d"]["activities"] == [{"name": "Custom Status", "type": 4, "state": "working", "emoji": {"id": "77", "name": "wave"}}]
            initial = wait_for(connection, 37)
            assert initial["d"]["subscriptions"]["1"]["typing"] is True
            assert initial["d"]["subscriptions"]["1"]["channels"] == {}
            subscription = wait_for(connection, 37)
            assert subscription["d"]["subscriptions"]["1"]["channels"]["2"] == [[100, 199], [200, 299]]
            members = wait_for(connection, 8)
            assert members["d"] == {"guild_id": "1", "user_ids": ["7", "8"], "limit": 100}
            packet(connection, 0, {"guild_id": "1", "members": [{"user": {"id": "7"}, "roles": ["50"]}, {"user": {"id": "8"}, "roles": []}], "chunk_index": 0, "chunk_count": 1}, "GUILD_MEMBERS_CHUNK", 2)
            search = wait_for(connection, 8)
            assert search["d"] == {"guild_id": "1", "query": "May", "limit": 25, "presences": False, "nonce": "complete-test"}
            packet(connection, 0, {"guild_id": "1", "nonce": "complete-test", "members": [{"user": {"id": "9", "username": "maya"}, "nick": "May", "roles": []}], "chunk_index": 0, "chunk_count": 1}, "GUILD_MEMBERS_CHUNK", 2)
            packet(connection, 0, {"merged_presences": {"friends": [{"user_id": "7", "status": "online"}]}}, "READY_SUPPLEMENTAL", 3)
            packet(connection, 0, {"user": {"id": "7"}, "status": "idle"}, "PRESENCE_UPDATE", 4)
            packet(connection, 0, {"id": "7", "nickname": "My friend"}, "RELATIONSHIP_UPDATE", 5)
            # Control frames may occur between fragments of a text message.
            data = json.dumps({"op": 0, "d": {"content": "synthetic 🌿"}, "t": "MESSAGE_CREATE", "s": 6}, ensure_ascii=False).encode()
            frame(connection, data[:17], final=False)
            frame(connection, b"ping", opcode=9)
            frame(connection, data[17:], opcode=0)
            packet(connection, 0, {}, "MESSAGE_UPDATE", 7)
            packet(connection, 0, {}, "GUILD_MEMBER_LIST_UPDATE", 8)
            packet(connection, 0, {}, "SWITCH_CHANNEL", 9)
            switched = wait_for(connection, 37)
            assert switched["d"]["subscriptions"]["1"]["channels"] == {"3": [[0, 99], [100, 199]]}
            reconnect_at = time.monotonic()
            packet(connection, 7, None)
        # Explicit Gateway handoffs should not incur transport-failure backoff.
        # The original unconditional backoff waits at least one second here.
        listener.settimeout(0.85)
        with upgrade(listener, "/resume?v=9&encoding=json") as connection:
            assert time.monotonic() - reconnect_at < 0.85, "Gateway delayed an explicit reconnect"
            resume = wait_for(connection, 6)
            assert resume["d"] == {"token": "synthetic-token", "session_id": "synthetic-session", "seq": 9}
            packet(connection, 0, {}, "RESUMED", 10)
            presence = wait_for(connection, 3)
            assert presence["d"]["status"] == "dnd" and presence["d"]["activities"][0]["state"] == "working"
            wait_for(connection, 37)
            wait_for(connection, 37)
            packet(connection, 0, {"id": "7"}, "RELATIONSHIP_REMOVE", 11)
            packet(connection, 0, {}, "MESSAGE_DELETE", 12)
            # Keep the peer open until the client has drained the final data;
            # closing during a subscription/heartbeat write can reset TCP first.
            assert wait_for(connection, 1)["d"] == 12
            frame(connection, struct.pack("!H", 4002), opcode=8)
        listener.settimeout(3)
        with upgrade(listener, "/resume?v=9&encoding=json") as connection:
            assert wait_for(connection, 6)["d"]["seq"] == 12
            packet(connection, 0, {}, "RESUMED", 13)
            wait_for(connection, 3)
            wait_for(connection, 37)
            wait_for(connection, 37)
            assert wait_for(connection, 1)["d"] == 13
            frame(connection, struct.pack("!H", 4004), opcode=8)
    except Exception as error:
        errors.append(error)


with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    listener.listen(2)
    listener.settimeout(8)
    errors = []
    thread = threading.Thread(target=serve, args=(listener, errors), daemon=True)
    thread.start()
    result = subprocess.run([sys.argv[1], f"ws://127.0.0.1:{listener.getsockname()[1]}/?v=9&encoding=json"], timeout=10, capture_output=True, text=True)
    thread.join(timeout=1)
    if errors:
        raise errors[0]
    assert re.search(r"gateway close=4002 ready=1 phase=ready recent_opcodes=\[(?:\d+,){0,7}\d+\]", result.stderr), "decode closes need bounded numeric opcode context"
    assert "synthetic-token" not in result.stderr, "Gateway diagnostics must not disclose credentials"
    if result.returncode:
        sys.stderr.write(result.stderr)
    sys.exit(result.returncode)
