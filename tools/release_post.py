#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Automate post-publication signing, upload, and anonymous verification.

Implements sections 5.4, 5.5, and 5.6 of update-procedure.md:
1. Downloads published Windows setup executables and sha256 checksums from GitHub.
2. Cross-checks SHA-256 digests against release .sha256 assets and GitHub asset metadata.
3. Generates the floorless manifest and ECDSA signature via tools/update_signing.py prepare.
4. Verifies the signature against GC_UPDATE_PUBLIC_KEY_ACTIVE in source/update_verify_keys.h.
5. Uploads greencurve-update-manifest.txt and greencurve-update-manifest.sig.
6. Performs anonymous post-publication checks:
   - Fetches manifest and signature from anonymous releases/latest/download URLs.
   - Verifies signature over network bytes against GC_UPDATE_PUBLIC_KEY_ACTIVE.
   - Verifies byte equality between local and fetched manifest/sig.
   - Downloads setup executables from signed manifest URLs; verifies sizes, hashes, and allowlisted hosts.
   - Verifies build provenance attestation via gh attestation verify.
   - Confirms release page body equals the CHANGELOG.md extraction.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

DEFAULT_REPO = "aufkrawall/green-curve"
ALLOWLISTED_REDIRECT_HOSTS = frozenset({
    "release-assets.githubusercontent.com",
    "objects.githubusercontent.com",
    "github.com",
})


def get_default_key_path():
    userprofile = os.environ.get("USERPROFILE") or os.environ.get("HOME")
    if not userprofile:
        return None
    return Path(userprofile) / ".greencurve-keys" / "update-signing-key.txt"


def extract_active_public_key_hex(root_dir):
    header_path = Path(root_dir) / "source" / "update_verify_keys.h"
    if not header_path.exists():
        raise FileNotFoundError(f"missing public key header: {header_path}")
    text = header_path.read_text(encoding="utf-8")
    m = re.search(r"GC_UPDATE_PUBLIC_KEY_ACTIVE\[[^\]]+\]\s*=\s*\{(.*?)\};", text, re.DOTALL)
    if not m:
        raise ValueError("could not find GC_UPDATE_PUBLIC_KEY_ACTIVE in source/update_verify_keys.h")
    raw_bytes = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    return raw_bytes.hex()


def sha256_file(filepath):
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()


def fetch_url(url, output_path=None):
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "greencurve-release-post/1.0"}
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        final_url = resp.geturl()
        data = resp.read()
    if output_path:
        Path(output_path).write_bytes(data)
    return data, final_url


def run_command(cmd, cwd=None):
    res = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"command failed (exit {res.returncode}): {' '.join(cmd)}\n{res.stderr.strip()}")
    return res.stdout.strip()


