"""Check that the executable reports the source version without starting a session."""

from pathlib import Path
import subprocess
import sys


expected = Path(sys.argv[2]).read_text(encoding="utf-8").strip()
result = subprocess.run(
    [sys.argv[1], *sys.argv[3:], "--version"],
    text=True, capture_output=True, check=True, timeout=10,
)
if result.stdout != f"cressida {expected}\n" or result.stderr:
    raise SystemExit(f"Unexpected version output: stdout={result.stdout!r}, stderr={result.stderr!r}")
