#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Simulate Green Curve in-app updates and stable releases on a test clone repository.

Enables automated pre-flight testing of the in-app updater across release boundaries
without exposing test updates to users on the public channel.

Supported subcommands:
  init            Validate offline signing key, gh auth, and ensure test repo is public.
  build-baseline  Build setup from released stable tag in an isolated worktree repointed
                  to the test repo, for manual installation by the operator.
  publish-hop1    Build candidate setup (HEAD) repointed to test repo, sign v1 + v2
                  manifests with offline key, publish release to test repo, and verify.
  publish-hop2    Build follow-up setup (bumping version) repointed to test repo, sign,
                  publish to test repo as latest, and verify non-regression.
  teardown        Delete test releases and tags from test repo; restore private visibility.

Invariants enforced:
- Repointed repository constants are modified ONLY inside isolated temporary git worktrees.
- The baseline client is built from the released stable tag, not from HEAD.
- Manifests (v1 + v2 freshness envelope) are signed with the maintainer's offline key.
- Test repository must be public for anonymous WinHTTP fetches; reverted to private on teardown.
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

# Local tool modules
SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import release_prep
import update_freshness
import update_manifest_tools
import update_signing

DEFAULT_TEST_REPO = "aufkrawall/green-curve-update-test"
DEFAULT_ARCH = "x64"
ALLOWLISTED_REDIRECT_HOSTS = frozenset({
    "release-assets.githubusercontent.com",
    "objects.githubusercontent.com",
    "github.com",
})


def log(msg, prefix="[simulate-release]"):
    """Print debug log message."""
    print(f"{prefix} {msg}", flush=True)


