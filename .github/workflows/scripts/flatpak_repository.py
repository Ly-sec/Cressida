"""Prepare a signed Flatpak update feed inside an existing static site artifact."""

import argparse
import base64
from contextlib import ExitStack
import hashlib
import html
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import urlsplit


APPLICATION = "dev.lysec.Cressida"
REPOSITORY = "Ly-sec/Cressida"
ARCHITECTURES = ("x86_64", "aarch64")
MAXIMUM_PAGES_BYTES = 1_000_000_000
MAXIMUM_BUNDLE_BYTES = 2 * 1024 * 1024 * 1024
COMMAND_SECONDS = 600
SIGNING_CACHE_SECONDS = 3600
PRIVATE_ENVIRONMENT = ("CRESSIDA_FLATPAK_GPG_KEY", "CRESSIDA_FLATPAK_GPG_PASSPHRASE")


def execute(arguments, *, input=None):
    environment = os.environ.copy()
    for name in PRIVATE_ENVIRONMENT:
        environment.pop(name, None)
    if arguments[0] != "gh":
        environment.pop("GH_TOKEN", None)
        environment.pop("GITHUB_TOKEN", None)
    result = subprocess.run(arguments, input=input, capture_output=True, timeout=COMMAND_SECONDS, env=environment)
    if result.returncode:
        raise ValueError(f"The repository preparation command failed: {arguments[0]}")
    return result.stdout


def repository_base(site_url):
    parsed = urlsplit(site_url)
    if (parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password
            or parsed.query or parsed.fragment or any(char.isspace() for char in site_url)
            or any(part in (".", "..") for part in parsed.path.split("/"))):
        raise ValueError("Set the public HTTPS website URL, including its repository subpath.")
    return site_url.rstrip("/") + "/flatpak/"


def tree_bytes(root):
    total = 0
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError("The Pages artifact must not contain symbolic links.")
        if path.is_file():
            total += path.stat().st_size
    return total


def stop_signing_agent(runner, keyring):
    try:
        runner(["gpgconf", "--homedir", str(keyring), "--kill", "gpg-agent"])
    except (OSError, ValueError, subprocess.TimeoutExpired):
        pass


def unlock_signer(runner, keyring, work, fingerprint, passphrase):
    if any(character in passphrase for character in ("\n", "\r", "\0")):
        raise ValueError("The signing passphrase must be a single line without a trailing newline.")
    probe, signature = work / "signing-check.txt", work / "signing-check.sig"
    probe.write_text("Cressida repository signing check\n")
    try:
        # Prime the isolated agent for the same signer used by Flatpak/GPGME.
        runner(["gpg", "--homedir", str(keyring), "--batch", "--yes", "--no-tty",
                "--pinentry-mode", "loopback", "--passphrase-fd", "0", "--local-user", fingerprint,
                "--output", str(signature), "--detach-sign", str(probe)],
               input=passphrase.encode() + b"\n")
    except ValueError as error:
        raise ValueError("Could not unlock the signing key. Check CRESSIDA_FLATPAK_GPG_PASSPHRASE.") from error
    finally:
        probe.unlink(missing_ok=True)
        signature.unlink(missing_ok=True)


