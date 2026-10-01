# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Renew ONLY the signed freshness envelope of the latest published release.

New updater clients refuse update metadata older than 30 days
(tools/update_freshness.py, source/update_freshness_policy.h), so the latest
release's `greencurve-update-v2.txt`/`.sig` must be re-signed before then or
every such client stops seeing updates.  This is the automated form of the
renewal runbook, with the same gates as initial publication:

1. The GitHub tag resolves to the reviewed commit, and the release is the
   current `releases/latest` (renewing anything else changes nothing clients
   fetch).
2. The PUBLISHED v1 manifest's signature verifies against the compiled active
   key, and it names exactly the published installers by size and SHA-256,
   both of which carry release-workflow provenance for that commit.
3. A new envelope is signed over those EXACT v1 bytes; the v1 pair and the
   installers are never regenerated or uploaded.
4. After an explicit (non-dry-run) invocation, only the v2 pair is replaced,
   then fetched anonymously and checked byte for byte and for expiry.
"""

import json
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

import release_post
import update_freshness

V1_MANIFEST = "greencurve-update-manifest.txt"
V1_SIGNATURE = "greencurve-update-manifest.sig"


def _manifest_entries(payload):
    entries = {}
    for line in payload.decode("utf-8").splitlines():
        if "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            entries[key.strip()] = value.strip()
    return entries


def _verify_signature(root, manifest, signature, pubkey_hex):
    release_post.run_command([sys.executable, str(root / "tools" / "update_signing.py"),
                              "verify", str(manifest), "--sig", str(signature),
                              "--pubkey", pubkey_hex])


def _check_published_installers(entries, version, directory):
    """The signed v1 manifest must name the published installers exactly."""
    if entries.get("format") != "1" or entries.get("version") != version:
        raise ValueError("published manifest does not describe this version")
    installers = []
    for arch in release_post.INSTALLER_ARCHES:
        expected_name = f"greencurve-{version}-windows-{arch}-setup.exe"
        if entries.get(f"{arch}_file") != expected_name:
            raise ValueError(f"published manifest does not name {expected_name}")
        path = directory / expected_name
        if not path.is_file():
            raise FileNotFoundError(f"missing published installer: {expected_name}")
        size = entries.get(f"{arch}_size", "")
        digest = entries.get(f"{arch}_sha256", "").lower()
        if not size.isdigit() or path.stat().st_size != int(size):
            raise ValueError(f"{expected_name} size differs from the signed manifest")
        if release_post.sha256_file(path) != digest:
            raise ValueError(f"{expected_name} SHA-256 differs from the signed manifest")
        installers.append(path)
    return installers


def run_renew_freshness(repo, version, key_path, root_dir, dry_run=False,
                        source_commit=None, now=None):
    root = Path(root_dir)
    if not re.fullmatch(r"[0-9]+\.[0-9]+(?:\.[0-9]+)?", version):
        raise ValueError("release version must be numeric MAJOR.MINOR[.PATCH]")
    if not re.fullmatch(r"[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+", repo):
        raise ValueError("release repository must have owner/repo form")
    print(f"=== Freshness renewal for {version} on {repo} ===")

    expected_commit = release_post.reviewed_release_commit(root, version, source_commit)
    published_commit = release_post.remote_release_commit(repo, version)
    if published_commit != expected_commit:
        raise ValueError(f"release source commit mismatch: reviewed {expected_commit}, "
                         f"GitHub tag {published_commit}")

    latest = json.loads(release_post.run_command(
        ["gh", "api", f"repos/{repo}/releases/latest"])).get("tag_name")
    if latest != version:
        raise ValueError(f"{version} is not releases/latest ({latest}); clients never fetch "
                         "its metadata, so renewing it would change nothing")

    info = json.loads(release_post.run_command(
        ["gh", "release", "view", version, "--repo", repo, "--json", "isDraft,isPrerelease,assets"]))
    if info.get("isDraft") or info.get("isPrerelease"):
        raise ValueError(f"release {version} is a draft or pre-release")
    installer_names = [f"greencurve-{version}-windows-{arch}-setup.exe"
                       for arch in release_post.INSTALLER_ARCHES]
    required = {V1_MANIFEST, V1_SIGNATURE, *installer_names}
    missing = required - {asset["name"] for asset in info.get("assets", [])}
    if missing:
        raise ValueError("release is missing assets needed for renewal: " + ", ".join(sorted(missing)))

    pubkey_hex = release_post.extract_active_public_key_hex(root)
    key_file = Path(key_path) if key_path else release_post.get_default_key_path()
    if not key_file or not key_file.exists():
        raise FileNotFoundError(f"update signing key not found: {key_file}")

    temp_dir = tempfile.mkdtemp(prefix=f"greencurve-{version}-renew-")
    try:
        work = Path(temp_dir)
        download = ["gh", "release", "download", version, "--repo", repo,
                    "--pattern", V1_MANIFEST, "--pattern", V1_SIGNATURE]
        for name in installer_names:
            download.extend(["--pattern", name])
        release_post.run_command(download + ["--dir", str(work)])

        v1_manifest, v1_signature = work / V1_MANIFEST, work / V1_SIGNATURE
        if not v1_manifest.is_file() or not v1_signature.is_file():
            raise FileNotFoundError("published v1 manifest or signature did not download")
        print("Verifying the published v1 manifest signature...")
        _verify_signature(root, v1_manifest, v1_signature, pubkey_hex)
        v1_bytes = v1_manifest.read_bytes()
        installers = _check_published_installers(_manifest_entries(v1_bytes), version, work)
        print("Verifying both installers' provenance before using the key...")
        for installer in installers:
            release_post.verify_installer_provenance(installer, repo, expected_commit)

        print("Signing a new freshness envelope over the unchanged v1 bytes...")
        release_post.run_command([sys.executable, str(root / "tools" / "update_signing.py"),
                                  "renew-freshness", str(v1_manifest),
                                  "--key", str(key_file), "--dir", str(work)])
        fresh_manifest = work / update_freshness.MANIFEST_ASSET
        fresh_signature = work / update_freshness.SIGNATURE_ASSET
        if v1_manifest.read_bytes() != v1_bytes:
            raise ValueError("renewal modified the v1 manifest")
        if update_freshness.unwrap(fresh_manifest.read_bytes(), now) != v1_bytes:
            raise ValueError("renewed envelope does not carry the exact published v1 manifest")
        _verify_signature(root, fresh_manifest, fresh_signature, pubkey_hex)
        expires = int(re.search(rb"expires=([0-9]+)\n", fresh_manifest.read_bytes()).group(1))
        expiry_text = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(expires))
        if dry_run:
            print(f"\n[DRY-RUN] Renewed envelope verified (would expire {expiry_text}). "
                  "Nothing was uploaded.")
            return True

        print("Replacing ONLY the v2 freshness pair on the release...")
        release_post.run_command(["gh", "release", "upload", version, "--repo", repo,
                                  str(fresh_manifest), str(fresh_signature), "--clobber"])

        print("Verifying anonymous delivery from releases/latest/download...")
        verify_dir = work / "verify"
        verify_dir.mkdir()
        base = f"https://github.com/{repo}/releases/latest/download"
        for local in (fresh_manifest, fresh_signature):
            network, _ = release_post.fetch_url(f"{base}/{local.name}", verify_dir / local.name)
            if network != local.read_bytes():
                raise ValueError("published freshness bytes differ from the renewed envelope")
        if update_freshness.unwrap((verify_dir / fresh_manifest.name).read_bytes(), now) != v1_bytes:
            raise ValueError("delivered envelope does not carry the published v1 manifest")
        _verify_signature(root, verify_dir / fresh_manifest.name,
                          verify_dir / fresh_signature.name, pubkey_hex)
        network_v1, _ = release_post.fetch_url(f"{base}/{V1_MANIFEST}", verify_dir / V1_MANIFEST)
        if network_v1 != v1_bytes:
            raise ValueError("the delivered v1 manifest changed during renewal")
        print(f"\nSUCCESS: {version} freshness renewed; it now expires {expiry_text}. "
              "Renew again before then.")
        return True
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)