def run_command(cmd, cwd=None, check=True):
    """Execute subprocess and return stdout text; log failures with stderr details."""
    cmd_str = " ".join(str(c) for c in cmd)
    log(f"exec: {cmd_str} (cwd: {cwd or '.'})")
    res = subprocess.run(
        cmd,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if check and res.returncode != 0:
        err_msg = res.stderr.strip() or res.stdout.strip()
        log(f"command failed (code {res.returncode}): {err_msg}", prefix="[ERROR]")
        raise RuntimeError(f"command failed (code {res.returncode}): {cmd_str}\n{err_msg}")
    return res.stdout.strip()


def parse_repo(repo_str):
    """Validate and split owner/repo string."""
    m = re.fullmatch(r"([A-Za-z0-9_-]+)/([A-Za-z0-9_.-]+)", repo_str.strip())
    if not m:
        raise ValueError(f"invalid repository format '{repo_str}'; expected owner/repo")
    return m.group(1), m.group(2)


def get_default_key_path():
    """Locate offline update signing key."""
    userprofile = os.environ.get("USERPROFILE") or os.environ.get("HOME")
    if not userprofile:
        return None
    return Path(userprofile) / ".greencurve-keys" / "update-signing-key.txt"


def get_active_pubkey_hex(root=ROOT_DIR):
    """Extract GC_UPDATE_PUBLIC_KEY_ACTIVE hex string from source/update_verify_keys.h."""
    header = Path(root) / "source" / "update_verify_keys.h"
    if not header.exists():
        raise FileNotFoundError(f"missing public key header: {header}")
    text = header.read_text(encoding="utf-8")
    m = re.search(r"GC_UPDATE_PUBLIC_KEY_ACTIVE\[[^\]]+\]\s*=\s*\{(.*?)\};", text, re.DOTALL)
    if not m:
        raise ValueError("could not find GC_UPDATE_PUBLIC_KEY_ACTIVE in update_verify_keys.h")
    raw = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    return raw.hex().lower()


def get_latest_stable_tag(root=ROOT_DIR):
    """Find the highest stable numeric release tag in the repository."""
    out = run_command(["git", "tag", "-l"], cwd=root)
    tags = [t.strip() for t in out.splitlines() if t.strip()]
    valid = []
    for tag in tags:
        try:
            update_manifest_tools.validate_version(tag)
            valid.append(tag)
        except ValueError:
            continue
    if not valid:
        raise RuntimeError("could not find any valid stable release tags in repository")

    def parse_components(ver):
        return tuple(int(x) for x in ver.split("."))

    valid.sort(key=parse_components)
    return valid[-1]


def patch_update_repo(source_dir, owner, repo):
    """Patch GC_UPDATE_REPO_OWNER and GC_UPDATE_REPO_NAME in update_url_policy.h."""
    policy_path = Path(source_dir) / "source" / "update_url_policy.h"
    if not policy_path.exists():
        raise FileNotFoundError(f"missing update_url_policy.h at {policy_path}")
    text = policy_path.read_text(encoding="utf-8")

    new_text, count_owner = re.subn(
        r'#define GC_UPDATE_REPO_OWNER "[^"]+"',
        f'#define GC_UPDATE_REPO_OWNER "{owner}"',
        text,
        count=1,
    )
    new_text, count_name = re.subn(
        r'#define GC_UPDATE_REPO_NAME  "[^"]+"',
        f'#define GC_UPDATE_REPO_NAME  "{repo}"',
        new_text,
        count=1,
    )

    if count_owner != 1 or count_name != 1:
        raise RuntimeError(f"failed to patch GC_UPDATE_REPO macros in {policy_path}")

    policy_path.write_text(new_text, encoding="utf-8")
    log(f"patched {policy_path.name} -> owner='{owner}', repo='{repo}'")


def create_worktree(root, git_ref, worktree_dir):
    """Create a temporary detached worktree at the specified git reference."""
    log(f"creating detached worktree at '{git_ref}' -> {worktree_dir}")
    run_command(["git", "worktree", "add", "--detach", str(worktree_dir), git_ref], cwd=root)


def setup_toolchain_junctions(root, worktree_dir):
    """Create NTFS junctions for untracked toolchain directories into the worktree."""
    junctions = []
    if os.name != "nt":
        return junctions

    for d in ("llvm-mingw", "seven-zip", "zig"):
        src = Path(root) / d
        dst = Path(worktree_dir) / d
        if src.is_dir() and not dst.exists():
            log(f"linking toolchain junction: {dst.name} -> {src}")
            # Use cmd mklink /J on Windows for non-elevated directory junctions
            res = subprocess.run(
                ["cmd.exe", "/c", "mklink", "/J", str(dst), str(src)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if res.returncode == 0:
                junctions.append(dst)
            else:
                log(f"warning: failed to create junction for {d}: {res.stderr.strip()}")
    return junctions


def cleanup_toolchain_junctions(junctions):
    """Safely unlink NTFS junctions without removing target files."""
    for j in junctions:
        if j.exists():
            log(f"removing toolchain junction: {j.name}")
            subprocess.run(["cmd.exe", "/c", "rmdir", str(j)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def remove_worktree(root, worktree_dir, junctions=None):
    """Clean up junctions and remove git worktree."""
    if junctions:
        cleanup_toolchain_junctions(junctions)
    log(f"removing worktree: {worktree_dir}")
    run_command(["git", "worktree", "remove", "--force", str(worktree_dir)], cwd=root, check=False)
    if Path(worktree_dir).exists():
        shutil.rmtree(worktree_dir, ignore_errors=True)


def sha256_file(filepath):
    """Compute lowercase hex SHA-256 digest of file."""
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()


def find_built_setup_exe(worktree_dir, version, arch="x64"):
    """Find the built setup executable in the worktree dist directory."""
    expected_name = f"greencurve-{version}-windows-{arch}-setup.exe"
    dist_dir = Path(worktree_dir) / "dist"
    candidates = list(dist_dir.rglob(expected_name))
    if not candidates:
        raise FileNotFoundError(f"setup executable '{expected_name}' not found under {dist_dir}")
    # Prefer the release toolchain variant if both msvc and release exist
    for c in candidates:
        if "release" in c.parts:
            return c
    return candidates[0]


def check_environment(root, key_path=None):
    """Verify prerequisites: git, gh authentication, and offline signing key."""
    log("checking environment preconditions...")
    gh_status = run_command(["gh", "auth", "status"], cwd=root, check=False)
    if "Logged in to" not in gh_status and "account" not in gh_status:
        raise RuntimeError(f"gh CLI is not authenticated; run 'gh auth login'.\nOutput:\n{gh_status}")
    log("gh CLI authentication confirmed")

    valid_key, key_info = release_prep.check_update_signing_key(root, key_path)
    if not valid_key:
        raise RuntimeError(f"signing key validation failed: {key_info}")
    log(f"update signing key confirmed (public key: {key_info[:16]}...{key_info[-16:]})")
    return True


def cmd_init(args):
    """Initialize simulation environment: verify keys and set test repo to public."""
    owner, repo = parse_repo(args.repo)
    check_environment(ROOT_DIR, args.key)

    log(f"checking target repository: {args.repo}")
    repo_json = json.loads(run_command(["gh", "api", f"repos/{args.repo}"]))
    current_visibility = repo_json.get("visibility", "unknown")
    log(f"repository visibility is currently: '{current_visibility}'")

    if current_visibility != "public":
        if args.dry_run:
            log("[DRY-RUN] skipping setting repo visibility to public")
        else:
            log("setting test repository visibility to 'public' for anonymous WinHTTP fetches...")
            run_command(["gh", "repo", "edit", args.repo, "--visibility", "public"])
            log("test repository is now public")

    releases = json.loads(run_command(["gh", "release", "list", "--repo", args.repo, "--json", "tagName,isLatest,createdAt"]))
    log(f"found {len(releases)} existing releases on {args.repo}")
    if releases and args.clear:
        if args.dry_run:
            log(f"[DRY-RUN] would clear {len(releases)} existing releases from {args.repo}")
        else:
            log("clearing existing test releases to ensure clean latest pointer...")
            for rel in releases:
                tag = rel["tagName"]
                log(f"deleting release and tag '{tag}'...")
                run_command(["gh", "release", "delete", tag, "--repo", args.repo, "--cleanup-tag", "--yes"])
            log("test repository cleared")

    log("init complete. Environment is ready for simulation.")
    return 0


def cmd_build_baseline(args):
    """Build baseline installer from released stable tag repointed to test repo."""
    owner, repo = parse_repo(args.repo)
    tag = args.stable_tag or get_latest_stable_tag(ROOT_DIR)
    update_manifest_tools.validate_version(tag)
    log(f"=== Building Baseline Client ({tag}) for {args.repo} ===")

    out_dir = Path(args.output_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    temp_dir = Path(tempfile.mkdtemp(prefix=f"gc-sim-baseline-{tag}-"))
    junctions = []
    try:
        create_worktree(ROOT_DIR, f"refs/tags/{tag}", temp_dir)
        junctions = setup_toolchain_junctions(ROOT_DIR, temp_dir)
        patch_update_repo(temp_dir, owner, repo)

        log(f"building Windows {args.arch} setup in worktree...")
        run_command([sys.executable, "build.py", "--target", "windows", "--arch", args.arch], cwd=temp_dir)

        built_exe = find_built_setup_exe(temp_dir, tag, args.arch)
        target_exe = out_dir / built_exe.name
        shutil.copy2(built_exe, target_exe)
        log(f"copied baseline installer to: {target_exe}")

        sha = sha256_file(target_exe)
        sha_file = target_exe.with_suffix(".exe.sha256")
        sha_file.write_text(f"{sha}  {target_exe.name}\n", encoding="utf-8")
        log(f"wrote checksum: {sha_file.name} ({sha[:16]}...)")
    finally:
        remove_worktree(ROOT_DIR, temp_dir, junctions)

    print("\n" + "=" * 70)
    print("BASELINE CLIENT READY FOR MANUAL INSTALLATION")
    print(f"Path: {target_exe}")
    print("Instructions:")
    print("1. Manually run the setup executable above to install Green Curve.")
    print("2. Open the Green Curve GUI and configure custom GPU settings (e.g. clock offset or fan curve).")
    print("3. Verify that the Updates dialog reports 'up to date' or 'no update manifest found'.")
    print("4. Proceed to publish Hop 1 when ready.")
    print("=" * 70 + "\n")
    return 0


def _build_and_stage_hop(version, ref, arch, repo, out_dir):
    """Helper: build setup in worktree and stage artifacts."""
    owner, repo_name = parse_repo(repo)
    update_manifest_tools.validate_version(version)
    out_dir.mkdir(parents=True, exist_ok=True)

    temp_dir = Path(tempfile.mkdtemp(prefix=f"gc-sim-hop-{version}-"))
    junctions = []
    try:
        create_worktree(ROOT_DIR, ref, temp_dir)
        junctions = setup_toolchain_junctions(ROOT_DIR, temp_dir)
        patch_update_repo(temp_dir, owner, repo_name)

        version_file = temp_dir / "VERSION"
        version_file.write_text(version, encoding="utf-8")

        log(f"building Windows {arch} setup for version {version} in worktree...")
        run_command([sys.executable, "build.py", "--target", "windows", "--arch", arch], cwd=temp_dir)

        built_exe = find_built_setup_exe(temp_dir, version, arch)
        target_exe = out_dir / built_exe.name
        shutil.copy2(built_exe, target_exe)
        log(f"copied hop installer to: {target_exe}")

        sha = sha256_file(target_exe)
        sha_file = target_exe.with_suffix(".exe.sha256")
        sha_file.write_text(f"{sha}  {target_exe.name}\n", encoding="utf-8")
        log(f"wrote checksum: {sha_file.name} ({sha[:16]}...)")
    finally:
        remove_worktree(ROOT_DIR, temp_dir, junctions)

    return target_exe


def _sign_and_publish_hop(version, repo, bits_dir, key_path, hop_label, dry_run=False):
    """Helper: generate signed manifests and upload release to test repo."""
    key_file = Path(key_path) if key_path else get_default_key_path()
    if not key_file or not key_file.exists():
        raise FileNotFoundError(f"update signing key not found: {key_file}")

    log("generating and signing update manifests (v1 + v2 freshness envelope)...")
    run_command([
        sys.executable, str(ROOT_DIR / "tools" / "update_signing.py"),
        "prepare",
        "--version", version,
        "--dir", str(bits_dir),
        "--key", str(key_file)
    ])

    manifest_v1 = bits_dir / "greencurve-update-manifest.txt"
    sig_v1 = bits_dir / "greencurve-update-manifest.sig"
    manifest_v2 = bits_dir / update_freshness.MANIFEST_ASSET
    sig_v2 = bits_dir / update_freshness.SIGNATURE_ASSET

    for f in (manifest_v1, sig_v1, manifest_v2, sig_v2):
        if not f.is_file():
            raise FileNotFoundError(f"expected updater asset was not generated: {f.name}")

    if dry_run:
        log(f"[DRY-RUN] manifests generated and signed locally for {version}. Skipping upload.")
        return

    # Check if release tag already exists and clobber/delete if needed
    existing = run_command(["gh", "release", "view", version, "--repo", repo], check=False)
    if "release not found" not in existing.lower() and "404" not in existing:
        log(f"release {version} already exists on {repo}; deleting stale release...")
        run_command(["gh", "release", "delete", version, "--repo", repo, "--cleanup-tag", "--yes"])

    log(f"creating release {version} on {repo}...")
    assets = list(bits_dir.glob("*.exe")) + list(bits_dir.glob("*.sha256")) + [manifest_v1, sig_v1, manifest_v2, sig_v2]
    asset_args = [str(a) for a in assets]

    run_command([
        "gh", "release", "create", version,
        *asset_args,
        "--repo", repo,
        "--title", f"Green Curve {version} ({hop_label})",
        "--notes", f"Automated in-app updater release simulation ({hop_label}).",
    ])
    log(f"release {version} published successfully on {repo}")

    # Anonymous post-publication verification
    log("verifying anonymous delivery and signature from releases/latest/download...")
    fixed_base = f"https://github.com/{repo}/releases/latest/download"
    req_v2 = urllib.request.Request(f"{fixed_base}/{update_freshness.MANIFEST_ASSET}", headers={"User-Agent": "gc-sim/1.0"})
    req_sig = urllib.request.Request(f"{fixed_base}/{update_freshness.SIGNATURE_ASSET}", headers={"User-Agent": "gc-sim/1.0"})

    with urllib.request.urlopen(req_v2, timeout=20) as resp:
        net_v2 = resp.read()
    with urllib.request.urlopen(req_sig, timeout=20) as resp:
        net_sig = resp.read()

    active_pubkey_hex = get_active_pubkey_hex(ROOT_DIR)
    temp_v = tempfile.mkdtemp(prefix="gc-sim-verify-")
    try:
        t_v2 = Path(temp_v) / "v2.txt"
        t_sig = Path(temp_v) / "v2.sig"
        t_v2.write_bytes(net_v2)
        t_sig.write_bytes(net_sig)
        run_command([
            sys.executable, str(ROOT_DIR / "tools" / "update_signing.py"),
            "verify", str(t_v2), "--sig", str(t_sig), "--pubkey", active_pubkey_hex
        ])
        log("anonymous v2 freshness signature verified against GC_UPDATE_PUBLIC_KEY_ACTIVE")
    finally:
        shutil.rmtree(temp_v, ignore_errors=True)


def cmd_publish_hop1(args):
    """Publish candidate release (Hop 1) to test repository."""
    version = args.version
    if not version:
        version_file = ROOT_DIR / "VERSION"
        if version_file.exists():
            version = version_file.read_text(encoding="utf-8").strip()
        else:
            raise ValueError("could not determine version; pass --version")

    log(f"=== Publishing Hop 1 Candidate ({version}) on {args.repo} ===")
    out_dir = Path(args.output_dir).resolve()
    _build_and_stage_hop(version, args.source_ref, args.arch, args.repo, out_dir)
    _sign_and_publish_hop(version, args.repo, out_dir, args.key, "Hop 1 Candidate", dry_run=args.dry_run)

    print("\n" + "=" * 70)
    print(f"HOP 1 CANDIDATE ({version}) PUBLISHED")
    print(f"Repository: https://github.com/{args.repo}/releases/latest")
    print("Instructions:")
    print("1. In your installed Green Curve GUI, click 'Check now' (or wait for auto-check / notification).")
    print(f"2. Confirm that update '{version}' is detected and offered.")
    print("3. Click 'Install' and verify:")
    print("   - Silent update execution and GUI relaunch.")
    print(f"   - GUI title reports version '{version}'.")
    print("   - Your previously configured GPU clock offsets and fan curves are preserved.")
    print("4. Proceed to publish Hop 2 to test updater non-regression.")
    print("=" * 70 + "\n")
    return 0


def cmd_publish_hop2(args):
    """Publish incremental follow-up release (Hop 2) to test repository."""
    version = args.version
    if not version:
        # Default to incrementing patch component of VERSION
        v_curr = (ROOT_DIR / "VERSION").read_text(encoding="utf-8").strip()
        parts = v_curr.split(".")
        if len(parts) == 2:
            parts.append("1")
        else:
            parts[-1] = str(int(parts[-1]) + 1)
        version = ".".join(parts)

    log(f"=== Publishing Hop 2 Follow-Up ({version}) on {args.repo} ===")
    out_dir = Path(args.output_dir).resolve()
    _build_and_stage_hop(version, args.source_ref, args.arch, args.repo, out_dir)
    _sign_and_publish_hop(version, args.repo, out_dir, args.key, "Hop 2 Regression Test", dry_run=args.dry_run)

    print("\n" + "=" * 70)
    print(f"HOP 2 REGRESSION TEST ({version}) PUBLISHED")
    print(f"Repository: https://github.com/{args.repo}/releases/latest")
    print("Instructions:")
    print("1. In your newly updated Green Curve GUI, click 'Check now'.")
    print(f"2. Confirm that the new updater implementation detects '{version}'.")
    print("3. Click 'Install' and confirm the update applies seamlessly.")
    print("4. Once verified, run teardown to clean up the test repository.")
    print("=" * 70 + "\n")
    return 0


def cmd_teardown(args):
    """Clean up test releases and restore test repo visibility to private."""
    owner, repo = parse_repo(args.repo)
    log(f"=== Tearing Down Test Releases on {args.repo} ===")

    releases = json.loads(run_command(["gh", "release", "list", "--repo", args.repo, "--json", "tagName"]))
    tags_to_delete = []
    if args.versions:
        tags_to_delete = [v.strip() for v in args.versions.split(",") if v.strip()]
    elif args.all_sim:
        tags_to_delete = [r["tagName"] for r in releases]

    if not tags_to_delete:
        log("no specific release tags provided for deletion (pass --versions <v1,v2> or --all-sim)")
    else:
        for tag in tags_to_delete:
            log(f"deleting release and tag '{tag}' from {args.repo}...")
            run_command(["gh", "release", "delete", tag, "--repo", args.repo, "--cleanup-tag", "--yes"], check=False)

    if args.private:
        log(f"reverting repository visibility to 'private' for {args.repo}...")
        run_command(["gh", "repo", "edit", args.repo, "--visibility", "private"], check=False)
        log("test repository visibility reverted to private")

    log("teardown complete.")
    return 0


def run_self_tests():
    """Run internal unit self-tests."""
    import simulate_release_tests
    return simulate_release_tests.run_tests()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="Run internal self-tests and exit")
    subparsers = parser.add_subparsers(dest="subcommand", help="Simulation action to perform")

    # init
    p_init = subparsers.add_parser("init", help="Verify keys and set test repo visibility to public")
    p_init.add_argument("--repo", default=DEFAULT_TEST_REPO, help=f"Test repository (default: {DEFAULT_TEST_REPO})")
    p_init.add_argument("--key", help="Offline update signing key path")
    p_init.add_argument("--clear", action="store_true", help="Delete existing releases on test repo")
    p_init.add_argument("--dry-run", action="store_true", help="Skip remote mutations")

    # build-baseline
    p_base = subparsers.add_parser("build-baseline", help="Build baseline setup from released stable tag")
    p_base.add_argument("--repo", default=DEFAULT_TEST_REPO, help=f"Test repository (default: {DEFAULT_TEST_REPO})")
    p_base.add_argument("--stable-tag", help="Stable release tag (default: latest stable tag)")
    p_base.add_argument("--arch", default=DEFAULT_ARCH, help=f"Target architecture (default: {DEFAULT_ARCH})")
    p_base.add_argument("--output-dir", default=str(ROOT_DIR / "dist" / "test-simulation" / "baseline"))

    # publish-hop1
    p_hop1 = subparsers.add_parser("publish-hop1", help="Build and publish Hop 1 candidate release to test repo")
    p_hop1.add_argument("--repo", default=DEFAULT_TEST_REPO, help=f"Test repository (default: {DEFAULT_TEST_REPO})")
    p_hop1.add_argument("--version", help="Candidate version (default: from VERSION file)")
    p_hop1.add_argument("--source-ref", default="HEAD", help="Git reference for candidate code (default: HEAD)")
    p_hop1.add_argument("--arch", default=DEFAULT_ARCH, help=f"Target architecture (default: {DEFAULT_ARCH})")
    p_hop1.add_argument("--key", help="Offline update signing key path")
    p_hop1.add_argument("--output-dir", default=str(ROOT_DIR / "dist" / "test-simulation" / "hop1"))
    p_hop1.add_argument("--dry-run", action="store_true", help="Build and sign locally without uploading")

    # publish-hop2
    p_hop2 = subparsers.add_parser("publish-hop2", help="Build and publish Hop 2 follow-up release to test repo")
    p_hop2.add_argument("--repo", default=DEFAULT_TEST_REPO, help=f"Test repository (default: {DEFAULT_TEST_REPO})")
    p_hop2.add_argument("--version", help="Follow-up version (default: bumped patch version)")
    p_hop2.add_argument("--source-ref", default="HEAD", help="Git reference for code (default: HEAD)")
    p_hop2.add_argument("--arch", default=DEFAULT_ARCH, help=f"Target architecture (default: {DEFAULT_ARCH})")
    p_hop2.add_argument("--key", help="Offline update signing key path")
    p_hop2.add_argument("--output-dir", default=str(ROOT_DIR / "dist" / "test-simulation" / "hop2"))
    p_hop2.add_argument("--dry-run", action="store_true", help="Build and sign locally without uploading")

    # teardown
    p_tear = subparsers.add_parser("teardown", help="Clean up test releases and revert test repo to private")
    p_tear.add_argument("--repo", default=DEFAULT_TEST_REPO, help=f"Test repository (default: {DEFAULT_TEST_REPO})")
    p_tear.add_argument("--versions", help="Comma-separated release tags to delete")
    p_tear.add_argument("--all-sim", action="store_true", help="Delete all releases found on test repo")
    p_tear.add_argument("--no-private", dest="private", action="store_false", help="Do not revert repo to private")
    p_tear.set_defaults(private=True)

    args = parser.parse_args()
    if args.self_test:
        sys.exit(0 if run_self_tests() else 1)

    if not args.subcommand:
        parser.print_help()
        sys.exit(1)

    cmd_map = {
        "init": cmd_init,
        "build-baseline": cmd_build_baseline,
        "publish-hop1": cmd_publish_hop1,
        "publish-hop2": cmd_publish_hop2,
        "teardown": cmd_teardown,
    }
    sys.exit(cmd_map[args.subcommand](args))


if __name__ == "__main__":
    main()