def prepare(site, site_url, private_key, fingerprint, *, passphrase="", runner=execute):
    site = Path(site)
    if site.is_symlink() or not site.is_dir() or not (site / "index.html").is_file():
        raise ValueError("Build the website first, then add its Flatpak feed.")
    destination = site / "flatpak"
    if destination.exists():
        raise ValueError("The website already contains a flatpak directory; refusing to overwrite it.")
    base = repository_base(site_url)
    fingerprint = fingerprint.strip().upper()
    if not private_key or not re.fullmatch(r"[0-9A-F]{40}|[0-9A-F]{64}", fingerprint):
        raise ValueError("Configure the Flatpak signing key and its full public fingerprint before enabling the feed.")
    release = json.loads(runner(["gh", "release", "view", "--repo", REPOSITORY,
                                 "--json", "tagName,isDraft,isPrerelease,assets"]))
    tag = release.get("tagName")
    if (not isinstance(tag, str) or not re.fullmatch(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", tag)
            or release.get("isDraft") is not False or release.get("isPrerelease") is not False):
        raise ValueError("The Flatpak stable feed requires a published stable release.")
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise ValueError("The published release has no usable asset metadata.")
    names = [f"cressida-{tag[1:]}-{arch}.flatpak" for arch in ARCHITECTURES]
    expected = {}
    for name in (*names, "SHA256SUMS"):
        matches = [asset for asset in assets if isinstance(asset, dict) and asset.get("name") == name]
        if len(matches) != 1:
            raise ValueError("The release must have exactly one bundle per architecture and one checksum file.")
        asset = matches[0]
        digest = asset.get("digest", "")
        size = asset.get("size")
        if (not isinstance(digest, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest)
                or type(size) is not int or not 0 < size <= MAXIMUM_BUNDLE_BYTES
                or asset.get("state") != "uploaded"):
            raise ValueError("A release asset is incomplete or missing its integrity metadata.")
        expected[name] = (size, digest.removeprefix("sha256:"))
    with tempfile.TemporaryDirectory(prefix="cressida-flatpak-", dir=site.parent) as temporary, ExitStack() as cleanup:
        work = Path(temporary)
        downloads, keyring, public = work / "downloads", work / "keyring", work / "public"
        downloads.mkdir()
        keyring.mkdir(mode=0o700)
        cleanup.callback(stop_signing_agent, runner, keyring)
        agent_config = keyring / "gpg-agent.conf"
        agent_config.write_text("allow-loopback-pinentry\ndefault-cache-ttl " + str(SIGNING_CACHE_SECONDS)
                                + "\nmax-cache-ttl " + str(SIGNING_CACHE_SECONDS) + "\n")
        agent_config.chmod(0o600)
        public.mkdir()
        runner(["gpg", "--homedir", str(keyring), "--batch", "--import"], input=private_key.encode())
        listing = runner(["gpg", "--homedir", str(keyring), "--batch", "--with-colons", "--fingerprint", "--list-secret-keys"]).decode()
        primary = []
        need_primary = False
        for line in listing.splitlines():
            fields = line.split(":")
            if fields[0] == "sec":
                need_primary = True
            elif fields[0] == "fpr" and need_primary:
                primary.append(fields[9])
                need_primary = False
        if primary != [fingerprint]:
            raise ValueError("The imported private key does not match the configured signing fingerprint.")
        public_key = runner(["gpg", "--homedir", str(keyring), "--batch", "--export", fingerprint])
        if not public_key:
            raise ValueError("The signing key has no exportable public key.")
        unlock_signer(runner, keyring, work, fingerprint, passphrase)
        for name, (size, digest) in expected.items():
            runner(["gh", "release", "download", tag, "--repo", REPOSITORY,
                    "--pattern", name, "--dir", str(downloads)])
            path = downloads / name
            if path.is_symlink() or not path.is_file() or path.stat().st_size != size:
                raise ValueError("A downloaded release file has an unexpected type or size.")
            with path.open("rb") as stream:
                if hashlib.file_digest(stream, "sha256").hexdigest() != digest:
                    raise ValueError("A downloaded release file failed its integrity check.")
        checksums = {}
        for line in (downloads / "SHA256SUMS").read_text().splitlines():
            digest, separator, name = line.partition("  ")
            if separator and name in names:
                if name in checksums or not re.fullmatch(r"[0-9a-f]{64}", digest):
                    raise ValueError("The release checksum file contains an invalid or duplicated bundle entry.")
                checksums[name] = digest
        if any(checksums.get(name) != expected[name][1] for name in names):
            raise ValueError("The bundle checksums do not agree with the published release metadata.")
        repo = public / "repo"
        runner(["ostree", "--repo=" + str(repo), "init", "--mode=archive-z2"])
        for name in names:
            runner(["flatpak", "build-import-bundle", "--no-update-summary",
                    "--gpg-sign=" + fingerprint, "--gpg-homedir=" + str(keyring),
                    str(repo), str(downloads / name)])
        # The feed keeps only the latest two architecture refs to fit static hosting.
        runner(["flatpak", "build-update-repo", "--gpg-sign=" + fingerprint,
                "--gpg-homedir=" + str(keyring), "--title=Cressida", "--prune", "--prune-depth=0", str(repo)])
        refs = runner(["flatpak", "repo", "--branches", str(repo)]).decode().splitlines()
        required = {f"app/{APPLICATION}/{arch}/stable" for arch in ARCHITECTURES}
        branches = {line.split()[0] for line in refs if line.strip()}
        if not required.issubset(branches) or not (repo / "summary").is_file() or not (repo / "summary.sig").is_file():
            raise ValueError("The signed repository is missing its application refs or signed summary.")
        encoded = base64.b64encode(public_key).decode()
        (public / "cressida.gpg").write_bytes(public_key)
        (public / "cressida.flatpakrepo").write_text(
            "[Flatpak Repo]\nTitle=Cressida\nUrl=" + base + "repo/\nGPGKey=" + encoded + "\n")
        (public / "cressida.flatpakref").write_text(
            "[Flatpak Ref]\nTitle=Cressida\nName=" + APPLICATION + "\nBranch=stable\nIsRuntime=false\nUrl="
            + base + "repo/\nGPGKey=" + encoded
            + "\nRuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo\n")
        (public / "index.html").write_text(
            '<!doctype html><html lang="en"><meta charset="utf-8"><title>Cressida Flatpak updates</title>'
            '<h1>Cressida Flatpak updates</h1><p>Stable release: ' + html.escape(tag)
            + '</p><p><a href="cressida.flatpakref">Install Cressida</a></p>'
            '<p><a href="cressida.flatpakrepo">Add the update repository</a></p></html>\n')
        total = tree_bytes(site) + tree_bytes(public)
        if total > MAXIMUM_PAGES_BYTES:
            raise ValueError("The website and update feed exceed the GitHub Pages size limit. Nothing was deployed.")
        public.rename(destination)
        return tag


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, required=True)
    parser.add_argument("--site-url", required=True)
    args = parser.parse_args()
    try:
        private_key = os.environ.pop("CRESSIDA_FLATPAK_GPG_KEY", "")
        passphrase = os.environ.pop("CRESSIDA_FLATPAK_GPG_PASSPHRASE", "")
        tag = prepare(args.site, args.site_url, private_key,
                      os.environ.get("CRESSIDA_FLATPAK_GPG_FINGERPRINT", ""), passphrase=passphrase)
        print(f"Prepared the signed Flatpak feed for {tag} beside the existing website.")
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
