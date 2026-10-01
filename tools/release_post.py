#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Automate post-publication signing, upload, and anonymous verification.

Implements sections 5.4, 5.5, and 5.6 of update-procedure.md:
1. Downloads published Windows setup executables and sha256 checksums from GitHub.
2. Requires both installers and checks their SHA-256 digests against release .sha256 assets.
3. Verifies both installers' provenance against the reviewed release commit and workflow.
4. Generates the floorless manifest and ECDSA signature via tools/update_signing.py prepare.
5. Verifies the signature against GC_UPDATE_PUBLIC_KEY_ACTIVE in source/update_verify_keys.h.
6. Uploads greencurve-update-manifest.txt and greencurve-update-manifest.sig.
7. Performs anonymous post-publication checks:
   - Fetches manifest and signature from anonymous releases/latest/download URLs.
   - Verifies signature over network bytes against GC_UPDATE_PUBLIC_KEY_ACTIVE.
   - Verifies byte equality between local and fetched manifest/sig.
   - Downloads setup executables from signed manifest URLs; verifies sizes, hashes, and allowlisted hosts.
   - Reports whether the release page body equals the CHANGELOG.md extraction.

The expected commit comes from the local release tag, or --source-commit with
the full reviewed SHA. GitHub's release tag must resolve to that same commit.
"""

import argparse
import update_freshness
import update_manifest_tools
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
INSTALLER_ARCHES = ("x64", "arm64")
RELEASE_WORKFLOW = ".github/workflows/release.yml"


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


def validate_commit_sha(commit):
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", commit):
        raise ValueError("release source commit must be a full 40-character SHA")
    return commit.lower()


def reviewed_release_commit(root, version, source_commit=None):
    if source_commit is not None:
        return validate_commit_sha(source_commit)
    try:
        commit = run_command(["git", "rev-parse", "--verify",
                              f"refs/tags/{version}^{{commit}}"], cwd=root)
    except (OSError, RuntimeError) as exc:
        raise ValueError(f"reviewed local release tag {version} is unavailable; "
                         "provide --source-commit with the full reviewed SHA") from exc
    return validate_commit_sha(commit)


def remote_release_commit(repo, version):
    document = json.loads(run_command(["gh", "api", f"repos/{repo}/git/ref/tags/{version}"]))
    seen = set()
    # Annotated tags can target other tags. Bound traversal and reject cycles;
    # release.targetCommitish can be a moving branch, so it is not used here.
    for _ in range(16):
        obj = document.get("object", {})
        sha = validate_commit_sha(obj.get("sha"))
        if obj.get("type") == "commit":
            return sha
        if obj.get("type") != "tag" or sha in seen:
            raise ValueError("release tag does not resolve to a unique commit")
        seen.add(sha)
        document = json.loads(run_command(["gh", "api", f"repos/{repo}/git/tags/{sha}"]))
    raise ValueError("release tag chain exceeds the supported depth")


def verify_installer_provenance(path, repo, commit):
    print(f"  {path.name}: verifying {RELEASE_WORKFLOW} provenance at {commit}")
    run_command(["gh", "attestation", "verify", str(path), "--repo", repo,
                 "--source-digest", commit,
                 "--signer-workflow", f"{repo}/{RELEASE_WORKFLOW}"])
    print(f"  {path.name}: release provenance verified")


def require_latest_release(repo, version):
    latest = json.loads(run_command(["gh", "api", f"repos/{repo}/releases/latest"])).get("tag_name")
    if latest != version:
        raise ValueError(f"{version} is not releases/latest ({latest}); "
                         "refusing to sign or publish updater metadata for another release")
    print(f"Updater channel still targets release {version}")


def run_post_release(repo, version, key_path, root_dir, dry_run=False, source_commit=None):
    root = Path(root_dir)
    update_manifest_tools.validate_version(version)
    if not re.fullmatch(r"[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+", repo):
        raise ValueError("release repository must have owner/repo form")
    print(f"=== Post-release publication automation for {version} on {repo} ===")

    expected_commit = reviewed_release_commit(root, version, source_commit)
    published_commit = remote_release_commit(repo, version)
    if published_commit != expected_commit:
        raise ValueError(f"release source commit mismatch: reviewed {expected_commit}, "
                         f"GitHub tag {published_commit}")
    print(f"Release tag matches reviewed source commit: {expected_commit}")

    # 1. Verify release exists on origin
    print("Checking GitHub release status...")
    info_json = run_command(["gh", "release", "view", version, "--repo", repo, "--json", "isDraft,isPrerelease,assets"])
    rel_info = json.loads(info_json)
    if rel_info.get("isDraft"):
        raise ValueError(f"release {version} is a draft; cannot issue updater manifest for draft release")
    if rel_info.get("isPrerelease"):
        raise ValueError(f"release {version} is marked pre-release; cannot issue updater manifest for pre-release")
    require_latest_release(repo, version)

    installer_names = {arch: f"greencurve-{version}-windows-{arch}-setup.exe"
                       for arch in INSTALLER_ARCHES}
    asset_names = {asset["name"] for asset in rel_info.get("assets", [])}
    required_names = {name + suffix for name in installer_names.values()
                      for suffix in ("", ".sha256")}
    missing = required_names - asset_names
    if missing:
        raise ValueError("release is missing required installer assets: " + ", ".join(sorted(missing)))

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
        download = ["gh", "release", "download", version, "--repo", repo]
        for name in installer_names.values():
            download.extend(["--pattern", name, "--pattern", name + ".sha256"])
        run_command(download + ["--dir", str(relbits)])

        # 3. Check hashes against release .sha256 assets
        installers = [relbits / name for name in installer_names.values()]
        for exe_path in installers:
            if not exe_path.is_file():
                raise FileNotFoundError(f"missing required installer: {exe_path.name}")
            sha_file = Path(str(exe_path) + ".sha256")
            if not sha_file.exists():
                raise FileNotFoundError(f"missing checksum file for {exe_path.name}")
            checksum_fields = sha_file.read_text(encoding="utf-8").split()
            if not checksum_fields or not re.fullmatch(r"[0-9a-fA-F]{64}", checksum_fields[0]):
                raise ValueError(f"invalid checksum file for {exe_path.name}")
            expected_hash = checksum_fields[0].lower()
            actual_hash = sha256_file(exe_path)
            if actual_hash != expected_hash:
                raise ValueError(f"hash mismatch on {exe_path.name}: expected {expected_hash}, got {actual_hash}")
            print(f"  {exe_path.name}: checksum verified ({actual_hash[:16]}...)")

        # Provenance is a prerequisite to using the offline key, in dry runs
        # too. Verify every installer that the manifest will authorize.
        print("Verifying both installers before signing or publication...")
        for exe_path in installers:
            verify_installer_provenance(exe_path, repo, expected_commit)

        # 4. Generate manifest and signature
        require_latest_release(repo, version)
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

        entries = update_manifest_tools.parse_manifest(manifest_file.read_bytes())
        if entries["version"] != version:
            raise ValueError("prepared manifest does not describe the requested release")

        fresh_manifest = relbits / update_freshness.MANIFEST_ASSET
        fresh_sig = relbits / update_freshness.SIGNATURE_ASSET
        if update_freshness.unwrap(fresh_manifest.read_bytes()) != manifest_file.read_bytes():
            raise ValueError("freshness metadata is not bound to the prepared manifest")
        run_command([sys.executable, str(root / "tools" / "update_signing.py"),
                     "verify", str(fresh_manifest), "--sig", str(fresh_sig),
                     "--pubkey", active_pubkey_hex])
        if dry_run:
            print("\n[DRY-RUN] Both installers' provenance and the local signature verified. "
                  "Skipping upload and anonymous delivery verification.")
            return True

        # 6. Upload updater assets
        require_latest_release(repo, version)
        print("Uploading updater assets to GitHub Release...")
        run_command([
            "gh", "release", "upload", version,
            "--repo", repo,
            str(manifest_file),
            str(sig_file), str(fresh_manifest), str(fresh_sig)
        ])

        # Confirm updater assets
        view_assets = json.loads(run_command(["gh", "release", "view", version, "--repo", repo, "--json", "assets"]))
        asset_names = [a["name"] for a in view_assets.get("assets", [])]
        if any(name not in asset_names for name in ("greencurve-update-manifest.txt",
            "greencurve-update-manifest.sig", update_freshness.MANIFEST_ASSET, update_freshness.SIGNATURE_ASSET)):
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

        for local in (fresh_manifest, fresh_sig):
            network, _ = fetch_url(f"{fixed_base}/{local.name}", verify_dir / local.name)
            if network != local.read_bytes():
                raise ValueError("published freshness bytes differ from local signed metadata")
        entries = update_manifest_tools.parse_manifest(net_manifest)

        # Fetch and verify each setup installer from manifest URLs
        for arch in INSTALLER_ARCHES:
            fname = entries[f"{arch}_file"]
            if fname != f"greencurve-{version}-windows-{arch}-setup.exe":
                raise ValueError("signed installer filename is not the expected basename")
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
    import release_post_tests

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

    release_post_tests.run_tests()
    import release_renew_tests
    release_renew_tests.run_tests()

    print("release_post self-tests passed")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", help="Release version (defaults to VERSION file)")
    parser.add_argument("--repo", default=DEFAULT_REPO, help=f"GitHub repository (default: {DEFAULT_REPO})")
    parser.add_argument("--key", help="Path to offline update signing private key")
    parser.add_argument("--source-commit", help="Full reviewed release commit SHA (defaults to the local release tag)")
    parser.add_argument("--dry-run", action="store_true", help="Verify both installers' provenance and sign locally without uploading")
    parser.add_argument("--renew-freshness", action="store_true",
                        help="Re-sign ONLY the 30-day v2 freshness pair of the latest release "
                             "(see tools/release_renew.py); never touches v1 or installers")
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
        if args.renew_freshness:
            import release_renew
            release_renew.run_renew_freshness(args.repo, version, args.key, root_dir,
                                              dry_run=args.dry_run,
                                              source_commit=args.source_commit)
        else:
            run_post_release(args.repo, version, args.key, root_dir,
                             dry_run=args.dry_run, source_commit=args.source_commit)
    except Exception as e:
        sys.exit(f"error: {e}")


if __name__ == "__main__":
    main()
