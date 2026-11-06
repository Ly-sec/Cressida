"""Validate multipart framing and binary data on a loopback-only HTTP peer."""
from email.parser import BytesParser
from email.policy import default
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import re
import subprocess
import sys
import threading

errors = []

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        try:
            assert self.path == "/messages"
            length = int(self.headers["Content-Length"])
            assert 0 < length < 8192
            body = self.rfile.read(length)
            message = BytesParser(policy=default).parsebytes(
                ("Content-Type: " + self.headers["Content-Type"] + "\r\n\r\n").encode() + body
            )
            parts = {part.get_param("name", header="content-disposition"): part for part in message.iter_parts()}
            payload = json.loads(parts["payload_json"].get_payload(decode=True))
            if payload.get("flags") == 8192:
                assert set(parts) == {"payload_json", "files[0]"}
                assert "content" not in payload and "message_reference" not in payload
                assert "sticker_ids" not in payload and "poll" not in payload
                assert payload["nonce"] == "123456790" and payload["enforce_nonce"] is True
                assert payload["attachments"] == [{
                    "id": 0, "filename": "voice-message.ogg",
                    "duration_secs": 1.25, "waveform": "AID/",
                }]
                assert parts["files[0]"].get_content_type() == "audio/ogg"
                assert parts["files[0]"].get_filename() == "voice-message.ogg"
                assert parts["files[0]"].get_payload(decode=True) == b"OggS\x00\x0a\xff"
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"ok")
                return
            assert set(parts) == {"payload_json", "files[0]", "files[1]"}
            assert payload["content"] == "first\nsecond 🌿 <@7> <@&8> @everyone @here"
            assert payload["message_reference"] == {"message_id": "20", "channel_id": "10", "fail_if_not_exists": True}
            assert payload["allowed_mentions"]["replied_user"] is False
            assert payload["allowed_mentions"]["parse"] == ["users", "roles", "everyone"]
            assert payload["nonce"] == "123456789" and payload["enforce_nonce"] is True
            assert "flags" not in payload
            assert len(payload["attachments"]) == 2
            for index, extension in enumerate(("png", "jpg")):
                attachment = payload["attachments"][index]
                assert attachment["id"] == index
                assert re.fullmatch(r"[0-9a-f]{32}\." + extension, attachment["filename"])
                assert parts[f"files[{index}]"].get_filename() == attachment["filename"]
            assert b"plant" not in body and b"second.jpg" not in body
            assert parts["files[0]"].get_content_type() == "image/png"
            assert parts["files[0]"].get_payload(decode=True) == b"\x89PNG\x00\x0a\xff"
            assert parts["files[1]"].get_payload(decode=True) == b"\xff\xd8\xff"
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"ok")
        except Exception as error:
            errors.append(repr(error))
            self.send_error(400)

with HTTPServer(("127.0.0.1", 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    result = subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}/messages"], timeout=8)
    server.shutdown()
    thread.join()
if errors:
    print("\n".join(errors), file=sys.stderr)
sys.exit(result.returncode or bool(errors))
