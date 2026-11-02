"""Capture shared public identity on private loopback HTTP requests."""

import base64
from collections import Counter
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import platform
import subprocess
import sys
import threading


PLATFORM = {
    "aarch64": "X11; Linux aarch64",
    "x86_64": "X11; Linux x86_64",
}.get(platform.machine(), "X11; Linux")
USER_AGENT = (
    f"Mozilla/5.0 ({PLATFORM}) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36"
)
PROPERTIES = {
    "os": "Linux", "browser": "Chrome", "device": "", "system_locale": "en-US",
    "browser_user_agent": USER_AGENT, "browser_version": "128.0.0.0", "os_version": "",
    "referrer": "", "referring_domain": "", "referrer_current": "", "referring_domain_current": "",
    "release_channel": "stable", "client_build_number": 634304, "client_event_source": None,
}
EXPECTED_PATHS = ["/rest-first", "/rest-reset", "/gateway", "/unrelated"]
errors, received = [], []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def header(self, name, expected):
        values = self.headers.get_all(name, [])
        assert values == ([] if expected is None else [expected]), f"Wrong or duplicate {name} on {self.path}"

    def do_GET(self):
        try:
            assert len(received) < len(EXPECTED_PATHS), "Unexpected extra HTTP request"
            assert self.path == EXPECTED_PATHS[len(received)], "Unexpected HTTP identity capture order"
            received.append(self.path)
            assert all(count == 1 for count in Counter(name.lower() for name in self.headers).values()), "No duplicate headers"
            if self.path == "/unrelated":
                for name in ("User-Agent", "Authorization", "Content-Type", "Origin", "Referer", "Accept-Language",
                             "X-Super-Properties", "X-Discord-Locale", "X-Discord-Timezone"):
                    self.header(name, None)
            else:
                self.header("User-Agent", USER_AGENT)
                self.header("Origin", "https://discord.com")
                self.header("Accept-Language", "en-US,en;q=0.9")
                if self.path.startswith("/rest-"):
                    self.header("Authorization", "synthetic-identity-token")
                    self.header("Content-Type", "application/json")
                    self.header("Referer", "https://discord.com/channels/@me")
                    self.header("X-Discord-Locale", "en-US")
                    self.header("X-Discord-Timezone", "UTC")
                    encoded = self.headers.get_all("X-Super-Properties", [])
                    assert len(encoded) == 1, "REST sends exactly one properties header"
                    decoded = json.loads(base64.b64decode(encoded[0], validate=True))
                    assert decoded == PROPERTIES, "REST properties match the public Identify profile"
                    assert all(key not in decoded for key in ("token", "authorization", "session_id", "user_id",
                                                               "account_id", "installation_id")), "No account data in identity"
                else:
                    self.header("Authorization", None)
                    self.header("Content-Type", None)
                    self.header("X-Super-Properties", None)
            self.send_response(200)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"ok")
        except Exception as error:
            errors.append(str(error))
            self.send_error(400)


with HTTPServer(("127.0.0.1", 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = subprocess.run(
            [sys.argv[1], f"http://127.0.0.1:{server.server_port}"],
            capture_output=True, text=True, timeout=12,
        )
    except subprocess.TimeoutExpired:
        sys.exit("The private identity capture peer timed out")
    finally:
        server.shutdown()
        thread.join(timeout=3)
if errors:
    print("\n".join(errors), file=sys.stderr)
if result.stdout:
    print(result.stdout, end="")
if result.stderr:
    print(result.stderr, end="", file=sys.stderr)
if received != EXPECTED_PATHS:
    print("The native peer did not complete all private identity captures", file=sys.stderr)
sys.exit(result.returncode or bool(errors) or received != EXPECTED_PATHS)