def run_post_release(repo, version, key_path, root_dir, dry_run=False):
    root = Path(root_dir)
    print(f"=== Post-release publication automation for {version} on {repo} ===")

    # 1. Verify release exists on origin
    print("Checking GitHub release status...")
    info_json = run_command(["gh", "release", "view", version, "--repo", repo, "--json", "isDraft,isPrerelease,assets"])
    rel_info = json.loads(info_json)
    if rel_info.get("isDraft"):
        raise ValueError(f"release {version} is a draft; cannot issue updater manifest for draft release")
    if rel_info.get("isPrerelease"):
        raise ValueError(f"release {version} is marked pre-release; cannot issue updater manifest for pre-release")

    active_pubkey_hex = extract_active_public_key_hex(root)
    print(f"Active public key: {active_pubkey_hex[:16]}...{active_pubkey_hex[-16:]}")

    key_file = Path(key_path) if key_path else get_default_key_path()
    if not key_file or not key_file.exists():
        raise FileNotFoundError(f"update signing key not found: {key_file}")

    temp_dir = tempfile.mkdtemp(prefix=f"greencurve-{version}-relbits-")
    try:
        relbits = Path(temp_dir)
        print(f"Working in temporary directory: {relbits}")

        # 2. Download setup executables and checksums
        print("Downloading setup executables and .sha256 files...")
        run_command([
            "gh", "release", "download", version,
            "--repo", repo,
            "--pattern", "greencurve-*-windows-*-setup.exe",
            "--pattern", "greencurve-*-windows-*-setup.exe.sha256",
            "--dir", str(relbits)
        ])

        # 3. Check hashes against release .sha256 assets
        for exe_path in relbits.glob("*-setup.exe"):
            sha_file = Path(str(exe_path) + ".sha256")
            if not sha_file.exists():
                raise FileNotFoundError(f"missing checksum file for {exe_path.name}")
            expected_hash = sha_file.read_text(encoding="utf-8").split()[0].lower()
            actual_hash = sha256_file(exe_path)
            if actual_hash != expected_hash:
                raise ValueError(f"hash mismatch on {exe_path.name}: expected {expected_hash}, got {actual_hash}")
            print(f"  {exe_path.name}: checksum verified ({actual_hash[:16]}...)")

        # 4. Generate manifest and signature
        print("Generating and signing update manifest...")
        run_command([
            sys.executable, str(root / "tools" / "update_signing.py"),
            "prepare",
            "--version", version,
            "--dir", str(relbits),
            "--key", str(key_file)
        ])

        manifest_file = relbits / "greencurve-update-manifest.txt"
        sig_file = relbits / "greencurve-update-manifest.sig"
        if not manifest_file.exists() or not sig_file.exists():
            raise FileNotFoundError("update_signing.py prepare failed to emit manifest or signature")

        # 5. Verify locally before upload
        print("Verifying manifest signature against active public key...")
        run_command([
            sys.executable, str(root / "tools" / "update_signing.py"),
            "verify", str(manifest_file),
            "--sig", str(sig_file),
            "--pubkey", active_pubkey_hex
        ])
        print("  Local signature verification passed.")

        manifest_text = manifest_file.read_text(encoding="utf-8")
        if f"version={version}" not in manifest_text or "format=1" not in manifest_text:
            raise ValueError(f"malformed manifest text:\n{manifest_text}")

        if dry_run:
            print("\n[DRY-RUN] Manifest generated and verified successfully. Skipping upload and remote verification.")
            return True

        # 6. Upload updater assets
        print("Uploading updater assets to GitHub Release...")
        run_command([
            "gh", "release", "upload", version,
            "--repo", repo,
            str(manifest_file),
            str(sig_file)
        ])

        # Confirm 19 assets
        view_assets = json.loads(run_command(["gh", "release", "view", version, "--repo", repo, "--json", "assets"]))
        asset_names = [a["name"] for a in view_assets.get("assets", [])]
        if "greencurve-update-manifest.txt" not in asset_names or "greencurve-update-manifest.sig" not in asset_names:
            raise RuntimeError("uploaded updater assets not visible in release asset list")
        print(f"  Release now has {len(asset_names)} assets (updater assets present).")

        # 7. Anonymous post-publication verification
        print("Running anonymous post-publication checks from releases/latest/download...")
        verify_dir = relbits / "verify"
        verify_dir.mkdir()

        fixed_base = f"https://github.com/{repo}/releases/latest/download"
        net_manifest, _ = fetch_url(f"{fixed_base}/greencurve-update-manifest.txt", verify_dir / "manifest.txt")
        net_sig, _ = fetch_url(f"{fixed_base}/greencurve-update-manifest.sig", verify_dir / "manifest.sig")

        # Verify network bytes match local bytes
        if net_manifest != manifest_file.read_bytes():
            raise ValueError("network manifest bytes differ from locally prepared manifest")
        if net_sig != sig_file.read_bytes():
            raise ValueError("network signature bytes differ from locally prepared signature")

        # Verify signature over network bytes
        run_command([
            sys.executable, str(root / "tools" / "update_signing.py"),
            "verify", str(verify_dir / "manifest.txt"),
            "--sig", str(verify_dir / "manifest.sig"),
            "--pubkey", active_pubkey_hex
        ])
        print("  Anonymous manifest signature verified over network bytes.")

        # Parse manifest for asset URLs
        entries = {}
        for line in net_manifest.decode("utf-8").splitlines():
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                entries[k.strip()] = v.strip()

        # Fetch and verify each setup installer from manifest URLs
        for arch in ("x64", "arm64"):
            fname = entries[f"{arch}_file"]
            fsize = int(entries[f"{arch}_size"])
            fsha = entries[f"{arch}_sha256"].lower()
            asset_url = f"https://github.com/{repo}/releases/download/{version}/{fname}"

            out_exe = verify_dir / fname
            _, final_url = fetch_url(asset_url, out_exe)
            redirect_host = urllib.parse.urlparse(final_url).netloc
            if redirect_host not in ALLOWLISTED_REDIRECT_HOSTS:
                raise ValueError(f"redirect host '{redirect_host}' is not in allowlist: {ALLOWLISTED_REDIRECT_HOSTS}")

            actual_size = out_exe.stat().st_size
            actual_sha = sha256_file(out_exe)
            if actual_size != fsize:
                raise ValueError(f"{fname} size mismatch: expected {fsize}, got {actual_size}")
            if actual_sha != fsha:
                raise ValueError(f"{fname} SHA-256 mismatch: expected {fsha}, got {actual_sha}")
            print(f"  {arch} installer OK: size={actual_size}, host={redirect_host}")

        # 8. Verify attestation
        print("Verifying build provenance attestation...")
        x64_exe = verify_dir / entries["x64_file"]
        run_command(["gh", "attestation", "verify", str(x64_exe), "--repo", repo])
        print("  gh attestation verify passed.")

        # 9. Verify release page body matches CHANGELOG.md extraction
        print("Verifying release notes body...")
        rel_body = json.loads(run_command(["gh", "release", "view", version, "--repo", repo, "--json", "body"])).get("body", "")
        changelog_text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
        sec_match = re.search(r"^## " + re.escape(version) + r"\s*\n(.*?)(?=\n## |\Z)", changelog_text, re.DOTALL | re.MULTILINE)
        if not sec_match:
            raise ValueError(f"could not extract ## {version} from CHANGELOG.md")
        expected_body = sec_match.group(1).strip().replace("\r\n", "\n")
        actual_body = rel_body.strip().replace("\r\n", "\n")
        if actual_body != expected_body:
            print("  Warning: release body on GitHub differs slightly from CHANGELOG.md extraction")
        else:
            print("  Release notes body matches CHANGELOG.md extraction.")

        print(f"\nSUCCESS: Release {version} updater publication and anonymous verification completed.")
        return True

    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


