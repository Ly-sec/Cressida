"""Require the client's argument parser to reject invalid syntax before startup."""

import subprocess
import sys

ARGUMENT_ERROR_EXIT_CODE = 2
CLI_TIMEOUT_SECONDS = 10


def main():
    if len(sys.argv) < 2:
        raise SystemExit("Expected the client executable and its invalid arguments.")
    try:
        result = subprocess.run(sys.argv[1:], check=False, timeout=CLI_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        raise SystemExit("The argument parser did not return within ten seconds.") from None
    if result.returncode != ARGUMENT_ERROR_EXIT_CODE:
        raise SystemExit(
            f"Expected argument error exit code {ARGUMENT_ERROR_EXIT_CODE}, got {result.returncode}."
        )


if __name__ == "__main__":
    main()
