"""Exercise credential changes with private Gateway peers and synthetic data."""

import socket
import struct
import subprocess
import sys
import threading
import time

from gateway_fixture import MAXIMUM_CONTROL_MESSAGES, frame, packet, receive, upgrade, wait_for, wait_for_sequence


ORIGINAL_TOKEN = "synthetic-original-credential"
REPLACEMENT_TOKEN = "synthetic-replacement-credential"
CASES = ("late-failure", "rotation", "reset", "unchanged", "account")
PEER_TIMEOUT = 8
PROCESS_TIMEOUT = 14
CLOSE_OPCODE = 8
OPCODE_MASK = 15
MINIMUM_IDENTIFY_SPACING = 4.5


def expect_disconnect(connection):
    for _ in range(MAXIMUM_CONTROL_MESSAGES):
        try:
            header = connection.recv(1, socket.MSG_PEEK)
            if not header or header[0] & OPCODE_MASK == CLOSE_OPCODE:
                return
            value = receive(connection)
        except (EOFError, ConnectionResetError):
            return
        assert value["op"] == 1, "retired authentication must stop application traffic"
        packet(connection, 11, None)
    raise AssertionError("retired authentication did not close its connection")


def serve(listener, case, errors):
    try:
        with upgrade(listener) as connection:
            assert wait_for(connection, 2)["d"]["token"] == ORIGINAL_TOKEN
            identified_at = time.monotonic()
            resume_url = f"ws://127.0.0.1:{listener.getsockname()[1]}/retired-session"
            packet(connection, 0, {"session_id": "synthetic-retired-session", "resume_gateway_url": resume_url}, "READY", 1)
            if case == "unchanged":
                assert wait_for(connection, 3)["d"]["status"] == "dnd"
                wait_for_sequence(connection, 1)
                packet(connection, 0, {}, "AUTHENTICATION_TEST_COMPLETE", 2)
                return
            if case == "late-failure":
                wait_for(connection, 3)
                frame(connection, struct.pack("!H", 4004), opcode=CLOSE_OPCODE)
                expect_disconnect(connection)
            else:
                expect_disconnect(connection)
        with upgrade(listener) as connection:
            identify = wait_for(connection, 2)
            assert time.monotonic() - identified_at >= MINIMUM_IDENTIFY_SPACING, "credential changes must retain Identify pacing"
            expected = ORIGINAL_TOKEN if case == "reset" else REPLACEMENT_TOKEN
            assert identify["d"]["token"] == expected, "fresh Identify must use the current credential"
            packet(connection, 0, {"session_id": "synthetic-current-session"}, "READY", 1)
            assert wait_for(connection, 3)["d"]["status"] == "dnd", "credential rotation retains the chosen presence"
            packet(connection, 0, {}, "AUTHENTICATION_TEST_COMPLETE", 2)
    except Exception as error:
        errors.append(error)


def main():
    cases = tuple(sys.argv[2:]) or CASES
    assert all(case in CASES for case in cases), "unknown authentication fixture case"
    for case in cases:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(2)
            listener.settimeout(PEER_TIMEOUT)
            errors = []
            thread = threading.Thread(target=serve, args=(listener, case, errors), daemon=True)
            thread.start()
            endpoint = f"ws://127.0.0.1:{listener.getsockname()[1]}/?v=9&encoding=json"
            result = subprocess.run([sys.argv[1], endpoint, case], timeout=PROCESS_TIMEOUT, capture_output=True, text=True)
            thread.join(timeout=1)
            if errors:
                raise AssertionError(f"Authentication case {case} failed: {errors[0]}")
            assert not thread.is_alive(), "private authentication peer did not finish"
            for private in (ORIGINAL_TOKEN, REPLACEMENT_TOKEN, "synthetic-retired-session", "synthetic-current-session", "127.0.0.1"):
                assert private not in result.stderr, "authentication diagnostics disclosed private data"
            if result.returncode:
                sys.stderr.write(result.stderr)
                sys.exit(result.returncode)
            print(f"Gateway authentication case {case} passed")


if __name__ == "__main__":
    main()