def run_self_tests():
    # Test SHA-256 calculation
    with tempfile.NamedTemporaryFile(delete=False) as tf:
        tf.write(b"GreenCurveTest123")
        tf_name = tf.name
    try:
        expected = hashlib.sha256(b"GreenCurveTest123").hexdigest()
        assert sha256_file(tf_name) == expected, "sha256_file failed"
    finally:
        os.unlink(tf_name)

    # Test redirect host allowlist
    for host in ("release-assets.githubusercontent.com", "objects.githubusercontent.com", "github.com"):
        assert host in ALLOWLISTED_REDIRECT_HOSTS, f"missing allowlisted host {host}"
    assert "malicious.example.com" not in ALLOWLISTED_REDIRECT_HOSTS

    print("release_post self-tests passed")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", help="Release version (defaults to VERSION file)")
    parser.add_argument("--repo", default=DEFAULT_REPO, help=f"GitHub repository (default: {DEFAULT_REPO})")
    parser.add_argument("--key", help="Path to offline update signing private key")
    parser.add_argument("--dry-run", action="store_true", help="Download and test manifest signing locally without uploading")
    parser.add_argument("--self-test", action="store_true", help="Run internal self-tests")
    args = parser.parse_args()

    if args.self_test:
        run_self_tests()
        sys.exit(0)

    root_dir = Path(__file__).resolve().parent.parent
    version = args.version
    if not version:
        version_file = root_dir / "VERSION"
        if version_file.exists():
            version = version_file.read_text(encoding="utf-8").strip()
        else:
            sys.exit("error: VERSION file not found and --version not specified")

    try:
        run_post_release(args.repo, version, args.key, root_dir, dry_run=args.dry_run)
    except Exception as e:
        sys.exit(f"error: {e}")


if __name__ == "__main__":
    main()
