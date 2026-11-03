"""Stage complete release assets, verify their remote digests, then publish."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from release_archive import release_version
from release_checksums import write_checksums


GH_TIMEOUT_SECONDS = 300
RELEASE_FIELDS = "databaseId,tagName,isDraft,isPrerelease,assets,url"


class GitHub:
    def __init__(self, repository):
        self.repository = repository

    def run(self, *arguments, missing_ok=False):
        result = subprocess.run(
            ["gh", "release", *arguments, "--repo", self.repository],
            text=True, capture_output=True, timeout=GH_TIMEOUT_SECONDS,
        )
        if result.returncode:
            if missing_ok and result.stderr.strip().casefold() == "release not found":
                return None
            raise ValueError(result.stderr.strip() or "The release command failed.")
        return result.stdout

    def view(self, tag):
        response = self.run("view", tag, "--json", RELEASE_FIELDS, missing_ok=True)
        if response is None:
            return None
        try:
            release = json.loads(response)
        except json.JSONDecodeError as error:
            raise ValueError("The release lookup returned invalid JSON.") from error
        if (not isinstance(release, dict) or release.get("tagName") != tag
                or type(release.get("databaseId")) is not int or release["databaseId"] <= 0
                or type(release.get("isDraft")) is not bool
                or not isinstance(release.get("url"), str)):
            raise ValueError("The release lookup returned an unexpected identity or state.")
        return release


def local_assets(destination, version):
    if destination.is_symlink() or not destination.is_dir():
        raise ValueError("The release asset directory must be a real directory.")
    checksums = destination / "SHA256SUMS"
    if checksums.is_symlink() or (checksums.exists() and not checksums.is_file()):
        raise ValueError("SHA256SUMS must be a regular file.")
    write_checksums(destination, version)
    assets = {}
    for line in checksums.read_text().splitlines():
        digest, name = line.split("  ", 1)
        assets[name] = {"size": (destination / name).stat().st_size, "digest": "sha256:" + digest}
    with checksums.open("rb") as source:
        assets[checksums.name] = {
            "size": checksums.stat().st_size,
            "digest": "sha256:" + hashlib.file_digest(source, "sha256").hexdigest(),
        }
    return assets


def verify_uploaded(release, expected, release_id):
    if release is None or release["databaseId"] != release_id or not release["isDraft"]:
        raise ValueError("The staged release changed identity or was published before verification.")
    assets = release.get("assets")
    if (not isinstance(assets, list) or len(assets) != len(expected)
            or any(not isinstance(asset, dict) or not isinstance(asset.get("name"), str) for asset in assets)
            or {asset.get("name") for asset in assets} != set(expected)):
        raise ValueError("The staged release does not contain the exact required asset set.")
    for asset in assets:
        local = expected[asset["name"]]
        if (asset.get("state") != "uploaded" or type(asset.get("size")) is not int
                or asset["size"] != local["size"]
                or asset.get("digest") != local["digest"]):
            raise ValueError(f"The uploaded release asset failed verification: {asset['name']}")


def publish(root, ref, repository, github=None):
    version = release_version(ref, root)
    tag = "v" + version
    github = github or GitHub(repository)
    release = github.view(tag)
    if release is not None and not release["isDraft"]:
        return f"Release already published; its assets were left unchanged: {release['url']}"
    destination = root / "dist"
    expected = local_assets(destination, version)
    if release is None:
        arguments = ["create", tag, "--draft", "--verify-tag", "--generate-notes", "--title", "Cressida " + tag]
        if "-" in version:
            arguments.append("--prerelease")
        github.run(*arguments)
        release = github.view(tag)
    if release is None or not release["isDraft"]:
        raise ValueError("The release could not be staged as a draft.")
    release_id = release["databaseId"]
    github.run("upload", tag, *(str(destination / name) for name in sorted(expected)), "--clobber")
    verify_uploaded(github.view(tag), expected, release_id)
    github.run("edit", tag, "--draft=false")
    published = github.view(tag)
    if published is None or published["databaseId"] != release_id or published["isDraft"]:
        raise ValueError("The verified release has not become public.")
    return f"Published complete release: {published['url']}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ref")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    repository = os.environ.get("GH_REPO", "")
    if not repository:
        parser.error("Set GH_REPO to the target repository.")
    try:
        print(publish(root, args.ref, repository))
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
