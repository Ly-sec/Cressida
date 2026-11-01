"""Select portable encryption checks from event metadata and a read-only comparison."""

import http.client
import json
import os
from pathlib import Path, PurePosixPath
import re
import urllib.request


COMPARISON_FILE_LIMIT = 300
RESPONSE_BYTE_LIMIT = 8 * 1024 * 1024
REQUEST_TIMEOUT_SECONDS = 30
SHA = re.compile(r"[0-9a-fA-F]{40}")
REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
RELEVANT_FILES = {
    "VERSION", "flake.nix", "flake.lock", "justfile",
    ".github/workflows/ci.yml", ".github/workflows/ci-dave.yml",
    ".github/workflows/release.yml",
    ".github/workflows/scripts/dave_changes.py",
    ".github/workflows/scripts/test_dave_changes.py",
    ".github/workflows/scripts/check_mls_snapshot.py",
    ".github/workflows/scripts/test_mls_snapshot.py",
    "src/voice/encryption.cpp", "src/voice/encryption.h", "src/voice/media.h",
    "tests/voice_dave_test.cpp", "tests/voice_dave_reference_test.cpp",
    "tests/voice_video_crypto_test.cpp", "tests/voice_encryption_test.cpp",
}
RELEVANT_DIRECTORIES = ("src/voice/dave/", "third_party/mlspp/", "tests/voice_dave/", "nix/")
MESON_FILES = {"meson.build", "meson.options", "meson_options.txt"}


def relevant(path):
    return (path in RELEVANT_FILES or path.startswith(RELEVANT_DIRECTORIES)
            or PurePosixPath(path).name in MESON_FILES)


def comparison_range(event_name, event):
    if event_name == "pull_request":
        base = event["pull_request"]["base"]["sha"]
        head = event["pull_request"]["head"]["sha"]
    elif event_name == "merge_group":
        base = event["merge_group"]["base_sha"]
        head = event["merge_group"]["head_sha"]
    elif event_name == "push" and event.get("ref", "").startswith("refs/heads/"):
        base, head = event["before"], event["after"]
    else:
        return None
    if not all(isinstance(value, str) and SHA.fullmatch(value) and int(value, 16) for value in (base, head)):
        return None
    return base, head


def select(event_name, event, fetch):
    try:
        revisions = comparison_range(event_name, event)
        if revisions is None:
            return True, "manual, release or unknown comparison"
        comparison = fetch(*revisions)
        statuses = ("ahead", "identical", "diverged") if event_name == "pull_request" else ("ahead", "identical")
        if comparison.get("status") not in statuses:
            return True, "nonlinear or unknown comparison"
        files = comparison.get("files")
        if not isinstance(files, list) or len(files) >= COMPARISON_FILE_LIMIT:
            return True, "missing or possibly truncated file list"
        for item in files:
            if not isinstance(item, dict) or not isinstance(item.get("filename"), str):
                return True, "incomplete file metadata"
            names = [item["filename"]]
            if item.get("status") == "renamed":
                if not isinstance(item.get("previous_filename"), str):
                    return True, "incomplete rename metadata"
                names.append(item["previous_filename"])
            if any(relevant(name) for name in names):
                return True, "encryption, build or workflow changes"
        return False, "only unrelated files changed"
    except (OSError, ValueError, TypeError, KeyError, AttributeError, http.client.HTTPException):
        return True, "comparison unavailable"


def fetch_comparison(base, head):
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    token = os.environ.get("GITHUB_TOKEN", "")
    if not REPOSITORY.fullmatch(repository) or not token:
        raise ValueError("Missing comparison context")
    request = urllib.request.Request(
        f"https://api.github.com/repos/{repository}/compare/{base}...{head}?per_page=1",
        headers={"Accept": "application/vnd.github+json", "Authorization": f"Bearer {token}",
                 "X-GitHub-Api-Version": "2022-11-28"},
    )
    with urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
        body = response.read(RESPONSE_BYTE_LIMIT + 1)
    if len(body) > RESPONSE_BYTE_LIMIT:
        raise ValueError("Comparison exceeds response limit")
    return json.loads(body)


def main():
    try:
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
        run, reason = select(os.environ.get("GITHUB_EVENT_NAME", ""), event, fetch_comparison)
    except (OSError, ValueError, KeyError):
        run, reason = True, "event unavailable"
    with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
        output.write(f"run={str(run).lower()}\n")
    print(f"Portable DAVE: {'run' if run else 'skip'} ({reason})")


if __name__ == "__main__":
    main()
